// ============================================================================
// EmitExplosion.CS.hlsl
// ----------------------------------------------------------------------------
// GPU Particle の爆発エフェクト用 Emit Compute Shader。
// 全方向にランダム放射するパーティクルを射出する。
// 位置・スケール・速度・寿命・色はシェーダー内の固定値で決めており、
// PerFrame のうち参照するのは gEmitCount と gMaxParticles だけ。
//
// u0 : gParticles      — パーティクルデータ
// u1 : gFreeListIndex  — FreeList の現在のインデックス
// u2 : gFreeList       — 空きパーティクルインデックスの配列
// b0 : gPerFrame       — deltaTime 等
//
// Dispatch(emitCount / 1024, 1, 1) で実行。
// ============================================================================

struct Particle
{
    float3 translate;   ///< 位置
    float pad0;         ///< パディング（16 bytes アラインメント）
    float3 scale;       ///< スケール（0 なら描画時に棄却される）
    float lifeTime;     ///< 寿命（秒）
    float3 velocity;    ///< 速度（1 フレームあたりの移動量）
    float currentTime;  ///< 経過時間（秒）
    float4 color;       ///< 色（RGBA。alpha が 0 なら死んでいる扱い）
};

cbuffer PerFrame : register(b0)
{
    float gDeltaTime;        ///< フレーム間隔（秒）
    uint gMaxParticles;      ///< 最大パーティクル数
    float gMinLifeTime;      ///< 最小寿命（秒）
    float gMaxLifeTime;      ///< 最大寿命（秒）

    float gMinScale;         ///< 最小スケール
    float gMaxScale;         ///< 最大スケール
    float gGravity;          ///< 重力の強さ（下方向が正）
    uint gEmitterShape;      ///< エミッタ形状（0: Point, 1: Sphere, 2: Box, 3: Cone）

    float3 gBaseVelocity;    ///< 基本速度（1 フレームあたりの移動量）
    float gVelocityVariance; ///< 速度のランダム分散

    float gShapeRadius;      ///< Sphere / Cone の半径
    float gConeAngle;        ///< Cone の半角（rad）
    float2 gShapePad;        ///< パディング

    float4 gStartColor;      ///< 開始色（RGBA）
    float4 gEndColor;        ///< 終了色（RGBA）

    float3 gEmitterPosition; ///< エミッタのワールド座標
    uint gEmitCount;         ///< 1 フレームの射出数

    float3 gShapeBoxSize;    ///< Box 形状のサイズ（全長 xyz）
    float gShapeBoxPad;      ///< パディング
};



RWStructuredBuffer<Particle> gParticles     : register(u0);
RWStructuredBuffer<int>      gFreeListIndex : register(u1);
RWStructuredBuffer<uint>     gFreeList      : register(u2);

/// @brief 簡易ハッシュ関数。seed から 0〜1 の擬似乱数を作る
/// @param seed 乱数のシード
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

/// @brief Hash を -1〜1 に広げた擬似乱数
/// @param seed 乱数のシード
/// @return -1〜1 の擬似乱数
float HashSigned(uint seed)
{
    return Hash(seed) * 2.0f - 1.0f;
}

/// @brief 球面上で一様なランダム方向ベクトルを生成する
/// @param seed 乱数のシード（seed と seed + 1 を使う）
/// @return 単位方向ベクトル
float3 RandomSphereDirection(uint seed)
{
    // cos(theta) を [-1, 1] で一様に、phi を [0, 2*PI] で一様に取る
    float cosTheta = HashSigned(seed);
    float sinTheta = sqrt(max(0.0f, 1.0f - cosTheta * cosTheta));
    float phi = Hash(seed + 1u) * 6.28318530718f; // 2 * PI

    return float3(
        sinTheta * cos(phi),
        sinTheta * sin(phi),
        cosTheta
    );
}

/// @brief エントリポイント。1 スレッドが 1 パーティクルを射出する
/// @param DTid ディスパッチスレッド ID（x が射出番号。gEmitCount 以上は何もしない）
[numthreads(1024, 1, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= gEmitCount)
    {
        return;
    }

    // FreeList のインデックスを 1 つ前に設定し、現在のインデックスを取得
    int freeListIndex;
    InterlockedAdd(gFreeListIndex[0], -1, freeListIndex);

    if (0 <= freeListIndex && freeListIndex < (int)gMaxParticles)
    {
        // FreeList から空きパーティクルインデックスを取得
        uint particleIndex = gFreeList[freeListIndex];

        // ランダムシード
        uint seed = particleIndex * 1973u + DTid.x * 6547u + 13331u;

        // パーティクル初期化
        Particle p = (Particle)0;

        // 位置: 原点付近に集中（gEmitterPosition は足していない）
        p.translate = float3(
            HashSigned(seed + 10u) * 0.5f,
            Hash(seed + 11u) * 0.5f,
            HashSigned(seed + 12u) * 0.5f
        );

        // スケール: やや小さめ
        float s = 0.15f + Hash(seed + 3u) * 0.2f;
        p.scale = float3(s, s, s);

        // 速度: 球面上のランダム方向 × ランダム速さ
        float3 dir = RandomSphereDirection(seed + 20u);
        float speed = 0.03f + Hash(seed + 30u) * 0.06f;
        p.velocity = dir * speed;

        // 寿命: 短め（爆発なので）
        p.lifeTime = 1.0f + Hash(seed + 7u) * 2.0f;
        p.currentTime = 0.0f;

        // 色: オレンジ〜赤のグラデーション
        float hue = Hash(seed + 40u);
        p.color = float4(
            1.0f,
            0.3f + hue * 0.5f,
            0.1f,
            1.0f
        );

        gParticles[particleIndex] = p;
    }
    else
    {
        // 空きがないので、減らした分を戻す
        InterlockedAdd(gFreeListIndex[0], 1);
    }
}
