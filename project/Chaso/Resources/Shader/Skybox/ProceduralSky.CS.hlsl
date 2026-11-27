// =============================================================================
// プロシージャル空の生成（Compute Shader）
//
// キューブマップの 6 面（RWTexture2DArray の 6 スライス）の各テクセルについて、
// その方向に見える空の色を計算して書き込む。
//   空のグラデーション（天頂 → 地平線 → 地平線より下）＋ 太陽 ＋ 流れる雲 ＋ 地平線のかすみ
// 方向ベクトルから直接計算するので、面の境目に継ぎ目は出ない。
// C++ 側: Engine/Render/Skybox/ProceduralSky.cpp（Dispatch(size/8, size/8, 6)）
// =============================================================================

/// @brief 空のパラメータ（ProceduralSky.cpp の ProceduralSkyCB と同じ並び）
cbuffer ProceduralSkyParams : register(b0)
{
    float3 gZenith;          ///< 天頂の色
    float gGradientExponent; ///< グラデーションの曲がり
    float3 gHorizon;         ///< 地平線の色
    float gHorizonHaze;      ///< 地平線のかすみ（0〜1）
    float3 gGround;          ///< 地平線より下の色
    float gSunCosOuter;      ///< 太陽の縁の外側の cos
    float3 gSunDir;          ///< 太陽へ向かう向き（正規化済み）
    float gSunCosInner;      ///< 太陽の縁の内側の cos
    float3 gSunColor;        ///< 太陽の色
    float gSunIntensity;     ///< 太陽本体の明るさ
    float3 gCloudColor;      ///< 雲の日なた側の色
    float gCloudCoverage;    ///< 雲の量（0〜1）
    float3 gCloudShadow;     ///< 雲の影側の色
    float gCloudScale;       ///< 雲の模様の細かさ
    float2 gCloudOffset;     ///< 雲の流れた量（XZ）
    float gCloudOpacity;     ///< 雲の不透明度
    uint gFaceSize;          ///< 1 面の一辺（テクセル）
    float gSunGlow;          ///< 太陽のまわりの光の強さ
    float gSunGlowPower;     ///< 太陽のまわりの光の鋭さ
    float gCloudSoftness;    ///< 雲の縁のぼかし幅
    float gPad;              ///< 16 バイト境界合わせ
};

RWTexture2DArray<float4> gOutput : register(u0);

/// @brief 2D 座標から 0〜1 の擬似乱数を作る
/// @param p 座標
/// @return 0〜1 の値
float Hash12(float2 p)
{
    float3 p3 = frac(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

/// @brief 値ノイズ（格子点の乱数を滑らかに補間）
/// @param p 座標
/// @return 0〜1 の値
float ValueNoise(float2 p)
{
    float2 i = floor(p);
    float2 f = frac(p);
    float2 u = f * f * (3.0 - 2.0 * f);
    float a = Hash12(i);
    float b = Hash12(i + float2(1.0, 0.0));
    float c = Hash12(i + float2(0.0, 1.0));
    float d = Hash12(i + float2(1.0, 1.0));
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

/// @brief 値ノイズを 6 オクターブ重ねる（もこもこした雲の形）
/// @param p 座標
/// @return おおよそ 0〜1 の値
float Fbm(float2 p)
{
    // オクターブごとに少し回して格子の向きをそろえない
    const float2x2 rot = float2x2(0.8, -0.6, 0.6, 0.8);
    float sum = 0.0;
    float amp = 0.5;
    for (int i = 0; i < 6; ++i)
    {
        sum += amp * ValueNoise(p);
        p = mul(rot, p) * 2.03 + 17.0;
        amp *= 0.5;
    }
    return sum / 0.984375; // 振幅の合計（1 - 0.5^6）で割って 0〜1 に戻す
}

/// @brief キューブマップの面とテクセルから方向ベクトルを作る（D3D のキューブマップの面の並び）
/// @param face 面の番号（0:+X 1:-X 2:+Y 3:-Y 4:+Z 5:-Z）
/// @param uv   面の中の位置（-1〜1。+u が右、+v が下）
/// @return 正規化した方向
float3 CubeDirection(uint face, float2 uv)
{
    float3 d;
    switch (face)
    {
    case 0: d = float3(1.0, -uv.y, -uv.x); break;
    case 1: d = float3(-1.0, -uv.y, uv.x); break;
    case 2: d = float3(uv.x, 1.0, uv.y); break;
    case 3: d = float3(uv.x, -1.0, -uv.y); break;
    case 4: d = float3(uv.x, -uv.y, 1.0); break;
    default: d = float3(-uv.x, -uv.y, -1.0); break;
    }
    return normalize(d);
}

/// @brief 雲の濃さ（0〜1）を求める
/// @param p 雲の平面上の座標
/// @return 濃さ
float CloudDensity(float2 p)
{
    // 大きな形を少しゆがめてから細部を足す（綿雲らしい塊にする）
    float2 warp = float2(ValueNoise(p * 0.5 + 3.1), ValueNoise(p * 0.5 + 7.7)) - 0.5;
    float n = Fbm(p + warp * 0.8);
    float threshold = 1.0 - gCloudCoverage;
    return smoothstep(threshold - gCloudSoftness * 0.5, threshold + gCloudSoftness * 0.5, n);
}

/// @brief 方向 d に見える空の色を求める
/// @param d 正規化した方向
/// @return 色（リニア）
float3 SkyColor(float3 d)
{
    float h = d.y;

    // --- 空のグラデーション ---
    float3 col;
    if (h >= 0.0)
    {
        col = lerp(gHorizon, gZenith, pow(saturate(h), gGradientExponent));
    }
    else
    {
        col = lerp(gHorizon, gGround, saturate(-h * 6.0));
    }

    // --- 太陽 ---
    float cosA = dot(d, gSunDir);
    float aboveHorizon = smoothstep(-0.02, 0.02, h);
    float disk = smoothstep(gSunCosOuter, gSunCosInner, cosA) * aboveHorizon;
    float glow = pow(saturate(cosA), gSunGlowPower) * gSunGlow;
    col += gSunColor * (disk * gSunIntensity + glow);

    // --- 雲（空の高いところにある平面へ投影する） ---
    if (h > 0.0 && gCloudCoverage > 0.0)
    {
        // 地平線に近いほど遠くを見るので、雲の平面上の座標は 1/h で広がる
        float2 p = d.xz / (h + 0.12) * gCloudScale + gCloudOffset;
        float density = CloudDensity(p);

        if (density > 0.0)
        {
            // 太陽側へ少しずらした所の濃さと比べて、太陽側が薄ければ日なた（明るい）
            float2 toSun = normalize(gSunDir.xz + 1e-4) * 0.15;
            float towardSun = CloudDensity(p + toSun);
            float lit = saturate(0.65 + (density - towardSun) * 1.5);
            float3 cloud = lerp(gCloudShadow, gCloudColor, lit);
            // 太陽の近くの雲は縁が光る
            cloud += gSunColor * glow * 1.5;
            // 地平線近くの雲は薄くしてかすみに溶かす
            float fade = smoothstep(0.0, 0.18, h);
            col = lerp(col, cloud, density * gCloudOpacity * fade);
        }
    }

    // --- 地平線のかすみ ---
    float haze = exp(-abs(h) * 14.0) * gHorizonHaze;
    float3 hazeColor = lerp(gHorizon, float3(1.0, 1.0, 1.0), 0.35);
    col = lerp(col, hazeColor, haze);

    return col;
}

/// @brief 1 スレッドで 1 テクセル（id.z が面の番号）
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gFaceSize || id.y >= gFaceSize)
    {
        return;
    }
    float2 uv = (float2(id.xy) + 0.5) / float(gFaceSize) * 2.0 - 1.0;
    float3 dir = CubeDirection(id.z, uv);
    gOutput[id] = float4(SkyColor(dir), 1.0);
}
