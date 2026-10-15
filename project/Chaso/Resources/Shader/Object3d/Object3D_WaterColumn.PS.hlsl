// ============================================================================
// Object3D_WaterColumn.PS.hlsl
// ----------------------------------------------------------------------------
// 激しい水しぶき（水柱）用のピクセルシェーダー。
// 特徴:
//   - ノイズを上方向へスクロールさせ、水が勢いよく噴き上がるように見せる
//   - 円柱の上端・下端をアルファでフェードさせる
//   - フレネル反射と環境マップの映り込み
//
// PSO は object3d_watercolumn（BACK カリング）/ object3d_watercolumn_front（FRONT カリング）と、
// それぞれのインスタンシング版（_inst）。深度書き込み OFF。
// ライトは平行光源と点光源だけを使う（スポット・面光源・影は使わない）。
// ============================================================================

#include "Object3d.hlsli"

// ---------- 定数バッファ ----------

/// @brief マテリアル定数（b0）
struct Material
{
    float4 color;                 ///< 水の色味（rgb）と不透明度（a）
    int lightingMode;             ///< 未使用
    float shininess;              ///< スペキュラ指数（300 未満なら 300 に引き上げる）
    float environmentCoefficient; ///< 未使用
    float padding;                ///< パディング
    float4x4 uvTransform;         ///< UV 変換（_42 に経過時間を入れて v 方向へスクロールさせる）
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

/// @brief スポットライト 1 灯ぶんのパラメータ（並びを合わせるために宣言だけしている）
struct SpotLight
{
    float4 color;     ///< 光の色
    float3 position;  ///< 光の位置
    float intensity;  ///< 強度
    float3 direction; ///< 照射方向
    float distance;   ///< 届く距離
    float decay;      ///< 減衰（指数）
    float cosAngle;   ///< 照射範囲の境界角の cos
    float2 padding;   ///< パディング
};
static const uint MAX_SPOT_LIGHTS = 256; ///< スポットライトの最大数
/// @brief スポットライトの配列（b4。このシェーダーでは未使用）
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

/// @brief ハッシュによる簡易な疑似乱数（決定的）
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
    f = f * f * (3.0 - 2.0 * f);

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

/// @brief fBm（Fractional Brownian Motion、3 オクターブ）
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

/// @brief 水柱の色・反射・透明度を合成して出力する
/// @param input 頂点シェーダーからの補間値
/// @return 出力色（プレマルチプライド済み）
PixelShaderOutput main(VertexShaderOutput input)
{
    PixelShaderOutput output;

    // uvTransform._42（0 始まりで 3 行 1 列）を、C++ 側から渡される時間として使う
    float timeOffset = gMaterial.uvTransform[3][1];

    // テクスチャをサンプル（_42 が v の平行移動になるので UV がスクロールする）
    float4 uv = mul(float4(input.texcoord, 0.0, 1.0), gMaterial.uvTransform);
    float4 tex = gTexture.Sample(gSampler, uv.xy);
    float4 base = gMaterial.color * tex * input.instColor;

    // texcoord.y で円柱の上端・下端をフェードアウトさせる
    float v = input.texcoord.y;
    // MeshGenerator の円柱はふつう v=0 が上端、v=1 が下端
    float heightFade = smoothstep(0.0, 0.15, v) * smoothstep(1.0, 0.85, v);

    if (base.a <= 0.001 || heightFade <= 0.001)
        discard;

    // 水の色
    float3 waterDeep   = float3(0.05, 0.35, 0.65);
    float3 waterBright = float3(0.45, 0.85, 1.0);
    float3 waterFoam   = float3(0.8, 0.95, 1.0);

    // ノイズ空間でワールド座標をずらしてスクロールさせ、水が上へ噴き上がるように見せる
    float3 wp = input.worldPosition + float3(0.0, timeOffset * 15.0, 0.0);

    // ノイズを縦に引き伸ばし、勢いよく流れる筋に見せる
    float3 noisePos = float3(wp.x * 5.0, wp.y * 1.5, wp.z * 5.0);
    float noiseVal = fbm3(noisePos);

    float3 waterColor = lerp(waterDeep, waterBright, noiseVal);

    float foamMask = smoothstep(0.5, 0.75, noiseVal);
    waterColor = lerp(waterColor, waterFoam, foamMask * 0.6);

    waterColor *= base.rgb * 2.0;

    // ベクトル
    float3 N = normalize(input.normal);
    float3 V = normalize(gCamera.worldPosition - input.worldPosition);

    // 流れる水に見せるため、ノイズの勾配で法線を揺らす
    float eps = 0.05;
    float nx = fbm3(noisePos + float3(eps, 0, 0)) - fbm3(noisePos - float3(eps, 0, 0));
    float ny = fbm3(noisePos + float3(0, eps, 0)) - fbm3(noisePos - float3(0, eps, 0));
    float nz = fbm3(noisePos + float3(0, 0, eps)) - fbm3(noisePos - float3(0, 0, eps));
    float3 noiseNormal = normalize(float3(nx, ny, nz));

    N = normalize(N + noiseNormal * 0.4);

    if (dot(N, V) < 0.0)
        N = -N;

    float NdotV = saturate(dot(N, V));

    // フレネル（F0 は水のおよその値）
    float F0 = 0.02;
    float fresnel = FresnelSchlick(NdotV, F0);
    float shininess = max(gMaterial.shininess, 300.0);

    // ライティング
    float3 diffuseSum = 0.0;
    float3 specularSum = 0.0;

    // 平行光源
    {
        float3 L = normalize(-gDirectionalLight.direction);
        float NdotL = saturate(dot(N, L));
        float diffuseTerm = NdotL * NdotL;

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
        if (i >= pointCount) break;
        PointLight pl = pointLights[i];
        if (pl.intensity <= 0.0 || pl.radius <= 0.0) continue;

        float3 toL = pl.position - input.worldPosition;
        float dist = length(toL);
        if (dist >= pl.radius || dist <= 1e-5) continue;

        float3 L = toL / dist;
        float t = saturate(1.0 - dist / pl.radius);
        float atten = pow(t, max(pl.decay, 0.0001));

        float NdotL = saturate(dot(N, L));
        float diffuseTerm = NdotL * NdotL;

        float3 lightCol = pl.color.rgb * pl.intensity * atten;
        diffuseSum += waterColor * lightCol * diffuseTerm;

        if (NdotL > 0.0)
        {
            float3 H = normalize(L + V);
            float specPow = pow(saturate(dot(N, H)), shininess);
            specularSum += lightCol * specPow;
        }
    }

    // 環境マップの映り込み
    float3 reflectedVec = reflect(-V, N);
    float4 envColor = gEnvironmentTexture.Sample(gSampler, reflectedVec);

    // 表面下散乱の近似（逆光で見るときほど光が透けて明るく見える）
    float3 L_dir = normalize(-gDirectionalLight.direction);
    float sss = saturate(dot(V, -L_dir)) * 0.4;
    float3 subsurface = waterBright * gDirectionalLight.color.rgb * gDirectionalLight.intensity * sss;

    // 合成
    float3 reflection = specularSum * (0.5 + 2.0 * fresnel);
    reflection += envColor.rgb * fresnel * 0.8;

    // heightFade で上端・下端ほど不透明度を下げる
    float opacity = saturate(base.a * heightFade * 0.9 + fresnel * 0.4 * heightFade);

    float innerGlow = smoothstep(0.0, 0.5, NdotV) * 0.2;
    float3 glowColor = waterBright * innerGlow;

    float3 finalColor = diffuseSum + subsurface + glowColor;

    // 泡の強いところをさらに明るくする
    finalColor += waterFoam * foamMask * 0.5;

    output.color.rgb = saturate(finalColor * opacity + reflection);
    output.color.a = opacity;

    return output;
}
