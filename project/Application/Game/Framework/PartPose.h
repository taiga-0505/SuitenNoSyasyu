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
/// @brief 回転行列をクォータニオンにする（RC::SetModelJointOverride に渡す用）
/// @details エンジンの MakeRotateMatrix(Quaternion) は行ベクトル規約の行列を作るので、
///          ここでも M を「v * M で回す行列」として扱う（M の転置が列ベクトル規約の回転）。
/// @param M 回転行列（行ベクトル規約）
/// @return 同じ回転を表す単位クォータニオン
inline RC::Quaternion ToQuaternion(const Mat3 &M) {
  // 列ベクトル規約の R[i][j] = M[j][i] で標準の式を使う
  const float r00 = M.m[0][0], r11 = M.m[1][1], r22 = M.m[2][2];
  const float r01 = M.m[1][0], r10 = M.m[0][1];
  const float r02 = M.m[2][0], r20 = M.m[0][2];
  const float r12 = M.m[2][1], r21 = M.m[1][2];
  RC::Quaternion q{};
  const float tr = r00 + r11 + r22;
  if (tr > 0.0f) {
    const float s = std::sqrt(tr + 1.0f) * 2.0f;
    q = {(r21 - r12) / s, (r02 - r20) / s, (r10 - r01) / s, 0.25f * s};
  } else if (r00 > r11 && r00 > r22) {
    const float s = std::sqrt(1.0f + r00 - r11 - r22) * 2.0f;
    q = {0.25f * s, (r01 + r10) / s, (r02 + r20) / s, (r21 - r12) / s};
  } else if (r11 > r22) {
    const float s = std::sqrt(1.0f + r11 - r00 - r22) * 2.0f;
    q = {(r01 + r10) / s, 0.25f * s, (r12 + r21) / s, (r02 - r20) / s};
  } else {
    const float s = std::sqrt(1.0f + r22 - r00 - r11) * 2.0f;
    q = {(r02 + r20) / s, (r12 + r21) / s, 0.25f * s, (r10 - r01) / s};
  }
  const float l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  if (l > 1e-8f) { q.x /= l; q.y /= l; q.z /= l; q.w /= l; }
  return q;
}
/// @brief 行列の転置（回転行列なら逆回転）
/// @param M 回転行列
/// @return M の転置
inline Mat3 Transpose(const Mat3 &M) {
  Mat3 r{};
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) r.m[i][j] = M.m[j][i];
  return r;
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
