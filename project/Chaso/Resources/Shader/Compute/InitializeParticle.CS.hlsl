// ============================================================================
// InitializeParticle.CS.hlsl
// ----------------------------------------------------------------------------
// GPU Particle の初期化用 Compute Shader。
// パーティクルバッファをゼロクリアし、FreeList を連番で初期化する。
//
// u0 : gParticles      — パーティクルデータ（RWStructuredBuffer<Particle>）
// u1 : gFreeListIndex  — FreeList の現在のインデックス（RWStructuredBuffer<int>）
// u2 : gFreeList       — 空きパーティクルインデックスの配列（RWStructuredBuffer<uint>）
// b0 : gPerFrame       — gMaxParticles だけ参照する
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

/// @brief エントリポイント。1 スレッドが 1 パーティクルと FreeList の 1 要素を初期化する
/// @param DTid ディスパッチスレッド ID（x がパーティクル番号）
[numthreads(1024, 1, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    uint particleIndex = DTid.x;
    if (particleIndex >= gMaxParticles)
    {
        return;
    }

    // パーティクルを非表示状態（scale = 0）で初期化
    gParticles[particleIndex] = (Particle)0;

    // FreeList を連番で初期化: [0, 1, 2, ..., gMaxParticles-1]
    gFreeList[particleIndex] = particleIndex;

    // スレッド 0 のみ: FreeListIndex を末尾に設定
    if (particleIndex == 0)
    {
        gFreeListIndex[0] = (int)(gMaxParticles - 1);
    }
}
