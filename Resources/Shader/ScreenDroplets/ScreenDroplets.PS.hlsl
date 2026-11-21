// =============================================================================
// ScreenDroplets（レンズ水滴）
//
// 水上⇔水中の遷移時に、カメラのレンズに付いた水を描くポストエフェクト（screendroplets PSO）。
//
// 水の「形」は ScreenDropletSim（CPU）が決め、ここでは光学だけを扱う。
//   1. 水滴（CPU から最大 96 粒）・静止した小粒（手続き）・水膜をメタボールとして足し合わせ、
//      1 枚の「水の厚み」の場にする。近い粒同士は自然につながり、ちぎれる
//   2. 場の勾配から解析的に法線を出す（ddx/ddy を使わないので縁がブロック状にならない）
//   3. 水滴は凸レンズ：背景を大きく、上下左右反転して映す。色ごとに屈折をずらす
//      縁は背景を映さず暗く沈み、光源の反対側に集光した明るい三日月が出る
//   4. 気泡（潜水時）は凹レンズ：背景を縮小して正立で映し、縁に全反射の銀色の輪が出る
//
// 浮上時はレンズ全面の水膜が中央から外へ割れて引き、前線にはメニスカスの盛り上がりが立つ。
// 潜水時は水面のラインが下から上へ抜ける。
// =============================================================================

#define MAX_DROPS 96 // ScreenDropletSim::kMaxDrops と一致させる

/// @struct PixelShaderInput
/// @brief フルスクリーン VS からの入力（Fullscreen.hlsli の VertexShaderOutput と同じ並び）
struct PixelShaderInput {
    float4 position : SV_POSITION; ///< クリップ空間位置
    float2 texcoord : TEXCOORD0;   ///< 画面 UV
};

/// @struct PixelShaderOutput
/// @brief ピクセル出力
struct PixelShaderOutput {
    float4 color : SV_TARGET0; ///< 出力色
};

Texture2D<float32_t4> gTexture : register(t0);
SamplerState gSampler : register(s0);

// b1: ScreenDroplets パラメータ（C++ 側 PostProcess::ScreenDropletsData と 1:1。3152 バイト）
// 座標はすべて「縦 = 1、横 = アスペクト比」の正規化座標（y は下向き）
cbuffer ScreenDropletsParams : register(b1) {
    float time;             ///< 経過時間（水膜の波立ちに使う）
    float intensity;        ///< 全体強度（0〜1。下がるほど水が痩せて消える。粒ごとに消える強度が違う）
    float speed;            ///< 流れる速さ（CPU 側でのみ使用）
    float distortion;       ///< 屈折の強さ
    float scale;            ///< 水滴の大きさ（1.5 で既定）
    float aspectRatio;      ///< アスペクト比（Width / Height）
    float elapsed;          ///< 演出開始からの秒数
    float mode;             ///< 0: 浮上（水滴・水膜） 1: 潜水（気泡・水面ライン）
    float dropCount;        ///< gDropA / gDropB の有効数
    float sheetCenterX;     ///< 水膜が割れ始める点 X
    float sheetCenterY;     ///< 水膜が割れ始める点 Y
    float sheetDuration;    ///< 水膜が引ききる / 水面ラインが抜けきる秒数
    float sheetRadiusMax;   ///< 水膜の乾いた領域が届く距離
    float sheetGravityBias; ///< 上ほど早く乾く度合い
    float invScreenHeight;  ///< 1 / 画面の高さ（ピクセル）。縁のアンチエイリアス用
    float _padding0;        ///< 16 バイト境界合わせ
    float drainStart;       ///< 遷移前に急いで消し始めた時刻（elapsed 基準。していなければ大きな値）
    float drainDuration;    ///< 急いで消すときに消えきるまでの秒数
    float2 _padding1;       ///< 16 バイト境界合わせ
    float4 gDropA[MAX_DROPS]; ///< xy: 中心, z: 半径, w: 後ろ側の伸び率
    float4 gDropB[MAX_DROPS]; ///< xy: 進行方向, z: 進行方向の伸び率, w: 直交方向の伸び率
};

static const float kIso = 0.22f;   ///< 場がこの値を超えた所が「水がある」
static const float kSlope = 0.75f; ///< 厚みの勾配 → 法線の傾きの倍率（大きいほど縁が急に立つ）

/// @struct Field
/// @brief メタボールの場（F: 正規化した場, G: F の勾配, T: 半径で重み付けした厚み）
struct Field {
    float F;
    float2 G;
    float T;
};

float Sq(float x) { return x * x; }

/// @brief 2D 座標から 0〜1 の擬似乱数を作る
float Hash21(float2 p) {
    p = frac(p * float2(123.34f, 345.45f));
    p += dot(p, p + 34.345f);
    return frac(p.x * p.y);
}

/// @brief 滑らかな 2D バリューノイズ（0〜1）
float ValueNoise(float2 p) {
    float2 i = floor(p);
    float2 f = frac(p);
    f = f * f * (3.0f - 2.0f * f);
    float a = Hash21(i);
    float b = Hash21(i + float2(1.0f, 0.0f));
    float c = Hash21(i + float2(0.0f, 1.0f));
    float d = Hash21(i + float2(1.0f, 1.0f));
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

/// @brief 画面外を参照したときに縁が伸びないよう折り返す
float2 MirrorUV(float2 uv) {
    return 1.0f - abs(1.0f - abs(uv));
}

/// @brief 水膜がこの点まで引いた時刻（ScreenDropletSim::ArrivalTime と同じ式）
float ArrivalTime(float2 c) {
    if (mode > 0.5f) {
        return saturate((1.08f - c.y) / 1.25f) * sheetDuration;
    }
    float2 d = c - float2(sheetCenterX, sheetCenterY);
    float m = length(d) + sheetGravityBias * d.y;
    float s = m / sheetRadiusMax;
    if (s <= 0.0f) return 0.0f;
    if (s >= 1.0f) return 1.0e3f;
    return sheetDuration * (1.0f - pow(1.0f - s, 1.0f / 3.0f));
}

/// @brief 1 粒ぶんの場を足す（進行方向に伸び、後ろ側が長くすぼむ涙型）
void AddDrop(inout Field f, float2 p, float4 A, float4 B) {
    float2 dp = p - A.xy;
    float r = A.z;
    float tail = A.w;
    float along = B.z;
    float perp = B.w;
    float reach = r * max(along * tail, perp);
    if (dot(dp, dp) >= reach * reach) return;

    float2 dir = B.xy;
    float2 side = float2(-dir.y, dir.x);
    float u = dot(dp, dir);
    float v = dot(dp, side);

    // 前半分は丸く、後ろ半分は長く細く（伸び率の微分は小さいので勾配では無視する）
    float front = smoothstep(-0.35f * r, 0.35f * r, u);
    float su = r * along * lerp(tail, 1.0f, front);
    float taper = lerp(lerp(0.72f, 1.0f, saturate(1.0f + u / su)), 1.0f, front);
    float sv = r * perp * taper;

    float a = u / su;
    float b = v / sv;
    float d2 = a * a + b * b;
    if (d2 >= 1.0f) return;

    // カーネル (1 - d²)²：中心でなだらか、外周で 0 に落ちる。足し合わせると粒同士がつながる
    float w = 1.0f - d2;
    f.F += w * w;
    f.G += -2.0f * w * (2.0f * a / su * dir + 2.0f * b / sv * side);
    f.T += r * w * w;
}

/// @brief 水膜が引いたあとにレンズに残る、動かない小粒（1 セルに最大 1 粒）
/// @param cell    セルの大きさ（粒はセルからはみ出さない）
/// @param seed    層ごとの乱数のずらし
/// @param density 粒が置かれるセルの割合
/// @param life    痩せて消えるまでの秒数（個体差 ±40%）
/// @details 消える時刻・強度を粒ごとにずらし、一斉に消えないようにしている。
void AddBeads(inout Field f, float2 p, float cell, float seed, float density, float life) {
    float2 id = floor(p / cell);
    // 粒は一様ではなく、ところどころ固まって残る
    float cluster = smoothstep(0.25f, 0.75f, ValueNoise(id * cell * 4.0f + seed));
    if (Hash21(id + seed) > density * cluster * 1.6f) return;
    float2 jitter = float2(Hash21(id * 1.7f + seed + 3.1f), Hash21(id * 2.3f + seed + 7.7f)) - 0.5f;
    float2 c = (id + 0.5f + jitter * 0.5f) * cell;
    float r0 = cell * (0.08f + 0.16f * Hash21(id * 3.9f + seed + 1.3f));

    float age = elapsed - ArrivalTime(c);
    if (age <= 0.0f) return;
    float lifeI = life * (0.6f + 0.8f * Hash21(id * 5.1f + seed));
    float r = r0 * sqrt(saturate(1.0f - age / lifeI)) * smoothstep(0.0f, 0.06f, age);
    // 遷移前に急いで消すとき・全体強度が下がるときも、粒ごとに違うタイミングで痩せる
    float order = Hash21(id * 7.3f + seed + 2.9f);
    float drainEnd = drainStart + drainDuration * (0.3f + 0.7f * order);
    r *= sqrt(saturate((drainEnd - elapsed) / (0.3f * drainDuration)));
    r *= sqrt(saturate((intensity - order * 0.75f) / 0.25f));
    if (r < 1.0e-5f) return;
    AddDrop(f, p, float4(c, r, 1.0f), float4(0.0f, 1.0f, 1.0f, 1.0f));
}

/// @brief 浮上時の水膜の場（前線より外側に膜が残り、前線にはメニスカスが盛り上がる）
float FilmField(float2 p) {
    float2 d = p - float2(sheetCenterX, sheetCenterY);
    float len = length(d);
    float m = len + sheetGravityBias * d.y;
    float2 q = d / max(len, 1.0e-4f);

    // 乾いた領域の広がり（ArrivalTime と同じ ease-out。終わったあとも外へ抜けきるまで広げる）
    // 開始直後に中心へ穴が開いていないよう、0.03 だけ遅らせて割れ始める
    float t = saturate(elapsed / sheetDuration);
    float R = sheetRadiusMax * (1.0f - pow(1.0f - t, 3.0f)) + max(0.0f, elapsed - sheetDuration) * 0.8f - 0.03f;

    // 前線の形：大きなうねり ＋ 取り残される指状の筋（方向だけで決めるので前線と一緒に外へ動く）
    // 中心付近・割れ始めは方向が定まらないので凹凸を消す（放射状の筋が出るのを防ぐ）
    float nearCenter = saturate(len * 4.0f) * saturate(R * 6.0f);
    float wave = (ValueNoise(q * 2.5f + 7.3f) - 0.5f) * 0.18f * nearCenter;
    float finger = pow(ValueNoise(q * 11.0f + 3.1f), 3.0f) * 0.16f * nearCenter;

    float s = m + wave + finger - R; // 正：まだ膜が残っている

    float cover = smoothstep(0.0f, 0.02f, s);
    float ridge = exp(-Sq((s - 0.025f) / 0.03f)) * smoothstep(-0.005f, 0.01f, s);
    float ripple = (ValueNoise(p * 7.0f + float2(0.0f, time * 0.6f)) - 0.5f) * 0.11f
                 + (ValueNoise(p * 19.0f - float2(time * 0.4f, 0.0f)) - 0.5f) * 0.03f;
    // 遷移前に急いで消すときは膜ごと引かせる
    float keep = saturate(1.0f - (elapsed - drainStart) / max(drainDuration * 0.5f, 1.0e-3f));
    return (cover * (kIso + 0.12f + ripple) + ridge * 0.5f) * keep;
}

/// @brief 潜水時に下から上へ抜ける水面ライン
/// @return x: 縦方向の UV ずれ, y: ラインの強さ, z: ラインからの符号付き距離（正が水中側）
float3 Waterline(float2 p) {
    float t = saturate(elapsed / sheetDuration);
    float y = 1.08f - 1.25f * t
            + 0.022f * sin(p.x * 6.5f + time * 4.0f)
            + 0.010f * sin(p.x * 17.0f - time * 7.0f);
    float s = (p.y - y) / 0.04f;
    float k = exp(-s * s) * (1.0f - smoothstep(0.8f, 1.0f, t));
    return float3(-0.07f * s * k, k, s);
}

PixelShaderOutput main(PixelShaderInput input) {
    PixelShaderOutput output;
    float2 uv = input.texcoord;

    if (intensity <= 0.001f) {
        output.color = gTexture.Sample(gSampler, uv);
        return output;
    }

    const bool dive = (mode > 0.5f);
    const float2 p = float2(uv.x * aspectRatio, uv.y);
    const float sizeMul = max(scale / 1.5f, 0.2f);
    const float2 toUV = float2(1.0f / aspectRatio, 1.0f);

    // ---- 1. 水の場を作る ----------------------------------------------------
    Field f;
    f.F = 0.0f;
    f.G = float2(0.0f, 0.0f);
    f.T = 0.0f;

    const int count = min((int)dropCount, MAX_DROPS);
    [loop]
    for (int i = 0; i < count; ++i) {
        AddDrop(f, p, gDropA[i], gDropB[i]);
    }

    float filmF = 0.0f;
    float3 wline = float3(0.0f, 0.0f, 0.0f);
    if (!dive) {
        AddBeads(f, p, 0.020f * sizeMul, 0.0f, 0.30f, 1.1f);
        AddBeads(f, p, 0.043f * sizeMul, 17.0f, 0.20f, 1.6f);
        if (elapsed < sheetDuration * 2.0f) {
            // 水膜は手続きなので勾配は中心差分で取る
            const float e = 1.5f * invScreenHeight;
            filmF = FilmField(p);
            float fx = FilmField(p + float2(e, 0.0f)) - FilmField(p - float2(e, 0.0f));
            float fy = FilmField(p + float2(0.0f, e)) - FilmField(p - float2(0.0f, e));
            f.F += filmF;
            f.G += float2(fx, fy) / (2.0f * e);
            f.T += filmF * 0.03f;
        }
    } else {
        AddBeads(f, p, 0.05f * sizeMul, 5.0f, 0.30f, 0.7f); // 突入で付く微小な気泡（時間差ではじける）
        wline = Waterline(p);
    }

    // 全体強度は CPU の水滴・小粒ともに「粒ごとの閾値」で半径へ効かせてある（一斉に消さないため）。
    // ここでは水膜と水面ラインだけを弱める
    const float fade = saturate(intensity);
    wline.xy *= fade;

    const float2 baseUV = uv + float2(0.0f, wline.x);
    float4 scene = gTexture.Sample(gSampler, MirrorUV(baseUV));
    // ハイライト・輪の明るさは周りの明るさに比例させる（暗転していく画面で水滴だけ光って浮かないように）
    const float envLight = saturate(dot(scene.rgb, float3(0.299f, 0.587f, 0.114f)) * 1.6f + 0.08f);

    // 縁までの距離をピクセル単位で求め、1 ピクセル幅でアンチエイリアスする
    const float gradLen = length(f.G);
    const float edgePx = (f.F - kIso) / max(gradLen * invScreenHeight, 1.0e-6f);
    const float wet = saturate(edgePx * 0.7f + 0.5f);

    if (wet <= 0.0f) {
        scene.rgb += wline.y * exp(-Sq(wline.z * 2.5f)) * 0.18f * envLight;
        output.color = float4(scene.rgb, 1.0f);
        return output;
    }

    // ---- 2. 厚み → 法線 -----------------------------------------------------
    // 厚み h = R·√((F - iso) / (1 - iso))：中心はなだらか、縁は急に立つ（接触角の大きい水滴）
    const float Ravg = f.T / max(f.F, 1.0e-4f);
    const float g = saturate((f.F - kIso) / (1.0f - kIso));
    const float2 gradH = Ravg * f.G / ((1.0f - kIso) * 2.0f * sqrt(max(g, 0.015f)));
    const float3 n = normalize(float3(-gradH * kSlope, 1.0f));
    const float filmShare = saturate(filmF / max(f.F, 1.0e-4f));

    // ---- 3. 屈折 ------------------------------------------------------------
    // 水滴（凸）：法線と逆向きに引く → 縁ほど遠くを映し、像が反転する
    // 気泡（凹）：法線の向きに引く → 縮小した正立像
    // 水膜の前線は細長く急なので、水滴と同じ強さで曲げると景色がちぎれて見える。膜の割合だけ弱める
    const float2 bend = n.xy * distortion * (dive ? 1.4f : -3.0f) * lerp(1.0f, 0.45f, filmShare) * toUV;
    float3 refr;
    refr.r = gTexture.Sample(gSampler, MirrorUV(baseUV + bend * 0.97f)).r;
    refr.g = gTexture.Sample(gSampler, MirrorUV(baseUV + bend)).g;
    refr.b = gTexture.Sample(gSampler, MirrorUV(baseUV + bend * 1.05f)).b;

    if (filmShare > 0.01f) {
        // 水膜越しの景色は少しにじむ
        const float br = 0.004f * filmShare;
        float3 blur = refr;
        blur += gTexture.Sample(gSampler, MirrorUV(baseUV + bend + float2(br, 0.0f) * toUV)).rgb;
        blur += gTexture.Sample(gSampler, MirrorUV(baseUV + bend - float2(br, 0.0f) * toUV)).rgb;
        blur += gTexture.Sample(gSampler, MirrorUV(baseUV + bend + float2(0.0f, br))).rgb;
        blur += gTexture.Sample(gSampler, MirrorUV(baseUV + bend - float2(0.0f, br))).rgb;
        refr = lerp(refr, blur * 0.2f, filmShare * 0.8f);
    }

    // ---- 4. 光 --------------------------------------------------------------
    const float3 L = normalize(float3(-0.35f, -0.80f, 0.50f)); // 左上から（y は下向き）
    const float3 H = normalize(L + float3(0.0f, 0.0f, 1.0f));
    const float ndh = saturate(dot(n, H));
    const float spec = (pow(ndh, 260.0f) * 2.0f + pow(ndh, 32.0f) * 0.08f) * envLight;
    const float fres = pow(1.0f - saturate(n.z), 3.0f);

    float3 col;
    if (!dive) {
        // 縁は斜めに入った光が全反射して背景を映さず、暗く沈む
        const float rim = saturate(fres * 1.7f);
        col = refr * lerp(1.0f, 0.15f, rim);
        // 上から来た光が水滴の中で集まり、下側（光源の反対側）が明るくなる
        const float2 nl = n.xy / max(length(n.xy), 1.0e-4f);
        const float caustic = Sq(saturate(dot(nl, -normalize(L.xy)))) * saturate((1.0f - n.z) * 2.5f) * (1.0f - rim);
        col += caustic * (refr * 0.5f + 0.06f) * 0.6f;
        // レンズと接する縁の細い線
        col *= 1.0f - 0.3f * exp(-Sq(edgePx / 1.5f)) * (1.0f - filmShare);
        // 水膜はわずかに曇る
        col = col * lerp(1.0f, 0.93f, filmShare) + filmShare * 0.02f * envLight;
        col += spec * float3(1.0f, 0.98f, 0.95f);
    } else {
        // 気泡：外周は全反射で暗い縁、その内側に銀色の輪
        const float ring = exp(-Sq((n.z - 0.45f) / 0.16f));
        col = refr * (1.0f - 0.85f * saturate(fres * 2.5f));
        col += ring * float3(0.60f, 0.85f, 1.0f) * 0.55f * envLight;
        col += spec * 2.0f * float3(0.9f, 0.97f, 1.0f);
    }

    float3 result = lerp(scene.rgb, col, wet);
    result += wline.y * exp(-Sq(wline.z * 2.5f)) * 0.18f * envLight;

    output.color = float4(result, 1.0f);
    return output;
}
