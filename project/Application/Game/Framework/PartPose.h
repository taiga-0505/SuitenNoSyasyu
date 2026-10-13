#pragma once
#include "Math/MathTypes.h"

#include <algorithm>
#include <cmath>

// ============================================================================
// 部位（プリミティブの組み合わせ）の姿勢合成
//
//   エンジンの描画は Transform の親子を解決しない（PrimitiveMesh へは値がそのまま渡る）ので、
//   「本体に付いた部位」のワールド姿勢はスクリプト側で合成して書き込む必要がある。
//   そのための 3x3 回転行列と、エンジンのオイラー角との相互変換をまとめたもの。
//
//   規約はエンジンと同じ「行ベクトル × 行列」（v' = v * M）。
//   TransformComponent::rotation (a, b, c) は M = Rx(a) * Ry(b) * Rz(c)（MakeAffineMatrix と同じ）。
//
//   使っているもの: TitleAmbientScript / ShipVisualScript
//   （BirdEnemyScript.cpp の BirdDetail も同じ定義。あちらは既存コードなので触っていない）
// ============================================================================

namespace PartPose {

constexpr float kPi = 3.14159265f;
constexpr float kTwoPi = 6.28318530718f;
constexpr float kHalfPi = 1.57079632679f;
constexpr float kDeg = kPi / 180.0f;

// 名前は RC::Add / 全域の Add（Math.h）と衝突しないようにしてある。
// using namespace した先で引数依存の照合により曖昧になるため（C2668）。
/// @brief ベクトルの加算
/// @param a 左辺
/// @param b 右辺
/// @return a + b
inline RC::Vector3 AddV(const RC::Vector3 &a, const RC::Vector3 &b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
/// @brief ベクトルのスカラー倍
/// @param a 対象のベクトル
/// @param s 倍率
/// @return a * s
inline RC::Vector3 ScaleV(const RC::Vector3 &a, float s) { return {a.x * s, a.y * s, a.z * s}; }

/// @brief 部位の姿勢合成に使う 3x3 回転行列（行ベクトル × 行列の規約）
struct Mat3 {
  float m[3][3]; ///< m[行][列]
};

/// @brief 行列の積
/// @param a 左の行列
/// @param b 右の行列
/// @return a * b（行ベクトル規約なので a の回転を先に適用する）
inline Mat3 MatMul(const Mat3 &a, const Mat3 &b) {
  Mat3 r{};
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j)
      r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
  return r;
}
/// @brief 単位行列を返す
/// @return 単位行列
inline Mat3 Identity() { return {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}}; }
/// @brief X / Y / Z 軸まわりの回転行列を作る（Math.cpp の MakeRotateMatrix(X/Y/Z) と同じ並び）
/// @param t 回転角（rad）
/// @return 回転行列
inline Mat3 RotX(float t) {
  const float c = std::cos(t), s = std::sin(t);
  return {{{1, 0, 0}, {0, c, s}, {0, -s, c}}};
}
inline Mat3 RotY(float t) {
  const float c = std::cos(t), s = std::sin(t);
  return {{{c, 0, -s}, {0, 1, 0}, {s, 0, c}}};
}
inline Mat3 RotZ(float t) {
  const float c = std::cos(t), s = std::sin(t);
  return {{{c, s, 0}, {-s, c, 0}, {0, 0, 1}}};
}
/// @brief 行ベクトル v を M で変換する（v * M）
/// @param v 変換するベクトル
/// @param M 回転行列
/// @return 変換後のベクトル
inline RC::Vector3 Apply(const RC::Vector3 &v, const Mat3 &M) {
  return {v.x * M.m[0][0] + v.y * M.m[1][0] + v.z * M.m[2][0],
          v.x * M.m[0][1] + v.y * M.m[1][1] + v.z * M.m[2][1],
          v.x * M.m[0][2] + v.y * M.m[1][2] + v.z * M.m[2][2]};
}
/// @brief TransformComponent::rotation（エンジンのオイラー角）から回転行列を作る
/// @param r オイラー角 (x, y, z)（rad）
/// @return Rx(r.x) * Ry(r.y) * Rz(r.z)
inline Mat3 FromEngineEuler(const RC::Vector3 &r) { return MatMul(MatMul(RotX(r.x), RotY(r.y)), RotZ(r.z)); }
/// @brief M = Rx(a) * Ry(b) * Rz(c) となる (a, b, c) を返す（TransformComponent::rotation に入れる値）
/// @details 展開すると m02 = -sin b, m12 = sin a cos b, m22 = cos a cos b, m01 = sin c cos b, m00 = cos b cos c。
/// @param M 回転行列
/// @return オイラー角 (a, b, c)（rad）
inline RC::Vector3 ToEngineEuler(const Mat3 &M) {
  const float sb = std::clamp(-M.m[0][2], -1.0f, 1.0f);
  const float b = std::asin(sb);
  // cos b ≠ 0 なら m12 / m22 から a、m01 / m00 から c が求まる
  if (std::fabs(sb) < 0.999999f) {
    return {std::atan2(M.m[1][2], M.m[2][2]), b, std::atan2(M.m[0][1], M.m[0][0])};
  }
  // ジンバルロック（真上・真下を向いた）：c = 0 に寄せる。m10 = sin a sin b, m11 = cos a
  return {std::atan2(M.m[1][0] * sb, M.m[1][1]), b, 0.0f};
}
/// @brief +Z が前、+X が右、+Y が上の本体を、ロール→ピッチ→ヨーの順に（本体の軸で）回した姿勢
/// @param yaw     ヨー角（rad。+Y 軸まわり）
/// @param pitchUp 正で機首上げ（Rx は正で機首下げなので符号を反転する）
/// @param roll    正で右側が上がる
/// @return 本体の回転行列
inline Mat3 BodyMatrix(float yaw, float pitchUp, float roll) {
  return MatMul(MatMul(RotZ(roll), RotX(-pitchUp)), RotY(yaw));
}
/// @brief 向き yaw（rad）の前方ベクトル（+Z が yaw 0）
/// @param yaw ヨー角（rad）
/// @return XZ 平面上の単位ベクトル
inline RC::Vector3 Forward(float yaw) { return {std::sin(yaw), 0.0f, std::cos(yaw)}; }

/// @brief 角度を -π〜π の範囲に折り返す
/// @param a 角度（rad）
/// @return 折り返した角度（rad）
inline float WrapAngle(float a) {
  while (a > kPi) a -= kTwoPi;
  while (a < -kPi) a += kTwoPi;
  return a;
}

} // namespace PartPose
