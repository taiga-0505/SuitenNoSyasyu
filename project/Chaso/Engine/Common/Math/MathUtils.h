#pragma once
#include "MathTypes.h" // RC::Vector3 / Aabb2D / Ray など
#include "Math.h"
#include <algorithm>
#include <cmath>

namespace RC {

/// @brief 中心座標とサイズから AABB2D を作成する
/// @param pos 中心座標（x, y のみ使う）
/// @param halfW 幅の半分
/// @param halfH 高さの半分
/// @return 作成された AABB2D
inline Aabb2D MakeAabb2D(const RC::Vector3 &pos, float halfW, float halfH) {
  Aabb2D a;
  a.left = pos.x - halfW;
  a.right = pos.x + halfW;
  a.bottom = pos.y - halfH;
  a.top = pos.y + halfH;
  return a;
}

/// @brief 2 つの AABB2D が重なっているか（衝突しているか）を判定する
/// @param a AABB 1
/// @param b AABB 2
/// @return 重なっていれば true
inline bool OverlapAabb(const Aabb2D &a, const Aabb2D &b) {
  if (a.right <= b.left)
    return false;
  if (a.left >= b.right)
    return false;
  if (a.top <= b.bottom)
    return false;
  if (a.bottom >= b.top)
    return false;
  return true;
}

// ----- スカラーの補助関数 -----

/// @brief 値を最小値と最大値の範囲内に収める
/// @param v 対象の値
/// @param mn 最小値
/// @param mx 最大値
/// @return クランプ後の値
inline float Clamp(float v, float mn, float mx) {
  return std::clamp(v, mn, mx);
}

/// @brief 値を 0.0〜1.0 の範囲に収める
/// @param v 対象の値
/// @return クランプ後の値
inline float Clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }

/// @brief 値を 0.0〜1.0 の範囲に収める（Clamp01 と同じ）
/// @param v 対象の値
/// @return クランプ後の値
inline float Saturate(float v) { return std::clamp(v, 0.0f, 1.0f); }

/// @brief 2 つの値の間を線形補間する
/// @param a 開始値
/// @param b 終了値
/// @param t 補間係数（0.0〜1.0）
/// @return 補間結果
inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }

/// @brief シャープネスとデルタタイムから、指数スムージング用の補間係数を計算する
/// @param sharpness 追従の鋭さ（大きいほど速く追従）
/// @param dt フレーム経過時間（秒）
/// @return 補間係数（Lerp の t として使用可能）
inline float ExpSmoothingFactor(float sharpness, float dt) {
  if (sharpness <= 0.0f)
    return 1.0f;
  if (dt <= 0.0f)
    return 0.0f;
  return 1.0f - std::exp(-sharpness * dt);
}

// ----- Vector3 の基本演算 -----

/// @brief Vector3 の加算
/// @param a ベクトル 1
/// @param b ベクトル 2
/// @return 加算結果
inline Vector3 Add(const Vector3 &a, const Vector3 &b) {
  return Vector3{a.x + b.x, a.y + b.y, a.z + b.z};
}

/// @brief Vector3 の減算
/// @param a ベクトル 1
/// @param b ベクトル 2
/// @return 減算結果
inline Vector3 Sub(const Vector3 &a, const Vector3 &b) {
  return Vector3{a.x - b.x, a.y - b.y, a.z - b.z};
}

/// @brief Vector3 のスカラー倍
/// @param v ベクトル
/// @param s 倍率
/// @return 乗算結果
inline Vector3 Mul(const Vector3 &v, float s) {
  return Vector3{v.x * s, v.y * s, v.z * s};
}

/// @brief Vector3 の内積
/// @param a ベクトル 1
/// @param b ベクトル 2
/// @return 内積結果
inline float Dot(const Vector3 &a, const Vector3 &b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

/// @brief Vector3 の長さの 2 乗を取得
/// @param v ベクトル
/// @return 長さの 2 乗
inline float LengthSq(const Vector3 &v) { return Dot(v, v); }

/// @brief Vector3 の長さを取得
/// @param v ベクトル
/// @return 長さ
inline float Length(const Vector3 &v) { return std::sqrt(LengthSq(v)); }

/// @brief 長さが 0 に近い場合にフォールバック値を返す、安全な正規化処理
/// @param v 正規化するベクトル
/// @param fallback 長さが 0 だった場合に返す値
/// @return 正規化後のベクトル
inline Vector3 SafeNormalize(const Vector3 &v,
                             const Vector3 &fallback = Vector3{0, 0, 1}) {
  const float lsq = LengthSq(v);
  if (lsq <= 1e-12f)
    return fallback;
  const float inv = 1.0f / std::sqrt(lsq);
  return Vector3{v.x * inv, v.y * inv, v.z * inv};
}

/// @brief Vector3 の線形補間
/// @param a 開始ベクトル
/// @param b 終了ベクトル
/// @param t 補間係数（0.0〜1.0）
/// @return 補間結果
inline Vector3 Lerp(const Vector3 &a, const Vector3 &b, float t) {
  return Vector3{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                 a.z + (b.z - a.z) * t};
}

/// @brief レイと AABB の交差判定（スラブ法）
/// @param ray 交差判定を行うレイ
/// @param min AABB の最小座標
/// @param max AABB の最大座標
/// @param outDistance 交差した場合の距離（レイの起点が箱の中なら出口までの距離）
/// @return 交差していれば true
inline bool IntersectRayAABB(const Ray& ray, const Vector3& min, const Vector3& max, float& outDistance) {
  auto safe_inv = [](float d) { return d != 0.0f ? 1.0f / d : 1.0f / 1e-6f; };
  float invDx = safe_inv(ray.direction.x);
  float invDy = safe_inv(ray.direction.y);
  float invDz = safe_inv(ray.direction.z);

  float tmin = (min.x - ray.origin.x) * invDx;
  float tmax = (max.x - ray.origin.x) * invDx;
  if (tmin > tmax) std::swap(tmin, tmax);

  float tymin = (min.y - ray.origin.y) * invDy;
  float tymax = (max.y - ray.origin.y) * invDy;
  if (tymin > tymax) std::swap(tymin, tymax);

  if ((tmin > tymax) || (tymin > tmax)) return false;

  if (tymin > tmin) tmin = tymin;
  if (tymax < tmax) tmax = tymax;

  float tzmin = (min.z - ray.origin.z) * invDz;
  float tzmax = (max.z - ray.origin.z) * invDz;
  if (tzmin > tzmax) std::swap(tzmin, tzmax);

  if ((tmin > tzmax) || (tzmin > tmax)) return false;

  if (tzmin > tmin) tmin = tzmin;
  if (tzmax < tmax) tmax = tzmax;

  if (tmax < 0.0f) return false;

  outDistance = tmin >= 0.0f ? tmin : tmax;
  return true;
}

/// @brief レイとスフィア（球）の交差判定
/// @param ray 交差判定を行うレイ
/// @param center スフィアの中心座標
/// @param radius スフィアの半径
/// @param outDistance 交差した場合の距離（レイの起点が球の中なら出口までの距離）
/// @return 交差していれば true
/// @note ray.direction は正規化されていること（判別式を簡略化しているため）。
inline bool IntersectRaySphere(const Ray& ray, const Vector3& center, float radius, float& outDistance) {
    Vector3 oc = Sub(ray.origin, center);
    float b = Dot(oc, ray.direction);
    float c = Dot(oc, oc) - radius * radius;
    float discriminant = b * b - c;
    if (discriminant < 0.0f) return false;

    float sqrtD = std::sqrt(discriminant);
    float t1 = -b - sqrtD;
    float t2 = -b + sqrtD;

    if (t2 < 0.0f) return false;
    outDistance = t1 >= 0.0f ? t1 : t2;
    return true;
}

/// @brief スクリーン座標（ピクセル）から 3D 空間の Ray を生成する
/// @param screenPos スクリーン上の 2D 座標
/// @param screenWidth 画面幅
/// @param screenHeight 画面高さ
/// @param viewMatrix カメラのビュー行列
/// @param projMatrix カメラのプロジェクション行列
/// @return 計算された Ray
inline Ray ScreenPointToRay(const Vector2& screenPos, float screenWidth, float screenHeight, const Matrix4x4& viewMatrix, const Matrix4x4& projMatrix) {
    // 1. スクリーン座標を NDC（-1.0〜1.0）に変換
    // 画面左上が (0,0) の座標系を想定
    float ndcX = (screenPos.x / screenWidth) * 2.0f - 1.0f;
    float ndcY = 1.0f - (screenPos.y / screenHeight) * 2.0f;

    // 2. ニアクリップ面とファークリップ面での NDC 座標（Z=0, Z=1 は DirectX 前提）
    Vector3 ndcNear = {ndcX, ndcY, 0.0f};
    Vector3 ndcFar  = {ndcX, ndcY, 1.0f};

    // 3. プロジェクション行列とビュー行列の合成の逆行列を求める
    Matrix4x4 viewProj = Multiply(viewMatrix, projMatrix);
    Matrix4x4 invViewProj = Inverse(viewProj);

    // 4. NDC からワールド座標へ変換
    Vector3 worldNear = Vector3Transform(ndcNear, invViewProj);
    Vector3 worldFar  = Vector3Transform(ndcFar, invViewProj);

    // 5. Ray の生成
    Ray ray;
    ray.origin = worldNear;
    ray.direction = SafeNormalize(Sub(worldFar, worldNear));

    return ray;
}

/// @brief 衝突解決のための結果を格納する構造体
struct CollisionResult {
    bool hit = false;         ///< 衝突しているか
    Vector3 normal = {0,0,0}; ///< 衝突法線（オブジェクト A から B を押し出す方向、または B から A への方向。関数によって定義）
    float depth = 0.0f;       ///< めり込み量
    Vector3 contactPoint = {0,0,0}; ///< 衝突位置
};

/// @brief 球と球の交差判定と押し出しベクトルの計算
/// @param centerA 球 A の中心
/// @param radiusA 球 A の半径
/// @param centerB 球 B の中心
/// @param radiusB 球 B の半径
/// @return hit が true なら、normal は A から B への方向（B を押し出す方向）、depth はめり込み量
inline CollisionResult CheckCollisionSphereSphere(const Vector3& centerA, float radiusA, const Vector3& centerB, float radiusB) {
    CollisionResult result;
    Vector3 diff = Sub(centerB, centerA);
    float distSq = LengthSq(diff);
    float radSum = radiusA + radiusB;
    if (distSq > 0.000001f && distSq < radSum * radSum) {
        float dist = std::sqrt(distSq);
        result.hit = true;
        result.normal = Mul(diff, 1.0f / dist);
        result.depth = radSum - dist;
        result.contactPoint = Add(centerA, Mul(result.normal, radiusA - result.depth * 0.5f));
    } else if (distSq <= 0.000001f) {
        // 完全に重なっている場合
        result.hit = true;
        result.normal = {0.0f, 1.0f, 0.0f}; // 方向が決まらないので上方向に押し出す
        result.depth = radSum;
        result.contactPoint = centerA;
    }
    return result;
}

/// @brief AABB と AABB の交差判定と押し出しベクトルの計算
/// @param minA AABB_A の最小座標
/// @param maxA AABB_A の最大座標
/// @param minB AABB_B の最小座標
/// @param maxB AABB_B の最大座標
/// @return hit が true なら、normal は A から B への方向、depth はめり込み量
inline CollisionResult CheckCollisionAabbAabb(const Vector3& minA, const Vector3& maxA, const Vector3& minB, const Vector3& maxB) {
    CollisionResult result;
    
    float overlapX = (std::min)(maxA.x, maxB.x) - (std::max)(minA.x, minB.x);
    float overlapY = (std::min)(maxA.y, maxB.y) - (std::max)(minA.y, minB.y);
    float overlapZ = (std::min)(maxA.z, maxB.z) - (std::max)(minA.z, minB.z);

    if (overlapX > 0 && overlapY > 0 && overlapZ > 0) {
        result.hit = true;
        
        // 最小のめり込み軸を探す
        if (overlapX < overlapY && overlapX < overlapZ) {
            result.depth = overlapX;
            float centerA_x = (minA.x + maxA.x) * 0.5f;
            float centerB_x = (minB.x + maxB.x) * 0.5f;
            result.normal = {centerB_x > centerA_x ? 1.0f : -1.0f, 0.0f, 0.0f};
        } else if (overlapY < overlapZ) {
            result.depth = overlapY;
            float centerA_y = (minA.y + maxA.y) * 0.5f;
            float centerB_y = (minB.y + maxB.y) * 0.5f;
            result.normal = {0.0f, centerB_y > centerA_y ? 1.0f : -1.0f, 0.0f};
        } else {
            result.depth = overlapZ;
            float centerA_z = (minA.z + maxA.z) * 0.5f;
            float centerB_z = (minB.z + maxB.z) * 0.5f;
            result.normal = {0.0f, 0.0f, centerB_z > centerA_z ? 1.0f : -1.0f};
        }

        // 接触点の近似（重なり領域の中心）
        result.contactPoint = {
            ( (std::max)(minA.x, minB.x) + (std::min)(maxA.x, maxB.x) ) * 0.5f,
            ( (std::max)(minA.y, minB.y) + (std::min)(maxA.y, maxB.y) ) * 0.5f,
            ( (std::max)(minA.z, minB.z) + (std::min)(maxA.z, maxB.z) ) * 0.5f
        };
    }
    return result;
}

/// @brief 球と AABB の交差判定と押し出しベクトルの計算
/// @param centerA 球 A の中心
/// @param radiusA 球 A の半径
/// @param minB AABB_B の最小座標
/// @param maxB AABB_B の最大座標
/// @return hit が true なら、normal は球から AABB への方向（AABB を押し出す方向）、depth はめり込み量
inline CollisionResult CheckCollisionSphereAabb(const Vector3& centerA, float radiusA, const Vector3& minB, const Vector3& maxB) {
    CollisionResult result;
    
    // AABB 上の、球の中心に最も近い点を探す
    float closestX = (std::max)(minB.x, (std::min)(centerA.x, maxB.x));
    float closestY = (std::max)(minB.y, (std::min)(centerA.y, maxB.y));
    float closestZ = (std::max)(minB.z, (std::min)(centerA.z, maxB.z));
    
    Vector3 closestPt = {closestX, closestY, closestZ};
    Vector3 diff = Sub(closestPt, centerA);
    float distSq = LengthSq(diff);
    
    if (distSq > 0.0f && distSq < radiusA * radiusA) {
        float dist = std::sqrt(distSq);
        result.hit = true;
        result.depth = radiusA - dist;
        result.normal = Mul(diff, 1.0f / dist); // 球から AABB の最近傍点への方向
        result.contactPoint = closestPt; // AABB 上の最近傍点を接点とする
    } else if (distSq == 0.0f) {
        // 球の中心が AABB の内部にある場合
        // 最近傍点では方向が決まらないので、球の中心から AABB の中心への方向を法線にする
        Vector3 centerB = { (minB.x + maxB.x)*0.5f, (minB.y + maxB.y)*0.5f, (minB.z + maxB.z)*0.5f };
        Vector3 internalDiff = Sub(centerB, centerA);
        float inDistSq = LengthSq(internalDiff);
        if (inDistSq > 0.000001f) {
            float inDist = std::sqrt(inDistSq);
            result.normal = Mul(internalDiff, 1.0f / inDist);
        } else {
            result.normal = {0.0f, 1.0f, 0.0f};
        }
        result.hit = true;
        result.depth = radiusA; // 近似。正確には AABB の境界までの距離を足す必要があるが、通常これに陥る前に弾かれる
        result.contactPoint = centerA;
    }
    return result;
}

} // namespace RC
