#include "Fullscreen.hlsli"

// =============================================================================
// フルスクリーン VS（ポストプロセス共通）
//
// 頂点バッファを使わず、SV_VertexID から画面を覆う大きな三角形 1 枚を作る。
// 画面外にはみ出た部分はクリップされるので、画面内の UV は 0〜1 になる。
// ポストプロセスの各 PSO（copyimage / bloom / fxaa など）で共通に使う。
// =============================================================================

static const uint32_t kNumVerTex = 3; ///< 頂点数（三角形 1 枚）
// クリップ空間の頂点位置
static const float4 kPositions[kNumVerTex] = {
    {-1.0f,1.0f,0.0f,1.0f}, // 左上
    {3.0f,1.0f,0.0f,1.0f}, // 右上
    {-1.0f,-3.0f,0.0f,1.0f}, // 左下
};

// 各頂点の UV（画面内で 0〜1 になるよう 2 まで伸ばしている）
static const float2 kTexCoords[kNumVerTex] = {
    {0.0f,0.0f}, // 左上
    {2.0f,0.0f}, // 右上
    {0.0f,2.0f}, // 左下
};

/// @brief フルスクリーン三角形の頂点を出力する
/// @param vertexId 頂点番号（0〜2）
/// @return クリップ空間位置と画面 UV
VertexShaderOutput main(uint32_t vertexId : SV_VertexID) {
    VertexShaderOutput output;
    
    output.position = kPositions[vertexId];
    output.texcoord = kTexCoords[vertexId];
    return output;
}
