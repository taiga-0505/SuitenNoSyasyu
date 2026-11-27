// =============================================================================
// FFTOcean.hlsli - FFT オーシャン（Tessendorf 方式）の Compute Shader 共通部
//
// 海面を「無数の正弦波の重ね合わせ」として、風から決まる波のスペクトルで振幅を与え、
// 毎フレーム逆 FFT で高さ・水平変位・傾きをテクスチャに書き出す。
//
//   1. FFTOceanInit.CS     : 初期スペクトル h0(k) を作る（風・タイルサイズ・乱数の種が変わったときだけ）
//   2. FFTOceanSpectrum.CS : 時刻 t のスペクトル h(k,t) と、変位・傾き・ヤコビアン用のスペクトルを作る
//   3. FFTOceanRow.CS      : 行ごとに逆 FFT（1 行 = 1 スレッドグループ、groupshared で 256 点）
//   4. FFTOceanColumn.CS   : 列ごとに逆 FFT し、符号を直して最終テクスチャへ書く
//
// 実の場を 2 つずつ「A + iB」として 1 本の複素 FFT に詰めている（どちらもエルミート対称なので
// 逆変換の実部が A、虚部が B になる）。8 つの場（Dx, h, Dz, Dxz, hx, hz, Dxx, Dzz）を
// float4 のテクスチャ 2 枚 = 複素 4 本で運ぶ。
//
// 出力（水面シェーダーが読む。値は「高さスケール 1・尖り 1」の生の値）
//   Displacement : x = Dx（水平変位 X）, y = h（高さ）, z = Dz（水平変位 Z）, w = ∂Dx/∂z
//   Derivatives  : x = ∂h/∂x, y = ∂h/∂z, z = ∂Dx/∂x, w = ∂Dz/∂z
// 高さスケールと尖りは水面シェーダー側（WaterParams の gFFTParams）で掛ける。
//
// ★ ルートシグネチャ（RootSignatureType::FFTOceanCS）: b0 = この cbuffer, u0〜u3 = テクスチャ（テーブル）
// ★ C++ 側は Engine/Render/Systems/RenderFFTOcean.cpp。FFTOceanCB と並びを揃えること
// =============================================================================

#ifndef FFT_OCEAN_HLSLI
#define FFT_OCEAN_HLSLI

static const uint  kFFTSize    = 256;           ///< テクスチャの一辺 N（C++ の kFFTSize と揃える）
static const uint  kFFTLogSize = 8;             ///< log2(N)
static const float kFFTPi      = 3.14159265359; ///< π

cbuffer FFTOceanParams : register(b0)
{
    float  gTileSize;    ///< タイル 1 枚のワールドサイズ L（m）
    float  gTime;        ///< 経過時間（秒）
    float  gWindSpeed;   ///< 風速 V（m/s）
    float  gGravity;     ///< 重力加速度（m/s²）

    float2 gWindDir;     ///< 風向き（XZ、正規化済み）
    float  gSmallCutoff; ///< これより短い波を弱める長さ l（m）。exp(-k²l²)
    uint   gSeed;        ///< 乱数の種
};

/// @brief 複素数の積
/// @param a 複素数（x: 実部, y: 虚部）
/// @param b 複素数
/// @return a × b
float2 CMul(float2 a, float2 b)
{
    return float2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x);
}

/// @brief 複素数に i を掛ける
/// @param a 複素数
/// @return i × a
float2 CMulI(float2 a)
{
    return float2(-a.y, a.x);
}

/// @brief テクセル番号に対応する波数ベクトル k（rad/m）
/// @param id テクセル番号（0〜N-1）。中央 N/2 が k = 0
/// @return 波数ベクトル（x: kx, y: kz）
float2 WaveVector(uint2 id)
{
    int2 n = int2(id) - int2(kFFTSize / 2, kFFTSize / 2);
    return float2(n) * (2.0 * kFFTPi / gTileSize);
}

#endif // FFT_OCEAN_HLSLI
