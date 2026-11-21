// =============================================================================
// Random（ランダムノイズ）
//
// UV と経過時間から作った乱数を画面に乗算し、ザラついたノイズを掛ける
// ポストエフェクト（random PSO）。
// =============================================================================

/// @struct PixelShaderInput
/// @brief フルスクリーン VS からの入力（Fullscreen.hlsli の VertexShaderOutput と同じ並び）
struct PixelShaderInput {
    float4 position : SV_POSITION; ///< クリップ空間位置
    float2 texcoord : TEXCOORD0;   ///< 画面 UV
};

/// @struct PixelShaderOutPut
/// @brief ピクセル出力
struct PixelShaderOutPut {
    float4 color : SV_TARGET0; ///< 出力色
};

Texture2D<float32_t4> gTexture : register(t0);
SamplerState gSampler : register(s0);

// b1: ランダムノイズのパラメータ
cbuffer RandomNoiseParams : register(b1) {
    float4 noiseColor; ///< ノイズに掛ける色（RGBA）
    float time;        ///< 経過時間（乱数のシードを変えるため）
    float intensity;   ///< ノイズの強さ（0 で元の色、1 でノイズのみ）
    float2 _padding;   ///< 16 バイト境界合わせ
};

/// @brief 2D の値から 0〜1 の擬似乱数を作る
/// @param value 乱数の元になる値
/// @return 0〜1 の擬似乱数
float32_t rand2dTo1d(float2 value) {
    return frac(sin(dot(value, float2(12.9898, 78.233))) * 43758.5453);
}

/// @brief 元の色に乱数ノイズを乗算し、intensity で混ぜる
/// @param input フルスクリーン VS の出力
/// @return 出力色
PixelShaderOutPut main(PixelShaderInput input) {
    PixelShaderOutPut output;

    // 経過時間（time）を掛けてシード値を変化させる
    float32_t random = rand2dTo1d(input.texcoord * time);
    
    // 元のシーンの色をサンプリング
    float4 baseColor = gTexture.Sample(gSampler, input.texcoord);
    
    // 乱数の値を入力画像に乗算
    float4 noisyColor = baseColor * float4(random, random, random, 1.0f) * noiseColor;
    
    // 強度（intensity）に応じて元の色とノイズのかかった色をブレンド
    output.color = lerp(baseColor, noisyColor, intensity);
    
    return output;
}
