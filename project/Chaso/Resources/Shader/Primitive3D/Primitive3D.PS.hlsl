// =============================================================================
// 3D プリミティブ（デバッグ線）用ピクセルシェーダ
//
// primitive3d / primitive3d_nodepth PSO で使う。頂点カラーをそのまま出力する。
// =============================================================================

/// @struct PSIn
/// @brief Primitive3D.VS からの入力
struct PSIn
{
    float4 svpos : SV_POSITION; ///< クリップ空間位置
    float4 color : COLOR;       ///< 頂点カラー
};

/// @brief 頂点カラーをそのまま出力する
/// @param i VS の出力
/// @return 出力色
float4 main(PSIn i) : SV_TARGET
{
    return i.color;
}
