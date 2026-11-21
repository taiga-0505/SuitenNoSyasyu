// ============================================================================
// UpdateElectric.CS.hlsl
// ----------------------------------------------------------------------------
// 電撃（ParticleType::Electric）用の更新 Compute Shader。
//
// UpdateParticle.CS との違いは「モデルの表面を這い回る」動きを足している点:
//   1. 殻への射影 … 速度で動かした後、殻（丸みのある箱 / 球。ElectricShell.hlsli 参照）の
//                   表面 + マージンへ戻す。中心は「現在の」エミッタ位置なので、モデルが動いても
//                   パーティクルは置いていかれず、まとわりついたまま追従する
//   2. ジッター   … 一定間隔で進行方向をランダムに折り曲げる → バチバチと不規則に走る
//   3. 接線化     … 速度から法線成分を除き、殻から離れる／食い込む動きを消す
//
// 色は既存と同じ startColor → endColor の遷移＋alpha 減衰。
// 細かな明滅と稲妻の「形」は ElectricParticle.PS 側で作る。
// gravity は電撃には不要なので参照しない。
//
// u0 : gParticles      — パーティクルデータ（RWStructuredBuffer）
// u1 : gFreeListIndex  — FreeList の現在のインデックス
// u2 : gFreeList       — 空きパーティクルインデックスの配列
// b0 : gPerFrame       — エミッタパラメータ
// ============================================================================

struct Particle
{
    float3 translate;   ///< 位置
    float pad0;         ///< 電撃ではパーティクル固有の乱数（0〜1）。EmitElectric が書き込む
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
    float gGravity;          ///< 重力の強さ（電撃では参照しない）
    uint gEmitterShape;      ///< エミッタ形状（2: Box なら箱の殻、それ以外は球の殻）

    // 速度（16 bytes）
    float3 gBaseVelocity;    ///< 基本速度（更新では参照しない）
    float gVelocityVariance; ///< 速さのランダム分散（更新では参照しない）

    // 形状パラメータ（16 bytes）
    // ※ 他タイプの gShapePad.xy に当たる 2 つを、電撃では殻の丸みとマージンに使う
    float gShapeRadius;      ///< 球の殻の半径
    float gConeAngle;        ///< Cone の半角（rad。電撃では参照しない）
    float gShellRoundness;   ///< 殻の角の丸み（0〜1。0 で角のある箱、1 で最短辺いっぱいまで丸める）
    float gShellMargin;      ///< 殻を表面から外側へ浮かせる距離（m）

    // 開始色（16 bytes）
    float4 gStartColor;      ///< 開始色（RGBA）

    // 終了色（16 bytes）
    float4 gEndColor;        ///< 終了色（RGBA）

    // エミッタ位置（16 bytes）
    float3 gEmitterPosition; ///< エミッタのワールド座標（殻の中心。毎フレーム現在値を使う）
    uint gEmitCount;         ///< 1 フレームの射出数

    // Box 形状サイズ（16 bytes）
    // ※ 他タイプの gShapeBoxPad に当たる 1 つを、電撃では殻の Y 軸回転に使う
    float3 gShapeBoxSize;    ///< Box の殻のサイズ（全長 xyz）
    float gShellYaw;         ///< 殻の Y 軸回転（rad）
};

RWStructuredBuffer<Particle> gParticles     : register(u0);
RWStructuredBuffer<int>      gFreeListIndex : register(u1);
RWStructuredBuffer<uint>     gFreeList      : register(u2);

#include "ElectricShell.hlsli"

/// @brief 方向転換の頻度（回/秒）。大きいほど細かくジグザグに走る
static const float kJitterRate = 40.0f;

/// @brief 方向転換時に新しいランダム方向へどれだけ寄せるか（0 = 直進、1 = 完全にランダム）
static const float kJitterBlend = 0.75f;

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

    Particle p = gParticles[particleIndex];

    // ------------------------------------------------------------
    // 1. 経過時間
    // ------------------------------------------------------------
    float timeBefore = p.currentTime;
    p.currentTime += gDeltaTime;

    // ------------------------------------------------------------
    // 2. ジッター（方向転換）
    // ------------------------------------------------------------
    // kJitterRate 回/秒の「刻み」をまたいだフレームだけ向きを変える。
    // 刻み番号をシードに使うので、同じパーティクルでも毎回違う方向へ折れる。
    float speed = length(p.velocity);
    uint tickBefore = (uint)floor(timeBefore * kJitterRate);
    uint tickAfter  = (uint)floor(p.currentTime * kJitterRate);
    if (tickAfter != tickBefore && speed > 1e-6f)
    {
        uint seed = particleIndex * 7919u + tickAfter * 104729u + (uint)(p.pad0 * 65535.0f);
        float3 newDir = normalize(lerp(p.velocity / speed, RandomDirection(seed), kJitterBlend));
        p.velocity = newDir * speed;
    }

    // ------------------------------------------------------------
    // 3. 速度による移動（velocity は 1 フレームあたりの移動量）
    // ------------------------------------------------------------
    p.translate += p.velocity;

    // ------------------------------------------------------------
    // 4. 殻の表面へ射影
    // ------------------------------------------------------------
    // 殻ローカル（エミッタ中心・Y 軸回転を戻した座標）で表面へ射影し、マージン分浮かせて戻す。
    // 中心が現在のエミッタ位置なので、モデルが移動・回転しても火花は表面に張り付いたまま。
    float3 h = ShellHalfExtents();
    float r = ShellCornerRadius(h);
    float3 local = WorldToShell(p.translate - gEmitterPosition);

    float3 normalLocal;
    float3 surface = ProjectToShell(local, h, r, normalLocal);
    p.translate = gEmitterPosition + ShellToWorld(surface + normalLocal * ShellLift(p.pad0));

    // 速度を接線面へ（法線成分を除去）。射影で縮んだ分は元の速さに戻す
    float3 normalWorld = ShellToWorld(normalLocal);
    p.velocity -= normalWorld * dot(p.velocity, normalWorld);
    float vLen = length(p.velocity);
    if (vLen > 1e-6f)
    {
        p.velocity *= speed / vLen;
    }

    // ------------------------------------------------------------
    // 5. 寿命と色（既存 UpdateParticle と同じ挙動）
    // ------------------------------------------------------------
    float lifeRatio = saturate(p.currentTime / p.lifeTime);
    float4 lerpedColor = lerp(gStartColor, gEndColor, lifeRatio);
    float alpha = 1.0f - lifeRatio;
    p.color = float4(lerpedColor.rgb, lerpedColor.a * alpha);

    gParticles[particleIndex] = p;

    // ------------------------------------------------------------
    // 6. 寿命切れの返却
    // ------------------------------------------------------------
    if (p.color.a <= 0.001f)
    {
        gParticles[particleIndex].color.a = 0;
        gParticles[particleIndex].scale = float3(0.0f, 0.0f, 0.0f);

        int freeListIndex;
        InterlockedAdd(gFreeListIndex[0], 1, freeListIndex);

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
