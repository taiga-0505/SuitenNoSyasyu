// =============================================================================
// 3D プリミティブ（デバッグ線）用頂点シェーダ
//
// primitive3d / primitive3d_nodepth PSO（ライン描画）で使う。
// 頂点はワールド座標で渡されるので、ビュー・プロジェクションだけを掛ける。
// =============================================================================

// フレーム共通の定数
cbuffer PerFrame : register(b0)
{
    row_major float4x4 gView; ///< ビュー行列
    row_major float4x4 gProj; ///< プロジェクション行列
};

/// @struct VSIn
/// @brief 頂点入力
struct VSIn
{
    float3 pos : POSITION; ///< ワールド座標
    float4 color : COLOR;  ///< 頂点カラー
};

/// @struct VSOut
/// @brief 頂点出力
struct VSOut
{
    float4 svpos : SV_POSITION; ///< クリップ空間位置
    float4 color : COLOR;       ///< 頂点カラー
};

/// @brief ワールド座標の頂点をクリップ空間へ変換する
/// @param v 頂点入力
/// @return クリップ空間位置と頂点カラー
VSOut main(VSIn v)
{
    VSOut o;
    float4 wp = float4(v.pos, 1.0f);
    float4 vp = mul(wp, gView);
    o.svpos = mul(vp, gProj);
    o.color = v.color;
    return o;
}
