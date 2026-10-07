#pragma once

// ============================================================================
// ColliderBroadphase
// ----------------------------------------------------------------------------
// ColliderComponent を持つエンティティを DynamicBVH に登録し、
// 「重なっている可能性のある組」だけを高速に列挙する（当たり判定の粗い判定）。
//
// 以前の Scene::ResolveCollisions は全エンティティの総当たり（O(N^2)）で、
// 1 万個では毎フレーム約 5000 万ペアを調べていた。BVH で候補を絞るので
// O(N log N) 程度になる。
//
// 使い方（Scene から）:
//   Sync(entities)                ... フレーム中に 1〜2 回。追加・削除・移動を BVH に反映
//   CollectPairs(...)             ... 重なり候補のペアを entities の添え字で列挙
//   QueryBox(box, cb)             ... 箱と重なる候補を列挙（キャラクター移動の判定用）
//   Refresh(e)                    ... 1 体だけ動かした直後に位置を反映
//
// 注意: QueryBox で受け取る Entry の tr / col 等は「直前の Sync 時点」のポインタ。
//       Sync 以降にエンティティが破棄されている可能性があるので、QueryBox の中では
//       必ず Entry::weak.lock() で生存を確かめてから触ること（Scene が面倒を見る）。
//       CollectPairs は Sync 直後に使う前提（その時点の entities 添え字を返す）。
// ============================================================================

#include "Math/DynamicBVH.h"
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

class Entity;
class TransformComponent;
class ColliderComponent;
class RigidbodyComponent;
class NativeScriptComponent;

/// @brief コライダーの粗い判定（ブロードフェーズ）
class ColliderBroadphase {
public:
  /// @brief 1 エンティティ分の登録情報（Sync 時点のコンポーネントをキャッシュしている）
  struct Entry {
    std::weak_ptr<Entity> weak;            ///< 対象（Sync 後に破棄された場合でも安全に判定できるよう弱参照で持つ）
    Entity *entity = nullptr;              ///< 照合用の生ポインタ（逆参照は weak.lock() 後に行うこと）
    TransformComponent *tr = nullptr;      ///< Sync 時点の Transform
    ColliderComponent *col = nullptr;      ///< Sync 時点の Collider
    RigidbodyComponent *rb = nullptr;      ///< Sync 時点の Rigidbody（無ければ nullptr）
    NativeScriptComponent *nsc = nullptr;  ///< Sync 時点のスクリプト（無ければ nullptr）
    RC::Vector3 lastCenter{0.0f, 0.0f, 0.0f}; ///< 前回登録時の中心（移動量の先読み用）
    uint32_t entityId = 0;                 ///< 照合用の ID（ポインタ再利用対策）
    uint32_t stamp = 0;                    ///< 最後に Sync で見つかったときの番号
    uint32_t order = 0;                    ///< entities 配列での添え字
    int32_t proxy = RC::DynamicBVH::kNullNode; ///< BVH の葉番号
    bool interesting = false;              ///< 押し出し対象（動的 Rigidbody）か OnCollision を受けるか
    bool inUse = false;                    ///< 使用中か
  };

  /// @brief entities の内容を BVH に反映する
  /// @details 非アクティブ・破棄予定・コライダー無効のエンティティは登録から外す。
  ///          呼び出し後、EntryAt(i) で entities[i] のキャッシュを引ける。
  void Sync(const std::vector<std::shared_ptr<Entity>> &entities);

  /// @brief 1 エンティティの位置だけを BVH に反映する（未登録なら何もしない）
  void Refresh(Entity *e);

  /// @brief 重なり候補のペアを (i, j)（entities の添え字、i < j、昇順）で列挙する
  /// @param includeAll false なら「片方以上が interesting」なペアだけ（以前のループと同じ絞り込み）
  /// @param out 出力先（clear してから詰める）
  void CollectPairs(bool includeAll, std::vector<std::pair<uint32_t, uint32_t>> &out) const;

  /// @brief box と重なる候補を列挙する
  /// @param cb bool(const Entry&)。false を返すと打ち切る
  template <class Callback> void QueryBox(const RC::BoundingBox &box, Callback &&cb) const {
    bvh_.Query(box, [&](int32_t proxy) {
      const Entry &en = entries_[bvh_.GetUserData(proxy)];
      return cb(en);
    });
  }

  /// @brief entities[order] の登録情報（未登録なら nullptr）
  const Entry *EntryAt(uint32_t order) const {
    if (order >= orderToEntry_.size()) return nullptr;
    const int32_t idx = orderToEntry_[order];
    return (idx >= 0) ? &entries_[idx] : nullptr;
  }

  /// @brief エンティティの増減があったことを知らせる（次の Query 前に Sync が必要になる）
  void MarkDirty() { dirty_ = true; }

  /// @brief Sync が必要か（一度も Sync していない、または MarkDirty された）
  bool IsDirty() const { return dirty_; }

  /// @brief 直前の Sync に渡した entities の要素数（増減の検出用）
  size_t SyncedEntityCount() const { return orderToEntry_.size(); }

  /// @brief 全登録を破棄する
  void Clear();

  /// @brief 登録数
  int32_t Count() const { return bvh_.GetProxyCount(); }

  /// @brief コライダーのワールド AABB（Scene の判定と同じ式。Capsule は AABB 扱い）
  static bool ComputeBounds(const TransformComponent &tr, const ColliderComponent &col,
                            RC::BoundingBox &out);

  /// @brief BVH の葉を太らせる量（m）
  /// @details ResolveCollisions のループ中に押し出しで少し動いた物同士も候補に残るよう、
  ///          押し出し量（めり込み深さ）より十分大きく取る。
  static constexpr float kFatMargin = 0.25f;

private:
  int32_t AcquireEntry_();
  void ReleaseEntry_(int32_t idx);

  RC::DynamicBVH bvh_{kFatMargin, 2.0f};
  std::vector<Entry> entries_;
  std::vector<int32_t> freeEntries_;
  std::vector<int32_t> orderToEntry_; ///< entities の添え字 → entries_ の添え字（-1 = 未登録）
  uint32_t stamp_ = 0;
  bool dirty_ = true;
};
