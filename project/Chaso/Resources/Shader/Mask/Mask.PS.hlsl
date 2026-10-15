#include "../Object3d/Object3d.hlsli"

// ============================================================================
// マスク書き込み用ピクセルシェーダ
//
// 「このピクセルは強調対象のオブジェクトである」という 1 ビットだけを書く。
// テクスチャもライティングも見ないので、モデルのシルエットがそのまま白く出る。
// このマスクを MaskOutline.PS.hlsl が膨張させて輪郭にする。
//
// ルートシグネチャは object3d と同じ（RootSignatureType::Object3D）なので、
// 既存の RC::DrawModel 系がそのまま流用できる（追加のバインドは不要）。
// ============================================================================

/// @struct PixelShaderOutput
/// @brief ピクセル出力
struct PixelShaderOutput
{
    float4 color : SV_TARGET0; ///< マスク値（常に白）
};

/// @brief 強調対象のピクセルとして白を書く
/// @param input Object3D 系 VS の出力（使わない）
/// @return 出力色（白）
PixelShaderOutput main(VertexShaderOutput input)
{
    PixelShaderOutput output;
    output.color = float4(1.0f, 1.0f, 1.0f, 1.0f);
    return output;
}
