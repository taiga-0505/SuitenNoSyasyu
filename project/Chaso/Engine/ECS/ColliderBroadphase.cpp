#include "ColliderBroadphase.h"

#include "ColliderComponent.h"
#include "Entity.h"
#include "NativeScriptComponent.h"
#include "RigidbodyComponent.h"
#include "TransformComponent.h"

#include <algorithm>
#include <cmath>

// ============================================================================
// 形状 → AABB
// ============================================================================

bool ColliderBroadphase::ComputeBounds(const TransformComponent &tr,
                                       const ColliderComponent &col,
                                       RC::BoundingBox &out) {
  // Scene::BuildColliderVolume / ResolveCollisions と同じ式（位置は TransformComponent::position）
  const RC::Vector3 center = {
      tr.position.x + col.center.x * tr.scale.x,
      tr.position.y + col.center.y * tr.scale.y,
      tr.position.z + col.center.z * tr.scale.z};

  if (col.shape == ColliderComponent::Shape::Sphere) {
    const float maxScale = (std::max)({std::abs(tr.scale.x), std::abs(tr.scale.y), std::abs(tr.scale.z)});
    const float r = std::abs(col.radius * maxScale);
    out = RC::BoundingBox::FromCenterHalf(center, {r, r, r});
  } else {
    // Capsule は未実装のため AABB として扱う（Scene 側と同じ）
    out = RC::BoundingBox::FromCenterHalf(
        center, {std::abs(col.size.x * tr.scale.x * 0.5f),
                 std::abs(col.size.y * tr.scale.y * 0.5f),
                 std::abs(col.size.z * tr.scale.z * 0.5f)});
  }
  // NaN が混ざると BVH が壊れるので弾く
  return std::isfinite(out.min.x) && std::isfinite(out.min.y) && std::isfinite(out.min.z) &&
         std::isfinite(out.max.x) && std::isfinite(out.max.y) && std::isfinite(out.max.z);
}

// ============================================================================
// 登録情報の確保・解放
// ============================================================================

int32_t ColliderBroadphase::AcquireEntry_() {
  if (!freeEntries_.empty()) {
    const int32_t idx = freeEntries_.back();
    freeEntries_.pop_back();
    entries_[idx] = Entry{};
    entries_[idx].inUse = true;
    return idx;
  }
  entries_.emplace_back();
  entries_.back().inUse = true;
  return static_cast<int32_t>(entries_.size()) - 1;
}

void ColliderBroadphase::ReleaseEntry_(int32_t idx) {
  Entry &en = entries_[idx];
  if (en.proxy != RC::DynamicBVH::kNullNode) {
    bvh_.DestroyProxy(en.proxy);
  }
  en = Entry{};
  freeEntries_.push_back(idx);
}

void ColliderBroadphase::Clear() {
  bvh_.Clear();
  entries_.clear();
  freeEntries_.clear();
  orderToEntry_.clear();
  dirty_ = true;
}

// ============================================================================
// 同期
// ============================================================================

void ColliderBroadphase::Sync(const std::vector<std::shared_ptr<Entity>> &entities) {
  ++stamp_;
  if (stamp_ == 0) ++stamp_; // 0 は「未使用」の意味で使っているので飛ばす

  orderToEntry_.assign(entities.size(), -1);

  for (size_t i = 0; i < entities.size(); ++i) {
    Entity *e = entities[i].get();
    if (!e || !e->IsActive() || e->IsPendingDestroy()) continue;

    auto *col = e->GetComponent<ColliderComponent>();
    if (!col || !col->IsEnabled()) continue;
    auto *tr = e->GetComponent<TransformComponent>();
    if (!tr) continue;

    RC::BoundingBox box;
    if (!ComputeBounds(*tr, *col, box)) continue;

    // コンポーネントに覚えている番号が本当にこのエンティティのものか照合する
    // （コンポーネントの複製・ポインタの再利用で別物を指している可能性がある）
    int32_t idx = col->broadphaseEntry;
    const bool valid = idx >= 0 && idx < static_cast<int32_t>(entries_.size()) &&
                       entries_[idx].inUse && entries_[idx].entity == e &&
                       entries_[idx].entityId == e->Id() && entries_[idx].stamp != stamp_;

    const RC::Vector3 center = box.Center();
    if (valid) {
      Entry &en = entries_[idx];
      const RC::Vector3 disp = {center.x - en.lastCenter.x, center.y - en.lastCenter.y,
                                center.z - en.lastCenter.z};
      bvh_.MoveProxy(en.proxy, box, disp);
    } else {
      idx = AcquireEntry_();
      Entry &en = entries_[idx];
      en.weak = entities[i];
      en.entity = e;
      en.entityId = e->Id();
      en.proxy = bvh_.CreateProxy(box, static_cast<uint32_t>(idx));
      col->broadphaseEntry = idx;
    }

    Entry &en = entries_[idx];
    en.tr = tr;
    en.col = col;
    en.rb = e->GetComponent<RigidbodyComponent>();
    en.nsc = e->GetComponent<NativeScriptComponent>();
    en.interesting = (en.rb && !en.rb->isKinematic) || (en.nsc != nullptr);
    en.lastCenter = center;
    en.stamp = stamp_;
    en.order = static_cast<uint32_t>(i);
    orderToEntry_[i] = idx;
  }

  // 今回見つからなかった登録（破棄・非アクティブ化・コライダー除去）を消す
  for (int32_t idx = 0; idx < static_cast<int32_t>(entries_.size()); ++idx) {
    if (entries_[idx].inUse && entries_[idx].stamp != stamp_) {
      ReleaseEntry_(idx);
    }
  }

  dirty_ = false;
}

void ColliderBroadphase::Refresh(Entity *e) {
  if (!e || dirty_) return;
  auto *col = e->GetComponent<ColliderComponent>();
  if (!col) return;
  const int32_t idx = col->broadphaseEntry;
  if (idx < 0 || idx >= static_cast<int32_t>(entries_.size())) return;
  Entry &en = entries_[idx];
  if (!en.inUse || en.entity != e || en.entityId != e->Id()) return;
  auto *tr = e->GetComponent<TransformComponent>();
  if (!tr) return;

  RC::BoundingBox box;
  if (!ComputeBounds(*tr, *col, box)) return;
  const RC::Vector3 center = box.Center();
  const RC::Vector3 disp = {center.x - en.lastCenter.x, center.y - en.lastCenter.y,
                            center.z - en.lastCenter.z};
  bvh_.MoveProxy(en.proxy, box, disp);
  en.lastCenter = center;
}

// ============================================================================
// ペア列挙
// ============================================================================

void ColliderBroadphase::CollectPairs(bool includeAll,
                                      std::vector<std::pair<uint32_t, uint32_t>> &out) const {
  out.clear();
  for (const Entry &a : entries_) {
    if (!a.inUse || a.proxy == RC::DynamicBVH::kNullNode) continue;
    if (!includeAll && !a.interesting) continue;

    bvh_.Query(bvh_.GetFatAABB(a.proxy), [&](int32_t proxy) {
      const Entry &b = entries_[bvh_.GetUserData(proxy)];
      if (&b == &a) return true;
      // 同じ組を二度出さない:
      //   includeAll → 添え字の小さい側からだけ出す
      //   それ以外   → 両方 interesting なら小さい側から、片方だけなら interesting 側から出す
      if ((includeAll || b.interesting) && b.order < a.order) return true;
      out.emplace_back((std::min)(a.order, b.order), (std::max)(a.order, b.order));
      return true;
    });
  }
  // 以前の二重ループ（i 昇順 → j 昇順）と同じ順番で処理されるよう並べる
  std::sort(out.begin(), out.end());
}
