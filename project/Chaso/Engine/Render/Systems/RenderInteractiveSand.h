#pragma once
#include "Math/MathTypes.h"
#include "struct.h"
#include <cstdint>
#include <d3d12.h>

// =============================================================================
// RenderInteractiveSand - 触れる砂浜
//
// 水面の波紋（RenderInteractiveWater）と同じ考え方で、砂の表面の「跡」を GPU のハイトマップに持つ。
//   R = 跡の高さ（m）。掘ると負、掘った砂が縁に寄った盛り上がりは正。ゆっくり崩れて平らに戻る
//   G = 掘りたての湿り（0〜1）。掘ると下の湿った砂が出て暗くなり、数秒で乾いて薄れる
//   B = 波に洗われた濡れ（0〜1）。波が均した所は濡れて暗く光り、ゆっくり乾く
//   A = 未使用
// ハイトマップは 512×512 テクセルで一辺 kInteractiveSandWorldSize（m）を覆い、
// 覆う範囲（窓）の中心は SetInteractiveSandCenter で動かせる（窓が動いても跡はワールドに残る）。
//
// 砂の地形そのものは CreateSandTerrain で高さの配列からメッシュを作り、DrawSand で描く。
// 砂のシェーダー（Sand.VS / Sand.PS）が、粒・風紋・波打ち際の濡れ・きらめきと、
// ハイトマップの跡（法線と色）をピクセルごとに描く。
// =============================================================================

namespace RC {

/// @brief 砂の跡のハイトマップが覆うワールドの一辺（m）。真上視点の画面（およそ 35m × 20m）をカメラ追従で覆う
inline constexpr float kInteractiveSandWorldSize = 50.0f;

/// @brief 砂の跡のハイトマップの一辺（テクセル）。1 テクセル ≒ 0.1m
inline constexpr int kInteractiveSandTexSize = 512;

/// @brief 砂の跡のシミュレーションを初期化する（ハイトマップ 2 枚を作る）
/// @note RenderContext の初期化後に呼ぶこと。初期化済みなら何もしない。
void InitInteractiveSand();

/// @brief 砂の跡のリソースを破棄する（GPU の完了待ちの後に呼ぶ前提）
void TermInteractiveSand();

/// @brief 溜まった跡・均しを詰めて、シミュレーションを 1 ステップ進めるコマンドを積む
/// @details 砂がしばらく描かれていない（DrawSand も跡の追加も無い）フレームは何もしない。
void UpdateInteractiveSand();

/// @brief ハイトマップが覆う範囲の中心を指定する（テクセル単位に丸める）
/// @param worldX 中心のワールド X（m）
/// @param worldZ 中心のワールド Z（m）
void SetInteractiveSandCenter(float worldX, float worldZ);

/// @brief いま描画に使っているハイトマップの範囲を取得する
/// @param outCenterX 中心のワールド X（m）
/// @param outCenterZ 中心のワールド Z（m）
/// @param outSize    覆う一辺（m）
void GetInteractiveSandWindow(float &outCenterX, float &outCenterZ, float &outSize);

/// @brief 砂を掘る（指でなぞる・足跡を押す）
/// @param worldX ワールド X（m）
/// @param worldZ ワールド Z（m）
/// @param radius 半径（m）
/// @param depth  中心の深さ（m。正の値）。同じ所を何度掘ってもこの深さより深くはならない
/// @param rim    掘った砂が縁に寄った盛り上がりの高さ（m）
/// @param fresh  掘りたての湿り（0〜1。掘った所が暗くなる量）
/// @return 範囲外・上限超え（1 フレーム 128 個）で捨てられたら false
bool AddSandDig(float worldX, float worldZ, float radius, float depth, float rim, float fresh = 1.0f);

/// @brief 砂を均して濡らす（波が寄せて跡を消す）
/// @param worldX ワールド X（m）
/// @param worldZ ワールド Z（m）
/// @param radius 半径（m）
/// @param amount このフレームに均す割合（0〜1。中心の値）
/// @return 範囲外・上限超え（1 フレーム 64 個）で捨てられたら false
bool AddSandSmooth(float worldX, float worldZ, float radius, float amount);

/// @brief 跡の崩れ方・乾き方を設定する（全シーン共通）
/// @param heightKeep 跡の高さの毎フレーム残存率（既定 0.9997。60fps で約 40 秒で半分）
/// @param slump      周りの平均へ寄せる毎フレームの割合（0〜0.25。縁が丸く崩れていく）
/// @param freshKeep  掘りたての湿りの毎フレーム残存率（既定 0.992）
/// @param wetKeep    波の濡れの毎フレーム残存率（既定 0.996）
void SetInteractiveSandParams(float heightKeep, float slump, float freshKeep, float wetKeep);

/// @brief 跡をすべて消す（次の更新で 0 クリアする）
void ClearInteractiveSand();

/// @brief 最新のハイトマップの SRV（R32G32B32A32_FLOAT）。未初期化なら空
/// @return GPU ディスクリプタハンドル
D3D12_GPU_DESCRIPTOR_HANDLE GetInteractiveSandMap();

// -----------------------------------------------------------------------------
// 砂の地形と描画
// -----------------------------------------------------------------------------

/// @struct SandLook
/// @brief 砂の見た目（Sand.PS.hlsl の SandParams へ詰める）
struct SandLook {
  Vector4 dryColor = {0.89f, 0.79f, 0.60f, 1.0f};   ///< 乾いた砂の色
  Vector4 wetColor = {0.62f, 0.52f, 0.38f, 1.0f};   ///< 濡れた砂の色
  Vector4 grainColorA = {0.95f, 0.62f, 0.58f, 1.0f}; ///< 混じる粒の色 1（貝殻のかけら）
  Vector4 grainColorB = {0.36f, 0.32f, 0.30f, 1.0f}; ///< 混じる粒の色 2（黒い粒）
  float waterLevel = 0.0f;       ///< 水面の高さ（m）。これより下と少し上は濡れる
  float wetBand = 0.5f;          ///< 水面からどの高さまで濡れて見えるか（m）
  float grainScale = 10.0f;      ///< 粒の細かさ（1m あたりの粒の数。画面で 1 粒が数ピクセルになる程度に）
  float grainStrength = 0.10f;   ///< 粒による色のばらつき（0〜）
  float speckDensity = 0.03f;    ///< 色の付いた粒の混じる割合（0〜1）
  float rippleWavelength = 0.55f; ///< 風紋の間隔（m）
  float rippleStrength = 0.22f;   ///< 風紋の陰影の強さ（0 で無し）
  float rippleDirDeg = 20.0f;    ///< 風紋が並ぶ向き（度。+X から +Z へ回る。風の吹く向き）
  float markStrength = 1.5f;     ///< 跡（掘った溝・盛り上がり）の陰影の強さ
  float freshDarken = 0.35f;     ///< 掘りたての所の暗さ（0〜1）
  float sparkle = 1.0f;          ///< 乾いた砂のきらめきの強さ（0 で無し）
  float wetShine = 1.0f;         ///< 濡れた砂の照り返しの強さ（0 で無し）
  float time = 0.0f;             ///< 経過時間（秒。きらめきの瞬きに使う）
};

/// @brief 高さの配列から砂の地形メッシュを作る
/// @param heights 高さ（m）。countX * countZ 個、行優先（z が行、x が列）。z = 0 の行が -Z 側
/// @param countX  X 方向の頂点数（2 以上）
/// @param countZ  Z 方向の頂点数（2 以上）
/// @param width   X 方向の長さ（m）。原点を中心に置く
/// @param depth   Z 方向の長さ（m）。原点を中心に置く
/// @return メッシュハンドル（失敗したら -1）。位置は GetSandTransformPtr で動かす
int CreateSandTerrain(const float *heights, uint32_t countX, uint32_t countZ, float width, float depth);

/// @brief 砂の地形の Transform を取得する
/// @param meshHandle CreateSandTerrain の戻り値
/// @return Transform へのポインタ（無効なハンドルなら nullptr）
Transform *GetSandTransformPtr(int meshHandle);

/// @brief 砂の地形を描く（不透明。水面より先に描かれ、深度を書く）
/// @param meshHandle CreateSandTerrain の戻り値
/// @param look       砂の見た目
void DrawSand(int meshHandle, const SandLook &look);

/// @brief 砂の地形メッシュを解放する
/// @param meshHandle CreateSandTerrain の戻り値
void UnloadSandTerrain(int meshHandle);

} // namespace RC
