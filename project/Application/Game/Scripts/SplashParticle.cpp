#include "ECS/ScriptableEntity.h"
#include "ECS/ScriptRegistry.h"
#include "ECS/TransformComponent.h"
#include "ECS/PrimitiveMeshComponent.h"
#include "RenderCommon.h"
#include "Scene.h"
#include "ECS/GPUParticleComponent.h"
#include "Particle/GPUParticle.h"
#include <cmath>

/// @brief 外側へ飛び散りながら縮んで消える水しぶき 1 粒のスクリプト
class SplashParticle : public ScriptableEntity {
protected:
    /// @brief 寿命（0.6〜1.6 秒）と、衝撃係数に応じたランダムな初速を決める
    void OnCreate() override {
        elapsed_ = 0.0f;
        lifetime_ = 0.6f + static_cast<float>(rand() % 100) / 100.0f;

        // 衝撃係数は "impact_factor" タグ（100 倍の整数、既定 100 = 1.0）から取得
        float impactFactor = 1.0f;
        if (Entity* self = GetEntity()) {
            int scaleTag = self->GetTagInt("impact_factor", 100);
            impactFactor = static_cast<float>(scaleTag) / 100.0f;
        }

        // ランダムな初速（水平方向はランダムな向き、上向きも加える）
        // 水平速度は衝撃係数の平方根、上向き速度は衝撃係数に比例させ、強い衝撃ほど高く上がるようにする
        float angle = static_cast<float>(rand() % 360) * 3.14159f / 180.0f;
        float speed = (4.0f + static_cast<float>(rand() % 100) / 15.0f) * std::sqrt(impactFactor);
        float upSpeed = (4.0f + static_cast<float>(rand() % 100) / 15.0f) * impactFactor;

        velX_ = std::cos(angle) * speed;
        velY_ = upSpeed;
        velZ_ = std::sin(angle) * speed;
    }

    /// @brief 重力付きで移動させ、時間経過で縮小・フェードし、寿命切れか水中に沈んだら消す
    /// @param deltaTime 前フレームからの経過時間（秒）
    void OnUpdate(float deltaTime) override {
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
            initScale_ = true;
        }

        // 寿命切れで消滅
        elapsed_ += deltaTime;
        if (elapsed_ >= lifetime_) {
            Cleanup();
            return;
        }

        // 重力
        velY_ -= 12.0f * deltaTime;

        // 移動
        tr->position.x += velX_ * deltaTime;
        tr->position.y += velY_ * deltaTime;
        tr->position.z += velZ_ * deltaTime;

        // 水面より下（-0.5）まで沈んだら消滅
        if (tr->position.y < -0.5f) {
            Cleanup();
            return;
        }

        // 経過割合に応じて縮小（最小 0.01）
        float t = elapsed_ / lifetime_;
        float scale = (1.0f - t) * startScale_;
        if (scale < 0.01f) scale = 0.01f;
        tr->scale = { scale, scale, scale };

        // 経過割合に応じてアルファを下げる
        auto* pm = GetComponent<PrimitiveMeshComponent>();
        if (pm && pm->meshHandle >= 0) {
            if (auto* mat = RC::GetPrimitiveMeshMaterialPtr(pm->meshHandle)) {
                mat->color.w = (1.0f - t) * 0.85f;
            }
        }
    }

private:
    float elapsed_ = 0.0f;
    float lifetime_ = 0.5f;
    float velX_ = 0.0f;
    float velY_ = 0.0f;
    float velZ_ = 0.0f;
    bool markedForDestroy_ = false;
    bool initScale_ = false;
    float startScale_ = 1.0f;

    /// @brief 消滅処理（破棄せず非アクティブ化してプールに戻す）
    void Cleanup() {
        if (markedForDestroy_) return;
        markedForDestroy_ = true;

        Entity* self = GetEntity();
        if (self) {
            self->SetActive(false);
        }
    }
};

REGISTER_SCRIPT(SplashParticle)

/// @brief 航跡用の小さな水滴が跳ねて消えるパーティクルのスクリプト
class WakeParticle : public ScriptableEntity {
protected:
    /// @brief 寿命とランダムな初速（上向き主体でわずかに横）を決める
    void OnCreate() override {
        elapsed_ = 0.0f;
        lifetime_ = 0.5f + static_cast<float>(rand() % 50) / 100.0f; // 0.5 ~ 1.0s

        // ランダムな初速（上方向＋わずかに横）
        float angle = static_cast<float>(rand() % 360) * 3.14159f / 180.0f;
        float speed = 0.5f + static_cast<float>(rand() % 100) / 100.0f;
        velX_ = std::cos(angle) * speed;
        velZ_ = std::sin(angle) * speed;
        velY_ = 3.0f + static_cast<float>(rand() % 100) / 50.0f; // 3.0 ~ 5.0
    }

    /// @brief 重力付きで移動させ、水面でバウンドさせながら縮小・フェードし、寿命で消す
    /// @param deltaTime 前フレームからの経過時間（秒）
    void OnUpdate(float deltaTime) override {
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
            initScale_ = true;
        }

        // 寿命切れで消滅
        elapsed_ += deltaTime;
        if (elapsed_ >= lifetime_) {
            Cleanup();
            return;
        }

        // 重力と移動
        velY_ -= 15.0f * deltaTime;
        tr->position.x += velX_ * deltaTime;
        tr->position.y += velY_ * deltaTime;
        tr->position.z += velZ_ * deltaTime;

        // 水面に落ちたら上下速度を反転・減衰させてバウンドし、横速度も減衰させる
        if (tr->position.y < 0.0f) {
            tr->position.y = 0.0f;
            velY_ *= -0.3f; // 少しバウンド
            velX_ *= 0.8f;
            velZ_ *= 0.8f;
        }

        // 徐々に小さくなる
        float t = elapsed_ / lifetime_;
        float currentScale = startScale_ * (1.0f - t);
        if (currentScale < 0.01f) currentScale = 0.01f;
        tr->scale = { currentScale, currentScale, currentScale };

        // メッシュのアルファを下げ、メッシュ側のトランスフォームも Transform に同期する
        auto* pm = GetComponent<PrimitiveMeshComponent>();
        if (pm && pm->meshHandle >= 0) {
            if (auto* mat = RC::GetPrimitiveMeshMaterialPtr(pm->meshHandle)) {
                mat->color.w = (1.0f - t) * 0.9f;
            }
            if (auto* pmTr = RC::GetPrimitiveMeshTransformPtr(pm->meshHandle)) {
                pmTr->scale = tr->scale;
                pmTr->translation = tr->position;
            }
        }
    }

private:
    float elapsed_ = 0.0f;
    float lifetime_ = 1.0f;
    float velX_ = 0.0f;
    float velY_ = 0.0f;
    float velZ_ = 0.0f;
    bool markedForDestroy_ = false;
    bool initScale_ = false;
    float startScale_ = 1.0f;

    /// @brief 消滅処理（破棄せず非アクティブ化してプールに戻す）
    void Cleanup() {
        if (markedForDestroy_) return;
        markedForDestroy_ = true;

        Entity* self = GetEntity();
        if (self) {
            self->SetActive(false);
        }
    }
};

REGISTER_SCRIPT(WakeParticle)

/// @brief 重い弾の着水で立ち上がる水柱パーティクルのスクリプト
class HeavySplashParticle : public ScriptableEntity {
protected:
    /// @brief 寿命（1.0〜1.2 秒）と、衝撃係数に応じた水柱の最大高さを決める
    void OnCreate() override {
        elapsed_ = 0.0f;
        lifetime_ = 1.0f + static_cast<float>(rand() % 20) / 100.0f; // 1.0 ~ 1.2s

        // 衝撃係数は "impact_factor" タグ（100 倍の整数、既定 100 = 1.0）から取得
        float impactFactor = 1.0f;
        if (Entity* self = GetEntity()) {
            int scaleTag = self->GetTagInt("impact_factor", 100);
            impactFactor = static_cast<float>(scaleTag) / 100.0f;
        }

        // 水柱が伸びる最大の高さ
        maxHeight_ = 15.0f * impactFactor;
    }

    /// @brief 水柱を伸ばしつつ幅を変化させ、終盤でフェード・UV スクロールし、寿命で消す
    /// @param deltaTime 前フレームからの経過時間（秒）
    void OnUpdate(float deltaTime) override {
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

        // 初回に基準の幅と根元の高さを記録
        if (!initScale_) {
            startScale_ = tr->scale.x; // 基準の幅
            startY_ = tr->position.y;
            initScale_ = true;
        }

        // 寿命切れで消滅
        elapsed_ += deltaTime;
        if (elapsed_ >= lifetime_) {
            Cleanup();
            return;
        }

        float t = elapsed_ / lifetime_;

        // 高さは ease-out（1-(1-t)^4）で、最初に一気に伸びてから最大高さに近づく
        float heightProgress = 1.0f - std::pow(1.0f - t, 4.0f);
        float currentHeight = maxHeight_ * heightProgress;

        // 幅は sin カーブで途中最大 1.5 倍まで膨らみ、最後に元の幅へ戻る
        float widthScale = startScale_ * (1.0f + std::sin(t * 3.14159f) * 0.5f);

        tr->scale = { widthScale, currentHeight, widthScale };
        
        // 根元を水面に固定するための位置補正
        // 円柱メッシュは中心が原点なので、高さの半分だけ上げると底面が startY_ に留まる
        tr->position.y = startY_ + currentHeight * 0.5f;

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
                pmTr->scale = tr->scale;
                pmTr->translation = tr->position;
                pmTr->rotation = tr->rotation;
            }
        }
    }

private:
    float elapsed_ = 0.0f;
    float lifetime_ = 1.0f;
    float maxHeight_ = 10.0f;
    float startY_ = 0.0f;
    bool markedForDestroy_ = false;
    bool initScale_ = false;
    float startScale_ = 1.0f;

    /// @brief 消滅処理（破棄せず非アクティブ化してプールに戻す）
    void Cleanup() {
        if (markedForDestroy_) return;
        markedForDestroy_ = true;
        Entity* self = GetEntity();
        if (self) {
            self->SetActive(false);
        }
    }
};

REGISTER_SCRIPT(HeavySplashParticle)

/// @brief ゆっくり浮き上がり、最後に弾けて消える泡パーティクルのスクリプト
class BubbleParticle : public ScriptableEntity {
protected:
    /// @brief 寿命（0.4〜1.0 秒）とランダムな初速（ゆっくり上昇）を決める
    void OnCreate() override {
        elapsed_ = 0.0f;
        lifetime_ = 0.4f + static_cast<float>(rand() % 60) / 100.0f; // 0.4 ~ 1.0s

        // 水平はランダムな向き、上向きはゆっくり
        float angle = static_cast<float>(rand() % 360) * 3.14159f / 180.0f;
        float speed = 0.5f + static_cast<float>(rand() % 100) / 100.0f;
        velX_ = std::cos(angle) * speed;
        velZ_ = std::sin(angle) * speed;
        velY_ = 1.0f + static_cast<float>(rand() % 100) / 50.0f; // 1.0 ~ 3.0

        baseVelY_ = velY_;
    }

    /// @brief 横に揺れながら上昇させ、終盤で縮めて弾ける表現をし、寿命で消す
    /// @param deltaTime 前フレームからの経過時間（秒）
    void OnUpdate(float deltaTime) override {
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
            initScale_ = true;
        }

        // 寿命切れで消滅
        elapsed_ += deltaTime;
        if (elapsed_ >= lifetime_) {
            Cleanup();
            return;
        }

        // 水平方向の揺らぎ（sin で振動する係数を、速度と直交する向きに掛けて加える）
        float wobble = std::sin(elapsed_ * 10.0f) * 0.5f;

        tr->position.x += (velX_ + wobble * velZ_) * deltaTime;
        tr->position.y += velY_ * deltaTime;
        tr->position.z += (velZ_ - wobble * velX_) * deltaTime;

        // 経過 80% 以降で急速に縮めて弾ける様子を表現する
        float t = elapsed_ / lifetime_;
        float scale = startScale_;
        if (t > 0.8f) {
            scale = startScale_ * (1.0f - (t - 0.8f) * 5.0f);
        }
        if (scale < 0.01f) scale = 0.01f;
        tr->scale = { scale, scale, scale };

        auto* pm = GetComponent<PrimitiveMeshComponent>();
        if (pm && pm->meshHandle >= 0) {
            if (auto* mat = RC::GetPrimitiveMeshMaterialPtr(pm->meshHandle)) {
                // 泡は白っぽく、透明度を高めにする
                mat->color = { 0.8f, 0.9f, 1.0f, (1.0f - t) * 0.6f };
            }
            // メッシュ側のトランスフォームを Transform に同期
            if (auto* pmTr = RC::GetPrimitiveMeshTransformPtr(pm->meshHandle)) {
                pmTr->scale = tr->scale;
                pmTr->translation = tr->position;
            }
        }
    }

private:
    float elapsed_ = 0.0f;
    float lifetime_ = 1.0f;
    float velX_ = 0.0f;
    float velY_ = 0.0f;
    float velZ_ = 0.0f;
    float baseVelY_ = 0.0f;
    bool markedForDestroy_ = false;
    bool initScale_ = false;
    float startScale_ = 1.0f;

    /// @brief 消滅処理（破棄せず非アクティブ化してプールに戻す）
    void Cleanup() {
        if (markedForDestroy_) return;
        markedForDestroy_ = true;
        Entity* self = GetEntity();
        if (self) {
            self->SetActive(false);
        }
    }
};

REGISTER_SCRIPT(BubbleParticle)

/// @brief GPU パーティクルで泡を一度だけバースト放出し、一定時間後に自身を破棄するエミッタースクリプト
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
