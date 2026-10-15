#include "Object3d.hlsli"

// =============================================================================
// Object3D 標準ピクセルシェーダー
//
// 3D モデルの通常描画用（object3d / object3d_inst / object3d_skin 系の PSO）。
//   - テクスチャ × マテリアル色 × インスタンス色。アルファ 0.5 以下は抜く（2 値抜き）
//   - 拡散は Lambert / Half-Lambert、鏡面は Blinn-Phong
//   - 平行光源・点光源・スポットライト・面光源（矩形 / 線）を合算する
//   - 点・スポット・面光源はシャドウアトラスで遮蔽し、壁の向こうへ光が漏れないようにする
//   - 平行光源のシャドウマップ（PCF）で影を落とし、最後に環境マップの映り込みを足す
//   - 法線マップ・ラフネスマップに対応（マテリアルのフラグで切り替え）
// =============================================================================

/// @brief マテリアル定数（b0）
struct Material
{
    float4 color;                 ///< 色（RGBA）
    int lightingMode;             ///< 0: なし、1: Lambert、2: Half-Lambert（※3 / 4 が来ても Lambert 扱い）
    float shininess;              ///< Blinn-Phong の指数（0 以下なら鏡面なし）
    float environmentCoefficient; ///< 環境マップの映り込み係数（0〜1）
    int useNormalMap;             ///< 0 以外で法線マップ（t2）を使う
    int useRoughnessMap;          ///< 0 以外でラフネスマップ（t3）を使う
    float2 padding;               ///< パディング（16 バイト境界合わせ）
    float4x4 uvTransform;         ///< UV 変換
};

ConstantBuffer<Material> gMaterial : register(b0);

/// @brief 平行光源と環境光（b1）
struct DirectionalLight
{
    float4 color;           ///< 光の色
    float3 direction;       ///< 光の進む向き（正規化済み想定）
    float intensity;        ///< 強度
    float3 ambientColor;    ///< 環境光の色
    float ambientIntensity; ///< 環境光の強さ（0 で環境光なし）
};

ConstantBuffer<DirectionalLight> gDirectionalLight : register(b1);

/// @brief カメラ情報（b2）
struct Camera
{
    float3 worldPosition; ///< カメラのワールド座標
};

ConstantBuffer<Camera> gCamera : register(b2);

/// @brief 点光源 1 灯ぶんのパラメータ
struct PointLight
{
    float4 color;      ///< 光の色
    float3 position;   ///< 光の位置
    float intensity;   ///< 強度
    float radius;      ///< 届く距離（0 以下なら無効）
    float decay;       ///< 減衰（指数）
    int shadowIndex;   ///< 影アトラスのタイル番号（-1 で影なし）
    float padding;     ///< 定数バッファのアラインメント調整
};

static const uint MAX_POINT_LIGHTS = 256; ///< 点光源の最大数

/// @brief 点光源の配列（b3）
cbuffer PointLightsCB : register(b3)
{
    uint pointCount;                          ///< 有効な点光源の数
    float3 _padPoint;                         ///< パディング（16 バイト境界合わせ）
    PointLight pointLights[MAX_POINT_LIGHTS]; ///< 点光源
};

/// @brief スポットライト 1 灯ぶんのパラメータ
struct SpotLight
{
    float4 color;      ///< 光の色
    float3 position;   ///< 光の位置
    float intensity;   ///< 強度
    float3 direction;  ///< 照射方向
    float distance;    ///< 届く距離（0 以下なら無効）
    float decay;       ///< 減衰（指数）
    float cosAngle;    ///< 照射範囲の境界角の cos（これより外側は 0）
    int shadowIndex;   ///< スポット影アトラスのタイル番号（-1 で影なし）
    float padding;     ///< パディング
};

static const uint MAX_SPOT_LIGHTS = 256; ///< スポットライトの最大数

/// @brief スポットライトの配列（b4）
cbuffer SpotLightsCB : register(b4)
{
    uint spotCount;                       ///< 有効なスポットライトの数
    float3 _padSpot;                      ///< パディング（16 バイト境界合わせ）
    SpotLight spotLights[MAX_SPOT_LIGHTS]; ///< スポットライト
};


/// @brief 面光源 1 灯ぶんのパラメータ
/// @details halfHeight が 0 以下なら、right 方向へ ±halfWidth 伸びる線光源（チューブライト）として扱う。
struct AreaLight
{
    float4 color;     ///< 光の色
    float3 position;  ///< 面の中心位置
    float intensity;  ///< 強度

    float3 right;     ///< 面の右方向（正規化推奨）
    float halfWidth;  ///< 右方向の半幅

    float3 up;        ///< 面の上方向（正規化推奨）
    float halfHeight; ///< 上方向の半高（0 以下で線光源）

    float range;      ///< 影響距離（0 以下なら無効）
    float decay;      ///< 減衰（指数）
    uint twoSided;    ///< 1 なら両面
    int shadowIndex;  ///< 影アトラスのタイル番号（-1 で影なし）
};

static const uint MAX_AREA_LIGHTS = 256; ///< 面光源の最大数

/// @brief 面光源の配列（b5）
cbuffer AreaLightsCB : register(b5)
{
    uint areaCount;                        ///< 有効な面光源の数
    float3 _padArea;                       ///< パディング（16 バイト境界合わせ）
    AreaLight areaLights[MAX_AREA_LIGHTS]; ///< 面光源
};

Texture2D<float4> gTexture : register(t0);
TextureCube<float4> gEnvironmentTexture : register(t1);
Texture2D<float4> gNormalMap : register(t2);
Texture2D<float4> gRoughnessMap : register(t3);
SamplerState gSampler : register(s0);

struct PixelShaderOutput
{
    float4 color : SV_TARGET0; ///< 出力色
};

/// @brief 全ライト・影・環境マップを反映した色を出力する
/// @param input 頂点シェーダーからの補間値
/// @return 出力色（a はベース色のアルファ）
PixelShaderOutput main(VertexShaderOutput input)
{
    PixelShaderOutput output;

    // UV 変換＆テクスチャサンプル
    float4 uv = mul(float4(input.texcoord, 0.0f, 1.0f), gMaterial.uvTransform);
    float2 transformedUV = uv.xy;
    float4 textureColor = gTexture.Sample(gSampler, transformedUV);

    // 2値抜き（必要なら閾値を調整）
    if (textureColor.a <= 0.5f /* || textureColor.a == 0.0f || output.color.a == 0.0f*/)
    {
        discard;
    }

    // ベース色（材質×テクスチャ）
    float4 base = gMaterial.color * textureColor;
    base *= input.instColor;

    // ライティングなし
    if (gMaterial.lightingMode == 0)
    {
        output.color = base;
        return output;
    }

    // =========================
    // Lambert / Half-Lambert + Blinn-Phong（固定）
    // =========================

    float3 N = normalize(input.normal);

    if (gMaterial.useNormalMap)
    {
        float4 normalMapCol = gNormalMap.Sample(gSampler, transformedUV);
        float3 normalTangentSpace = normalMapCol.xyz * 2.0f - 1.0f;

        // TBN 行列を計算
        float3 dp1 = ddx(input.worldPosition);
        float3 dp2 = ddy(input.worldPosition);
        float2 duv1 = ddx(transformedUV);
        float2 duv2 = ddy(transformedUV);

        float r = 1.0f / (duv1.x * duv2.y - duv1.y * duv2.x);
        float3 T = normalize((dp1 * duv2.y - dp2 * duv1.y) * r);
        float3 B = normalize((dp2 * duv1.x - dp1 * duv2.x) * r);

        float3x3 TBN = float3x3(T, B, N);
        N = normalize(mul(normalTangentSpace, TBN));
    }

    float currentShininess = gMaterial.shininess;
    if (gMaterial.useRoughnessMap && currentShininess > 0.0f)
    {
        float4 roughnessCol = gRoughnessMap.Sample(gSampler, transformedUV);
        float roughness = roughnessCol.r;
        currentShininess = max(0.1f, currentShininess * (1.0f - roughness));
        if (roughness > 0.99f) currentShininess = 0.0f;
    }

    // L は「面 → 光」方向に統一（Directionalは -direction でOK）
    float3 L = normalize(-gDirectionalLight.direction);

    // V は「面 → 視点」
    float3 V = normalize(gCamera.worldPosition - input.worldPosition);

    float NdotL = dot(N, L);

    // 拡散（Lambert or Half-Lambert）
    float diffuseTerm = 0.0f;
    if (gMaterial.lightingMode == 2)
    {
        // Half-Lambert（暗部を持ち上げ）
        float h = saturate(NdotL * 0.5f + 0.5f);
        diffuseTerm = h * h;
    }
    else
    {
        diffuseTerm = saturate(NdotL);
    }

    float3 dirLightCol = gDirectionalLight.color.rgb * gDirectionalLight.intensity;

    // -----------------
    // 平行光源
    // -----------------
    float3 diffuseDir = base.rgb * dirLightCol * diffuseTerm;

    float3 specularDir = 0.0f;
    if (currentShininess > 0.0f && NdotL > 0.0f) // 裏面にハイライト出さない
    {
        float3 H = normalize(L + V);
        float specPow = pow(saturate(dot(N, H)), currentShininess);
        specularDir = dirLightCol * specPow; // 白ハイライト
    }

// -----------------
// 点光源（最大 MAX_POINT_LIGHTS 灯）
// -----------------
float3 diffusePoint = 0.0f;
float3 specularPoint = 0.0f;

[loop]
for (uint i = 0; i < MAX_POINT_LIGHTS; ++i)
{
    if (i >= pointCount) { break; }

    PointLight pl = pointLights[i];

    if (pl.intensity <= 0.0f || pl.radius <= 0.0f) { continue; }

    float3 toLight = pl.position - input.worldPosition;
    float dist = length(toLight);

    if (dist >= pl.radius || dist <= 1e-5f) { continue; }

    float3 Lp = toLight / dist;

    // 減衰: (1 - d/r)^decay
    float t = saturate(1.0f - dist / pl.radius);
    float attenuation = pow(t, max(pl.decay, 0.0001f));

    float NdotLp = dot(N, Lp);

    float diffuseTermP = 0.0f;
    if (gMaterial.lightingMode == 2)
    {
        float h = saturate(NdotLp * 0.5f + 0.5f);
        diffuseTermP = h * h;
    }
    else
    {
        diffuseTermP = saturate(NdotLp);
    }

    float3 pointLightCol = pl.color.rgb * pl.intensity * attenuation;

    // 点光源の影（壁の向こう側へ漏れない）
    if (pl.shadowIndex >= 0)
    {
        pointLightCol *= SampleOmniShadowDown(pl.shadowIndex, input.worldPosition, N, Lp);
    }

    diffusePoint += base.rgb * pointLightCol * diffuseTermP;

    if (currentShininess > 0.0f && NdotLp > 0.0f)
    {
        float3 H = normalize(Lp + V);
        float specPow = pow(saturate(dot(N, H)), currentShininess);
        specularPoint += pointLightCol * specPow;
    }
}

// -----------------
// スポットライト（最大 MAX_SPOT_LIGHTS 灯）
// -----------------
float3 diffuseSpot = 0.0f;
float3 specularSpot = 0.0f;

[loop]
for (uint i = 0; i < MAX_SPOT_LIGHTS; ++i)
{
    if (i >= spotCount) { break; }

    SpotLight sl = spotLights[i];

    if (sl.intensity <= 0.0f || sl.distance <= 0.0f) { continue; }

    float3 toLightS = sl.position - input.worldPosition;
    float distS = length(toLightS);

    if (distS >= sl.distance || distS <= 1e-5f) { continue; }

    float3 Ls = toLightS / distS;

    // 距離減衰: (1 - d/D)^decay
    float tS = saturate(1.0f - distS / sl.distance);
    float attenuationS = pow(tS, max(sl.decay, 0.0001f));

    // スポット係数（中心ほど強く、cosAngle以下で0）
    float3 dirN = normalize(sl.direction);
    float cosLD = dot(-Ls, dirN); // 光の向き(ライト→前) と (ライト→ピクセル) を比較
    float spot = saturate((cosLD - sl.cosAngle) / max(1e-5f, (1.0f - sl.cosAngle)));

    if (spot <= 0.0f) { continue; }

    float3 spotLightCol = sl.color.rgb * sl.intensity * attenuationS * spot;

    // スポット影: 壁などに遮られている分だけ光を弱める（完全に遮られれば 0 ＝ 向こう側へ漏れない）
    if (sl.shadowIndex >= 0)
    {
        spotLightCol *= SampleSpotShadow(sl.shadowIndex, input.worldPosition, N, Ls);
    }

    float NdotLs = dot(N, Ls);

    float diffuseTermS = 0.0f;
    if (gMaterial.lightingMode == 2)
    {
        float h = saturate(NdotLs * 0.5f + 0.5f);
        diffuseTermS = h * h;
    }
    else
    {
        diffuseTermS = saturate(NdotLs);
    }

    diffuseSpot += base.rgb * spotLightCol * diffuseTermS;

    if (currentShininess > 0.0f && NdotLs > 0.0f)
    {
        float3 Hs = normalize(Ls + V);
        float specPowS = pow(saturate(dot(N, Hs)), currentShininess);
        specularSpot += spotLightCol * specPowS;
    }
}


// 合算

    // -----------------
    // 面光源（矩形）※擬似: 四隅 4 サンプル
    // -----------------
    float3 diffuseArea = 0.0f;
    float3 specularArea = 0.0f;

    const uint aCount = min(areaCount, MAX_AREA_LIGHTS);
    [loop]
    for (uint ai = 0; ai < aCount; ++ai)
    {
        AreaLight al = areaLights[ai];
        if (al.intensity <= 0.0f || al.range <= 0.0f)
            continue;

        float3 R = al.right;
        float3 U = al.up;

        // 念のため正規化（0ベクトル回避）
        float rLen = length(R);
        float uLen = length(U);
        if (rLen < 1e-5f || uLen < 1e-5f)
            continue;
        R /= rLen;
        U /= uLen;

        // -----------------
        // チューブ（線）ライト: halfHeight <= 0 なら「position を中心に right 方向へ ±halfWidth 伸びる線分」
        // から全方位に光る線光源として扱う（紐・ネオン管など）。
        // 線分上でピクセルに最も近い点を光源位置とみなす近似（全長にわたって均一な光の帯になる）。
        // -----------------
        if (al.halfHeight <= 0.0f)
        {
            float3 segA = al.position - R * al.halfWidth;
            float3 segAB = R * (2.0f * al.halfWidth);
            float segLen2 = max(dot(segAB, segAB), 1e-6f);
            float tSeg = saturate(dot(input.worldPosition - segA, segAB) / segLen2);
            float3 nearest = segA + segAB * tSeg;

            float3 toT = nearest - input.worldPosition;
            float distT = length(toT);
            if (distT <= 1e-5f || distT >= al.range)
                continue;

            float3 Lt = toT / distT;
            float tT = saturate(1.0f - distT / al.range);
            float attenuationT = pow(tT, max(al.decay, 0.0001f));

            float NdotLt = dot(N, Lt);
            float diffuseTermT = 0.0f;
            if (gMaterial.lightingMode == 2)
            {
                float h = saturate(NdotLt * 0.5f + 0.5f);
                diffuseTermT = h * h;
            }
            else
            {
                diffuseTermT = saturate(NdotLt);
            }

            float3 tubeLightCol = al.color.rgb * al.intensity * attenuationT;

            // 線光源の影（壁の向こう側へ漏れない）
            if (al.shadowIndex >= 0)
            {
                tubeLightCol *= SampleOmniShadowDown(al.shadowIndex, input.worldPosition, N, Lt);
            }

            diffuseArea += base.rgb * tubeLightCol * diffuseTermT;

            if (currentShininess > 0.0f && NdotLt > 0.0f)
            {
                float3 Ht = normalize(Lt + V);
                float specPowT = pow(saturate(dot(N, Ht)), currentShininess);
                specularArea += tubeLightCol * specPowT;
            }
            continue;
        }

        float3 Ln = cross(R, U);
        float nLen = length(Ln);
        if (nLen < 1e-5f)
            continue;
        Ln /= nLen;

        float3 c = al.position;
        float hw = al.halfWidth;
        float hh = al.halfHeight;

        float3 corners[4] = {
            c + R * hw + U * hh,
            c + R * hw - U * hh,
            c - R * hw + U * hh,
            c - R * hw - U * hh
        };

        float3 areaLightColBase = al.color.rgb * al.intensity;

        // 面光源の影（4 隅サンプル共通。中心方向で 1 回だけ判定する）
        if (al.shadowIndex >= 0)
        {
            float3 toCenter = al.position - input.worldPosition;
            float distCenter = max(length(toCenter), 1e-5f);
            areaLightColBase *= SampleOmniShadowDown(al.shadowIndex, input.worldPosition, N, toCenter / distCenter);
        }

        // 4サンプル平均
        [unroll]
        for (int si = 0; si < 4; ++si)
        {
            float3 toL = corners[si] - input.worldPosition;
            float dist = length(toL);
            if (dist <= 1e-5f || dist >= al.range)
                continue;

            float3 La = toL / dist;

            // 片面発光チェック（twoSided==0 の時だけ）
            if (al.twoSided == 0)
            {
                // 点がライトの裏側ならスキップ
                if (dot(Ln, -La) <= 0.0f)
                    continue;
            }

            float t = saturate(1.0f - dist / al.range);
            float attenuation = pow(t, max(al.decay, 0.0001f));

            float NdotLa = dot(N, La);

            float diffuseTermA = 0.0f;
            if (gMaterial.lightingMode == 2)
            {
                float h = saturate(NdotLa * 0.5f + 0.5f);
                diffuseTermA = h * h;
            }
            else
            {
                diffuseTermA = saturate(NdotLa);
            }

            float3 areaLightCol = areaLightColBase * attenuation * 0.25f;

            diffuseArea += base.rgb * areaLightCol * diffuseTermA;

            if (currentShininess > 0.0f && NdotLa > 0.0f)
            {
                float3 H = normalize(La + V);
                float specPow = pow(saturate(dot(N, H)), currentShininess);
                specularArea += areaLightCol * specPow;
            }
        }
    }

// 合算
    // 環境光（DirectionalLight の ambientColor / ambientIntensity で制御。既定 0 = ライトの当たった所だけ見える）
    float3 ambient = base.rgb * gDirectionalLight.ambientColor * gDirectionalLight.ambientIntensity;
    output.color.rgb = ambient + (diffuseDir + diffusePoint + diffuseSpot + diffuseArea) + (specularDir + specularPoint + specularSpot + specularArea);

    // -----------------
    // シャドウマップ（平行光源の影判定）
    // -----------------
    if (gShadowParams.shadowMapEnabled)
    {
        // クリップ空間(-1〜1) から UV座標(0〜1) へ変換
        float3 projCoords = input.lightSpacePos.xyz / input.lightSpacePos.w;
        projCoords.x = projCoords.x * 0.5f + 0.5f;
        projCoords.y = -projCoords.y * 0.5f + 0.5f;

        // 範囲内かチェック
        if (projCoords.x >= 0.0f && projCoords.x <= 1.0f &&
            projCoords.y >= 0.0f && projCoords.y <= 1.0f &&
            projCoords.z >= 0.0f && projCoords.z <= 1.0f)
        {
            // 法線とライト方向の角度に応じてバイアスを変動させる (Slope-Scale Depth Bias 簡易版)
            float NdotLShadow = max(0.0f, dot(N, -gShadowParams.lightDirection));
            float currentBias = max(0.001f, gShadowParams.bias * (1.0f - NdotLShadow));

            // 3x3 の PCF で「光が当たっている率」を求める (0:完全に影, 1:影なし)
            // 比較サンプラは (参照深度 <= 格納深度) で 1 を返すため、バイアスは参照側から引く
            float litRate = SampleShadowPCF(projCoords.xy, projCoords.z - currentBias);

            // シャドウマップの端で影が唐突に切れないよう、外周へ向けてフェードさせる
            float2 fadeUV = abs(projCoords.xy * 2.0f - 1.0f); // 中心0 〜 端1
            float edgeFade = saturate((1.0f - max(fadeUV.x, fadeUV.y)) / 0.05f);
            litRate = lerp(1.0f, litRate, edgeFade);

            // 影の濃さ（連続値）。二値判定ではないため輪郭のジャギーが出ない
            float shadowFactor = (1.0f - litRate) * gShadowParams.color.a;
            output.color.rgb = lerp(output.color.rgb, output.color.rgb * gShadowParams.color.rgb, shadowFactor);
        }
    }

    // 環境マップ映り込み
    [branch]
    if (gMaterial.environmentCoefficient > 0.0f) {
        float3 cameraToPosition = normalize(input.worldPosition - gCamera.worldPosition);
        float3 reflectedVector = reflect(cameraToPosition, N);
        float4 environmentColor = gEnvironmentTexture.Sample(gSampler, reflectedVector);
        output.color.rgb += environmentColor.rgb * gMaterial.environmentCoefficient;
    }

    output.color.a = base.a;

    return output;
}
