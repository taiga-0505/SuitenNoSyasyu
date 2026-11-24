// ============================================================================
// Object3D_InkCloud.PS.hlsl
// ----------------------------------------------------------------------------
// 墨の煙パーティクル（EffectParticleSystem の InkCloud）用のピクセルシェーダー。
// 以前は水球シェーダー（Object3D_WaterBall）で描いていたが、あちらは
//   - 縁ほど不透明（フレネル）・空の映り込みと青い発光を足す
// ので、暗い色を渡しても「縁の光る透明なシャボン玉」に見えて墨にならなかった。
// ここでは逆に
//   - 中心（視線に正対する所）ほど濃く、縁ほど透ける
//   - ノイズで輪郭を崩して、ちぎれた煙の形にする
//   - 色は墨色そのまま（光でわずかに明暗を付けるだけ。映り込み・発光は足さない）
// にする。プレマルチプライド済みアルファで出力する（PSO: object3d_inkcloud_inst）。
// ============================================================================

#include "Object3d.hlsli"

// ---------- 定数バッファ（Object3D / WaterBall と同じ並び。使う物だけ宣言する） ----------

/// @brief マテリアル定数（b0）
struct Material
{
    float4 color;                 ///< 乗算色（EffectParticleSystem は白を渡す）
    int lightingMode;             ///< 未使用
    float shininess;              ///< 未使用
    float environmentCoefficient; ///< 未使用
    float padding;                ///< パディング
    float4x4 uvTransform;         ///< 未使用
};
ConstantBuffer<Material> gMaterial : register(b0);

/// @brief 平行光源と環境光（b1）
struct DirectionalLight
{
    float4 color;           ///< 光の色
    float3 direction;       ///< 光の進む向き
    float intensity;        ///< 強度
    float3 ambientColor;    ///< 環境光の色
    float ambientIntensity; ///< 環境光の強さ
};
ConstantBuffer<DirectionalLight> gDirectionalLight : register(b1);

/// @brief カメラ情報（b2）
struct Camera
{
    float3 worldPosition; ///< カメラのワールド座標
};
ConstantBuffer<Camera> gCamera : register(b2);

struct PixelShaderOutput
{
    float4 color : SV_TARGET0; ///< 出力色（プレマルチプライド済み）
};

/// @brief ハッシュによる簡易な疑似乱数
/// @param p 入力座標
/// @return 0〜1 の乱数
float InkHash(float3 p)
{
    p = frac(p * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return frac((p.x + p.y) * p.z);
}

/// @brief 3D バリューノイズ
/// @param p サンプル位置
/// @return 0〜1 のノイズ値
float InkNoise(float3 p)
{
    float3 i = floor(p);
    float3 f = frac(p);
    f = f * f * (3.0 - 2.0 * f);
    float n000 = InkHash(i + float3(0, 0, 0));
    float n100 = InkHash(i + float3(1, 0, 0));
    float n010 = InkHash(i + float3(0, 1, 0));
    float n110 = InkHash(i + float3(1, 1, 0));
    float n001 = InkHash(i + float3(0, 0, 1));
    float n101 = InkHash(i + float3(1, 0, 1));
    float n011 = InkHash(i + float3(0, 1, 1));
    float n111 = InkHash(i + float3(1, 1, 1));
    float nx00 = lerp(n000, n100, f.x);
    float nx10 = lerp(n010, n110, f.x);
    float nx01 = lerp(n001, n101, f.x);
    float nx11 = lerp(n011, n111, f.x);
    return lerp(lerp(nx00, nx10, f.y), lerp(nx01, nx11, f.y), f.z);
}

/// @brief 墨の煙を描く
/// @param input 頂点シェーダーからの補間値（instColor: 墨の色と不透明度）
/// @return 出力色（プレマルチプライド済み）
PixelShaderOutput main(VertexShaderOutput input)
{
    PixelShaderOutput output;

    float3 N = normalize(input.normal);
    float3 V = normalize(gCamera.worldPosition - input.worldPosition);
    float facing = saturate(dot(N, V)); // 1: 正対（煙の芯）、0: 輪郭

    // 輪郭をノイズで崩す（2 オクターブ。ワールド座標なので、膨らむと模様が流れて煙らしく動く）
    float3 wp = input.worldPosition;
    float n = InkNoise(wp * 2.3) * 0.65 + InkNoise(wp * 5.1 + 7.7) * 0.35;

    // 芯ほど濃く、縁ほど透ける。ノイズで縁を食い込ませて丸い球に見せない
    float density = smoothstep(0.15, 0.85, facing * 0.8 + (n - 0.5) * 0.9);
    float alpha = saturate(input.instColor.a * gMaterial.color.a * density);
    if (alpha <= 0.002)
        discard;

    // 墨色に光でわずかな明暗だけ付ける（真っ黒だと水中の暗い背景に溶けるので、環境光で少し持ち上げる）
    float3 L = normalize(-gDirectionalLight.direction);
    float lit = saturate(dot(N, L) * 0.5 + 0.5);
    float3 light = gDirectionalLight.color.rgb * gDirectionalLight.intensity * lerp(0.35, 0.8, lit)
                 + gDirectionalLight.ambientColor * gDirectionalLight.ambientIntensity;
    float3 inkColor = input.instColor.rgb * gMaterial.color.rgb * light;

    output.color = float4(inkColor * alpha, alpha);
    return output;
}
