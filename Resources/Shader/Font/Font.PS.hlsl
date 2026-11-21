// =============================================================================
// Font.PS.hlsl
// R8_UNORM のグリフアトラス（カバレッジ）をアルファとして頂点カラーを出力する
// （font PSO）
// =============================================================================

/// @struct FontVSOutput
/// @brief Font.VS からの入力
struct FontVSOutput
{
    float4 position : SV_POSITION; ///< クリップ空間位置
    float2 texcoord : TEXCOORD0;   ///< アトラス UV
    float4 color    : COLOR0;      ///< 頂点カラー
};

/// @struct Material
/// @brief 文字のマテリアル
/// @details C++ 側 SpriteMaterial と同じレイアウト（FontManager は白・identity 固定）。
struct Material
{
    float4 color;         ///< 乗算色（RGBA）
    float4x4 uvTransform; ///< UV 変換行列（ここでは使わない）
};

ConstantBuffer<Material> gMaterial : register(b0);

Texture2D<float> gAtlas : register(t0);
SamplerState gSampler : register(s0); // リニア

/// @brief 頂点カラーにアトラスのカバレッジをαとして掛けて出力する
/// @param input Font.VS の出力
/// @return 出力色（ほぼ透明なピクセルは discard）
float4 main(FontVSOutput input) : SV_TARGET
{
    float coverage = gAtlas.Sample(gSampler, input.texcoord);

    float4 outColor = input.color * gMaterial.color;
    outColor.a *= coverage;

    // 完全に透明なピクセルは捨てる（Sprite の 0.5 αテストは文字の縁を欠くので使わない）
    if (outColor.a <= 0.001f)
        discard;

    return outColor;
}
