#pragma once

// ============================================================================
// DynamicBVH
// ----------------------------------------------------------------------------
// 動的に追加・削除・移動できる AABB 木（Bounding Volume Hierarchy）。
//
//  - 葉には「太らせた AABB（fat AABB）」を入れる。実形状が fat の内側で動いている間は
//    木を一切触らないので、毎フレーム少しずつ動く物の更新コストがほぼゼロになる。
//  - 挿入時は表面積ヒューリスティック（SAH）で兄弟ノードを選び、
//    AVL 木と同じ要領の回転で高さの偏りを抑える（高さ ≒ O(log N) を保つ）。
//  - 用途: 当たり判定のブロードフェーズ（ペア列挙・近傍検索）と、
//          描画カリング（視錐台に対して部分木ごと採用／棄却）。
//
// スレッドセーフではない（Query 系は const だが同時に Move/Create しないこと）。
// ============================================================================

#include "BoundingBox.h"
#include <cstdint>
#include <vector>

namespace RC {

/// @brief 動的 AABB 木
class DynamicBVH {
public:
  /// @brief 無効なノード番号
  static constexpr int32_t kNullNode = -1;

  /// @param fatMargin 葉の AABB を太らせる量（m）。大きいほど再挿入が減るがクエリの誤検出が増える
  /// @param displacementMultiplier MoveProxy に渡した移動量の何倍を進行方向へ先読みして太らせるか
  explicit DynamicBVH(float fatMargin = 0.1f, float displacementMultiplier = 2.0f)
      : fatMargin_(fatMargin), displacementMultiplier_(displacementMultiplier) {}

  /// @brief 葉（プロキシ）を追加する
  /// @param tight 実形状を包む AABB
  /// @param userData 呼び出し側の識別子（配列の添え字など）
  /// @return プロキシ番号（DestroyProxy / MoveProxy に渡す）
  int32_t CreateProxy(const BoundingBox &tight, uint32_t userData);

  /// @brief 葉を削除する
  void DestroyProxy(int32_t proxyId);

  /// @brief 葉を移動する
  /// @param tight 新しい実形状の AABB
  /// @param displacement 今回の移動量（先読みして fat AABB を進行方向へ伸ばす。不明なら 0）
  /// @return 木を組み替えた（再挿入した）なら true。fat の内側に収まっていれば false
  bool MoveProxy(int32_t proxyId, const BoundingBox &tight,
                 const Vector3 &displacement = {0.0f, 0.0f, 0.0f});

  /// @brief 葉に紐づけた識別子
  uint32_t GetUserData(int32_t proxyId) const { return nodes_[proxyId].userData; }

  /// @brief 葉に紐づけた識別子を差し替える（呼び出し側の配列を詰め直したとき用）
  void SetUserData(int32_t proxyId, uint32_t userData) { nodes_[proxyId].userData = userData; }

  /// @brief 葉の fat AABB
  const BoundingBox &GetFatAABB(int32_t proxyId) const { return nodes_[proxyId].aabb; }

  /// @brief box と重なる葉を列挙する
  /// @param cb bool(int32_t proxyId)。false を返すと列挙を打ち切る
  template <class Callback> void Query(const BoundingBox &box, Callback &&cb) const;

  /// @brief 視錐台にかかる葉を列挙する（保守的：fat AABB で判定するので多めに返ることはあるが、漏れはない）
  /// @details 完全に内側の部分木は以降の判定を省いて丸ごと採用する。
  /// @param cb void(int32_t proxyId)
  template <class Callback> void QueryFrustum(const Frustum &frustum, Callback &&cb) const;

  /// @brief 全ての葉を列挙する
  /// @param cb void(int32_t proxyId)
  template <class Callback> void ForEachProxy(Callback &&cb) const;

  /// @brief 全て削除する
  void Clear();

  /// @brief 木の高さ（葉だけなら 0）
  int32_t GetHeight() const { return (root_ == kNullNode) ? 0 : nodes_[root_].height; }

  /// @brief 葉の数
  int32_t GetProxyCount() const { return proxyCount_; }

  /// @brief 構造が壊れていないか検証する（デバッグ・テスト用。O(N)）
  bool Validate() const;

  /// @brief 太らせ量
  float GetFatMargin() const { return fatMargin_; }

private:
  /// @brief 木のノード。葉は child1 == kNullNode
  struct Node {
    BoundingBox aabb;              ///< 葉: fat AABB / 内部ノード: 子の和
    uint32_t userData = 0;         ///< 葉のみ有効
    int32_t parent = kNullNode;    ///< 親（未使用ノードではフリーリストの next）
    int32_t child1 = kNullNode;    ///< 子1
    int32_t child2 = kNullNode;    ///< 子2
    int32_t height = -1;           ///< 葉=0、未使用=-1
    bool IsLeaf() const { return child1 == kNullNode; }
  };

  int32_t AllocateNode_();
  void FreeNode_(int32_t node);
  void InsertLeaf_(int32_t leaf);
  void RemoveLeaf_(int32_t leaf);
  int32_t Balance_(int32_t a);
  void RefitUpward_(int32_t index);
  int32_t ValidateNode_(int32_t index, int32_t parent, int32_t &leafCount, bool &ok) const;
  BoundingBox MakeFat_(const BoundingBox &tight, const Vector3 &displacement) const;

  std::vector<Node> nodes_;
  int32_t root_ = kNullNode;
  int32_t freeList_ = kNullNode;
  int32_t proxyCount_ = 0;
  float fatMargin_ = 0.1f;
  float displacementMultiplier_ = 2.0f;
};

// ----------------------------------------------------------------------------
// テンプレート実装
// ----------------------------------------------------------------------------
namespace detail {
/// @brief 探索用スタック。浅い木（ほぼ全て）は固定長配列で回し、溢れたときだけヒープを使う
class BvhStack {
public:
  void Push(int32_t v) {
    if (size_ < kInline) { inline_[size_++] = v; return; }
    overflow_.push_back(v);
    ++size_;
  }
  int32_t Pop() {
    --size_;
    if (size_ >= kInline) { const int32_t v = overflow_.back(); overflow_.pop_back(); return v; }
    return inline_[size_];
  }
  bool Empty() const { return size_ == 0; }

private:
  static constexpr int32_t kInline = 256;
  int32_t inline_[kInline];
  int32_t size_ = 0;
  std::vector<int32_t> overflow_;
};
} // namespace detail

template <class Callback>
void DynamicBVH::Query(const BoundingBox &box, Callback &&cb) const {
  if (root_ == kNullNode) return;
  detail::BvhStack stack;
  stack.Push(root_);
  while (!stack.Empty()) {
    const int32_t id = stack.Pop();
    const Node &n = nodes_[id];
    if (!n.aabb.Overlaps(box)) continue;
    if (n.IsLeaf()) {
      if (!cb(id)) return;
    } else {
      stack.Push(n.child1);
      stack.Push(n.child2);
    }
  }
}

template <class Callback>
void DynamicBVH::QueryFrustum(const Frustum &frustum, Callback &&cb) const {
  if (root_ == kNullNode) return;
  // 最上位ビットを「部分木が完全に内側」の印に使う（ノード番号は正なので衝突しない）
  constexpr uint32_t kInsideBit = 0x80000000u;
  detail::BvhStack stack;
  stack.Push(root_);
  while (!stack.Empty()) {
    const uint32_t raw = static_cast<uint32_t>(stack.Pop());
    const bool inside = (raw & kInsideBit) != 0;
    const int32_t id = static_cast<int32_t>(raw & ~kInsideBit);
    const Node &n = nodes_[id];

    bool childInside = inside;
    if (!inside) {
      const Frustum::Result r = frustum.Classify(n.aabb);
      if (r == Frustum::Result::Outside) continue;
      childInside = (r == Frustum::Result::Inside);
    }
    if (n.IsLeaf()) {
      cb(id);
      continue;
    }
    const uint32_t flag = childInside ? kInsideBit : 0u;
    stack.Push(static_cast<int32_t>(static_cast<uint32_t>(n.child1) | flag));
    stack.Push(static_cast<int32_t>(static_cast<uint32_t>(n.child2) | flag));
  }
}

template <class Callback>
void DynamicBVH::ForEachProxy(Callback &&cb) const {
  for (int32_t i = 0; i < static_cast<int32_t>(nodes_.size()); ++i) {
    if (nodes_[i].height == 0) cb(i);
  }
}

} // namespace RC
