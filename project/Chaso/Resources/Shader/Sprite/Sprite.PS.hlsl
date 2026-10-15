// =============================================================================
// スプライト用ピクセルシェーダ
//
// sprite / sprite3d PSO で使う。UV 変換してテクスチャを引き、α が 0.5 以下を
// 捨てるアルファテスト方式（sprite3d で深度書き込み ON でもフチが破綻しないため）。
// =============================================================================

/// @struct SpriteVSOutput
/// @brief Sprite.VS からの入力
struct SpriteVSOutput
{
    float4 position : SV_POSITION; ///< クリップ空間位置
    float2 texcoord : TEXCOORD0;   ///< テクスチャ座標
};

/// @struct Material
/// @brief スプライトのマテリアル
/// @details C++ の struct Material と同じレイアウトにする。
struct Material
{
    float4 color;         ///< 乗算色（RGBA）
    float4x4 uvTransform; ///< UV 変換行列
};

// b0: cbMat_ がバインドされるレジスタ
ConstantBuffer<Material> gMaterial : register(b0);

Texture2D gTexture : register(t0);
SamplerState gSampler : register(s0);

/// @brief UV 変換したテクスチャ色にマテリアル色を掛けて出力する
/// @param input Sprite.VS の出力
/// @return 出力色（α が 0.5 以下のピクセルは discard）
float4 main(SpriteVSOutput input) : SV_TARGET
{
    // UV 変換
    float4 uv = mul(float4(input.texcoord, 0.0f, 1.0f), gMaterial.uvTransform);
    float2 tex = uv.xy;

    float4 texColor = gTexture.Sample(gSampler, tex);

    // αテスト（半透明のフチを残さない）
    if (texColor.a <= 0.5f)
        discard;

    return texColor * gMaterial.color;
}
