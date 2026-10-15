#include "Skybox.hlsli"

// =============================================================================
// スカイボックス用頂点シェーダ
//
// skybox PSO（深度テスト ON・書き込み OFF・カリング無し）で使う。
// 深度を最遠に固定し、頂点位置をキューブマップの方向ベクトルとして渡す。
// =============================================================================

/// @struct TransformationMatrix
/// @brief オブジェクトごとの変換行列（Object3D の b0 と同じレイアウト）
struct TransformationMatrix
{
    float4x4 WVP;                   ///< ワールド・ビュー・プロジェクション行列
    float4x4 World;                 ///< ワールド行列（ここでは使わない）
    float4x4 worldInverseTranspose; ///< ワールド行列の逆転置（ここでは使わない）
};

ConstantBuffer<TransformationMatrix> gTransformationMatrix : register(b0);

/// @struct VertexShaderInput
/// @brief 頂点入力（Object3D と同じレイアウト）
struct VertexShaderInput
{
    float4 position : POSITION0; ///< ローカル座標（キューブの頂点。方向ベクトルとしても使う）
    float2 texcoord : TEXCOORD0; ///< テクスチャ座標（ここでは使わない）
    float3 normal : NORMAL0;     ///< 法線（ここでは使わない）
};

/// @brief 深度を最遠に固定した位置と、キューブマップ用の方向ベクトルを出力する
/// @param input 頂点入力
/// @return クリップ空間位置と方向ベクトル
VertexShaderOutput main(VertexShaderInput input)
{
    VertexShaderOutput output;
    // WVP 変換後、z を w にコピーして深度を最遠（z/w = 1）にする
    output.position = mul(input.position, gTransformationMatrix.WVP).xyww;
    // 頂点位置をそのまま方向ベクトルとして出力（キューブマップのサンプリング用）
    output.texcoord = input.position.xyz;
    return output;
}
