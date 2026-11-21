// ============================================================================
// UpdateParticle.CS.hlsl
// ----------------------------------------------------------------------------
// GPU Particle の毎フレーム更新用 Compute Shader。
// velocity による移動、deltaTime による寿命管理、重力適用、
// startColor → endColor のカラー遷移、alpha 減衰を行う。
// 寿命切れパーティクルは FreeList に返却する。
//
// u0 : gParticles      — パーティクルデータ（RWStructuredBuffer）
// u1 : gFreeListIndex  — FreeList の現在のインデックス
// u2 : gFreeList       — 空きパーティクルインデックスの配列
// b0 : gPerFrame       — エミッタパラメータ
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

    // 重力を速度に適用
    gParticles[particleIndex].velocity.y -= gGravity * gDeltaTime;

    // 速度による移動
    gParticles[particleIndex].translate += gParticles[particleIndex].velocity;

    // 経過時間の更新
    gParticles[particleIndex].currentTime += gDeltaTime;

    // 寿命の進行度（0.0〜1.0）
    float lifeRatio = saturate(gParticles[particleIndex].currentTime / gParticles[particleIndex].lifeTime);

    // startColor → endColor のカラー遷移 + alpha 減衰
    float4 lerpedColor = lerp(gStartColor, gEndColor, lifeRatio);
    float alpha = 1.0f - lifeRatio;
    gParticles[particleIndex].color = float4(lerpedColor.rgb, lerpedColor.a * alpha);

    // alpha が 0 になったら FreeList に返却
    if (gParticles[particleIndex].color.a <= 0.001f)
    {
        gParticles[particleIndex].color.a = 0;

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
