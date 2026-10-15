#include "../Fullscreen/Fullscreen.hlsli"

// =============================================================================
// 深度ベースアウトライン
//
// 深度をビュー空間 Z に戻し、3x3 の Prewitt フィルタで深度の段差を検出して
// 輪郭色を合成するポストエフェクト（depthbasedoutline PSO）。
// outlineMode で段差の手前側・奥側のどちらに線を出すかを選べる。
// =============================================================================

Texture2D<float4> gTexture : register(t0);
Texture2D<float> gDepthTexture : register(t1);

SamplerState gSampler : register(s0);
SamplerState gSamplerPoint : register(s1);

/// @brief 輪郭を出さない矩形（HUD の領域）の上限
/// @details このパスは 2D まで描き終えた最終画に掛かるので、放っておくと
///          HP バーやタイマーの上に壁の輪郭が乗ってしまう。UI が申告した矩形を素通しにする。
static const int kMaxExclusions = 16;

/// @struct Material
/// @brief 深度ベースアウトラインのパラメータ（b1）
/// @details C++ 側 PostProcess::MaterialData と 1:1 で対応する。増減時は両方を合わせること。
struct Material {
    float4x4 projectionInverse; ///< プロジェクション逆行列
    float4 outlineColor;        ///< 輪郭の色
    float outlineWeight;        ///< 検出の重み（段差の大きさに掛ける倍率）
    float outlineThickness;     ///< 太さ（ピクセル）
    int outlineMode;            ///< 0 = 両側、1 = 外側、2 = 内側
    float padding;              ///< 16 バイト境界合わせ
    int excludeCount;           ///< 有効な除外矩形の数
    float3 padding2;            ///< 16 バイト境界合わせ
    float4 excludeRects[kMaxExclusions]; ///< (minU, minV, maxU, maxV)
};
ConstantBuffer<Material> gMaterial : register(b1);

// Prewitt フィルタ（横方向用）
// NOTE: 下のループでは [x][y] の順で引くため、実際には縦横が入れ替わった勾配になる。
//       最終的に length(difference) しか使わないので結果には影響しない。
static const float kPrewittHorizontalKernel[3][3] = {
    { -1.0f, 0.0f, 1.0f },
    { -1.0f, 0.0f, 1.0f },
    { -1.0f, 0.0f, 1.0f }
};

// Prewitt フィルタ（縦方向用）
static const float kPrewittVerticalKernel[3][3] = {
    { -1.0f, -1.0f, -1.0f },
    {  0.0f,  0.0f,  0.0f },
    {  1.0f,  1.0f,  1.0f }
};

/// @brief 深度の段差を検出し、輪郭色を合成する
/// @param input フルスクリーン VS の出力
/// @return 出力色
float4 main(VertexShaderOutput input) : SV_TARGET {
    // UI の上には輪郭を出さない（HUD が申告した矩形の中は素通し）
    for (int e = 0; e < gMaterial.excludeCount; ++e) {
        const float4 rect = gMaterial.excludeRects[e];
        if (input.texcoord.x >= rect.x && input.texcoord.x <= rect.z &&
            input.texcoord.y >= rect.y && input.texcoord.y <= rect.w) {
            return gTexture.Sample(gSampler, input.texcoord);
        }
    }

    uint width, height;
    gDepthTexture.GetDimensions(width, height);
    float2 uvStepSize = float2(1.0f / width, 1.0f / height);

    float2 difference = float2(0.0f, 0.0f);
    float centerZ = 0.0f;
    float minZ = 100000.0f;
    float maxZ = -100000.0f;

    // 周囲 3x3 の深度をビュー空間 Z に戻し、勾配と最小・最大を集める
    for (int y = 0; y < 3; ++y) {
        for (int x = 0; x < 3; ++x) {
            float2 offset = float2(x - 1, y - 1) * uvStepSize * gMaterial.outlineThickness;
            float2 sampleUv = input.texcoord + offset;

            float ndcDepth = gDepthTexture.SampleLevel(gSamplerPoint, sampleUv, 0);
            float4 viewSpace = mul(float4(0.0f, 0.0f, ndcDepth, 1.0f), gMaterial.projectionInverse);
            float viewZ = viewSpace.z / viewSpace.w;

            if (x == 1 && y == 1) centerZ = viewZ;
            minZ = min(minZ, viewZ);
            maxZ = max(maxZ, viewZ);

            difference.x += viewZ * kPrewittHorizontalKernel[x][y];
            difference.y += viewZ * kPrewittVerticalKernel[x][y];
        }
    }

    float weight = length(difference);
    weight = saturate(weight * gMaterial.outlineWeight);

    // 中心が段差のどちら側にあるかで、片側だけに線を残す
    if (gMaterial.outlineMode == 1) { // 外側
        if (abs(centerZ - minZ) < abs(centerZ - maxZ)) {
            weight = 0.0f;
        }
    } else if (gMaterial.outlineMode == 2) { // 内側
        if (abs(centerZ - maxZ) < abs(centerZ - minZ)) {
            weight = 0.0f;
        }
    }

    float4 color = gTexture.Sample(gSampler, input.texcoord);

    return lerp(color, gMaterial.outlineColor, weight);
}
