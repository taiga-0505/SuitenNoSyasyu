// =============================================================================
// FFTOceanIFFT.hlsli - 256 点の逆 FFT（行 / 列）の本体
//
// FFTOceanRow.CS / FFTOceanColumn.CS が FFT_COLUMN を定義する / しないで読み込む。
// 1 スレッドグループ = 1 行（または 1 列）、1 スレッド = 1 点。
// 入力をビット反転の位置へ置いてから、時間間引き（Cooley-Tukey）のバタフライを log2(N) 段、
// groupshared の 2 枚を交互に使って進める。
//
// 中央を k = 0 にした並び（n = n' + N/2）で変換しているので、結果には (-1)^x が掛かる。
// 行と列の 2 回で (-1)^(x+y) になり、列のほう（最後）でまとめて直す。
//
// 行: u0, u1 を読んで u2, u3 へ書く（中間結果）
// 列: u0, u1 を読んで u2 = Displacement, u3 = Derivatives へ書く（最終結果）
// =============================================================================

#include "FFTOcean.hlsli"

RWTexture2D<float4> gInA  : register(u0); ///< 入力 A（複素 2 本）
RWTexture2D<float4> gInB  : register(u1); ///< 入力 B（複素 2 本）
RWTexture2D<float4> gOutA : register(u2); ///< 出力 A（列では Displacement）
RWTexture2D<float4> gOutB : register(u3); ///< 出力 B（列では Derivatives）

groupshared float4 sA[2][kFFTSize]; ///< A の作業領域（交互に使う 2 枚）
groupshared float4 sB[2][kFFTSize]; ///< B の作業領域

/// @brief 行 / 列の番号と行内の位置からテクセル座標を作る
/// @param lineIdx 行（列）の番号
/// @param i    行（列）の中の位置
/// @return テクセル座標
uint2 TexelOf(uint lineIdx, uint i)
{
#ifdef FFT_COLUMN
    return uint2(lineIdx, i);
#else
    return uint2(i, lineIdx);
#endif
}

/// @brief 複素数 2 本（float4 の xy と zw）それぞれに w を掛ける
/// @param w 掛ける複素数
/// @param v 複素数 2 本
/// @return w × v（2 本とも）
float4 CMul2(float2 w, float4 v)
{
    return float4(CMul(w, v.xy), CMul(w, v.zw));
}

/// @brief エントリポイント
/// @param gid SV_GroupID（x が行 / 列の番号）
/// @param tid SV_GroupThreadID（x が行 / 列の中の位置）
[numthreads(kFFTSize, 1, 1)]
void main(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID)
{
    uint lineIdx = gid.x;
    uint i = tid.x;

    // ビット反転した位置へ読み込む
    uint rev = reversebits(i) >> (32u - kFFTLogSize);
    sA[0][rev] = gInA[TexelOf(lineIdx, i)];
    sB[0][rev] = gInB[TexelOf(lineIdx, i)];
    GroupMemoryBarrierWithGroupSync();

    uint src = 0;
    [unroll]
    for (uint stage = 0; stage < kFFTLogSize; ++stage)
    {
        uint halfSize = 1u << stage;
        uint j = i & (2u * halfSize - 1u); // ブロック内の位置
        bool top = (j < halfSize);
        uint jj = top ? j : (j - halfSize);
        uint evenIdx = top ? i : (i - halfSize);
        uint oddIdx = evenIdx + halfSize;

        // 逆変換なので回転は +2π
        float s, c;
        sincos(kFFTPi * float(jj) / float(halfSize), s, c);
        float2 w = float2(c, s);
        float sign = top ? 1.0 : -1.0;

        float4 a = sA[src][evenIdx] + sign * CMul2(w, sA[src][oddIdx]);
        float4 b = sB[src][evenIdx] + sign * CMul2(w, sB[src][oddIdx]);
        sA[1u - src][i] = a;
        sB[1u - src][i] = b;
        GroupMemoryBarrierWithGroupSync();
        src = 1u - src;
    }

    float4 resA = sA[src][i];
    float4 resB = sB[src][i];
    uint2 texel = TexelOf(lineIdx, i);

#ifdef FFT_COLUMN
    // 中央を k = 0 にした並びの補正 (-1)^(x+y)。実部と虚部が詰めた 2 つの実の場
    float sgn = ((texel.x + texel.y) & 1u) ? -1.0 : 1.0;
    resA *= sgn;
    resB *= sgn;
    // resA = (Dx, h, Dz, ∂Dx/∂z), resB = (∂h/∂x, ∂h/∂z, ∂Dx/∂x, ∂Dz/∂z)
    gOutA[texel] = resA;
    gOutB[texel] = resB;
#else
    gOutA[texel] = resA;
    gOutB[texel] = resB;
#endif
}
