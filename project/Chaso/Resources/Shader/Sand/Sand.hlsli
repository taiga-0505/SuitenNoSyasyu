// =============================================================================
// Sand.hlsli - 砂の地形（RenderInteractiveSand::DrawSand）の VS / PS で共有する定義
// ルートシグネチャは水面（Water）を流用する: b0 VS トランスフォーム / b1 平行光源 / b2 カメラ /
// b6 SandParams（WaterParams の位置）/ t4 跡のハイトマップ / s0 ラップ / s2 クランプ
// =============================================================================

/// @struct VertexShaderOutput
/// @brief 砂の VS の出力（= PS の入力）
struct VertexShaderOutput
{
    float4 position      : SV_POSITION; ///< クリップ空間位置
    float3 normal        : NORMAL0;     ///< 地形のワールド法線
    float3 worldPosition : POSITION0;   ///< ワールド位置
};

cbuffer SandParams : register(b6)
{
    float4 gSandWindow;  ///< 跡のハイトマップの窓。xy: 中心のワールド XZ, z: 一辺（m）, w: 1 テクセルの長さ（m）
    float4 gDryColor;    ///< 乾いた砂の色
    float4 gWetColor;    ///< 濡れた砂の色
    float4 gGrainColorA; ///< 混じる粒の色 1（貝殻のかけら）
    float4 gGrainColorB; ///< 混じる粒の色 2（黒い粒）
    float4 gGrain;       ///< x: 粒の細かさ（1m あたり）, y: 色のばらつき, z: 色の付いた粒の割合, w: 時間（秒）
    float4 gRipple;      ///< x: 風紋の間隔（m）, y: 強さ, z: 向き cos, w: 向き sin
    float4 gWet;         ///< x: 水面の高さ, y: 濡れて見える高さ（m）, z: 照り返し, w: きらめき
    float4 gMark;        ///< x: 跡の陰影の強さ, y: 掘りたての暗さ
};
