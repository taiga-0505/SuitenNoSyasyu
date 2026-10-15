// =============================================================================
// 2D プリミティブ用頂点シェーダ
//
// primitive2d PSO で使う。CPU 側（Primitive2D）が画面を覆う三角形の頂点を
// クリップ座標で渡すので、変換せずに PS へ渡す。図形の形は PS 側で決める。
// =============================================================================

/// @struct VSIn
/// @brief 頂点入力
struct VSIn
{
    float4 pos : POSITION; ///< クリップ座標
    float2 uv : TEXCOORD;  ///< テクスチャ座標（PS では使わない）
};
/// @struct VSOut
/// @brief 頂点出力
struct VSOut
{
    float4 pos : SV_POSITION; ///< クリップ空間位置
    float2 uv : TEXCOORD;     ///< テクスチャ座標（PS では使わない）
};
/// @brief 頂点を変換せずにそのまま出力する
/// @param i 頂点入力
/// @return クリップ空間位置と UV
VSOut main(VSIn i)
{
    VSOut o;
    o.pos = i.pos;
    o.uv = i.uv;
    return o;
}
