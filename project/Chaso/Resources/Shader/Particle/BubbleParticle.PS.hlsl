// ============================================================================
// BubbleParticle.PS.hlsl
// ----------------------------------------------------------------------------
// 泡パーティクル用ピクセルシェーダ（gpu_particle_bubble PSO）。
// UV から中心距離を計算し、縁を白く不透明に、中心を透明に描画する。
// ============================================================================

#include "GPUParticle.hlsli"

Texture2D<float4> gTexture : register(t0); // 現状はサンプリングしない（バインドだけ他のパーティクル PSO と揃えている）
SamplerState gSampler : register(s0);

/// @struct PixelShaderOutput
/// @brief ピクセル出力
struct PixelShaderOutput
{
    float4 color : SV_TARGET0; ///< 出力色
};

/// @brief 板ポリゴンを縁が白く中心が透明な泡として描く
/// @param input GPUParticle.VS の出力
/// @return 出力色（円の外側とほぼ透明なピクセルは discard）
PixelShaderOutput main(VertexShaderOutput input)
{
    PixelShaderOutput output;

    // UV 座標から中心（0.5, 0.5）への距離を計算
    float2 center = float2(0.5f, 0.5f);
    float dist = distance(input.texcoord, center);

    // 半径 0.5 より外側は描画しない（完全な円にする）
    if (dist > 0.5f)
    {
        discard;
    }

    // 泡の縁の計算
    // dist は 0.0（中心）〜 0.5（外周）に分布する。
    // dist を 0.0〜1.0 に正規化
    float normalizedDist = dist * 2.0f;

    // 縁の太さとシャープさの調整
    // 1.0 に近いほど縁。pow でカーブをつけて急激に不透明にする
    float edgeFactor = pow(normalizedDist, 4.0f);
    
    // ハイライト（左上からの光を想定して少しずらした位置を明るくする）
    float2 highlightOffset = float2(0.35f, 0.35f);
    float highlightDist = distance(input.texcoord, highlightOffset);
    float highlightFactor = saturate(1.0f - highlightDist * 5.0f);
    highlightFactor = pow(highlightFactor, 2.0f);

    // 最終的なアルファ値
    // 縁の部分 + ハイライト部分 + 基本色（input.color）のアルファ
    float alpha = saturate(edgeFactor + highlightFactor);
    alpha *= input.color.a;

    // 泡の基本色は白ベースだが、設定色（input.color）も混ぜて色付き泡も可能にする
    // 縁とハイライトは白に近づける
    float3 baseColor = lerp(input.color.rgb, float3(1.0f, 1.0f, 1.0f), edgeFactor * 0.8f);
    baseColor += float3(1.0f, 1.0f, 1.0f) * highlightFactor; // ハイライトの加算

    output.color = float4(saturate(baseColor), alpha);

    if (output.color.a < 0.01f)
    {
        discard;
    }

    return output;
}
