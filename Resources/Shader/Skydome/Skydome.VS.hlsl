#include "../Object3d/Object3d.hlsli"

// ----------------------------------------------------------------------------
// Skydome 用頂点シェーダ
// ----------------------------------------------------------------------------
// Object3D_Single.VS.hlsl と同じ入出力だが、クリップ座標の z を w に置き換えて
// 深度を常に最遠 (z/w = 1) にする（Skybox.VS.hlsl と同じ手法）。
//
// これにより、カメラの Far クリップ距離や天球の半径・カメラ位置に関係なく
// 天球は「無限遠の背景」として描画され、Far より奥にある頂点が
// クリップされて消える問題が起きない。
// PSO 側（object3d_skydome）は深度テスト LESS_EQUAL・深度書き込み OFF で使う。
// ----------------------------------------------------------------------------

/// @struct TransformationMatrix
/// @brief オブジェクトごとの変換行列（Object3D の b0 と同じレイアウト）
struct TransformationMatrix
{
    float4x4 WVP;                   ///< ワールド・ビュー・プロジェクション行列
    float4x4 World;                 ///< ワールド行列
    float4x4 worldInverseTranspose; ///< ワールド行列の逆転置（法線変換用）
};

ConstantBuffer<TransformationMatrix> gTransformationMatrix : register(b0);

/// @struct VertexShaderInput
/// @brief 頂点入力（Object3D と同じレイアウト）
struct VertexShaderInput
{
    float4 position : POSITION0; ///< ローカル座標
    float2 texcoord : TEXCOORD0; ///< テクスチャ座標
    float3 normal : NORMAL0;     ///< 法線
};

/// @brief 天球の頂点を変換し、深度を最遠に固定して出力する
/// @param input 頂点入力
/// @return Object3D.PS と同じレイアウトの頂点出力
VertexShaderOutput main(VertexShaderInput input)
{
    VertexShaderOutput output;

    float4 clipPos = mul(input.position, gTransformationMatrix.WVP);
    // z = w にして深度を最遠に固定（Far クリップに掛からない）
    output.position = clipPos.xyww;

    output.texcoord = input.texcoord;
    output.normal = normalize(mul(input.normal, (float3x3) gTransformationMatrix.worldInverseTranspose));
    output.worldPosition = mul(input.position, gTransformationMatrix.World).xyz;
    output.instColor = float4(1, 1, 1, 1);

    // Object3D.PS と出力レイアウトを揃えるため計算しておく（天球は lightingMode = 0 なので未使用）
    output.lightSpacePos = mul(float4(output.worldPosition, 1.0f), gShadowParams.lightViewProjection);

    return output;
}
