#include "Object3d.hlsli"

// =============================================================================
// Object3D インスタンシング描画の頂点シェーダー
//
// 同じメッシュをまとめて 1 ドローで描く用。行列と色はインスタンスごとに
// StructuredBuffer（t1）から SV_InstanceID で引く。PS は単体描画版と共用する。
// =============================================================================

//struct TransformationMatrix {
//    float4x4 WVP;
//    float4x4 World;
//    float4x4 worldInverseTranspose;
//};

//ConstantBuffer<TransformationMatrix> gTransformationMatrix : register(b0);

/// @brief インスタンス 1 個ぶんのデータ（C++ 側 ModelResource::InstanceGPU と同じ並び）
struct InstanceData
{
    float4x4 WVP;                   ///< ワールド × ビュー × プロジェクション
    float4x4 World;                 ///< ワールド行列
    float4x4 WorldInverseTranspose; ///< ワールド行列の逆転置（法線の変換用）
    float4 color;                   ///< インスタンスごとの乗算色
};

StructuredBuffer<InstanceData> gInstances : register(t1);

struct VertexShaderInput {
    float4 position : POSITION0; ///< モデルローカルの位置
    float2 texcoord : TEXCOORD0; ///< UV
    float3 normal : NORMAL0;     ///< モデルローカルの法線
};

/// @brief インスタンスごとの行列で頂点を変換し、ライティング・影に必要な値を PS へ渡す
/// @param input      頂点入力
/// @param instanceId インスタンス番号（gInstances の添字）
/// @return クリップ空間位置・ワールド座標・ライト空間座標などの出力
VertexShaderOutput main(VertexShaderInput input, uint instanceId : SV_InstanceID)
{
    InstanceData inst = gInstances[instanceId];

    VertexShaderOutput output;
    output.position = mul(input.position, inst.WVP);
    output.texcoord = input.texcoord;
    output.normal = normalize(mul(input.normal, (float3x3) inst.WorldInverseTranspose));
    output.worldPosition = mul(input.position, inst.World).xyz;
    output.instColor = inst.color;

    // ライトビュープロジェクションでライト空間の座標を計算
    output.lightSpacePos = mul(float4(output.worldPosition, 1.0f), gShadowParams.lightViewProjection);

    return output;
}
