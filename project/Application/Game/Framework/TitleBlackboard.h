#pragma once
#include "Math/MathTypes.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>
#include <vector>

// ============================================================================
// タイトル画面のスクリプト同士で共有する「掲示板」
//
//   タイトルのスクリプト（TitleScreen / TitleAmbient / TitleMouseRipple / TitleOctopus）は
//   互いを直接参照しない方針。そのうえで「船が文字を押しのける」「タコが文字にしがみつく」
//   「カーソルで船が揺れる」のように、別のスクリプトが持つ物の位置を知りたい場面がある。
//   そこで値だけを置く場所をここに用意する（書く人・読む人がそれぞれ決まっている）。
//
//     Letters / Menu / LettersInteractive … TitleScreenScript が毎フレーム書く（PublishLetters）
//     文字の押しのけ要求                   … TitleAmbient / TitleOctopus が PushLetter で書き、
//                                            TitleScreenScript が ConsumeLetterPush / ConsumeLetterShake で読んで 0 に戻す
//     Cursor*                              … TitleMouseRippleScript が毎フレーム書く（SetCursor / InvalidateCursor）
//
//   値はすべて State の private メンバに閉じ込め、書き換えは「誰が・何を」書くかが名前で分かる
//   専用関数だけに絞っている（以前は public なメンバ変数を誰でも直接書き換えられた）。
//   読む側は const 参照の getter だけを使う。
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
///          焼いたあとは読むだけなので、格子の値はコンストラクタでだけ受け取り private に閉じ込める。
class LetterMask {
public:
  /// @brief 空の距離場（Sample は常に「遠い外側」を返す）
  LetterMask() = default;

  /// @brief 焼いた格子から距離場を作る
  /// @param w 格子の横の数
  /// @param h 格子の縦の数
  /// @param x0 格子 (0, 0) の中心のメッシュ X
  /// @param y0 格子 (0, 0) の中心のメッシュ Y
  /// @param cell 格子 1 つの大きさ（メッシュ単位）
  /// @param sdf w * h 個の距離（負が文字の中）
  LetterMask(int w, int h, float x0, float y0, float cell, std::vector<float> sdf)
      : w_(w), h_(h), x0_(x0), y0_(y0), cell_(cell), sdf_(std::move(sdf)) {}

  /// @brief メッシュ座標での符号付き距離（双線形補間。格子の外は外側の距離を足す）
  /// @param mx メッシュ X
  /// @param my メッシュ Y
  /// @return 距離（メッシュ単位。負が中）
  float Sample(float mx, float my) const {
    if (w_ < 2 || h_ < 2 || sdf_.size() < static_cast<size_t>(w_) * static_cast<size_t>(h_)) return 1e3f;
    const float gx = (mx - x0_) / cell_, gy = (my - y0_) / cell_;
    const float cx = std::clamp(gx, 0.0f, static_cast<float>(w_ - 1));
    const float cy = std::clamp(gy, 0.0f, static_cast<float>(h_ - 1));
    const float outside = std::sqrt((gx - cx) * (gx - cx) + (gy - cy) * (gy - cy)) * cell_;
    const int ix = (std::min)(static_cast<int>(cx), w_ - 2);
    const int iy = (std::min)(static_cast<int>(cy), h_ - 2);
    const float fx = cx - static_cast<float>(ix), fy = cy - static_cast<float>(iy);
    const float a = sdf_[iy * w_ + ix], b = sdf_[iy * w_ + ix + 1];
    const float c = sdf_[(iy + 1) * w_ + ix], d = sdf_[(iy + 1) * w_ + ix + 1];
    const float v = (a + (b - a) * fx) * (1.0f - fy) + (c + (d - c) * fx) * fy;
    return v + outside;
  }

private:
  int w_ = 0;              ///< 格子の横の数
  int h_ = 0;              ///< 格子の縦の数
  float x0_ = 0.0f;        ///< 格子 (0, 0) の中心のメッシュ X
  float y0_ = 0.0f;        ///< 格子 (0, 0) の中心のメッシュ Y
  float cell_ = 1.0f;      ///< 格子 1 つの大きさ（メッシュ単位）
  std::vector<float> sdf_; ///< w * h 個。負が文字の中
};

/// @brief 水面に浮いているタイトル文字 1 つの今の姿勢（真上から見た足元の長方形）
/// @details 不変条件を持たない受け渡し用の値の束なので struct のままにしている（01_01 の例外）。
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

/// @brief 共有する値（掲示板の中身）
/// @details 値は private。読む側は const の getter、書く側は用途ごとの専用関数だけを使う。
class State {
public:
  // ------------------------------------------------------------------
  // 読み取り（誰でも）
  // ------------------------------------------------------------------

  /// @brief タイトル文字の今の姿勢
  /// @return 文字の並び（読み取り専用）
  const std::vector<Letter> &Letters() const { return letters_; }

  /// @brief メニュー項目の今の姿勢（押しのけはしない。よける・しがみつく判定用）
  /// @return メニュー項目の並び（読み取り専用）
  const std::vector<Letter> &Menu() const { return menu_; }

  /// @brief 文字に触れてよい状態か（起動演出・飛び込み・浮上の最中は false）
  /// @return 触れてよければ true
  bool LettersInteractive() const { return lettersInteractive_; }

  /// @brief カーソルが画面内で水面に当たっているか
  /// @return 当たっていれば true
  bool CursorValid() const { return cursorValid_; }

  /// @brief カーソルが指す水面上の点（ワールド）
  /// @return 位置（無効になっても最後の値を保つ）
  const RC::Vector3 &CursorPos() const { return cursorPos_; }

  /// @brief カーソルが指す点の速度（m/s。なめらかにしたもの）
  /// @return 速度
  const RC::Vector3 &CursorVel() const { return cursorVel_; }

  /// @brief このフレームに左クリックされたか
  /// @return クリックされていれば true
  bool CursorClicked() const { return cursorClicked_; }

  // ------------------------------------------------------------------
  // 文字（TitleScreenScript が書く）
  // ------------------------------------------------------------------

  /// @brief 文字・メニューの今の姿勢を書き出す
  /// @param letters タイトル文字の姿勢
  /// @param menu メニュー項目の姿勢
  /// @param interactive 文字に触れてよい状態か
  /// @details 押しのけ・揺れの要求の受け皿は文字と同じ数にそろえる（数が変わったときだけ 0 で作り直す）。
  void PublishLetters(const std::vector<Letter> &letters, const std::vector<Letter> &menu, bool interactive) {
    lettersInteractive_ = interactive;
    if (letterPush_.size() != letters.size()) letterPush_.assign(letters.size(), RC::Vector3{0.0f, 0.0f, 0.0f});
    if (letterShake_.size() != letters.size()) letterShake_.assign(letters.size(), 0.0f);
    letters_ = letters;
    menu_ = menu;
  }

  /// @brief 溜まっている押しのけ要求を取り出して 0 に戻す
  /// @param index 文字の番号
  /// @param outPush 押しのけたい量（xz、m）の出力先
  /// @return 受け皿があれば true（範囲外なら false で outPush は触らない）
  bool ConsumeLetterPush(size_t index, RC::Vector3 &outPush) {
    if (index >= letterPush_.size()) return false;
    outPush = letterPush_[index];
    letterPush_[index] = {0.0f, 0.0f, 0.0f};
    return true;
  }

  /// @brief 溜まっている揺れの要求を取り出して 0 に戻す
  /// @param index 文字の番号
  /// @param outShake 揺らしたい強さの出力先
  /// @return 受け皿があれば true（範囲外なら false で outShake は触らない）
  bool ConsumeLetterShake(size_t index, float &outShake) {
    if (index >= letterShake_.size()) return false;
    outShake = letterShake_[index];
    letterShake_[index] = 0.0f;
    return true;
  }

  /// @brief 文字・メニューの情報をすべて消す（TitleScreenScript の OnDestroy）
  void ClearLetters() {
    letters_.clear();
    menu_.clear();
    letterPush_.clear();
    letterShake_.clear();
    lettersInteractive_ = false;
  }

  // ------------------------------------------------------------------
  // 押しのけ要求（TitleAmbient / TitleOctopus が書く）
  // ------------------------------------------------------------------

  /// @brief 文字の押しのけ要求を足し込む（添字が範囲外なら何もしない）
  /// @param index 文字の番号
  /// @param push 押しのけたい量（xz、m）
  /// @param shake 揺らしたい強さ（0 以上）
  void PushLetter(size_t index, const RC::Vector3 &push, float shake) {
    if (index >= letters_.size()) return;
    if (letterPush_.size() < letters_.size()) letterPush_.resize(letters_.size(), RC::Vector3{0.0f, 0.0f, 0.0f});
    if (letterShake_.size() < letters_.size()) letterShake_.resize(letters_.size(), 0.0f);
    letterPush_[index].x += push.x;
    letterPush_[index].z += push.z;
    letterShake_[index] = (std::max)(letterShake_[index], shake);
  }

  // ------------------------------------------------------------------
  // カーソル（TitleMouseRippleScript が書く）
  // ------------------------------------------------------------------

  /// @brief カーソルが水面に当たっていることと、その位置・速さ・クリックを書く
  /// @param pos カーソルが指す水面上の点（ワールド）
  /// @param vel その点の速度（m/s）
  /// @param clicked このフレームに左クリックされたか
  void SetCursor(const RC::Vector3 &pos, const RC::Vector3 &vel, bool clicked) {
    cursorValid_ = true;
    cursorPos_ = pos;
    cursorVel_ = vel;
    cursorClicked_ = clicked;
  }

  /// @brief このフレームはカーソルが無効（位置・速さは最後の値を保つ）
  void InvalidateCursor() {
    cursorValid_ = false;
    cursorClicked_ = false;
  }

  /// @brief カーソルの情報を消す（TitleMouseRippleScript の OnDestroy。速さも 0 に戻す）
  void ResetCursor() {
    InvalidateCursor();
    cursorVel_ = {0.0f, 0.0f, 0.0f};
  }

private:
  // ---- 文字（TitleScreenScript が書く）----
  std::vector<Letter> letters_;          ///< タイトル文字の姿勢
  bool lettersInteractive_ = false;      ///< 文字に触れてよい状態か
  std::vector<RC::Vector3> letterPush_;  ///< 押しのけたい量（xz、m）。letters_ と同じ添字。読んだら 0 に戻す
  std::vector<float> letterShake_;       ///< 揺らしたい強さ。letters_ と同じ添字。読んだら 0 に戻す
  std::vector<Letter> menu_;             ///< メニュー項目の姿勢

  // ---- カーソル（TitleMouseRippleScript が書く）----
  bool cursorValid_ = false;   ///< カーソルが画面内で水面に当たっているか
  RC::Vector3 cursorPos_{};    ///< カーソルが指す水面上の点（ワールド）
  RC::Vector3 cursorVel_{};    ///< その点の速度（m/s。なめらかにしたもの）
  bool cursorClicked_ = false; ///< このフレームに左クリックされたか
};

/// @brief 共有の値（1 つだけ）
/// @details 実体は関数内 static に閉じ込め、名前空間スコープの書き換え可能な変数は置かない。
///          中身は State の private メンバなので、この参照からも専用関数経由でしか書き換えられない。
/// @return 共有の値への参照（読むだけなら const 参照で受けること）
inline State &Get() {
  static State s;
  return s;
}

/// @brief 文字の押しのけ要求を足し込む（添字が範囲外なら何もしない）
/// @param index 文字の番号
/// @param push 押しのけたい量（xz、m）
/// @param shake 揺らしたい強さ（0 以上）
inline void PushLetter(size_t index, const RC::Vector3 &push, float shake) { Get().PushLetter(index, push, shake); }

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
