#include "../Fullscreen/Fullscreen.hlsli"

// =============================================================================
// CopyImage（画像の転送）
//
// 入力テクスチャをそのまま出力先へ写す。ポストプロセスの転送用（copyimage PSO）。
// =============================================================================

Texture2D<float4> gTexture : register(t0);
SamplerState gSampler : register(s0);

/// @struct PixelShaderOutPut
/// @brief ピクセル出力
struct PixelShaderOutPut {
    float4 color : SV_TARGET0; ///< 出力色
};

/// @brief 入力テクスチャの色をそのまま出力する
/// @param input フルスクリーン VS の出力
/// @return 出力色
PixelShaderOutPut main(VertexShaderOutput input) {
    PixelShaderOutPut output;
    output.color = gTexture.Sample(gSampler, input.texcoord);
    return output;
}