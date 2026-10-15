#include "../Fullscreen/Fullscreen.hlsli"

// =============================================================================
// BoxFilter（ボックスフィルタ）
//
// 周囲 (2k+1)×(2k+1) ピクセルを同じ重みで平均して平滑化するポストエフェクト
// （boxfilter PSO）。
// =============================================================================

Texture2D<float4> gTexture : register(t0);
SamplerState gSampler : register(s0);

// PostEffectParams（ルート定数 b0）
cbuffer PostEffectParams : register(b0) {
    uint gKernelRadius; ///< カーネル半径 k（1 = 3x3, 2 = 5x5, ...）
    uint pad0;          ///< 16 バイト境界合わせ
    uint pad1;          ///< 16 バイト境界合わせ
    uint pad2;          ///< 16 バイト境界合わせ
};

/// @struct PixelShaderOutPut
/// @brief ピクセル出力
struct PixelShaderOutPut {
    float4 color : SV_TARGET0; ///< 出力色
};

/// @brief 周囲のピクセルを均等な重みで平均する
/// @param input フルスクリーン VS の出力
/// @return 出力色（α は 1 固定）
PixelShaderOutPut main(VertexShaderOutput input) {
    PixelShaderOutPut output;

    // 1. 1 ピクセル分の UV（uvStepSize）を算出
    uint32_t width, height;
    gTexture.GetDimensions(width, height);
    float32_t2 uvStepSize = float32_t2(rcp((float32_t)width), rcp((float32_t)height));

    // 2. カーネル半径
    int32_t k = (int32_t)gKernelRadius;

    // 3. カーネル範囲でループを回す（k = 1 なら -1〜+1、k = 2 なら -2〜+2）
    output.color.rgb = float32_t3(0.0f, 0.0f, 0.0f);
    output.color.a = 1.0f;

    float32_t kernelSize = (2.0f * (float32_t)k + 1.0f);
    float32_t weight = 1.0f / (kernelSize * kernelSize);

    for (int32_t x = -k; x <= k; ++x) {
        for (int32_t y = -k; y <= k; ++y) {
            // 現在の texcoord を算出
            float32_t2 texcoord = input.texcoord + float32_t2((float32_t)x, (float32_t)y) * uvStepSize;
            // 色に 1/(2k+1)^2 を掛けて足す
            float32_t3 fetchColor = gTexture.Sample(gSampler, texcoord).rgb;
            output.color.rgb += fetchColor * weight;
        }
    }

    return output;
}
