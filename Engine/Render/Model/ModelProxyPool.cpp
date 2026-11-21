#include "ModelProxyPool.h"

#include "Math/Math.h"
#include "Model/ModelManager.h"
#include "Graphics/Model/ModelObject.h"

namespace RC {

// ============================================================================
// 生成・破棄
// ============================================================================

ModelProxyHandle ModelProxyPool::Create(int modelHandle) {
  uint32_t slot;
  if (!freeSlots_.empty()) {
    slot = freeSlots_.back();
    freeSlots_.pop_back();
  } else {
    slot = static_cast<uint32_t>(flags_.size());
    world_.emplace_back();
    worldInvTranspose_.emplace_back();
    color_.emplace_back();
    modelHandle_.emplace_back(-1);
    bvhProxy_.emplace_back(DynamicBVH::kNullNode);
    generation_.emplace_back(0u);
    flags_.emplace_back(static_cast<uint8_t>(0));
  }

  world_[slot] = MakeIdentity4x4();
  worldInvTranspose_[slot] = MakeIdentity4x4();
  color_[slot] = {1.0f, 1.0f, 1.0f, 1.0f};
  modelHandle_[slot] = modelHandle;
  bvhProxy_[slot] = DynamicBVH::kNullNode;
  flags_[slot] = static_cast<uint8_t>(kAlive | kVisible | kCastShadow);
  MarkDirty_(slot);
  ++aliveCount_;

  return {slot, generation_[slot]};
}

void ModelProxyPool::Destroy(ModelProxyHandle h) {
  if (!IsAlive(h)) return;
  const uint32_t slot = h.index;
  if (bvhProxy_[slot] != DynamicBVH::kNullNode) {
    bvh_.DestroyProxy(bvhProxy_[slot]);
    bvhProxy_[slot] = DynamicBVH::kNullNode;
  }
  // dirty リストに残っていても、kAlive が落ちているので UpdateDirty で読み飛ばされる
  flags_[slot] = 0;
  modelHandle_[slot] = -1;
  ++generation_[slot]; // 古いハンドルを無効にする
  freeSlots_.push_back(slot);
  --aliveCount_;
}

void ModelProxyPool::Clear() {
  world_.clear();
  worldInvTranspose_.clear();
  color_.clear();
  modelHandle_.clear();
  bvhProxy_.clear();
  // 世代番号は捨てない（Clear 前のハンドルが Clear 後に作ったスロットを指さないように）
  for (auto &g : generation_) ++g;
  const size_t keep = generation_.size();
  flags_.assign(keep, 0);
  world_.resize(keep);
  worldInvTranspose_.resize(keep);
  color_.resize(keep);
  modelHandle_.assign(keep, -1);
  bvhProxy_.assign(keep, DynamicBVH::kNullNode);
  freeSlots_.clear();
  for (uint32_t i = static_cast<uint32_t>(keep); i > 0; --i) freeSlots_.push_back(i - 1);
  dirtySlots_.clear();
  pendingSlots_.clear();
  bvh_.Clear();
  aliveCount_ = 0;
}

// ============================================================================
// 設定
// ============================================================================

Matrix4x4 ModelProxyPool::ComputeInverseTranspose_(const Matrix4x4 &w) {
  return Transpose(Inverse(w));
}

void ModelProxyPool::MarkDirty_(uint32_t slot) {
  flags_[slot] &= static_cast<uint8_t>(~kWitValid); // 逆転置は次に要求されたときに計算し直す
  if ((flags_[slot] & kDirty) == 0) {
    flags_[slot] |= kDirty;
    dirtySlots_.push_back(slot);
  }
}

void ModelProxyPool::SetWorld(ModelProxyHandle h, const Matrix4x4 &world) {
  if (!IsAlive(h)) return;
  world_[h.index] = world;
  MarkDirty_(h.index);
}

void ModelProxyPool::SetTransform(ModelProxyHandle h, const Transform &t) {
  if (!IsAlive(h)) return;
  world_[h.index] = MakeAffineMatrix(t.scale, t.rotation, t.translation);
  MarkDirty_(h.index);
}

void ModelProxyPool::SetColor(ModelProxyHandle h, const Vector4 &color) {
  if (!IsAlive(h)) return;
  color_[h.index] = color; // 境界箱は変わらないので dirty にしない
}

void ModelProxyPool::SetVisible(ModelProxyHandle h, bool visible) {
  if (!IsAlive(h)) return;
  if (visible) flags_[h.index] |= kVisible;
  else flags_[h.index] &= static_cast<uint8_t>(~kVisible);
}

void ModelProxyPool::SetCastShadow(ModelProxyHandle h, bool cast) {
  if (!IsAlive(h)) return;
  if (cast) flags_[h.index] |= kCastShadow;
  else flags_[h.index] &= static_cast<uint8_t>(~kCastShadow);
}

// ============================================================================
// 差分更新
// ============================================================================

void ModelProxyPool::UpdateDirty(ModelManager &models) {
  if (dirtySlots_.empty()) return;

  pendingSlots_.clear();
  for (const uint32_t slot : dirtySlots_) {
    if ((flags_[slot] & kAlive) == 0) continue; // 破棄済み

    const ::ModelObject *m = models.Get(modelHandle_[slot]);
    if (!m || !m->IsReady() || !m->GetMesh() || !m->GetMesh()->Ready()) {
      // テンプレートのロード待ち。境界箱が決まらないので次フレームへ持ち越す
      pendingSlots_.push_back(slot);
      continue;
    }

    const Matrix4x4 &w = world_[slot];
    // 逆転置はここでは計算しない（カメラに映ったときに WorldInverseTranspose が計算する）
    const BoundingBox bounds = m->GetMesh()->LocalBounds().Transformed(w);

    if (bvhProxy_[slot] == DynamicBVH::kNullNode) {
      bvhProxy_[slot] = bvh_.CreateProxy(bounds, slot);
    } else {
      bvh_.MoveProxy(bvhProxy_[slot], bounds);
    }
    flags_[slot] &= static_cast<uint8_t>(~kDirty);
  }

  // 持ち越し分だけを残す（kDirty は立ったまま）
  dirtySlots_.swap(pendingSlots_);
  pendingSlots_.clear();
}

} // namespace RC
