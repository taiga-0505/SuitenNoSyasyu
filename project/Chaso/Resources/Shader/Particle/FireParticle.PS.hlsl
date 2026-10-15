// ============================================================================
// FireParticle.PS.hlsl
// ----------------------------------------------------------------------------
// 炎パーティクル用ピクセルシェーダ（gpu_particle_fire PSO）。
// UV から手続き的に炎の粒を描くので、テクスチャの絵柄には依存しない
// （BubbleParticle.PS と同じ方針。gTexture は宣言のみで参照しない）。
//
// 加算合成（kBlendModeAdd）前提。重なった部分が白熱して芯ができる。
//
// やっていること:
//   1. UV を横に縮めて縦長の粒にする → 上に伸びる炎らしいシルエット
//   2. 寿命が進むほど粒を細くし、上端へ寄せる → 先細り
//   3. 中心を白熱色へ寄せる → 炎の芯
//   4. 固有乱数を位相にした高速なちらつき
// ============================================================================

#include "GPUParticle.hlsli"

Texture2D<float4> gTexture : register(t0); // 手続き描画のため参照しない（バインドの互換用）
SamplerState gSampler : register(s0);

/// @struct PixelShaderOutput
/// @brief ピクセル出力
struct PixelShaderOutput
{
    float4 color : SV_TARGET0; ///< 出力色
};

/// @brief 粒の縦横比。1 より大きいほど縦長になる
static const float kAspect = 2.1f;

/// @brief 白熱した芯の色（加算されるので 1 を超える値にはしない）
static const float3 kCoreColor = float3(1.0f, 0.95f, 0.75f);

/// @brief 板ポリゴンに手続き的に炎の粒を描く
/// @param input GPUParticle.VS の出力（particleParams に寿命の進行度と固有乱数）
/// @return 出力色（半径外とほぼ透明なピクセルは discard）
PixelShaderOutput main(VertexShaderOutput input)
{
    PixelShaderOutput output;

    const float age = input.particleParams.x;  // 0 = 生成直後, 1 = 消滅直前
    const float rnd = input.particleParams.y;  // パーティクル固有の乱数

    // --- 1. UV を -1〜1 に変換し、横を縮めて縦長の粒にする ---
    float2 p = (input.texcoord - 0.5f) * 2.0f;
    p.x *= kAspect;

    // --- 2. 寿命が進むほど上へ寄せて細くする（炎の先細り）---
    // 上に流れながら細くなることで、粒ひとつでも炎の舌のように見える。
    // ※ テクスチャ座標の V は下向きが正なので、上へ寄せるには足す
    p.y += age * 0.55f;
    float radius = lerp(1.0f, 0.42f, age);

    float dist = length(p);

    // 半径外は描かない（加算合成では四角い縁が目立つため必ず切る）
    if (dist > radius)
    {
        discard;
    }

    // --- 3. 中心から外周への減衰 ---
    // pow でカーブを付け、中心付近に密度を集める（ふわっとした縁になる）
    float falloff = saturate(1.0f - dist / radius);
    float body = pow(falloff, 1.8f);

    // --- 4. 白熱した芯 ---
    // 生まれたばかりのパーティクルほど芯を強く出す（根元が最も熱い）
    float core = pow(saturate(1.0f - dist / (radius * 0.55f)), 3.0f);
    float coreHeat = core * (1.0f - age) * (1.0f - age);

    // --- 5. ちらつき ---
    // 位相を固有乱数でずらすので、炎全体が同時に明滅することはない
    float flicker = 0.82f + 0.18f * sin(age * 34.0f + rnd * 62.8f);

    // --- 6. 合成 ---
    // input.color は UpdateFire が startColor → endColor へ遷移させた色。
    // つまりエディタの色設定がそのまま炎の色になる。
    float3 rgb = input.color.rgb * body + kCoreColor * coreHeat;
    float alpha = input.color.a * body * flicker;

    output.color = float4(rgb, saturate(alpha));

    if (output.color.a < 0.004f)
    {
        discard;
    }

    return output;
}
