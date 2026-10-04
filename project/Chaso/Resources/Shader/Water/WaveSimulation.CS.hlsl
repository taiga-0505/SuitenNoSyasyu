// WaveSimulation.CS.hlsl
// 2D波動方程式を用いた波の伝播シミュレーション
// 速度成分にのみ減衰をかけることで、波が通過した後に水面が元の高さに戻る
//
// テクスチャは R32G32B32A32_FLOAT:
//   R = 波紋の高さ（m）。従来どおり波動方程式で伝播する
//   G = 引き波の泡（0..kFoamMax）。波としては伝播せず、その場に残って滲みながらゆっくり消える
//       （船尾のスクリューや尾がかき回した、白く濁った帯）
//   B = 波頭の白波（0..kFoamMax）。すぐ消える（毎フレーム置き直す前提）。
//       船首の砕け波や V 字（ケルビン波）の腕の白い線のように「今その場所にだけある白」に使う。
//       長く残すと腕が外へ動いた跡が塗りつぶされて V にならないため、残存率を小さくしている
//   A = 未使用

cbuffer SimulationParams : register(b0)
{
    float gAlpha;        // 波の伝播速度係数 (c^2 * dt^2 / dx^2)
    float gDamping;      // 減衰係数 (速度にのみ適用, 例: 0.95 ～ 0.99)
    int   gSourceCount;  // 現在の波源の数
    float gResetFlag;    // 1.0 なら状態をゼロにリセットするフラグ

    // x, y = UV座標 (0.0 ～ 1.0)
    // z = 波源の半径 (UV空間での半径)
    // w = 波源の強さ (高さの加算値)
    float4 gSources[64];

    // ---- 引き波の泡（G）----
    float gFoamDecay;       // 毎フレームの残存率（例: 0.992）
    float gFoamSpread;      // 毎フレームの滲み（隣との平均へ寄せる割合。0..0.25）
    int   gFoamSourceCount; // 泡の波源の数
    float gCrestDecay;      // 波頭（B）の毎フレーム残存率（例: 0.8）

    // x, y = UV座標 / z = 半径（UV）/ w = このフレームに足す量
    float4 gFoamSources[64];

    // ---- 波頭の白波（B）----
    int    gCrestSourceCount;
    float3 gCrestPad;
    float4 gCrestSources[128];
};

static const float kFoamMax = 2.0f;

// t0: 1フレーム前のハイトマップ
Texture2D<float4> gPrevHeight : register(t0);
// t1: 2フレーム前のハイトマップ
Texture2D<float4> gPrevPrevHeight : register(t1);

// u0: 出力先のハイトマップ
RWTexture2D<float4> gOutHeight : register(u0);

/// 半径内で (1 - t²) の重みを返す（範囲外は 0）
float Falloff(float2 uv, float4 src)
{
    float radius = max(src.z, 1e-5f);
    float t = distance(uv, src.xy) / radius;
    return (t < 1.0f) ? (1.0f - t * t) : 0.0f;
}

[numthreads(16, 16, 1)]
void main( uint3 DTid : SV_DispatchThreadID )
{
    uint width, height;
    gOutHeight.GetDimensions(width, height);

    if (DTid.x >= width || DTid.y >= height)
        return;

    int3 pos = int3(DTid.x, DTid.y, 0);

    // リセットフラグが立っている場合は強制的に0クリア
    if (gResetFlag > 0.5f)
    {
        gOutHeight[DTid.xy] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    float4 c1 = gPrevHeight.Load(pos);
    float h1 = c1.x;
    float h2 = gPrevPrevHeight.Load(pos).x;

    // 上下左右のピクセル値を取得（境界では自分自身をクランプ扱い）
    int xLeft  = max((int)DTid.x - 1, 0);
    int xRight = min((int)DTid.x + 1, (int)width - 1);
    int yUp    = max((int)DTid.y - 1, 0);
    int yDown  = min((int)DTid.y + 1, (int)height - 1);

    float4 cLeft  = gPrevHeight.Load(int3(xLeft, DTid.y, 0));
    float4 cRight = gPrevHeight.Load(int3(xRight, DTid.y, 0));
    float4 cUp    = gPrevHeight.Load(int3(DTid.x, yUp, 0));
    float4 cDown  = gPrevHeight.Load(int3(DTid.x, yDown, 0));

    // =========================
    // 高さ（R）：波動方程式
    // =========================
    // velocity = h1 - h2 (前フレームとの差が速度に相当)
    // laplacian = 空間的な広がり
    float laplacian = cLeft.x + cRight.x + cUp.x + cDown.x - 4.0f * h1;

    // 1. Verlet積分に基づく波動方程式（局所的な速度減衰 gDamping を適用）
    float hNew = h1 + (h1 - h2) * gDamping + gAlpha * laplacian;
    // 2. 水面全体が自然に0（平坦）に戻るように、全体に微小な減衰をかける
    hNew *= 0.99f;

    // 波源（力）の加算
    float2 uv = float2((float)DTid.x / (float)(width - 1), (float)DTid.y / (float)(height - 1));
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

    // 発散（NaN/Infinity）を防ぐためのクランプ
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

    gOutHeight[DTid.xy] = float4(hNew, fNew, cNew, 0.0f);
}
