#include "../Fullscreen/Fullscreen.hlsli"

// =============================================================================
// Grayscale（グレースケール）
//
// 輝度を求めて白黒にするポストエフェクト（grayscale PSO）。
// lerpFactor で元の色との混ぜ具合を変えられる。
// =============================================================================

// PostEffectConstants (b0)
cbuffer GrayscaleParams : register(b0) {
    float32_t lerpFactor; ///< 白黒の度合い（1.0 で完全に白黒、0.0 でフルカラー）
    float32_t pad1;       ///< 16 バイト境界合わせ
    float32_t pad2;       ///< 16 バイト境界合わせ
    float32_t pad3;       ///< 16 バイト境界合わせ
};

Texture2D<float4> gTexture : register(t0);
SamplerState gSampler : register(s0);

/// @struct PixelShaderOutPut
/// @brief ピクセル出力
struct PixelShaderOutPut {
    float4 color : SV_TARGET0; ///< 出力色
};

/// @brief 入力色を lerpFactor の割合で白黒に近づける
/// @param input フルスクリーン VS の出力
/// @return 出力色
PixelShaderOutPut main(VertexShaderOutput input) {
    PixelShaderOutPut output;
    output.color = gTexture.Sample(gSampler, input.texcoord);
    // BT.709 の係数で輝度を求める
    float32_t value = dot(output.color.rgb, float32_t3(0.2125f, 0.7154f, 0.0721f));
    output.color.rgb = lerp(output.color.rgb, float32_t3(value, value, value), lerpFactor);
    return output;
}
