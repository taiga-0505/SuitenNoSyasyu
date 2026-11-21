// ============================================================================
// UpdateRain.CS.hlsl
// ----------------------------------------------------------------------------
// GPU Particle の雨エフェクト用 Update Compute Shader。
// 重力加速度を適用して落下させ、地面到達で FreeList に返却する。
//
// u0 : gParticles      — パーティクルデータ（RWStructuredBuffer）
// u1 : gFreeListIndex  — FreeList の現在のインデックス
// u2 : gFreeList       — 空きパーティクルインデックスの配列
// b0 : gPerFrame       — フレーム情報（deltaTime）
//
// 重力・地面の高さはシェーダー内の固定値（kGravity / kGroundY）で、
// PerFrame のうち参照するのは gDeltaTime と gMaxParticles だけ。
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
    float gGravity;          ///< 重力の強さ（雨では参照せず kGravity を使う）
    uint gEmitterShape;      ///< エミッタ形状（0: Point, 1: Sphere, 2: Box, 3: Cone）

    float3 gBaseVelocity;    ///< 基本速度（1 フレームあたりの移動量）
    float gVelocityVariance; ///< 速度のランダム分散

    float gShapeRadius;      ///< Sphere / Cone の半径
    float gConeAngle;        ///< Cone の半角（rad）
    float2 gShapePad;        ///< パディング

    float4 gStartColor;      ///< 開始色（RGBA）
    float4 gEndColor;        ///< 終了色（RGBA）

    float3 gEmitterPosition; ///< エミッタのワールド座標
    float gEmitterPad;       ///< C++ 側の emitCount の枠（ここでは使わない）

    float3 gShapeBoxSize;    ///< Box 形状のサイズ（全長 xyz）
    float gShapeBoxPad;      ///< パディング
};


static const float kGravity = -0.001f;   ///< 重力加速度（1 フレームあたりの速度変化）
static const float kGroundY = -1.0f;     ///< 地面の高さ（これ以下に落ちたら消す）

RWStructuredBuffer<Particle> gParticles     : register(u0);
RWStructuredBuffer<int>      gFreeListIndex : register(u1);
RWStructuredBuffer<uint>     gFreeList      : register(u2);

/// @brief エントリポイント。1 スレッドが 1 パーティクルを更新する
/// @param DTid ディスパッチスレッド ID（x がパーティクル番号）
[numthreads(1024, 1, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    uint particleIndex = DTid.x;
    if (particleIndex >= gMaxParticles)
    {
        return;
    }

    // alpha が 0 のパーティクルは死んでいるとみなして更新しない
    if (gParticles[particleIndex].color.a == 0)
    {
        return;
    }

    // 重力加速度を速度に適用
    gParticles[particleIndex].velocity.y += kGravity;

    // 速度による移動
    gParticles[particleIndex].translate += gParticles[particleIndex].velocity;

    // 経過時間の更新
    gParticles[particleIndex].currentTime += gDeltaTime;

    // 寿命に基づくアルファ減衰
    float alpha = 1.0f - (gParticles[particleIndex].currentTime / gParticles[particleIndex].lifeTime);

    // 地面到達チェック（地面以下なら即座に消滅）
    if (gParticles[particleIndex].translate.y <= kGroundY)
    {
        alpha = 0.0f;
    }

    gParticles[particleIndex].color.a = saturate(alpha);

    // alpha が 0 になったら FreeList に返却
    if (gParticles[particleIndex].color.a == 0)
    {
        // スケールを 0 にして VertexShader 出力で棄却されるようにする
        gParticles[particleIndex].scale = float3(0.0f, 0.0f, 0.0f);

        // FreeList に返却
        int freeListIndex;
        InterlockedAdd(gFreeListIndex[0], 1, freeListIndex);

        // 最新の FreeListIndex の場所に死んだ ParticleIndex を設定
        if ((freeListIndex + 1) < (int)gMaxParticles)
        {
            gFreeList[freeListIndex + 1] = particleIndex;
        }
        else
        {
            // ここに来るはずはないが、安全策
            InterlockedAdd(gFreeListIndex[0], -1);
        }
    }
}
