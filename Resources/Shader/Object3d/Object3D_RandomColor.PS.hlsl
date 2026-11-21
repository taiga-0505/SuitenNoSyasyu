#include "Object3d.hlsli"

// =============================================================================
// RandomColor（オブジェクトごとのランダム色表示）
//
// ビューシェーディングのデバッグ表示用。Material.color をシードにしたハッシュから
// 鮮やかな色を決めて塗り分け、オブジェクトの境目を見分けやすくする。
// 陰影は固定方向の簡易シェーディングのみで、シーンのライトは参照しない。
// =============================================================================

/// @brief マテリアル定数（b0）。このシェーダーでは color をシードにのみ使う
struct Material
{
    float4 color;                 ///< 色のシード（RGBA のビット列をハッシュに使う）
    int lightingMode;             ///< 未使用
    float shininess;              ///< 未使用
    float environmentCoefficient; ///< 未使用
    float padding;                ///< パディング
    float4x4 uvTransform;         ///< 未使用
};

ConstantBuffer<Material> gMaterial : register(b0);

struct PixelShaderOutput
{
    float4 color : SV_TARGET0; ///< 出力色
};

/// @brief Wang hash で 32 bit 値を攪拌する
/// @param seed 入力値
/// @return ハッシュ値
uint WangHash(uint seed)
{
    seed = (seed ^ 61u) ^ (seed >> 16u);
    seed *= 9u;
    seed = seed ^ (seed >> 4u);
    seed *= 0x27d4eb2du;
    seed = seed ^ (seed >> 15u);
    return seed;
}

/// @brief HSV → RGB 変換
/// @param hue        色相（0〜1）
/// @param saturation 彩度（0〜1）
/// @param value      明度（0〜1）
/// @return RGB 色
float3 HsvToRgb(float hue, float saturation, float value)
{
    float h6 = hue * 6.0f;
    float c = value * saturation;
    float x = c * (1.0f - abs(fmod(h6, 2.0f) - 1.0f));
    float m = value - c;

    float3 rgb;
    if (h6 < 1.0f)      rgb = float3(c, x, 0.0f);
    else if (h6 < 2.0f) rgb = float3(x, c, 0.0f);
    else if (h6 < 3.0f) rgb = float3(0.0f, c, x);
    else if (h6 < 4.0f) rgb = float3(0.0f, x, c);
    else if (h6 < 5.0f) rgb = float3(x, 0.0f, c);
    else                 rgb = float3(c, 0.0f, x);

    return rgb + m;
}

/// @brief マテリアルごとのランダム色に簡易陰影を付けて出力する
/// @param input 頂点シェーダーからの補間値
/// @return 出力色
PixelShaderOutput main(VertexShaderOutput input)
{
    PixelShaderOutput output;

    // 定数バッファの Material.color のみをシードに使用
    // ※ 補間される値 (instColor 等) に asuint() を使うと
    //    ピクセルごとにビットパターンが変わりノイズになるため使用しない
    uint seed = asuint(gMaterial.color.r) ^
                (asuint(gMaterial.color.g) * 2654435761u) ^
                (asuint(gMaterial.color.b) * 374761393u) ^
                (asuint(gMaterial.color.a) * 668265263u);

    seed = WangHash(seed);

    // ハッシュから鮮やかなHSVカラーを生成
    float hue = frac(float(seed) / 4294967296.0f);
    float3 baseColor = HsvToRgb(hue, 0.65f, 0.92f);

    // 法線ベースの軽いシェーディング（立体感）
    float3 N = normalize(input.normal);
    float shade = abs(dot(N, normalize(float3(0.3f, -1.0f, 0.5f)))) * 0.35f + 0.65f;

    output.color = float4(baseColor * shade, 1.0f);
    output.color *= input.instColor;

    return output;
}
