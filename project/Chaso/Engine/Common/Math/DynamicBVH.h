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
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace RC {

/// @class DynamicBVH
/// @brief 動的 AABB 木
class DynamicBVH {
public:
  /// @brief 無効なノード番号
  static constexpr int32_t kNullNode = -1;

  /// @brief コンストラクタ
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
  /// @param proxyId CreateProxy で得たプロキシ番号
  void DestroyProxy(int32_t proxyId);

  /// @brief 葉を移動する
  /// @param proxyId      プロキシ番号
  /// @param tight 新しい実形状の AABB
  /// @param displacement 今回の移動量（先読みして fat AABB を進行方向へ伸ばす。不明なら 0）
  /// @return 木を組み替えた（再挿入した）なら true。fat の内側に収まっていれば false
  bool MoveProxy(int32_t proxyId, const BoundingBox &tight,
                 const Vector3 &displacement = {0.0f, 0.0f, 0.0f});

  /// @brief 葉に紐づけた識別子を取得する
  /// @param proxyId プロキシ番号
  /// @return CreateProxy / SetUserData で渡した識別子
  uint32_t GetUserData(int32_t proxyId) const { return nodes_[proxyId].userData; }

  /// @brief 葉に紐づけた識別子を差し替える（呼び出し側の配列を詰め直したとき用）
  /// @param proxyId  プロキシ番号
  /// @param userData 新しい識別子
  void SetUserData(int32_t proxyId, uint32_t userData) { nodes_[proxyId].userData = userData; }

  /// @brief 葉の fat AABB を取得する
  /// @param proxyId プロキシ番号
  /// @return 太らせた AABB
  const BoundingBox &GetFatAABB(int32_t proxyId) const { return nodes_[proxyId].aabb; }

  /// @brief box と重なる葉を列挙する
  /// @param box 判定する箱
  /// @param cb  bool(int32_t proxyId)。false を返すと列挙を打ち切る
  template <class Callback> void Query(const BoundingBox &box, Callback &&cb) const;

  /// @brief 線分（origin + dir * t, 0 <= t <= maxT）が通る葉を列挙する
  /// @details 各ノードの AABB を inflate だけ広げて判定するので、球を飛ばす判定（SphereCast）にも使える。
  ///          コールバックが返した値で以降の探索範囲を縮める（最も近い物だけ欲しいときに無駄な葉を飛ばせる）。
  /// @param origin  起点
  /// @param dir     方向（正規化していなくてもよい。t は dir の長さ単位になる）
  /// @param maxT    探索する t の上限
  /// @param inflate ノードの AABB を広げる量（レイなら 0、球なら半径）
  /// @param cb      float(int32_t proxyId, float currentMaxT)。
  ///                新しい上限を返す（変えないなら currentMaxT、0 以下で打ち切り）
  template <class Callback>
  void RayCast(const Vector3 &origin, const Vector3 &dir, float maxT, float inflate, Callback &&cb) const;

  /// @brief 線分と AABB の交差区間を求める（スラブ法）
  /// @param origin 起点
  /// @param invDir 方向の各成分の逆数（成分が 0 なら ±無限大でよい）
  /// @param box    判定する箱
  /// @param maxT   t の上限
  /// @param tEnter 入る位置の t（起点が箱の中なら 0）
  /// @return [0, maxT] の範囲で交差していれば true
  static bool SegmentOverlapsBox(const Vector3 &origin, const Vector3 &invDir, const BoundingBox &box,
                                 float maxT, float &tEnter) {
    float t0 = 0.0f;
    float t1 = maxT;
    const float o[3] = {origin.x, origin.y, origin.z};
    const float inv[3] = {invDir.x, invDir.y, invDir.z};
    const float lo[3] = {box.min.x, box.min.y, box.min.z};
    const float hi[3] = {box.max.x, box.max.y, box.max.z};
    for (int a = 0; a < 3; ++a) {
      if (std::isinf(inv[a])) {
        // この軸に平行：起点がスラブの外なら絶対に当たらない
        if (o[a] < lo[a] || o[a] > hi[a]) return false;
        continue;
      }
      float tn = (lo[a] - o[a]) * inv[a];
      float tf = (hi[a] - o[a]) * inv[a];
      if (tn > tf) std::swap(tn, tf);
      if (tn > t0) t0 = tn;
      if (tf < t1) t1 = tf;
      if (t0 > t1) return false;
    }
    tEnter = t0;
    return true;
  }

  /// @brief 視錐台にかかる葉を列挙する（保守的：fat AABB で判定するので多めに返ることはあるが、漏れはない）
  /// @details 完全に内側の部分木は以降の判定を省いて丸ごと採用する。
  /// @param frustum 視錐台
  /// @param cb      void(int32_t proxyId)
  template <class Callback> void QueryFrustum(const Frustum &frustum, Callback &&cb) const;

  /// @brief 全ての葉を列挙する
  /// @param cb void(int32_t proxyId)
  template <class Callback> void ForEachProxy(Callback &&cb) const;

  /// @brief 全て削除する
  void Clear();

  /// @brief 木の高さを取得する
  /// @return 根の高さ（葉だけなら 0）
  int32_t GetHeight() const { return (root_ == kNullNode) ? 0 : nodes_[root_].height; }

  /// @brief 葉の数を取得する
  /// @return 登録中のプロキシ数
  int32_t GetProxyCount() const { return proxyCount_; }

  /// @brief 構造が壊れていないか検証する（デバッグ・テスト用。O(N)）
  /// @return 親子関係・高さ・箱・葉の数・フリーリストがすべて整合していれば true
  bool Validate() const;

  /// @brief 太らせ量を取得する
  /// @return 葉の AABB を太らせる量（m）
  float GetFatMargin() const { return fatMargin_; }

private:
  /// @brief 木のノード。葉は child1 == kNullNode
  struct Node {
    BoundingBox aabb;              ///< 葉: fat AABB / 内部ノード: 子の和
    uint32_t userData = 0;         ///< 葉のみ有効
    int32_t parent = kNullNode;    ///< 親（未使用ノードではフリーリストの next）
    int32_t child1 = kNullNode;    ///< 子 1
    int32_t child2 = kNullNode;    ///< 子 2
    int32_t height = -1;           ///< 葉 = 0、未使用 = -1
    /// @brief 葉か
    /// @return child1 が無ければ true
    bool IsLeaf() const { return child1 == kNullNode; }
  };

  /// @brief ノードを 1 つ確保する（フリーリストに空きがあれば再利用する）
  /// @return ノード番号（height = 0 で初期化済み）
  /// @note nodes_ が再確保されうるので、呼び出し前に取ったノードへの参照は無効になる。
  int32_t AllocateNode_();
  /// @brief ノードをフリーリストへ返す
  /// @param node ノード番号
  void FreeNode_(int32_t node);
  /// @brief 葉を木に挿入する（SAH で兄弟を選び、祖先を直しながら回転する）
  /// @param leaf 葉のノード番号
  void InsertLeaf_(int32_t leaf);
  /// @brief 葉を木から外す（親ノードは解放し、兄弟を祖父へつなぐ）
  /// @param leaf 葉のノード番号
  void RemoveLeaf_(int32_t leaf);
  /// @brief 子の高さが 2 以上ずれていれば回転して偏りを直す
  /// @param a 対象のノード番号
  /// @return 回転後に a の位置に来たノード番号
  int32_t Balance_(int32_t a);
  /// @brief index から根まで、回転しつつ AABB と高さを更新する
  /// @param index 開始ノード番号
  void RefitUpward_(int32_t index);
  /// @brief 部分木を再帰的に検証する
  /// @param index     対象のノード番号
  /// @param parent    期待する親のノード番号
  /// @param leafCount 見つけた葉の数（加算される）
  /// @param ok        不整合があれば false にされる
  /// @return 部分木の実際の高さ
  int32_t ValidateNode_(int32_t index, int32_t parent, int32_t &leafCount, bool &ok) const;
  /// @brief 実形状の AABB から fat AABB を作る
  /// @param tight        実形状の AABB
  /// @param displacement 今回の移動量（進行方向へだけ displacementMultiplier_ 倍伸ばす）
  /// @return 太らせた AABB
  BoundingBox MakeFat_(const BoundingBox &tight, const Vector3 &displacement) const;

  std::vector<Node> nodes_;             ///< ノード配列（未使用ノードも含む）
  int32_t root_ = kNullNode;            ///< 根のノード番号
  int32_t freeList_ = kNullNode;        ///< 未使用ノードのフリーリストの先頭
  int32_t proxyCount_ = 0;              ///< 葉の数
  float fatMargin_ = 0.1f;              ///< 葉の AABB を太らせる量（m）
  float displacementMultiplier_ = 2.0f; ///< 移動量の先読み倍率
};

// ----------------------------------------------------------------------------
// テンプレート実装
// ----------------------------------------------------------------------------
namespace detail {
/// @brief 探索用スタック。浅い木（ほぼ全て）は固定長配列で回し、溢れたときだけヒープを使う
class BvhStack {
public:
  /// @brief 値を積む
  /// @param v 積む値
  void Push(int32_t v) {
    if (size_ < kInline) { inline_[size_++] = v; return; }
    overflow_.push_back(v);
    ++size_;
  }
  /// @brief 値を取り出す
  /// @return 最後に積んだ値
  /// @note 空のときに呼ばないこと。
  int32_t Pop() {
    --size_;
    if (size_ >= kInline) { const int32_t v = overflow_.back(); overflow_.pop_back(); return v; }
    return inline_[size_];
  }
  /// @brief 空か
  /// @return 要素が無ければ true
  bool Empty() const { return size_ == 0; }

private:
  static constexpr int32_t kInline = 256; ///< 固定長配列に入る要素数
  int32_t inline_[kInline];               ///< 先頭 kInline 個の格納先
  int32_t size_ = 0;                      ///< 総要素数
  std::vector<int32_t> overflow_;         ///< kInline を超えた分の格納先
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
void DynamicBVH::RayCast(const Vector3 &origin, const Vector3 &dir, float maxT, float inflate,
                         Callback &&cb) const {
  if (root_ == kNullNode || !(maxT > 0.0f)) return;
  const auto inv = [](float d) {
    return (d != 0.0f) ? 1.0f / d : std::numeric_limits<float>::infinity();
  };
  const Vector3 invDir = {inv(dir.x), inv(dir.y), inv(dir.z)};
  detail::BvhStack stack;
  stack.Push(root_);
  while (!stack.Empty()) {
    const int32_t id = stack.Pop();
    const Node &n = nodes_[id];
    float tEnter = 0.0f;
    const BoundingBox box = (inflate > 0.0f) ? n.aabb.Expanded(inflate) : n.aabb;
    if (!SegmentOverlapsBox(origin, invDir, box, maxT, tEnter)) continue;
    if (n.IsLeaf()) {
      const float newMax = cb(id, maxT);
      if (!(newMax > 0.0f)) return; // 0 以下（または NaN）で打ち切り
      maxT = (std::min)(maxT, newMax);
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
