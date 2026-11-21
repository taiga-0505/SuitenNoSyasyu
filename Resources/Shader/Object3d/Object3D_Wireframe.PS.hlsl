#include "Object3d.hlsli"

// =============================================================================
// Wireframe（ワイヤーフレーム表示）
//
// 単色でワイヤーフレームを描く。塗りつぶしモードを WIREFRAME にした PSO
// （object3d_wire / _inst / _skin）から使う。ソリッドの上に重ねる前提なので
// PSO 側は深度書き込み OFF・カリング無し。
// =============================================================================

struct PixelShaderOutput
{
    float4 color : SV_TARGET0; ///< 出力色
};

/// @brief ワイヤーフレームの線色を出力する
/// @param input 頂点シェーダーからの補間値（instColor を乗算する）
/// @return 出力色
PixelShaderOutput main(VertexShaderOutput input)
{
    PixelShaderOutput output;
    
    // 単色のワイヤーフレーム色（白）
    output.color = float4(0.8f, 0.8f, 0.8f, 1.0f) * input.instColor;
    
    return output;
}
