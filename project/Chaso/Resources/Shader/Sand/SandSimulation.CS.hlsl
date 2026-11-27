// =============================================================================
// SandSimulation.CS.hlsl - 砂の跡（RenderInteractiveSand）を 1 ステップ進める
//
// テクスチャは R32G32B32A32_FLOAT:
//   R = 跡の高さ（m）。掘ると負、縁に寄った砂は正。少しずつ周りへ崩れて 0 に戻る
//   G = 掘りたての湿り（0〜1）。掘った所は下の湿った砂が出て暗い。数秒で乾く
//   B = 波に洗われた濡れ（0〜1）。均した所は濡れて暗く光る。ゆっくり乾く
//   A = 未使用
//
// 掘る：中心が深い窪み（何度掘ってもその深さより深くはならない = 指でなぞった溝の深さは一定）と、
//       そのすぐ外側の盛り上がり（掘った砂が寄った縁）。縁は既にある溝の中には盛らない。
// 均す：高さと湿りを 0 へ寄せ、濡れを足す（波が寄せて跡を消す）。
//
// 窓（テクスチャが覆う範囲）はカメラに合わせて動き、動いたぶん（gShift）だけ前フレームをずらして読む。
// ルートシグネチャは WaveSimulationCS を流用（b0 / t0 / t1 / u0。t1 は読まない）。
// =============================================================================

cbuffer SandSimParams : register(b0)
{
    int    gDigCount;     ///< 掘るの数
    int    gSmoothCount;  ///< 均すの数
    float  gResetFlag;    ///< 1 なら 0 クリア
    float  gHeightKeep;   ///< 高さの毎フレーム残存率
    int2   gShift;        ///< 前フレームを読むときのずらし量（テクセル）
    float  gFreshKeep;    ///< 湿りの毎フレーム残存率
    float  gWetKeep;      ///< 濡れの毎フレーム残存率
    float  gSlump;        ///< 周りの平均へ寄せる割合（崩れ）
    float  gTexelWorld;   ///< 1 テクセルのワールドの長さ（m）
    float2 gPad;          ///< 16 バイト境界合わせ
    float4 gDigs[128];     ///< 掘る（xy: UV、z: 半径（UV）、w: 深さ（m））
    float4 gDigExtra[128]; ///< x: 縁の盛り上がり（m）、y: 湿り（0〜1）
    float4 gSmooths[64];   ///< 均す（xy: UV、z: 半径（UV）、w: 割合）
};

Texture2D<float4> gPrev : register(t0);
RWTexture2D<float4> gOut : register(u0);

static const float kMaxHeight = 0.6f; ///< 跡の高さの上限（m。発散よけ）

/// @brief 前フレームを窓の移動ぶんずらして読む（前の窓の外なら 0）
float4 LoadShifted(int2 p, int2 size)
{
    int2 q = p + gShift;
    if (any(q < 0) || any(q >= size))
        return float4(0, 0, 0, 0);
    return gPrev.Load(int3(q, 0));
}

[numthreads(16, 16, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    uint w, h;
    gOut.GetDimensions(w, h);
    if (DTid.x >= w || DTid.y >= h)
        return;
    if (gResetFlag > 0.5f)
    {
        gOut[DTid.xy] = float4(0, 0, 0, 0);
        return;
    }
    int2 size = int2((int)w, (int)h);
    int2 p = int2(DTid.xy);

    float4 c = LoadShifted(p, size);
    float4 l = LoadShifted(int2(max(p.x - 1, 0), p.y), size);
    float4 r = LoadShifted(int2(min(p.x + 1, size.x - 1), p.y), size);
    float4 u = LoadShifted(int2(p.x, max(p.y - 1, 0)), size);
    float4 d = LoadShifted(int2(p.x, min(p.y + 1, size.y - 1)), size);

    // 崩れ：周りの平均へ少し寄せてから、全体をゆっくり 0 へ戻す
    float hgt = lerp(c.x, (l.x + r.x + u.x + d.x) * 0.25f, gSlump) * gHeightKeep;
    float fresh = lerp(c.y, (l.y + r.y + u.y + d.y) * 0.25f, 0.05f) * gFreshKeep;
    float wet = c.z * gWetKeep;

    float2 uv = (float2(p) + 0.5f) / float2(size);

    // ---- 掘る ----
    for (int i = 0; i < gDigCount; ++i)
    {
        float4 dg = gDigs[i];
        float radius = max(dg.z, 1e-5f);
        float t = distance(uv, dg.xy) / radius;
        float depth = dg.w;
        float rim = gDigExtra[i].x;
        if (t < 1.0f)
        {
            // 中心が深く、縁へなめらかに浅くなる窪み。既に深ければそのまま（min）
            float carve = -depth * (0.5f + 0.5f * cos(3.14159265f * t));
            hgt = min(hgt, carve);
            fresh = max(fresh, gDigExtra[i].y * saturate(1.2f - t));
        }
        // 縁：窪みの外側 0.85〜1.7 倍に盛る。既にある溝の底には盛らない
        if (rim > 0.0f && t > 0.85f && t < 1.7f && hgt > -0.3f * depth)
        {
            float k = sin(3.14159265f * (t - 0.85f) / 0.85f);
            hgt = max(hgt, rim * k);
        }
    }

    // ---- 均す（波） ----
    for (int j = 0; j < gSmoothCount; ++j)
    {
        float4 sm = gSmooths[j];
        float t = distance(uv, sm.xy) / max(sm.z, 1e-5f);
        if (t < 1.0f)
        {
            float a = sm.w * (1.0f - t * t);
            hgt = lerp(hgt, 0.0f, a);
            fresh = lerp(fresh, 0.0f, a);
            wet = max(wet, saturate(a * 3.0f));
        }
    }

    gOut[DTid.xy] = float4(clamp(hgt, -kMaxHeight, kMaxHeight), saturate(fresh), saturate(wet), 0.0f);
}
