// ============================================================================
// ElectricParticle.PS.hlsl
// ----------------------------------------------------------------------------
// 電撃パーティクル用ピクセルシェーダ（gpu_particle_electric PSO）。
// UV から手続き的に「ギザギザの稲妻 1 本（＋枝分かれ）と発光する芯」を描くので、
// テクスチャの絵柄には依存しない（FireParticle.PS と同じ方針。gTexture は宣言のみ）。
//
// 加算合成（kBlendModeAdd）前提。重なった部分が白く飛んで芯ができる。
//
// やっていること:
//   1. UV を -1〜1 に変換し、固有乱数で板ごとに回転 → 稲妻の向きがバラける
//   2. 折れ線の稲妻（幹）を描く。折れ点は乱数で決め、寿命が進むごとにシードが
//      切り替わる → 形がバチバチと変わる
//   3. 幹は「大きな折れ 5 分割」＋「細かなギザギザ 13 分割」の 2 段重ね
//   4. 幹の途中から枝を 1 本出す（乱数で有無・向きが決まる）
//   5. 中心に発光ドット → 小さな板でも「火花の粒」として見える
//   6. シードが切り替わるたびに明るさもランダムに変わる → 明滅
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

/// @brief 芯の太さ（板の半幅を 1 としたときの比）
static const float kCoreWidth = 0.045f;

/// @brief にじみ（グロー）の広がり
static const float kGlowWidth = 0.20f;

/// @brief 寿命 1 回分（age 0→1）の間に稲妻の形が切り替わる回数
static const float kShapeRate = 8.0f;

/// @brief 幹の折れ幅（板の半幅に対する比）
static const float kTrunkAmp = 0.55f;

/// @brief 枝の折れ幅
static const float kBranchAmp = 0.28f;

/// @brief 枝の長さ（板の半幅に対する比）
static const float kBranchLength = 0.7f;

/// @brief 白熱した芯の色（加算されるので 1 を超える値にはしない）
static const float3 kCoreColor = float3(0.92f, 0.97f, 1.0f);

/// @brief 簡易ハッシュ関数（EmitParticle.CS.hlsl と同じもの）
/// @param seed シード
/// @return 0〜1 の擬似乱数
float Hash(uint seed)
{
    seed = (seed ^ 61u) ^ (seed >> 16u);
    seed *= 9u;
    seed = seed ^ (seed >> 4u);
    seed *= 0x27d4eb2du;
    seed = seed ^ (seed >> 15u);
    return float(seed) / 4294967295.0f;
}

/// @brief Hash を -1〜1 に広げたもの
/// @param seed シード
/// @return -1〜1 の擬似乱数
float HashSigned(uint seed)
{
    return Hash(seed) * 2.0f - 1.0f;
}

/// @brief 折れ線の高さを返す
/// @param x     -1〜1 の横位置
/// @param seed  形状シード
/// @param segs  分割数
/// @param amp   折れ幅
/// @param slope 出力: その位置での傾き dy/dx（線への距離補正に使う）
/// @return 折れ線の高さ
/// @details 両端（x = -1, x = 1）の高さは 0 に固定。線が板の左右中央から出入りする形になり、
///          枝を描くときは「x = -1 の点が幹に付く」ことにも使う。
float BoltY(float x, uint seed, uint segs, float amp, out float slope)
{
    float t = saturate((x + 1.0f) * 0.5f) * (float)segs;
    uint i = min((uint)floor(t), segs - 1u);
    float f = t - (float)i;
    float y0 = (i == 0u) ? 0.0f : HashSigned(seed + i * 17u);
    float y1 = (i + 1u >= segs) ? 0.0f : HashSigned(seed + (i + 1u) * 17u);
    slope = (y1 - y0) * amp * (float)segs * 0.5f;
    return lerp(y0, y1, f) * amp;
}

/// @brief 幹の形（大きな折れ＋細かなギザギザ）
/// @param x     -1〜1 の横位置
/// @param seed  形状シード
/// @param amp   大きな折れの折れ幅（細かなギザギザはその 0.3 倍）
/// @param slope 出力: 2 段を合わせた傾き dy/dx
/// @return 幹の高さ
float BoltShapeY(float x, uint seed, float amp, out float slope)
{
    float slopeLow;
    float slopeHigh;
    float y = BoltY(x, seed, 5u, amp, slopeLow)
            + BoltY(x, seed + 977u, 13u, amp * 0.3f, slopeHigh);
    slope = slopeLow + slopeHigh;
    return y;
}

/// @brief 稲妻 1 本の輝度を芯とにじみに分けて返す
/// @param p          描く座標（-1〜1）
/// @param seed       形状シード
/// @param amp        折れ幅
/// @param widthScale 太さの倍率（枝は細くする）
/// @param env        包絡（両端フェードや先細りを呼び出し側で決める）
/// @param core       出力: 芯の輝度
/// @param glow       出力: にじみの輝度
void BoltIntensity(float2 p, uint seed, float amp, float widthScale, float env,
                   out float core, out float glow)
{
    float slope;
    float y = BoltShapeY(p.x, seed, amp, slope);

    // 芯: 傾いた線分への垂直距離 ≒ 縦方向の距離 / sqrt(1 + 傾き^2)
    //     これを入れないと急な折れの部分だけ線が太く見える
    // にじみ: 補正なしの縦方向距離を使う。補正係数は折れ点で不連続なので、
    //     広いにじみに使うと折れ点から放射状の筋が出てしまう
    float distV = abs(p.y - y);
    float distN = distV * rsqrt(1.0f + slope * slope);

    float coreWidth = kCoreWidth * widthScale;
    float glowWidth = kGlowWidth * widthScale;
    core = exp(-(distN * distN) / (coreWidth * coreWidth)) * env;
    glow = exp(-distV / glowWidth) * env;
}

/// @brief 板ポリゴンに手続き的に稲妻と発光の芯を描く
/// @param input GPUParticle.VS の出力（particleParams に寿命の進行度と固有乱数）
/// @return 出力色（ほぼ透明なピクセルは discard）
PixelShaderOutput main(VertexShaderOutput input)
{
    PixelShaderOutput output;

    const float age = input.particleParams.x;  // 0 = 生成直後, 1 = 消滅直前
    const float rnd = input.particleParams.y;  // パーティクル固有の乱数

    // --- 1. 座標系: UV を -1〜1 に変換し、板ごとにランダムに回転 ---
    float2 p = (input.texcoord - 0.5f) * 2.0f;
    float angle = rnd * 6.28318530f;
    float c = cos(angle);
    float s = sin(angle);
    p = float2(p.x * c - p.y * s, p.x * s + p.y * c);

    // 形状シード: 固有乱数 × 寿命の段階。段階が進むごとに別の形になる
    uint stage = (uint)floor(age * kShapeRate);
    uint seed = (uint)(rnd * 4096.0f) * 131u + stage * 7919u;

    // --- 2. 幹 ---
    // 板の左右端で線が切れて見えないよう、|x| が 0.55 を超えたらフェード
    float trunkEnv = 1.0f - smoothstep(0.55f, 1.0f, abs(p.x));
    float core;
    float glow;
    BoltIntensity(p, seed, kTrunkAmp, 1.0f, trunkEnv, core, glow);

    // --- 3. 枝 ---
    float branchCore = 0.0f;
    float branchGlow = 0.0f;
    if (Hash(seed + 5u) > 0.4f)
    {
        // 幹の上の点（根元）と、そこから出る向き
        float xb = HashSigned(seed + 6u) * 0.5f;
        float unusedSlope;
        float yb = BoltShapeY(xb, seed, kTrunkAmp, unusedSlope);
        float branchAngle = (0.6f + Hash(seed + 7u) * 0.8f)
                          * ((Hash(seed + 8u) > 0.5f) ? 1.0f : -1.0f);
        float2 dir = float2(cos(branchAngle), sin(branchAngle));

        // 枝の局所座標（x: 根元からの距離, y: 枝からのずれ）
        float2 q = p - float2(xb, yb);
        float2 lq = float2(dot(q, dir), dot(q, float2(-dir.y, dir.x)));
        if (lq.x > 0.0f && lq.x < kBranchLength)
        {
            float along = lq.x / kBranchLength;             // 0 = 根元, 1 = 先端
            float2 bp = float2(along * 2.0f - 1.0f, lq.y);  // BoltY は x = -1 で 0 → 根元が幹に付く
            float taper = 1.0f - along;                     // 先へ行くほど細く暗く
            BoltIntensity(bp, seed + 313u, kBranchAmp, 0.7f, taper, branchCore, branchGlow);
        }
    }

    // --- 4. 中心の発光ドット ---
    float dotGlow = exp(-dot(p, p) * 18.0f);

    // --- 5. 明滅 ---
    // 形が切り替わるたびに明るさも変わる。位相は固有乱数なので全体が同時に明滅しない
    float flicker = lerp(0.45f, 1.0f, Hash(seed + 21u));

    // --- 6. 合成 ---
    // input.color は UpdateElectric が startColor → endColor へ遷移させた色。
    // にじみは設定色、芯は白に寄せる（加算で重なると白熱する）
    // 板を回転させているので、内接円の外（四隅）に何も残らないよう半径でフェードして
    // 四角い縁が見えないようにする
    float edgeFade = 1.0f - smoothstep(0.75f, 1.0f, length(p));
    float coreSum = saturate(core + branchCore * 0.8f + dotGlow * 0.6f) * edgeFade;
    float glowSum = saturate(glow * 0.55f + branchGlow * 0.35f + dotGlow * 0.4f) * edgeFade;

    float3 rgb = input.color.rgb * glowSum + kCoreColor * coreSum;
    float alpha = input.color.a * saturate(glowSum + coreSum) * flicker;

    output.color = float4(rgb, saturate(alpha));

    if (output.color.a < 0.004f)
    {
        discard;
    }

    return output;
}
