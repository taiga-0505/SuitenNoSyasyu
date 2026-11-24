// =============================================================================
// WaveSimulation.CS.hlsl
// 2D 波動方程式を用いた波の伝播シミュレーション（インタラクティブ波紋のハイトマップを更新する）。
// 速度成分にのみ減衰をかけることで、波が通過した後に水面が元の高さに戻る。
//
// テクスチャは R32G32B32A32_FLOAT:
//   R = 波紋の高さ（m）。従来どおり波動方程式で伝播する
//   G = 引き波の泡（0〜kFoamMax）。波としては伝播せず、その場に残って滲みながらゆっくり消える
//       （船尾のスクリューや尾がかき回した、白く濁った帯）
//   B = 波頭の白波（0〜kFoamMax）。すぐ消える（毎フレーム置き直す前提）。
//       船首の砕け波や V 字（ケルビン波）の腕の白い線のように「今その場所にだけある白」に使う。
//       長く残すと腕が外へ動いた跡が塗りつぶされて V にならないため、残存率を小さくしている
//   A = 墨（0〜kInkMax）。水面に落ちた墨の濃さ。G と同じく伝播せず、周りへ滲みながらゆっくり薄れる
//
// テクスチャが覆う範囲（窓）はカメラに合わせて動く。動いたぶん（gShiftPrev / gShiftPrevPrev）だけ
// 前フレームの中身をずらして読むことで、波紋・泡・墨はワールドに貼り付いたまま残る。
// 窓の外から入ってきた所は 0（何も無い水面）から始まる。
// =============================================================================

cbuffer SimulationParams : register(b0)
{
    float gAlpha;        ///< 波の伝播速度係数（c^2 * dt^2 / dx^2）
    float gDamping;      ///< 減衰係数（速度にのみ適用。例: 0.95〜0.99）
    int   gSourceCount;  ///< 現在の波源の数
    float gResetFlag;    ///< 1.0 なら状態をゼロにリセットするフラグ

    // x, y = UV 座標（0.0〜1.0。今回の窓の UV）
    // z = 波源の半径（UV 空間での半径）
    // w = 波源の強さ（高さの加算値）
    float4 gSources[64]; ///< 波紋の波源

    // ---- 引き波の泡（G）----
    float gFoamDecay;       ///< 毎フレームの残存率（例: 0.992）
    float gFoamSpread;      ///< 毎フレームの滲み（隣との平均へ寄せる割合。0〜0.25）
    int   gFoamSourceCount; ///< 泡の波源の数
    float gCrestDecay;      ///< 波頭（B）の毎フレーム残存率（例: 0.8）

    // x, y = UV 座標 / z = 半径（UV）/ w = このフレームに足す量
    float4 gFoamSources[64]; ///< 引き波の泡の波源

    // ---- 波頭の白波（B）----
    int    gCrestSourceCount;  ///< 波頭の波源の数
    float3 gCrestPad;          ///< 16 バイト境界合わせ
    float4 gCrestSources[128]; ///< 波頭の白波の波源（xy: UV 座標、z: 半径（UV）、w: このフレームに足す量）

    // ---- 窓の移動 ----
    int2   gShiftPrev;      ///< t0（1 フレーム前）を読むときのずらし量（テクセル）
    int2   gShiftPrevPrev;  ///< t1（2 フレーム前）を読むときのずらし量（テクセル）

    // ---- 墨（A）----
    float  gInkDecay;        ///< 毎フレームの残存率（例: 0.997）
    float  gInkSpread;       ///< 毎フレームの滲み（0〜0.25）
    int    gInkSourceCount;  ///< 墨の波源の数
    float  gInkPad;          ///< 16 バイト境界合わせ
    float4 gInkSources[64];  ///< 墨の波源（xy: UV 座標、z: 半径（UV）、w: このフレームに足す量）
};

static const float kFoamMax = 2.0f; ///< 泡（G / B）の上限
static const float kInkMax = 2.0f;  ///< 墨（A）の上限

// t0: 1 フレーム前のハイトマップ
Texture2D<float4> gPrevHeight : register(t0);
// t1: 2 フレーム前のハイトマップ
Texture2D<float4> gPrevPrevHeight : register(t1);

// u0: 出力先のハイトマップ
RWTexture2D<float4> gOutHeight : register(u0);

/// @brief 半径内で (1 - t²) の重みを返す（範囲外は 0）
/// @param uv  評価する位置（UV）
/// @param src 波源（xy: UV 座標、z: 半径（UV））
/// @return 重み（0〜1）
float Falloff(float2 uv, float4 src)
{
    float radius = max(src.z, 1e-5f);
    float t = distance(uv, src.xy) / radius;
    return (t < 1.0f) ? (1.0f - t * t) : 0.0f;
}

/// @brief 前フレームのテクスチャを、窓の移動ぶんずらして読む（前フレームの窓の外なら 0）
/// @param tex   読むテクスチャ
/// @param p     今回の窓でのテクセル座標
/// @param shift 今回の中心 − そのテクスチャを書いたときの中心（テクセル）
/// @param size  テクスチャの一辺（テクセル）
/// @return その場所の値
float4 LoadShifted(Texture2D<float4> tex, int2 p, int2 shift, int2 size)
{
    int2 q = p + shift;
    if (any(q < 0) || any(q >= size))
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    return tex.Load(int3(q, 0));
}

/// @brief エントリポイント。1 スレッドが 1 テクセルを 1 ステップ進める
/// @param DTid ディスパッチスレッド ID（xy がテクセル座標）
[numthreads(16, 16, 1)]
void main( uint3 DTid : SV_DispatchThreadID )
{
    uint width, height;
    gOutHeight.GetDimensions(width, height);

    if (DTid.x >= width || DTid.y >= height)
        return;

    int2 size = int2((int)width, (int)height);
    int2 pos = int2(DTid.xy);

    // リセットフラグが立っている場合は強制的に 0 クリア
    if (gResetFlag > 0.5f)
    {
        gOutHeight[DTid.xy] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    float4 c1 = LoadShifted(gPrevHeight, pos, gShiftPrev, size);
    float h1 = c1.x;
    float h2 = LoadShifted(gPrevPrevHeight, pos, gShiftPrevPrev, size).x;

    // 上下左右のピクセル値を取得（境界では自分自身をクランプ扱い。クランプは今回の窓で行う）
    int xLeft  = max(pos.x - 1, 0);
    int xRight = min(pos.x + 1, size.x - 1);
    int yUp    = max(pos.y - 1, 0);
    int yDown  = min(pos.y + 1, size.y - 1);

    float4 cLeft  = LoadShifted(gPrevHeight, int2(xLeft, pos.y), gShiftPrev, size);
    float4 cRight = LoadShifted(gPrevHeight, int2(xRight, pos.y), gShiftPrev, size);
    float4 cUp    = LoadShifted(gPrevHeight, int2(pos.x, yUp), gShiftPrev, size);
    float4 cDown  = LoadShifted(gPrevHeight, int2(pos.x, yDown), gShiftPrev, size);

    // =========================
    // 高さ（R）：波動方程式
    // =========================
    // velocity = h1 - h2（前フレームとの差が速度に相当）
    // laplacian = 空間的な広がり
    float laplacian = cLeft.x + cRight.x + cUp.x + cDown.x - 4.0f * h1;

    // 1. Verlet 積分に基づく波動方程式（局所的な速度減衰 gDamping を適用）
    float hNew = h1 + (h1 - h2) * gDamping + gAlpha * laplacian;
    // 2. 水面全体が自然に 0（平坦）に戻るように、全体に微小な減衰をかける
    hNew *= 0.99f;

    // 波源（力）の加算。UV はテクセル中心（水面シェーダーのサンプル位置・CPU の読み戻しと同じ規則）
    float2 uv = (float2(pos) + 0.5f) / float2(size);
    for (int i = 0; i < gSourceCount; ++i)
    {
        float2 sourceUV = gSources[i].xy;
        float radius = gSources[i].z;
        float strength = gSources[i].w;

        float dist = distance(uv, sourceUV);
        if (dist < radius)
        {
            // スムーズな減衰関数
            float t = dist / radius;
            float falloff = (1.0f - t * t);
            falloff *= falloff; // (1 - t²)² — なだらかに減衰
            hNew += strength * falloff;
        }
    }

    // 発散（NaN / Infinity）を防ぐためのクランプ
    hNew = clamp(hNew, -0.5f, 0.5f);

    // =========================
    // 引き波の泡（G）：その場に残り、滲みながら消える
    // =========================
    float fAvg = (cLeft.y + cRight.y + cUp.y + cDown.y) * 0.25f;
    float fNew = lerp(c1.y, fAvg, saturate(gFoamSpread));
    // 薄い泡がいつまでも残らないよう、残存率に加えてわずかに引き算する
    fNew = fNew * saturate(gFoamDecay) - 0.0004f;
    for (int k = 0; k < gFoamSourceCount; ++k)
    {
        fNew += gFoamSources[k].w * Falloff(uv, gFoamSources[k]);
    }
    fNew = clamp(fNew, 0.0f, kFoamMax);

    // =========================
    // 波頭の白波（B）：すぐ消える。滲ませない（線をくっきり保つ）
    // =========================
    float cNew = c1.z * saturate(gCrestDecay);
    for (int m = 0; m < gCrestSourceCount; ++m)
    {
        cNew += gCrestSources[m].w * Falloff(uv, gCrestSources[m]);
    }
    cNew = clamp(cNew, 0.0f, kFoamMax);

    // =========================
    // 墨（A）：その場に残り、周りへ滲みながら薄れる
    // =========================
    // 隣との平均へ寄せる（拡散）ので、落とした直後の濃い点がじわじわ輪になって広がり、
    // 広がるほど中心が薄くなる。泡より長く残し、薄い所が水面の色に溶けて見えるよう最後に小さく引く。
    float iAvg = (cLeft.w + cRight.w + cUp.w + cDown.w) * 0.25f;
    float iNew = lerp(c1.w, iAvg, saturate(gInkSpread));
    iNew = iNew * saturate(gInkDecay) - 0.0002f;
    for (int n = 0; n < gInkSourceCount; ++n)
    {
        iNew += gInkSources[n].w * Falloff(uv, gInkSources[n]);
    }
    iNew = clamp(iNew, 0.0f, kInkMax);

    gOutHeight[DTid.xy] = float4(hNew, fNew, cNew, iNew);
}
