#pragma once
#include "Math/MathTypes.h"
#include <d3d12.h>
#include <vector>

namespace RC {

/// @struct WaveSource
/// @brief 波紋シミュレーションに与える 1 つの波源（泡の波源にも同じ形で使う）
struct WaveSource {
  RC::Vector2 uv;  ///< ハイトマップ上の位置（UV。0〜1）
  float radius;    ///< 半径（UV 空間。0.01 = 1m）
  float strength;  ///< 高さの加算値（負で押し下げ）。泡の波源では足す量
};

/// @brief 波紋ハイトマップが覆うワールドの一辺（m）。UV = xz / この値 + 0.5
/// @details Water.VS.hlsl / Water.PS.hlsl の waveUV 計算と同じ値。変えるときはシェーダー側も揃える。
inline constexpr float kInteractiveWaterWorldSize = 100.0f;

/// @brief インタラクティブな波紋シミュレーションを初期化する（ハイトマップ 3 枚と定数バッファを作る）
/// @note RenderContext の初期化後に呼ぶこと。初期化済みなら何もしない。
void InitInteractiveWater();

/// @brief シミュレーションリソースと読み戻しバッファを破棄する
/// @note GPU の完了待ちの後に呼ぶ前提。
void TermInteractiveWater();

/// @brief 波源を追加する（次の UpdateInteractiveWater で消費される）
/// @param source 追加する波源（1 フレーム 64 個まで。超えた分は捨てる）
void AddWaveSource(const WaveSource& source);

/// @brief ワールド座標を指定して波源を追加する
/// @param worldX   ワールド X（m）
/// @param worldZ   ワールド Z（m）
/// @param radius   半径（UV 空間。0.01 = 1m）。既存の呼び出し側と同じ単位
/// @param strength 高さの加算値（負で押し下げ）
/// @return ハイトマップの範囲外・上限超えで捨てられたら false
bool AddWaveSourceAtWorld(float worldX, float worldZ, float radius, float strength);

// ---------------------------------------------------------------------------
// 泡（白波）
// ---------------------------------------------------------------------------
// ハイトマップに 2 種類の泡を持つ。どちらも波としては伝播しない。Water.PS.hlsl が白く描く。
//   G: 引き波の泡 … 置いた場所に残り、少しずつ滲み（幅が広がり）ながら消える。
//                   船尾のスクリューや尾がかき回した、後ろへ尾を引く白い濁り。1 フレーム 64 個まで
//   B: 波頭の白波 … すぐ消える（既定 0.8/フレーム）。毎フレーム置き直して「今そこにある白」を描く。
//                   船首の砕け波や V 字の腕の白い線。長く残すと腕が外へ動いた跡で V の内側が
//                   塗りつぶされるので、こちらに分けている。1 フレーム 128 個まで

/// @brief ワールド座標を指定して引き波の泡（長く残る）を足す
/// @param worldX ワールド X（m）
/// @param worldZ ワールド Z（m）
/// @param radius 半径（UV 空間。0.01 = 1m）
/// @param amount このフレームに足す量（中心の値。0〜2 にクランプされる。0 以下なら捨てる）
/// @return 範囲外・上限超えで捨てられたら false
bool AddFoamSourceAtWorld(float worldX, float worldZ, float radius, float amount);

/// @brief ワールド座標を指定して波頭の白波（すぐ消える）を足す
/// @param worldX ワールド X（m）
/// @param worldZ ワールド Z（m）
/// @param radius 半径（UV 空間。0.01 = 1m）
/// @param amount このフレームに足す量。置き続けたときの定常値は amount / (1 - crestDecay)
/// @return 範囲外・上限超えで捨てられたら false
bool AddCrestFoamSourceAtWorld(float worldX, float worldZ, float radius, float amount);

/// @brief 泡の残り方を設定する（全シーン共通。既定 0.992 / 0.10 / 0.80）
/// @param decayPerFrame      引き波の毎フレーム残存率（0.992 で 60fps のとき約 1.4 秒で半減）
/// @param spreadPerFrame     引き波の毎フレームの滲み（0〜0.25。大きいほど後ろで幅広くなる）
/// @param crestDecayPerFrame 波頭の毎フレーム残存率（0〜0.99。小さいほど線がくっきり・すぐ消える）
void SetInteractiveFoamParams(float decayPerFrame, float spreadPerFrame, float crestDecayPerFrame);

/// @brief 現在の泡の残り方を取得する
/// @param outDecayPerFrame      引き波の毎フレーム残存率の出力先
/// @param outSpreadPerFrame     引き波の毎フレームの滲みの出力先
/// @param outCrestDecayPerFrame 波頭の毎フレーム残存率の出力先
void GetInteractiveFoamParams(float& outDecayPerFrame, float& outSpreadPerFrame, float& outCrestDecayPerFrame);

/// @brief コンピュートシェーダーを実行し、波のシミュレーションを 1 ステップ進める
/// @details 溜まった波源・泡の波源を定数バッファへ詰めて消費し、CS の実行コマンドを 3D 描画キューの先頭（sortKey 0）に積む。
///          読み戻しが有効なら、コピー済みのハイトマップを CPU 側へ写し、今回の結果のコピーも積む。
void UpdateInteractiveWater();

/// @brief 最新のハイトマップの GPU SRV ハンドルを取得する
/// @return SRV ハンドル（R32G32B32A32_FLOAT。r: 高さ、g: 引き波の泡、b: 波頭の白波）。未初期化なら空
D3D12_GPU_DESCRIPTOR_HANDLE GetInteractiveWaterHeightMap();

// ---------------------------------------------------------------------------
// CPU 読み戻し（任意）
// ---------------------------------------------------------------------------
// 浮遊物を波紋に反応させたいシーン（タイトルなど）だけが有効にする。
// 有効中は毎フレーム GPU→READBACK バッファへ 256KB コピーし、フェンス完了後に
// CPU 側の配列へ写す（1〜2 フレーム遅れ。GPU 待ちはしない）。無効なら一切コストは掛からない。
// 有効にした側が OnDestroy 等で必ず無効に戻すこと。

/// @brief ハイトマップの CPU 読み戻しを有効／無効にする
/// @param enabled true で有効。無効にすると CPU 側の高さを 0 に戻す（バッファは Term まで保持）
void SetInteractiveWaterReadback(bool enabled);

/// @brief CPU 読み戻しが有効か
/// @return 有効なら true
bool IsInteractiveWaterReadbackEnabled();

/// @brief 波紋の高さ（m）を取得する
/// @details Water.VS.hlsl と同じ写像（xz/100+0.5）とサンプラ（linear, clamp）で読む。
/// @param worldX ワールド X（m）
/// @param worldZ ワールド Z（m）
/// @return 波紋の高さ（m）。読み戻しが無効／未着なら 0
float SampleInteractiveWaterHeight(float worldX, float worldZ);

/// @brief 波紋の高さと法線を取得する
/// @param worldX    ワールド X（m）
/// @param worldZ    ワールド Z（m）
/// @param outHeight 波紋の高さ（m）
/// @param outNormal 波紋だけの法線（Water.VS.hlsl の interactiveNormal と同じ有限差分）
/// @return 読み戻しが無効／未着なら false（outHeight=0, outNormal=(0,1,0)）
bool SampleInteractiveWater(float worldX, float worldZ, float& outHeight, Vector3& outNormal);

} // namespace RC
