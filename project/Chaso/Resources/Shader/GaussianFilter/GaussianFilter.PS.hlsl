#include "../Fullscreen/Fullscreen.hlsli"

// =============================================================================
// GaussianFilter（ガウシアンフィルタ）
//
// 周囲 (2k+1)×(2k+1) ピクセルをガウス関数の重みで平均してぼかすポストエフェクト
// （gaussianfilter PSO）。縦横を分けない 1 パスの 2D カーネル。
// =============================================================================

Texture2D<float4> gTexture : register(t0);
SamplerState gSampler : register(s0);

// PostEffectParams（ルート定数 b0）
cbuffer PostEffectParams : register(b0) {
    uint gKernelRadius; ///< カーネル半径 k（1 = 3x3, 2 = 5x5, ...）
    float gSigma;       ///< 標準偏差（ピクセル。0 以下は 0.0001 として扱う）
    uint pad0;          ///< 16 バイト境界合わせ
    uint pad1;          ///< 16 バイト境界合わせ
};

/// @struct PixelShaderOutPut
/// @brief ピクセル出力
struct PixelShaderOutPut {
    float4 color : SV_TARGET0; ///< 出力色
};

/// @brief 周囲のピクセルをガウス関数の重みで平均する
/// @param input フルスクリーン VS の出力
/// @return 出力色（α は 1 固定）
PixelShaderOutPut main(VertexShaderOutput input) {
    PixelShaderOutPut output;

    // 1. 1 ピクセル分の UV（uvStepSize）を算出
    uint32_t width, height;
    gTexture.GetDimensions(width, height);
    float32_t2 uvStepSize = float32_t2(rcp((float32_t)width), rcp((float32_t)height));

    // 2. カーネル半径とシグマ
    int32_t k = (int32_t)gKernelRadius;
    float32_t sigma = gSigma;

    if (sigma <= 0.0f) {
        sigma = 0.0001f;
    }

    // 3. カーネル範囲でループを回し、重みとその合計を溜める
    float32_t3 color = float32_t3(0.0f, 0.0f, 0.0f);
    float32_t weightSum = 0.0f;

    for (int32_t x = -k; x <= k; ++x) {
        for (int32_t y = -k; y <= k; ++y) {
            // 現在の texcoord を算出
            float32_t2 texcoord = input.texcoord + float32_t2((float32_t)x, (float32_t)y) * uvStepSize;
            float32_t3 fetchColor = gTexture.Sample(gSampler, texcoord).rgb;
            
            // ガウス関数の重みを計算
            float32_t weight = exp(-((float32_t)(x * x + y * y)) / (2.0f * sigma * sigma));
            
            color += fetchColor * weight;
            weightSum += weight;
        }
    }

    // 4. 正規化して出力
    output.color.rgb = color / weightSum;
    output.color.a = 1.0f;

    return output;
}
