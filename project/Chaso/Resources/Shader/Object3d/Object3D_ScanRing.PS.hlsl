#include "Object3d.hlsli"

// ============================================================================
// Object3D_ScanRing.PS
//   特殊ドアの「場所指定ホログラム」用エフェクトシェーダ。
//   RC::GenerateRing() で作ったリングメッシュ（XZ平面）に貼って使う。
//
//   UV の約束（MeshGenerator::GenerateRing / GenerateRingEx(isVerticalUV=false)）:
//     u = 円周方向の比率 0..1
//     v = 半径方向。内周側 = 1.0 / 外周側 = 0.0
//
//   マテリアル定数の使い方（RC::SetPrimitiveMeshEffectParams が設定する）:
//     color.rgb              = ホログラムの発光色
//     color.a                = 全体の明度スケール（フェード用）
//     shininess              = スキャン進捗 0..1（この角度までが「点灯済み」）
//     environmentCoefficient = 経過秒数（脈動・走査バンドのアニメーション位相）
//
//   加算合成（kBlendModeAdd）・深度書き込みOFF・カリング無しの PSO
//   (object3d_scanring) 専用。ライティングもテクスチャも参照しない。
// ============================================================================

/// @brief マテリアル定数（b0）。エフェクト用に一部を流用する
struct Material
{
    float4 color;                 ///< 発光色（RGB）＋ 明度スケール（A）
    int lightingMode;             ///< 未使用
    float shininess;              ///< スキャン進捗（0〜1）として流用
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

// 外周のグロー。アウトラインを引くようになったので、そちらに主役を譲るため
// 「細く鋭い光」から「線の内側へ広くにじむ弱い光」に変えている。
static const float kRimPower = 10.0f;      ///< 外周グローの広がり（小さいほど内側まで広がる）
static const float kRimLevel = 0.5f;       ///< 外周グローの明るさ（アウトラインより暗くする）
static const float kInnerRingPos = 0.17f;  ///< 内側の二重リング位置（半径比）
static const float kDashCount = 24.0f;     ///< 円周方向の破線数
static const float kSpokeCount = 8.0f;     ///< 放射スポーク数
static const float kBandSpeed = 0.55f;     ///< 走査バンドが半径方向へ流れる速さ
static const float kIdleLevel = 0.30f;     ///< 未点灯側（進捗より先の角度）の明るさ

// 外周アウトライン（境界を示すキレのある 1 本線）
static const float kOutlinePixels = 2.5f;  ///< 線の太さ（ピクセル）
static const float kOutlineLevel = 1.0f;   ///< 線の明るさ（0〜1）
static const float kOutlineMaxWidth = 0.15f; ///< radial 換算の太さ上限（真横から見たときの暴走防止）

/// @brief 場所指定ホログラムのリングの発光を出力する
/// @param input 頂点シェーダーからの補間値（texcoord だけを使う）
/// @return 出力色
PixelShaderOutput main(VertexShaderOutput input)
{
    const float angle = frac(input.texcoord.x);       // 0..1 円周
    const float radial = saturate(input.texcoord.y);  // 0 = 外周 / 1 = 中心
    const float progress = saturate(gMaterial.shininess);
    const float t = gMaterial.environmentCoefficient;

    const float edge = saturate(1.0f - radial); // 0 = 中心 / 1 = 外周

    // --- 外周のグロー（アウトラインの内側へにじむ光） ---
    const float rim = pow(edge, kRimPower) * kRimLevel;
    const float rimInner = pow(saturate(1.0f - abs(radial - kInnerRingPos) * 14.0f), 3.0f) * 0.55f;

    // --- 床に落ちる薄いヘイズ（円盤の内側を薄く塗る） ---
    const float haze = pow(radial, 0.85f) * 0.09f + 0.04f;

    // --- 半径方向へ流れる走査バンド ---
    // ガウシアンは pow() ではなく自乗で書く（HLSL の pow は基数が負だと NaN になる）
    const float bandPos = frac(t * kBandSpeed);
    const float bandD = (radial - bandPos) * 7.0f;
    const float band = exp(-(bandD * bandD)) * 0.5f;

    // --- 円周方向の破線（外周寄りだけに乗せる） ---
    const float dash = smoothstep(0.28f, 0.42f, abs(frac(angle * kDashCount) - 0.5f));
    const float ticks = dash * pow(edge, 5.0f) * 0.45f;

    // --- 放射スポーク（旧 DrawLine3D のスポーク相当） ---
    const float spokeMask = pow(saturate(1.0f - abs(frac(angle * kSpokeCount + 0.5f) - 0.5f) * 26.0f), 2.0f);
    const float spoke = spokeMask * (0.16f + 0.30f * edge);

    float intensity = rim + rimInner + haze + band + ticks + spoke;

    // --- 進捗アーク：点灯済みの角度は明るく、未点灯側は輪郭だけ残す ---
    const float filled = step(angle, progress);
    intensity *= lerp(kIdleLevel, 1.0f, filled);

    // --- 進捗の先端フレア（走査中だけ光らせる） ---
    const float leadGate = step(0.005f, progress) * step(progress, 0.995f);
    const float leadD = (angle - progress) * 90.0f;
    const float lead = exp(-(leadD * leadD)) * 1.4f * leadGate;
    intensity += lead * (0.35f + 0.65f * edge);

    // --- 全体の脈動 ---
    intensity *= 0.86f + 0.14f * sin(t * 7.0f);

    // --- 外周アウトライン ---
    // 「ここが範囲の境界」を示す線なので、進捗アークや脈動の影響を受けず常に全周同じ明るさ。
    // 太さは fwidth() でピクセル単位に換算して決めるため、サークルの大小（フェーズ1の
    // 大サークルとフェーズ2/3の小サークル）やカメラ距離が変わっても線幅が崩れない。
    // 加算合成で内側のグローと足し合わせると白飛びするので max() で置き換える。
    const float radialPerPixel = max(fwidth(input.texcoord.y), 1e-5f);
    const float outlineWidth = min(kOutlinePixels * radialPerPixel, kOutlineMaxWidth);
    const float outline = 1.0f - smoothstep(outlineWidth - radialPerPixel, outlineWidth, radial);
    intensity = max(intensity, outline * kOutlineLevel);

    PixelShaderOutput output;
    output.color = float4(gMaterial.color.rgb, saturate(intensity) * gMaterial.color.a);
    return output;
}
