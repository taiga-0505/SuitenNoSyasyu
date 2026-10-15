#include "../Fullscreen/Fullscreen.hlsli"

// =============================================================================
// Vignette（周辺減光）
//
// 画面の端ほど暗くするポストエフェクト（vignette PSO）。パラメータは持たない。
// =============================================================================

Texture2D<float4> gTexture : register(t0);
SamplerState gSampler : register(s0);

/// @struct PixelShaderOutPut
/// @brief ピクセル出力
struct PixelShaderOutPut {
    float4 color : SV_TARGET0; ///< 出力色
};

/// @brief 画面端ほど暗くなる係数を入力色に掛ける
/// @param input フルスクリーン VS の出力
/// @return 出力色
PixelShaderOutPut main(VertexShaderOutput input) {
    PixelShaderOutPut output;
    output.color = gTexture.Sample(gSampler, input.texcoord);

    // 周囲を 0 に、中心になるほど明るくなるように計算で調整
    float32_t2 correct = input.texcoord * (1.0f - input.texcoord.yx);
    // correct だけで計算すると中心の最大値が 0.0625 で暗すぎるので、16 倍して中心を 1 にする
    float vignette = correct.x * correct.y * 16.0f;
    // 0.8 乗で減光のカーブを緩める
    vignette = saturate(pow(vignette, 0.8f));
    // 係数として乗算
    output.color.rgb *= vignette;

    return output;
}
