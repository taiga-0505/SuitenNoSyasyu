#include "../Fullscreen/Fullscreen.hlsli"

// =============================================================================
// Sepia（セピア調）
//
// 輝度を求めてセピア色を掛けるポストエフェクト（sepia PSO）。
// =============================================================================

Texture2D<float4> gTexture : register(t0);
SamplerState gSampler : register(s0);

/// @struct PixelShaderOutPut
/// @brief ピクセル出力
struct PixelShaderOutPut {
    float4 color : SV_TARGET0; ///< 出力色
};

/// @brief 入力色をセピア調に変換する
/// @param input フルスクリーン VS の出力
/// @return 出力色
PixelShaderOutPut main(VertexShaderOutput input) {
    PixelShaderOutPut output;
    output.color = gTexture.Sample(gSampler, input.texcoord);
    // BT.709 の係数で輝度を求め、セピア色を掛ける
    float32_t value = dot(output.color.rgb, float32_t3(0.2125f, 0.7154f, 0.0721f));
    output.color.rgb = value * float32_t3(1.0f, 74.0f / 107.0f, 43.0f / 107.0f);
    return output;
}
