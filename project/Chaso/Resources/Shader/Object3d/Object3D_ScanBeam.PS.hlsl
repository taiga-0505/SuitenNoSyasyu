#include "Object3d.hlsli"

// ============================================================================
// Object3D_ScanBeam.PS
//   ドアから場所指定サークルへ伸びる「接続ビーム」用エフェクトシェーダ。
//   RC::GeneratePlane() で作った 1x1 の板（XZ平面）に貼って使う。
//   ローカル +Z をビームの進行方向、scale.z を全長、scale.x を幅として使う想定。
//
//   UV の約束（MeshGenerator::GeneratePlane）:
//     u = 幅方向 0..1（0.5 が中心線）
//     v = 長さ方向。v=0 が +Z 端 / v=1 が -Z 端
//     → ドアを -Z 側に置くので along = 1 - v が「ドアからの距離比」になる
//
//   マテリアル定数の使い方（RC::SetPrimitiveMeshEffectParams が設定する）:
//     color.rgb              = ビームの発光色
//     color.a                = 全体の明度スケール
//     shininess              = 伸長率 0..1（ここまでしか描かない）
//     environmentCoefficient = 経過秒数（流れるパルスの位相）
//
//   加算合成（kBlendModeAdd）・深度書き込みOFF・カリング無しの PSO
//   (object3d_scanbeam) 専用。
// ============================================================================

/// @brief マテリアル定数（b0）。エフェクト用に一部を流用する
struct Material
{
    float4 color;                 ///< 発光色（RGB）＋ 明度スケール（A）
    int lightingMode;             ///< 未使用
    float shininess;              ///< 伸長率（0〜1）として流用
    float environmentCoefficient; ///< 経過秒数として流用
    int useNormalMap;             ///< 未使用
    int useRoughnessMap;          ///< 未使用
    float2 padding;               ///< パディング
    float4x4 uvTransform;         ///< 未使用（恒等行列のまま）
};

ConstantBuffer<Material> gMaterial : register(b0);

struct PixelShaderOutput
{
    float4 color : SV_TARGET0; ///< 出力色（加算合成。rgb は発光色、a が強さ）
};

static const float kTau = 6.28318530718f; ///< 2π

static const float kPulseCount = 5.0f;  ///< ビーム上に同時に走るパルス数
static const float kPulseSpeed = 1.9f;  ///< パルスの流れる速さ

/// @brief 接続ビームの発光を出力する
/// @param input 頂点シェーダーからの補間値（texcoord だけを使う）
/// @return 出力色
PixelShaderOutput main(VertexShaderOutput input)
{
    // 幅方向：中心 0 → 端 1
    const float across = saturate(abs(input.texcoord.x - 0.5f) * 2.0f);
    // 長さ方向：ドア側 0 → サークル側 1
    const float along = saturate(1.0f - input.texcoord.y);

    const float deploy = saturate(gMaterial.shininess);
    const float t = gMaterial.environmentCoefficient;

    const float center = saturate(1.0f - across);

    // --- 芯線とその周りのにじみ ---
    const float core = pow(center, 7.0f);
    const float glow = pow(center, 1.6f) * 0.18f;

    // --- ドア→サークルへ流れるパルス ---
    const float wave = sin((along * kPulseCount - t * kPulseSpeed) * kTau) * 0.5f + 0.5f;
    const float pulse = pow(saturate(wave), 8.0f) * 0.9f;

    // --- 伸長マスク：先端より先は描かない ---
    const float mask = smoothstep(deploy, deploy - 0.06f, along);

    // --- 先端の閃光 ---
    // ガウシアンは pow() ではなく自乗で書く（HLSL の pow は基数が負だと NaN になる）
    const float tipD = (along - deploy) * 40.0f;
    const float tip = exp(-(tipD * tipD)) * 1.5f * (center * center);

    // 伸長が始まる前（deploy≈0）に先端の閃光だけが点かないよう立ち上がりで抑える
    float intensity = (core + glow + pulse * (core + 0.22f)) * mask
                    + tip * saturate(deploy * 20.0f);

    // --- 微弱な明滅 ---
    intensity *= 0.88f + 0.12f * sin(t * 9.0f);

    PixelShaderOutput output;
    output.color = float4(gMaterial.color.rgb, saturate(intensity) * gMaterial.color.a);
    return output;
}
