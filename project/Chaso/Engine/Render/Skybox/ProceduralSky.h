#pragma once
#include "ProceduralSkySettings.h"
#include <d3d12.h>

namespace RC {

// =============================================================================
// プロシージャル空（ProceduralSky）
// =============================================================================
// 空のグラデーション・太陽・流れる雲を Compute Shader でキューブマップ（一辺 kProceduralSkySize）に描く。
// 出来たキューブマップは Skybox の背景としてそのまま描かれ、環境マップとして水面の反射にも使われる。
// キューブマップはプロセスで 1 枚だけ（同時に見える空は 1 つなので、プロシージャル空の Skybox は全部これを共有する）。
// シェーダー: Resources/Shader/Skybox/ProceduralSky.CS.hlsl

/// @brief キューブマップ 1 面の一辺（テクセル）
inline constexpr int kProceduralSkySize = 512;

/// @brief キューブマップを作る（RenderContext の初期化後に呼ぶ。初期化済みなら何もしない）
void InitProceduralSky();

/// @brief キューブマップを破棄する（GPU の完了待ちの後に呼ぶ前提）
void TermProceduralSky();

/// @brief 初期化済みか
/// @return キューブマップを作ってあれば true
bool IsProceduralSkyReady();

/// @brief 空の設定を渡す（SkyboxComponent が毎フレーム呼ぶ）
/// @param settings 設定
void SetProceduralSky(const ProceduralSkySettings &settings);

/// @brief 最後に渡された設定
/// @return 設定
const ProceduralSkySettings &GetProceduralSkySettings();

/// @brief 必要ならキューブマップを描き直す（Compute のコマンドを 3D キューの先頭に積む）
/// @details 設定・太陽の向き・雲の位置のどれかが前回と違うときだけ計算する。
///          1 フレームに何度呼んでも計算は 1 回だけ。
void UpdateProceduralSky();

/// @brief キューブマップの SRV（TextureCube。未初期化なら ptr = 0）
/// @return GPU ディスクリプタハンドル
D3D12_GPU_DESCRIPTOR_HANDLE GetProceduralSkySrv();

} // namespace RC
