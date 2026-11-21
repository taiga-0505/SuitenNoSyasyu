// ============================================================================
// EmitParticle.CS.hlsl
// ----------------------------------------------------------------------------
// GPU Particle の射出用 Compute Shader。
// FreeList から空きインデックスを取得してパーティクルを初期化する。
// Particle Editor から渡されるパラメータに基づいて
// 寿命、サイズ、速度、色、エミッタ形状を設定する。
//
// u0 : gParticles      — パーティクルデータ
// u1 : gFreeListIndex  — FreeList の現在のインデックス
// u2 : gFreeList       — 空きパーティクルインデックスの配列
// b0 : gPerFrame       — エミッタパラメータ
//
// Dispatch(1, 1, 1) で emitCount スレッド（最大 1024）実行。
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
    // 基本パラメータ（16 bytes）
    float gDeltaTime;        ///< フレーム間隔（秒）
    uint gMaxParticles;      ///< 最大パーティクル数
    float gMinLifeTime;      ///< 最小寿命（秒）
    float gMaxLifeTime;      ///< 最大寿命（秒）

    // スケール（16 bytes）
    float gMinScale;         ///< 最小スケール
    float gMaxScale;         ///< 最大スケール
    float gGravity;          ///< 重力の強さ（下方向が正）
    uint gEmitterShape;      ///< エミッタ形状（0: Point, 1: Sphere, 2: Box, 3: Cone）

    // 速度（16 bytes）
    float3 gBaseVelocity;    ///< 基本速度（1 フレームあたりの移動量）
    float gVelocityVariance; ///< 速度のランダム分散

    // 形状パラメータ（16 bytes）
    float gShapeRadius;      ///< Sphere / Cone の半径
    float gConeAngle;        ///< Cone の半角（rad）
    float2 gShapePad;        ///< パディング

    // 開始色（16 bytes）
    float4 gStartColor;      ///< 開始色（RGBA）

    // 終了色（16 bytes）
    float4 gEndColor;        ///< 終了色（RGBA）

    // エミッタ位置（16 bytes）
    float3 gEmitterPosition; ///< エミッタのワールド座標
    uint gEmitCount;         ///< 1 フレームの射出数

    // Box 形状サイズ（16 bytes）
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

/// @brief エミッタ形状ごとの初期位置を計算する
/// @param seed 乱数のシード
/// @return 発生位置（ワールド座標）
float3 ComputeEmitPosition(uint seed)
{
    float3 pos = gEmitterPosition;

    if (gEmitterShape == 0) // Point
    {
        // 一点から（位置変化なし）
    }
    else if (gEmitterShape == 1) // Sphere
    {
        // 球の内部で一様な位置（半径を 1/3 乗して中心への偏りを打ち消す）
        float theta = Hash(seed) * 6.28318530f;
        float phi = acos(HashSigned(seed + 1u));
        float r = gShapeRadius * pow(Hash(seed + 2u), 1.0f / 3.0f);
        pos.x += r * sin(phi) * cos(theta);
        pos.y += r * sin(phi) * sin(theta);
        pos.z += r * cos(phi);
    }
    else if (gEmitterShape == 2) // Box
    {
        pos.x += HashSigned(seed) * gShapeBoxSize.x * 0.5f;
        pos.y += HashSigned(seed + 1u) * gShapeBoxSize.y * 0.5f;
        pos.z += HashSigned(seed + 2u) * gShapeBoxSize.z * 0.5f;
    }
    else if (gEmitterShape == 3) // Cone
    {
        float angle = Hash(seed) * 6.28318530f;
        float radiusFactor = Hash(seed + 1u);
        float halfAngle = gConeAngle;
        float spreadRadius = tan(halfAngle) * radiusFactor * gShapeRadius;
        pos.x += cos(angle) * spreadRadius;
        pos.z += sin(angle) * spreadRadius;
    }

    return pos;
}

/// @brief エミッタ形状ごとの初速を計算する
/// @param seed 乱数のシード
/// @return 初速（1 フレームあたりの移動量）
float3 ComputeEmitVelocity(uint seed)
{
    float3 vel = gBaseVelocity;

    if (gEmitterShape == 0) // Point
    {
        vel.x += HashSigned(seed) * gVelocityVariance;
        vel.y += HashSigned(seed + 1u) * gVelocityVariance;
        vel.z += HashSigned(seed + 2u) * gVelocityVariance;
    }
    else if (gEmitterShape == 1) // Sphere
    {
        // 放射状
        float theta = Hash(seed) * 6.28318530f;
        float phi = acos(HashSigned(seed + 1u));
        float speed = length(gBaseVelocity) + HashSigned(seed + 2u) * gVelocityVariance;
        vel.x = sin(phi) * cos(theta) * speed;
        vel.y = sin(phi) * sin(theta) * speed;
        vel.z = cos(phi) * speed;
    }
    else if (gEmitterShape == 2) // Box
    {
        vel.x += HashSigned(seed) * gVelocityVariance;
        vel.y += HashSigned(seed + 1u) * gVelocityVariance;
        vel.z += HashSigned(seed + 2u) * gVelocityVariance;
    }
    else if (gEmitterShape == 3) // Cone
    {
        float angle = Hash(seed) * 6.28318530f;
        float spread = Hash(seed + 1u) * gConeAngle;
        float speed = length(gBaseVelocity) + HashSigned(seed + 2u) * gVelocityVariance;
        vel.x = sin(spread) * cos(angle) * speed;
        vel.y = cos(spread) * speed;
        vel.z = sin(spread) * sin(angle) * speed;
    }

    return vel;
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
        uint seed = particleIndex * 1973u + DTid.x * 6547u + 9277u;

        // パーティクル初期化
        Particle p = (Particle)0;

        // 位置: エミッタ形状に基づく
        p.translate = ComputeEmitPosition(seed + 100u);

        // スケール: パラメータ範囲
        float s = gMinScale + Hash(seed + 3u) * (gMaxScale - gMinScale);
        p.scale = float3(s, s, s);

        // 速度: エミッタ形状に基づく
        p.velocity = ComputeEmitVelocity(seed + 200u);

        // 寿命: パラメータ範囲
        p.lifeTime = gMinLifeTime + Hash(seed + 7u) * (gMaxLifeTime - gMinLifeTime);
        p.currentTime = 0.0f;

        // 色: 開始色
        p.color = gStartColor;

        gParticles[particleIndex] = p;
    }
    else
    {
        // 空きがないので、減らした分を戻す
        InterlockedAdd(gFreeListIndex[0], 1);
    }
}
