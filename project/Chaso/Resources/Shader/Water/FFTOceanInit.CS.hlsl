// =============================================================================
// FFTOceanInit.CS.hlsl - FFT オーシャンの初期スペクトル h0(k) を作る
//
// 風速・風向き・タイルサイズ・乱数の種が変わったときだけ実行する（毎フレームではない）。
//
// スペクトル（波数 k ごとの波の強さ）は Phillips 型：
//   Ψ(k) = α / (2 k⁴) · exp(-1 / (k Lw)²) · D(θ) · exp(-k² l²)
//     Lw = V² / g        … 風速 V で一番発達する波の長さの目安
//     α  = 0.0081        … Phillips 定数
//     D(θ) = (2/π)cos²θ  … 風向きに沿った波ほど強い（向かい風側は 7% に抑える）
//   これを波数平面で積分すると分散 σ² = α Lw² / 4、有義波高 Hs = 4σ ≈ 0.18 V²/g になり、
//   実際の海（Pierson-Moskowitz の Hs ≈ 0.21 V²/g）とほぼ同じ高さになる。
//   ※ タイルより長い波は表せないので、風が強くてタイルが小さいと実際より低くなる。
//
// 各 k に複素ガウス乱数 ξ を掛けて h0(k) = ξ · √(Ψ(k) Δk² / 4) とする（Δk = 2π / L）。
// 出力 u0: xy = h0(k), zw = conj(h0(-k))（時間発展で両方使うので一緒に持つ）
// =============================================================================

#include "FFTOcean.hlsli"

RWTexture2D<float4> gH0 : register(u0); ///< 出力: 初期スペクトル

static const float kPhillipsAlpha  = 0.0081; ///< Phillips 定数
static const float kBackwardDamp   = 0.07;   ///< 風上へ進む波の強さ（風下へ進む波に対する比）

/// @brief 32bit ハッシュ（PCG）
/// @param v 入力
/// @return ハッシュ値
uint Hash(uint v)
{
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

/// @brief テクセルごとに決まった 2 つの標準正規乱数（Box-Muller）
/// @param id テクセル番号
/// @return 平均 0・分散 1 の独立な乱数 2 つ
float2 Gaussian2(uint2 id)
{
    uint h1 = Hash(id.x + Hash(id.y + Hash(gSeed)));
    uint h2 = Hash(h1 ^ 0x9E3779B9u);
    float u1 = max(float(h1 & 0x00FFFFFFu) / 16777216.0, 1e-7);
    float u2 = float(h2 & 0x00FFFFFFu) / 16777216.0;
    float r = sqrt(-2.0 * log(u1));
    return r * float2(cos(2.0 * kFFTPi * u2), sin(2.0 * kFFTPi * u2));
}

/// @brief 波数 k の成分 1 つあたりの振幅 √(Ψ(k) Δk² / 4)
/// @param k 波数ベクトル（rad/m）
/// @return 振幅（m）
float SpectrumAmplitude(float2 k)
{
    float kl = length(k);
    if (kl < 1e-6) {
        return 0.0; // 平均水位（k = 0）は動かさない
    }
    float Lw = gWindSpeed * gWindSpeed / max(gGravity, 1e-3);
    float kl2 = kl * kl;

    // 方向の広がり：風向きとの角度の cos²。風上へ進む波は弱める
    float c = dot(k / kl, gWindDir);
    float dir = (2.0 / kFFTPi) * c * c * ((c < 0.0) ? kBackwardDamp : 1.0);

    float psi = kPhillipsAlpha / (2.0 * kl2 * kl2)
              * exp(-1.0 / max(kl2 * Lw * Lw, 1e-12))
              * exp(-kl2 * gSmallCutoff * gSmallCutoff)
              * dir;

    float dk = 2.0 * kFFTPi / gTileSize;
    return sqrt(max(psi, 0.0) * dk * dk * 0.25);
}

/// @brief エントリポイント（8x8 スレッドで N×N を覆う）
/// @param id テクセル番号
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= kFFTSize || id.y >= kFFTSize) {
        return;
    }

    // -k のテクセル番号（中央が k = 0 なので N - n。n = 0 はナイキストで自分自身に戻る）
    uint2 mid = (kFFTSize - id.xy) % kFFTSize;

    float2 k = WaveVector(id.xy);
    float2 h0k  = Gaussian2(id.xy) * SpectrumAmplitude(k);
    float2 h0mk = Gaussian2(mid)   * SpectrumAmplitude(-k);

    // ナイキスト（端の行・列）は -k の相手が格子に無く、実の場にならない（虚部が漏れる）ので使わない
    if (id.x == 0 || id.y == 0) {
        h0k = float2(0, 0);
        h0mk = float2(0, 0);
    }

    gH0[id.xy] = float4(h0k, h0mk.x, -h0mk.y);
}
