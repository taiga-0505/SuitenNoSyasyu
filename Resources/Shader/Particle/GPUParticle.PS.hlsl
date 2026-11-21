// ============================================================================
// GPUParticle.PS.hlsl
// ----------------------------------------------------------------------------
// GPU パーティクル用ピクセルシェーダ（gpu_particle PSO）。
// テクスチャサンプリングとカラー乗算を行う。
// ============================================================================

#include "GPUParticle.hlsli"

Texture2D<float4> gTexture : register(t0);
SamplerState gSampler : register(s0);

/// @struct PixelShaderOutput
/// @brief ピクセル出力
struct PixelShaderOutput
{
    float4 color : SV_TARGET0; ///< 出力色
};

/// @brief テクスチャ色にパーティクル色を掛けて出力する
/// @param input GPUParticle.VS の出力
/// @return 出力色（α が 0 のピクセルは discard）
PixelShaderOutput main(VertexShaderOutput input)
{
    PixelShaderOutput output;

    float4 textureColor = gTexture.Sample(gSampler, input.texcoord);
    output.color = textureColor * input.color;

    // 完全に透明なピクセルは書かない
    if (output.color.a == 0.0f)
    {
        discard;
    }

    return output;
}
