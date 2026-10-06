// Water.PS.hlsl - Pixel Shader for ocean/water surface
// Fresnel reflection, scrolling normals, depth-based color

#include "Water.hlsli"

// b0: マテリアル（Object3D と同じレイアウト）
struct Material
{
    float4   color;
    int      lightingMode;
    float    shininess;
    float    environmentCoefficient;
    float    padding;
    float4x4 uvTransform;
};

ConstantBuffer<Material> gMaterial : register(b0);

// b1: ディレクショナルライト
struct DirectionalLight
{
    float4 color;
    float3 direction;
    float  intensity;
    float3 ambientColor;    // 環境光の色
    float ambientIntensity; // 環境光の強さ（0 で環境光なし）
};

ConstantBuffer<DirectionalLight> gDirectionalLight : register(b1);

// b2: カメラ
struct Camera
{
    float3 worldPosition;
};

ConstantBuffer<Camera> gCamera : register(b2);

// b6: 水面パラメータ (VS と共有)
cbuffer WaterParams : register(b6)
{
    float  gTime;
    float  gWaveHeight;
    float  gWaveSpeed;
    float  gWaveFreq;

    float  gWaveHeight2;
    float  gWaveSpeed2;
    float  gWaveFreq2;
    float  gWaveSteepness;

    float4 gWaterShallowColor;
    float4 gWaterDeepColor;

    float  gFresnelPower;
    float  gSpecularPower;
    float  gNormalScrollSpeed;
    float  gNormalStrength;

    float4 gInvScreenSize;
    float4 gCameraNearFar; // x: near, y: far
    float4 gFoamParams;    // x: foamDepth, y: foamScale
    float4 gFoamColor;

    float4 gObstacles[4];  // xyz: pos, w: radius
    float4 gObstacleCount; // x: count

    float4 gOceanParams;   // x: 詳細波の強さ, y: 尖り(choppy), z: 白波の強さ, w: 頂点で変位させる最短波長(m)
    float4 gOceanParams2;  // x: 透明度(m), y: 遠景フェード距離(m), z: 法線マップのタイルサイズ(m, 0=従来UV), w: 白波の量
    float4 gSssColor;      // rgb: 波頭を透ける光の色, a: 強さ
    float4 gRefractParams; // x: 屈折が有効(1/0), y: 歪みの強さ(画面UV), z: 交差部のフェード幅(m)
};

#include "OceanWaves.hlsli"

// t0: テクスチャ（法線マップとしても使用可能）
Texture2D<float4> gTexture : register(t0);

// t1: 環境キューブマップ (反射用)
TextureCube<float4> gEnvironmentTexture : register(t1);

// t4: インタラクティブ波紋ハイトマップ（r: 高さ, g: 引き波の泡, b: 波頭の白波）
Texture2D<float4> gInteractiveWave : register(t4);

// t5: Foam用の深度テクスチャ
Texture2D<float> gDepthTexture : register(t5);

// t6: 水を描く直前の画面のコピー（屈折用。gRefractParams.x が 1 のときだけ有効）
Texture2D<float4> gSceneColor : register(t6);

SamplerState gSampler : register(s0);
SamplerState gSamplerClamp : register(s2);

struct PixelShaderOutput
{
    float4 color : SV_TARGET0;
};

// =====================================================
// マウス波紋（インタラクティブ波）のライティング
// =====================================================
// 波紋の高さは数 cm、傾きは多くの所で 2〜3°しかない。真上視点・ほぼ真上の太陽だと
// N・L も フレネル もほとんど変わらず、形は動くのに光り方が変わらない。
// そこで「光の計算に使う傾き」だけを強め、次の 2 つで光の変化として見せる。
//   ・太陽側の斜面を明るく、反対側を暗く（ライトの水平方向で決まるので、ライトを回せば陰影も回る）
//   ・水底のコースティクス（波紋の山がレンズになって光を集め、谷で散らす）
static const float kRippleNormalGain = 4.0;  // 小さな波紋の傾きを法線へ反映する倍率（頂点の変位は変えない）
static const float kRippleMaxSlope   = 0.35; // 倍率をかけた傾きの頭打ち（tan ≈ 19°）。急な斜面は本来の傾きのまま
static const float kRippleSunShade   = 0.9;  // 太陽側/反対側の斜面の明暗の強さ（0 で無効）
static const float kRippleShadeMin   = 0.7;  // 斜面の陰影で暗くなる下限（真上の太陽では実際 2 割程度しか暗くならない）
static const float kRippleShadeMax   = 1.3;  // 斜面の陰影で明るくなる上限
static const float kCausticFocus     = 0.75; // 集光の強さ。物理値は 1-1/1.33 ≈ 0.25（波紋が低いので誇張）
static const float kCausticMax       = 2.5;  // 集光で明るくなる上限
static const float kCausticMin       = 0.6;  // 光が散って暗くなる下限（溝の真下が黒く抜けないように）

// 波紋の傾き s（= -∇h）に倍率をかけ、大きくなるほど頭打ちにする。
//   小さな傾き : ほぼ s * kRippleNormalGain（細かい波紋でも光が動いて見える）
//   大きな傾き : kRippleMaxSlope に近づく。ただし本来の傾きより小さくはしない
float2 ShapeRippleSlope(float2 s)
{
    float m = length(s);
    if (m < 1e-6) {
        return s;
    }
    float g = m * kRippleNormalGain;
    float limited = g / sqrt(1.0 + (g / kRippleMaxSlope) * (g / kRippleMaxSlope));
    return s * (max(limited, m) / m);
}

// =====================================================
// ノーマルマップから法線を取得（スクロール合成）
// =====================================================
float3 GetScrolledNormal(float2 uv, float time, float3 geometryNormal, float strength)
{
    float scrollSpeed = gNormalScrollSpeed;

    // 2層のUVスクロール（異なる速度・方向で重ね合わせ）
    float2 uv1 = uv * 4.0 + float2(scrollSpeed * time * 0.6, scrollSpeed * time * 0.4);
    float2 uv2 = uv * 6.0 + float2(-scrollSpeed * time * 0.3, scrollSpeed * time * 0.7);

    // テクスチャから法線マップを取得（0..1 → -1..1）
    float3 n1 = gTexture.Sample(gSampler, uv1).xyz * 2.0 - 1.0;
    float3 n2 = gTexture.Sample(gSampler, uv2).xyz * 2.0 - 1.0;

    // 2層の法線をブレンド
    float3 blended = normalize(n1 + n2);

    // 強度を調整
    blended.xy *= strength;
    blended = normalize(blended);

    // 法線空間 → ワールド空間のマッピング（簡易版）
    // geometryNormal を up として、TBN を構築
    float3 N = normalize(geometryNormal);
    float3 T = normalize(cross(N, float3(0, 0, 1)));
    if (length(T) < 0.001)
        T = normalize(cross(N, float3(1, 0, 0)));
    float3 B = cross(N, T);

    return normalize(T * blended.x + B * blended.y + N * blended.z);
}

// =====================================================
// 詳細波の傾きと水平変位の偏微分（ピクセルごとに解析的に計算）
// =====================================================
// 頂点では表せない短い波も、ここでは傾き（法線）として描ける。
// footprint は 1 ピクセルが覆うワールド幅。波長がそれに近い波は
// チラつき（エイリアス）の元なので弱める。
struct DetailSurface
{
    float2 slope;    // (dH/dx, dH/dz)
    float3 jacobian; // (dDx/dx, dDz/dz, dDx/dz)
};

DetailSurface EvaluateDetailSurface(float2 xz, float time, float footprint)
{
    DetailSurface o;
    o.slope = float2(0, 0);
    o.jacobian = float3(0, 0, 0);

    float detail = gOceanParams.x;
    if (detail <= 0.0) {
        return o;
    }

    [unroll]
    for (int i = 0; i < kDetailWaveCount; ++i)
    {
        DetailWave w = GetDetailWave(i, gWaveFreq, gWaveSpeed, gWaveHeight, detail, gOceanParams.y);
        float lambda = kTwoPi / w.k;
        float aa = smoothstep(footprint * 2.0, footprint * 6.0, lambda);
        float ph = DetailPhase(w, xz, time);
        float s = sin(ph);
        float c = cos(ph);

        o.slope += w.dir * (w.amp * w.k * c * aa);

        float wa = w.qa * w.k * s * aa;
        o.jacobian.x -= wa * w.dir.x * w.dir.x;
        o.jacobian.y -= wa * w.dir.y * w.dir.y;
        o.jacobian.z -= wa * w.dir.x * w.dir.y;
    }
    return o;
}

// =====================================================
// GGX スペキュラ（太陽のギラつき）
// =====================================================
float SpecularGGX(float3 N, float3 V, float3 L, float alpha, float F0)
{
    float3 H = normalize(L + V);
    float NdotL = saturate(dot(N, L));
    float NdotV = max(dot(N, V), 1e-3);
    float NdotH = saturate(dot(N, H));
    float VdotH = saturate(dot(V, H));

    float a2 = alpha * alpha;
    float d  = NdotH * NdotH * (a2 - 1.0) + 1.0;
    float D  = a2 / (3.14159265 * d * d);

    float k   = alpha * 0.5;
    float vis = 0.25 / ((NdotL * (1.0 - k) + k) * (NdotV * (1.0 - k) + k));

    float F = F0 + (1.0 - F0) * pow(1.0 - VdotH, 5.0);

    return D * vis * F * NdotL;
}

// =====================================================
// 深度値をリニアに変換（近・遠クリップ平面を使用）
// =====================================================
float LinearizeDepth(float depth, float nearZ, float farZ)
{
    // D3D12 (Z: 0 to 1, Reversed-Zではない標準プロジェクションを想定)
    // プロジェクションの設定によっては Reversed-Z を考慮する必要がある場合があります。
    // 今回は標準の投影として扱います。
    return (nearZ * farZ) / (farZ - depth * (farZ - nearZ));
}

PixelShaderOutput main(VertexShaderOutput input)
{
    PixelShaderOutput output;

    float3 V = normalize(gCamera.worldPosition - input.worldPosition);

    // =========================
    // 深度計算（Zテスト代用＆岸からの距離）
    // =========================
    float2 screenUV = input.position.xy * gInvScreenSize.xy;
    float sceneDepthNonLinear = gDepthTexture.SampleLevel(gSamplerClamp, screenUV, 0).r;
    float sceneDepth = LinearizeDepth(sceneDepthNonLinear, gCameraNearFar.x, gCameraNearFar.y);
    float pixelDepth = LinearizeDepth(input.position.z, gCameraNearFar.x, gCameraNearFar.y);
    float depthDiff = sceneDepth - pixelDepth;

    if (depthDiff < 0.0f) {
        discard;
    }

    // 法線計算（ジオメトリ法線）
    float3 geoNormal = normalize(input.normal);

    // =========================
    // 跳ね返り波（法線のうねり）の計算
    // =========================
    // ddx, ddy を用いてスクリーン空間での水深の変化量とワールド座標の変化量を取得
    float dDepthX = ddx(depthDiff);
    float dDepthY = ddy(depthDiff);
    float3 dPosW_X = ddx(input.worldPosition);
    float3 dPosW_Y = ddy(input.worldPosition);

    // 水深が深くなる方向（岩から離れる方向＝沖）のワールド空間ベクトルを計算
    float3 depthGradient = normalize(dDepthX * dPosW_X + dDepthY * dPosW_Y + float3(0, 0.0001f, 0));
    depthGradient.y = 0.0f; // 水平方向のみにする
    depthGradient = normalize(depthGradient + float3(0, 0.0001f, 0));

    // 岸からの跳ね返り波の位相（泡の計算と同じ）
    float returnWaveFreq = 4.0f;
    float returnWaveSpeed = 3.5f;
    float returnWavePhase = depthDiff * returnWaveFreq + gTime * returnWaveSpeed;

    // 岸辺に近いほど波を強くする
    float foamDepthThreshold = max(gFoamParams.x, 0.001f);
    float foamFade = saturate(depthDiff / foamDepthThreshold);
    float foamBaseIntensity = pow(1.0f - foamFade, 1.5f);

    // 跳ね返り波による法線の傾き（cos波でうねりを表現）
    float returnSlope = cos(returnWavePhase) * 0.5f * foamBaseIntensity;

    // 跳ね返り方向（沖方向）に法線を傾ける
    float3 bounceNormal = depthGradient * returnSlope;

    // 波紋ハイトマップの法線計算
    float2 waveUV = (input.worldPosition.xz / 100.0f) + 0.5f;
    float texelSize = 1.0f / 256.0f;
    float hL = gInteractiveWave.Sample(gSamplerClamp, waveUV + float2(-texelSize, 0)).r;
    float hR = gInteractiveWave.Sample(gSamplerClamp, waveUV + float2(texelSize, 0)).r;
    float hD = gInteractiveWave.Sample(gSamplerClamp, waveUV + float2(0, -texelSize)).r;
    float hU = gInteractiveWave.Sample(gSamplerClamp, waveUV + float2(0, texelSize)).r;
    // r: 中心の高さ / g: 引き波 / b: 波頭。範囲外（UV が 0..1 の外）はクランプで端の値になるので 0 に落とす
    float4 waveCenter = gInteractiveWave.Sample(gSamplerClamp, waveUV);
    float inWaveArea = (all(waveUV > 0.0) && all(waveUV < 1.0)) ? 1.0 : 0.0;
    float2 foamSample = waveCenter.gb * inWaveArea;
    float wakeFoam  = foamSample.x;
    float crestFoam = foamSample.y;

    // 波紋の法線（光の計算用に傾きを強める。ShapeRippleSlope 参照）と、集光に使う曲率（ラプラシアン）
    float texelWorld = 100.0f * texelSize;
    float2 rippleSlope = float2(hL - hR, hD - hU) / (2.0f * texelWorld) * inWaveArea;
    rippleSlope = ShapeRippleSlope(rippleSlope);
    float3 interactiveNormal = normalize(float3(rippleSlope.x, 1.0f, rippleSlope.y));
    float rippleLaplacian = (hL + hR + hU + hD - 4.0f * waveCenter.r) / (texelWorld * texelWorld) * inWaveArea;

    // ジオメトリ法線 + 波紋法線 + 跳ね返り法線
    geoNormal = normalize(geoNormal + (interactiveNormal - float3(0, 1, 0)) + bounceNormal);

    // =========================
    // 詳細波（ピクセルごとの解析法線）
    // =========================
    // 高さ場 y = h(x,z) の法線は (-hx, 1, -hz)。主波の法線を傾きに戻し、詳細波の傾きを足す。
    float camDist = distance(gCamera.worldPosition, input.worldPosition);
    float distT = (gOceanParams2.y > 0.0) ? saturate(camDist / gOceanParams2.y) : 0.0; // 0: 近景, 1: 遠景
    float2 fw = fwidth(input.restXZ);
    float footprint = max(max(fw.x, fw.y), 1e-4);
    DetailSurface detailSurf = EvaluateDetailSurface(input.restXZ, gTime, footprint);
    {
        float ny = max(geoNormal.y, 0.2);
        float2 slope = geoNormal.xz / -ny + detailSurf.slope;
        geoNormal = normalize(float3(-slope.x, 1.0, -slope.y));
    }

    // ノイズテクスチャのスクロール法線と合成
    // gOceanParams2.z > 0 ならワールド座標で敷き詰める（平面の大きさに関係なく同じ細かさになる）
    float2 normalUV = (gOceanParams2.z > 0.0)
        ? input.worldPosition.xz / (gOceanParams2.z * 4.0)
        : input.texcoord;
    // 遠景では細かい法線を弱める（タイルの繰り返しとギラギラしたチラつきを抑える）
    float normalStrength = gNormalStrength * lerp(1.0, 0.35, distT);
    float3 N = GetScrolledNormal(normalUV, gTime, geoNormal, normalStrength);

    // =========================
    // さざ波のムラ（真上視点のシーンだけ。crestTint > 0 のとき）
    // =========================
    // 法線マップは uv*4 / uv*6 で敷き詰めているので、真上から見ると細かい白い照り返しが
    // 画面全体に同じ密度で並び、模様が均一すぎて不自然に見える。
    // 同じ法線マップをずっと粗いスケールで引いて「風が当たってざわつく所 / 凪いで鏡のような所」の
    // マスクを作り、凪の所では細かい法線を弱める。照り返し・映り込みもそれに合わせて疎密が出る。
    float roughMask = 1.0;
    if (gFoamParams.z > 0.0)
    {
        float2 m1 = input.texcoord * 1.3 + float2(gTime * 0.004, gTime * 0.003);
        float2 m2 = input.texcoord * 2.1 + float2(-gTime * 0.003, gTime * 0.005);
        float n = gTexture.Sample(gSampler, m1).x * 0.6 + gTexture.Sample(gSampler, m2).y * 0.4;
        roughMask = lerp(0.12, 1.0, smoothstep(0.42, 0.62, n));
        N = normalize(lerp(geoNormal, N, roughMask));
    }

    // =========================
    // フレネル効果
    // =========================
    float NdotV = saturate(dot(N, V));
    // Schlick 近似: F = F0 + (1 - F0) * (1 - cosTheta)^5
    float F0 = 0.02; // 水の屈折率から算出される反射率
    float fresnel = F0 + (1.0 - F0) * pow(1.0 - NdotV, gFresnelPower);

    // =========================
    // 水の色（深度ベース）
    // =========================
    // フレネル値をそのまま深さの指標として使い、
    // 正面が浅瀬色、掠める角度が深海色にブレンド
    float4 waterColor = lerp(gWaterShallowColor, gWaterDeepColor, 1.0 - NdotV);
    waterColor *= gMaterial.color; // マテリアル色を乗算

    // 透明度（gOceanParams2.x, m）：水面の奥にある物体が近いほど浅瀬色に寄せ、透かす。
    // 光は水中で exp(-距離/透明度) で減衰する（Beer-Lambert）。奥に何も無い外洋では depthDiff が
    // 遠クリップ近くまで伸びるので効果は 0 になり、従来どおりの色になる。
    const bool refractionOn = (gRefractParams.x > 0.5);
    if (gOceanParams2.x > 0.0 && !refractionOn) // 屈折ありのときは下の屈折の合成で同じことをする
    {
        float shallowness = exp(-max(depthDiff, 0.0) / gOceanParams2.x);
        waterColor.rgb = lerp(waterColor.rgb, gWaterShallowColor.rgb * gMaterial.color.rgb, shallowness * 0.6);
        waterColor.a   = lerp(waterColor.a, waterColor.a * 0.4, shallowness);
    }

    // =========================
    // 波の高さによる色付け（gFoamParams.z = crestTint、0 で無効）
    // =========================
    // 上の lerp は視線と法線の角度だけで色を決めるため、真上から見下ろすと
    // NdotV がほぼ 1 で固定され、うねりが色にまったく出ない（一枚のベタ塗りになる）。
    // 真上視点のシーン（タイトル）では頂点の変位で谷を深海色へ、山を明るく振る。
    float crestTint = gFoamParams.z;
    float crestT = 0.0;
    if (crestTint > 0.0)
    {
        float amplitude = max(gWaveHeight + gWaveHeight2 + gWaveHeight * 0.25
                              + DetailAmplitudeSum(gWaveFreq, gWaveHeight, gOceanParams.x), 0.001);
        crestT = smoothstep(0.0, 1.0, saturate(input.waveHeight / amplitude * 0.5 + 0.5)); // 0: 谷, 1: 山
        float3 troughColor = lerp(waterColor.rgb, gWaterDeepColor.rgb, crestTint * 0.85);
        float3 crestColor  = waterColor.rgb * (1.0 + crestTint * 0.35);
        waterColor.rgb = lerp(troughColor, crestColor, crestT);

        // 浅瀬（真上視点のシーンだけ）：水底が近いほど明るく・透き通らせる。
        // depthDiff は「水面 → その画素の奥にある不透明物」までの距離（真上視点ならほぼ水深）。
        // 水底が無い所は depthDiff が遠クリップ近くまで伸びるので shallowT = 0 になり、従来どおりの色。
        const float kShallowVisibility = 16.0; // この深さ（m）より深いと浅瀬の効果が消える
        float shallowT = 1.0 - saturate(depthDiff / kShallowVisibility);
        shallowT = shallowT * shallowT * (3.0 - 2.0 * shallowT);
        waterColor.rgb = lerp(waterColor.rgb, gWaterShallowColor.rgb * 1.25, shallowT * 0.5);
        waterColor.a = lerp(waterColor.a, waterColor.a * 0.45, shallowT);
    }

    // =========================
    // 環境マップ反射
    // =========================
    float3 reflectedDir = reflect(-V, N);
    // 波の裏側で反射ベクトルが下を向くと、スカイボックスの地面側（暗い色）を拾って黒い斑点になる。
    // 実際には別の波面に当たって空を映すので、水平より上に折り返しておく。
    reflectedDir.y = max(reflectedDir.y, 0.02);
    reflectedDir = normalize(reflectedDir);
    float4 envColor = gEnvironmentTexture.Sample(gSampler, reflectedDir);

    // =========================
    // ライティング (簡易)
    // =========================
    float3 L = normalize(-gDirectionalLight.direction);
    float NdotL = saturate(dot(N, L));

    // Half-Lambert
    float diffuse = saturate(NdotL * 0.5 + 0.5);
    diffuse *= diffuse;

    float3 lightColor = gDirectionalLight.color.rgb * gDirectionalLight.intensity;

    // スペキュラ (GGX)
    // Specular Power（Blinn-Phong の指数）を GGX の粗さへ換算して互換を保つ: alpha = √(2/(n+2))
    // 遠景では 1 ピクセルに多数の波面が入るので粗さを上げる → 水平線に向かって太陽の光の道が伸びる
    float alpha = sqrt(2.0 / (max(gSpecularPower, 1.0) + 2.0));
    alpha = lerp(alpha, max(alpha, 0.22), distT);
    alpha = clamp(alpha, 0.02, 1.0);
    float specular = min(SpecularGGX(N, V, L, alpha, F0), 16.0);

    // =========================
    // サブサーフェススキャタリング（波頭を透ける光）
    // =========================
    // 太陽の方を向いて波を見ると、薄くなった波頭を光が通り抜けて明るい青緑に光る。
    //   crest   : 波の高い所ほど薄く透けやすい
    //   facing  : 視線が太陽の方向（水平成分）を向いているほど強い
    //   grazing : 波面を斜めから見ているほど強い（真上視点でもわずかに残す）
    float sss = 0.0;
    if (gSssColor.a > 0.0)
    {
        float ampAll = max(gWaveHeight * 1.25 + gWaveHeight2
                           + DetailAmplitudeSum(gWaveFreq, gWaveHeight, gOceanParams.x), 0.001);
        float crest = saturate(input.waveHeight / ampAll * 0.5 + 0.5);
        float2 vh = -V.xz;
        float2 lh = L.xz;
        float facing = (dot(vh, vh) > 1e-6 && dot(lh, lh) > 1e-6)
            ? saturate(dot(normalize(vh), normalize(lh))) : 0.0;
        float sunLow = saturate(1.0 - L.y * 0.7); // 太陽が低いほど横から抜ける
        float grazing = lerp(0.3, 1.0, 1.0 - NdotV);
        sss = gSssColor.a * crest * crest * (0.2 + 1.6 * pow(facing, 4.0) * sunLow) * grazing;
    }

    // =========================
    // 最終合成
    // =========================
    // 波紋の陰影：法線の水平成分がライトの水平方向を向いている斜面ほど明るい。
    // 真上からの光では N・L の変化は傾きの 2 乗でしか効かないため、1 次の項として別に足す。
    float2 sunHorizontal = L.xz;
    float  sunHorizontalLen = length(sunHorizontal);
    float2 sunDirXZ = (sunHorizontalLen > 1e-4) ? sunHorizontal / sunHorizontalLen : float2(0, 0);
    float  rippleShade = clamp(1.0 + dot(interactiveNormal.xz, sunDirXZ) * kRippleSunShade,
                               kRippleShadeMin, kRippleShadeMax);

    // 水面色（ディフューズ）+ 環境マップ反射 + SSS + スペキュラ
    float3 finalColor = waterColor.rgb * lightColor * diffuse * rippleShade;

    // =========================
    // 屈折（スクリーンテクスチャ方式）
    // =========================
    // 水を描く直前の画面を、水面の法線（さざ波・うねり）でずらして引き、水中の物体をゆらがせる。
    // 光は水中を進むほど吸収・散乱されるので、
    //   透過光 = 画面の色 × 浅瀬色の色味^(厚み/透明度) × exp(-厚み/透明度)
    //   散乱光 = 水の色（深海色寄り） × (1 - exp(-厚み/透明度))
    // を足す。浅い所は水底が透け、深い所は水の色になる。
    if (refractionOn)
    {
        float2 refrOffset = N.xz * gRefractParams.y;
        refrOffset *= saturate(depthDiff);                    // 交差部はずらさない（物体の縁がちぎれないように）
        refrOffset /= max(1.0, pixelDepth * 0.05);            // 遠くほど画面上のずれを小さく
        float2 refrUV = saturate(screenUV + refrOffset);

        // ずらした先が水面より手前の物体なら、それを水中に映り込ませないよう元の位置へ戻す
        float refrDepth = LinearizeDepth(gDepthTexture.SampleLevel(gSamplerClamp, refrUV, 0).r,
                                         gCameraNearFar.x, gCameraNearFar.y);
        if (refrDepth < pixelDepth) {
            refrUV = screenUV;
            refrDepth = sceneDepth;
        }
        float thickness = max(refrDepth - pixelDepth, 0.0);
        float3 sceneCol = gSceneColor.SampleLevel(gSamplerClamp, refrUV, 0).rgb;

        // 水底のコースティクス：水面を高さ場 h とみると、深さ d の水底に届く光の密度は
        //   1 / (1 + (1 - 1/η) * d * ∇²h)
        // 山（∇²h < 0）の下で集まって明るく、谷（∇²h > 0）の下で散って暗くなる。
        float causticJ = 1.0 + kCausticFocus * thickness * rippleLaplacian;
        float caustic  = clamp(1.0 / max(causticJ, 1e-3), kCausticMin, kCausticMax);
        sceneCol *= caustic;

        float clarity = (gOceanParams2.x > 0.0) ? gOceanParams2.x : 3.0;
        float d = thickness / clarity;
        float T = exp(-d);
        float3 tint = gWaterShallowColor.rgb
                    / max(max(gWaterShallowColor.r, gWaterShallowColor.g), max(gWaterShallowColor.b, 1e-3));
        float3 transmit = pow(max(tint, 1e-3), min(d, 16.0));

        finalColor = sceneCol * transmit * T + finalColor * (1.0 - T);
    }

    finalColor = lerp(finalColor, envColor.rgb, fresnel * gMaterial.environmentCoefficient);
    finalColor += gSssColor.rgb * lightColor * sss * (1.0 - fresnel);
    finalColor += lightColor * specular;

    // =========================
    // 白波（波の山が押しつぶされて砕ける所）
    // =========================
    // Gerstner 波は山で頂点が寄り集まる。水平変位のヤコビアン J が 1 より小さい所ほど
    // 水面が圧縮されている（0 で折り返す＝砕ける）ので、そこに泡を乗せる。
    float whitecap = 0.0;
    if (gOceanParams.z > 0.0)
    {
        float3 jm = input.mainJacobian + detailSurf.jacobian;
        float J = (1.0 + jm.x) * (1.0 + jm.y) - jm.z * jm.z;
        float threshold = lerp(0.55, 0.95, saturate(gOceanParams2.w)); // 白波の量
        float foldFoam = saturate((threshold - J) / 0.25);

        // 泡の形を崩すノイズ（法線マップのチャンネルを借用、ゆっくり流す）
        float2 wp = input.worldPosition.xz;
        float lace = gTexture.Sample(gSampler, wp * 0.21 + float2(gTime * 0.02, -gTime * 0.013)).r * 0.6
                   + gTexture.Sample(gSampler, wp * 0.53 + float2(-gTime * 0.03, gTime * 0.021)).g * 0.4;
        foldFoam *= smoothstep(0.35, 0.65, lace + foldFoam * 0.3);

        whitecap = saturate(foldFoam * gOceanParams.z) * lerp(1.0, 0.6, distT);
        float3 capLit = gFoamColor.rgb * min(lightColor * lerp(0.7, 0.95, NdotL), 0.95);
        finalColor = lerp(finalColor, capLit, whitecap);
    }

    // 山のいちばん高いところだけ薄く白を乗せる（白波）。crestTint が 0 なら何もしない
    if (crestTint > 0.0)
    {
        // 白波も凪の所では立てない（ムラのマスクで疎密を付ける）
        float whitecap = pow(crestT, 8.0) * crestTint * 0.35 * roughMask;
        finalColor = lerp(finalColor, gFoamColor.rgb, whitecap);
    }

    // =========================
    // 引き波の泡（波紋テクスチャの G チャンネル）
    // =========================
    // スクリプトが置いた 2 種類の泡を描く。
    //   引き波（g）… 船尾がかき回した帯。主役は「白く濁った明るい水色」で、白い筋はまばらに乗せるだけ。
    //                ベタ塗りの白にすると帯が白飛びして見えるので、白の不透明度に上限を設ける
    //   波頭（b）  … 船首の砕け波と V の腕。細い白い線。スクリプトが毎フレーム置き直し、すぐ消える
    // 白はライトの強さで白飛び（＋ブルーム）しないよう明るさに上限を設ける。
    // 0.39m/テクセルのボケを隠すためにノイズで崩すが、細かすぎるとザラついた砂嵐に見えるので低周波だけ使う。
    float foamMask = 0.0;
    float milkyT = 0.0;
    if (wakeFoam > 0.001 || crestFoam > 0.001)
    {
        float2 fp = input.worldPosition.xz;
        // 法線マップの成分をノイズとして借用（周期 8m / 3m 程度、ゆっくり流す）
        float n1 = gTexture.Sample(gSampler, fp * 0.12 + float2(gTime * 0.010, gTime * 0.007)).r;
        float n2 = gTexture.Sample(gSampler, fp * 0.33 + float2(-gTime * 0.016, gTime * 0.011)).g;
        float lace = saturate(((n1 * 0.6 + n2 * 0.4) - 0.5) * 2.0 + 0.5);

        // 泡の白（日向で少し明るく。白飛びしないよう上限 0.9）
        float3 foamLit = gFoamColor.rgb * min(lightColor * lerp(0.75, 0.92, NdotL), 0.9);

        // ---- 引き波：白濁 ＋ まばらな白い筋 ----
        milkyT = smoothstep(0.03, 1.2, wakeFoam);
        finalColor = lerp(finalColor, lerp(finalColor, foamLit, 0.35), milkyT);

        float thr = 1.0 - saturate(wakeFoam * 0.8);
        float streak = smoothstep(thr - 0.12, thr + 0.12, lace) * smoothstep(0.08, 0.35, wakeFoam);
        float trailMask = streak * 0.55;

        // ---- 波頭：細い白線（ノイズで少しだけ途切れさせる）----
        float crestMask = smoothstep(0.25, 0.8, crestFoam * lerp(0.7, 1.3, lace)) * 0.9;

        foamMask = max(trailMask, crestMask);
        finalColor = lerp(finalColor, foamLit, foamMask);
    }

    // =========================
    // 波打ち際（フォーム）の計算
    // =========================


    // =========================
    // 跳ね返り波（逆向きに広がる波紋）の計算
    // =========================
    // depthDiff (水深) を使って等深線に沿った波紋を作る。
    // gTime を足すことで、浅いところ(0)から深いところ(>0)へ向かって波紋が動く(跳ね返るように見える)。
    float returnWave = sin(returnWavePhase) * 0.5f + 0.5f; // 0 ~ 1

    // ノイズを使って泡の形を不規則にする
    // 少しゆっくり動くノイズUV
    float2 noiseUV = input.worldPosition.xz * 0.15f + float2(gTime * 0.05f, -gTime * 0.05f);
    // gTexture の r チャンネルをノイズとして借用
    float foamNoise = gTexture.Sample(gSampler, noiseUV).r;

    // 波打ち際全体の泡強度を合成（基本強度 × 跳ね返り波 × ノイズ）
    // さらに、岸スレスレ（foamFadeが0に近い部分）は常に白く残るようにする
    float dynamicFoam = foamBaseIntensity * returnWave * (foamNoise * 2.0f);
    float staticFoam = pow(1.0f - foamFade, 4.0f); // 岸の根本の強い白線
    float foamIntensity = saturate(dynamicFoam + staticFoam);

    // フォームの色を最終カラーに加算ブレンド
    float3 foamColor = gFoamColor.rgb * gFoamParams.y; // スケール適用
    finalColor = lerp(finalColor, foamColor, foamIntensity * gFoamColor.a);

    // 泡は不透明（浅瀬で透けている所でも白く乗る）
    // 屈折ありのときは自前で水中を合成済みなので不透明。物体との交差部だけ αでぼかしてクリッピングの線を消す
    float baseAlpha = refractionOn ? saturate(depthDiff / max(gRefractParams.z, 0.001)) : waterColor.a;
    output.color = float4(finalColor, lerp(baseAlpha, 1.0, max(max(foamMask, milkyT * 0.4), whitecap)));

    return output;
}
