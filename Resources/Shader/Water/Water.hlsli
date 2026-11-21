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
