#pragma once

namespace RC {

/// @brief 2 次元ベクトル
struct Vector2 {
  float x, y; ///< x, y 成分
};

/// @brief 2 次元整数ベクトル（グリッド座標用）
struct Vector2Int {
  int x, y; ///< x, y 成分

  /// @brief 等価比較演算子
  /// @param other 比較対象
  /// @return 同一座標なら true
  bool operator==(const Vector2Int &other) const {
    return x == other.x && y == other.y;
  }
};

/// @brief 3 次元ベクトル
struct Vector3 {
  float x, y, z; ///< x, y, z 成分
};

/// @brief 4 次元ベクトル
struct Vector4 {
  float x, y, z, w; ///< x, y, z, w 成分
};

/// @brief 3x3 行列
struct Matrix3x3 {
  float m[3][3]; ///< 3x3 の要素配列（[行][列]）
};

/// @brief 4x4 行列
struct Matrix4x4 {
  float m[4][4]; ///< 4x4 の要素配列（[行][列]）
};

/// @brief 回転軸の定義
enum ShaftType {
  X, ///< X 軸
  Y, ///< Y 軸
  Z  ///< Z 軸
};

/// @brief 2 次元の AABB（軸平行境界矩形）
struct Aabb2D {
  float left = 0;   ///< 左端座標
  float right = 0;  ///< 右端座標
  float bottom = 0; ///< 下端座標
  float top = 0;    ///< 上端座標
};

/// @brief クォータニオン（四元数）
struct Quaternion {
  float x, y, z, w; ///< x, y, z, w 成分
};

/// @brief 3 次元空間のレイ（半直線）
struct Ray {
  Vector3 origin;    ///< レイの起点
  Vector3 direction; ///< レイの方向（正規化されていること）
};

} // namespace RC
