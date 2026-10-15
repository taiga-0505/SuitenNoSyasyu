// =============================================================================
// スプライト用頂点シェーダ
//
// sprite / sprite3d PSO で共通に使う。sprite では正射影の WVP、
// sprite3d ではカメラの WVP が渡されるだけで、シェーダは同じ。
// =============================================================================

/// @struct SpriteVSInput
/// @brief 頂点入力
struct SpriteVSInput
{
    float4 position : POSITION0; ///< ローカル座標
    float2 texcoord : TEXCOORD0; ///< テクスチャ座標
};

/// @struct SpriteVSOutput
/// @brief 頂点出力
struct SpriteVSOutput
{
    float4 position : SV_POSITION; ///< クリップ空間位置
    float2 texcoord : TEXCOORD0;   ///< テクスチャ座標
};

/// @struct TransformationMatrix
/// @brief スプライトの変換行列
struct TransformationMatrix
{
    float4x4 WVP;   ///< ワールド・ビュー・プロジェクション行列
    float4x4 World; ///< ワールド行列（ここでは使わない）
};

ConstantBuffer<TransformationMatrix> gTransformationMatrix : register(b0);
/// @brief 頂点をクリップ空間へ変換する
/// @param input 頂点入力
/// @return クリップ空間位置とテクスチャ座標
SpriteVSOutput main(SpriteVSInput input)
{
    SpriteVSOutput output;
    output.position = mul(input.position, gTransformationMatrix.WVP);
    output.texcoord = input.texcoord;
    return output;
}
