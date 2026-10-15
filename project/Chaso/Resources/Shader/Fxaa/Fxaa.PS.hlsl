#include "../Fullscreen/Fullscreen.hlsli"

// =============================================================================
// FXAA（Fast Approximate Anti-Aliasing）
//
// 輝度のエッジを検出し、エッジに沿った方向へ 4 タップ分を混ぜて段差をならす。
// MSAA を使っていない構成でも、アウトライン（DepthBasedOutline / MaskOutline）や
// タイルの縁がカメラを振ったときにチラつくのを抑えられる。
//
// パラメータは無し（b0 は使わない）。積むだけで効く。
// 積む順序は「一番最後」。ブルームや色調整の後に掛けないと、
// せっかくならした段差がまた作られる。
//
// サンプラは s2（リニア＋クランプ）を使う。s0 はラップなので画面端が回り込む。
// =============================================================================

Texture2D<float32_t4> gTexture : register(t0);
SamplerState gSamplerClamp : register(s2);

/// @struct PixelShaderOutPut
/// @brief ピクセル出力
struct PixelShaderOutPut {
    float4 color : SV_TARGET0; ///< 出力色
};

// エッジと判定する輝度差（相対）。小さいほど積極的にならす
static const float kEdgeThreshold = 0.125f;
// これ未満の輝度差は無視する（暗部のノイズをならさないため）
static const float kEdgeThresholdMin = 0.0312f;
// 探索方向の補正項（細いエッジで方向が暴れるのを抑える）
static const float kDirReduce = 1.0f / 8.0f;
static const float kDirReduceMin = 1.0f / 128.0f;
// 混ぜに行く距離の上限（テクセル単位）
static const float kSpanMax = 8.0f;

/// @brief エッジ判定用の輝度を求める（BT.601 の係数）
/// @param color RGB
/// @return 輝度
float Luma(float3 color) {
    return dot(color, float3(0.299f, 0.587f, 0.114f));
}

/// @brief 輝度エッジに沿って周囲の色を混ぜ、段差をならす
/// @param input フルスクリーン VS の出力
/// @return 出力色（α は 1 固定）
PixelShaderOutPut main(VertexShaderOutput input) {
    PixelShaderOutPut output;

    uint width, height;
    gTexture.GetDimensions(width, height);
    const float2 texel = float2(1.0f / width, 1.0f / height);
    const float2 uv = input.texcoord;

    const float3 rgbM = gTexture.Sample(gSamplerClamp, uv).rgb;

    // 斜め 4 近傍の輝度でエッジを判定する
    const float lM  = Luma(rgbM);
    const float lNW = Luma(gTexture.Sample(gSamplerClamp, uv + float2(-1.0f, -1.0f) * texel).rgb);
    const float lNE = Luma(gTexture.Sample(gSamplerClamp, uv + float2( 1.0f, -1.0f) * texel).rgb);
    const float lSW = Luma(gTexture.Sample(gSamplerClamp, uv + float2(-1.0f,  1.0f) * texel).rgb);
    const float lSE = Luma(gTexture.Sample(gSamplerClamp, uv + float2( 1.0f,  1.0f) * texel).rgb);

    const float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));
    const float lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));
    const float range = lMax - lMin;

    // 平坦な場所は何もしない
    if (range < max(kEdgeThresholdMin, lMax * kEdgeThreshold)) {
        output.color = float4(rgbM, 1.0f);
        return output;
    }

    // エッジに沿った方向（輝度勾配の直交方向）
    float2 dir;
    dir.x = -((lNW + lNE) - (lSW + lSE));
    dir.y =  ((lNW + lSW) - (lNE + lSE));

    const float dirReduce = max(min(abs(dir.x), abs(dir.y)) * kDirReduce, kDirReduceMin);
    const float rcpDirMin = 1.0f / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    dir = clamp(dir * rcpDirMin, -kSpanMax, kSpanMax) * texel;

    // エッジ方向に内側 2 タップ（rgbA）と、さらに外側まで含む 4 タップ（rgbB）
    const float3 rgbA = 0.5f * (
        gTexture.Sample(gSamplerClamp, uv + dir * (1.0f / 3.0f - 0.5f)).rgb +
        gTexture.Sample(gSamplerClamp, uv + dir * (2.0f / 3.0f - 0.5f)).rgb);

    const float3 rgbB = rgbA * 0.5f + 0.25f * (
        gTexture.Sample(gSamplerClamp, uv + dir * -0.5f).rgb +
        gTexture.Sample(gSamplerClamp, uv + dir *  0.5f).rgb);

    // 外側まで混ぜた結果が元の輝度範囲を外れたら、混ぜ過ぎなので内側だけを採用する
    const float lB = Luma(rgbB);
    output.color = float4((lB < lMin || lB > lMax) ? rgbA : rgbB, 1.0f);
    return output;
}
