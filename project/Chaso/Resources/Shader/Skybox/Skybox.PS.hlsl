#include "Skybox.hlsli"

// =============================================================================
// スカイボックス用ピクセルシェーダ
//
// skybox PSO で使う。キューブマップを方向ベクトルでサンプリングし、
// マテリアル色を掛けて出力する（ライティングはしない）。
// =============================================================================

/// @struct Material
/// @brief マテリアル（Object3D の Material と同じレイアウト）
struct Material
{
    float4 color;         ///< 乗算色（RGBA）
    int lightingMode;     ///< ライティング方式（ここでは使わない）
    float shininess;      ///< 鏡面反射の鋭さ（ここでは使わない）
    float padding[2];     ///< 16 バイト境界合わせ
    float4x4 uvTransform; ///< UV 変換行列（ここでは使わない）
};

/// @struct PixelShaderOutput
/// @brief ピクセル出力
struct PixelShaderOutput
{
    float4 color : SV_TARGET0; ///< 出力色
};

ConstantBuffer<Material> gMaterial : register(b0);
TextureCube<float4> gTexture : register(t0);
SamplerState gSampler : register(s0);

/// @brief キューブマップの色にマテリアル色を掛けて出力する
/// @param input スカイボックス VS の出力
/// @return 出力色
PixelShaderOutput main(VertexShaderOutput input)
{
    PixelShaderOutput output;
    // キューブマップを方向ベクトルでサンプリング
    float4 textureColor = gTexture.Sample(gSampler, input.texcoord);
    output.color = textureColor * gMaterial.color;
    return output;
}
