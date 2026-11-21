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
    float4x4 viewInverse;   ///< ビュー逆行列（水中の物の線を薄めるためにワールド座標へ戻す）
    float waterHeight;      ///< 水面のワールド Y
    float waterClarity;     ///< 水の透明度（m）。0 以下なら水中の線を薄めない
    float2 padding3;        ///< 16 バイト境界合わせ
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

/// @brief 平面とみなす曲がり具合（1/Z の 2 階差分を中心の 1/Z で割った値）の上限
/// @details これ以下の所は「なめらかな面」として線を出さない（2 倍までの間でなめらかに消す）。
///          角（壁と床の境目など）の線まで消えるようなら小さくする。
static const float kPlanarThreshold = 0.02f;

/// @brief 水中の線の薄め方に「視線の中の水の厚み」をどれだけ混ぜるか
/// @details Water.PS.hlsl の kWaterPathWeight と同じ値にして、物が水に沈んで見えなくなるのと
///          同じ割合で線も消えるようにする（線だけ透けて見えるのを防ぐ）。
static const float kOutlineWaterPathWeight = 0.1f;

/// @brief 水中の物に出す線の濃さ（0: 出さない, 1: 水上と同じ）
/// @details 水中の物は屈折で見かけの位置が揺れるのに、線は本当の位置に出るためズレる。
///          上げるほど水中の敵は見つけやすくなるが、そのぶん体と線のズレも目立つ。
static const float kUnderwaterOutlineStrength = 0.0f;

/// @brief 水面からこの深さ（m）までで、水上の線から水中の線へ切り替える
static const float kOutlineUnderwaterBand = 0.3f;

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
    float invZ[3][3]; // 1 / ビュー Z（平面判定に使う）

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
            invZ[x][y] = 1.0f / max(abs(viewZ), 1e-4f);

            difference.x += viewZ * kPrewittHorizontalKernel[x][y];
            difference.y += viewZ * kPrewittVerticalKernel[x][y];
        }
    }

    float weight = length(difference);
    weight = saturate(weight * gMaterial.outlineWeight);

    // 平らな面（遠くの床や海底を浅い角度で見た所）は線にしない。
    // 深度の 1 次の傾きだけで判定すると、遠くの斜面は 1 ピクセルごとの Z の差が大きいので
    // 面全体が輪郭色で塗りつぶされ、その上の深度を書かない物（水弾・水しぶき）が埋もれてしまう。
    // 平面上では 1/Z が画面上で一次式になるので、中心を通る 4 方向の 2 階差分がほぼ 0 になる。
    // 段差（本物の輪郭）では大きくなるので、これが小さい所は線を消す。
    {
        const float wc = invZ[1][1];
        float curv = abs(invZ[0][1] + invZ[2][1] - 2.0f * wc);
        curv = max(curv, abs(invZ[1][0] + invZ[1][2] - 2.0f * wc));
        curv = max(curv, abs(invZ[0][0] + invZ[2][2] - 2.0f * wc));
        curv = max(curv, abs(invZ[2][0] + invZ[0][2] - 2.0f * wc));
        const float relCurv = curv / max(wc, 1e-6f);
        weight *= smoothstep(kPlanarThreshold, kPlanarThreshold * 2.0f, relCurv);
    }

    // 水中の物の線は、物が水に霞むのと同じだけ薄める。
    // このパスは水を描いた後の画に掛かるが、水面は深度を書かないので、深度は水の奥の物のもの。
    // そのままだと水の色で隠れた深い床やサメの輪郭線だけが水面越しに透けて見えてしまう。
    // 水の減衰（Water.PS.hlsl の屈折）と同じ式で、線の重みに透過率を掛ける。
    if (gMaterial.waterClarity > 0.0f && weight > 0.0f) {
        float ndcDepth = gDepthTexture.SampleLevel(gSamplerPoint, input.texcoord, 0);
        float4 ndc = float4(input.texcoord.x * 2.0f - 1.0f, 1.0f - input.texcoord.y * 2.0f, ndcDepth, 1.0f);
        float4 vp = mul(ndc, gMaterial.projectionInverse);
        float3 scenePos = mul(float4(vp.xyz / vp.w, 1.0f), gMaterial.viewInverse).xyz;
        float3 camPos = mul(float4(0.0f, 0.0f, 0.0f, 1.0f), gMaterial.viewInverse).xyz;

        // カメラが水上で、その画素の物が水面より下にあるときだけ（水中視点は Underwater 側の担当）
        float depthBelow = gMaterial.waterHeight - scenePos.y;
        if (camPos.y > gMaterial.waterHeight && depthBelow > 0.0f) {
            float3 toScene = scenePos - camPos;
            float dist = length(toScene);
            float sinDown = max(-toScene.y / max(dist, 1e-4f), 1e-3f);
            float pathInWater = depthBelow / sinDown; // 水面から物までの視線の長さ
            float d = lerp(depthBelow, pathInWater, kOutlineWaterPathWeight) / gMaterial.waterClarity;
            // 水中の物は水面の屈折（Water.PS が画面をずらして引く）で見かけの位置が動くが、
            // 深度は本当の位置のままなので、線だけ体からズレてしまう。水中の線は基本的に出さない。
            // 水面すぐ下（0.3m）までは残し、水面から体を出している物の線が途切れないようにする。
            float submerge = smoothstep(0.0f, kOutlineUnderwaterBand, depthBelow);
            weight *= exp(-d) * lerp(1.0f, kUnderwaterOutlineStrength, submerge);
        }
    }

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
