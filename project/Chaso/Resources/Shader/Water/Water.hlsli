// Water.hlsli - Shared structures between Water VS and PS

struct VertexShaderOutput {
    float4 position      : SV_POSITION;
    float2 texcoord      : TEXCOORD0;
    float3 normal        : NORMAL0;
    float3 worldPosition : POSITION0;
    float4 instColor     : COLOR0;
    float  waveHeight    : TEXCOORD1; // 静水面からの変位（Gerstner＋波紋）。PS の高さ色付けに使う
    float2 restXZ        : TEXCOORD2; // 変位前のワールド XZ。PS で詳細波の傾きを解析的に求めるのに使う
    float3 mainJacobian  : TEXCOORD3; // 主波（＋反射波）の水平変位の偏微分 (dDx/dx, dDz/dz, dDx/dz)。白波判定用
};
