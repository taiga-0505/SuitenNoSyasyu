#include "../Fullscreen/Fullscreen.hlsli"

// =============================================================================
// RadialBlur（放射状ブラー）
//
// 指定した中心から外へ向かってサンプルを並べて平均し、放射状にぶらす
// ポストエフェクト（radialblur PSO）。
// =============================================================================

// PostEffectConstants (b0)
// param0: center.x（float）
// param1: center.y（float）
// param2: blurWidth（float）
// param3: numSamples（int）
/// @struct PostEffectConstants
/// @brief ラジアルブラーのパラメータ（ルート定数 4 つ）
struct PostEffectConstants {
    float2 center;   ///< ブラーの中心（UV）
    float blurWidth; ///< 1 サンプルごとに進める距離（中心からの方向ベクトルに対する倍率）
    int numSamples;  ///< サンプル数（1 未満は 1 として扱う）
};

ConstantBuffer<PostEffectConstants> gParams : register(b0);
Texture2D<float4> gTexture : register(t0);
SamplerState gSamplerClamp : register(s2);

/// @struct PS_OUTPUT
/// @brief ピクセル出力
struct PS_OUTPUT {
    float4 color : SV_TARGET; ///< 出力色
};

/// @brief 中心から外向きに並べたサンプルを平均する
/// @param input フルスクリーン VS の出力
/// @return 出力色（α は 1 固定）
PS_OUTPUT main(VertexShaderOutput input) {
    // 中心から現在の UV への方向を計算
    float2 direction = input.texcoord - gParams.center;
    float3 outputColor = float3(0.0f, 0.0f, 0.0f);

    // サンプリング点数を制限（最低 1、最大 100 程度にクランプするのが安全だが、
    // ここではエンジンのパラメータを信頼する）
    int numSamples = max(1, gParams.numSamples);

    for (int i = 0; i < numSamples; ++i) {
        // 現在の UV から放射状にサンプリング点を進めながらサンプリングしていく
        float2 texcoord = input.texcoord + direction * gParams.blurWidth * (float)i;
        outputColor.rgb += gTexture.Sample(gSamplerClamp, texcoord).rgb;
    }
    
    // 平均化する
    outputColor.rgb /= (float)numSamples;
    
    PS_OUTPUT output;
    output.color = float4(outputColor, 1.0f);
    return output;
}
