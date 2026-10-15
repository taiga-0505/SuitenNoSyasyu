#include "../Fullscreen/Fullscreen.hlsli"

// =============================================================================
// Dissolve（ディゾルブ）
//
// ノイズマスクの値が閾値以下の部分を背景色で塗りつぶし、境目を発光させる
// ポストエフェクト（dissolve PSO）。シーン遷移の幕などに使う。
// =============================================================================

// t0: シーンテクスチャ
Texture2D<float4> gTexture : register(t0);
SamplerState gSampler : register(s0);

// t1: ノイズテクスチャ（マスク）
Texture2D<float32_t> gMaskTexture : register(t1);

// b1: Dissolve パラメータ
cbuffer DissolveParams : register(b1) {
    float4 edgeColor;    ///< 境目の発光色（RGB + α）
    float4 baseColor;    ///< 抜けた部分の背景色（RGB + α）
    float threshold;     ///< 閾値（0.0〜1.0）
    float edgeRange;     ///< 境目として光らせる幅（既定 0.03）
    float2 _padding;     ///< 16 バイト境界合わせ
};

/// @struct PixelShaderOutPut
/// @brief ピクセル出力
struct PixelShaderOutPut {
    float4 color : SV_TARGET0; ///< 出力色
};

/// @brief マスク値と閾値を比べ、背景色・発光する境目・元の色を出し分ける
/// @param input フルスクリーン VS の出力
/// @return 出力色
PixelShaderOutPut main(VertexShaderOutput input) {
    PixelShaderOutPut output;

    // マスクテクスチャからノイズ値をサンプリング
    float32_t mask = gMaskTexture.Sample(gSampler, input.texcoord);

    // マスク値が閾値以下の場合はベースカラーを出力
    if (mask <= threshold) {
        output.color = baseColor;
        return output;
    }

    // シーンテクスチャをサンプリング
    output.color = gTexture.Sample(gSampler, input.texcoord);

    // 境目らしさを算出: threshold 〜 threshold + edgeRange の範囲を 0〜1 に補間
    // 1 から引くことで、閾値に近いほど境目らしさが高くなる
    float32_t edge = 1.0f - smoothstep(threshold, threshold + edgeRange, mask);

    // 境目らしいほど指定した色を加算
    output.color.rgb += edge * edgeColor.rgb;

    return output;
}
