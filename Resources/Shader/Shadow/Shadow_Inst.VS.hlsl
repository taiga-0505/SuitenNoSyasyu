#include "../Object3d/Object3d.hlsli"

// =============================================================================
// シャドウマップ用頂点シェーダ（インスタンシング）
//
// shadow_inst PSO で使う。インスタンスごとのワールド行列を StructuredBuffer から読み、
// ライト視点のクリップ空間へ変換する。
// =============================================================================

/// @struct InstanceData
/// @brief インスタンス 1 つ分のデータ（Object3D_Inst と同じレイアウト）
struct InstanceData
{
    float4x4 WVP;                   ///< ワールド・ビュー・プロジェクション行列（ここでは使わない）
    float4x4 World;                 ///< ワールド行列
    float4x4 WorldInverseTranspose; ///< ワールド行列の逆転置（ここでは使わない）
    float4 color;                   ///< インスタンス色（ここでは使わない）
};

StructuredBuffer<InstanceData> gInstances : register(t1);

/// @struct VertexShaderInput
/// @brief 頂点入力（Object3D と同じレイアウト）
struct VertexShaderInput {
    float4 position : POSITION0; ///< ローカル座標
    float2 texcoord : TEXCOORD0; ///< テクスチャ座標
    float3 normal : NORMAL0;     ///< 法線
};

/// @struct VSOutput
/// @brief 頂点出力
struct VSOutput {
    float4 position : SV_POSITION; ///< ライト視点のクリップ空間位置
};

/// @brief インスタンスの頂点をライト視点のクリップ空間へ変換する
/// @param input      頂点入力
/// @param instanceId インスタンス番号
/// @return ライト視点のクリップ空間位置
VSOutput main(VertexShaderInput input, uint instanceId : SV_InstanceID) {
    VSOutput output;
    InstanceData inst = gInstances[instanceId];
    float4 worldPos = mul(input.position, inst.World);
    output.position = mul(worldPos, gShadowParams.lightViewProjection);
    
    return output;
}
