#include "../Object3d/Object3d.hlsli"

// =============================================================================
// シャドウマップ用頂点シェーダ（スキニング）
//
// shadow_skin PSO で使う。ボーン行列でスキニングしてからワールド変換し、
// ライト視点のクリップ空間へ変換する。
// =============================================================================

/// @struct TransformationMatrix
/// @brief オブジェクトごとの変換行列（Object3D の b0 と同じレイアウト）
struct TransformationMatrix
{
    float4x4 WVP;                   ///< ワールド・ビュー・プロジェクション行列（ここでは使わない）
    float4x4 World;                 ///< ワールド行列
    float4x4 worldInverseTranspose; ///< ワールド行列の逆転置（ここでは使わない）
};
ConstantBuffer<TransformationMatrix> gTransformationMatrix : register(b0);

StructuredBuffer<float4x4> gSkinMatrices : register(t1); // ボーンごとのスキニング行列

/// @struct VertexShaderInput
/// @brief スキニング付きの頂点入力
struct VertexShaderInput
{
    float4 position    : POSITION0;     ///< ローカル座標
    float2 texcoord    : TEXCOORD0;     ///< テクスチャ座標
    float3 normal      : NORMAL0;       ///< 法線
    int4   boneIndex   : BLENDINDICES0; ///< 影響ボーンの番号（最大 4 本）
    float4 boneWeight  : BLENDWEIGHT0;  ///< 各ボーンの重み
};

/// @struct VSOutput
/// @brief 頂点出力
struct VSOutput {
    float4 position : SV_POSITION; ///< ライト視点のクリップ空間位置
};

/// @brief スキニングした頂点をライト視点のクリップ空間へ変換する
/// @param input 頂点入力
/// @return ライト視点のクリップ空間位置
VSOutput main(VertexShaderInput input)
{
    VSOutput output;

    // 4 本のボーン行列を重みで混ぜる
    float4x4 skinMat = (float4x4)0;
    skinMat += input.boneWeight.x * gSkinMatrices[input.boneIndex.x];
    skinMat += input.boneWeight.y * gSkinMatrices[input.boneIndex.y];
    skinMat += input.boneWeight.z * gSkinMatrices[input.boneIndex.z];
    skinMat += input.boneWeight.w * gSkinMatrices[input.boneIndex.w];

    float4 skinnedPos = mul(input.position, skinMat);
    float4 worldPos = mul(skinnedPos, gTransformationMatrix.World);
    output.position = mul(worldPos, gShadowParams.lightViewProjection);

    return output;
}
