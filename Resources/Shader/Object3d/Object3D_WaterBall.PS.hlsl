// ============================================================================
// Object3D_WaterBall.PS.hlsl
// ----------------------------------------------------------------------------
// 水弾・水しぶきパーティクル用の水球ピクセルシェーダー。
// 特徴:
//   - ワールド座標から作るプロシージャルノイズで表面を揺らす（時間は使わない）
//   - フレネル反射（本物の水のように縁が明るい）
//   - 環境マップの映り込み
//   - 表面下散乱の近似（光が水を透けて見える）
//   - 半透明を正しく重ねるため、プレマルチプライド済みのアルファで出力する
//
// PSO は object3d_water（BACK カリング）/ object3d_water_front（FRONT カリング）と、
// それぞれのインスタンシング版（_inst）。深度書き込み OFF。
// 点・スポットライトの影（シャドウアトラス）と面光源はこのシェーダーでは使わない。
// ============================================================================

#include "Object3d.hlsli"

// ---------- 定数バッファ（Object3D / Glass と同じ並び） ----------

/// @brief マテリアル定数（b0）
struct Material
{
    float4 color;                 ///< 水の色味（rgb）と不透明度（a）
    int lightingMode;             ///< 未使用（常に Half-Lambert 相当）
    float shininess;              ///< スペキュラ指数（512 未満なら 512 に引き上げる）
    float environmentCoefficient; ///< 未使用
    float padding;                ///< パディング
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

/// @brief 点光源 1 灯ぶんのパラメータ（末尾の shadowIndex は使わないので padding として読む）
struct PointLight
{
    float4 color;     ///< 光の色
    float3 position;  ///< 光の位置
    float intensity;  ///< 強度
    float radius;     ///< 届く距離（0 以下なら無効）
    float decay;      ///< 減衰（指数）
    float2 padding;   ///< パディング
};
static const uint MAX_POINT_LIGHTS = 256; ///< 点光源の最大数
/// @brief 点光源の配列（b3）
cbuffer PointLightsCB : register(b3)
{
    uint pointCount;                          ///< 有効な点光源の数
    float3 _padPoint;                         ///< パディング
    PointLight pointLights[MAX_POINT_LIGHTS]; ///< 点光源
};

/// @brief スポットライト 1 灯ぶんのパラメータ（末尾の shadowIndex は使わないので padding として読む）
struct SpotLight
{
    float4 color;     ///< 光の色
    float3 position;  ///< 光の位置
    float intensity;  ///< 強度
    float3 direction; ///< 照射方向
    float distance;   ///< 届く距離（0 以下なら無効）
    float decay;      ///< 減衰（指数）
    float cosAngle;   ///< 照射範囲の境界角の cos（これより外側は 0）
    float2 padding;   ///< パディング
};
static const uint MAX_SPOT_LIGHTS = 256; ///< スポットライトの最大数
/// @brief スポットライトの配列（b4）
cbuffer SpotLightsCB : register(b4)
{
    uint spotCount;                        ///< 有効なスポットライトの数
    float3 _padSpot;                       ///< パディング
    SpotLight spotLights[MAX_SPOT_LIGHTS]; ///< スポットライト
};

/// @brief 面光源 1 灯ぶんのパラメータ（並びを合わせるために宣言だけしている）
struct AreaLight
{
    float4 color;     ///< 光の色
    float3 position;  ///< 面の中心位置
    float intensity;  ///< 強度
    float3 right;     ///< 面の右方向
    float halfWidth;  ///< 右方向の半幅
    float3 up;        ///< 面の上方向
    float halfHeight; ///< 上方向の半高
    float range;      ///< 影響距離
    float decay;      ///< 減衰（指数）
    uint twoSided;    ///< 1 なら両面
    uint pad;         ///< パディング
};
static const uint MAX_AREA_LIGHTS = 256; ///< 面光源の最大数
/// @brief 面光源の配列（b5。このシェーダーでは未使用）
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

// ==========================================================================
// ユーティリティ関数
// ==========================================================================

/// @brief ハッシュによる簡易な疑似乱数（決定的で、時間は使わない）
/// @param p 入力座標
/// @return 0〜1 の乱数
float hash31(float3 p)
{
    p = frac(p * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return frac((p.x + p.y) * p.z);
}

/// @brief 3D バリューノイズ
/// @param p サンプル位置
/// @return 0〜1 のノイズ値
float noise3D(float3 p)
{
    float3 i = floor(p);
    float3 f = frac(p);
    f = f * f * (3.0 - 2.0 * f); // smoothstep 補間

    float n000 = hash31(i + float3(0, 0, 0));
    float n100 = hash31(i + float3(1, 0, 0));
    float n010 = hash31(i + float3(0, 1, 0));
    float n110 = hash31(i + float3(1, 1, 0));
    float n001 = hash31(i + float3(0, 0, 1));
    float n101 = hash31(i + float3(1, 0, 1));
    float n011 = hash31(i + float3(0, 1, 1));
    float n111 = hash31(i + float3(1, 1, 1));

    float nx00 = lerp(n000, n100, f.x);
    float nx10 = lerp(n010, n110, f.x);
    float nx01 = lerp(n001, n101, f.x);
    float nx11 = lerp(n011, n111, f.x);

    float nxy0 = lerp(nx00, nx10, f.y);
    float nxy1 = lerp(nx01, nx11, f.y);

    return lerp(nxy0, nxy1, f.z);
}

/// @brief 表面の細部用の fBm（Fractional Brownian Motion、3 オクターブ）
/// @param p サンプル位置
/// @return ノイズ値（おおよそ 0〜0.875）
float fbm3(float3 p)
{
    float val = 0.0;
    float amp = 0.5;
    float freq = 1.0;
    for (int i = 0; i < 3; ++i)
    {
        val += amp * noise3D(p * freq);
        freq *= 2.17;
        amp *= 0.5;
    }
    return val;
}

/// @brief Schlick 近似のフレネル反射率
/// @param cosTheta 法線と視線の内積
/// @param F0       正面から見たときの反射率
/// @return 反射率（0〜1）
float FresnelSchlick(float cosTheta, float F0)
{
    float x = 1.0 - saturate(cosTheta);
    float x2 = x * x;
    float x5 = x2 * x2 * x;
    return F0 + (1.0 - F0) * x5;
}

// ==========================================================================
// エントリポイント
// ==========================================================================

/// @brief 水球の色・反射・透明度を合成して出力する
/// @param input 頂点シェーダーからの補間値
/// @return 出力色（プレマルチプライド済み）
PixelShaderOutput main(VertexShaderOutput input)
{
    PixelShaderOutput output;

    // UV 変換＆テクスチャサンプル
    float4 uv = mul(float4(input.texcoord, 0.0, 1.0), gMaterial.uvTransform);
    float4 tex = gTexture.Sample(gSampler, uv.xy);
    float4 base = gMaterial.color * tex * input.instColor;

    if (base.a <= 0.001)
        discard;

    // ==================
    // 水の色
    // ==================
    // ベース色を、深みのある青〜シアンの水の配色で置き換える
    float3 waterDeep   = float3(0.02, 0.15, 0.45);  // 深い海の青
    float3 waterBright = float3(0.15, 0.55, 0.95);   // 明るい水面
    float3 waterFoam   = float3(0.6, 0.85, 1.0);     // 泡・ハイライトの色

    // ワールド座標でプロシージャルノイズの模様を作る
    float3 wp = input.worldPosition;
    float noiseVal = fbm3(wp * 8.0); // 高周波の細部

    // ノイズに応じて深い水と明るい水を混ぜる
    float3 waterColor = lerp(waterDeep, waterBright, noiseVal);

    // ノイズの山に、泡のような淡いハイライトを足す
    float foamMask = smoothstep(0.55, 0.75, noiseVal);
    waterColor = lerp(waterColor, waterFoam, foamMask * 0.4);

    // マテリアルの色味を掛ける（material.color で色を寄せられる）
    waterColor *= base.rgb * 2.5; // 暗めのベース色を補うため持ち上げる

    // ==================
    // ベクトル
    // ==================
    float3 N = normalize(input.normal);
    float3 V = normalize(gCamera.worldPosition - input.worldPosition);

    // さざ波に見せるため、ノイズの勾配で法線を揺らす
    float eps = 0.02;
    float nx = fbm3((wp + float3(eps, 0, 0)) * 8.0) - fbm3((wp - float3(eps, 0, 0)) * 8.0);
    float ny = fbm3((wp + float3(0, eps, 0)) * 8.0) - fbm3((wp - float3(0, eps, 0)) * 8.0);
    float nz = fbm3((wp + float3(0, 0, eps)) * 8.0) - fbm3((wp - float3(0, 0, eps)) * 8.0);
    float3 noiseNormal = normalize(float3(nx, ny, nz));

    // ノイズ法線を形状の法線に混ぜる
    N = normalize(N + noiseNormal * 0.35);

    if (dot(N, V) < 0.0)
        N = -N;

    float NdotV = saturate(dot(N, V));

    // ==================
    // フレネル（水の IOR ≒ 1.33）
    // ==================
    float ior = 1.33;
    float f = (1.0 - ior) / (1.0 + ior);
    float F0 = f * f; // 水ならおよそ 0.02
    float fresnel = FresnelSchlick(NdotV, F0);

    // 水面はなめらかなので、スペキュラ指数を高くする
    float shininess = max(gMaterial.shininess, 512.0);

    // ==================
    // ライティング
    // ==================
    float3 diffuseSum = 0.0;
    float3 specularSum = 0.0;

    // 平行光源
    {
        float3 L = normalize(-gDirectionalLight.direction);
        float NdotL = dot(N, L);
        float diffuseTerm = saturate(NdotL * 0.5 + 0.5);
        diffuseTerm *= diffuseTerm;

        float3 lightCol = gDirectionalLight.color.rgb * gDirectionalLight.intensity;
        diffuseSum += waterColor * lightCol * diffuseTerm;

        if (NdotL > 0.0)
        {
            float3 H = normalize(L + V);
            float specPow = pow(saturate(dot(N, H)), shininess);
            specularSum += lightCol * specPow;
        }
    }

    // 点光源
    [loop]
    for (uint i = 0; i < MAX_POINT_LIGHTS; ++i)
    {
        if (i >= pointCount)
            break;
        PointLight pl = pointLights[i];
        if (pl.intensity <= 0.0 || pl.radius <= 0.0)
            continue;

        float3 toL = pl.position - input.worldPosition;
        float dist = length(toL);
        if (dist >= pl.radius || dist <= 1e-5)
            continue;

        float3 L = toL / dist;
        float t = saturate(1.0 - dist / pl.radius);
        float atten = pow(t, max(pl.decay, 0.0001));

        float NdotL = dot(N, L);
        float diffuseTerm = saturate(NdotL * 0.5 + 0.5);
        diffuseTerm *= diffuseTerm;

        float3 lightCol = pl.color.rgb * pl.intensity * atten;
        diffuseSum += waterColor * lightCol * diffuseTerm;

        if (NdotL > 0.0)
        {
            float3 H = normalize(L + V);
            float specPow = pow(saturate(dot(N, H)), shininess);
            specularSum += lightCol * specPow;
        }
    }

    // スポットライト
    [loop]
    for (uint j = 0; j < MAX_SPOT_LIGHTS; ++j)
    {
        if (j >= spotCount)
            break;
        SpotLight sl = spotLights[j];
        if (sl.intensity <= 0.0 || sl.distance <= 0.0)
            continue;

        float3 toL = sl.position - input.worldPosition;
        float dist = length(toL);
        if (dist >= sl.distance || dist <= 1e-5)
            continue;

        float3 L = toL / dist;
        float t = saturate(1.0 - dist / sl.distance);
        float atten = pow(t, max(sl.decay, 0.0001));

        float3 dirN = normalize(sl.direction);
        float cosLD = dot(-L, dirN);
        float spot = saturate((cosLD - sl.cosAngle) / max(1e-5, (1.0 - sl.cosAngle)));

        float3 lightCol = sl.color.rgb * sl.intensity * atten * spot;

        float NdotL = dot(N, L);
        float diffuseTerm = saturate(NdotL * 0.5 + 0.5);
        diffuseTerm *= diffuseTerm;

        diffuseSum += waterColor * lightCol * diffuseTerm;

        if (NdotL > 0.0)
        {
            float3 H = normalize(L + V);
            float specPow = pow(saturate(dot(N, H)), shininess);
            specularSum += lightCol * specPow;
        }
    }

    // ==================
    // 環境マップの映り込み
    // ==================
    float3 cameraToSurface = normalize(input.worldPosition - gCamera.worldPosition);
    float3 reflectedVec = reflect(cameraToSurface, N);
    float4 envColor = gEnvironmentTexture.Sample(gSampler, reflectedVec);

    // ==================
    // 表面下散乱の近似
    // ==================
    // 光源が水の向こう側にある（逆光で見る）ときほど、光が透けて明るく見える
    float3 L_dir = normalize(-gDirectionalLight.direction);
    float sss = saturate(dot(V, -L_dir)) * 0.3;
    float3 subsurface = waterBright * gDirectionalLight.color.rgb * gDirectionalLight.intensity * sss;

    // ==================
    // 合成
    // ==================
    // 反射（フレネルで重み付け。縁ほど強く映る）
    float3 reflection = specularSum * (0.3 + 2.5 * fresnel);
    reflection += envColor.rgb * fresnel * 0.8;

    // 水の不透明度: 半透明で、縁ほど不透明になる（フレネル）
    float opacity = saturate(base.a * 0.7 + fresnel * 0.5);

    // 内側の光: 中心をほんのり明るくする
    float innerGlow = smoothstep(0.0, 0.6, NdotV) * 0.15;
    float3 glowColor = waterBright * innerGlow;

    // 最終色（プレマルチプライド済みアルファ。反射は不透明度で薄めない）
    float3 finalColor = diffuseSum + subsurface + glowColor;
    output.color.rgb = saturate(finalColor * opacity + reflection);
    output.color.a = opacity;

    return output;
}
