// ============================================================================
// GPUParticle.VS.hlsl
// ----------------------------------------------------------------------------
// GPU パーティクル用頂点シェーダ（各パーティクル PSO で共通）。
// Compute Shader で初期化・更新された StructuredBuffer<Particle> を参照し、
// ビルボード回転・スケール・平行移動でワールド座標を作ってから ViewProjection 変換を行う。
// ============================================================================

#include "GPUParticle.hlsli"

StructuredBuffer<Particle> gParticles : register(t0);
ConstantBuffer<PerView> gPerView : register(b0);

/// @struct VertexShaderInput
/// @brief 頂点入力（板ポリゴン。Object3D と同じレイアウト）
struct VertexShaderInput
{
    float4 position : POSITION0; ///< ローカル座標
    float2 texcoord : TEXCOORD0; ///< テクスチャ座標
    float3 normal : NORMAL0;     ///< 法線（ここでは使わない）
};

/// @brief インスタンス番号のパーティクルとして板ポリゴンを配置する
/// @param input      頂点入力
/// @param instanceId インスタンス番号（gParticles の添字）
/// @return クリップ空間位置・UV・色・寿命の進行度と固有乱数
VertexShaderOutput main(VertexShaderInput input, uint instanceId : SV_InstanceID)
{
    VertexShaderOutput output;

    Particle particle = gParticles[instanceId];

    // 1. スケール適用（ローカル座標）
    float3 localPos = input.position.xyz * particle.scale;

    // 2. ビルボード回転適用（3x3 行列として扱う）
    float3 rotatedPos = mul(localPos, (float3x3)gPerView.billboardMatrix);

    // 3. 平行移動適用（ワールド座標へ）
    float3 worldPos = rotatedPos + particle.translate;

    // 4. ViewProjection 変換
    output.position = mul(float4(worldPos, 1.0f), gPerView.viewProjection);
    output.texcoord = input.texcoord;
    output.color = particle.color;

    // 5. PS 側で経過時間を使えるように寿命の進行度と固有乱数を渡す
    //    （炎の先細り・ちらつきに使う。lifeTime が 0 のときは 0 除算を避ける）
    float lifeRatio = (particle.lifeTime > 0.0f)
        ? saturate(particle.currentTime / particle.lifeTime)
        : 0.0f;
    output.particleParams = float2(lifeRatio, particle.pad0);

    return output;
}
