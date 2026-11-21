#include "Object3d.hlsli"

// =============================================================================
// Object3D スキニング描画の頂点シェーダー
//
// スケルトンアニメーション付きモデル用。頂点ごとに最大 4 本のボーンの行列を
// ウェイトで混ぜて（リニアブレンドスキニング）から、通常どおりワールド・クリップ空間へ変換する。
// PS は単体描画版と共用する。
// =============================================================================

/// @brief 座標変換行列（b0）
struct TransformationMatrix
{
    float4x4 WVP;                   ///< ワールド × ビュー × プロジェクション
    float4x4 World;                 ///< ワールド行列
    float4x4 worldInverseTranspose; ///< ワールド行列の逆転置（法線の変換用）
};

ConstantBuffer<TransformationMatrix> gTransformationMatrix : register(b0);

// 行列パレット（SkinCluster）: T_i = InverseBindPose_i * SkeletonSpaceMatrix_i
StructuredBuffer<float4x4> gSkinMatrices : register(t1);

struct VertexShaderInput
{
    float4 position    : POSITION0;     ///< モデルローカルの位置
    float2 texcoord    : TEXCOORD0;     ///< UV
    float3 normal      : NORMAL0;       ///< モデルローカルの法線
    int4   boneIndex   : BLENDINDICES0; ///< 影響するボーンの番号（gSkinMatrices の添字。最大 4 本）
    float4 boneWeight  : BLENDWEIGHT0;  ///< 各ボーンのウェイト
};

/// @brief スキニングを適用してから頂点を変換し、ライティング・影に必要な値を PS へ渡す
/// @param input 頂点入力（ボーン番号・ウェイト付き）
/// @return クリップ空間位置・ワールド座標・ライト空間座標などの出力
VertexShaderOutput main(VertexShaderInput input)
{
    VertexShaderOutput output;

    // リニアブレンドスキニング: skinMat = Σ w_i * T_i
    float4x4 skinMat = (float4x4)0;
    skinMat += input.boneWeight.x * gSkinMatrices[input.boneIndex.x];
    skinMat += input.boneWeight.y * gSkinMatrices[input.boneIndex.y];
    skinMat += input.boneWeight.z * gSkinMatrices[input.boneIndex.z];
    skinMat += input.boneWeight.w * gSkinMatrices[input.boneIndex.w];

    // スキニング適用（モデルローカル空間 → スケルトン空間）
    float4 skinnedPos = mul(input.position, skinMat);
    float3 skinnedNormal = normalize(mul(input.normal, (float3x3)skinMat));

    // ワールド → クリップ空間
    output.position = mul(skinnedPos, gTransformationMatrix.WVP);
    output.texcoord = input.texcoord;
    output.normal = normalize(mul(skinnedNormal, (float3x3)gTransformationMatrix.worldInverseTranspose));
    output.worldPosition = mul(skinnedPos, gTransformationMatrix.World).xyz;
    output.instColor = float4(1, 1, 1, 1);

    // ライトビュープロジェクションでライト空間の座標を計算
    output.lightSpacePos = mul(float4(output.worldPosition, 1.0f), gShadowParams.lightViewProjection);

    return output;
}
