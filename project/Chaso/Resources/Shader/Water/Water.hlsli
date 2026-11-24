// =============================================================================
// Water.hlsli - 水面の VS と PS で共有する構造体
// =============================================================================

/// @struct VertexShaderOutput
/// @brief 水面 VS の出力（= PS の入力）
struct VertexShaderOutput {
    float4 position      : SV_POSITION; ///< クリップ空間位置
    float2 texcoord      : TEXCOORD0;   ///< メッシュの UV
    float3 normal        : NORMAL0;     ///< 主波（＋反射波）から求めたワールド法線
    float3 worldPosition : POSITION0;   ///< 変位後のワールド位置
    float4 instColor     : COLOR0;      ///< インスタンス色（現在は常に白）
    float  waveHeight    : TEXCOORD1;   ///< 静水面からの変位（Gerstner＋波紋）。PS の高さ色付けに使う
    float2 restXZ        : TEXCOORD2;   ///< 変位前のワールド XZ。PS で詳細波の傾きを解析的に求めるのに使う
    float3 mainJacobian  : TEXCOORD3;   ///< 主波（＋反射波）の水平変位の偏微分（dDx/dx, dDz/dz, dDx/dz）。白波判定用
};

/// @brief ワールド XZ を、波紋ハイトマップの窓の UV（0〜1）へ変換する
/// @param worldXZ ワールド XZ
/// @param window  窓（xy: 中心のワールド XZ, z: 一辺（m））。WaterParams の gWaveWindow
/// @return 窓の UV（窓の外は 0〜1 の外になる）
float2 WaveWindowUV(float2 worldXZ, float4 window)
{
    return (worldXZ - window.xy) / max(window.z, 1e-3) + 0.5;
}

/// @brief 窓の縁で 0 へ落とす重み（窓の外は 0）
/// @param uv 窓の UV
/// @return 重み（0〜1）
/// @details 窓はカメラに合わせて動くので、縁で波紋・泡・墨が切れた線が見えないよう 4% 幅（≒4m）でぼかす。
float WaveWindowEdgeFade(float2 uv)
{
    float2 d = min(uv, 1.0 - uv); // 縁までの距離（UV）
    return saturate(min(d.x, d.y) / 0.04);
}
