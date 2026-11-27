// =============================================================================
// Sand.VS.hlsl - 砂の地形の頂点シェーダー
// 地形の形はメッシュ（CreateSandTerrain）がそのまま持つので、ワールドへ変換して渡すだけ。
// 跡（掘った溝・盛り上がり）は細かすぎて頂点では表せないので、PS が法線と色で描く。
// =============================================================================

#include "Sand.hlsli"

struct TransformationMatrix
{
    float4x4 WVP;                   ///< World-View-Projection 行列
    float4x4 World;                 ///< World 行列
    float4x4 worldInverseTranspose; ///< World の逆転置行列
};
ConstantBuffer<TransformationMatrix> gTransformationMatrix : register(b0);

struct VertexShaderInput
{
    float4 position : POSITION0; ///< 頂点位置（ローカル）
    float2 texcoord : TEXCOORD0; ///< テクスチャ座標（使わない）
    float3 normal   : NORMAL0;   ///< 地形の法線（ローカル）
};

VertexShaderOutput main(VertexShaderInput input)
{
    VertexShaderOutput o;
    o.position = mul(input.position, gTransformationMatrix.WVP);
    o.worldPosition = mul(input.position, gTransformationMatrix.World).xyz;
    // 地形は拡縮しない前提なので World の回転部分で向きを変える
    o.normal = normalize(mul(input.normal, (float3x3)gTransformationMatrix.World));
    return o;
}
