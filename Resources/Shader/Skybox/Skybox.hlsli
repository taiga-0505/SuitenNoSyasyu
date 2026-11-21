// =============================================================================
// スカイボックス用の VS 出力定義（Skybox.VS / Skybox.PS で共有）
// =============================================================================

/// @struct VertexShaderOutput
/// @brief スカイボックスの頂点出力
struct VertexShaderOutput {
    float4 position : SV_POSITION; ///< クリップ空間位置（深度は最遠に固定済み）
    float3 texcoord : TEXCOORD0;   ///< キューブマップをサンプリングする方向ベクトル
};
