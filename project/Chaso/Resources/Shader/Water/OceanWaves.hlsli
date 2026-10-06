// OceanWaves.hlsli - 海面の「詳細波」（主波の上に重ねる短い波の群れ）
//
// 主波 3 本（Water.VS.hlsl の GetWaveDef）だけだと周期が見えて「プールの波」に見える。
// ここでは主波の向き（風向き）のまわりに角度をばらした 8 本の短い Gerstner 波を足し、
// 海らしい不規則さを出す。
//
//   ・波長比は非整数（1.37, 1.79, ...）にして、全体の繰り返し周期を見えなくする
//   ・角速度は深水波の分散関係 ω = √(g k) に従う。主波の速度 gWaveSpeed を基準に
//     ω_i = gWaveSpeed * √(ratio_i) とすることで、ユーザーが調整した時間スケールを保ったまま
//     「短い波ほど遅く進む」本物の海の動きになる
//   ・振幅は波形勾配（k*A）がほぼ一定になるよう 1/ratio で落とす（実際の海も短い波ほど低い）
//
// VS では「頂点間隔で表せる長さの波」だけを変位させ（minWavelength 未満はフェードアウト）、
// PS では全ての詳細波の傾きをピクセルごとに解析的に求めて法線へ足す。
// これでメッシュの分割数に関係なく細かいうねりの陰影が出る。
//
// ★ CPU 側 RC::WaterSurface（Engine/Common/Water/WaterSurface.h）にも同じ表を持たせている。
//   片方だけ変えると、見た目の水面と浮力の水面がずれるので必ず両方直すこと。

#ifndef OCEAN_WAVES_HLSLI
#define OCEAN_WAVES_HLSLI

static const int   kDetailWaveCount = 8;
static const float kWindAngle       = 0.620249486; // atan2(0.5, 0.7) = 主波 0 の向き
static const float kDetailAngle[kDetailWaveCount] = { -0.62, 0.47, -0.21, 0.83, 0.12, -0.97, 0.58, -0.41 };
static const float kDetailRatio[kDetailWaveCount] = {  1.37, 1.79,  2.23, 2.71, 3.29,  3.97, 4.81,  5.83 };
static const float kDetailPhase[kDetailWaveCount] = {  0.0,  1.7,   4.1,  2.3,  5.5,   0.9,  3.3,   6.0  };
static const float kDetailAmpScale  = 0.35; // 主波の振幅に対する詳細波の振幅（1/ratio を掛ける前）
static const float kDetailMaxSlope  = 0.10; // 1 本あたりの k*A の上限（波が尖りすぎてループしないように）
static const float kTwoPi           = 6.28318530718;

struct DetailWave
{
    float2 dir;   // 進行方向
    float  k;     // 波数 (rad/m)
    float  omega; // 角速度 (rad/s)
    float  amp;   // 鉛直振幅 (m)
    float  qa;    // 水平変位の振幅 (m)  = Gerstner の Q*A
    float  phase0;
};

// baseFreq/baseSpeed/baseHeight は主波 0 のパラメータ（gWaveFreq, gWaveSpeed, gWaveHeight）
// detail: 詳細波の強さ (0 で無効) / choppy: 尖り具合 (0..1)
DetailWave GetDetailWave(int i, float baseFreq, float baseSpeed, float baseHeight,
                         float detail, float choppy)
{
    DetailWave w;
    float ang = kWindAngle + kDetailAngle[i];
    w.dir    = float2(cos(ang), sin(ang));
    w.k      = max(baseFreq, 1e-3) * kDetailRatio[i];
    w.omega  = baseSpeed * sqrt(kDetailRatio[i]);
    w.amp    = min(baseHeight * kDetailAmpScale / kDetailRatio[i], kDetailMaxSlope / w.k) * detail;
    // Σ(Q*A*k) = choppy * detail になるよう配分 → choppy=1 でも全波の山が揃わない限り折り返さない
    w.qa     = choppy * detail / (kDetailWaveCount * w.k);
    w.phase0 = kDetailPhase[i];
    return w;
}

float DetailPhase(DetailWave w, float2 xz, float time)
{
    return w.k * dot(w.dir, xz) - w.omega * time + w.phase0;
}

// 詳細波の鉛直振幅の合計（PS の高さ正規化に使う）
float DetailAmplitudeSum(float baseFreq, float baseHeight, float detail)
{
    float sum = 0.0;
    [unroll]
    for (int i = 0; i < kDetailWaveCount; ++i)
    {
        float k = max(baseFreq, 1e-3) * kDetailRatio[i];
        sum += min(baseHeight * kDetailAmpScale / kDetailRatio[i], kDetailMaxSlope / k) * detail;
    }
    return sum;
}

#endif // OCEAN_WAVES_HLSLI
