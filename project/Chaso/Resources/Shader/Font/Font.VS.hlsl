// =============================================================================
// Font.VS.hlsl
// スクリーン座標（ピクセル）の頂点を正射影で変換するだけの頂点シェーダ
// （font PSO）
// =============================================================================

/// @struct FontVSInput
/// @brief 頂点入力
struct FontVSInput
{
    float4 position : POSITION0; ///< ピクセル座標（x, y, 0, 1）
    float2 texcoord : TEXCOORD0; ///< アトラス UV
    float4 color    : COLOR0;    ///< 頂点カラー
};

/// @struct FontVSOutput
/// @brief 頂点出力
struct FontVSOutput
{
    float4 position : SV_POSITION; ///< クリップ空間位置
    float2 texcoord : TEXCOORD0;   ///< アトラス UV
    float4 color    : COLOR0;      ///< 頂点カラー
};

/// @struct TransformationMatrix
/// @brief 変換行列（C++ 側 TransformationMatrix と同じレイアウト）
struct TransformationMatrix
{
    float4x4 WVP;                   ///< ワールド・ビュー・プロジェクション行列（正射影）
    float4x4 World;                 ///< ワールド行列（ここでは使わない）
    float4x4 WorldInverseTranspose; ///< ワールド行列の逆転置（ここでは使わない）
};

ConstantBuffer<TransformationMatrix> gTransformationMatrix : register(b0);

/// @brief ピクセル座標の頂点をクリップ空間へ変換する
/// @param input 頂点入力
/// @return クリップ空間位置・アトラス UV・頂点カラー
FontVSOutput main(FontVSInput input)
{
    FontVSOutput output;
    output.position = mul(input.position, gTransformationMatrix.WVP);
    output.texcoord = input.texcoord;
    output.color = input.color;
    return output;
}
