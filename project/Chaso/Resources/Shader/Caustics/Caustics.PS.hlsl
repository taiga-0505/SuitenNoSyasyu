#include "../Fullscreen/Fullscreen.hlsli"
#include "Caustics.hlsli"

// =============================================================================
// Caustics ポストエフェクト
//
// 水面から差し込む光の網目模様をスクリーンスペースで加算合成する。
//
//   1. 深度 → NDC → ビュー空間 →（viewInverse）→ ワールド座標を復元
//   2. ddx/ddy でビュー空間法線を復元し、ワールド空間へ変換
//   3. ワールド XZ 平面を UV として手続き型 Voronoi を 2 層評価し、
//      セル境界（F2 - F1 が小さい領域）を「網目」として抽出
//   4. 上向き法線・水面からの深さ・カメラ距離で減衰させて加算
//
// 前提:
//   - t1 は深度 SRV（DXGI_FORMAT_R24_UNORM_X8_TYPELESS）
//   - 行列は row-vector 規約（DXC に -Zpr が渡されているため mul(vector, matrix)）
//   - G-Buffer が無いため法線は深度の画面空間微分から復元している。
//     ジオメトリのエッジでは法線が壊れるが、加算量が僅かなので実用上問題ない。
// =============================================================================

// t0: シーンカラー
Texture2D<float32_t4> gTexture : register(t0);
// t1: 深度バッファ
Texture2D<float32_t> gDepthTexture : register(t1);

SamplerState gSamplerLinear : register(s0); // リニア＋ラップ
SamplerState gSamplerPoint : register(s1);  // ポイント＋ラップ

// b1: Caustics パラメータ
cbuffer CausticsParams : register(b1) {
    float4x4 projectionInverse; ///< プロジェクション逆行列
    float4x4 viewInverse;       ///< ビュー逆行列（= カメラのワールド行列）
    float4 causticsColor;       ///< 網目の発光色（RGB / A は未使用）
    float time;                 ///< 経過時間
    float intensity;            ///< 全体の強さ
    float scale;                ///< ワールド 1 単位あたりのタイル密度
    float speed;                ///< 模様のうねり速度
    float contrast;             ///< 網目のコントラスト（pow の指数）
    float chromaticOffset;      ///< 色収差量（0 で無効）
    float waterHeight;          ///< 水面のワールド Y 座標
    float depthFadeDistance;    ///< 水面からこの距離で減衰しきる
    float upwardBias;           ///< 上向き法線への偏り（0: 無指向 〜 1: 完全に Y 依存）
    float distanceFadeStart;    ///< カメラからの距離減衰の開始
    float distanceFadeEnd;      ///< カメラからの距離減衰の終了
    float lerpFactor;           ///< 水中ブレンド率（0: 適用しない 〜 1: 完全適用）
};

/// @struct PixelShaderOutPut
/// @brief ピクセル出力
struct PixelShaderOutPut {
    float4 color : SV_TARGET0; ///< 出力色
};

// -----------------------------------------------------------------------------
// 網目模様の評価
// 実体は Caustics.hlsli にあり、LightShaft.PS.hlsl と共有している。
// （同じ time / speed / contrast を渡せば光柱の断面と床の網目が一致する）
// -----------------------------------------------------------------------------
/// @brief cbuffer の time / speed / contrast で網目模様を評価する
/// @param worldUV スケール適用済みのワールド XZ 座標
/// @return caustics 強度（0〜1）
float SampleCaustics(float2 worldUV) {
    return CausticsPattern(worldUV, time, speed, contrast);
}

// -----------------------------------------------------------------------------
// 深度からビュー空間座標を復元する
// -----------------------------------------------------------------------------
/// @brief 深度からビュー空間座標を復元する
/// @param uv    画面 UV
/// @param depth 深度バッファの値
/// @return ビュー空間座標
float3 ReconstructViewPos(float2 uv, float depth) {
    float4 ndc = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, depth, 1.0f);
    float4 viewPos = mul(ndc, projectionInverse);
    return viewPos.xyz / viewPos.w;
}

/// @brief 復元したワールド座標で網目を評価し、減衰を掛けてシーンに加算する
/// @param input フルスクリーン VS の出力
/// @return 出力色（α は 1 固定）
PixelShaderOutPut main(VertexShaderOutput input) {
    PixelShaderOutPut output;

    float2 uv = input.texcoord;
    float4 baseColor = gTexture.Sample(gSamplerLinear, uv);

    // 適用しない場合は早期に抜ける（分岐は画面全体で一様なのでコストゼロ）
    if (lerpFactor <= 0.0f || intensity <= 0.0f) {
        output.color = float4(baseColor.rgb, 1.0f);
        return output;
    }

    // --- 深度 → ビュー空間 → ワールド空間 ---
    float depthVal = gDepthTexture.Sample(gSamplerPoint, uv);

    // 遠クリップ面（空・スカイボックス）には caustics を落とさない
    float sceneMask = step(depthVal, 0.99999f);

    float3 viewPos = ReconstructViewPos(uv, depthVal);
    float3 worldPos = mul(float4(viewPos, 1.0f), viewInverse).xyz;

    // --- 法線復元（ビュー空間座標の画面空間微分の外積）---
    float3 viewDdx = ddx(viewPos);
    float3 viewDdy = ddy(viewPos);
    float3 viewNormal = normalize(cross(viewDdy, viewDdx));

    // 外積の符号は射影行列とワインディングに依存するため、
    // 必ずカメラを向くように反転して正規化する（viewPos = 視点→表面のベクトル）
    viewNormal *= (dot(viewNormal, viewPos) > 0.0f) ? -1.0f : 1.0f;

    float3 worldNormal = normalize(mul(float4(viewNormal, 0.0f), viewInverse).xyz);

    // --- ワールド XZ 平面へ投影して網目を評価 ---
    float2 worldUV = worldPos.xz * scale;
    float caustics = SampleCaustics(worldUV);

    // --- 色収差 ---
    // 網目を 3 回評価すると重いので、画面空間の勾配を使った 1 次近似で
    // R/B をずらす。ほぼ無コストでプリズム感が出る。
    float3 causticsRGB = caustics.xxx;
    if (chromaticOffset > 0.0f) {
        // ddx + ddy だと 45 度方向で打ち消し合ってしまうので、勾配の大きさを使う
        float gradient = length(float2(ddx(caustics), ddy(caustics)));
        float shift = gradient * chromaticOffset;
        causticsRGB = float3(caustics + shift, caustics, caustics - shift);
        causticsRGB = saturate(causticsRGB);
    }

    // --- 減衰 ---

    // 上向き法線ほど強く。upwardBias = 0 なら無指向。
    float upward = saturate(worldNormal.y);
    float normalWeight = lerp(1.0f, upward, upwardBias);

    // 水面からの深さによる減衰（深いところほど光が届かない）
    float depthBelowSurface = waterHeight - worldPos.y;
    float depthFade = 1.0f - saturate(depthBelowSurface / max(depthFadeDistance, 0.001f));

    // 水面より上のピクセルには落とさない（境界は少しだけぼかす）
    float surfaceMask = saturate(depthBelowSurface * 4.0f);

    // カメラからの距離による減衰（遠景でモアレになるのを防ぐ）
    float distanceFade = 1.0f - saturate(
        (viewPos.z - distanceFadeStart) /
        max(distanceFadeEnd - distanceFadeStart, 0.001f));

    float attenuation = intensity * normalWeight * depthFade *
                        surfaceMask * sceneMask * distanceFade * lerpFactor;

    // --- 加算合成 ---
    float3 finalColor = baseColor.rgb + causticsColor.rgb * causticsRGB * attenuation;

    output.color = float4(finalColor, 1.0f);
    return output;
}
