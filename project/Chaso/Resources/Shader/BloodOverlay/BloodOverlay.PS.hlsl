#include "../Fullscreen/Fullscreen.hlsli"

// ============================================================================
// BloodOverlay
//   プレイヤーが被弾／瀕死のときに、画面の周辺へ血がへばりつくポストエフェクト
//   （bloodoverlay PSO）。
//   テクスチャは使わず、ハッシュノイズだけで飛沫・にじみ・濡れた艶を作る。
//
//   駆動は 2 系統ある（強いほうが採用される）。
//     hitFlash  : 被弾の瞬間に跳ね上がり、ホスト側で短時間で 0 へ落とす
//     lowHealth : HP が閾値を下回っている間ずっと掛かる。心拍で脈動する
//
//   パターン自体は time に依存させていない（テレビの砂嵐のようにチラつかせない）。
//   血の形を変えたいときはホスト側から seed を振り直す。
// ============================================================================

/// @struct PixelShaderOutput
/// @brief ピクセル出力
struct PixelShaderOutput {
    float4 color : SV_TARGET0; ///< 出力色
};

Texture2D<float4> gTexture : register(t0);
SamplerState gSampler : register(s0);

// b1: BloodOverlay パラメータ（合計 64 バイト = float4 × 4）
cbuffer BloodOverlayParams : register(b1) {
    float time;           ///< 経過時間（脈動の位相にのみ使う）
    float hitFlash;       ///< 被弾フラッシュ（0.0〜1.0）
    float lowHealth;      ///< 低 HP 持続（0.0〜1.0）
    float pulseSpeed;     ///< 脈動の速さ（1.0 で毎秒 1 周期）

    float4 bloodColor;    ///< 血の色（RGB。A は未使用）

    float coverage;       ///< 最大時に画面の何割まで血が侵食するか（0.0〜1.0）
    float splatterScale;  ///< 飛沫の密度（大きいほど細かい粒になる）
    float desaturate;     ///< 血の下の彩度をどれだけ落とすか（0.0〜1.0）
    float aspectRatio;    ///< アスペクト比（Width / Height）

    float seed;           ///< 飛沫パターンの種（0〜1）。被弾のたびに振り直す
    float3 _padding;      ///< 16 バイト境界合わせ
};

// ---------------------------------------------------------------------------
// ノイズ
// ---------------------------------------------------------------------------

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
    float2 u = f * f * (3.0f - 2.0f * f); // smoothstep 補間

    float a = Hash21(i + float2(0.0f, 0.0f));
    float b = Hash21(i + float2(1.0f, 0.0f));
    float c = Hash21(i + float2(0.0f, 1.0f));
    float d = Hash21(i + float2(1.0f, 1.0f));

    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

/// @brief 4 オクターブの fBm。血の境界をギザギザにするのに使う
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

/// @brief セル状の血だまり／飛沫
/// @param uv        画面 UV（アスペクト補正済みを想定）
/// @param scale     グリッド密度。大きいほど細かい粒
/// @param cellSeed  セル乱数のオフセット。層ごとに変える
/// @param dripBias  1.0 で下方向に伸びた涙形（垂れた血）、0.0 で正円
/// @param cull      このしきい値未満のセルは空にする（大きいほどまばら）
/// @return 0.0（血なし）〜 1.0（粒の中心）
float Splatter(float2 uv, float scale, float cellSeed, float dripBias, float cull) {
    float2 g = uv * scale + cellSeed;
    float2 id = floor(g);
    float2 f = frac(g) - 0.5f;

    float best = 1.0f;

    // 3x3 の近傍セルを見て、いちばん近い粒までの正規化距離を取る
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            float2 o = float2(x, y);
            float2 rnd = Hash22(id + o + cellSeed);

            // 一定割合のセルを空にして、粒が並びすぎないようにする
            if (rnd.x < cull) {
                continue;
            }

            float2 center = o + (rnd - 0.5f) * 0.85f;
            float2 d = f - center;

            // 下側だけ縦に引き伸ばして、垂れた血の涙形にする
            if (d.y > 0.0f) {
                d.y /= (1.0f + dripBias * 1.8f);
            }

            float radius = 0.12f + rnd.y * 0.24f;
            best = min(best, length(d) / radius);
        }
    }

    // フチを少しだけぼかす（1.0 でシャープな輪郭、0 で外側）
    return 1.0f - smoothstep(0.55f, 1.0f, best);
}

/// @brief 心拍。1 周期に「ドクン・ドクン」の 2 拍が入る
/// @param t 位相（1.0 で 1 周期）
/// @return 拍の強さ（0〜1）
/// @note ガウス山は pow() ではなく自乗で作る。pow() は底が負だと結果が未定義で、
///       (p - 0.02) は周期の頭で普通に負になる。
float Heartbeat(float t) {
    float p = frac(t);
    float d1 = (p - 0.02f) * 9.0f;
    float d2 = (p - 0.26f) * 11.0f;
    return saturate(exp(-d1 * d1) + exp(-d2 * d2) * 0.62f);
}

// ---------------------------------------------------------------------------
// 本体
// ---------------------------------------------------------------------------

/// @brief 画面周辺に血だまりと飛沫を重ね、被弾時は全体に赤味を差す
/// @param input フルスクリーン VS の出力
/// @return 出力色（α は 1 固定。被弾・瀕死どちらも無ければ素通し）
PixelShaderOutput main(VertexShaderOutput input) {
    PixelShaderOutput output;

    float2 uv = input.texcoord;

    // 被弾も瀕死も無ければ完全に素通し（PSO を積んだままでも実質ゼロコスト）
    // 脈の底は 0.80。ここを下げると拍と拍の間で血が引きすぎて、
    // 「瀕死が続いている」ではなく「点滅している」ように見える。
    float pulse = lowHealth * lerp(0.80f, 1.0f, Heartbeat(time * pulseSpeed));
    float amount = saturate(max(hitFlash, pulse));
    if (amount <= 0.001f) {
        output.color = gTexture.Sample(gSampler, uv);
        return output;
    }

    // --- 画面端からの距離（スーパー楕円 p = 4）---
    // 放射距離だと 16:9 では上下の辺が 0.5 までしか行かず、左右にだけ血が寄る。
    // かといって矩形距離（max）だと四隅が四辺と同じ濃さで平坦になる。
    // (|x|^4 + |y|^4)^(1/4) なら四辺がちょうど 1.0、四隅が 1.19 になり、
    // 「隅から滲み出して辺へ広がる」という欲しい順序になる。
    float2 c = abs(uv - 0.5f) * 2.0f; // 0: 中心 〜 1: 辺
    float2 c2 = c * c;
    float edge = sqrt(sqrt(dot(c2, c2))); // = (c.x^4 + c.y^4)^(1/4)

    // --- 境界を fBm で崩す（きれいな楕円のままだとただのビネットに見える）---
    // NOTE: seed は [0, 1)。掛ける係数を大きくすると座標が大きくなり、
    //       Hash21 内の frac() が拾える刻みが粗くなってパターンの種類が減る。
    float2 noiseUv = uv * float2(aspectRatio, 1.0f) * 3.2f + seed * 7.0f;
    float warp = (Fbm(noiseUv) - 0.5f) * 0.42f;
    float edgeWarped = max(edge + warp, 0.0f); // 四隅の 1.19 を潰さないよう上は clamp しない

    // --- 侵食量。amount が大きいほど内側まで血が来る ---
    float reach = coverage * amount;
    float threshold = 1.0f - reach;
    float band = smoothstep(threshold, threshold + 0.34f, edgeWarped);

    // 血が絶対に届かない内側は、ここで打ち切って Splatter（9 セル探索 × 2 層）を丸ごと省く。
    // 画面の大半はこちらに落ちるので、これがこのパスの実効コストを決めている。
    // 返り血の帯（下の inward）より少しだけ広く取ること。
    if (edgeWarped < threshold - 0.16f) {
        output.color = gTexture.Sample(gSampler, uv);
        return output;
    }

    // --- 飛沫（2 層）。縁の血だまり ＋ まばらな返り血 ---
    float2 splatUv = uv * float2(aspectRatio, 1.0f);
    float blobCoarse = Splatter(splatUv, splatterScale * 5.0f, seed * 3.7f, 0.35f, 0.30f);
    float blobFine = Splatter(splatUv + 0.37f, splatterScale * 17.0f, seed * 2.9f + 4.2f, 0.85f, 0.74f);

    // 円が並んで見えないよう、粗い層を高周波ノイズで食い破る（1 オクターブで足りる）
    float grain = ValueNoise(splatUv * 26.0f + seed * 5.0f);
    blobCoarse = saturate(blobCoarse * (0.42f + 1.10f * grain));

    // 縁の血だまり。いちばん外側だけは飛沫に関係なく厚く残す
    float pool = band * (0.55f + 0.45f * blobCoarse);
    pool = max(pool, smoothstep(0.90f, 1.14f, edgeWarped) * amount);

    // 返り血は帯のすぐ内側だけ。奥まで飛ばすと窓の雨粒にしか見えない
    float inward = smoothstep(threshold - 0.14f, threshold + 0.04f, edgeWarped) *
                   (1.0f - smoothstep(threshold + 0.10f, threshold + 0.34f, edgeWarped));
    float spray = blobFine * inward * 0.75f;

    float density = saturate(max(pool, spray) * amount);
    if (density <= 0.002f) {
        output.color = gTexture.Sample(gSampler, uv);
        return output;
    }

    // --- 濡れたレンズ感：血が厚いところで背景を外向きにわずかに引っ張る ---
    // NOTE: Splatter の勾配を差分で取ると 9 セル探索がもう 2 回走って重い。
    //       見た目の寄与は小さいので、放射方向 × fBm の揺らぎで代用している。
    float2 toEdge = (uv - 0.5f) * float2(aspectRatio, 1.0f);
    float2 dir = toEdge / max(length(toEdge), 0.0001f);
    float2 distortedUv = clamp(uv + dir * density * warp * 0.020f, 0.0f, 1.0f);

    float3 scene = gTexture.Sample(gSampler, distortedUv).rgb;

    // --- 合成 ---
    // 1) 血の下は彩度を落として、赤だけが立つようにする
    float lum = dot(scene, float3(0.299f, 0.587f, 0.114f));
    float3 col = lerp(scene, lum.xxx, saturate(desaturate * density));

    // 2) 血で暗くなる（厚いところほど落ちる）
    col *= lerp(1.0f, 0.30f, density);

    // 3) 血の色を被せる。背景の明暗は残して「透けた血」に見せる。
    //    厚いところは血の色で塗り切る（0.92 止まりだと下の色が透けてピンクに転ぶ）
    float3 tinted = bloodColor.rgb * (0.30f + 0.70f * lum);
    col = lerp(col, tinted, saturate(density * 1.25f));

    // 4) 粒のフチに濡れた艶（内側の縁だけ薄く光らせる）
    float rim = saturate(blobCoarse - blobCoarse * blobCoarse) * 4.0f;
    col += bloodColor.rgb * rim * density * 0.18f;

    // 5) 被弾の瞬間だけ、画面全体にうっすら赤味を差す（血の外側にも衝撃を伝える）
    //    明るくしすぎるとピンクに転ぶので、緑と青を落として赤を残すだけにする
    col = lerp(col, col * float3(1.10f, 0.55f, 0.58f), saturate(hitFlash) * 0.35f);

    output.color = float4(col, 1.0f);
    return output;
}
