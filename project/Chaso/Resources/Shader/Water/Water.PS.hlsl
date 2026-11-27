// =============================================================================
// Water.PS.hlsl - 海面・水面のピクセルシェーダー
//
// フレネル反射（環境マップ）、スクロールする法線マップ、深度ベースの色を基本に、
//   - 詳細波の解析法線（OceanWaves.hlsli）
//   - インタラクティブ波紋（WaveSimulation.CS）の法線・コースティクス・引き波 / 波頭の泡
//   - GGX スペキュラ、SSS（波頭を透ける光）、白波、平行光源の影
//   - 屈折（水を描く直前の画面のコピーをずらして引く）
//   - 波打ち際の泡と跳ね返り波
// を、波（法線）/ 水中から出てくる光 / 反射 / 泡 の成分ごとに別の関数で求め、
// 最後に足し合わせて最終色を出す（合成の式は下の「水面の成分ごとの計算」を参照）。頂点側は Water.VS.hlsl。
// =============================================================================

#include "Water.hlsli"

// b0: マテリアル（Object3D と同じレイアウト）
struct Material
{
    float4   color;                  ///< 水の色に乗算する色（RGBA）
    int      lightingMode;           ///< ライティングモード（水面では参照しない）
    float    shininess;              ///< 光沢度（水面では参照しない）
    float    environmentCoefficient; ///< 環境マップの映り込み係数（0〜1）
    float    padding;                ///< パディング
    float4x4 uvTransform;            ///< UV 変換行列（水面では参照しない）
};

ConstantBuffer<Material> gMaterial : register(b0);

// b1: ディレクショナルライト
struct DirectionalLight
{
    float4 color;           ///< 光の色（RGBA）
    float3 direction;       ///< 光の方向
    float  intensity;       ///< 光の強度
    float3 ambientColor;    ///< 環境光の色
    float ambientIntensity; ///< 環境光の強さ（0 で環境光なし）
};

ConstantBuffer<DirectionalLight> gDirectionalLight : register(b1);

// b2: カメラ
struct Camera
{
    float3 worldPosition; ///< カメラのワールド座標
};

ConstantBuffer<Camera> gCamera : register(b2);

// b6: 水面パラメータ（VS と共有）
cbuffer WaterParams : register(b6)
{
    float  gTime;          ///< 経過時間（秒）
    float  gWaveHeight;    ///< 主波の高さスケール
    float  gWaveSpeed;     ///< 主波の速度スケール
    float  gWaveFreq;      ///< 主波の周波数

    float  gWaveHeight2;   ///< 第 2 波の高さ
    float  gWaveSpeed2;    ///< 第 2 波の速度
    float  gWaveFreq2;     ///< 第 2 波の周波数
    float  gWaveSteepness; ///< Gerstner の鋭さ（0〜1）

    float4 gWaterShallowColor; ///< 浅瀬の色
    float4 gWaterDeepColor;    ///< 深海の色

    float  gFresnelPower;      ///< フレネル指数
    float  gSpecularPower;     ///< スペキュラ指数（GGX の粗さへ換算して使う）
    float  gNormalScrollSpeed; ///< 法線マップスクロール速度
    float  gNormalStrength;    ///< 法線マップ強度

    float4 gInvScreenSize; ///< xy: 画面サイズの逆数
    float4 gCameraNearFar; ///< x: near, y: far
    float4 gFoamParams;    ///< x: foamDepth, y: foamScale, z: 波の高さによる色付けの強さ（crestTint。0 で無効）
    float4 gFoamColor;     ///< 泡の色（rgb）と強さ（a）

    float4 gObstacles[4];  ///< 障害物（xyz: 位置, w: 半径）
    float4 gObstacleCount; ///< x: 障害物の数（PS では参照しない。y, z は VS の反射用）

    float4 gOceanParams;   ///< x: 詳細波の強さ, y: 尖り（choppy）, z: 白波の強さ, w: 頂点で変位させる最短波長（m）
    float4 gOceanParams2;  ///< x: 透明度（m）, y: 遠景フェード距離（m）, z: 法線マップのタイルサイズ（m。0 = 従来 UV）, w: 白波の量
    float4 gSssColor;      ///< rgb: 波頭を透ける光の色, a: 強さ
    float4 gRefractParams; ///< x: 屈折が有効（1/0）, y: 歪みの強さ（画面 UV）, z: 交差部のフェード幅（m）
    float4 gWaveWindow;    ///< 波紋ハイトマップの範囲。xy: 中心のワールド XZ, z: 一辺（m）
    float4 gInkColor;      ///< 水面の墨の色（rgb）と強さ（a。0 で描かない）
};

#include "OceanWaves.hlsli"

// t0: テクスチャ（法線マップとしても使用可能）
Texture2D<float4> gTexture : register(t0);

// t1: 環境キューブマップ（反射用）
TextureCube<float4> gEnvironmentTexture : register(t1);

// t4: インタラクティブ波紋ハイトマップ（r: 高さ, g: 引き波の泡, b: 波頭の白波, a: 墨）
Texture2D<float4> gInteractiveWave : register(t4);

// t5: 泡（フォーム）用の深度テクスチャ
Texture2D<float> gDepthTexture : register(t5);

// t6: 水を描く直前の画面のコピー（屈折用。gRefractParams.x が 1 のときだけ有効）
Texture2D<float4> gSceneColor : register(t6);

SamplerState gSampler : register(s0);
SamplerState gSamplerClamp : register(s2);

// =====================================================
// 平行光源の影（Object3d.hlsli の ShadowData と同じレイアウト・同じバッファ）
// =====================================================
// Water は b6 / t4 を WaterParams / 波紋に使っているので、影は b7 / t7 で受け取る。
struct ShadowData
{
    float4x4 lightViewProjection; ///< ライトのビュープロジェクション行列
    float3   lightDirection;      ///< ライトの方向
    float    bias;                ///< 深度バイアス（斜面ほど大きくかける基準値）
    float4   color;               ///< rgb: 影の色, a: 濃さ
    int      shadowMapEnabled;    ///< シャドウマップが有効か（0 で無効）
    float2   shadowMapTexelSize;  ///< 1 テクセルの UV サイズ
    float    pcfRadius;           ///< PCF のタップ間隔（テクセル単位。0 以下で 1 タップ）
};
ConstantBuffer<ShadowData> gShadowParams : register(b7);
Texture2D<float> gShadowMap : register(t7);
SamplerComparisonState gShadowCmpSampler : register(s3);

// 水面の影の効き方
static const float kWaterShadowPcfScale = 1.5; ///< 水面は波で縁が揺れるので、物体より少し柔らかくする
static const float kWaterSunBlock       = 2.0; ///< 影の濃さ（a）に対する直射光（ギラつき・SSS）の遮り方。a = 0.5 で完全に消える

/// @brief 水面上の点が太陽から見えている率を返す
/// @param worldPos 水面のワールド位置
/// @param N        水面の法線（バイアスの調整に使う）
/// @return 1: 日向, 0: 完全に影（影が無効・シャドウマップの範囲外では 1）
float SampleWaterShadow(float3 worldPos, float3 N)
{
    if (gShadowParams.shadowMapEnabled == 0) {
        return 1.0;
    }

    float4 lp = mul(float4(worldPos, 1.0), gShadowParams.lightViewProjection);
    float3 proj = lp.xyz / lp.w;
    float2 uv = float2(proj.x * 0.5 + 0.5, -proj.y * 0.5 + 0.5);
    if (any(uv < 0.0) || any(uv > 1.0) || proj.z < 0.0 || proj.z > 1.0) {
        return 1.0;
    }

    // 水面自体はシャドウマップに描かれない（自己影が起きない）ので、バイアスは
    // 喫水線で船体と水面が接する所の取りこぼし対策ぶんだけでよい
    float NdotL = saturate(dot(N, -gShadowParams.lightDirection));
    float compareDepth = proj.z - max(0.001, gShadowParams.bias * (1.0 - NdotL));

    float lit;
    if (gShadowParams.pcfRadius <= 0.0) {
        lit = gShadowMap.SampleCmpLevelZero(gShadowCmpSampler, uv, compareDepth);
    } else {
        float2 stepUV = gShadowParams.shadowMapTexelSize * gShadowParams.pcfRadius * kWaterShadowPcfScale;
        float sum = 0.0;
        [unroll]
        for (int y = -1; y <= 1; ++y) {
            [unroll]
            for (int x = -1; x <= 1; ++x) {
                sum += gShadowMap.SampleCmpLevelZero(gShadowCmpSampler, uv + float2(x, y) * stepUV, compareDepth);
            }
        }
        lit = sum / 9.0;
    }

    // シャドウマップの外周で影が唐突に切れないようにフェード（Object3d と同じ）
    float2 fadeUV = abs(uv * 2.0 - 1.0);
    float edgeFade = saturate((1.0 - max(fadeUV.x, fadeUV.y)) / 0.05);
    return lerp(1.0, lit, edgeFade);
}

struct PixelShaderOutput
{
    float4 color : SV_TARGET0; ///< 出力色（a は水の不透明度）
};

// =====================================================
// マウス波紋（インタラクティブ波）のライティング
// =====================================================
// 波紋の高さは数 cm、傾きは多くの所で 2〜3°しかない。真上視点・ほぼ真上の太陽だと
// N・L も フレネル もほとんど変わらず、形は動くのに光り方が変わらない。
// そこで「光の計算に使う傾き」だけを強め、次の 2 つで光の変化として見せる。
//   ・太陽側の斜面を明るく、反対側を暗く（ライトの水平方向で決まるので、ライトを回せば陰影も回る）
//   ・水底のコースティクス（波紋の山がレンズになって光を集め、谷で散らす）
static const float kRippleNormalGain = 4.0;  ///< 小さな波紋の傾きを法線へ反映する倍率（頂点の変位は変えない）
static const float kRippleMaxSlope   = 0.35; ///< 倍率をかけた傾きの頭打ち（tan ≈ 19°）。急な斜面は本来の傾きのまま
static const float kRippleSunShade   = 0.9;  ///< 太陽側 / 反対側の斜面の明暗の強さ（0 で無効）
static const float kRippleShadeMin   = 0.7;  ///< 斜面の陰影で暗くなる下限（真上の太陽では実際 2 割程度しか暗くならない）
static const float kRippleShadeMax   = 1.3;  ///< 斜面の陰影で明るくなる上限
static const float kCausticFocus     = 0.75; ///< 集光の強さ。物理値は 1-1/1.33 ≈ 0.25（波紋が低いので誇張）
static const float kCausticMax       = 2.5;  ///< 集光で明るくなる上限
static const float kCausticMin       = 0.6;  ///< 光が散って暗くなる下限（溝の真下が黒く抜けないように）

/// @brief 屈折の減衰に「視線の中の水の厚み」をどれだけ混ぜるか（0: 深さだけ, 1: 厚みだけ＝従来）
/// @details 少しだけ混ぜて、遠くを浅い角度で見たときに浅い所も徐々に霞むようにしている。
///          DepthBasedOutline.PS.hlsl の kOutlineWaterPathWeight と同じ値にすること（線の消え方を揃える）。
static const float kWaterPathWeight = 0.1;

/// @brief 波紋の傾き s（= -∇h）に倍率をかけ、大きくなるほど頭打ちにする
/// @details 小さな傾きはほぼ s * kRippleNormalGain（細かい波紋でも光が動いて見える）。
///          大きな傾きは kRippleMaxSlope に近づく。ただし本来の傾きより小さくはしない。
/// @param s 波紋の傾き
/// @return 光の計算に使う傾き
float2 ShapeRippleSlope(float2 s)
{
    float m = length(s);
    if (m < 1e-6) {
        return s;
    }
    float g = m * kRippleNormalGain;
    float limited = g / sqrt(1.0 + (g / kRippleMaxSlope) * (g / kRippleMaxSlope));
    return s * (max(limited, m) / m);
}

// =====================================================
// ノーマルマップから法線を取得（スクロール合成）
// =====================================================
/// @brief 2 層スクロールした法線マップを geometryNormal の周りに合成した法線を返す
/// @param uv             法線マップの基準 UV
/// @param time           経過時間（秒）
/// @param geometryNormal 合成のベースになる法線（ワールド）
/// @param strength       法線マップの強さ
/// @return 合成後の法線（ワールド、正規化済み）
float3 GetScrolledNormal(float2 uv, float time, float3 geometryNormal, float strength)
{
    float scrollSpeed = gNormalScrollSpeed;

    // 2 層の UV スクロール（異なる速度・方向で重ね合わせ）
    float2 uv1 = uv * 4.0 + float2(scrollSpeed * time * 0.6, scrollSpeed * time * 0.4);
    float2 uv2 = uv * 6.0 + float2(-scrollSpeed * time * 0.3, scrollSpeed * time * 0.7);

    // テクスチャから法線マップを取得（0〜1 → -1〜1）
    float3 n1 = gTexture.Sample(gSampler, uv1).xyz * 2.0 - 1.0;
    float3 n2 = gTexture.Sample(gSampler, uv2).xyz * 2.0 - 1.0;

    // 2 層の法線をブレンド
    float3 blended = normalize(n1 + n2);

    // 強度を調整
    blended.xy *= strength;
    blended = normalize(blended);

    // 法線空間 → ワールド空間のマッピング（簡易版）
    // geometryNormal を up として、TBN を構築
    float3 N = normalize(geometryNormal);
    float3 T = normalize(cross(N, float3(0, 0, 1)));
    if (length(T) < 0.001)
        T = normalize(cross(N, float3(1, 0, 0)));
    float3 B = cross(N, T);

    return normalize(T * blended.x + B * blended.y + N * blended.z);
}

// =====================================================
// 詳細波の傾きと水平変位の偏微分（ピクセルごとに解析的に計算）
// =====================================================
// 頂点では表せない短い波も、ここでは傾き（法線）として描ける。
// footprint は 1 ピクセルが覆うワールド幅。波長がそれに近い波は
// チラつき（エイリアス）の元なので弱める。
struct DetailSurface
{
    float2 slope;    ///< 高さの傾き（dH/dx, dH/dz）
    float3 jacobian; ///< 水平変位の偏微分（dDx/dx, dDz/dz, dDx/dz）
};

/// @brief 詳細波の傾きと水平変位の偏微分を求める
/// @param xz        変位前のワールド XZ
/// @param time      経過時間（秒）
/// @param footprint 1 ピクセルが覆うワールド幅（エイリアス対策の減衰に使う）
/// @return 詳細波の傾きと水平変位の偏微分
DetailSurface EvaluateDetailSurface(float2 xz, float time, float footprint)
{
    DetailSurface o;
    o.slope = float2(0, 0);
    o.jacobian = float3(0, 0, 0);

    float detail = gOceanParams.x;
    if (detail <= 0.0) {
        return o;
    }

    [unroll]
    for (int i = 0; i < kDetailWaveCount; ++i)
    {
        DetailWave w = GetDetailWave(i, gWaveFreq, gWaveSpeed, gWaveHeight, detail, gOceanParams.y);
        float lambda = kTwoPi / w.k;
        float aa = smoothstep(footprint * 2.0, footprint * 6.0, lambda);
        float ph = DetailPhase(w, xz, time);
        float s = sin(ph);
        float c = cos(ph);

        o.slope += w.dir * (w.amp * w.k * c * aa);

        float wa = w.qa * w.k * s * aa;
        o.jacobian.x -= wa * w.dir.x * w.dir.x;
        o.jacobian.y -= wa * w.dir.y * w.dir.y;
        o.jacobian.z -= wa * w.dir.x * w.dir.y;
    }
    return o;
}

// =====================================================
// GGX スペキュラ（太陽のギラつき）
// =====================================================
/// @brief GGX のスペキュラ（NdotL を掛けた後の値）を求める
/// @param N     法線
/// @param V     視線方向（表面 → カメラ）
/// @param L     光の方向（表面 → 光源）
/// @param alpha 粗さ
/// @param F0    垂直入射時の反射率
/// @return スペキュラの強さ
float SpecularGGX(float3 N, float3 V, float3 L, float alpha, float F0)
{
    float3 H = normalize(L + V);
    float NdotL = saturate(dot(N, L));
    float NdotV = max(dot(N, V), 1e-3);
    float NdotH = saturate(dot(N, H));
    float VdotH = saturate(dot(V, H));

    float a2 = alpha * alpha;
    float d  = NdotH * NdotH * (a2 - 1.0) + 1.0;
    float D  = a2 / (3.14159265 * d * d);

    float k   = alpha * 0.5;
    float vis = 0.25 / ((NdotL * (1.0 - k) + k) * (NdotV * (1.0 - k) + k));

    float F = F0 + (1.0 - F0) * pow(1.0 - VdotH, 5.0);

    return D * vis * F * NdotL;
}

// =====================================================
// 深度値をリニアに変換（近・遠クリップ平面を使用）
// =====================================================
/// @brief 深度バッファの値をビュー空間の距離に戻す
/// @param depth 深度バッファの値（0〜1）
/// @param nearZ ニアクリップ距離
/// @param farZ  ファークリップ距離
/// @return リニアな深度（ビュー空間の Z）
float LinearizeDepth(float depth, float nearZ, float farZ)
{
    // D3D12（Z: 0〜1、Reversed-Z ではない標準プロジェクションを想定）
    // プロジェクションの設定によっては Reversed-Z を考慮する必要がある。
    // ここでは標準の投影として扱う。
    return (nearZ * farZ) / (farZ - depth * (farZ - nearZ));
}

/// @brief 水の垂直入射時の反射率（屈折率 1.33 から）
static const float kWaterF0 = 0.02;

// =============================================================================
// 水面の成分ごとの計算
// =============================================================================
// 最終色は次の 4 段で作る。各段は互いの結果に手を入れず、必要な値は引数で受け取る。
//
//   1. 波     EvaluateWaveSurface  : 法線（形）だけを決める。色は持たない。
//                                    主波・詳細波・波紋・跳ね返り波は「高さ場の傾き ∇h」として足す
//   2. 水中光 EvaluateWaterBody    : 水の中から出てくる光（水の色・屈折・コースティクス・墨・SSS）
//   3. 反射   EvaluateReflection   : 水面で跳ね返る光（空の映り込み・太陽のギラつき）
//   4. 泡     EvaluateFoam         : 水面に浮いて上を覆う白（白波・引き波・波頭・波打ち際）
//
// 合成は加算：
//   最終色 = 水中光 × (1 - 反射率) × 泡の透過率
//          + 反射   ×              泡の透過率
//          + 泡（不透明度を掛け済みの色）
// 各項に重みが掛かっているのはエネルギーを保つため（重み無しで足すと反射や泡の所が白飛びする）。
// 以前の「lerp を順に重ねる」書き方を展開したものと同じ値になる。

/// @brief 画面・深度から求める、この画素の共通情報
struct WaterPixel
{
    float3 V;          ///< 表面 → カメラ（正規化済み）
    float2 screenUV;   ///< 画面 UV
    float  sceneDepth; ///< 水の奥にある不透明物までのリニア深度
    float  pixelDepth; ///< 水面のリニア深度
    float  depthDiff;  ///< 水面 → 奥の不透明物までの距離（岸からの距離の目安）
    float  distT;      ///< 遠景フェード（0: 近景, 1: 遠景）
};

/// @brief 波打ち際の跳ね返り波（波の傾きと泡の両方が同じ位相を使う）
struct ShoreWave
{
    float phase;     ///< 跳ね返り波の位相
    float fade;      ///< 0: 岸際 〜 1: foamDepth 以上離れた
    float intensity; ///< 岸に近いほど 1 に近い強さ
};

/// @brief 波の計算結果（形だけ。色は持たない）
struct WaveSurface
{
    float3 geoNormal;       ///< うねりの法線（主波＋詳細波＋波紋＋跳ね返り波。法線マップは含まない）
    float3 N;               ///< 最終の法線（法線マップ・凪のムラ込み）
    float2 rippleSlope;     ///< 波紋の傾き（光の計算用に強めたもの。-∇h）
    float  rippleLaplacian; ///< 波紋の曲率（コースティクス用）
    float3 detailJacobian;  ///< 詳細波の水平変位の偏微分（白波用）
    float  roughMask;       ///< さざ波のムラ（1: ざわつく所, 小: 凪）
    float  wakeFoam;        ///< 引き波の泡（波紋テクスチャ g）
    float  crestFoam;       ///< 波頭の泡（波紋テクスチャ b）
    float  inkAmount;       ///< 墨の濃さ（波紋テクスチャ a）
};

/// @brief 各成分が共通で使う光の情報
struct WaterLighting
{
    float3 L;          ///< 表面 → 光源
    float3 lightColor; ///< 光の色 × 強さ
    float  NdotL;      ///< N・L
    float  diffuse;    ///< ハーフランバートの 2 乗
    float  NdotV;      ///< N・V
    float  fresnel;    ///< フレネル反射率
    float3 shadowTint; ///< 日光で照らされる成分に掛ける影の色
    float  sunVisible; ///< 直射光（ギラつき・SSS）が届く率
};

/// @brief 水中から出てくる光
struct WaterBody
{
    float3 color;   ///< 水の色（拡散・屈折・墨込み）。反射率ぶん減らして足す
    float3 scatter; ///< 波頭を透ける光（SSS）。(1 - フレネル) を掛けて足す
    float  alpha;   ///< 水の不透明度（泡・墨を除く）
    float  crestT;  ///< 0: 谷 〜 1: 山（crestTint が有効なときだけ）
    float  inkT;    ///< 墨の覆い具合（泡を薄くするのと α に使う）
};

/// @brief 水面で跳ね返る光
struct WaterReflection
{
    float3 env;      ///< 空の映り込みの色
    float  weight;   ///< 映り込みの割合（フレネル × 環境マップ係数）
    float3 specular; ///< 太陽のギラつき（そのまま足す）
};

/// @brief 泡（上に被さる層）。color は不透明度を掛け済み、transmit は下の層が透けて見える率
struct FoamLayer
{
    float3 color;     ///< 泡の色（不透明度を掛け済み）
    float  transmit;  ///< 下（水中光・反射）が見える率
    float  alphaMask; ///< 水の α を不透明へ寄せる量
};

/// @brief 泡の層に 1 枚重ねる（lerp(下, c, a) と同じ結果になる形で積む）
/// @param f 泡の層
/// @param c 重ねる泡の色
/// @param a 重ねる泡の不透明度
void FoamOver(inout FoamLayer f, float3 c, float a)
{
    f.color = f.color * (1.0 - a) + c * a;
    f.transmit *= 1.0 - a;
}

// -----------------------------------------------------------------------------
// 0. 画素の共通情報
// -----------------------------------------------------------------------------

/// @brief 画面・深度の情報を集める（奥に物が無く水面が手前に出ない画素は discard）
/// @param input VS からの補間値
/// @return 画素の共通情報
WaterPixel GetWaterPixel(VertexShaderOutput input)
{
    WaterPixel px;
    px.V = normalize(gCamera.worldPosition - input.worldPosition);
    px.screenUV = input.position.xy * gInvScreenSize.xy;
    float sceneDepthNonLinear = gDepthTexture.SampleLevel(gSamplerClamp, px.screenUV, 0).r;
    px.sceneDepth = LinearizeDepth(sceneDepthNonLinear, gCameraNearFar.x, gCameraNearFar.y);
    px.pixelDepth = LinearizeDepth(input.position.z, gCameraNearFar.x, gCameraNearFar.y);
    px.depthDiff = px.sceneDepth - px.pixelDepth;

    if (px.depthDiff < 0.0f) {
        discard;
    }

    float camDist = distance(gCamera.worldPosition, input.worldPosition);
    px.distT = (gOceanParams2.y > 0.0) ? saturate(camDist / gOceanParams2.y) : 0.0;
    return px;
}

/// @brief 岸からの跳ね返り波の位相と強さ
/// @param depthDiff 水面 → 奥の不透明物までの距離
/// @return 跳ね返り波の情報
ShoreWave GetShoreWave(float depthDiff)
{
    // depthDiff（水深）を使って等深線に沿った波紋を作る。
    // gTime を足すことで、浅いところ（0）から深いところ（> 0）へ向かって波紋が動く（跳ね返るように見える）。
    const float returnWaveFreq = 4.0f;
    const float returnWaveSpeed = 3.5f;

    ShoreWave s;
    s.phase = depthDiff * returnWaveFreq + gTime * returnWaveSpeed;
    float foamDepthThreshold = max(gFoamParams.x, 0.001f);
    s.fade = saturate(depthDiff / foamDepthThreshold);
    s.intensity = pow(1.0f - s.fade, 1.5f);
    return s;
}

// -----------------------------------------------------------------------------
// 1. 波（法線）
// -----------------------------------------------------------------------------
// 高さ場 y = h(x,z) の法線は (-hx, 1, -hz)。波源ごとに ∇h を求めて足すと、
// 高さを足し合わせた水面の法線になる（波源を足し引きしても互いに影響しない）。

/// @brief 主波（VS の Gerstner ＋ 障害物の反射波）の ∇h
/// @param vertexNormal VS が求めた法線
/// @return ∇h
float2 MainWaveGradient(float3 vertexNormal)
{
    float3 n = normalize(vertexNormal);
    float ny = max(n.y, 0.2); // 折り返し寸前で傾きが発散しないように
    return n.xz / -ny;
}

/// @brief 岸からの跳ね返り波の ∇h
/// @param input VS からの補間値（ワールド位置の画面微分に使う）
/// @param depthDiff 水面 → 奥の不透明物までの距離
/// @param shore 跳ね返り波の情報
/// @return ∇h
float2 ShoreBounceGradient(VertexShaderOutput input, float depthDiff, ShoreWave shore)
{
    // ddx, ddy を用いてスクリーン空間での水深の変化量とワールド座標の変化量を取得し、
    // 水深が深くなる方向（岩から離れる方向＝沖）を求める
    float dDepthX = ddx(depthDiff);
    float dDepthY = ddy(depthDiff);
    float3 dPosW_X = ddx(input.worldPosition);
    float3 dPosW_Y = ddy(input.worldPosition);
    float3 depthGradient = normalize(dDepthX * dPosW_X + dDepthY * dPosW_Y + float3(0, 0.0001f, 0));
    depthGradient.y = 0.0f; // 水平方向のみにする
    depthGradient = normalize(depthGradient + float3(0, 0.0001f, 0));

    // cos 波でうねりを表し、岸辺に近いほど強くする。法線を沖方向へ傾ける = 沖へ下る斜面
    float returnSlope = cos(shore.phase) * 0.5f * shore.intensity;
    return -depthGradient.xz * returnSlope;
}

/// @brief 波の計算（法線・波紋・泡や墨の入力）
/// @param input VS からの補間値
/// @param px 画素の共通情報
/// @param shore 跳ね返り波の情報
/// @return 波の計算結果
WaveSurface EvaluateWaveSurface(VertexShaderOutput input, WaterPixel px, ShoreWave shore)
{
    WaveSurface w;

    // ---- 波紋ハイトマップ（ハイトマップはカメラに合わせて動く窓 gWaveWindow を覆う）----
    float2 waveUV = WaveWindowUV(input.worldPosition.xz, gWaveWindow);
    float texelSize = 1.0f / 256.0f;
    float hL = gInteractiveWave.Sample(gSamplerClamp, waveUV + float2(-texelSize, 0)).r;
    float hR = gInteractiveWave.Sample(gSamplerClamp, waveUV + float2(texelSize, 0)).r;
    float hD = gInteractiveWave.Sample(gSamplerClamp, waveUV + float2(0, -texelSize)).r;
    float hU = gInteractiveWave.Sample(gSamplerClamp, waveUV + float2(0, texelSize)).r;
    // r: 中心の高さ / g: 引き波 / b: 波頭 / a: 墨。
    // 範囲外（UV が 0〜1 の外）はクランプで端の値になるので 0 に落とし、窓の縁ではぼかして切れ目を消す
    float4 waveCenter = gInteractiveWave.Sample(gSamplerClamp, waveUV);
    float inWaveArea = WaveWindowEdgeFade(waveUV);
    w.wakeFoam  = waveCenter.g * inWaveArea;
    w.crestFoam = waveCenter.b * inWaveArea;
    w.inkAmount = waveCenter.a * inWaveArea;

    // 波紋の傾き（光の計算用に強める。ShapeRippleSlope 参照）と、集光に使う曲率（ラプラシアン）
    float texelWorld = gWaveWindow.z * texelSize;
    w.rippleSlope = ShapeRippleSlope(float2(hL - hR, hD - hU) / (2.0f * texelWorld) * inWaveArea);
    w.rippleLaplacian = (hL + hR + hU + hD - 4.0f * waveCenter.r) / (texelWorld * texelWorld) * inWaveArea;

    // ---- 詳細波（ピクセルごとの解析法線）----
    float2 fw = fwidth(input.restXZ);
    float footprint = max(max(fw.x, fw.y), 1e-4);
    DetailSurface detail = EvaluateDetailSurface(input.restXZ, gTime, footprint);
    w.detailJacobian = detail.jacobian;

    // ---- 波源ごとの ∇h を足す ----
    float2 gradMain   = MainWaveGradient(input.normal);
    float2 gradDetail = detail.slope;
    float2 gradRipple = -w.rippleSlope; // rippleSlope は -∇h
    float2 gradBounce = ShoreBounceGradient(input, px.depthDiff, shore);
    float2 grad = gradMain + gradDetail + gradRipple + gradBounce;
    w.geoNormal = normalize(float3(-grad.x, 1.0, -grad.y));

    // ---- 法線マップ（細かいさざ波）----
    // gOceanParams2.z > 0 ならワールド座標で敷き詰める（平面の大きさに関係なく同じ細かさになる）
    float2 normalUV = (gOceanParams2.z > 0.0)
        ? input.worldPosition.xz / (gOceanParams2.z * 4.0)
        : input.texcoord;
    // 遠景では細かい法線を弱める（タイルの繰り返しとギラギラしたチラつきを抑える）
    float normalStrength = gNormalStrength * lerp(1.0, 0.35, px.distT);
    w.N = GetScrolledNormal(normalUV, gTime, w.geoNormal, normalStrength);

    // ---- さざ波のムラ（真上視点のシーンだけ。crestTint > 0 のとき）----
    // 法線マップは uv*4 / uv*6 で敷き詰めているので、真上から見ると細かい白い照り返しが
    // 画面全体に同じ密度で並び、模様が均一すぎて不自然に見える。
    // 同じ法線マップをずっと粗いスケールで引いて「風が当たってざわつく所 / 凪いで鏡のような所」の
    // マスクを作り、凪の所では細かい法線を弱める。照り返し・映り込みもそれに合わせて疎密が出る。
    w.roughMask = 1.0;
    if (gFoamParams.z > 0.0)
    {
        float2 m1 = input.texcoord * 1.3 + float2(gTime * 0.004, gTime * 0.003);
        float2 m2 = input.texcoord * 2.1 + float2(-gTime * 0.003, gTime * 0.005);
        float n = gTexture.Sample(gSampler, m1).x * 0.6 + gTexture.Sample(gSampler, m2).y * 0.4;
        w.roughMask = lerp(0.12, 1.0, smoothstep(0.42, 0.62, n));
        w.N = normalize(lerp(w.geoNormal, w.N, w.roughMask));
    }
    return w;
}

// -----------------------------------------------------------------------------
// 光の共通情報
// -----------------------------------------------------------------------------

/// @brief 各成分が共通で使う光・影・フレネルを求める
/// @param input VS からの補間値
/// @param px 画素の共通情報
/// @param N 最終の法線
/// @return 光の情報
WaterLighting EvaluateLighting(VertexShaderOutput input, WaterPixel px, float3 N)
{
    WaterLighting lit;
    lit.L = normalize(-gDirectionalLight.direction);
    lit.lightColor = gDirectionalLight.color.rgb * gDirectionalLight.intensity;
    lit.NdotL = saturate(dot(N, lit.L));

    // ハーフランバート（さらに 2 乗して暗部を締める）
    lit.diffuse = saturate(lit.NdotL * 0.5 + 0.5);
    lit.diffuse *= lit.diffuse;

    // Schlick 近似: F = F0 + (1 - F0) * (1 - cosTheta)^n
    // （指数 5 の代わりに gFresnelPower を使い、見た目で調整できるようにしている）
    lit.NdotV = saturate(dot(N, px.V));
    lit.fresnel = kWaterF0 + (1.0 - kWaterF0) * pow(1.0 - lit.NdotV, gFresnelPower);

    // 影の中の水面は「直射日光が届かない」だけで、空の映り込み（環境マップ）はそのまま残る。
    //   shadowTint : 日光で照らされる成分（水の色・泡）を Object3d と同じ色・濃さで暗くする
    //   sunVisible : 太陽のギラつきと波頭を透ける光（SSS）。影の中ではほぼ消える
    float shadowAmt = 1.0 - SampleWaterShadow(input.worldPosition, N);
    lit.shadowTint = lerp(float3(1.0, 1.0, 1.0), gShadowParams.color.rgb, shadowAmt * gShadowParams.color.a);
    lit.sunVisible = 1.0 - shadowAmt * saturate(gShadowParams.color.a * kWaterSunBlock);
    return lit;
}

// -----------------------------------------------------------------------------
// 2. 水中から出てくる光
// -----------------------------------------------------------------------------

/// @brief 水の色・屈折・コースティクス・墨・SSS を求める
/// @param input VS からの補間値
/// @param px 画素の共通情報
/// @param w 波の計算結果
/// @param lit 光の情報
/// @return 水中から出てくる光
WaterBody EvaluateWaterBody(VertexShaderOutput input, WaterPixel px, WaveSurface w, WaterLighting lit)
{
    WaterBody body;
    body.crestT = 0.0;
    body.inkT = 0.0;
    body.scatter = float3(0, 0, 0);

    float3 N = w.N;
    float3 V = px.V;
    float depthDiff = px.depthDiff;

    // =========================
    // 水の色（深度ベース）
    // =========================
    // 視線と法線の角度（1 - NdotV）を深さの指標として使い、
    // 正面が浅瀬色、掠める角度が深海色にブレンド
    float4 waterColor = lerp(gWaterShallowColor, gWaterDeepColor, 1.0 - lit.NdotV);
    waterColor *= gMaterial.color; // マテリアル色を乗算

    // 透明度（gOceanParams2.x, m）：水面の奥にある物体が近いほど浅瀬色に寄せ、透かす。
    // 光は水中で exp(-距離/透明度) で減衰する（Beer-Lambert）。奥に何も無い外洋では depthDiff が
    // 遠クリップ近くまで伸びるので効果は 0 になり、従来どおりの色になる。
    const bool refractionOn = (gRefractParams.x > 0.5);
    if (gOceanParams2.x > 0.0 && !refractionOn) // 屈折ありのときは下の屈折の合成で同じことをする
    {
        float shallowness = exp(-max(depthDiff, 0.0) / gOceanParams2.x);
        waterColor.rgb = lerp(waterColor.rgb, gWaterShallowColor.rgb * gMaterial.color.rgb, shallowness * 0.6);
        waterColor.a   = lerp(waterColor.a, waterColor.a * 0.4, shallowness);
    }

    // =========================
    // 波の高さによる色付け（gFoamParams.z = crestTint、0 で無効）
    // =========================
    // 上の lerp は視線と法線の角度だけで色を決めるため、真上から見下ろすと
    // NdotV がほぼ 1 で固定され、うねりが色にまったく出ない（一枚のベタ塗りになる）。
    // 真上視点のシーン（タイトル）では頂点の変位で谷を深海色へ、山を明るく振る。
    float crestTint = gFoamParams.z;
    if (crestTint > 0.0)
    {
        float amplitude = max(gWaveHeight + gWaveHeight2 + gWaveHeight * 0.25
                              + DetailAmplitudeSum(gWaveFreq, gWaveHeight, gOceanParams.x), 0.001);
        body.crestT = smoothstep(0.0, 1.0, saturate(input.waveHeight / amplitude * 0.5 + 0.5)); // 0: 谷, 1: 山
        float3 troughColor = lerp(waterColor.rgb, gWaterDeepColor.rgb, crestTint * 0.85);
        float3 crestColor  = waterColor.rgb * (1.0 + crestTint * 0.35);
        waterColor.rgb = lerp(troughColor, crestColor, body.crestT);

        // 浅瀬（真上視点のシーンだけ）：水底が近いほど明るく・透き通らせる。
        // depthDiff は「水面 → その画素の奥にある不透明物」までの距離（真上視点ならほぼ水深）。
        // 水底が無い所は depthDiff が遠クリップ近くまで伸びるので shallowT = 0 になり、従来どおりの色。
        const float kShallowVisibility = 16.0; // この深さ（m）より深いと浅瀬の効果が消える
        float shallowT = 1.0 - saturate(depthDiff / kShallowVisibility);
        shallowT = shallowT * shallowT * (3.0 - 2.0 * shallowT);
        waterColor.rgb = lerp(waterColor.rgb, gWaterShallowColor.rgb * 1.25, shallowT * 0.5);
        waterColor.a = lerp(waterColor.a, waterColor.a * 0.45, shallowT);
    }

    // =========================
    // 拡散（波紋の陰影込み）
    // =========================
    // 波紋の陰影：法線の水平成分がライトの水平方向を向いている斜面ほど明るい。
    // 真上からの光では N・L の変化は傾きの 2 乗でしか効かないため、1 次の項として別に足す。
    float3 interactiveNormal = normalize(float3(w.rippleSlope.x, 1.0f, w.rippleSlope.y));
    float2 sunHorizontal = lit.L.xz;
    float  sunHorizontalLen = length(sunHorizontal);
    float2 sunDirXZ = (sunHorizontalLen > 1e-4) ? sunHorizontal / sunHorizontalLen : float2(0, 0);
    float  rippleShade = clamp(1.0 + dot(interactiveNormal.xz, sunDirXZ) * kRippleSunShade,
                               kRippleShadeMin, kRippleShadeMax);

    float3 color = waterColor.rgb * lit.lightColor * lit.diffuse * rippleShade * lit.shadowTint;

    // =========================
    // 屈折（スクリーンテクスチャ方式）
    // =========================
    // 水を描く直前の画面を、水面の法線（さざ波・うねり）でずらして引き、水中の物体をゆらがせる。
    // 光は水中を進むほど吸収・散乱されるので、
    //   透過光 = 画面の色 × 浅瀬色の色味^(厚み/透明度) × exp(-厚み/透明度)
    //   散乱光 = 水の色（深海色寄り） × (1 - exp(-厚み/透明度))
    // を足す。浅い所は水底が透け、深い所は水の色になる。
    if (refractionOn)
    {
        float2 refrOffset = N.xz * gRefractParams.y;
        refrOffset *= saturate(depthDiff);                    // 交差部はずらさない（物体の縁がちぎれないように）
        refrOffset /= max(1.0, px.pixelDepth * 0.05);         // 遠くほど画面上のずれを小さく
        float2 refrUV = saturate(px.screenUV + refrOffset);

        // ずらした先が水面より手前の物体なら、それを水中に映り込ませないよう元の位置へ戻す
        float refrDepth = LinearizeDepth(gDepthTexture.SampleLevel(gSamplerClamp, refrUV, 0).r,
                                         gCameraNearFar.x, gCameraNearFar.y);
        if (refrDepth < px.pixelDepth) {
            refrUV = px.screenUV;
            refrDepth = px.sceneDepth;
        }
        float thickness = max(refrDepth - px.pixelDepth, 0.0);
        float3 sceneCol = gSceneColor.SampleLevel(gSamplerClamp, refrUV, 0).rgb;

        // 水底のコースティクス：水面を高さ場 h とみると、深さ d の水底に届く光の密度は
        //   1 / (1 + (1 - 1/η) * d * ∇²h)
        // 山（∇²h < 0）の下で集まって明るく、谷（∇²h > 0）の下で散って暗くなる。
        float causticJ = 1.0 + kCausticFocus * thickness * w.rippleLaplacian;
        float caustic  = clamp(1.0 / max(causticJ, 1e-3), kCausticMin, kCausticMax);
        sceneCol *= caustic;

        float clarity = (gOceanParams2.x > 0.0) ? gOceanParams2.x : 3.0;
        // 減衰は「水面からの深さ」を主に使う。視線の中の水の厚みだけで決めると、
        // 浅い角度で見たときに水面すぐ下のサメやタコまで消えて、輪郭線しか残らなかった。
        // 深さ主体にすると、浅い所の物は顔まで見え、深い床や海底は従来どおり水の色に沈む。
        float depthBelow = thickness * saturate(abs(V.y));
        float d = lerp(depthBelow, thickness, kWaterPathWeight) / clarity;
        float T = exp(-d);
        float3 tint = gWaterShallowColor.rgb
                    / max(max(gWaterShallowColor.r, gWaterShallowColor.g), max(gWaterShallowColor.b, 1e-3));
        float3 transmit = pow(max(tint, 1e-3), min(d, 16.0));

        color = sceneCol * transmit * T + color * (1.0 - T);
    }

    // =========================
    // 墨（波紋テクスチャの A チャンネル）
    // =========================
    // 水面に落ちた墨は表面に浮いて広がるので、水の色（拡散・屈折で透けた水中）だけを変え、
    // 反射（空の映り込み・スペキュラ）は残す（墨の膜も光を照り返す）。
    //
    // 墨流しのように
    //   - 芯 : 濃い所。青みのある黒（墨色）でほぼ塗りつぶす
    //   - 筋 : 薄く滲んだ所。水の色を暗くするだけ（灰色の濃淡）。ドメインワープで渦を巻いた筋にする
    // の 2 段に分ける。ノイズは法線マップの成分を借用し（泡の lace と同じ）、ゆっくり流して渦を動かす。
    if (w.inkAmount > 0.001 && gInkColor.a > 0.0)
    {
        float2 ip = input.worldPosition.xz;
        // ドメインワープ：座標を低周波ノイズでずらしてから細かいノイズを引くと、筋が渦を巻いて流れる
        float2 warp = float2(gTexture.Sample(gSampler, ip * 0.11 + float2(gTime * 0.010, 0.37)).r,
                             gTexture.Sample(gSampler, ip * 0.11 + float2(0.71, -gTime * 0.008)).g) - 0.5;
        float2 q = ip + warp * 9.0;
        float m1 = gTexture.Sample(gSampler, q * 0.35 + float2(gTime * 0.004, 0.0)).r;
        float m2 = gTexture.Sample(gSampler, q * 0.9 + float2(0.0, -gTime * 0.006)).g;
        float marble = saturate(((m1 * 0.6 + m2 * 0.4) - 0.5) * 3.0 + 0.5); // 0〜1 に引き伸ばす

        // 筋（墨流しの縞）：濃さの等高線をノイズでゆがめた細い帯。薄い所ほど縞が目立つ
        float bands = 1.0 - abs(frac(w.inkAmount * 2.5 + marble * 1.3) * 2.0 - 1.0); // 0〜1 の三角波

        float d = w.inkAmount * lerp(0.35, 1.65, marble);
        float coreT = smoothstep(0.45, 0.95, d);                                  // 芯
        float wispT = smoothstep(0.03, 0.4, d) * lerp(0.35, 1.0, bands);         // 滲んだ筋
        float strength = saturate(gInkColor.a);
        coreT *= strength;
        wispT *= strength;

        // 筋：水の色を暗く沈める（色を置き換えないので、灰色がかった滲みになる）
        color *= lerp(1.0, 0.4, wispT);
        // 芯：墨色（光は受ける。日陰では暗く）
        float3 inkLit = gInkColor.rgb * lit.lightColor * lerp(0.6, 1.0, lit.diffuse) * lit.shadowTint;
        color = lerp(color, inkLit, coreT);
        body.inkT = max(coreT, wispT * 0.6);
    }

    // =========================
    // サブサーフェススキャタリング（波頭を透ける光）
    // =========================
    // 太陽の方を向いて波を見ると、薄くなった波頭を光が通り抜けて明るい青緑に光る。
    //   crest   : 波の高い所ほど薄く透けやすい
    //   facing  : 視線が太陽の方向（水平成分）を向いているほど強い
    //   grazing : 波面を斜めから見ているほど強い（真上視点でもわずかに残す）
    if (gSssColor.a > 0.0)
    {
        float ampAll = max(gWaveHeight * 1.25 + gWaveHeight2
                           + DetailAmplitudeSum(gWaveFreq, gWaveHeight, gOceanParams.x), 0.001);
        float crest = saturate(input.waveHeight / ampAll * 0.5 + 0.5);
        float2 vh = -V.xz;
        float2 lh = lit.L.xz;
        float facing = (dot(vh, vh) > 1e-6 && dot(lh, lh) > 1e-6)
            ? saturate(dot(normalize(vh), normalize(lh))) : 0.0;
        float sunLow = saturate(1.0 - lit.L.y * 0.7); // 太陽が低いほど横から抜ける
        float grazing = lerp(0.3, 1.0, 1.0 - lit.NdotV);
        float sss = gSssColor.a * crest * crest * (0.2 + 1.6 * pow(facing, 4.0) * sunLow) * grazing;
        sss *= lit.sunVisible; // 影の中では波頭を抜けてくる日光が無い
        body.scatter = gSssColor.rgb * lit.lightColor * sss;
    }

    body.color = color;
    // 屈折ありのときは自前で水中を合成済みなので不透明。物体との交差部だけ α でぼかしてクリッピングの線を消す
    body.alpha = refractionOn ? saturate(depthDiff / max(gRefractParams.z, 0.001)) : waterColor.a;
    return body;
}

// -----------------------------------------------------------------------------
// 3. 反射
// -----------------------------------------------------------------------------

/// @brief 空の映り込みと太陽のギラつきを求める
/// @param px 画素の共通情報
/// @param w 波の計算結果
/// @param lit 光の情報
/// @return 水面で跳ね返る光
WaterReflection EvaluateReflection(WaterPixel px, WaveSurface w, WaterLighting lit)
{
    WaterReflection r;

    // 環境マップ反射
    float3 reflectedDir = reflect(-px.V, w.N);
    // 波の裏側で反射ベクトルが下を向くと、スカイボックスの地面側（暗い色）を拾って黒い斑点になる。
    // 実際には別の波面に当たって空を映すので、水平より上に折り返しておく。
    reflectedDir.y = max(reflectedDir.y, 0.02);
    reflectedDir = normalize(reflectedDir);
    r.env = gEnvironmentTexture.Sample(gSampler, reflectedDir).rgb;
    r.weight = lit.fresnel * gMaterial.environmentCoefficient;

    // スペキュラ（GGX）
    // Specular Power（Blinn-Phong の指数）を GGX の粗さへ換算して互換を保つ: alpha = √(2/(n+2))
    // 遠景では 1 ピクセルに多数の波面が入るので粗さを上げる → 水平線に向かって太陽の光の道が伸びる
    float alpha = sqrt(2.0 / (max(gSpecularPower, 1.0) + 2.0));
    alpha = lerp(alpha, max(alpha, 0.22), px.distT);
    alpha = clamp(alpha, 0.02, 1.0);
    float specular = min(SpecularGGX(w.N, px.V, lit.L, alpha, kWaterF0), 16.0) * lit.sunVisible;
    r.specular = lit.lightColor * specular;
    return r;
}

// -----------------------------------------------------------------------------
// 4. 泡
// -----------------------------------------------------------------------------

/// @brief 白波・引き波・波頭・波打ち際の泡を 1 枚の層にまとめる
/// @param input VS からの補間値
/// @param px 画素の共通情報
/// @param shore 跳ね返り波の情報
/// @param w 波の計算結果
/// @param lit 光の情報
/// @param body 水中光の計算結果（山の位置・墨の覆い具合を使う）
/// @return 泡の層
FoamLayer EvaluateFoam(VertexShaderOutput input, WaterPixel px, ShoreWave shore,
                       WaveSurface w, WaterLighting lit, WaterBody body)
{
    FoamLayer f;
    f.color = float3(0, 0, 0);
    f.transmit = 1.0;
    f.alphaMask = 0.0;

    // 墨の上では泡を薄くする（白い泡が墨を突き抜けて見えないように）
    float wakeFoam  = w.wakeFoam  * (1.0 - body.inkT * 0.8);
    float crestFoam = w.crestFoam * (1.0 - body.inkT * 0.5);

    // =========================
    // 白波（波の山が押しつぶされて砕ける所）
    // =========================
    // Gerstner 波は山で頂点が寄り集まる。水平変位のヤコビアン J が 1 より小さい所ほど
    // 水面が圧縮されている（0 で折り返す＝砕ける）ので、そこに泡を乗せる。
    if (gOceanParams.z > 0.0)
    {
        float3 jm = input.mainJacobian + w.detailJacobian;
        float J = (1.0 + jm.x) * (1.0 + jm.y) - jm.z * jm.z;
        float threshold = lerp(0.55, 0.95, saturate(gOceanParams2.w)); // 白波の量
        float foldFoam = saturate((threshold - J) / 0.25);

        // 泡の形を崩すノイズ（法線マップのチャンネルを借用、ゆっくり流す）
        float2 wp = input.worldPosition.xz;
        float lace = gTexture.Sample(gSampler, wp * 0.21 + float2(gTime * 0.02, -gTime * 0.013)).r * 0.6
                   + gTexture.Sample(gSampler, wp * 0.53 + float2(-gTime * 0.03, gTime * 0.021)).g * 0.4;
        foldFoam *= smoothstep(0.35, 0.65, lace + foldFoam * 0.3);

        float whitecap = saturate(foldFoam * gOceanParams.z) * lerp(1.0, 0.6, px.distT);
        float3 capLit = gFoamColor.rgb * min(lit.lightColor * lerp(0.7, 0.95, lit.NdotL), 0.95) * lit.shadowTint;
        FoamOver(f, capLit, whitecap);
        f.alphaMask = max(f.alphaMask, whitecap);
    }

    // 山のいちばん高いところだけ薄く白を乗せる（真上視点の白波）。crestTint が 0 なら何もしない
    if (gFoamParams.z > 0.0)
    {
        // 白波も凪の所では立てない（ムラのマスクで疎密を付ける）
        float crestCap = pow(body.crestT, 8.0) * gFoamParams.z * 0.35 * w.roughMask;
        FoamOver(f, gFoamColor.rgb * lit.shadowTint, crestCap);
    }

    // =========================
    // 引き波・波頭の泡（波紋テクスチャの G / B チャンネル）
    // =========================
    // スクリプトが置いた 2 種類の泡を描く。
    //   引き波（g）… 船尾がかき回した帯。主役は「白く濁った明るい水色」で、白い筋はまばらに乗せるだけ。
    //                ベタ塗りの白にすると帯が白飛びして見えるので、白の不透明度に上限を設ける
    //   波頭（b）  … 船首の砕け波と V の腕。細い白い線。スクリプトが毎フレーム置き直し、すぐ消える
    // 白はライトの強さで白飛び（＋ブルーム）しないよう明るさに上限を設ける。
    // 0.39m / テクセルのボケを隠すためにノイズで崩すが、細かすぎるとザラついた砂嵐に見えるので低周波だけ使う。
    if (wakeFoam > 0.001 || crestFoam > 0.001)
    {
        float2 fp = input.worldPosition.xz;
        // 法線マップの成分をノイズとして借用（周期 8m / 3m 程度、ゆっくり流す）
        float n1 = gTexture.Sample(gSampler, fp * 0.12 + float2(gTime * 0.010, gTime * 0.007)).r;
        float n2 = gTexture.Sample(gSampler, fp * 0.33 + float2(-gTime * 0.016, gTime * 0.011)).g;
        float lace = saturate(((n1 * 0.6 + n2 * 0.4) - 0.5) * 2.0 + 0.5);

        // 泡の白（日向で少し明るく。白飛びしないよう上限 0.9）
        float3 foamLit = gFoamColor.rgb * min(lit.lightColor * lerp(0.75, 0.92, lit.NdotL), 0.9) * lit.shadowTint;

        // ---- 引き波：白濁 ＋ まばらな白い筋 ----
        float milkyT = smoothstep(0.03, 1.2, wakeFoam);
        FoamOver(f, foamLit, milkyT * 0.35);

        float thr = 1.0 - saturate(wakeFoam * 0.8);
        float streak = smoothstep(thr - 0.12, thr + 0.12, lace) * smoothstep(0.08, 0.35, wakeFoam);
        float trailMask = streak * 0.55;

        // ---- 波頭：細い白線（ノイズで少しだけ途切れさせる）----
        float crestMask = smoothstep(0.25, 0.8, crestFoam * lerp(0.7, 1.3, lace)) * 0.9;

        float foamMask = max(trailMask, crestMask);
        FoamOver(f, foamLit, foamMask);
        f.alphaMask = max(f.alphaMask, max(foamMask, milkyT * 0.4));
    }

    // =========================
    // 波打ち際（岸の白線と跳ね返り波の泡）
    // =========================
    float returnWave = sin(shore.phase) * 0.5f + 0.5f; // 0〜1

    // ノイズを使って泡の形を不規則にする（gTexture の r チャンネルを借用、少しゆっくり動かす）
    float2 noiseUV = input.worldPosition.xz * 0.15f + float2(gTime * 0.05f, -gTime * 0.05f);
    float foamNoise = gTexture.Sample(gSampler, noiseUV).r;

    // 波打ち際全体の泡強度を合成（基本強度 × 跳ね返り波 × ノイズ）
    // さらに、岸スレスレ（fade が 0 に近い部分）は常に白く残るようにする
    float dynamicFoam = shore.intensity * returnWave * (foamNoise * 2.0f);
    float staticFoam = pow(1.0f - shore.fade, 4.0f); // 岸の根本の強い白線
    float shoreFoam = saturate(dynamicFoam + staticFoam);

    float3 shoreColor = gFoamColor.rgb * gFoamParams.y * lit.shadowTint; // スケール適用（影の中は暗く）
    FoamOver(f, shoreColor, shoreFoam * gFoamColor.a);
    return f;
}

// =============================================================================
// エントリポイント
// =============================================================================

/// @brief エントリポイント。水面の各成分を個別に求め、足し合わせて最終色と不透明度を出す
/// @param input VS からの補間値
/// @return 出力色
PixelShaderOutput main(VertexShaderOutput input)
{
    WaterPixel px = GetWaterPixel(input);
    ShoreWave shore = GetShoreWave(px.depthDiff);

    // 1. 波（形）
    WaveSurface wave = EvaluateWaveSurface(input, px, shore);
    WaterLighting lit = EvaluateLighting(input, px, wave.N);

    // 2〜4. 各成分（互いの色には手を入れない）
    WaterBody       body = EvaluateWaterBody(input, px, wave, lit);
    WaterReflection refl = EvaluateReflection(px, wave, lit);
    FoamLayer       foam = EvaluateFoam(input, px, shore, wave, lit, body);

    // 合成（加算）
    float3 bodyTerm    = (body.color * (1.0 - refl.weight) + body.scatter * (1.0 - lit.fresnel)) * foam.transmit;
    float3 reflectTerm = (refl.env * refl.weight + refl.specular) * foam.transmit;
    float3 foamTerm    = foam.color;
    float3 finalColor  = bodyTerm + reflectTerm + foamTerm;

    // 泡・墨は水面に浮いた不透明な膜なので、濃い所は透かさない（波打ち際の泡は従来どおり α に含めない）
    PixelShaderOutput output;
    output.color = float4(finalColor, lerp(body.alpha, 1.0, max(foam.alphaMask, body.inkT * 0.85)));
    return output;
}
