#include "ECS/ScriptableEntity.h"
#include "ECS/ScriptRegistry.h"
#include "ECS/TransformComponent.h"
#include "ECS/ColliderComponent.h"
#include "ECS/NativeScriptComponent.h"
#include "ECS/PrimitiveMeshComponent.h"
#include "ECS/CameraComponent.h"
#include "RenderCommon.h"
#include "Render/Systems/RenderInteractiveWater.h"
#include "Scene.h"
#include "Common/EngineConfig.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#if RC_ENABLE_IMGUI
#include "imgui/imgui.h"
#endif

/// @brief 自機弾が水中を進むときの調整値（全自機弾で共通）
/// @details 弾は実行時にコードで作られシーン JSON に保存されないため、値はここの既定値で決める。
///          Debug / Development では弾エンティティの Inspector から実行中に調整できる（終了すると戻る）。
///          不変条件の無い設定値の束なので struct のままにしている。
struct UnderwaterBulletTuning {
    float drag = 2.5f;            ///< 水の抵抗（毎秒 velocity *= exp(-drag)）。大きいほど水中で早く止まる
    float gravityScale = 0.3f;    ///< 水中での重力の倍率（浮力のぶん弱める）
    float minSpeed = 6.0f;        ///< この速さを下回ったら泡になって消える
    float bubbleInterval = 0.04f; ///< 水中の軌跡に泡を出す間隔（秒）
    int burstBubbles = 10;        ///< 水中で消える・当たるときに弾ける泡の数
};

namespace {

/// @brief 泡を出す（大きさは base〜base+0.1 で少しずつ変える）
/// @param pos 出す位置（ワールド座標）
/// @param count 個数
/// @param baseScale 最小の大きさ
void SpawnBubbleParticles(const RC::Vector3& pos, int count, float baseScale) {
    for (int i = 0; i < count; ++i) {
        RC::EffectParticleSpawn bubble;
        bubble.kind = RC::EffectParticleKind::Bubble;
        bubble.position = pos;
        bubble.scale = baseScale + (i % 3) * 0.05f;
        bubble.color = { 0.8f, 0.9f, 1.0f, 0.6f };
        RC::SpawnEffectParticle(bubble);
    }
}

/// @brief 水面に波を加える（ワールド座標 -50〜50 を水面 UV 0〜1 に変換）
/// @param pos 波を立てる位置（ワールド座標）
/// @param radius 波の半径（UV 単位）
/// @param strength 波の強さ
void AddSurfaceWave(const RC::Vector3& pos, float radius, float strength) {
    RC::WaveSource source;
    source.uv = RC::Vector2((pos.x / 100.0f) + 0.5f, (pos.z / 100.0f) + 0.5f);
    source.radius = radius;
    source.strength = strength;
    RC::AddWaveSource(source);
}

// ----------------------------------------------------------------------------
// 弾種（bullet_type タグ）ごとの振る舞い
// ----------------------------------------------------------------------------

/// @brief 弾種（bullet_type タグ 0:通常 / 1:拡散 / 2:重い弾）ごとの違いをまとめた基底クラス
/// @details 以前は bulletType を文字列で持ち、ダメージ・重力・飛ばし方・着弾エフェクトの各所で
///          `if (bulletType == "heavy")` のように分岐していた。弾種を足すと分岐を全部探して
///          直す必要があったため、ポリモーフィズムで「弾種 1 つ = クラス 1 つ」にまとめた。
///          弾は状態を持たない（ダメージ等の値は WaterBullet 側に書き込む）ので、
///          各弾種は共有の const インスタンスを使い回す（Flyweight 的な使い方）。
///          既定の実装は「通常弾」の振る舞い。派生は違うところだけを override する。
class BulletKind {
public:
    virtual ~BulletKind() = default;

    /// @brief 弾種ごとのダメージ・重力・既定速度を反映する
    /// @param damage [in,out] 与えるダメージ
    /// @param gravity [in,out] 重力加速度（負で下向き）
    /// @param baseSpeed [in,out] 初速。速度タグが無いときだけ弾種の既定値で上書きする
    /// @param hasSpeedTag bullet_speed タグで速度が指定されているか
    /// @details 通常弾は何も書き換えない（以前の実装と同じ。プールで再利用された弾は
    ///          前回の弾種の値を引き継ぐ点も含めて挙動を変えていない）。
    virtual void ApplyStats(int& damage, float& gravity, float& baseSpeed, bool hasSpeedTag) const {
        (void)damage;
        (void)gravity;
        (void)baseSpeed;
        (void)hasSpeedTag;
    }

    /// @brief dir_x/y/z タグから初速を決める
    /// @param self 弾のエンティティ（タグの読み出し元）
    /// @param isPlayerBullet 自機の弾か
    /// @param baseSpeed 初速の大きさ
    /// @param outVelocity [out] 決めた初速（決められなかったときは書き換えない）
    /// @return 初速を決めたら true
    /// @details 通常弾・重い弾は dir_x/y/z（1000 倍の整数、-9999 は未指定）を正規化した向きへ飛ばす。
    virtual bool AimFromTags(const Entity& self, bool isPlayerBullet, float baseSpeed, RC::Vector3& outVelocity) const {
        (void)isPlayerBullet;
        const int dirX = self.GetTagInt("dir_x", -9999);
        const int dirY = self.GetTagInt("dir_y", -9999);
        const int dirZ = self.GetTagInt("dir_z", -9999);
        if (dirX == -9999 || dirY == -9999 || dirZ == -9999) return false;

        float dx = static_cast<float>(dirX) / 1000.0f;
        float dy = static_cast<float>(dirY) / 1000.0f;
        float dz = static_cast<float>(dirZ) / 1000.0f;
        float len = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (len > 0.01f) { dx /= len; dy /= len; dz /= len; }
        else { dx = 0.0f; dy = 0.0f; dz = 1.0f; }
        outVelocity = { dx * baseSpeed, dy * baseSpeed, dz * baseSpeed };
        return true;
    }

    /// @brief 水中で消えるときに弾ける泡の倍率
    /// @return s_underwater.burstBubbles に掛ける倍率
    virtual int BurstBubbleMultiplier() const { return 1; }

    /// @brief 水上で消えるときに添える泡の数
    /// @return 泡の数
    virtual int SurfaceBubbleCount() const { return 4; }

    /// @brief 水面の波と水しぶきを出す（泡は出さない）
    /// @param pos 出す位置（ワールド座標）
    /// @details 通常弾は小さな波と、大きさ・色を少しずつ変えた水しぶきを 12 粒散らす。
    ///          水しぶき・泡・水柱は Entity を作らず、エンジンのエフェクト粒として出す。
    ///          （以前は 1 粒ごとに Entity＋専用メッシュ＋スクリプトを作っており、
    ///            弾を撃つと数百個の Entity が増えて更新・描画・エディタ表示が重くなっていた）
    virtual void SpawnSurfaceEffect(const RC::Vector3& pos) const {
        AddSurfaceWave(pos, 0.05f, 0.5f);
        const int splashCount = 12;
        for (int i = 0; i < splashCount; ++i) {
            RC::EffectParticleSpawn splash;
            splash.kind = RC::EffectParticleKind::Splash;
            splash.position = pos;
            splash.scale = 0.15f + (i % 4) * 0.05f;
            const float r = 0.3f + (i % 3) * 0.15f;
            const float g = 0.6f + (i % 2) * 0.2f;
            splash.color = { r, g, 1.0f, 0.85f };
            splash.impactFactor = 1.0f;
            RC::SpawnEffectParticle(splash);
        }
    }

protected:
    /// @brief 基底だけを直接作らせない（必ず派生の弾種を使う）
    BulletKind() = default;
};

/// @brief 通常弾（bullet_type = 0）。基底の既定の振る舞いそのもの
class NormalBulletKind final : public BulletKind {};

/// @brief 拡散弾（bullet_type = 1）。少し軽く、水平方向へ山なりに飛ぶ
class SpreadBulletKind final : public BulletKind {
public:
    /// @brief ダメージ 1・重力 -2.5、速度タグが無ければ初速 35
    void ApplyStats(int& damage, float& gravity, float& baseSpeed, bool hasSpeedTag) const override {
        damage = 1;
        gravity = -2.5f;
        if (!hasSpeedTag) baseSpeed = 35.0f;
    }

    /// @brief 自機の拡散弾は向きの水平成分だけを使い、上向き 1.5 を与える
    /// @details 敵弾の拡散は向きを決めない（以前の実装どおり。ShipEnemyScript の D-13 参照）。
    bool AimFromTags(const Entity& self, bool isPlayerBullet, float baseSpeed, RC::Vector3& outVelocity) const override {
        if (!isPlayerBullet) return false;
        float dx = static_cast<float>(self.GetTagInt("dir_x", 0)) / 1000.0f;
        float dz = static_cast<float>(self.GetTagInt("dir_z", 1000)) / 1000.0f;
        // 念のため正規化
        float len = std::sqrt(dx * dx + dz * dz);
        if (len > 0.01f) { dx /= len; dz /= len; }
        else { dx = 0.0f; dz = 1.0f; }
        outVelocity = { dx * baseSpeed, 1.5f, dz * baseSpeed };
        return true;
    }
};

/// @brief 重い弾（bullet_type = 2）。高ダメージで遅く、速く落ち、着弾で水柱が立つ
class HeavyBulletKind final : public BulletKind {
public:
    /// @brief ダメージ 3・重力 -6（重いので速く落ちる）、速度タグが無ければ初速 25（重いので遅い）
    void ApplyStats(int& damage, float& gravity, float& baseSpeed, bool hasSpeedTag) const override {
        damage = 3;
        gravity = -6.0f;
        if (!hasSpeedTag) baseSpeed = 25.0f;
    }

    /// @brief 水中で消えるときは泡を 2 倍出す
    int BurstBubbleMultiplier() const override { return 2; }

    /// @brief 水上で消えるときは泡を 8 個添える
    int SurfaceBubbleCount() const override { return 8; }

    /// @brief 大きく強い波と、水柱を 1 本出す（水しぶきの粒は出さない）
    void SpawnSurfaceEffect(const RC::Vector3& pos) const override {
        AddSurfaceWave(pos, 0.1f, 1.5f);
        // 水柱：太さ 1.5 の平たい状態から一気に伸びる（旧 HeavySplash の impact_factor = 150）
        RC::EffectParticleSpawn column;
        column.kind = RC::EffectParticleKind::HeavySplash;
        column.position = pos;
        column.scale = 1.5f;
        column.color = { 0.9f, 0.95f, 1.0f, 1.0f };
        column.impactFactor = 1.5f;
        RC::SpawnEffectParticle(column);
    }
};

/// @brief 通常弾の共有インスタンス
/// @return 通常弾
const BulletKind& NormalKind() {
    static const NormalBulletKind kKind{};
    return kKind;
}

/// @brief 拡散弾の共有インスタンス
/// @return 拡散弾
const BulletKind& SpreadKind() {
    static const SpreadBulletKind kKind{};
    return kKind;
}

/// @brief 重い弾の共有インスタンス
/// @return 重い弾
const BulletKind& HeavyKind() {
    static const HeavyBulletKind kKind{};
    return kKind;
}

/// @brief bullet_type タグの値から弾種を引く（Factory Method）
/// @param tag bullet_type タグの値（0:normal / 1:spread / 2:heavy）
/// @return 弾種。未知の値なら nullptr（呼び出し側は前回の弾種のままにする。以前の実装と同じ）
/// @details タグ（int）→ 振る舞いクラスへの変換をここ 1 か所に閉じ込め、
///          WaterBullet 本体からは弾種の番号を見えなくするために置いた。
const BulletKind* FindBulletKind(int tag) {
    if (tag == 0) return &NormalKind();
    if (tag == 1) return &SpreadKind();
    if (tag == 2) return &HeavyKind();
    return nullptr;
}

// ----------------------------------------------------------------------------
// 撃ち手の陣営（自機の弾 / 敵の弾）ごとの振る舞い
// ----------------------------------------------------------------------------

/// @brief 弾の撃ち手の陣営（自機 / 敵）ごとの違いをまとめた基底クラス
/// @details 以前は `isPlayerBullet` の bool で、当たる相手・水面での扱い・撃ち落とし・スコア加算を
///          あちこちで if 分岐していた。陣営ごとの規則を 1 クラスにまとめ、ポリモーフィズムで呼ぶ。
///          陣営はエンティティ名（"PlayerBullet" かどうか）で決まる。
class BulletFaction {
public:
    virtual ~BulletFaction() = default;

    /// @brief 命中したらダメージを与える相手か
    /// @param other 調べる相手
    /// @return ダメージ対象なら true
    virtual bool IsDamageTarget(const Entity& other) const = 0;

    /// @brief 宝箱などのアイテム（is_item）にも当たるか
    /// @return 当たるなら true
    virtual bool HitsItems() const = 0;

    /// @brief 水面（y = 0）を越えて進めるか
    /// @return 進めるなら true（false なら着水で消える）
    virtual bool PassesWaterSurface() const = 0;

    /// @brief 自機の弾で撃ち落とされる弾か
    /// @param self 弾自身のエンティティ（"shootable" タグを見る）
    /// @return 撃ち落とされるなら true
    virtual bool IsShootable(const Entity& self) const = 0;

    /// @brief ダメージ対象に当てたらスコアを加算するか
    /// @return 加算するなら true
    virtual bool ScoresOnHit() const = 0;

    /// @brief 当たり判定の対象か（ダメージ対象・地形・アイテム）
    /// @param other 調べる相手
    /// @return 対象なら true
    bool IsCollisionTarget(const Entity& other) const {
        const bool isTerrainEntity = (other.GetTagInt("is_terrain", 0) == 1);
        return IsDamageTarget(other) || isTerrainEntity || (HitsItems() && other.GetTagInt("is_item", 0) == 1);
    }

protected:
    /// @brief 基底だけを直接作らせない
    BulletFaction() = default;
};

/// @brief 自機の弾（"PlayerBullet"）
class PlayerBulletFaction final : public BulletFaction {
public:
    /// @brief 敵（is_enemy）に当たる
    bool IsDamageTarget(const Entity& other) const override { return other.GetTagInt("is_enemy", 0) == 1; }
    /// @brief 宝箱などのアイテムにも当たる（ポイント加算はアイテム側が行う）
    bool HitsItems() const override { return true; }
    /// @brief 水面を越えてしぶきを上げ、そのまま進む
    bool PassesWaterSurface() const override { return true; }
    /// @brief 自機の弾は撃ち落とされない
    bool IsShootable(const Entity& self) const override {
        (void)self;
        return false;
    }
    /// @brief 敵に当てたらスコア +1
    bool ScoresOnHit() const override { return true; }
};

/// @brief 敵の弾（"EnemyBullet" など、PlayerBullet 以外）
class EnemyBulletFaction final : public BulletFaction {
public:
    /// @brief プレイヤー（is_player）に当たる
    bool IsDamageTarget(const Entity& other) const override { return other.GetTagInt("is_player", 0) == 1; }
    /// @brief アイテムには当たらない
    bool HitsItems() const override { return false; }
    /// @brief 着水で消える（砲弾が水面で弾ける演出を残す）
    bool PassesWaterSurface() const override { return false; }
    /// @brief `shootable` の付いた敵弾（船の砲弾）は撃ち落とせる
    bool IsShootable(const Entity& self) const override { return self.GetTagInt("shootable", 0) == 1; }
    /// @brief 敵弾が当たってもスコアは増えない
    bool ScoresOnHit() const override { return false; }
};

/// @brief 弾の陣営を名前から決める
/// @param self 弾のエンティティ（nullptr なら敵の弾として扱う。以前の実装と同じ）
/// @return 陣営の共有インスタンス
const BulletFaction& FactionOf(const Entity* self) {
    static const PlayerBulletFaction kPlayer{};
    static const EnemyBulletFaction kEnemy{};
    if (self && self->GetName() == "PlayerBullet") return kPlayer;
    return kEnemy;
}

} // namespace

/// @brief 水弾の飛行・命中判定・着弾演出を行うスクリプト
/// @details タグ（bullet_type / bullet_speed / dir_* 等）から初期速度を決め、重力付きで飛ばす。
///          移動線分のレイキャストで命中を判定し、消滅時はエンティティを破棄せず非アクティブ化してプールに戻す。
///          自機弾は水面を越えても消えず、しぶきを出して水中（または水上）へ進む。空中から飛び込んだ弾は
///          抵抗で減速し、泡の軌跡を残して、遅くなったら泡になって消える（水中で撃った弾は減速しない）。
///          敵弾は従来どおり水面で消える。
///          弾種ごとの違いは BulletKind、撃ち手の陣営ごとの違いは BulletFaction に分けている。
///
///          生存中 / 消滅済み・空中 / 水中は bool フラグのままにしている（State パターンにしない）。
///          消滅は Die() で非アクティブ化した瞬間にプールへ戻り、次の再利用で reused タグから
///          全部リセットされるだけの一時的な状態で、遷移も「飛行 → 消滅」の一方向しかない。
///          空中 / 水中も物理の係数が変わるだけなので、状態クラスに分けると却って追いにくくなるため。
class WaterBullet : public ScriptableEntity {
protected:
    /// @brief 弾を消滅させる（現在位置に水しぶきを出して非アクティブ化）
    /// @note 二重に呼ばれても 1 回だけ処理する
    void Die() {
        if (isDead_) return;
        isDead_ = true;
        if (Entity* self = GetEntity()) {
            if (auto* tr = self->GetComponent<TransformComponent>()) {
                SpawnSplash(tr->position);
            }
            // エンティティを破棄せず、非アクティブにしてプール（再利用）する
            self->SetActive(false);
            initialized_ = false;
        }
    }

    /// @brief 経過時間を初期化し、未付与なら球のトリガーコライダーを付ける
    void OnCreate() override {
        elapsed_ = 0.0f;
        // ColliderComponent が無ければ半径 1 の球コライダーを付与する
        Entity* self = GetEntity();
        if (self && !self->HasComponent<ColliderComponent>()) {
            auto* col = &self->AddComponent<ColliderComponent>();
            col->shape = ColliderComponent::Shape::Sphere;
            col->radius = 1.0f;
            col->isTrigger = true; // 物理反射させずイベントだけ取る
        }
    }

    /// @brief 初回に速度を決定し、重力付きで移動・命中判定・撃ち落とし判定・寿命／着水判定を行う
    /// @param deltaTime 前フレームからの経過時間（秒）
    void OnUpdate(float deltaTime) override {
        Entity* self = GetEntity();
        // プールから再利用された弾（reused タグ付き）は状態をリセットして撃ち直しに備える
        if (self && self->GetTagInt("reused", 0) == 1) {
            elapsed_ = 0.0f;
            markedForDestroy_ = false;
            isDead_ = false;
            initialized_ = false;
            velocity_ = {0.0f, 0.0f, 0.0f};
            inWater_ = false;
            bubbleTimer_ = 0.0f;
            self->ClearTag("reused");
        }

        if (markedForDestroy_ || isDead_) return;

        auto* tr = GetComponent<TransformComponent>();
        if (!tr) return;

        Scene* scene = GetScene();
        if (!scene) return;

        // 初回更新時に、エンティティ名とタグから弾種・速度・寿命・飛行方向を決める
        if (!initialized_) {
            if (self) {
                InitializeFromTags(*self, tr->position);
            }
            // 水中から撃った弾（自機が潜っているとき）は従来どおり減速させない。
            // 減速するのは「空中から水に飛び込んだ」弾だけ（水中戦の射程を変えないため）
            inWater_ = false;
            bubbleTimer_ = 0.0f;
            initialized_ = true;
        }

        const BulletFaction& faction = FactionOf(self);

        // === 物理（重力を加えて移動。水中では抵抗で減速し、重力も弱まる） ===
        RC::Vector3 oldPos = tr->position;
        if (inWater_) {
            const float k = std::exp(-s_underwater.drag * deltaTime);
            velocity_.x *= k;
            velocity_.y *= k;
            velocity_.z *= k;
            velocity_.y += gravity_ * s_underwater.gravityScale * deltaTime;
        } else {
            velocity_.y += gravity_ * deltaTime;
        }
        tr->position.x += velocity_.x * deltaTime;
        tr->position.y += velocity_.y * deltaTime;
        tr->position.z += velocity_.z * deltaTime;

        // 連続衝突判定（CCD）：高速な弾のすり抜けを防ぐため、前フレーム位置→現在位置のレイで最も手前の命中を探す
        RC::Vector3 diff = { tr->position.x - oldPos.x, tr->position.y - oldPos.y, tr->position.z - oldPos.z };
        float distSq = diff.x * diff.x + diff.y * diff.y + diff.z * diff.z;
        if (distSq > 0.001f) {
            float dist = std::sqrt(distSq);
            RC::Ray ray;
            ray.origin = oldPos;
            ray.direction = { diff.x / dist, diff.y / dist, diff.z / dist };

            float closestDist = dist;
            Entity* hitEntity = nullptr;
            RC::Vector3 hitPoint = tr->position;

            // 候補は BVH から引く（以前は弾 1 発ごとに全エンティティを走査しており、
            // 弾 20 発で毎フレーム数 ms かかっていた）。範囲は今回の移動線分を、
            // 自分の半径と余裕（同じフレームで相手が少し動いた分）だけ広げた箱。
            float myRadius = 0.0f;
            if (auto* myCol = GetComponent<ColliderComponent>()) {
                myRadius = myCol->radius * (std::max)((std::max)(std::abs(tr->scale.x), std::abs(tr->scale.y)), std::abs(tr->scale.z));
            }
            const float pad = myRadius + 1.0f;
            const RC::BoundingBox sweepBox = {
                {(std::min)(oldPos.x, tr->position.x) - pad, (std::min)(oldPos.y, tr->position.y) - pad,
                 (std::min)(oldPos.z, tr->position.z) - pad},
                {(std::max)(oldPos.x, tr->position.x) + pad, (std::max)(oldPos.y, tr->position.y) + pad,
                 (std::max)(oldPos.z, tr->position.z) + pad}};
            static std::vector<Entity*> s_candidates; // 使い回して毎フレームの確保をなくす
            s_candidates.clear();
            scene->QueryColliderCandidates(sweepBox, [](Entity* c) { s_candidates.push_back(c); });

            for (Entity* e : s_candidates) {
                if (e == self || !e->IsActive() || e->IsPendingDestroy()) continue;

                const std::string& eName = e->GetName();
                // 弾やパーティクルなど、確実に当たり判定対象外のものはGetComponentやGetTagの前に除外する（超高速化）
                if (eName == "Splash" || eName == "Bubble" || eName == "HeavySplash" ||
                    eName == "PlayerBullet" || eName == "EnemyBullet" || eName == "Wake" ||
                    eName == "Effects" || eName == "PlayerBullets" || eName == "EnemyBullets") {
                    continue;
                }

                auto* eCol = e->GetComponent<ColliderComponent>();
                auto* eTr = e->GetComponent<TransformComponent>();
                if (!eCol || !eCol->IsEnabled() || !eTr) continue;

                // 疎結合: タグで当たり判定対象かチェック（対象は陣営ごとに違う）
                if (!faction.IsCollisionTarget(*e)) continue;

                // 相手コライダーを自分の半径ぶん膨らませ、レイ（点）との交差判定に置き換える
                RC::Vector3 scaledCenter = {
                    eCol->center.x * eTr->scale.x,
                    eCol->center.y * eTr->scale.y,
                    eCol->center.z * eTr->scale.z
                };
                RC::Vector3 center = RC::Add(eTr->position, scaledCenter);

                float t = -1.0f;
                bool hit = false;
                if (eCol->shape == ColliderComponent::Shape::Sphere) {
                    float r = eCol->radius * (std::max)((std::max)(std::abs(eTr->scale.x), std::abs(eTr->scale.y)), std::abs(eTr->scale.z));
                    auto* myCol = GetComponent<ColliderComponent>();
                    if (myCol) {
                        float myR = myCol->radius * (std::max)((std::max)(std::abs(tr->scale.x), std::abs(tr->scale.y)), std::abs(tr->scale.z));
                        r += myR;
                    }
                    hit = RC::IntersectRaySphere(ray, center, r, t);
                } else if (eCol->shape == ColliderComponent::Shape::AABB) {
                    RC::Vector3 h = { std::abs(eCol->size.x * eTr->scale.x * 0.5f), std::abs(eCol->size.y * eTr->scale.y * 0.5f), std::abs(eCol->size.z * eTr->scale.z * 0.5f) };
                    auto* myCol = GetComponent<ColliderComponent>();
                    if (myCol) {
                        float myR = myCol->radius * (std::max)((std::max)(std::abs(tr->scale.x), std::abs(tr->scale.y)), std::abs(tr->scale.z));
                        h.x += myR; h.y += myR; h.z += myR;
                    }
                    RC::Vector3 minBox = RC::Sub(center, h);
                    RC::Vector3 maxBox = RC::Add(center, h);
                    hit = RC::IntersectRayAABB(ray, minBox, maxBox, t);
                }

                // 移動距離内で最も近い命中を採用する
                if (hit && t >= 0.0f && t <= closestDist) {
                    closestDist = t;
                    hitEntity = e;
                    hitPoint = RC::Add(ray.origin, RC::Mul(ray.direction, t));
                }
            }

            // 命中点へ位置を戻してから衝突処理を行う
            if (hitEntity) {
                // 水面を越えたのと同じフレームで当たった場合も、先に水面のしぶきを出す
                if (faction.PassesWaterSurface() && ((oldPos.y > 0.0f) != (hitPoint.y > 0.0f))) {
                    const float denom = oldPos.y - hitPoint.y;
                    const float t = (std::abs(denom) > 1e-6f) ? (oldPos.y / denom) : 0.0f;
                    SpawnSurfaceSplash({oldPos.x + (hitPoint.x - oldPos.x) * t, 0.0f,
                                        oldPos.z + (hitPoint.z - oldPos.z) * t});
                }
                tr->position = hitPoint;
                OnCollision(hitEntity, hitPoint);
                if (isDead_) return;
            }
        }

        // 撃ち落とし：`shootable` の付いた敵弾（船の砲弾）は、自機の弾が近くを通ると両方消える
        if (faction.IsShootable(*self)) {
            if (TryShootDown(scene, self, oldPos, tr->position)) return;
        }

        // 寿命切れで消滅
        elapsed_ += deltaTime;
        if (elapsed_ >= lifetime_) {
            Die();
            return;
        }

        // 水面（y = 0）をどちら向きにでも跨いだとき
        const bool crossedDown = (oldPos.y > 0.0f && tr->position.y <= 0.0f);
        const bool crossedUp = (oldPos.y < 0.0f && tr->position.y >= 0.0f);
        if (crossedDown || crossedUp) {
            if (!faction.PassesWaterSurface()) {
                // 敵弾は従来どおり着水で消滅（砲弾が水面で弾ける演出を残す）
                Die();
                return;
            }
            // 自機弾は水面を越えた点でしぶきを上げ、そのまま進む
            const float denom = oldPos.y - tr->position.y;
            const float t = (std::abs(denom) > 1e-6f) ? (oldPos.y / denom) : 0.0f;
            const RC::Vector3 surfacePos = {
                oldPos.x + (tr->position.x - oldPos.x) * t,
                0.0f,
                oldPos.z + (tr->position.z - oldPos.z) * t};
            SpawnSurfaceSplash(surfacePos);
            inWater_ = crossedDown;
            if (inWater_) SpawnBubbleParticles(surfacePos, 3, 0.04f); // 潜り込んだ瞬間の泡
        }

        // 飛び込んだ弾：泡の軌跡を残し、遅くなったら泡になって消える
        if (inWater_) {
            bubbleTimer_ += deltaTime;
            if (bubbleTimer_ >= s_underwater.bubbleInterval) {
                bubbleTimer_ = 0.0f;
                SpawnBubbleParticles(tr->position, 1, 0.05f);
            }
            const float speedSq = velocity_.x * velocity_.x + velocity_.y * velocity_.y + velocity_.z * velocity_.z;
            if (speedSq < s_underwater.minSpeed * s_underwater.minSpeed) {
                Die();
                return;
            }
        }
    }

public:
    /// @brief 水中を進む自機弾の調整値を Inspector に出す（値は全自機弾で共通）
    void OnImGui() override {
#if RC_ENABLE_IMGUI
        ImGui::TextUnformatted("Underwater (shared by all PlayerBullets)");
        ImGui::DragFloat("Drag##WaterBullet", &s_underwater.drag, 0.05f, 0.0f, 20.0f);
        ImGui::DragFloat("Gravity Scale##WaterBullet", &s_underwater.gravityScale, 0.01f, 0.0f, 2.0f);
        ImGui::DragFloat("Min Speed##WaterBullet", &s_underwater.minSpeed, 0.1f, 0.0f, 40.0f);
        ImGui::DragFloat("Bubble Interval##WaterBullet", &s_underwater.bubbleInterval, 0.005f, 0.005f, 1.0f);
        ImGui::DragInt("Burst Bubbles##WaterBullet", &s_underwater.burstBubbles, 1.0f, 0, 40);
        ImGui::Text("InWater: %s", inWater_ ? "yes" : "no");
#endif
    }

    /// @brief 命中相手の種類（アイテム／敵・プレイヤー／地形）に応じてダメージやスコアを積み、弾を消滅させる
    /// @param other 命中した相手エンティティ
    /// @param contactPoint 命中位置（ワールド座標、ログ出力に使用）
    /// @note ダメージは相手の "pending_damage" タグ、スコアはカメラの "score_add" タグに加算して通知する
    void OnCollision(Entity* other, const RC::Vector3& contactPoint = {}) override {
        if (isDead_ || !other) return;

        Entity* self = GetEntity();
        std::string myName = self ? self->GetName() : "";
        const BulletFaction& faction = FactionOf(self);
        std::string targetName = other->GetName();

        // 当たった瞬間をログで確認
        printf("[Collision] %s hit %s at (%.2f, %.2f, %.2f)\n",
               myName.c_str(), targetName.c_str(),
               contactPoint.x, contactPoint.y, contactPoint.z);

        // 当たり判定対象の確認 (タグによる疎結合化。対象は陣営ごとに違う)
        bool hitTarget = faction.IsDamageTarget(*other);
        bool hitTerrain = (other->GetTagInt("is_terrain", 0) == 1);
        // 宝箱などのアイテム。ダメージだけ積み、ポイント加算はアイテム側（TreasureChestScript）が行う
        bool hitItem = (faction.HitsItems() && (other->GetTagInt("is_item", 0) == 1));

        if (hitItem) {
            int pendingDmg = other->GetTagInt("pending_damage", 0);
            other->SetTag("pending_damage", pendingDmg + damage_);
            Die();
        } else if (hitTarget) {
            // 相手の pending_damage に加算（実際の減算は相手側スクリプトが行う）
            int pendingDmg = other->GetTagInt("pending_damage", 0);
            other->SetTag("pending_damage", pendingDmg + damage_);

            // 敵に当てたらスコア +1（カメラに score_add を積む。毎回の全走査を避けるためカメラをキャッシュ）
            if (faction.ScoresOnHit()) {
                std::shared_ptr<Entity> cam = cachedCamera_.lock();
                if (!cam || cam->IsPendingDestroy() || !cam->IsActive()) {
                    if (Scene* scene = GetScene()) {
                        for (auto& pe : scene->GetEntities()) {
                            if (pe->HasComponent<CameraComponent>()) {
                                cam = pe;
                                cachedCamera_ = cam;
                                break;
                            }
                        }
                    }
                }
                if (cam) {
                    int scoreAdd = cam->GetTagInt("score_add", 0);
                    cam->SetTag("score_add", scoreAdd + 1);
                }
            }
            Die();
        } else if (hitTerrain) {
            // 地形に当たって消滅（エフェクトは Die() 内で出す）
            Die();
        }
    }

private:
    static inline UnderwaterBulletTuning s_underwater; ///< 水中の挙動の調整値（全自機弾で共通。Inspector から調整）

    RC::Vector3 velocity_ = { 0.0f, 0.0f, 0.0f }; ///< 現在の速度（m/s）
    float lifetime_ = 3.0f;                        ///< 寿命（秒）。bullet_lifetime タグで上書き
    float gravity_ = -3.0f;                        ///< 重力加速度（弾種で変わる）
    int damage_ = 1;                               ///< 命中で与えるダメージ（弾種で変わる）
    bool initialized_ = false;                     ///< タグから初期化済みか
    const BulletKind* kind_ = &NormalKind();       ///< 弾種（bullet_type タグから決まる振る舞い）
    bool isDead_ = false;                          ///< 消滅済みか（プールへ戻した）
    float elapsed_ = 0.0f;
    bool markedForDestroy_ = false;
    bool inWater_ = false;      ///< 空中から水に飛び込んで減速中か（自機弾のみ。水中で撃った弾は false のまま）
    float bubbleTimer_ = 0.0f;  ///< 水中の泡の軌跡を出すためのタイマー
    std::weak_ptr<Entity> cachedCamera_;

    /// @brief エンティティ名とタグから弾種・速度・寿命・飛行方向を決める（初回更新時）
    /// @param self 弾のエンティティ
    /// @param myPos 弾の現在位置（狙いの基準）
    void InitializeFromTags(Entity& self, const RC::Vector3& myPos) {
        std::string name = self.GetName();
        // 弾種（0:normal / 1:spread / 2:heavy）。未知の値なら前回の弾種のまま
        if (const BulletKind* kind = FindBulletKind(self.GetTagInt("bullet_type", 0))) {
            kind_ = kind;
        }

        // タグは整数しか持てないため、速度・寿命は 10 倍した値で受け取り 1/10 して使う
        float baseSpeed = 40.0f;
        int speedTag = self.GetTagInt("bullet_speed", 0);
        if (speedTag > 0) baseSpeed = static_cast<float>(speedTag) / 10.0f;

        // 弾種ごとのダメージ・重力・既定速度（速度タグ指定があればそちらを優先）
        kind_->ApplyStats(damage_, gravity_, baseSpeed, speedTag != 0);

        int lifeTag = self.GetTagInt("bullet_lifetime", 0);
        if (lifeTag > 0) lifetime_ = static_cast<float>(lifeTag) / 10.0f;

        // 飛行方向は弾種ごとの規則で dir_x/y/z タグから決める。
        // 決まらなかった自機弾（方向未指定）は最寄りの "Enemy" を狙う
        if (name == "PlayerBullet" || name == "EnemyBullet") {
            const bool isPlayerBullet = (name == "PlayerBullet");
            const bool aimed = kind_->AimFromTags(self, isPlayerBullet, baseSpeed, velocity_);
            if (!aimed && isPlayerBullet && velocity_.x == 0.0f && velocity_.z == 0.0f) {
                SetVelocityTowardTarget("Enemy", baseSpeed, "player", myPos);
            }
        }
    }

    /// @brief このフレームの移動線分（from→to）の近くに自機の弾があれば、両方を消す
    /// @details 砲弾と自機の弾はどちらも速いので、点どうしの距離ではなく
    ///          自分の移動線分との距離で見る（すれ違いの取りこぼし防止）。
    /// @param scene 自機の弾を探すシーン
    /// @param self 自分（敵弾）のエンティティ。"shoot_radius" タグ（100 倍の整数、既定 120）で判定半径を決める
    /// @param from このフレームの移動開始位置
    /// @param to このフレームの移動後の位置
    /// @return 撃ち落とされたら true（自分は Die 済み）
    bool TryShootDown(Scene* scene, Entity* self, const RC::Vector3& from, const RC::Vector3& to) {
        const float radius = static_cast<float>(self->GetTagInt("shoot_radius", 120)) / 100.0f;
        if (radius <= 0.0f) return false;
        const RC::Vector3 seg = { to.x - from.x, to.y - from.y, to.z - from.z };
        const float segLenSq = seg.x * seg.x + seg.y * seg.y + seg.z * seg.z;

        for (auto& e : scene->GetEntities()) {
            if (!e || !e->IsActive() || e->IsPendingDestroy()) continue;
            if (e->GetName() != "PlayerBullet") continue;
            auto* ptr = e->GetComponent<TransformComponent>();
            if (!ptr) continue;

            // 自機の弾から線分への最近点（t を 0〜1 にクランプ）を求め、距離が半径以内か判定する
            float t = 0.0f;
            if (segLenSq > 1e-6f) {
                t = ((ptr->position.x - from.x) * seg.x + (ptr->position.y - from.y) * seg.y +
                     (ptr->position.z - from.z) * seg.z) / segLenSq;
                t = (std::max)(0.0f, (std::min)(1.0f, t));
            }
            const float dx = ptr->position.x - (from.x + seg.x * t);
            const float dy = ptr->position.y - (from.y + seg.y * t);
            const float dz = ptr->position.z - (from.z + seg.z * t);
            if (dx * dx + dy * dy + dz * dz > radius * radius) continue;

            // 自機の弾はプールへ戻す（再利用時に reused タグで初期化される）
            e->SetActive(false);

            // スコア加算（墨玉の撃ち落としと同じく +1）
            std::shared_ptr<Entity> cam = cachedCamera_.lock();
            if (!cam || cam->IsPendingDestroy() || !cam->IsActive()) {
                cam = nullptr;
                for (auto& pe : scene->GetEntities()) {
                    if (pe && pe->HasComponent<CameraComponent>()) { cam = pe; cachedCamera_ = pe; break; }
                }
            }
            if (cam) cam->SetTag("score_add", cam->GetTagInt("score_add", 0) + 1);

            // 撃ち落とした位置で水しぶき
            if (auto* tr = GetComponent<TransformComponent>()) {
                tr->position = { from.x + seg.x * t, from.y + seg.y * t, from.z + seg.z * t };
            }
            Die();
            return true;
        }
        return false;
    }

    /// @brief 指定名のエンティティのうち XZ 平面で最も近いものへ向けて速度を設定する
    /// @param targetName 狙う対象のエンティティ名
    /// @param speed 水平方向の速さ
    /// @param ownerName 撃ち手の名前（現状未使用）
    /// @param myPos 弾の現在位置
    /// @note 対象が見つからなければ +Z 方向に飛ばす。縦方向には常に 1.5 を与える
    void SetVelocityTowardTarget(const std::string& targetName, float speed,
                                  const std::string& ownerName, const RC::Vector3& myPos) {
        (void)ownerName;
        Scene* scene = GetScene();
        if (!scene) return;

        // 対象が見つからなかったときの既定方向（+Z）
        RC::Vector3 aimDir = { 0.0f, 0.0f, 1.0f };

        // 距離 999 未満で最も近い対象を探す
        float nearestDist = 999.0f;
        for (auto& e : scene->GetEntities()) {
            if (e->GetName() == targetName && !e->IsPendingDestroy() && e->IsActive()) {
                auto* eTr = e->GetComponent<TransformComponent>();
                if (eTr) {
                    float dx = eTr->position.x - myPos.x;
                    float dz = eTr->position.z - myPos.z;
                    float d = std::sqrt(dx * dx + dz * dz);
                    if (d < nearestDist && d > 0.01f) {
                        nearestDist = d;
                        aimDir = { dx / d, 0.0f, dz / d };
                    }
                }
            }
        }

        velocity_ = { aimDir.x * speed, 1.5f, aimDir.z * speed };
    }

    /// @brief 弾を破棄済みとしてマークし、非アクティブ化する（水しぶきは出さない）
    void DestroyBullet() {
        if (markedForDestroy_) return;
        markedForDestroy_ = true;

        Entity* self = GetEntity();
        if (self) {
            self->SetActive(false);
            // エンティティを再利用するため、削除やメッシュの解放はしない
        }
    }

    uint64_t effectsFolderGuid_ = 0;

    /// @brief エフェクト用の "Effects" フォルダエンティティを取得する（無ければ作成）
    /// @param scene 検索・作成先のシーン
    /// @return フォルダエンティティの GUID（一度見つけたらキャッシュを返す）
    uint64_t GetEffectsFolder(Scene* scene) {
        if (effectsFolderGuid_ != 0) return effectsFolderGuid_;
        for (auto& e : scene->GetEntities()) {
            if (e->GetName() == "Effects" && e->IsFolder()) {
                effectsFolderGuid_ = e->Guid();
                return effectsFolderGuid_;
            }
        }
        auto folder = scene->CreateEntity("Effects");
        folder->SetIsFolder(true);
        effectsFolderGuid_ = folder->Guid();
        return effectsFolderGuid_;
    }

    /// @brief 消滅位置にエフェクトを出す
    /// @details 水中なら泡がまとめて弾けるだけ（水面の波・しぶきは出さない）。
    ///          水上なら水面の波と水しぶき（heavy は水柱）に泡を添える。数は弾種が決める。
    /// @param pos 消滅位置（ワールド座標）
    void SpawnSplash(const RC::Vector3& pos) {
        if (pos.y < 0.0f) {
            SpawnBubbleParticles(pos, s_underwater.burstBubbles * kind_->BurstBubbleMultiplier(), 0.06f);
            return;
        }
        SpawnSurfaceSplash(pos);
        SpawnBubbleParticles(pos, kind_->SurfaceBubbleCount(), 0.05f);
    }

    /// @brief 水面の波と水しぶき（heavy は水柱）を出す（泡は出さない）
    /// @param pos 出す位置（ワールド座標）
    /// @details 波・しぶきの出し方は弾種（BulletKind::SpawnSurfaceEffect）に任せる。
    void SpawnSurfaceSplash(const RC::Vector3& pos) {
        Scene* scene = GetScene();
        if (!scene) return;
        kind_->SpawnSurfaceEffect(pos);
    }
};

REGISTER_SCRIPT(WaterBullet)
