#include "DynamicBVH.h"

#include <cassert>

namespace RC {

// ============================================================================
// ノード管理（フリーリスト）
// ============================================================================

int32_t DynamicBVH::AllocateNode_() {
  if (freeList_ == kNullNode) {
    nodes_.emplace_back();
    const int32_t id = static_cast<int32_t>(nodes_.size()) - 1;
    nodes_[id].height = 0;
    return id;
  }
  const int32_t id = freeList_;
  freeList_ = nodes_[id].parent;
  nodes_[id] = Node{};
  nodes_[id].height = 0;
  return id;
}

void DynamicBVH::FreeNode_(int32_t node) {
  nodes_[node].parent = freeList_;
  nodes_[node].child1 = kNullNode;
  nodes_[node].child2 = kNullNode;
  nodes_[node].height = -1;
  freeList_ = node;
}

void DynamicBVH::Clear() {
  nodes_.clear();
  root_ = kNullNode;
  freeList_ = kNullNode;
  proxyCount_ = 0;
}

// ============================================================================
// プロキシ操作
// ============================================================================

BoundingBox DynamicBVH::MakeFat_(const BoundingBox &tight, const Vector3 &d) const {
  BoundingBox fat = tight.Expanded(fatMargin_);
  // 進行方向へだけ先読みして伸ばす（逆方向は伸ばさない）
  const float dx = d.x * displacementMultiplier_;
  const float dy = d.y * displacementMultiplier_;
  const float dz = d.z * displacementMultiplier_;
  if (dx < 0.0f) fat.min.x += dx; else fat.max.x += dx;
  if (dy < 0.0f) fat.min.y += dy; else fat.max.y += dy;
  if (dz < 0.0f) fat.min.z += dz; else fat.max.z += dz;
  return fat;
}

int32_t DynamicBVH::CreateProxy(const BoundingBox &tight, uint32_t userData) {
  const int32_t id = AllocateNode_();
  nodes_[id].aabb = MakeFat_(tight, {0.0f, 0.0f, 0.0f});
  nodes_[id].userData = userData;
  nodes_[id].height = 0;
  InsertLeaf_(id);
  ++proxyCount_;
  return id;
}

void DynamicBVH::DestroyProxy(int32_t proxyId) {
  assert(proxyId >= 0 && proxyId < static_cast<int32_t>(nodes_.size()));
  assert(nodes_[proxyId].IsLeaf() && nodes_[proxyId].height == 0);
  RemoveLeaf_(proxyId);
  FreeNode_(proxyId);
  --proxyCount_;
}

bool DynamicBVH::MoveProxy(int32_t proxyId, const BoundingBox &tight,
                           const Vector3 &displacement) {
  assert(proxyId >= 0 && proxyId < static_cast<int32_t>(nodes_.size()));
  assert(nodes_[proxyId].IsLeaf());

  const BoundingBox &fat = nodes_[proxyId].aabb;
  if (fat.Contains(tight)) {
    // まだ fat の内側。ただし過去の大きな移動量の先読みで fat が肥大化したまま
    // 止まっている場合は誤検出が増えるので作り直す
    // （基準: 今回の移動量で作り直した fat より、さらに太らせ量の 4 倍以上余っている）
    const BoundingBox huge = MakeFat_(tight, displacement).Expanded(fatMargin_ * 4.0f);
    if (huge.Contains(fat)) {
      return false;
    }
  }

  RemoveLeaf_(proxyId);
  nodes_[proxyId].aabb = MakeFat_(tight, displacement);
  InsertLeaf_(proxyId);
  return true;
}

// ============================================================================
// 挿入・削除
// ============================================================================

void DynamicBVH::InsertLeaf_(int32_t leaf) {
  if (root_ == kNullNode) {
    root_ = leaf;
    nodes_[root_].parent = kNullNode;
    return;
  }

  // --- 1) SAH で兄弟にするノードを探す ---
  const BoundingBox leafAABB = nodes_[leaf].aabb;
  int32_t index = root_;
  while (!nodes_[index].IsLeaf()) {
    const int32_t c1 = nodes_[index].child1;
    const int32_t c2 = nodes_[index].child2;

    const float area = nodes_[index].aabb.SurfaceArea();
    const float combinedArea = BoundingBox::Union(nodes_[index].aabb, leafAABB).SurfaceArea();

    // ここに新しい親を作るコスト
    const float cost = 2.0f * combinedArea;
    // さらに下へ降りる場合に、祖先が広がるぶんのコスト
    const float inheritanceCost = 2.0f * (combinedArea - area);

    auto descendCost = [&](int32_t child) {
      const float unionArea = BoundingBox::Union(leafAABB, nodes_[child].aabb).SurfaceArea();
      if (nodes_[child].IsLeaf()) {
        return unionArea + inheritanceCost;
      }
      return (unionArea - nodes_[child].aabb.SurfaceArea()) + inheritanceCost;
    };
    const float cost1 = descendCost(c1);
    const float cost2 = descendCost(c2);

    if (cost < cost1 && cost < cost2) break;
    index = (cost1 < cost2) ? c1 : c2;
  }
  const int32_t sibling = index;

  // --- 2) 新しい親を作って sibling と leaf をぶら下げる ---
  const int32_t oldParent = nodes_[sibling].parent;
  const int32_t newParent = AllocateNode_(); // ※ nodes_ が再確保されうるので参照は持たない
  nodes_[newParent].parent = oldParent;
  nodes_[newParent].aabb = BoundingBox::Union(leafAABB, nodes_[sibling].aabb);
  nodes_[newParent].height = nodes_[sibling].height + 1;
  nodes_[newParent].child1 = sibling;
  nodes_[newParent].child2 = leaf;
  nodes_[sibling].parent = newParent;
  nodes_[leaf].parent = newParent;

  if (oldParent != kNullNode) {
    if (nodes_[oldParent].child1 == sibling) {
      nodes_[oldParent].child1 = newParent;
    } else {
      nodes_[oldParent].child2 = newParent;
    }
  } else {
    root_ = newParent;
  }

  // --- 3) 祖先の AABB・高さを直しつつ回転でバランスを取る ---
  RefitUpward_(nodes_[leaf].parent);
}

void DynamicBVH::RemoveLeaf_(int32_t leaf) {
  if (leaf == root_) {
    root_ = kNullNode;
    return;
  }

  const int32_t parent = nodes_[leaf].parent;
  const int32_t grandParent = nodes_[parent].parent;
  const int32_t sibling =
      (nodes_[parent].child1 == leaf) ? nodes_[parent].child2 : nodes_[parent].child1;

  if (grandParent != kNullNode) {
    // 親を消して、兄弟を祖父に直接つなぐ
    if (nodes_[grandParent].child1 == parent) {
      nodes_[grandParent].child1 = sibling;
    } else {
      nodes_[grandParent].child2 = sibling;
    }
    nodes_[sibling].parent = grandParent;
    FreeNode_(parent);
    RefitUpward_(grandParent);
  } else {
    root_ = sibling;
    nodes_[sibling].parent = kNullNode;
    FreeNode_(parent);
  }
  nodes_[leaf].parent = kNullNode;
}

void DynamicBVH::RefitUpward_(int32_t index) {
  while (index != kNullNode) {
    index = Balance_(index);
    const int32_t c1 = nodes_[index].child1;
    const int32_t c2 = nodes_[index].child2;
    nodes_[index].height = 1 + (std::max)(nodes_[c1].height, nodes_[c2].height);
    nodes_[index].aabb = BoundingBox::Union(nodes_[c1].aabb, nodes_[c2].aabb);
    index = nodes_[index].parent;
  }
}

// ============================================================================
// 回転（AVL 木と同じ考え方）
// ----------------------------------------------------------------------------
// A の子 B / C の高さが 2 以上ずれていたら、高い側を A の位置へ持ち上げる。
//
//   回転前:  A の子 = [B, C]、C の子 = [F, G]（F の方が高いとする）
//   回転後:  C の子 = [A, F]、A の子 = [B, G]
//   （C が A の位置に上がり、C の子のうち低い方 G を A へ渡す。B が高い場合は左右反転）
//
// 戻り値は回転後に A の位置に来たノード。
// ============================================================================
int32_t DynamicBVH::Balance_(int32_t iA) {
  if (nodes_[iA].IsLeaf() || nodes_[iA].height < 2) {
    return iA;
  }

  const int32_t iB = nodes_[iA].child1;
  const int32_t iC = nodes_[iA].child2;
  const int32_t balance = nodes_[iC].height - nodes_[iB].height;

  // 子 iUp を持ち上げ、iA を iUp の子にする共通処理
  auto rotateUp = [&](int32_t iUp, int32_t iOther, bool upIsChild2) -> int32_t {
    const int32_t iF = nodes_[iUp].child1;
    const int32_t iG = nodes_[iUp].child2;

    // iUp を A の位置へ
    nodes_[iUp].child1 = iA;
    nodes_[iUp].parent = nodes_[iA].parent;
    nodes_[iA].parent = iUp;

    const int32_t upParent = nodes_[iUp].parent;
    if (upParent != kNullNode) {
      if (nodes_[upParent].child1 == iA) {
        nodes_[upParent].child1 = iUp;
      } else {
        nodes_[upParent].child2 = iUp;
      }
    } else {
      root_ = iUp;
    }

    // iUp の子のうち高い方を iUp に残し、低い方を A に渡す
    int32_t keep = iF, give = iG;
    if (nodes_[iF].height < nodes_[iG].height) {
      keep = iG;
      give = iF;
    }
    nodes_[iUp].child2 = keep;
    if (upIsChild2) {
      nodes_[iA].child2 = give; // A の子: [iOther(=B), give]
    } else {
      nodes_[iA].child1 = give; // A の子: [give, iOther(=C)]
    }
    nodes_[give].parent = iA;

    nodes_[iA].aabb = BoundingBox::Union(nodes_[iOther].aabb, nodes_[give].aabb);
    nodes_[iA].height = 1 + (std::max)(nodes_[iOther].height, nodes_[give].height);
    nodes_[iUp].aabb = BoundingBox::Union(nodes_[iA].aabb, nodes_[keep].aabb);
    nodes_[iUp].height = 1 + (std::max)(nodes_[iA].height, nodes_[keep].height);
    return iUp;
  };

  if (balance > 1) {
    return rotateUp(iC, iB, /*upIsChild2=*/true);
  }
  if (balance < -1) {
    return rotateUp(iB, iC, /*upIsChild2=*/false);
  }
  return iA;
}

// ============================================================================
// 検証
// ============================================================================

int32_t DynamicBVH::ValidateNode_(int32_t index, int32_t parent, int32_t &leafCount,
                                  bool &ok) const {
  const Node &n = nodes_[index];
  if (n.parent != parent) ok = false;
  if (n.IsLeaf()) {
    if (n.child2 != kNullNode || n.height != 0) ok = false;
    ++leafCount;
    return 0;
  }
  if (n.child1 == kNullNode || n.child2 == kNullNode) {
    ok = false;
    return 0;
  }
  const int32_t h1 = ValidateNode_(n.child1, index, leafCount, ok);
  const int32_t h2 = ValidateNode_(n.child2, index, leafCount, ok);
  const int32_t h = 1 + (std::max)(h1, h2);
  if (n.height != h) ok = false;
  // 内部ノードの箱は子の和と一致していること
  const BoundingBox u = BoundingBox::Union(nodes_[n.child1].aabb, nodes_[n.child2].aabb);
  if (!(u.Contains(n.aabb) && n.aabb.Contains(u))) ok = false;
  // ※ 単回転しか行わないので「左右の高さ差が常に 1 以内」までは保証しない（Box2D と同じ）。
  //    高さの妥当性はテスト側で O(log N) に収まっているかで見る。
  return h;
}

bool DynamicBVH::Validate() const {
  if (root_ == kNullNode) return proxyCount_ == 0;
  bool ok = true;
  int32_t leafCount = 0;
  ValidateNode_(root_, kNullNode, leafCount, ok);
  if (leafCount != proxyCount_) ok = false;

  // フリーリストと使用中ノードの合計がノード総数と一致すること
  int32_t freeCount = 0;
  for (int32_t i = freeList_; i != kNullNode; i = nodes_[i].parent) {
    ++freeCount;
    if (freeCount > static_cast<int32_t>(nodes_.size())) return false; // 循環
  }
  const int32_t used = (proxyCount_ > 0) ? (2 * proxyCount_ - 1) : 0;
  if (used + freeCount != static_cast<int32_t>(nodes_.size())) ok = false;
  return ok;
}

} // namespace RC
