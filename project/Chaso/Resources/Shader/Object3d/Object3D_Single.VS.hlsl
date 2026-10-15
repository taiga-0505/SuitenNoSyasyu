#include "Object3d.hlsli"

// =============================================================================
// Object3D 単体描画の頂点シェーダー
//
// 1 オブジェクト = 1 ドローの通常描画用。行列は定数バッファ（b0）から受け取る。
// 通常の Object3d.PS のほか、ガラス・水球・水柱・スキャン系・ワイヤーフレーム・
// デバッグ表示など、Object3D 系 PS の単体描画版すべてで共用する。
// =============================================================================

/// @brief 座標変換行列（b0）
struct TransformationMatrix
{
    float4x4 WVP;                   ///< ワールド × ビュー × プロジェクション
    float4x4 World;                 ///< ワールド行列
    float4x4 worldInverseTranspose; ///< ワールド行列の逆転置（法線の変換用）
};

ConstantBuffer<TransformationMatrix> gTransformationMatrix : register(b0);

struct VertexShaderInput
{
    float4 position : POSITION0; ///< モデルローカルの位置
    float2 texcoord : TEXCOORD0; ///< UV
    float3 normal : NORMAL0;     ///< モデルローカルの法線
};

/// @brief 頂点をクリップ空間へ変換し、ライティング・影に必要な値を PS へ渡す
/// @param input 頂点入力
/// @return クリップ空間位置・ワールド座標・ライト空間座標などの出力
VertexShaderOutput main(VertexShaderInput input)
{
    VertexShaderOutput output;
    output.position = mul(input.position, gTransformationMatrix.WVP);
    output.texcoord = input.texcoord;
    output.normal = normalize(mul(input.normal, (float3x3) gTransformationMatrix.worldInverseTranspose));
    output.worldPosition = mul(input.position, gTransformationMatrix.World).xyz;
    output.instColor = float4(1, 1, 1, 1);
    
    // ライトビュープロジェクションでライト空間の座標を計算
    output.lightSpacePos = mul(float4(output.worldPosition, 1.0f), gShadowParams.lightViewProjection);
    
    return output;
}
