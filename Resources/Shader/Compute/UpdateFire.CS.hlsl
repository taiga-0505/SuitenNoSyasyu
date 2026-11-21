// ============================================================================
// UpdateFire.CS.hlsl
// ----------------------------------------------------------------------------
// 炎（ParticleType::Fire）用の更新 Compute Shader。
//
// UpdateParticle.CS との違いは炎らしい動きを足している点:
//   1. 浮力 … gGravity を負の値にすると上向きに加速する（既存と同じ式）
//   2. ゆらぎ … pad0 の乱数を位相にした sin/cos で横方向にゆらす
//   3. 収束 … 高く昇るほど炎の中心軸へ引き戻し、先細りのシルエットを作る
//
// 色は既存と同じ startColor → endColor の遷移＋alpha 減衰なので、
// パーティクルエディタの色設定はそのまま効く。
// 炎の「形」と「白熱した芯」は FireParticle.PS 側で作る。
//
// u0 : gParticles      — パーティクルデータ（RWStructuredBuffer）
// u1 : gFreeListIndex  — FreeList の現在のインデックス
// u2 : gFreeList       — 空きパーティクルインデックスの配列
// b0 : gPerFrame       — エミッタパラメータ
// ============================================================================

struct Particle
{
    float3 translate;   ///< 位置
    float pad0;         ///< 炎ではパーティクル固有の乱数（0〜1）。EmitFire が書き込む
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
    float gGravity;          ///< 重力の強さ（下方向が正。炎では負にして浮力にする）
    uint gEmitterShape;      ///< エミッタ形状（炎では参照しない）

    // 速度（16 bytes）
    float3 gBaseVelocity;    ///< 基本速度（1 フレームあたりの移動量）
    float gVelocityVariance; ///< 速度のランダム分散

    // 形状パラメータ（16 bytes）
    float gShapeRadius;      ///< 火種の円盤の半径（ゆらぎの振幅にも使う）
    float gConeAngle;        ///< Cone の半角（rad。炎では参照しない）
    float2 gShapePad;        ///< パディング

    // 開始色（16 bytes）
    float4 gStartColor;      ///< 開始色（RGBA）

    // 終了色（16 bytes）
    float4 gEndColor;        ///< 終了色（RGBA）

    // エミッタ位置（16 bytes）
    float3 gEmitterPosition; ///< エミッタのワールド座標（収束先の中心軸）
    uint gEmitCount;         ///< 1 フレームの射出数

    // Box 形状サイズ（16 bytes）
    float3 gShapeBoxSize;    ///< Box 形状のサイズ（全長 xyz。炎では参照しない）
    float gShapeBoxPad;      ///< パディング
};

RWStructuredBuffer<Particle> gParticles     : register(u0);
RWStructuredBuffer<int>      gFreeListIndex : register(u1);
RWStructuredBuffer<uint>     gFreeList      : register(u2);

// 炎のゆらぎの速さ（rad/s）。X と Z で素数寄りの比にして周期が揃わないようにする
static const float kSwayFreqX = 7.3f; ///< X 方向のゆらぎの速さ（rad/s）
static const float kSwayFreqZ = 5.9f; ///< Z 方向のゆらぎの速さ（rad/s）

/// @brief 炎の先細り: 中心軸へ引き戻す強さ（1/s）
/// @note 引き戻し先は「現在の」エミッタ位置なので、手を振ると炎の柱ごと
///       手についてくる（＝置いていかれて後ろに残らない）。
///       手のひらの炎としてはこの方が自然なので意図的にこうしている。
///       たなびく炎にしたい場合は発生位置をパーティクルごとに覚える必要があるが、
///       Particle 構造体の空き領域（pad0）は乱数で使い切っている。
static const float kConvergeStrength = 2.6f;

/// @brief 先細りが最大になる高さ（m）の逆数
/// @details 炎の高さは「初速 × 寿命」でおよそ 20cm。
///          その手前で収束が最大になるようにしておくと、
///          上端がきれいに一点へ集まって炎の舌に見える。
static const float kConvergeHeightInv = 6.0f;

/// @brief ゆらぎの速度振幅を火種の半径の何倍にするか（1/s）
/// @details 振幅を gVelocityVariance から取らないのは単位が混ざるため。
///          gVelocityVariance は Emit 側で「1 フレームあたりの移動量」として
///          使われており、こちらの m/s と直接比較できない。
///          火種の半径に比例させておけば、炎を大きくすればゆらぎも大きくなる。
static const float kSwayAmpPerRadius = 3.0f;

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
    // 1. 浮力
    // ------------------------------------------------------------
    // gGravity を負にすると上向きに加速する（＝熱で立ち上がる）。
    // velocity は「1 フレームあたりの移動量」なので、
    // gGravity は 0.05 程度のごく小さい値で十分効く。
    p.velocity.y -= gGravity * gDeltaTime;

    // ------------------------------------------------------------
    // 2. 速度による移動
    // ------------------------------------------------------------
    p.translate += p.velocity;

    // ------------------------------------------------------------
    // 3. 横方向のゆらぎ
    // ------------------------------------------------------------
    // pad0 の乱数を位相にすることで、パーティクルごとに違うタイミングで
    // ゆれる。位相を共有すると炎全体が同じ方向に揺れて「旗」に見える。
    float phase = p.pad0 * 6.28318530f;
    float swayAmp = gShapeRadius * kSwayAmpPerRadius;
    p.translate.x += sin(p.currentTime * kSwayFreqX + phase) * swayAmp * gDeltaTime;
    p.translate.z += cos(p.currentTime * kSwayFreqZ + phase * 1.7f) * swayAmp * gDeltaTime;

    // ------------------------------------------------------------
    // 4. 中心軸への収束（先細りのシルエット）
    // ------------------------------------------------------------
    // 根元は広く、上へ行くほど炎の芯へ集めることで円錐状の輪郭になる。
    // 引き戻しを高さで強めないと、上に昇るほど散って煙のように見える。
    float heightAbove = max(0.0f, p.translate.y - gEmitterPosition.y);
    float converge = saturate(heightAbove * kConvergeHeightInv) * kConvergeStrength * gDeltaTime;
    p.translate.x += (gEmitterPosition.x - p.translate.x) * converge;
    p.translate.z += (gEmitterPosition.z - p.translate.z) * converge;

    // ------------------------------------------------------------
    // 5. 寿命と色（既存 UpdateParticle と同じ挙動）
    // ------------------------------------------------------------
    p.currentTime += gDeltaTime;

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
