#pragma once

// ============================================================================
// BoundingBox / Frustum
// ----------------------------------------------------------------------------
// カリングと当たり判定の粗い判定（ブロードフェーズ）で共通に使う軸平行境界箱と視錐台。
// DynamicBVH・RenderProxy・コライダーのブロードフェーズから使うため、
// MathTypes.h 以外に依存しないヘッダーのみの実装にしている（単体テストしやすくするため）。
// ============================================================================

#include "MathTypes.h"
#include <algorithm>
#include <cfloat>

namespace RC {

/// @brief 軸平行境界箱（ワールド空間の min / max）
struct BoundingBox {
  Vector3 min{0.0f, 0.0f, 0.0f}; ///< 最小座標
  Vector3 max{0.0f, 0.0f, 0.0f}; ///< 最大座標

  /// @brief 中心と半サイズから作る
  static BoundingBox FromCenterHalf(const Vector3 &c, const Vector3 &h) {
    return {{c.x - h.x, c.y - h.y, c.z - h.z}, {c.x + h.x, c.y + h.y, c.z + h.z}};
  }

  /// @brief 何も含まない（Union の初期値用）箱
  static BoundingBox Empty() {
    return {{FLT_MAX, FLT_MAX, FLT_MAX}, {-FLT_MAX, -FLT_MAX, -FLT_MAX}};
  }

  /// @brief 2 つの箱を包む箱
  static BoundingBox Union(const BoundingBox &a, const BoundingBox &b) {
    return {{(std::min)(a.min.x, b.min.x), (std::min)(a.min.y, b.min.y), (std::min)(a.min.z, b.min.z)},
            {(std::max)(a.max.x, b.max.x), (std::max)(a.max.y, b.max.y), (std::max)(a.max.z, b.max.z)}};
  }

  /// @brief 点を含むように広げる
  void Encapsulate(const Vector3 &p) {
    min.x = (std::min)(min.x, p.x); min.y = (std::min)(min.y, p.y); min.z = (std::min)(min.z, p.z);
    max.x = (std::max)(max.x, p.x); max.y = (std::max)(max.y, p.y); max.z = (std::max)(max.z, p.z);
  }

  /// @brief min <= max になっているか（Empty() のままなら false）
  bool IsValid() const { return min.x <= max.x && min.y <= max.y && min.z <= max.z; }

  /// @brief 重なっているか（接しているだけでも true）
  bool Overlaps(const BoundingBox &o) const {
    return min.x <= o.max.x && max.x >= o.min.x &&
           min.y <= o.max.y && max.y >= o.min.y &&
           min.z <= o.max.z && max.z >= o.min.z;
  }

  /// @brief o を完全に内包しているか
  bool Contains(const BoundingBox &o) const {
    return min.x <= o.min.x && min.y <= o.min.y && min.z <= o.min.z &&
           o.max.x <= max.x && o.max.y <= max.y && o.max.z <= max.z;
  }

  /// @brief 表面積（BVH の SAH コスト計算用）
  float SurfaceArea() const {
    const float dx = max.x - min.x, dy = max.y - min.y, dz = max.z - min.z;
    return 2.0f * (dx * dy + dy * dz + dz * dx);
  }

  /// @brief 全方向に m だけ広げた箱
  BoundingBox Expanded(float m) const {
    return {{min.x - m, min.y - m, min.z - m}, {max.x + m, max.y + m, max.z + m}};
  }

  /// @brief 箱の中心
  Vector3 Center() const {
    return {(min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f, (min.z + max.z) * 0.5f};
  }

  /// @brief 箱の半サイズ
  Vector3 HalfExtent() const {
    return {(max.x - min.x) * 0.5f, (max.y - min.y) * 0.5f, (max.z - min.z) * 0.5f};
  }

  /// @brief 行列（行ベクトル規約: p' = p * M）で変換した 8 頂点を包む箱
  /// @details Arvo の方法。回転したオブジェクトでも必ず元の形状を内包する（保守的）。
  BoundingBox Transformed(const Matrix4x4 &m) const {
    BoundingBox r;
    const float lo[3] = {min.x, min.y, min.z};
    const float hi[3] = {max.x, max.y, max.z};
    float rmin[3] = {m.m[3][0], m.m[3][1], m.m[3][2]};
    float rmax[3] = {m.m[3][0], m.m[3][1], m.m[3][2]};
    for (int col = 0; col < 3; ++col) {
      for (int row = 0; row < 3; ++row) {
        const float a = m.m[row][col] * lo[row];
        const float b = m.m[row][col] * hi[row];
        rmin[col] += (std::min)(a, b);
        rmax[col] += (std::max)(a, b);
      }
    }
    r.min = {rmin[0], rmin[1], rmin[2]};
    r.max = {rmax[0], rmax[1], rmax[2]};
    return r;
  }
};

/// @brief 視錐台（6 平面）。カリング用
/// @details 平面は (a,b,c,d) で a*x + b*y + c*z + d >= 0 が内側。
///          符号判定しか行わないので正規化はしない。
struct Frustum {
  /// @brief 箱と視錐台の位置関係
  enum class Result {
    Outside,   ///< 完全に外
    Intersect, ///< 境界をまたぐ
    Inside,    ///< 完全に内
  };

  Vector4 planes[6]{};

  /// @brief ViewProjection 行列（行ベクトル規約: clip = p * VP、D3D の z ∈ [0, 1]）から作る
  static Frustum FromViewProjection(const Matrix4x4 &vp) {
    auto col = [&](int c) { return Vector4{vp.m[0][c], vp.m[1][c], vp.m[2][c], vp.m[3][c]}; };
    const Vector4 c0 = col(0), c1 = col(1), c2 = col(2), c3 = col(3);
    auto add = [](const Vector4 &a, const Vector4 &b) { return Vector4{a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w}; };
    auto sub = [](const Vector4 &a, const Vector4 &b) { return Vector4{a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w}; };
    Frustum f;
    f.planes[0] = add(c3, c0); // left   : x >= -w
    f.planes[1] = sub(c3, c0); // right  : x <=  w
    f.planes[2] = add(c3, c1); // bottom : y >= -w
    f.planes[3] = sub(c3, c1); // top    : y <=  w
    f.planes[4] = c2;          // near   : z >=  0
    f.planes[5] = sub(c3, c2); // far    : z <=  w
    return f;
  }

  /// @brief 箱との位置関係を調べる（p-vertex / n-vertex 法）
  Result Classify(const BoundingBox &b) const {
    bool intersect = false;
    for (const Vector4 &p : planes) {
      // 平面の法線方向にいちばん出ている頂点（p-vertex）が外なら箱全体が外
      const float px = (p.x >= 0.0f) ? b.max.x : b.min.x;
      const float py = (p.y >= 0.0f) ? b.max.y : b.min.y;
      const float pz = (p.z >= 0.0f) ? b.max.z : b.min.z;
      if (p.x * px + p.y * py + p.z * pz + p.w < 0.0f) {
        return Result::Outside;
      }
      // 逆側の頂点（n-vertex）が外なら境界をまたいでいる
      const float nx = (p.x >= 0.0f) ? b.min.x : b.max.x;
      const float ny = (p.y >= 0.0f) ? b.min.y : b.max.y;
      const float nz = (p.z >= 0.0f) ? b.min.z : b.max.z;
      if (p.x * nx + p.y * ny + p.z * nz + p.w < 0.0f) {
        intersect = true;
      }
    }
    return intersect ? Result::Intersect : Result::Inside;
  }

  /// @brief 箱が少しでも内側にかかるか（保守的：迷ったら true）
  bool Intersects(const BoundingBox &b) const { return Classify(b) != Result::Outside; }
};

} // namespace RC
