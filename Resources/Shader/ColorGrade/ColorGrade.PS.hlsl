#include "../Fullscreen/Fullscreen.hlsli"

// =============================================================================
// カラーグレーディング
//
// 露出・コントラスト・彩度・色温度・カラーフィルタを 1 パスで掛け、
// ゲーム全体の色調をまとめる。
//
// レンダーターゲットは *_UNORM_SRGB なので、Sample はリニア値を返し、
// 書き込み時にハードウェアが再エンコードする。つまりここでの計算はリニア空間。
// 露出（乗算）はリニアで正しく、コントラストは中間グレー 0.18、彩度は輝度を軸に掛ける。
//
// 積む順序は「ブルームの後・FXAA の前」。
// =============================================================================

Texture2D<float32_t4> gTexture : register(t0);
SamplerState gSampler : register(s0);

// b1: カラーグレーディングのパラメータ
cbuffer ColorGradeParams : register(b1) {
    float4 colorFilter;   ///< 全体に掛ける色（RGB。A は未使用）
    float exposure;       ///< 露出（EV。0 で等倍、+1 で 2 倍明るい）
    float contrast;       ///< コントラスト（1.0 で素通し）
    float saturation;     ///< 彩度（1.0 で素通し、0 で白黒）
    float temperature;    ///< 色温度（-1 寒色 〜 +1 暖色）
    float tint;           ///< 色偏り（-1 緑 〜 +1 マゼンタ）
    float lerpFactor;     ///< 適用率（0 で無効、1 で全適用）
    float2 padding;       ///< 16 バイト境界合わせ
};

/// @struct PixelShaderOutPut
/// @brief ピクセル出力
struct PixelShaderOutPut {
    float4 color : SV_TARGET0; ///< 出力色
};

// リニア空間の中間グレー。コントラストの軸に使う
static const float kMidGrey = 0.18f;

/// @brief BT.709 の係数で輝度を求める
/// @param color リニア RGB
/// @return 輝度
float Luma(float3 color) {
    return dot(color, float3(0.2125f, 0.7154f, 0.0721f));
}

/// @brief 露出・色温度・カラーフィルタ・コントラスト・彩度を順に掛ける
/// @param input フルスクリーン VS の出力
/// @return 出力色（lerpFactor で元の色と混ぜたもの）
PixelShaderOutPut main(VertexShaderOutput input) {
    PixelShaderOutPut output;

    const float4 base = gTexture.Sample(gSampler, input.texcoord);

    // 適用率 0 なら素通し（分岐は画面全体で一様なのでコストは無い）
    if (lerpFactor <= 0.0f) {
        output.color = base;
        return output;
    }

    float3 color = base.rgb;

    // 1. 露出（EV → 線形倍率）
    color *= exp2(exposure);

    // 2. 色温度・色偏り
    //    暖色は赤を上げて青を下げる、寒色は逆。tint は緑とマゼンタの綱引き。
    const float3 warmShift = float3(1.0f, 0.55f, -1.0f) * temperature * 0.12f;
    const float3 tintShift = float3(0.5f, -1.0f, 0.5f) * tint * 0.12f;
    color *= max(0.0f, 1.0f + warmShift + tintShift);

    // 3. カラーフィルタ
    color *= colorFilter.rgb;

    // 4. コントラスト（中間グレーを軸に伸縮）
    color = (color - kMidGrey) * contrast + kMidGrey;

    // 5. 彩度
    const float luma = Luma(color);
    color = lerp(float3(luma, luma, luma), color, saturation);

    color = max(color, 0.0f);

    output.color = float4(lerp(base.rgb, color, saturate(lerpFactor)), base.a);
    return output;
}
