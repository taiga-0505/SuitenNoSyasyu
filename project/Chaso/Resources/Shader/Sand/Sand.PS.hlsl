// =============================================================================
// Sand.PS.hlsl - 砂の地形のピクセルシェーダー
//
// 砂らしさはすべて手続きで描く（テクスチャを使わない）:
//   - 粒        : 細かいノイズの色のばらつきと凹凸、ときどき混じる貝殻のかけら・黒い粒
//   - 風紋      : 風下側が急な非対称の縞。ノイズで揺らし、濡れた所・掘った所では消える
//   - 波打ち際  : 水面より少し上までは濡れて暗く、斜めの光を細かく照り返す
//   - きらめき  : 乾いた砂の中の小さな粒がときどき光る
//   - 跡        : RenderInteractiveSand のハイトマップ（R: 高さ / G: 掘りたての湿り / B: 波の濡れ）
//
// 真上からの光だけでは凹凸が見えないので、細かい凹凸（粒・風紋・跡）は
// 斜めの「起伏用の光」で陰影を付け、平らな所を 1 とした比で明るさに掛ける。
// =============================================================================

#include "Sand.hlsli"

struct DirectionalLight
{
    float4 color;           ///< 光の色（RGBA）
    float3 direction;       ///< 光の方向
    float  intensity;       ///< 光の強度
    float3 ambientColor;    ///< 環境光の色
    float  ambientIntensity; ///< 環境光の強さ
};
ConstantBuffer<DirectionalLight> gDirectionalLight : register(b1);

struct Camera
{
    float3 worldPosition; ///< カメラのワールド座標
};
ConstantBuffer<Camera> gCamera : register(b2);

Texture2D<float4> gSandMap : register(t4); ///< 跡のハイトマップ
SamplerState gSamplerClamp : register(s2);

struct PixelShaderOutput
{
    float4 color : SV_TARGET0; ///< 出力色
};

/// @brief 起伏用の光（左上の斜め上から）。真上視点で凹凸が読めるように傾けてある
static const float3 kReliefLight = float3(-0.50f, 0.72f, 0.48f);
/// @brief 風紋の風上側の割合（残りが急な風下側）
static const float kRippleWindward = 0.72f;

/// @brief 2D のハッシュ（0〜1）
float Hash21(float2 p)
{
    p = frac(p * float2(123.34f, 456.21f));
    p += dot(p, p + 45.32f);
    return frac(p.x * p.y);
}

/// @brief バリューノイズ（0〜1）
float ValueNoise(float2 p)
{
    float2 i = floor(p);
    float2 f = frac(p);
    float2 u = f * f * (3.0f - 2.0f * f);
    float a = Hash21(i);
    float b = Hash21(i + float2(1, 0));
    float c = Hash21(i + float2(0, 1));
    float d = Hash21(i + float2(1, 1));
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

/// @brief 3 オクターブのノイズ（0〜1）
float Fbm(float2 p)
{
    float v = 0.0f;
    float a = 0.5f;
    for (int i = 0; i < 3; ++i)
    {
        v += a * ValueNoise(p);
        p = p * 2.03f + 7.1f;
        a *= 0.5f;
    }
    return v / 0.875f;
}

/// @brief 跡のハイトマップを読む（窓の縁ではぼかし、窓の外は 0）
float4 SampleMarks(float2 xz)
{
    float2 uv = (xz - gSandWindow.xy) / max(gSandWindow.z, 1e-3f) + 0.5f;
    float2 e = min(uv, 1.0f - uv);
    float fade = saturate(min(e.x, e.y) / 0.03f);
    return gSandMap.SampleLevel(gSamplerClamp, uv, 0) * fade;
}

PixelShaderOutput main(VertexShaderOutput input)
{
    PixelShaderOutput o;
    float3 wp = input.worldPosition;
    float2 xz = wp.xz;
    float3 nMacro = normalize(input.normal);
    float time = gGrain.w;

    // ---- 跡（掘った溝・縁の盛り上がり・湿り・波の濡れ） ----
    float4 mark = SampleMarks(xz);
    float texStep = gSandWindow.w;
    float hL = SampleMarks(xz - float2(texStep, 0)).x;
    float hR = SampleMarks(xz + float2(texStep, 0)).x;
    float hD = SampleMarks(xz - float2(0, texStep)).x;
    float hU = SampleMarks(xz + float2(0, texStep)).x;
    float2 markGrad = float2(hR - hL, hU - hD) / (2.0f * texStep) * gMark.x;
    float markH = mark.x;
    float fresh = mark.y;

    // ---- 濡れ：水面より少し上まで（境目はノイズで波打たせる）と、波に洗われた所 ----
    float above = wp.y - gWet.x + (ValueNoise(xz * 0.6f) - 0.5f) * 0.5f * gWet.y;
    float tideWet = 1.0f - smoothstep(0.0f, gWet.y, above);
    float wet = saturate(max(tideWet, mark.z * 0.85f));

    // ---- 風紋（乾いた所だけ。掘った所・湿った所では崩れて消える） ----
    float2 dir = gRipple.zw;
    float2 perp = float2(-dir.y, dir.x);
    float warp = Fbm(xz * 0.10f) * 3.2f + sin(dot(xz, perp) * 0.31f) * 0.7f;
    float u = (dot(xz, dir) + warp) / gRipple.x;
    float f = frac(u);
    float profile = (f < kRippleWindward) ? f / kRippleWindward : (1.0f - f) / (1.0f - kRippleWindward);
    float slope = (f < kRippleWindward) ? 1.0f / kRippleWindward : -1.0f / (1.0f - kRippleWindward);
    // 山と谷の角をなまらせる（ちらつき防止）
    slope *= smoothstep(0.0f, 0.06f, f) * smoothstep(0.0f, 0.06f, 1.0f - f) * 0.7f + 0.3f * sign(slope);
    float rippleFade = (1.0f - wet) * (1.0f - saturate(abs(markH) * 30.0f)) * (1.0f - fresh);
    rippleFade *= 0.55f + 0.45f * ValueNoise(xz * 0.05f + 3.0f); // 風紋の濃い所・薄い所
    float rippleAmt = gRipple.y * rippleFade;

    // ---- 粒の凹凸（格子の向きが目立たないよう座標を回してから読む） ----
    float2 gxz = float2(xz.x * 0.866f - xz.y * 0.5f, xz.x * 0.5f + xz.y * 0.866f);
    float g1 = ValueNoise(gxz * gGrain.x);
    float g2 = ValueNoise(gxz.yx * gGrain.x * 2.31f + 17.0f);

    // ---- 細かい法線（地形 + 跡 + 風紋 + 粒） ----
    float3 n = nMacro;
    n += float3(-markGrad.x, 0.0f, -markGrad.y);
    n += float3(dir.x, 0.0f, dir.y) * (-slope * rippleAmt * 0.35f);
    n += float3(g1 - 0.5f, 0.0f, g2 - 0.5f) * (0.10f * (1.0f - wet * 0.6f));
    n = normalize(n);

    // ---- 色 ----
    float3 albedo = gDryColor.rgb;
    albedo *= 0.93f + 0.14f * Fbm(xz * 0.06f);                      // 大きなまだら
    albedo *= 1.0f + (profile - 0.5f) * 0.12f * rippleFade;          // 風紋の山は明るく、谷は暗く
    albedo *= 1.0f + (g1 * 0.6f + g2 * 0.4f - 0.5f) * 2.0f * gGrain.y; // 粒のばらつき

    // 色の付いた粒（貝殻のかけら・黒い粒）
    float speckScale = gGrain.x * 0.42f;
    float2 cell = floor(xz * speckScale);
    float2 cf = frac(xz * speckScale);
    float2 cc = float2(Hash21(cell + 3.1f), Hash21(cell + 7.7f)) * 0.6f + 0.2f;
    float speck = (Hash21(cell) < gGrain.z) ? smoothstep(0.24f, 0.12f, length(cf - cc)) : 0.0f;
    float3 speckColor = (Hash21(cell + 11.3f) < 0.55f) ? gGrainColorA.rgb : gGrainColorB.rgb;
    albedo = lerp(albedo, speckColor, speck * (1.0f - wet * 0.5f));

    // 濡れ・掘りたての湿りで暗く
    albedo = lerp(albedo, gWetColor.rgb, wet);
    albedo = lerp(albedo, gWetColor.rgb, saturate(fresh * gMark.y * 2.0f) * (1.0f - wet));
    // 溝の底は少し暗く、縁の盛り上がりは少し明るく
    albedo *= 1.0f + clamp(markH * 2.5f * gMark.x, -0.22f, 0.10f);

    // ---- 光 ----
    float3 L = normalize(-gDirectionalLight.direction);
    float3 Lr = normalize(kReliefLight);
    float sun = saturate(dot(nMacro, L));
    // 細かい凹凸は斜めの光で、平らな所を 1 とした比で効かせる
    float relief = saturate(dot(n, Lr)) / max(saturate(dot(nMacro, Lr)), 0.2f);
    relief = clamp(relief, 0.35f, 1.6f);
    float3 lightCol = gDirectionalLight.color.rgb * saturate(gDirectionalLight.intensity);
    float3 color = albedo * lightCol * (0.62f + 0.38f * sun) * relief;

    // 濡れた砂：空を少し映し、斜めの光を細かい凹凸で照り返す
    float3 V = normalize(gCamera.worldPosition - wp);
    color = lerp(color, color * float3(0.92f, 1.0f, 1.08f) + float3(0.03f, 0.05f, 0.07f), wet * 0.6f);
    float3 R = reflect(-Lr, n);
    float glint = pow(saturate(dot(R, V)), 48.0f);
    color += glint * wet * gWet.z * 0.55f * lightCol;

    // 乾いた砂のきらめき：小さな粒がときどき瞬く
    float sparkScale = gGrain.x * 0.9f;
    float2 sc = floor(xz * sparkScale);
    float sh = Hash21(sc + 21.7f);
    if (sh > 0.992f)
    {
        float2 sp = frac(xz * sparkScale) - (float2(Hash21(sc + 1.3f), Hash21(sc + 5.9f)) * 0.6f + 0.2f);
        float dotMask = smoothstep(0.16f, 0.0f, length(sp));
        float twinkle = pow(saturate(sin(time * 2.3f + sh * 400.0f)), 12.0f);
        color += dotMask * twinkle * gWet.w * (1.0f - wet) * 1.2f;
    }

    o.color = float4(color, 1.0f);
    return o;
}
