#include "../Fullscreen/Fullscreen.hlsli"

// ============================================================================
// VisionBleed（意識が遠のく視界）
//   力尽きたときに、視界の周辺から「水彩の滲み」が内側へ入り込んでくるポストエフェクト
//   （visionbleed PSO）。テクスチャは使わず、低周波ノイズだけで作る。
//
//   - 滲み（bleed）: 周辺ほど強い、なめらかなぼかし・色のにじみ・減光。
//                    境界はドメインワープした fBm で、水に落としたインクのように
//                    不規則にうねりながらゆっくり広がる。
//                    濡れた領域の縁はわずかに濃くする（水彩の「縁の溜まり」）。
//                    濡れた領域の中には大きな色ムラ（pigment）をゆっくり漂わせる。
//                    粒状のノイズは使わない（砂嵐・テレビのざらつきに見えるため）。
//   - 二重像（ghost）: 画面を左右にずらした 2 枚を重ねる。焦点が合わない感じ。
//                      ホスト側で瞼が開き直るたびに跳ね上げ、ゆっくり 0 へ戻す。
//
//   パラメータは b0 のルート定数 4 つで足りるので専用 CBuffer は持たない。
//   積む順は Underwater などの「景色」系より後、BloodOverlay / InkOverlay（レンズの表面）
//   より前（DeathScreenFx がこの並びを保つ）。
// ============================================================================

Texture2D<float4> gTexture : register(t0);
SamplerState gSamplerClamp : register(s2); // リニア＋クランプ（ずらした UV が反対側へ回り込まない）

// PostEffectConstants (b0)
cbuffer VisionBleedParams : register(b0) {
    float bleed;   ///< 滲みの進み具合（0: なし 〜 1: 中央まで滲む）
    float ghost;   ///< 二重像のずれ（UV。画面幅に対する割合）
    float pigment; ///< 濡れた領域の色ムラの強さ（0〜1）
    float time;    ///< 経過時間（秒。PostProcess の内部時間）
};

/// @struct PixelShaderOutput
/// @brief ピクセル出力
struct PixelShaderOutput {
    float4 color : SV_TARGET0; ///< 出力色
};

// ---------------------------------------------------------------------------
// ノイズ（すべて低周波。画素単位の乱数は使わない）
// ---------------------------------------------------------------------------

/// @brief 2D 座標から 0〜1 の擬似乱数を作る（格子点用）
/// @param p 入力座標
/// @return 0〜1 の擬似乱数
float Hash21(float2 p) {
    p = frac(p * float2(127.1f, 311.7f));
    p += dot(p, p + 45.32f);
    return frac(p.x * p.y);
}

/// @brief 値ノイズ（格子点の乱数を滑らかに補間）
/// @param p 入力座標
/// @return 0〜1 のノイズ値
float ValueNoise(float2 p) {
    float2 i = floor(p);
    float2 f = frac(p);
    float2 u = f * f * (3.0f - 2.0f * f);
    float a = Hash21(i + float2(0.0f, 0.0f));
    float b = Hash21(i + float2(1.0f, 0.0f));
    float c = Hash21(i + float2(0.0f, 1.0f));
    float d = Hash21(i + float2(1.0f, 1.0f));
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

/// @brief 4 オクターブの fBm（おおよそ 0〜1）
/// @param p 入力座標
/// @return ノイズ値
float Fbm(float2 p) {
    float v = 0.0f;
    float a = 0.5f;
    [unroll]
    for (int i = 0; i < 4; ++i) {
        v += a * ValueNoise(p);
        p = p * 2.03f + 17.1f;
        a *= 0.5f;
    }
    return v / 0.9375f;
}

// ---------------------------------------------------------------------------
// サンプリング
// ---------------------------------------------------------------------------

/// @brief 二重像を考慮して 1 点を読む
/// @param uv   読む位置
/// @param gOff 二重像のずれ（UV）
/// @return RGB
float3 Tap(float2 uv, float2 gOff) {
    [branch]
    if (ghost > 1e-5f) {
        // 片方をやや濃くすると「どちらが本物か分からない」感じが出る
        return gTexture.SampleLevel(gSamplerClamp, uv + gOff, 0.0f).rgb * 0.56f +
               gTexture.SampleLevel(gSamplerClamp, uv - gOff, 0.0f).rgb * 0.44f;
    }
    return gTexture.SampleLevel(gSamplerClamp, uv, 0.0f).rgb;
}

/// @brief 色のにじみ付きで 1 点を読む（R を外側、B を内側へずらす）
/// @param uv   読む位置
/// @param dir  画面中心から外向きの単位ベクトル
/// @param ca   ずらす量（UV）
/// @param gOff 二重像のずれ（UV）
/// @return RGB
float3 TapBleedColor(float2 uv, float2 dir, float ca, float2 gOff) {
    return float3(Tap(uv + dir * ca, gOff).r, Tap(uv, gOff).g, Tap(uv - dir * ca, gOff).b);
}

/// @brief 同心リング 1 本分のサンプルを足し込む（なめらかさのため 3 本を半径をずらして重ねる）
/// @param acc    足し込み先
/// @param wsum   重みの合計
/// @param uv     中心
/// @param radius リングの半径（UV。縦横それぞれ）
/// @param count  リング上のサンプル数
/// @param phase  リングの回転（隣のリングとサンプル位置が重ならないように）
/// @param w      このリングの重み
/// @param gOff   二重像のずれ（UV）
void AddRing(inout float3 acc, inout float wsum, float2 uv, float2 radius, int count,
             float phase, float w, float2 gOff) {
    const float angStep = 6.2831853f / count;
    for (int i = 0; i < count; ++i) {
        const float a = phase + angStep * i;
        acc += Tap(uv + float2(cos(a), sin(a)) * radius, gOff) * w;
        wsum += w;
    }
}

/// @brief 周辺から水彩のように滲むぼかし・色のにじみ・減光と二重像を掛ける
/// @param input フルスクリーン VS の出力
/// @return 出力色
PixelShaderOutput main(VertexShaderOutput input) {
    PixelShaderOutput output;
    const float2 uv = input.texcoord;
    const float4 src = gTexture.SampleLevel(gSamplerClamp, uv, 0.0f);

    if (bleed <= 0.001f && ghost <= 1e-5f) {
        output.color = src;
        return output;
    }

    uint width, height;
    gTexture.GetDimensions(width, height);
    const float aspect = (float)width / (float)max(height, 1u);
    const float2 texel = float2(1.0f / width, 1.0f / height);

    // 画面中心からの距離（アスペクト補正。四隅で 1）
    const float2 d = uv - 0.5f;
    const float r = length(float2(d.x * aspect, d.y)) / length(float2(0.5f * aspect, 0.5f));
    const float2 dir = (dot(d, d) > 1e-8f) ? normalize(d) : float2(0.0f, 0.0f);

    // --- 滲みのマスク（0: 中央・はっきり 〜 1: 周辺・滲みきった）---
    // ドメインワープした fBm で、インクが水に広がるような巻き込みのある縁にする
    const float2 p = float2(uv.x * aspect, uv.y) * 2.2f;
    const float2 q = float2(Fbm(p + float2(0.0f, time * 0.05f)),
                            Fbm(p + float2(5.2f, 1.3f) - time * 0.04f));
    const float n = Fbm(p + q * 2.2f + float2(time * 0.03f, -time * 0.02f));
    const float edge = r + (n - 0.5f) * 0.45f;
    const float inner = lerp(1.05f, -0.05f, bleed); // bleed = 1 で中央まで入り込む
    float m = smoothstep(inner, inner + 0.45f, edge);
    m *= saturate(bleed * 6.0f); // ごく小さい bleed で四隅だけが急に出ないように

    // 色が外へ流れ出す：中心寄りを読むことで、像が外側へ引きずられて見える
    const float2 run = -dir * 0.02f * m * bleed;
    // 濡れたところのゆるいうねり
    const float2 warp = (q - 0.5f) * 0.018f * m * bleed + run;
    // 色のにじみ（滲んだところだけ）
    const float ca = 0.006f * m * bleed;
    // 二重像：主に左右。縦はゆっくり揺らす
    const float2 gOff = float2(ghost, ghost * 0.3f * sin(time * 1.7f));

    const float3 sharp = TapBleedColor(uv + warp, dir, ca, gOff);

    // --- なめらかなぼかし（半径をずらした 3 本の同心リング。回転は固定なのでざらつかない）---
    float3 col = sharp;
    const float radiusPx = 20.0f * (height / 720.0f) * m * bleed;
    [branch]
    if (radiusPx > 0.5f) {
        float3 acc = sharp;
        float wsum = 1.0f;
        const float2 c = uv + warp;
        const float2 rad = radiusPx * texel;
        AddRing(acc, wsum, c, rad * 0.34f, 6, 0.00f, 0.90f, gOff);
        AddRing(acc, wsum, c, rad * 0.67f, 10, 0.31f, 0.70f, gOff);
        AddRing(acc, wsum, c, rad * 1.00f, 14, 0.13f, 0.50f, gOff);
        const float3 blurred = acc / wsum;
        col = lerp(sharp, blurred, saturate(m * 1.3f));
    }

    // --- 水彩の縁の溜まり（濡れた領域の境目だけ少し濃くなる）---
    const float rim = smoothstep(0.05f, 0.45f, m) * (1.0f - smoothstep(0.45f, 0.9f, m));
    col *= 1.0f - rim * 0.3f * saturate(bleed * 2.0f);

    // --- 色ムラ（濡れた領域の中に大きな濃淡がゆっくり漂う。粒ではなく染み）---
    const float blot = Fbm(p * 3.1f + q * 1.5f + 11.3f);
    col *= lerp(1.0f, 0.78f + 0.44f * blot, m * pigment);

    // --- 減光と色の抜け（周辺から暗く狭くなる）---
    const float luma = dot(col, float3(0.2125f, 0.7154f, 0.0721f));
    col = lerp(col, luma.xxx, m * 0.45f);
    col *= 1.0f - m * bleed * 0.7f;

    output.color = float4(saturate(col), src.a);
    return output;
}
