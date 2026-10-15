#include "../Fullscreen/Fullscreen.hlsli"

// =============================================================================
// Bloom（明部のにじみ）
//
// 暗い部屋の中で光る物（紐のチューブライト、スポットライト、目的アイテム、
// インタラクトの白い輪郭）をにじませて「光っている」ことを伝える。
//
// 1 パスで完結させている。
// 本来のブルームは「明部抽出 → 縮小バッファでガウスブラー → 加算」の
// マルチパスだが、このエンジンのポストプロセスは
// 「1 エフェクト = 1 フルスクリーンパス」のスタックなので、
// 広い半径の 2 リング（合計 17 タップ）でにじみを近似している。
// 暗い背景に明るい光源が点在する画なら、この近似でも十分に見える。
// より広く柔らかいにじみが要るなら、専用の縮小RTを持つマルチパスに作り替えること
// （その際はシーンRTを R16G16B16A16_FLOAT にすると明部の余白が増えて質が上がる）。
//
// レンダーターゲットが *_UNORM_SRGB でリニア値は 1.0 までしか入らないため、
// 抽出の閾値は「1.0 超え」ではなく輝度 threshold 以上で判定する。
//
// 積む順序はアウトラインの後・カラーグレーディングの前。
// =============================================================================

Texture2D<float32_t4> gTexture : register(t0);
SamplerState gSamplerClamp : register(s2); // リニア＋クランプ（端が回り込まない）

// PostEffectConstants (b0)
cbuffer BloomParams : register(b0) {
    float threshold;  ///< この輝度から上を光として拾う（0〜1）
    float intensity;  ///< 加算する強さ（0 で無効）
    float radius;     ///< にじみの広がり（ピクセル）
    float knee;       ///< 閾値付近の柔らかさ（0 でスパッと切る）
};

/// @struct PixelShaderOutPut
/// @brief ピクセル出力
struct PixelShaderOutPut {
    float4 color : SV_TARGET0; ///< 出力色
};

// 2 リング分のサンプル方向（8 方向 × 2 半径）。円周を 45 度刻みで取る
static const float2 kRing[8] = {
    float2( 1.0f,  0.0f), float2( 0.7071f,  0.7071f),
    float2( 0.0f,  1.0f), float2(-0.7071f,  0.7071f),
    float2(-1.0f,  0.0f), float2(-0.7071f, -0.7071f),
    float2( 0.0f, -1.0f), float2( 0.7071f, -0.7071f)
};

/// @brief BT.709 の係数で輝度を求める
/// @param color リニア RGB
/// @return 輝度
float Luma(float3 color) {
    return dot(color, float3(0.2125f, 0.7154f, 0.0721f));
}

/// @brief 明部だけを取り出す（soft knee 付き）
/// @param color リニア RGB
/// @return 閾値に応じて重み付けした色（暗部は 0）
float3 BrightPass(float3 color) {
    const float luma = Luma(color);
    // knee = 0 なら threshold でスパッと、knee を上げると threshold 手前から緩やかに出る
    const float soft = max(knee, 1e-4f);
    const float weight = saturate((luma - threshold) / soft);
    return color * weight;
}

/// @brief 周囲の明部をにじませて元の色に加算する
/// @param input フルスクリーン VS の出力
/// @return 出力色
PixelShaderOutPut main(VertexShaderOutput input) {
    PixelShaderOutPut output;

    const float4 base = gTexture.Sample(gSamplerClamp, input.texcoord);

    if (intensity <= 0.0f) {
        output.color = base;
        return output;
    }

    uint width, height;
    gTexture.GetDimensions(width, height);
    const float2 texel = float2(1.0f / width, 1.0f / height);

    // 中心 + 内リング（半径 r）+ 外リング（半径 2r）
    float3 bloom = BrightPass(base.rgb) * 0.20f;
    float weightSum = 0.20f;

    const float2 inner = texel * radius;
    const float2 outer = texel * radius * 2.0f;

    [unroll]
    for (int i = 0; i < 8; ++i) {
        const float3 a = gTexture.Sample(gSamplerClamp, input.texcoord + kRing[i] * inner).rgb;
        const float3 b = gTexture.Sample(gSamplerClamp, input.texcoord + kRing[i] * outer).rgb;
        bloom += BrightPass(a) * 0.075f;
        bloom += BrightPass(b) * 0.025f;
        weightSum += 0.100f;
    }

    bloom /= weightSum;

    // 加算合成（暗部は変化せず、光の周りだけが持ち上がる）
    output.color = float4(base.rgb + bloom * intensity, base.a);
    return output;
}
