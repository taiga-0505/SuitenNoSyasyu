#include "../Fullscreen/Fullscreen.hlsli"

// ============================================================================
// InkOverlay
//   タコの墨がレンズ（画面）へベチャッと貼り付いて視界を塞ぐポストエフェクト（inkoverlay PSO）。
//   テクスチャは使わず、ハッシュノイズだけで「墨だまり」「飛び散った粒」「垂れ筋」を作る。
//
//   墨は最大 kMaxSplats 個まで同時に貼れる。1 個ごとに位置・大きさ・強さ・経過時間・種を
//   ホスト側（InkScreenFx）が持ち、毎フレーム流し込む。
//
//   消え方は「全体が薄くなる」ではなく「縁から崩れて小さくなる」。
//   strength を下げると侵食のしきい値が上がり、ノイズで穴が空きながら中心へ縮んでいく。
//   一様に透けていくと、墨ではなく半透明のシールに見えるため。
// ============================================================================

// 同時に貼れる墨の最大数（C++ 側 PostProcess の kMaxInkSplats と揃える）
#define kMaxSplats 6

/// @struct PixelShaderOutput
/// @brief ピクセル出力
struct PixelShaderOutput {
    float4 color : SV_TARGET0; ///< 出力色
};

Texture2D<float4> gTexture : register(t0);
SamplerState gSampler : register(s0);

// b1: InkOverlay パラメータ
//   ヘッダ 32 バイト + 墨 32 バイト × 6 = 224 バイト（float4 × 14）
cbuffer InkOverlayParams : register(b1) {
    float time;          ///< 経過時間（墨のぬめりの揺らぎにだけ使う）
    float aspectRatio;   ///< アスペクト比（Width / Height）
    int   splatCount;    ///< 有効な墨の数（0〜kMaxSplats）
    float murk;          ///< 画面全体の濁り（墨が水に溶けた感じ。0.0〜1.0）

    float4 inkColor;     ///< 墨の色（RGB）。A = 最も濃いところの不透明度

    // splatA: xy = 中心 UV, z = 半径（画面の高さを 1 とした長さ）, w = 強さ（0〜1）
    // splatB: x = 貼り付いてからの秒数, y = 種（0〜1）, zw = 未使用
    float4 splatA[kMaxSplats]; ///< 墨ごとの位置・半径・強さ
    float4 splatB[kMaxSplats]; ///< 墨ごとの経過時間・種
};

// ---------------------------------------------------------------------------
// ノイズ（BloodOverlay と同じ系統）
// ---------------------------------------------------------------------------

/// @brief 1D の値から 0〜1 の擬似乱数を作る
/// @param p 入力値
/// @return 0〜1 の擬似乱数
float Hash11(float p) {
    p = frac(p * 0.1031f);
    p *= p + 33.33f;
    p *= p + p;
    return frac(p);
}

/// @brief 2D 座標から 0〜1 の擬似乱数を作る
/// @param p 入力座標
/// @return 0〜1 の擬似乱数
float Hash21(float2 p) {
    p = frac(p * float2(127.1f, 311.7f));
    p += dot(p, p + 45.32f);
    return frac(p.x * p.y);
}

/// @brief 2D 座標から 0〜1 の 2 成分の擬似乱数を作る
/// @param p 入力座標
/// @return 0〜1 の擬似乱数 2 つ
float2 Hash22(float2 p) {
    float2 q = float2(dot(p, float2(127.1f, 311.7f)),
                      dot(p, float2(269.5f, 183.3f)));
    return frac(sin(q) * 43758.5453f);
}

/// @brief 値ノイズ（格子点の乱数を滑らかに補間）
/// @param p 入力座標
/// @return 0〜1 のノイズ値
float ValueNoise(float2 p) {
    float2 i = floor(p);
    float2 f = frac(p);
    float2 u = f * f * (3.0f - 2.0f * f);
    float a = Hash21(i);
    float b = Hash21(i + float2(1.0f, 0.0f));
    float c = Hash21(i + float2(0.0f, 1.0f));
    float d = Hash21(i + float2(1.0f, 1.0f));
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

/// @brief 4 オクターブの fBm
/// @param p 入力座標
/// @return おおむね 0〜1 のノイズ値
float Fbm(float2 p) {
    float sum = 0.0f;
    float amp = 0.5f;
    for (int i = 0; i < 4; ++i) {
        sum += ValueNoise(p) * amp;
        p *= 2.03f;
        amp *= 0.5f;
    }
    return sum;
}

// ---------------------------------------------------------------------------
// 墨 1 個ぶんの「濃さの場」
//   返り値は 0 以下（墨なし）〜 1.6（墨の芯）。0 付近が輪郭。しきい値で切るのは呼び出し側。
//   p はアスペクト補正済み・墨の中心を原点にした座標（+Y が画面の下）。
// ---------------------------------------------------------------------------

// dripMask: 垂れ筋の形（0〜1。幅は一定）。垂れ筋だけは侵食に通さず、呼び出し側で丸ごと薄くする。
//   侵食（しきい値を上げる）で消すと、筋が細るだけで長さは残り、細い線になって居座るため。
/// @brief 墨 1 個ぶんの濃さの場を求める
/// @param p        墨の中心を原点にしたアスペクト補正済み座標（+Y が画面の下）
/// @param radius   墨の半径（画面の高さを 1 とした長さ）
/// @param age      貼り付いてからの秒数（垂れ筋の伸びに使う）
/// @param seed     形の種（0〜1）
/// @param dripMask 垂れ筋のマスク（出力。0〜1）
/// @return 濃さ（0 以下 = 墨なし、0 付近 = 輪郭、最大 1.6 = 芯。範囲外は -1）
float InkField(float2 p, float radius, float age, float seed, out float dripMask) {
    dripMask = 0.0f;
    const float r = length(p);
    const float2 q = p / radius; // 半径 1 に正規化

    // 範囲外は丸ごと打ち切る（垂れ筋は下に長く伸びるので下側だけ広めに取る）
    if (q.y < 0.0f ? (r > radius * 2.1f) : (length(q * float2(1.0f, 0.4f)) > 2.4f)) {
        return -1.0f; // 0 を返すと侵食しきい値（新しい墨は約 0）に引っかかって薄く塗られる
    }

    // --- 本体：角度方向にギザギザした墨だまり ---
    // 角度だけのノイズだと星形になるので、位置の fBm で輪郭を崩す
    float n = Fbm(q * 1.8f + seed * 17.0f);
    float n2 = ValueNoise(q * 6.5f + seed * 31.0f);
    float edgeR = 0.70f + 0.58f * n + 0.14f * n2;
    float body = 1.0f - length(q) / edgeR; // 中心 1 → 輪郭 0 → 外側で負

    // --- 飛び散った粒：本体の外側 0.9R 〜 1.6R の輪にだけ出す ---
    float spray = -1.0f;
    {
        float2 g = q * 3.2f + seed * 9.0f;
        float2 id = floor(g);
        float2 f = frac(g) - 0.5f;
        for (int y = -1; y <= 1; ++y) {
            for (int x = -1; x <= 1; ++x) {
                float2 o = float2(x, y);
                float2 rnd = Hash22(id + o + seed * 5.3f);
                if (rnd.x < 0.62f) continue; // 6 割のセルは空（多いと雨粒に見える）
                float2 c = o + (rnd - 0.5f) * 0.8f;
                float2 cq = (id + o + 0.5f + (rnd - 0.5f) * 0.8f - seed * 9.0f) / 3.2f; // 粒の中心（q 空間）
                float ring = length(cq);
                if (ring < 0.9f || ring > 1.6f) continue;
                // 外へ行くほど小さく
                float rad = (0.10f + 0.24f * rnd.y) * (1.0f - (ring - 0.9f) * 0.8f);
                spray = max(spray, 1.0f - length(f - c) / rad);
            }
        }
    }

    // --- 垂れ筋：本体の下半分から数本、時間とともに伸びる ---
    float drip = -1.0f;
    if (q.y > -0.2f) {
        const float kCols = 7.0f;
        float colX = q.x * kCols * 0.5f + seed * 13.0f; // q.x ∈ [-1,1] を 7 列ほどへ
        float colId = floor(colX);
        float fx = frac(colX) - 0.5f;
        float h = Hash11(colId * 1.7f + seed * 91.0f);
        if (h > 0.45f) {
            // 列の位置が本体の中なら、本体の縁から下へ伸ばす
            float colCenterQx = (colId + 0.5f - seed * 13.0f) / (kCols * 0.5f);
            if (abs(colCenterQx) < 0.85f) {
                float startY = sqrt(saturate(1.0f - colCenterQx * colCenterQx)) * 0.75f;
                // 最初の 0.25 秒で一気に、そのあとゆっくり伸びる（重さのある液体）
                float grow = saturate(age / 0.25f) * 0.45f + saturate(age / 3.5f) * 0.55f;
                float len = (0.35f + 1.05f * Hash11(colId * 3.1f + seed * 57.0f)) * grow;
                float t = (q.y - startY) / max(len, 0.001f); // 0: 付け根 〜 1: 先端
                if (t < 1.15f) {
                    // 先端のしずく（width × 1.35）が列の幅 0.5 をはみ出すと縦にスパッと切れるので、上限 0.36 に抑える
                    float width = lerp(0.30f, 0.14f, saturate(t)) * (0.7f + 0.5f * h);
                    float stripe = 1.0f - abs(fx) / width;
                    // 先端のしずく（少し太った玉）。列の幅を 1 とした単位で距離を測る
                    float2 tipD = float2(fx, (q.y - (startY + len)) * kCols * 0.5f);
                    float tip = 1.0f - length(tipD) / (width * 1.35f);
                    float cut = (t <= 1.0f) ? stripe : -1.0f;
                    drip = max(cut, tip);
                    // 付け根は本体とつながるよう、t < 0 でも本体の縁近くまで描く
                    if (t < 0.0f) drip = max(drip, stripe * saturate(1.0f + t * 4.0f));
                    // 輪郭だけ柔らかくした一定幅のマスクにする
                    dripMask = smoothstep(0.0f, 0.22f, drip);
                }
            }
        }
    }

    // 粒も本体より先に消えるよう弱めにする（残ると点々だけが浮いて見える）
    return max(body * 1.6f, spray * 0.6f);
}

// ---------------------------------------------------------------------------
// 本体
// ---------------------------------------------------------------------------

/// @brief 全ての墨を合成し、濁り・屈折・艶を加えて画面に重ねる
/// @param input フルスクリーン VS の出力
/// @return 出力色（α は 1 固定。墨も濁りも無ければ素通し）
PixelShaderOutput main(VertexShaderOutput input) {
    PixelShaderOutput output;
    float2 uv = input.texcoord;

    if (splatCount <= 0 && murk <= 0.001f) {
        output.color = gTexture.Sample(gSampler, uv);
        return output;
    }

    const float2 aspect = float2(aspectRatio, 1.0f);

    // --- 全墨の和集合 ---
    float ink = 0.0f;   // 0〜1 の最終被覆
    float core = 0.0f;  // 厚み（艶と色の深さに使う）
    float2 refr = 0.0f; // 縁での屈折オフセット（アスペクト補正空間）
    float rim = 0.0f;   // 左上を向いた縁のハイライト
    const float2 kLightDir = float2(-0.55f, -0.83f); // 光の来る向き（画面の左上。+Y が下）
    [loop]
    for (int i = 0; i < splatCount && i < kMaxSplats; ++i) {
        float4 A = splatA[i];
        float4 B = splatB[i];
        float strength = saturate(A.w);
        if (strength <= 0.001f) continue;

        float2 p = (uv - A.xy) * aspect;
        float dripMask;
        float f = InkField(p, A.z, B.x, B.y, dripMask);
        if (f <= -0.5f && dripMask <= 0.0f) continue;

        // 侵食：弱るほどしきい値が上がる。ノイズを混ぜて穴を開けながら縮ませる
        float holes = ValueNoise(p / A.z * 4.0f + B.y * 23.0f);
        float erode = (1.0f - strength) * (0.95f + 0.55f * holes) - 0.02f;
        float cov = smoothstep(erode, erode + 0.06f, f);
        // 最後の 3 割は全体をすっと薄くして消す（崩れだけで消すと小さな欠片が点々と残る）
        cov *= smoothstep(0.0f, 0.3f, strength);
        // 垂れ筋は幅を保ったまま、本体より先（強さ 0.8 → 0.45 の間）に薄くなって消える
        float drip = dripMask * smoothstep(0.45f, 0.8f, strength);
        cov = max(cov, drip);

        ink = 1.0f - (1.0f - ink) * (1.0f - cov);
        core = max(core, max(saturate((f - erode) * 1.4f) * cov, drip * 0.6f));

        // 縁の帯（輪郭で 1、内側へ入るほど 0）。屈折と艶はここだけに掛ける。
        // NOTE: 以前は ddx/ddy で墨の勾配を取っていたが、微分は 2x2 ピクセル単位で
        //       一定になるため、大きく増幅すると画面に四角い格子とギザギザが出た。
        //       墨の中心からの放射方向で代用し、微分は一切使わない。
        float thin = (f > -0.5f) ? cov * (1.0f - saturate((f - erode) * 3.0f)) : 0.0f;
        float2 dir = p / max(length(p), 0.0001f);
        refr += dir * thin;
        rim = max(rim, thin * saturate(dot(dir, kLightDir)));
    }

    // --- 濡れたレンズ感：墨の縁で背景をわずかに屈折させる（画面の高さの 0.8% まで）---
    float2 distortedUv = clamp(uv - refr * 0.008f / aspect, 0.0f, 1.0f);
    float3 scene = gTexture.Sample(gSampler, distortedUv).rgb;

    // --- 画面全体の濁り（墨が溶けて水が曇る）---
    float lum = dot(scene, float3(0.299f, 0.587f, 0.114f));
    float3 col = scene;
    if (murk > 0.001f) {
        float m = saturate(murk);
        col = lerp(col, lum.xxx, m * 0.55f);                       // 彩度を落とす
        col = lerp(col, inkColor.rgb * (0.4f + 0.6f * lum), m * 0.45f); // 墨色に寄せる
        // 周辺から暗くする（中央はまだ見える）
        float2 c = (uv - 0.5f) * aspect;
        float vig = saturate(dot(c, c) * 1.6f);
        col *= lerp(1.0f, 0.35f, m * (0.35f + 0.65f * vig));
    }

    if (ink <= 0.002f) {
        output.color = float4(col, 1.0f);
        return output;
    }

    // --- 墨の色：厚いところほど黒く、薄い縁だけ少し透ける ---
    float3 inkBase = inkColor.rgb * lerp(1.0f, 0.35f, core);
    // 薄い縁は背景の明暗をわずかに透かす（完全な黒ベタは切り抜きに見える）
    inkBase += scene * 0.10f * (1.0f - core);

    // --- 艶：左上を向いた縁にだけ細いハイライト ---
    // ゆっくりぬめる揺らぎ（固まったペンキに見せない）
    float sheen = 0.75f + 0.25f * sin(time * 1.3f + uv.y * 9.0f + uv.x * 5.0f);
    inkBase += rim * rim * 0.30f * sheen;

    col = lerp(col, inkBase, saturate(ink * inkColor.a));

    output.color = float4(col, 1.0f);
    return output;
}
