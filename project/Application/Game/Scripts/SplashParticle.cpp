#include "ECS/ScriptableEntity.h"
#include "ECS/ScriptRegistry.h"
#include "ECS/TransformComponent.h"
#include "ECS/PrimitiveMeshComponent.h"
#include "RenderCommon.h"
#include "Scene.h"
#include "ECS/GPUParticleComponent.h"
#include "Particle/GPUParticle.h"
#include <cmath>
#include <cstdlib>

// =====================================================================
// 水しぶき・航跡・水柱・泡の 1 粒ずつのスクリプト
// =====================================================================
// 4 種類の粒は「プールから再利用されたら初期化し直す → 初回にスケールを記録 →
// 寿命を進め、切れたら非アクティブにしてプールへ返す → 種類ごとの動き」という
// 同じ流れを、それぞれのクラスに丸ごと書き写していた。
// 流れを基底クラス PooledParticleScript に固定し（Template Method）、
// 種類ごとに違う「初速・寿命の決め方」と「1 フレームの動き」だけを
// 仮想関数で差し替える形にした（ポリモーフィズム）。

/// @brief プールで使い回す 1 粒のパーティクルの基底クラス（Template Method）
/// @details 毎フレームの流れ（OnUpdate、派生クラスでは上書きできない）:
///            1. "reused" タグが立っていたら（プールから再利用された）初期化し直す
///            2. 返却済みなら何もしない
///            3. 初回だけ初期スケール（縮小の基準）を記録し、OnCaptureStart を呼ぶ
///            4. 寿命を進め、切れたらプールへ返す
///            5. OnUpdateParticle で種類ごとの動きをする
class PooledParticleScript : public ScriptableEntity {
protected:
    /// @brief 経過時間を 0 に戻し、種類ごとの初期化（OnSpawn）を行う
    void OnCreate() final {
        elapsed_ = 0.0f;
        OnSpawn();
    }

    /// @brief 再利用の検出・寿命の管理・種類ごとの更新を決まった順で行う
    /// @param deltaTime 前フレームからの経過時間（秒）
    void OnUpdate(float deltaTime) final {
        Entity* self = GetEntity();
        // プールから再利用された場合は初期化し直す
        if (self && self->GetTagInt("reused", 0) == 1) {
            OnCreate();
            initScale_ = false;
            markedForDestroy_ = false;
            self->ClearTag("reused");
        }

        if (markedForDestroy_) return;

        auto* tr = GetComponent<TransformComponent>();
        if (!tr) return;

        // 初回に初期スケールを記録（縮小の基準にする）
        if (!initScale_) {
            startScale_ = tr->scale.x;
            OnCaptureStart(*tr);
            initScale_ = true;
        }

        // 寿命切れで消滅
        elapsed_ += deltaTime;
        if (elapsed_ >= lifetime_) {
            ReturnToPool();
            return;
        }

        OnUpdateParticle(*tr, deltaTime);
    }

    /// @brief 寿命と初速など、種類ごとの初期値を決める（生成時・再利用時に呼ばれる）
    /// @note SetLifetime で寿命を必ず設定すること
    virtual void OnSpawn() = 0;

    /// @brief 初回の更新で、初期スケールの記録と同時に呼ばれる（既定は何もしない）
    /// @param tr 粒の Transform
    virtual void OnCaptureStart(const TransformComponent& tr) { (void)tr; }

    /// @brief 寿命内の 1 フレームぶんの動き（移動・縮小・フェード）
    /// @param tr 粒の Transform
    /// @param deltaTime 前フレームからの経過時間（秒）
    virtual void OnUpdateParticle(TransformComponent& tr, float deltaTime) = 0;

    /// @brief 消滅処理（破棄せず非アクティブ化してプールに戻す）
    void ReturnToPool() {
        if (markedForDestroy_) return;
        markedForDestroy_ = true;

        Entity* self = GetEntity();
        if (self) {
            self->SetActive(false);
        }
    }

    /// @brief 寿命を設定する
    /// @param seconds 寿命（秒）
    void SetLifetime(float seconds) { lifetime_ = seconds; }

    /// @brief 生成（再利用）されてからの経過秒数
    /// @return 経過秒数
    float Elapsed() const { return elapsed_; }

    /// @brief 寿命に対する経過の割合
    /// @return 経過秒数 ÷ 寿命
    float Progress() const { return elapsed_ / lifetime_; }

    /// @brief 初回に記録した初期スケール（X 成分）
    /// @return 初期スケール
    float StartScale() const { return startScale_; }

    /// @brief "impact_factor" タグ（100 倍の整数、既定 100 = 1.0）から衝撃係数を読む
    /// @return 衝撃係数（タグが無ければ 1.0、エンティティが無ければ 1.0）
    float ReadImpactFactor() const {
        float impactFactor = 1.0f;
        if (Entity* self = GetEntity()) {
            int scaleTag = self->GetTagInt("impact_factor", 100);
            impactFactor = static_cast<float>(scaleTag) / 100.0f;
        }
        return impactFactor;
    }

private:
    float elapsed_ = 0.0f;          ///< 生成（再利用）されてからの経過秒数
    float lifetime_ = 1.0f;         ///< 寿命（秒）
    bool markedForDestroy_ = false; ///< プールへ返却済みか
    bool initScale_ = false;        ///< 初期スケールを記録済みか
    float startScale_ = 1.0f;       ///< 初期スケール（縮小の基準）
};

/// @brief 外側へ飛び散りながら縮んで消える水しぶき 1 粒のスクリプト
class SplashParticle : public PooledParticleScript {
protected:
    /// @brief 寿命（0.6〜1.6 秒）と、衝撃係数に応じたランダムな初速を決める
    void OnSpawn() override {
        SetLifetime(0.6f + static_cast<float>(rand() % 100) / 100.0f);

        // 衝撃係数は "impact_factor" タグ（100 倍の整数、既定 100 = 1.0）から取得
        const float impactFactor = ReadImpactFactor();

        // ランダムな初速（水平方向はランダムな向き、上向きも加える）
        // 水平速度は衝撃係数の平方根、上向き速度は衝撃係数に比例させ、強い衝撃ほど高く上がるようにする
        float angle = static_cast<float>(rand() % 360) * 3.14159f / 180.0f;
        float speed = (4.0f + static_cast<float>(rand() % 100) / 15.0f) * std::sqrt(impactFactor);
        float upSpeed = (4.0f + static_cast<float>(rand() % 100) / 15.0f) * impactFactor;

        velX_ = std::cos(angle) * speed;
        velY_ = upSpeed;
        velZ_ = std::sin(angle) * speed;
    }

    /// @brief 重力付きで移動させ、時間経過で縮小・フェードし、水中に沈んだら消す
    /// @param tr 粒の Transform
    /// @param deltaTime 前フレームからの経過時間（秒）
    void OnUpdateParticle(TransformComponent& tr, float deltaTime) override {
        // 重力
        velY_ -= 12.0f * deltaTime;

        // 移動
        tr.position.x += velX_ * deltaTime;
        tr.position.y += velY_ * deltaTime;
        tr.position.z += velZ_ * deltaTime;

        // 水面より下（-0.5）まで沈んだら消滅
        if (tr.position.y < -0.5f) {
            ReturnToPool();
            return;
        }

        // 経過割合に応じて縮小（最小 0.01）
        float t = Progress();
        float scale = (1.0f - t) * StartScale();
        if (scale < 0.01f) scale = 0.01f;
        tr.scale = { scale, scale, scale };

        // 経過割合に応じてアルファを下げる
        auto* pm = GetComponent<PrimitiveMeshComponent>();
        if (pm && pm->meshHandle >= 0) {
            if (auto* mat = RC::GetPrimitiveMeshMaterialPtr(pm->meshHandle)) {
                mat->color.w = (1.0f - t) * 0.85f;
            }
        }
    }

private:
    float velX_ = 0.0f;
    float velY_ = 0.0f;
    float velZ_ = 0.0f;
};

REGISTER_SCRIPT(SplashParticle)

/// @brief 航跡用の小さな水滴が跳ねて消えるパーティクルのスクリプト
class WakeParticle : public PooledParticleScript {
protected:
    /// @brief 寿命とランダムな初速（上向き主体でわずかに横）を決める
    void OnSpawn() override {
        SetLifetime(0.5f + static_cast<float>(rand() % 50) / 100.0f); // 0.5 ~ 1.0s

        // ランダムな初速（上方向＋わずかに横）
        float angle = static_cast<float>(rand() % 360) * 3.14159f / 180.0f;
        float speed = 0.5f + static_cast<float>(rand() % 100) / 100.0f;
        velX_ = std::cos(angle) * speed;
        velZ_ = std::sin(angle) * speed;
        velY_ = 3.0f + static_cast<float>(rand() % 100) / 50.0f; // 3.0 ~ 5.0
    }

    /// @brief 重力付きで移動させ、水面でバウンドさせながら縮小・フェードする
    /// @param tr 粒の Transform
    /// @param deltaTime 前フレームからの経過時間（秒）
    void OnUpdateParticle(TransformComponent& tr, float deltaTime) override {
        // 重力と移動
        velY_ -= 15.0f * deltaTime;
        tr.position.x += velX_ * deltaTime;
        tr.position.y += velY_ * deltaTime;
        tr.position.z += velZ_ * deltaTime;

        // 水面に落ちたら上下速度を反転・減衰させてバウンドし、横速度も減衰させる
        if (tr.position.y < 0.0f) {
            tr.position.y = 0.0f;
            velY_ *= -0.3f; // 少しバウンド
            velX_ *= 0.8f;
            velZ_ *= 0.8f;
        }

        // 徐々に小さくなる
        float t = Progress();
        float currentScale = StartScale() * (1.0f - t);
        if (currentScale < 0.01f) currentScale = 0.01f;
        tr.scale = { currentScale, currentScale, currentScale };

        // メッシュのアルファを下げ、メッシュ側のトランスフォームも Transform に同期する
        auto* pm = GetComponent<PrimitiveMeshComponent>();
        if (pm && pm->meshHandle >= 0) {
            if (auto* mat = RC::GetPrimitiveMeshMaterialPtr(pm->meshHandle)) {
                mat->color.w = (1.0f - t) * 0.9f;
            }
            if (auto* pmTr = RC::GetPrimitiveMeshTransformPtr(pm->meshHandle)) {
                pmTr->scale = tr.scale;
                pmTr->translation = tr.position;
            }
        }
    }

private:
    float velX_ = 0.0f;
    float velY_ = 0.0f;
    float velZ_ = 0.0f;
};

REGISTER_SCRIPT(WakeParticle)

/// @brief 重い弾の着水で立ち上がる水柱パーティクルのスクリプト
class HeavySplashParticle : public PooledParticleScript {
protected:
    /// @brief 寿命（1.0〜1.2 秒）と、衝撃係数に応じた水柱の最大高さを決める
    void OnSpawn() override {
        SetLifetime(1.0f + static_cast<float>(rand() % 20) / 100.0f); // 1.0 ~ 1.2s

        // 衝撃係数は "impact_factor" タグ（100 倍の整数、既定 100 = 1.0）から取得
        const float impactFactor = ReadImpactFactor();

        // 水柱が伸びる最大の高さ
        maxHeight_ = 15.0f * impactFactor;
    }

    /// @brief 初回に根元の高さを記録する（基準の幅は基底クラスが記録する）
    /// @param tr 粒の Transform
    void OnCaptureStart(const TransformComponent& tr) override { startY_ = tr.position.y; }

    /// @brief 水柱を伸ばしつつ幅を変化させ、終盤でフェード・UV スクロールする
    /// @param tr 粒の Transform
    /// @param deltaTime 前フレームからの経過時間（秒）
    void OnUpdateParticle(TransformComponent& tr, float deltaTime) override {
        float t = Progress();

        // 高さは ease-out（1-(1-t)^4）で、最初に一気に伸びてから最大高さに近づく
        float heightProgress = 1.0f - std::pow(1.0f - t, 4.0f);
        float currentHeight = maxHeight_ * heightProgress;

        // 幅は sin カーブで途中最大 1.5 倍まで膨らみ、最後に元の幅へ戻る
        float widthScale = StartScale() * (1.0f + std::sin(t * 3.14159f) * 0.5f);

        tr.scale = { widthScale, currentHeight, widthScale };

        // 根元を水面に固定するための位置補正
        // 円柱メッシュは中心が原点なので、高さの半分だけ上げると底面が startY_ に留まる
        tr.position.y = startY_ + currentHeight * 0.5f;

        auto* pm = GetComponent<PrimitiveMeshComponent>();
        if (pm && pm->meshHandle >= 0) {
            if (auto* mat = RC::GetPrimitiveMeshMaterialPtr(pm->meshHandle)) {
                // 経過 70% 以降で線形にフェードアウト
                float alpha = 1.0f;
                if (t > 0.7f) {
                    alpha = 1.0f - (t - 0.7f) / 0.3f;
                }
                mat->color.w = alpha;

                // 水が勢いよく流れる表現として UV を V 方向にスクロール（行列の平行移動成分）
                mat->uvTransform.m[3][1] -= deltaTime * 3.0f;
            }
            // メッシュ側のトランスフォームを Transform に同期
            if (auto* pmTr = RC::GetPrimitiveMeshTransformPtr(pm->meshHandle)) {
                pmTr->scale = tr.scale;
                pmTr->translation = tr.position;
                pmTr->rotation = tr.rotation;
            }
        }
    }

private:
    float maxHeight_ = 10.0f; ///< 水柱が伸びる最大の高さ
    float startY_ = 0.0f;     ///< 根元（水面）の高さ
};

REGISTER_SCRIPT(HeavySplashParticle)

/// @brief ゆっくり浮き上がり、最後に弾けて消える泡パーティクルのスクリプト
class BubbleParticle : public PooledParticleScript {
protected:
    /// @brief 寿命（0.4〜1.0 秒）とランダムな初速（ゆっくり上昇）を決める
    void OnSpawn() override {
        SetLifetime(0.4f + static_cast<float>(rand() % 60) / 100.0f); // 0.4 ~ 1.0s

        // 水平はランダムな向き、上向きはゆっくり
        float angle = static_cast<float>(rand() % 360) * 3.14159f / 180.0f;
        float speed = 0.5f + static_cast<float>(rand() % 100) / 100.0f;
        velX_ = std::cos(angle) * speed;
        velZ_ = std::sin(angle) * speed;
        velY_ = 1.0f + static_cast<float>(rand() % 100) / 50.0f; // 1.0 ~ 3.0
    }

    /// @brief 横に揺れながら上昇させ、終盤で縮めて弾ける表現をする
    /// @param tr 粒の Transform
    /// @param deltaTime 前フレームからの経過時間（秒）
    void OnUpdateParticle(TransformComponent& tr, float deltaTime) override {
        // 水平方向の揺らぎ（sin で振動する係数を、速度と直交する向きに掛けて加える）
        float wobble = std::sin(Elapsed() * 10.0f) * 0.5f;

        tr.position.x += (velX_ + wobble * velZ_) * deltaTime;
        tr.position.y += velY_ * deltaTime;
        tr.position.z += (velZ_ - wobble * velX_) * deltaTime;

        // 経過 80% 以降で急速に縮めて弾ける様子を表現する
        float t = Progress();
        float scale = StartScale();
        if (t > 0.8f) {
            scale = StartScale() * (1.0f - (t - 0.8f) * 5.0f);
        }
        if (scale < 0.01f) scale = 0.01f;
        tr.scale = { scale, scale, scale };

        auto* pm = GetComponent<PrimitiveMeshComponent>();
        if (pm && pm->meshHandle >= 0) {
            if (auto* mat = RC::GetPrimitiveMeshMaterialPtr(pm->meshHandle)) {
                // 泡は白っぽく、透明度を高めにする
                mat->color = { 0.8f, 0.9f, 1.0f, (1.0f - t) * 0.6f };
            }
            // メッシュ側のトランスフォームを Transform に同期
            if (auto* pmTr = RC::GetPrimitiveMeshTransformPtr(pm->meshHandle)) {
                pmTr->scale = tr.scale;
                pmTr->translation = tr.position;
            }
        }
    }

private:
    float velX_ = 0.0f;
    float velY_ = 0.0f;
    float velZ_ = 0.0f;
};

REGISTER_SCRIPT(BubbleParticle)

/// @brief GPU パーティクルで泡を一度だけバースト放出し、一定時間後に自身を破棄するエミッタースクリプト
/// @details 1 粒ずつのパーティクルとは違い、プールへ返さずに破棄する・粒を自分で動かさない、
///          という点で PooledParticleScript とは is-a が成り立たないので、別の流れのまま残している。
class GPUBubbleEmitter : public ScriptableEntity {
protected:
    /// @brief GPUParticleComponent のパーティクルシステムを泡用の設定（球形エミッタ・浮上・白→透明）にする
    void OnCreate() override {
        elapsed_ = 0.0f;
        lifetime_ = 2.0f; // 2秒間エミッターとして存在

        if (Entity* self = GetEntity()) {
            if (auto* gpu = self->GetComponent<GPUParticleComponent>()) {
                if (gpu->particleSystem) {
                    gpu->particleSystem->SetMaxParticles(1024);
                    gpu->particleSystem->SetParticleType(ParticleType::Explosion);
                    gpu->particleSystem->SetBlendMode(kBlendModeNormal);
                    gpu->particleSystem->minScale_ = 0.5f;
                    gpu->particleSystem->maxScale_ = 1.5f;
                    // 色付きの泡など、基本は白
                    gpu->particleSystem->startColor_ = {1.0f, 1.0f, 1.0f, 1.0f};
                    gpu->particleSystem->endColor_ = {1.0f, 1.0f, 1.0f, 0.0f};
                    gpu->particleSystem->SetEmitCount(100); // 最初の一瞬で一気に出す
                    gpu->particleSystem->minLifeTime_ = 1.0f;
                    gpu->particleSystem->maxLifeTime_ = 2.5f;
                    gpu->particleSystem->gravity_ = -3.0f; // 上に向かって浮上
                    gpu->particleSystem->baseVelocity_ = {0.0f, 2.0f, 0.0f};
                    gpu->particleSystem->velocityVariance_ = 0.5f;
                    gpu->particleSystem->emitterShape_ = EmitterShape::Sphere;
                    gpu->particleSystem->shapeRadius_ = 1.0f;
                }
            }
        }
    }

    /// @brief 生成 0.1 秒後に放出を止め、寿命が来たらエンティティごと破棄する
    /// @param deltaTime 前フレームからの経過時間（秒）
    void OnUpdate(float deltaTime) override {
        if (markedForDestroy_) return;

        elapsed_ += deltaTime;

        // 生成後すぐに emitCount を 0 にして放出を止める（バースト放出のシミュレート）
        if (elapsed_ > 0.1f) {
            if (Entity* self = GetEntity()) {
                if (auto* gpu = self->GetComponent<GPUParticleComponent>()) {
                    if (gpu->particleSystem) {
                        gpu->particleSystem->SetEmitCount(0);
                    }
                }
            }
        }

        // 放出済みの粒が消えきる頃（寿命）にエンティティごと破棄する（他の粒と違いプールしない）
        if (elapsed_ >= lifetime_) {
            markedForDestroy_ = true;
            if (Entity* self = GetEntity()) {
                self->Destroy();
            }
        }
    }

private:
    float elapsed_ = 0.0f;
    float lifetime_ = 2.0f;
    bool markedForDestroy_ = false;
};

REGISTER_SCRIPT(GPUBubbleEmitter)
