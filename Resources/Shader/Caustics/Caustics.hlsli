#ifndef CHASO_CAUSTICS_HLSLI
#define CHASO_CAUSTICS_HLSLI

// =============================================================================
// Caustics 用の手続き型ノイズ
//
// Caustics.PS.hlsl（床に落ちる網目）と LightShaft.PS.hlsl（降り注ぐ光柱）が
// 共有する。同じ関数・同じ time / scale / speed を渡すことで、
// 光柱の断面と床の網目が同じ模様になり、両者が自然に一致する。
// =============================================================================

static const float kTau = 6.28318530718f; ///< 2π

// -----------------------------------------------------------------------------
// 2 次元ハッシュ（セル ID から特徴点の位相を生成する）
// -----------------------------------------------------------------------------
/// @brief セル座標から 0〜1 の 2 次元ハッシュを作る
/// @param p セル座標
/// @return 0〜1 の 2 成分の擬似乱数
float2 CausticsHash2(float2 p) {
    p = float2(dot(p, float2(127.1f, 311.7f)),
               dot(p, float2(269.5f, 183.3f)));
    return frac(sin(p) * 43758.5453123f);
}

// -----------------------------------------------------------------------------
// Voronoi（F1 = 最近傍距離, F2 = 2 番目に近い距離）
// 特徴点を sin で周回させることで模様がうねる。
// -----------------------------------------------------------------------------
/// @brief 周囲 3x3 セルの特徴点から F1 / F2 を求める
/// @param uv    評価する座標（1 単位 = 1 セル）
/// @param phase 特徴点を周回させる位相
/// @param f1    最近傍の特徴点までの距離（出力）
/// @param f2    2 番目に近い特徴点までの距離（出力）
void CausticsVoronoi(float2 uv, float phase, out float f1, out float f2) {
    float2 cell = floor(uv);
    float2 local = frac(uv);

    f1 = 8.0f;
    f2 = 8.0f;

    [unroll]
    for (int y = -1; y <= 1; ++y) {
        [unroll]
        for (int x = -1; x <= 1; ++x) {
            float2 offset = float2(x, y);
            float2 h = CausticsHash2(cell + offset);

            // 特徴点をセル内で円運動させる（0.42 はセルからはみ出さない範囲）
            // ※ 'point' は HLSL の予約語なので変数名に使えない
            float2 featurePoint = offset + 0.5f + 0.42f * sin(phase + kTau * h);

            float d = length(featurePoint - local);
            if (d < f1) {
                f2 = f1;
                f1 = d;
            } else if (d < f2) {
                f2 = d;
            }
        }
    }
}

// -----------------------------------------------------------------------------
// Voronoi のセル境界を網目として抽出する
// F2 - F1 はセル境界上で 0 になるので、そこを明るい線にする。
// width を小さくすると細くシャープな網目になる。
// broad にはセル中心ほど明るい滑らかな場が返る（強度のばらつき用）。
// -----------------------------------------------------------------------------
/// @brief Voronoi のセル境界を網目の強さとして返す
/// @param uv    評価する座標
/// @param phase 特徴点を周回させる位相
/// @param width 網目の線の太さ（F2 - F1 の幅）
/// @param broad セル中心ほど明るい滑らかな値（出力。0〜1）
/// @return 網目の強さ（境界上で 1）
float CausticsNet(float2 uv, float phase, float width, out float broad) {
    float f1, f2;
    CausticsVoronoi(uv, phase, f1, f2);

    broad = 1.0f - saturate(f1);
    return 1.0f - smoothstep(0.0f, width, f2 - f1);
}

// -----------------------------------------------------------------------------
// 2 層の網目を合成して最終的な caustics 強度を得る
// -----------------------------------------------------------------------------
/// @brief 2 層の網目を合成した caustics 強度を求める
/// @param worldUV  スケール適用済みのワールド XZ 座標
/// @param time     経過時間
/// @param speed    うねる速度
/// @param contrast pow の指数（大きいほど細くシャープ）
/// @return caustics 強度（0〜1）
float CausticsPattern(float2 worldUV, float time, float speed, float contrast) {
    float phaseA = time * speed;
    float phaseB = time * speed * 1.37f + 2.4f;

    // 逆方向にスクロールさせると特定方向への「流れ」が消えて自然になる
    float2 uvA = worldUV + float2(0.11f, 0.07f) * time * speed;
    float2 uvB = worldUV * 2.13f - float2(0.09f, 0.13f) * time * speed;

    float broadA, broadB;
    float netA = CausticsNet(uvA, phaseA, 0.32f, broadA);
    float netB = CausticsNet(uvB, phaseB, 0.26f, broadB);

    // 網目の「重ね合わせ」。実際の caustics も複数の網が重なって見える。
    // ※ スパークル寄りの見た目にしたい場合は max を min に変えると
    //   2 つの網が交差した点だけが光るようになる。
    float net = max(netA, netB * 0.8f);

    // 強度にムラを付ける（一様に光ると嘘っぽくなる）
    net *= lerp(0.55f, 1.0f, broadA * broadB);

    return pow(saturate(net), contrast);
}

// -----------------------------------------------------------------------------
// 軽量版：レイマーチの各サンプルで呼ぶための 1 層だけの網目
//
// CausticsPattern は Voronoi を 2 層評価する（1 呼び出しで 18 回ハッシュ）ため、
// レイマーチで数十回呼ぶと現実的なコストに収まらない。
// 光柱の断面はソフトに見えるので 1 層で十分。
//
// CausticsPattern の「1 層目」と完全に同じ UV・位相・width を使うので、
// 床の網目の主要な網（1 層目）と光柱の位置は一致する。
// 2 層目の細かい網と broadB による強度ムラは省いているため、
// 完全に同一のパターンではなく「支配的な網が一致する」近似。
// -----------------------------------------------------------------------------
/// @brief CausticsPattern の 1 層目だけを評価する軽量版
/// @param worldUV  スケール適用済みのワールド XZ 座標
/// @param time     経過時間
/// @param speed    うねる速度
/// @param contrast pow の指数（大きいほど細くシャープ）
/// @return caustics 強度（0〜1）
float CausticsPatternCheap(float2 worldUV, float time, float speed, float contrast) {
    float phase = time * speed;
    float2 uv = worldUV + float2(0.11f, 0.07f) * time * speed;

    float broad;
    float net = CausticsNet(uv, phase, 0.32f, broad);
    net *= lerp(0.55f, 1.0f, broad);

    return pow(saturate(net), contrast);
}

#endif // CHASO_CAUSTICS_HLSLI
