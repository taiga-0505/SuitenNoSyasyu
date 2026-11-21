#include "Object3d.hlsli"

// =============================================================================
// Glass（ガラス）
//
// 窓板やガラス容器などの半透明描画用（object3d_glass 系の PSO）。
// プレマルチプライド済みの色を出力し、PSO 側は深度書き込み OFF。
// 背面用（FRONT カリング）と表面用（BACK カリング）の PSO があり、2 パスで描ける。
//
// マテリアル定数の一部をガラス用に流用している。
//   environmentCoefficient = IOR（1〜3 の範囲外なら 1.5）
//   padding                = 粗さ roughness（0〜1。すりガラスほど大きく）
//
// ライトの合算は Object3d.PS と同じ考え方（影も同じく反映）。ただし環境光は加えない。
// フレネルで端ほど不透明・反射が強くなり、環境マップの映り込みもフレネルで掛ける。
// =============================================================================

/// @brief マテリアル定数（b0）。一部をガラス用に流用する
struct Material
{
    float4 color;                 ///< rgb = 色味、a = 不透明度
    int lightingMode;             ///< 0: なし、1: Lambert、2: Half-Lambert（ガラスでも使ってよい）
    float shininess;              ///< スペキュラ指数（ガラスは 256〜1024 くらいが映える）
    float environmentCoefficient; ///< ガラスでは IOR として使う
    float padding;                ///< ガラスでは roughness（0〜1）として使う
    float4x4 uvTransform;         ///< UV 変換
};
ConstantBuffer<Material> gMaterial : register(b0);

/// @brief 平行光源と環境光（b1）
struct DirectionalLight
{
    float4 color;           ///< 光の色
    float3 direction;       ///< 光の進む向き
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

/// @brief 点光源 1 灯ぶんのパラメータ（Object3d.PS と同じ並び）
struct PointLight
{
    float4 color;      ///< 光の色
    float3 position;   ///< 光の位置
    float intensity;   ///< 強度
    float radius;      ///< 届く距離（0 以下なら無効）
    float decay;       ///< 減衰（指数）
    int shadowIndex;   ///< 影アトラスのタイル番号（-1 で影なし）
    float padding;     ///< パディング
};
static const uint MAX_POINT_LIGHTS = 256; ///< 点光源の最大数
/// @brief 点光源の配列（b3）
cbuffer PointLightsCB : register(b3)
{
    uint pointCount;                          ///< 有効な点光源の数
    float3 _padPoint;                         ///< パディング
    PointLight pointLights[MAX_POINT_LIGHTS]; ///< 点光源
};

/// @brief スポットライト 1 灯ぶんのパラメータ（Object3d.PS と同じ並び）
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
    uint spotCount;                        ///< 有効なスポットライトの数
    float3 _padSpot;                       ///< パディング
    SpotLight spotLights[MAX_SPOT_LIGHTS]; ///< スポットライト
};

/// @brief 面光源 1 灯ぶんのパラメータ（Object3d.PS と同じ並び）
struct AreaLight
{
    float4 color;     ///< 光の色
    float3 position;  ///< 面の中心位置
    float intensity;  ///< 強度
    float3 right;     ///< 面の右方向
    float halfWidth;  ///< 右方向の半幅
    float3 up;        ///< 面の上方向
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
    float3 _padArea;                       ///< パディング
    AreaLight areaLights[MAX_AREA_LIGHTS]; ///< 面光源
};

Texture2D<float4> gTexture : register(t0);
TextureCube<float4> gEnvironmentTexture : register(t1);
SamplerState gSampler : register(s0);

struct PixelShaderOutput
{
    float4 color : SV_TARGET0; ///< 出力色（プレマルチプライド済み）
};

/// @brief 拡散項を求める（Half-Lambert / Lambert の共通化）
/// @param NdotL        法線とライト方向の内積
/// @param lightingMode 2 なら Half-Lambert、それ以外は Lambert
/// @return 拡散の強さ（0〜1）
float CalcDiffuseTerm(float NdotL, int lightingMode)
{
    if (lightingMode == 2)
    {
        float h = saturate(NdotL * 0.5f + 0.5f);
        return h * h;
    }
    return saturate(NdotL);
}

/// @brief Schlick 近似のフレネル反射率
/// @param cosTheta 法線と視線の内積
/// @param F0       正面から見たときの反射率
/// @return 反射率（0〜1）
float FresnelSchlick(float cosTheta, float F0)
{
    float x = 1.0f - saturate(cosTheta);
    float x5 = x * x * x * x * x;
    return F0 + (1.0f - F0) * x5;
}

/// @brief ガラスの透過色と反射を合成して出力する
/// @param input 頂点シェーダーからの補間値
/// @return 出力色（プレマルチプライド済み）
PixelShaderOutput main(VertexShaderOutput input)
{
    PixelShaderOutput output;

    // UV変換 & サンプル
    float4 uv = mul(float4(input.texcoord, 0.0f, 1.0f), gMaterial.uvTransform);
    float4 tex = gTexture.Sample(gSampler, uv.xy);

    float4 base = gMaterial.color * tex * input.instColor;

    // ガラスは cutout しない（完全透明だけ捨てる）
    if (base.a <= 0.001f)
        discard;

    // ライティング無しなら透明色だけ（※プレマルチ前提）
    if (gMaterial.lightingMode == 0)
    {
        output.color.rgb = base.rgb * base.a;
        output.color.a = base.a;
        return output;
    }

    // ==========
    // 共通ベクトル
    // ==========
    float3 N = normalize(input.normal);
    float3 V = normalize(gCamera.worldPosition - input.worldPosition);

    // 2-sided用：裏面に回ったら法線反転（窓板とかで重要）
    if (dot(N, V) < 0.0f)
        N = -N;

    float NdotV = saturate(dot(N, V));

    // ==========
    // フレネル
    // ==========
    // Glassでは environmentCoefficient を IORに、padding を roughness に流用
    float iorRaw = gMaterial.environmentCoefficient;
    float ior = (iorRaw > 1.0f && iorRaw < 3.0f) ? iorRaw : 1.5f; // 範囲外なら既定値 1.5
    float f = (1.0f - ior) / (1.0f + ior);
    float F0 = f * f; // ガラスだとだいたい 0.04 付近
    float fresnel = FresnelSchlick(NdotV, F0);

    float rough = saturate(gMaterial.padding);

    // roughness→指数（rough=0で鋭い、rough=1で広い）
    float autoSh = lerp(1024.0f, 64.0f, rough * rough);

    // Material側が大きい値ならそれを優先、低いならautoを使う
    float shininess = max(gMaterial.shininess, autoSh);

    // ==========
    // ライト合算（元のObject3Dと同じ考え方）
    // ==========
    float3 diffuseSum = 0.0f;
    float3 specularSum = 0.0f;

    // ---- 平行光源
    {
        float3 L = normalize(-gDirectionalLight.direction);
        float NdotL = dot(N, L);
        float diffuseTerm = CalcDiffuseTerm(NdotL, gMaterial.lightingMode);

        float3 lightCol = gDirectionalLight.color.rgb * gDirectionalLight.intensity;

        diffuseSum += base.rgb * lightCol * diffuseTerm;

        if (NdotL > 0.0f)
        {
            float3 H = normalize(L + V);
            float specPow = pow(saturate(dot(N, H)), shininess);
            specularSum += lightCol * specPow;
        }
    }

    // ---- 点光源（最大 MAX_POINT_LIGHTS 灯）
    [loop]
    for (uint i = 0; i < MAX_POINT_LIGHTS; ++i)
    {
        if (i >= pointCount)
            break;

        PointLight pl = pointLights[i];
        if (pl.intensity <= 0.0f || pl.radius <= 0.0f)
            continue;

        float3 toL = pl.position - input.worldPosition;
        float dist = length(toL);
        if (dist >= pl.radius || dist <= 1e-5f)
            continue;

        float3 L = toL / dist;

        float t = saturate(1.0f - dist / pl.radius);
        float atten = pow(t, max(pl.decay, 0.0001f));

        float NdotL = dot(N, L);
        float diffuseTerm = CalcDiffuseTerm(NdotL, gMaterial.lightingMode);

        float3 lightCol = pl.color.rgb * pl.intensity * atten;

        // 点光源の影（壁の向こう側へ漏れない）
        if (pl.shadowIndex >= 0)
        {
            lightCol *= SampleOmniShadowDown(pl.shadowIndex, input.worldPosition, N, L);
        }

        diffuseSum += base.rgb * lightCol * diffuseTerm;

        if (NdotL > 0.0f)
        {
            float3 H = normalize(L + V);
            float specPow = pow(saturate(dot(N, H)), shininess);
            specularSum += lightCol * specPow;
        }
    }

    // ---- スポットライト（最大 MAX_SPOT_LIGHTS 灯）
    [loop]
    for (uint i = 0; i < MAX_SPOT_LIGHTS; ++i)
    {
        if (i >= spotCount)
            break;

        SpotLight sl = spotLights[i];
        if (sl.intensity <= 0.0f || sl.distance <= 0.0f)
            continue;

        float3 toL = sl.position - input.worldPosition;
        float dist = length(toL);
        if (dist >= sl.distance || dist <= 1e-5f)
            continue;

        float3 L = toL / dist;

        float t = saturate(1.0f - dist / sl.distance);
        float atten = pow(t, max(sl.decay, 0.0001f));

        float3 dirN = normalize(sl.direction);
        float cosLD = dot(-L, dirN);
        float spot = saturate((cosLD - sl.cosAngle) / max(1e-5f, (1.0f - sl.cosAngle)));
        if (spot <= 0.0f)
            continue;

        float3 lightCol = sl.color.rgb * sl.intensity * atten * spot;

        // スポット影（壁の向こう側へ漏れない）
        if (sl.shadowIndex >= 0)
        {
            lightCol *= SampleSpotShadow(sl.shadowIndex, input.worldPosition, N, L);
        }

        float NdotL = dot(N, L);
        float diffuseTerm = CalcDiffuseTerm(NdotL, gMaterial.lightingMode);

        diffuseSum += base.rgb * lightCol * diffuseTerm;

        if (NdotL > 0.0f)
        {
            float3 H = normalize(L + V);
            float specPow = pow(saturate(dot(N, H)), shininess);
            specularSum += lightCol * specPow;
        }
    }

    // ---- 面光源（元の擬似 4 サンプルそのまま）
    const uint aCount = min(areaCount, MAX_AREA_LIGHTS);
    [loop]
    for (uint ai = 0; ai < aCount; ++ai)
    {
        AreaLight al = areaLights[ai];
        if (al.intensity <= 0.0f || al.range <= 0.0f)
            continue;

        float3 R = al.right;
        float3 U = al.up;
        float rLen = length(R);
        float uLen = length(U);
        if (rLen < 1e-5f || uLen < 1e-5f)
            continue;
        R /= rLen;
        U /= uLen;

        // チューブ（線）ライト: halfHeight <= 0 なら right 方向の線分から全方位に光る線光源（Object3d.PS と同じ近似）
        if (al.halfHeight <= 0.0f)
        {
            float3 segA = al.position - R * al.halfWidth;
            float3 segAB = R * (2.0f * al.halfWidth);
            float segLen2 = max(dot(segAB, segAB), 1e-6f);
            float tSeg = saturate(dot(input.worldPosition - segA, segAB) / segLen2);
            float3 toT = (segA + segAB * tSeg) - input.worldPosition;
            float distT = length(toT);
            if (distT <= 1e-5f || distT >= al.range)
                continue;

            float3 Lt = toT / distT;
            float attenT = pow(saturate(1.0f - distT / al.range), max(al.decay, 0.0001f));
            float NdotLt = dot(N, Lt);
            float diffuseTermT = CalcDiffuseTerm(NdotLt, gMaterial.lightingMode);
            float3 lightColT = al.color.rgb * al.intensity * attenT;

            // 線光源の影（壁の向こう側へ漏れない）
            if (al.shadowIndex >= 0)
            {
                lightColT *= SampleOmniShadowDown(al.shadowIndex, input.worldPosition, N, Lt);
            }

            diffuseSum += base.rgb * lightColT * diffuseTermT;
            if (NdotLt > 0.0f)
            {
                float3 Ht = normalize(Lt + V);
                float specPowT = pow(saturate(dot(N, Ht)), shininess);
                specularSum += lightColT * specPowT;
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

        float3 corners[4] =
        {
            c + R * hw + U * hh,
            c + R * hw - U * hh,
            c - R * hw + U * hh,
            c - R * hw - U * hh
        };

        float3 lightBase = al.color.rgb * al.intensity;

        // 面光源の影（4 隅サンプル共通。中心方向で 1 回だけ判定する）
        if (al.shadowIndex >= 0)
        {
            float3 toCenter = al.position - input.worldPosition;
            float distCenter = max(length(toCenter), 1e-5f);
            lightBase *= SampleOmniShadowDown(al.shadowIndex, input.worldPosition, N, toCenter / distCenter);
        }

        [unroll]
        for (int si = 0; si < 4; ++si)
        {
            float3 toL = corners[si] - input.worldPosition;
            float dist = length(toL);
            if (dist <= 1e-5f || dist >= al.range)
                continue;

            float3 L = toL / dist;

            if (al.twoSided == 0)
            {
                if (dot(Ln, -L) <= 0.0f)
                    continue;
            }

            float t = saturate(1.0f - dist / al.range);
            float atten = pow(t, max(al.decay, 0.0001f));

            float NdotL = dot(N, L);
            float diffuseTerm = CalcDiffuseTerm(NdotL, gMaterial.lightingMode);

            float3 lightCol = lightBase * atten * 0.25f;

            diffuseSum += base.rgb * lightCol * diffuseTerm;

            if (NdotL > 0.0f)
            {
                float3 H = normalize(L + V);
                float specPow = pow(saturate(dot(N, H)), shininess);
                specularSum += lightCol * specPow;
            }
        }
    }

    // ==========
    // ガラス合成（重要）
    // ==========
    // 不透明度：素材a + (端で少し増える) + (曇りで増える)
    float opacity = saturate(base.a + fresnel * 0.15f + rough * 0.25f);

    // 反射：フレネルで端が強く、roughで弱く
    float specScale = lerp(1.0f, 0.25f, rough);
    float3 reflection = specularSum * specScale * (0.2f + 2.0f * fresnel);

    // 環境マップ反射（ガラスでもフレネル係数で映り込み）
    {
        float3 cameraToPosition = normalize(input.worldPosition - gCamera.worldPosition);
        float3 reflectedVector = reflect(cameraToPosition, N);
        float4 envColor = gEnvironmentTexture.Sample(gSampler, reflectedVector);
        reflection += envColor.rgb * fresnel * specScale;
    }

    // 透過っぽい“にじみ”：すりガラスほど増える（超控えめ）
    float scatter = lerp(0.02f, 0.15f, rough);
    float3 transmissionLit = diffuseSum * scatter;

    // プレマルチ出力：rgbは alpha を掛けた“透過色” + 反射（反射はαで薄まらない）
    output.color.rgb = saturate(base.rgb * opacity + transmissionLit * opacity + reflection);
    output.color.a = opacity;

    return output;
}
