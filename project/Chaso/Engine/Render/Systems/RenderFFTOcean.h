#pragma once
#include <cstdint>
#include <d3d12.h>

namespace RC {

// =============================================================================
// FFT オーシャン（Tessendorf 方式）
// =============================================================================
// 風速・風向きから決まる波のスペクトルを、毎フレーム Compute Shader の逆 FFT で
// 256×256 のテクスチャ（一辺 tileSize m のタイル。繰り返して敷き詰める）に展開する。
// 水面シェーダーは Gerstner 波（主波・詳細波）に、このテクスチャの変位と傾きを足す。
//
//   Displacement（t8, VS / PS）: x = Dx, y = 高さ, z = Dz, w = ∂Dx/∂z
//   Derivatives （t9, PS）     : x = ∂h/∂x, y = ∂h/∂z, z = ∂Dx/∂x, w = ∂Dz/∂z
//   値は「高さスケール 1・尖り 1」の生の値。スケールは WaterParams（gFFTParams）で掛ける。
//
// ※ CPU 側の水面（RC::WaterSurface・浮力）にはまだ反映されない（GPU だけの見た目）。
// シェーダー: Resources/Shader/Water/FFTOcean*.hlsl(i)

/// @brief FFT のテクスチャの一辺（テクセル）。FFTOcean.hlsli の kFFTSize と揃える
inline constexpr int kFFTOceanSize = 256;

/// @struct FFTOceanSettings
/// @brief FFT オーシャンの設定（WaterComponent から毎フレーム渡す）
struct FFTOceanSettings {
  bool enabled = false;           ///< 有効か
  float tileSize = 128.0f;        ///< タイル 1 枚のワールドサイズ（m）。これより長い波は表せない
  float windSpeed = 8.0f;         ///< 風速（m/s）。有義波高はおよそ 0.18 × 風速² / g
  float windDirectionDeg = 35.0f; ///< 風向き（度。+X から +Z へ回る向き）
  float smallWaveCutoff = 0.05f;  ///< これより短い波を弱める長さ（m）
  uint32_t seed = 1;              ///< 乱数の種（変えると波の並びが変わる）
  float heightScale = 1.0f;       ///< 高さの倍率（0 で FFT を足さない）
  float choppiness = 1.0f;        ///< 水平変位の倍率（尖り。大きすぎると波が折り返す）
};

/// @brief FFT 用のテクスチャを作る（RenderContext の初期化後に呼ぶ。初期化済みなら何もしない）
void InitFFTOcean();

/// @brief FFT 用のテクスチャを破棄する（GPU の完了待ちの後に呼ぶ前提）
void TermFFTOcean();

/// @brief このフレームの設定を渡す（水面を描くシーンが毎フレーム呼ぶ）
/// @param settings 設定。風・タイル・種が前回と違えば初期スペクトルを作り直す
/// @note 呼ばれなかったフレームは計算しない（水面の無いシーンでコストを掛けない）。
void SetFFTOcean(const FFTOceanSettings &settings);

/// @brief FFT を 1 フレーム分進める（Compute のコマンドを 3D キューの先頭に積む）
/// @param timeSec 水面の時刻（秒。GetWaterTime と同じ値）
void UpdateFFTOcean(float timeSec);

/// @brief 直近の UpdateFFTOcean で計算したか（水面シェーダーが FFT を足すかどうか）
/// @return 計算したなら true
bool IsFFTOceanActive();

/// @brief 最後に渡された設定
/// @return 設定
const FFTOceanSettings &GetFFTOceanSettings();

/// @brief 波の高さのおおよその振幅（2σ, m。高さスケール込み）。山・谷の色付けの正規化に使う
/// @return 振幅（m）。無効なら 0
float GetFFTOceanAmplitude();

/// @brief 変位テクスチャの SRV（未初期化なら空）
/// @return GPU ディスクリプタハンドル
D3D12_GPU_DESCRIPTOR_HANDLE GetFFTOceanDisplacementMap();

/// @brief 傾きテクスチャの SRV（未初期化なら空）
/// @return GPU ディスクリプタハンドル
D3D12_GPU_DESCRIPTOR_HANDLE GetFFTOceanDerivativeMap();

} // namespace RC
