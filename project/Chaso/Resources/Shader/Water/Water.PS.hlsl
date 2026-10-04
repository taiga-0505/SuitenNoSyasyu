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
};

// t0: テクスチャ（法線マップとしても使用可能）
Texture2D<float4> gTexture : register(t0);

// t1: 環境キューブマップ (反射用)
TextureCube<float4> gEnvironmentTexture : register(t1);

// t4: インタラクティブ波紋ハイトマップ（r: 高さ, g: 引き波の泡, b: 波頭の白波）
Texture2D<float4> gInteractiveWave : register(t4);

// t5: Foam用の深度テクスチャ
Texture2D<float> gDepthTexture : register(t5);

SamplerState gSampler : register(s0);
SamplerState gSamplerClamp : register(s2);

struct PixelShaderOutput
{
    float4 color : SV_TARGET0;
};

// =====================================================
// ノーマルマップから法線を取得（スクロール合成）
// =====================================================
float3 GetScrolledNormal(float2 uv, float time, float3 geometryNormal)
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
    blended.xy *= gNormalStrength;
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
    // 泡の量（g: 引き波 / b: 波頭）。範囲外（UV が 0..1 の外）はクランプで端の値になるので 0 に落とす
    float2 foamSample = gInteractiveWave.Sample(gSamplerClamp, waveUV).gb;
    foamSample *= (all(waveUV > 0.0) && all(waveUV < 1.0)) ? 1.0 : 0.0;
    float wakeFoam  = foamSample.x;
    float crestFoam = foamSample.y;
    float3 interactiveNormal = normalize(float3(hL - hR, 2.0f * (100.0f * texelSize), hD - hU));

    // ジオメトリ法線 + 波紋法線 + 跳ね返り法線
    geoNormal = normalize(geoNormal + (interactiveNormal - float3(0, 1, 0)) + bounceNormal);

    // ノイズテクスチャのスクロール法線と合成
    float3 N = GetScrolledNormal(input.texcoord, gTime, geoNormal);

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
        float amplitude = max(gWaveHeight + gWaveHeight2 + gWaveHeight * 0.25, 0.001);
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

    // スペキュラ (Blinn-Phong)
    float3 H = normalize(L + V);
    float specular = pow(saturate(dot(N, H)), gSpecularPower) * gDirectionalLight.intensity;

    // =========================
    // 最終合成
    // =========================
    // 水面色（ディフューズ）+ 環境マップ反射 + スペキュラ
    float3 finalColor = waterColor.rgb * lightColor * diffuse;
    finalColor = lerp(finalColor, envColor.rgb, fresnel * gMaterial.environmentCoefficient);
    finalColor += lightColor * specular * 0.5;

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
    output.color = float4(finalColor, lerp(waterColor.a, 1.0, max(foamMask, milkyT * 0.4)));

    return output;
}
