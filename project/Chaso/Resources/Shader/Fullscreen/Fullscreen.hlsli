// =============================================================================
// フルスクリーンパス共通の VS 出力
//
// Fullscreen.VS.hlsl が出力し、ポストプロセスの各 PS が入力として受け取る。
// =============================================================================

/// @struct VertexShaderOutput
/// @brief フルスクリーン三角形の頂点出力
struct VertexShaderOutput {
    float4 position : SV_POSITION; ///< クリップ空間位置
    float2 texcoord : TEXCOORD0;   ///< 画面 UV（左上 0,0 〜 右下 1,1）
};
