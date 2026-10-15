// ============================================================================
// GPUParticle.hlsli
// ----------------------------------------------------------------------------
// GPU パーティクル用の共通構造体定義（VS と各パーティクル PS で共有）
// ============================================================================

/// @struct Particle
/// @brief パーティクル 1 粒分のデータ（Compute Shader が初期化・更新する）
struct Particle
{
    float3 translate;  ///< ワールド座標
    float pad0;        ///< 予備領域。ParticleType::Fire / Electric では固有の乱数（0〜1）として使う
    float3 scale;      ///< 大きさ
    float lifeTime;    ///< 寿命（秒）
    float3 velocity;   ///< 速度
    float currentTime; ///< 生成からの経過時間（秒）
    float4 color;      ///< 色（RGBA）
};

/// @struct PerView
/// @brief カメラごとの定数
struct PerView
{
    row_major float4x4 viewProjection;  ///< ビュー・プロジェクション行列
    row_major float4x4 billboardMatrix; ///< カメラへ向けるビルボード回転行列
};

/// @struct VertexShaderOutput
/// @brief GPUParticle.VS の出力
struct VertexShaderOutput
{
    float4 position : SV_POSITION; ///< クリップ空間位置
    float2 texcoord : TEXCOORD0;   ///< テクスチャ座標
    float4 color : COLOR0;         ///< パーティクル色
    /// @brief パーティクル固有の情報
    /// x = 寿命の進行度（0 = 生成直後, 1 = 消滅直前）
    /// y = パーティクル固有の乱数（0〜1）。Particle.pad0 の値
    /// @details 炎のように「経過時間で形やちらつきを変えたい」PS のために
    ///          VS から渡す。使わない PS は無視すればよい。
    float2 particleParams : TEXCOORD1;
};
