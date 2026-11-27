// =============================================================================
// FFTOceanSpectrum.CS.hlsl - 時刻 t のスペクトルを作る（毎フレーム）
//
// 深水波の分散関係 ω = √(g k) で各成分を回す：
//   h(k, t) = h0(k) e^{iωt} + conj(h0(-k)) e^{-iωt}
// これは h(-k, t) = conj(h(k, t)) を満たすので、逆 FFT の結果は実数になる。
//
// 高さ h から、同じ成分の
//   水平変位 D = -i k/|k| h（山へ寄せて尖らせる。Gerstner の Q と同じ働き）
//   傾き     ∇h = i k h
//   変位の偏微分 ∂Dx/∂x = kx²/|k| h, ∂Dz/∂z = kz²/|k| h, ∂Dx/∂z = kx kz/|k| h（白波のヤコビアン用）
// を作り、実の場 2 つずつを「A + iB」で 1 つの複素数に詰める。
//
// 入力  u0: 初期スペクトル（xy = h0(k), zw = conj(h0(-k))）
// 出力  u1: xy = Dx + i h,    zw = Dz + i ∂Dx/∂z
//       u2: xy = hx + i hz,   zw = ∂Dx/∂x + i ∂Dz/∂z
// =============================================================================

#include "FFTOcean.hlsli"

RWTexture2D<float4> gH0    : register(u0); ///< 入力: 初期スペクトル
RWTexture2D<float4> gSpecA : register(u1); ///< 出力: 変位と高さ
RWTexture2D<float4> gSpecB : register(u2); ///< 出力: 傾きと変位の偏微分

/// @brief エントリポイント（8x8 スレッドで N×N を覆う）
/// @param id テクセル番号
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= kFFTSize || id.y >= kFFTSize) {
        return;
    }

    float4 h0 = gH0[id.xy];
    float2 k = WaveVector(id.xy);
    float kl = length(k);
    float invK = (kl > 1e-6) ? 1.0 / kl : 0.0;

    float omega = sqrt(gGravity * kl);
    float s, c;
    sincos(omega * gTime, s, c);
    float2 e = float2(c, s);
    float2 h = CMul(h0.xy, e) + CMul(h0.zw, float2(c, -s));

    float2 ih = CMulI(h); // i h

    float2 dx  = -ih * (k.x * invK);
    float2 dz  = -ih * (k.y * invK);
    float2 hx  = ih * k.x;
    float2 hz  = ih * k.y;
    float2 dxx = h * (k.x * k.x * invK);
    float2 dzz = h * (k.y * k.y * invK);
    float2 dxz = h * (k.x * k.y * invK);

    // 実の場 A, B を A + iB として詰める
    gSpecA[id.xy] = float4(dx + CMulI(h),  dz + CMulI(dxz));
    gSpecB[id.xy] = float4(hx + CMulI(hz), dxx + CMulI(dzz));
}
