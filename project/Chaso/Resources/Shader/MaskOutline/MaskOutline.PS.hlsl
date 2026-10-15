#include "../Fullscreen/Fullscreen.hlsli"

// ============================================================================
// マスクアウトライン
//
// Mask.PS.hlsl が書いた「強調対象のシルエット」を膨張させ、元のシルエットとの
// 差分＝オブジェクトのすぐ外側のリングだけを塗る。
// 深度ベースアウトライン（画面全体）とは独立したパスなので、
// 「対象だけ白く太く」を全体の輪郭設定とは別に持てる。
//
// t1 に来るのはマスク（RenderContext のマスク RT）。
// s1（ポイントサンプラ）は WRAP なので、UV は自分で clamp して画面端の回り込みを防ぐ。
// ============================================================================

Texture2D<float4> gTexture : register(t0);
Texture2D<float4> gMaskTexture : register(t1);

SamplerState gSampler : register(s0);
SamplerState gSamplerPoint : register(s1);

/// @brief 輪郭を出さない矩形（HUD の領域）の上限
/// @details このパスは 2D まで描き終えた最終画に掛かるので、放っておくと
///          HP バーやマップの上に輪郭が浮いてしまう。UI 側が申告した矩形を素通しにする。
static const int kMaxExclusions = 16;

/// @struct MaskOutlineParams
/// @brief マスクアウトラインのパラメータ（b1）
struct MaskOutlineParams {
    float4 color;     ///< 輪郭色（a は合成の最大不透明度）
    float thickness;  ///< 輪郭のピクセル幅（シェーダ内で 1〜2 に丸める）
    float strength;   ///< 全体の強さ（0〜1）
    int excludeCount; ///< 有効な除外矩形の数
    float padding;    ///< 16 バイト境界合わせ
    float4 excludeRects[kMaxExclusions]; ///< (minU, minV, maxU, maxV)
};
ConstantBuffer<MaskOutlineParams> gParams : register(b1);

/// @brief マスクを 1 点サンプリングする（画面外は端の値で clamp）
/// @param uv サンプリング位置
/// @return マスク値（0 = 対象外, 1 = 対象）
float SampleMask(float2 uv) {
    return gMaskTexture.SampleLevel(gSamplerPoint, saturate(uv), 0).r;
}

/// @brief マスクを膨張させた差分（対象のすぐ外側）に輪郭色を合成する
/// @param input フルスクリーン VS の出力
/// @return 出力色
float4 main(VertexShaderOutput input) : SV_TARGET {
    float4 base = gTexture.Sample(gSampler, input.texcoord);

    // UI の上には輪郭を出さない（HUD が申告した矩形の中は素通し）
    for (int i = 0; i < gParams.excludeCount; ++i) {
        const float4 rect = gParams.excludeRects[i];
        if (input.texcoord.x >= rect.x && input.texcoord.x <= rect.z &&
            input.texcoord.y >= rect.y && input.texcoord.y <= rect.w) {
            return base;
        }
    }

    uint width, height;
    gMaskTexture.GetDimensions(width, height);
    const float2 texel = float2(1.0f / width, 1.0f / height);

    // 中心が対象の内側なら輪郭は描かない（外側リングだけを塗るため早期脱出）
    const float center = SampleMask(input.texcoord);
    if (center >= 1.0f) {
        return base;
    }

    // 半径 r ピクセルの円形カーネルで最大値を取る＝マスクの膨張
    // 1 ピクセル刻みで回すので、太さを上げても輪郭に隙間ができない
    const int r = (int)clamp(round(gParams.thickness), 1.0f, 2.0f);
    float dilated = 0.0f;
    for (int y = -r; y <= r; ++y) {
        for (int x = -r; x <= r; ++x) {
            if (x * x + y * y > r * r) continue; // 円の外は捨てて角張りを防ぐ
            dilated = max(dilated, SampleMask(input.texcoord + float2(x, y) * texel));
        }
    }

    float edge = saturate(dilated - center) * saturate(gParams.strength);
    edge *= gParams.color.a;

    return float4(lerp(base.rgb, gParams.color.rgb, edge), base.a);
}
