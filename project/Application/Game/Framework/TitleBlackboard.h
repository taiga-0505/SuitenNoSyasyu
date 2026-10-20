#pragma once
#include "Math/MathTypes.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

// ============================================================================
// タイトル画面のスクリプト同士で共有する「掲示板」
//
//   タイトルのスクリプト（TitleScreen / TitleAmbient / TitleMouseRipple / TitleOctopus）は
//   互いを直接参照しない方針。そのうえで「船が文字を押しのける」「タコが文字にしがみつく」
//   「カーソルで船が揺れる」のように、別のスクリプトが持つ物の位置を知りたい場面がある。
//   そこで値だけを置く場所をここに用意する（書く人・読む人がそれぞれ決まっている）。
//
//     letters / menu / lettersInteractive … TitleScreenScript が毎フレーム書く（文字・メニューの今の姿勢）
//     letterPush                   … TitleAmbient / TitleOctopus が書き、TitleScreenScript が読んで 0 に戻す
//     cursor*                      … TitleMouseRippleScript が毎フレーム書く（カーソルの水面上の位置）
//
//   エンティティの更新順は Title.json の並び（TitleScreen → TitleAmbient → TitleMouseRipple →
//   … → TitleOctopus）。読む側が 1 フレーム古い値を見ることがあるが、見た目には分からない。
//   シーンを抜けるとき（各スクリプトの OnDestroy）に自分が書いた値を消すこと。
// ============================================================================

namespace TitleBlackboard {

/// @brief 文字を真上から見た形（縁取り込み）の符号付き距離場
/// @details TitleScreenScript が文字を作るときに 1 度だけ焼く。座標は文字メッシュのローカル
///          （X: 幅、Y: 文字の上方向。ベースラインが Y = 0）で、単位もメッシュのまま（拡大率 1）。
///          値は「いちばん近い輪郭までの距離」で、文字の中が負・外が正。
struct LetterMask {
  int w = 0;           ///< 格子の横の数
  int h = 0;           ///< 格子の縦の数
  float x0 = 0.0f;     ///< 格子 (0, 0) の中心のメッシュ X
  float y0 = 0.0f;     ///< 格子 (0, 0) の中心のメッシュ Y
  float cell = 1.0f;   ///< 格子 1 つの大きさ（メッシュ単位）
  std::vector<float> sdf; ///< w * h 個。負が文字の中

  /// @brief メッシュ座標での符号付き距離（双線形補間。格子の外は外側の距離を足す）
  /// @param mx メッシュ X
  /// @param my メッシュ Y
  /// @return 距離（メッシュ単位。負が中）
  float Sample(float mx, float my) const {
    if (w < 2 || h < 2 || sdf.size() < static_cast<size_t>(w) * static_cast<size_t>(h)) return 1e3f;
    const float gx = (mx - x0) / cell, gy = (my - y0) / cell;
    const float cx = std::clamp(gx, 0.0f, static_cast<float>(w - 1));
    const float cy = std::clamp(gy, 0.0f, static_cast<float>(h - 1));
    const float outside = std::sqrt((gx - cx) * (gx - cx) + (gy - cy) * (gy - cy)) * cell;
    const int ix = (std::min)(static_cast<int>(cx), w - 2);
    const int iy = (std::min)(static_cast<int>(cy), h - 2);
    const float fx = cx - static_cast<float>(ix), fy = cy - static_cast<float>(iy);
    const float a = sdf[iy * w + ix], b = sdf[iy * w + ix + 1];
    const float c = sdf[(iy + 1) * w + ix], d = sdf[(iy + 1) * w + ix + 1];
    const float v = (a + (b - a) * fx) * (1.0f - fy) + (c + (d - c) * fx) * fy;
    return v + outside;
  }
};

/// @brief 水面に浮いているタイトル文字 1 つの今の姿勢（真上から見た足元の長方形）
struct Letter {
  RC::Vector3 center{};  ///< 足元の長方形の中心（ワールド。y は文字の上面の高さ）
  float halfX = 1.0f;    ///< 長方形の半幅（文字のローカル X 方向、m）
  float halfZ = 1.0f;    ///< 長方形の半高さ（文字のローカル Y ＝ ワールド Z 方向、m）
  float yaw = 0.0f;      ///< 水面上での向き（rad）
  RC::Vector3 rotation{}; ///< 文字エンティティの Transform.rotation（しがみつく物が同じ傾きに乗るため）
  float topY = 0.0f;     ///< 文字の上面の高さ（m）
  bool visible = false;  ///< 起動演出で水中にいる・非表示のあいだは false
  // ---- 形（無ければ長方形として扱う）----
  std::shared_ptr<const LetterMask> mask; ///< 真上から見た形（縁取り込み）
  float maskScale = 1.0f;   ///< メッシュ 1 単位 → m（文字の今の拡大率）
  float maskCenterY = 0.0f; ///< 長方形の中心（center）のメッシュ Y
};

/// @brief 共有する値
struct State {
  // ---- 文字（TitleScreenScript が書く）----
  std::vector<Letter> letters;
  /// 文字に触れてよい状態か（起動演出・飛び込み・浮上の最中は false）
  bool lettersInteractive = false;
  /// 文字を押しのけたい量（xz、m）。letters と同じ添字。書く側は「足りないぶん」を足し込む。
  /// TitleScreenScript が読んだら 0 に戻す
  std::vector<RC::Vector3> letterPush;
  /// 文字を揺らしたい強さ（ぶつかった瞬間の衝撃）。letters と同じ添字。読んだら 0 に戻す
  std::vector<float> letterShake;
  /// メニュー項目（TitleScreenScript が書く。押しのけはしない。よける・しがみつく判定用）
  std::vector<Letter> menu;

  // ---- カーソル（TitleMouseRippleScript が書く）----
  bool cursorValid = false;   ///< カーソルが画面内で水面に当たっているか
  RC::Vector3 cursorPos{};    ///< カーソルが指す水面上の点（ワールド）
  RC::Vector3 cursorVel{};    ///< その点の速度（m/s。なめらかにしたもの）
  bool cursorClicked = false; ///< このフレームに左クリックされたか
};

/// @brief 共有の値（1 つだけ）
/// @return 共有の値への参照
inline State &Get() {
  static State s;
  return s;
}

/// @brief 文字の押しのけ要求を足し込む（添字が範囲外なら何もしない）
/// @param index 文字の番号
/// @param push 押しのけたい量（xz、m）
/// @param shake 揺らしたい強さ（0 以上）
inline void PushLetter(size_t index, const RC::Vector3 &push, float shake) {
  State &s = Get();
  if (index >= s.letters.size()) return;
  if (s.letterPush.size() < s.letters.size()) s.letterPush.resize(s.letters.size(), RC::Vector3{0.0f, 0.0f, 0.0f});
  if (s.letterShake.size() < s.letters.size()) s.letterShake.resize(s.letters.size(), 0.0f);
  s.letterPush[index].x += push.x;
  s.letterPush[index].z += push.z;
  s.letterShake[index] = (std::max)(s.letterShake[index], shake);
}

/// @brief 円と文字の足元の長方形が重なっているかを調べ、円を外へ出す向きと深さを返す
/// @param L 文字
/// @param cx 円の中心 X
/// @param cz 円の中心 Z
/// @param radius 円の半径（m）
/// @param outNormal 文字 → 円 の向き（xz、正規化済み）
/// @param outDepth めり込みの深さ（m）
/// @return 重なっていれば true
inline bool CircleVsLetter(const Letter &L, float cx, float cz, float radius, RC::Vector3 &outNormal,
                           float &outDepth) {
  // 文字のローカル（X: 幅、Z: 高さ）へ移す。文字の回転は Ry(yaw)：ローカル X → (cos, 0, -sin)
  const float c = std::cos(L.yaw), s = std::sin(L.yaw);
  const float dx = cx - L.center.x, dz = cz - L.center.z;
  const float lx = dx * c - dz * s;
  const float lz = dx * s + dz * c;
  const float qx = std::clamp(lx, -L.halfX, L.halfX);
  const float qz = std::clamp(lz, -L.halfZ, L.halfZ);
  float nx = lx - qx, nz = lz - qz;
  float dist = std::sqrt(nx * nx + nz * nz);
  if (dist > radius) return false;
  if (dist > 1e-5f) {
    nx /= dist;
    nz /= dist;
    outDepth = radius - dist;
  } else {
    // 中心が長方形の中：近い辺の方へ出す
    const float ex = L.halfX - std::fabs(lx);
    const float ez = L.halfZ - std::fabs(lz);
    if (ex < ez) {
      nx = (lx >= 0.0f) ? 1.0f : -1.0f;
      nz = 0.0f;
      outDepth = ex + radius;
    } else {
      nx = 0.0f;
      nz = (lz >= 0.0f) ? 1.0f : -1.0f;
      outDepth = ez + radius;
    }
  }
  // ワールドへ戻す（逆回転）
  outNormal = {nx * c + nz * s, 0.0f, -nx * s + nz * c};
  return true;
}

/// @brief ワールド xz → 文字のローカル（X: 幅、Z: 文字の上方向。center が原点、m）
/// @param L 文字
/// @param x ワールド X
/// @param z ワールド Z
/// @param lx ローカル X の出力先
/// @param lz ローカル Z の出力先
inline void ToLetterLocal(const Letter &L, float x, float z, float &lx, float &lz) {
  const float c = std::cos(L.yaw), s = std::sin(L.yaw);
  const float dx = x - L.center.x, dz = z - L.center.z;
  lx = dx * c - dz * s;
  lz = dx * s + dz * c;
}

/// @brief 文字のローカル（m）→ ワールド xz
/// @param L 文字
/// @param lx ローカル X
/// @param lz ローカル Z
/// @param x ワールド X の出力先
/// @param z ワールド Z の出力先
inline void FromLetterLocal(const Letter &L, float lx, float lz, float &x, float &z) {
  const float c = std::cos(L.yaw), s = std::sin(L.yaw);
  x = L.center.x + lx * c + lz * s;
  z = L.center.z - lx * s + lz * c;
}

/// @brief 文字のローカル（m）での、文字の形までの符号付き距離（m。負が文字の上）
/// @param L 文字
/// @param lx ローカル X
/// @param lz ローカル Z
/// @return 距離（形が無ければ足元の長方形で測る）
inline float LetterDistanceLocal(const Letter &L, float lx, float lz) {
  if (L.mask && L.maskScale > 1e-4f) {
    const float inv = 1.0f / L.maskScale;
    return L.mask->Sample(lx * inv, lz * inv + L.maskCenterY) * L.maskScale;
  }
  const float qx = std::fabs(lx) - L.halfX, qz = std::fabs(lz) - L.halfZ;
  const float ox = (std::max)(qx, 0.0f), oz = (std::max)(qz, 0.0f);
  return std::sqrt(ox * ox + oz * oz) + (std::min)((std::max)(qx, qz), 0.0f);
}

/// @brief ワールド xz での、文字の形までの符号付き距離（m。負が文字の上）
/// @param L 文字
/// @param x ワールド X
/// @param z ワールド Z
/// @return 距離
inline float LetterDistance(const Letter &L, float x, float z) {
  float lx = 0.0f, lz = 0.0f;
  ToLetterLocal(L, x, z, lx, lz);
  return LetterDistanceLocal(L, lx, lz);
}

/// @brief 文字の形から離れる向き（ワールド xz、正規化済み。距離場の勾配）
/// @param L 文字
/// @param x ワールド X
/// @param z ワールド Z
/// @return 向き（勾配が取れなければ center から離れる向き）
inline RC::Vector3 LetterOutward(const Letter &L, float x, float z) {
  const float e = 0.06f;
  float gx = LetterDistance(L, x + e, z) - LetterDistance(L, x - e, z);
  float gz = LetterDistance(L, x, z + e) - LetterDistance(L, x, z - e);
  float len = std::sqrt(gx * gx + gz * gz);
  if (len < 1e-5f) {
    gx = x - L.center.x;
    gz = z - L.center.z;
    len = std::sqrt(gx * gx + gz * gz);
    if (len < 1e-5f) return {0.0f, 0.0f, -1.0f};
  }
  return {gx / len, 0.0f, gz / len};
}

} // namespace TitleBlackboard
