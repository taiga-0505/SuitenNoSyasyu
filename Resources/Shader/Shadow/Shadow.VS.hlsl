#include "../Object3d/Object3d.hlsli"

// =============================================================================
// シャドウマップ用頂点シェーダ（単体描画）
//
// shadow PSO で使う。ルートシグネチャ・入力レイアウトは object3d と同じ。
// 頂点をライト視点のクリップ空間へ変換するだけ。
// =============================================================================

/// @struct TransformationMatrix
/// @brief オブジェクトごとの変換行列（Object3D の b0 と同じレイアウト）
struct TransformationMatrix {
    matrix WVP;                   ///< ワールド・ビュー・プロジェクション行列（ここでは使わない）
    matrix World;                 ///< ワールド行列
    matrix WorldInverseTranspose; ///< ワールド行列の逆転置（法線変換用。ここでは使わない）
};
ConstantBuffer<TransformationMatrix> gTransform : register(b0);

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

/// @brief 頂点をライト視点のクリップ空間へ変換する
/// @param input 頂点入力
/// @return ライト視点のクリップ空間位置
VSOutput main(VertexShaderInput input) {
    VSOutput output;
    // シャドウマップ描画時は、カメラの代わりにライト視点の変換を使う。
    // b0 の WVP は使わず、World 行列 × シャドウの lightViewProjection で変換する。
    float4 worldPos = mul(input.position, gTransform.World);
    output.position = mul(worldPos, gShadowParams.lightViewProjection);
    
    return output;
}
