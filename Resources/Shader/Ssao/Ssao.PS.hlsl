#include "../Fullscreen/Fullscreen.hlsli"

// =============================================================================
// SSAO（スクリーンスペース・アンビエントオクルージョン）
//
// 壁と床の隅、物の接地部分を暗くして「そこに接している」感を出す。
// タイルで組んだ部屋は陰影が平坦になりがちなので、空間の読みやすさに効く。
//
// G-Buffer が無いので、Caustics / LightShaft と同じやり方で
// 深度からビュー空間の位置を復元し、その画面空間微分（ddx/ddy）で法線を作る。
//
// サンプルは固定の 16 点スパイラル。ランダム回転を掛けないのでノイズが出ず、
// AO 用のブラーパスが不要（そのぶん平坦な面にごく薄い縞が出ることがある）。
//
// 前提:
//   - t1 は深度 SRV（DXGI_FORMAT_R24_UNORM_X8_TYPELESS）
//   - 行列は row-vector 規約（DXC に -Zpr が渡されている）
//   - 積む順序は前のほう（アウトラインの次・Bloom より前）。
//     色を乗算するだけなので厳密な位置は問わないが、にじみや色調整の前に置く
// =============================================================================

Texture2D<float32_t4> gTexture : register(t0);
Texture2D<float32_t> gDepthTexture : register(t1);

SamplerState gSampler : register(s0);
SamplerState gSamplerPoint : register(s1); // ポイント（深度は補間してはいけない）

// b1: SSAO パラメータ
cbuffer SsaoParams : register(b1) {
    float4x4 projectionInverse; ///< プロジェクション逆行列
    float radius;               ///< サンプリング半径（ワールド単位＝メートル）
    float intensity;            ///< 効きの強さ（0 で無効）
    float bias;                 ///< 自己遮蔽を避けるための下駄（メートル）
    float power;                ///< AO のコントラスト（pow の指数）
};

/// @struct PixelShaderOutPut
/// @brief ピクセル出力
struct PixelShaderOutPut {
    float4 color : SV_TARGET0; ///< 出力色
};

// 16 点スパイラル（単位円内。半径が徐々に広がるので近接と遠方を同時に拾える）
static const float2 kKernel[16] = {
    float2( 0.2500f,  0.0000f), float2(-0.1768f,  0.1768f),
    float2( 0.0000f, -0.3536f), float2( 0.3062f,  0.3062f),
    float2(-0.5000f,  0.0000f), float2( 0.3889f, -0.3889f),
    float2( 0.0000f,  0.6250f), float2(-0.4773f, -0.4773f),
    float2( 0.7500f,  0.0000f), float2(-0.5657f,  0.5657f),
    float2( 0.0000f, -0.8750f), float2( 0.6540f,  0.6540f),
    float2(-1.0000f,  0.0000f), float2( 0.7424f, -0.7424f),
    float2( 0.0000f,  0.9375f), float2(-0.4243f, -0.4243f)
};

/// @brief 深度からビュー空間座標を復元する
/// @param uv    画面 UV
/// @param depth 深度バッファの値
/// @return ビュー空間座標
float3 ReconstructViewPos(float2 uv, float depth) {
    const float4 ndc = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, depth, 1.0f);
    const float4 viewPos = mul(ndc, projectionInverse);
    return viewPos.xyz / viewPos.w;
}

/// @brief 周囲の深度から遮蔽度を求め、シーンの色に AO を乗算する
/// @param input フルスクリーン VS の出力
/// @return 出力色
PixelShaderOutPut main(VertexShaderOutput input) {
    PixelShaderOutPut output;

    const float4 base = gTexture.Sample(gSampler, input.texcoord);

    if (intensity <= 0.0f) {
        output.color = base;
        return output;
    }

    const float2 uv = input.texcoord;
    const float centerDepth = gDepthTexture.SampleLevel(gSamplerPoint, uv, 0);

    // 何も描かれていない（＝遠クリップ）画素は陰を付けない。
    // ここで return してはいけない。下で ddx/ddy を使うため、画素ごとに分岐すると
    // クアッド内でレーンが分かれて微分が未定義になり、空とのシルエット境界に
    // 誤った AO の縁が出る。Caustics.PS.hlsl と同じく乗算マスクで無効化する。
    const float sceneMask = step(centerDepth, 0.99999f);

    const float3 centerView = ReconstructViewPos(uv, centerDepth);

    // 深度の画面空間微分から法線を作る（エッジでは壊れるが、AO は薄いので実用上問題ない）
    float3 normal = normalize(cross(ddy(centerView), ddx(centerView)));
    normal *= (dot(normal, centerView) > 0.0f) ? -1.0f : 1.0f; // 必ずカメラ側を向かせる

    uint width, height;
    gDepthTexture.GetDimensions(width, height);
    const float2 texel = float2(1.0f / width, 1.0f / height);

    // ワールド半径 radius が、この深度では画面上で何ピクセルに相当するか。
    // 遠くの物ほど小さく探索する（遠景が真っ黒にならないように）
    //
    // 射影行列の縦スケール（= 1/tan(fovY/2)）が必要だが、ここには逆行列しか無い。
    // 透視投影なら逆行列の _m11 がその逆数（= tan(fovY/2)）なので、割り戻して使う。
    // これを掛けないと FOV が反映されず、サンプル分布と下の falloff の
    // ワールド半径が食い違って距離減衰が効かなくなる。
    const float projScaleY = 1.0f / max(abs(projectionInverse._m11), 1e-6f);
    const float viewZ = max(centerView.z, 0.001f);
    const float pixelRadius = radius * projScaleY / viewZ * (float)height * 0.5f;

    float occlusion = 0.0f;

    [unroll]
    for (int i = 0; i < 16; ++i) {
        const float2 sampleUv = uv + kKernel[i] * pixelRadius * texel;

        // 画面外はサンプルしない（クランプで端の値を引き伸ばすと壁際が黒く縁取られる）
        if (any(sampleUv < 0.0f) || any(sampleUv > 1.0f)) continue;

        const float sampleDepth = gDepthTexture.SampleLevel(gSamplerPoint, sampleUv, 0);
        if (sampleDepth >= 1.0f) continue;

        const float3 sampleView = ReconstructViewPos(sampleUv, sampleDepth);
        const float3 diff = sampleView - centerView;
        const float dist = length(diff);
        if (dist < 1e-4f) continue;

        // 法線より前（手前）にある点だけを遮蔽として数える
        const float cosine = dot(normal, diff / dist);
        const float occluded = saturate(cosine - bias / max(dist, 1e-4f));

        // 半径から外れるほど寄与を落とす（遠くの物が手前を暗くしないように）
        const float falloff = saturate(1.0f - dist / max(radius, 1e-4f));

        occlusion += occluded * falloff;
    }

    occlusion = saturate(occlusion / 16.0f);
    float ao = 1.0f - pow(occlusion, max(power, 0.01f)) * intensity;

    // 空・天球（遠クリップ）は陰を付けない
    ao = lerp(1.0f, saturate(ao), sceneMask);

    output.color = float4(base.rgb * ao, base.a);
    return output;
}
