#include "../Fullscreen/Fullscreen.hlsli"

// =============================================================================
// Tonemap（トーンマッピング）
//
// HDR（AppConfig.json の "hdr": true）のときだけ使う。
// 浮動小数点のシーン（1.0 を超える明るさを持てる）を、画面に出せる 0〜1 に収める。
// ポストプロセスの最後（FXAA があればその直前）に PostProcess が自動で差し込む。
//
// 演算子:
//   0: ACES（映画っぽいフィルミックな曲線。明部がなめらかに白へ寄り、少しコントラストが付く）
//   1: Reinhard（白の位置 whitePoint で 1.0 に届く、素直な圧縮）
//   2: なし（露出だけ掛けて 0〜1 に切る。8bit 描画と同じ見た目の確認用）
// =============================================================================

Texture2D<float32_t4> gTexture : register(t0);
SamplerState gSampler : register(s0);

// PostEffectConstants (b0)
cbuffer TonemapParams : register(b0) {
    float exposure;   ///< 露出の倍率（2^EV。1 で等倍）
    uint op;          ///< 演算子（0: ACES, 1: Reinhard, 2: なし）
    float whitePoint; ///< Reinhard で 1.0 になる明るさ
    float pad;
};

/// @struct PixelShaderOutPut
/// @brief ピクセル出力
struct PixelShaderOutPut {
    float4 color : SV_TARGET0; ///< 出力色（リニア。最後のコピーで sRGB に変換される）
};

/// @brief ACES の近似曲線（Krzysztof Narkowicz のフィット）
float3 ACESFilm(float3 x) {
    const float a = 2.51f;
    const float b = 0.03f;
    const float c = 2.43f;
    const float d = 0.59f;
    const float e = 0.14f;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

/// @brief 白の位置つき Reinhard
float3 ReinhardExtended(float3 x, float white) {
    const float w2 = max(white * white, 1e-4f);
    return x * (1.0f + x / w2) / (1.0f + x);
}

/// @brief HDR の色を 0〜1 に収める
PixelShaderOutPut main(VertexShaderOutput input) {
    PixelShaderOutPut output;
    const float4 src = gTexture.Sample(gSampler, input.texcoord);
    // NaN・負の値（加算合成の誤差など）が後段へ漏れないようにする
    float3 hdr = max(src.rgb, 0.0f) * exposure;
    if (any(isnan(hdr))) {
        hdr = 0.0f;
    }

    float3 ldr;
    if (op == 0) {
        ldr = ACESFilm(hdr);
    } else if (op == 1) {
        ldr = saturate(ReinhardExtended(hdr, whitePoint));
    } else {
        ldr = saturate(hdr);
    }
    output.color = float4(ldr, saturate(src.a));
    return output;
}
