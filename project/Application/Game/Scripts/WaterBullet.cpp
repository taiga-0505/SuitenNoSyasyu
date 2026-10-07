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
#include <algorithm>
#include <cmath>
#include <vector>

/// @brief Water bullet: flies forward, detects collision, spawns splash effect
class WaterBullet : public ScriptableEntity {
public:
    RC::Vector3 velocity = { 0.0f, 0.0f, 0.0f };
    float lifetime = 3.0f;
    float gravity = -3.0f;
    int damage = 1;
    bool initialized = false;
    std::string bulletType = "normal";
    bool isDead = false;

protected:
    void Die() {
        if (isDead) return;
        isDead = true;
        if (Entity* self = GetEntity()) {
            if (auto* tr = self->GetComponent<TransformComponent>()) {
                SpawnSplash(tr->position);
            }
            // エンティティを破棄せず、非アクティブにしてプール（再利用）する
            self->SetActive(false);
            initialized = false;
        }
    }

    void OnCreate() override {
        elapsed_ = 0.0f;
        // ColliderComponentの付与
        Entity* self = GetEntity();
        if (self && !self->HasComponent<ColliderComponent>()) {
            auto* col = &self->AddComponent<ColliderComponent>();
            col->shape = ColliderComponent::Shape::Sphere;
            col->radius = 1.0f;
            col->isTrigger = true; // 物理反射させずイベントだけ取る
        }
    }

    void OnUpdate(float deltaTime) override {
        Entity* self = GetEntity();
        if (self && self->GetTagInt("reused", 0) == 1) {
            elapsed_ = 0.0f;
            markedForDestroy_ = false;
            isDead = false;
            initialized = false;
            velocity = {0.0f, 0.0f, 0.0f};
            self->ClearTag("reused");
        }

        if (markedForDestroy_ || isDead) return;

        auto* tr = GetComponent<TransformComponent>();
        if (!tr) return;

        Scene* scene = GetScene();
        if (!scene) return;

        // On first update, determine velocity from entity name + target
        if (!initialized) {
            if (self) {
                std::string name = self->GetName();
                int bType = self->GetTagInt("bullet_type", 0);
                if (bType == 0) bulletType = "normal";
                else if (bType == 1) bulletType = "spread";
                else if (bType == 2) bulletType = "heavy";
                
                float baseSpeed = 40.0f;
                int speedTag = self->GetTagInt("bullet_speed", 0);
                if (speedTag > 0) baseSpeed = static_cast<float>(speedTag) / 10.0f;

                if (bulletType == "heavy") {
                    damage = 3;
                    gravity = -6.0f; // Falls faster due to weight
                    if (speedTag == 0) baseSpeed = 25.0f; // Slower
                } else if (bulletType == "spread") {
                    damage = 1;
                    gravity = -2.5f;
                    if (speedTag == 0) baseSpeed = 35.0f;
                }

                int lifeTag = self->GetTagInt("bullet_lifetime", 0);
                if (lifeTag > 0) this->lifetime = static_cast<float>(lifeTag) / 10.0f;

                if (name == "PlayerBullet" || name == "EnemyBullet") {
                    int dirX = self->GetTagInt("dir_x", -9999);
                    int dirY = self->GetTagInt("dir_y", -9999);
                    int dirZ = self->GetTagInt("dir_z", -9999);
                    
                    if (dirX != -9999 && dirY != -9999 && dirZ != -9999 && bulletType != "spread") {
                        float dx = static_cast<float>(dirX) / 1000.0f;
                        float dy = static_cast<float>(dirY) / 1000.0f;
                        float dz = static_cast<float>(dirZ) / 1000.0f;
                        float len = std::sqrt(dx * dx + dy * dy + dz * dz);
                        if (len > 0.01f) { dx /= len; dy /= len; dz /= len; }
                        else { dx = 0.0f; dy = 0.0f; dz = 1.0f; }
                        velocity = { dx * baseSpeed, dy * baseSpeed, dz * baseSpeed };
                    } else if (name == "PlayerBullet" && bulletType == "spread") {
                        float dx = static_cast<float>(self->GetTagInt("dir_x", 0)) / 1000.0f;
                        float dz = static_cast<float>(self->GetTagInt("dir_z", 1000)) / 1000.0f;
                        // Normalize just in case
                        float len = std::sqrt(dx * dx + dz * dz);
                        if (len > 0.01f) { dx /= len; dz /= len; }
                        else { dx = 0.0f; dz = 1.0f; }
                        velocity = { dx * baseSpeed, 1.5f, dz * baseSpeed };
                    } else if (name == "PlayerBullet" && velocity.x == 0.0f && velocity.z == 0.0f) {
                        SetVelocityTowardTarget("Enemy", baseSpeed, "player", tr->position);
                    }
                }
            }
            initialized = true;
        }

        // === Physics ===
        RC::Vector3 oldPos = tr->position;
        velocity.y += gravity * deltaTime;
        tr->position.x += velocity.x * deltaTime;
        tr->position.y += velocity.y * deltaTime;
        tr->position.z += velocity.z * deltaTime;

        // Continuous Collision Detection (CCD) Raycast
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
            
            bool isPlayerBullet = (self->GetName() == "PlayerBullet");

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

                // 疎結合: タグで当たり判定対象かチェック
                bool isTarget = false;
                bool isTerrainEntity = (e->GetTagInt("is_terrain", 0) == 1);
                if (isPlayerBullet) {
                    // 敵・地形に加えて、宝箱などのアイテム（is_item）にも当たる
                    if (e->GetTagInt("is_enemy", 0) == 1 || isTerrainEntity ||
                        e->GetTagInt("is_item", 0) == 1) isTarget = true;
                } else {
                    if (e->GetTagInt("is_player", 0) == 1 || isTerrainEntity) isTarget = true;
                }
                if (!isTarget) continue;

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

                if (hit && t >= 0.0f && t <= closestDist) {
                    closestDist = t;
                    hitEntity = e;
                    hitPoint = RC::Add(ray.origin, RC::Mul(ray.direction, t));
                }
            }

            if (hitEntity) {
                tr->position = hitPoint;
                OnCollision(hitEntity, hitPoint);
                if (isDead) return;
            }
        }

        // 撃ち落とし：`shootable` の付いた敵弾（船の砲弾）は、自機の弾が近くを通ると両方消える
        if (!isPlayerBulletSelf(self) && self->GetTagInt("shootable", 0) == 1) {
            if (TryShootDown(scene, self, oldPos, tr->position)) return;
        }

        // Lifetime
        elapsed_ += deltaTime;
        if (elapsed_ >= lifetime) {
            Die();
            return;
        }

        // Water surface collision (crossing y = 0)
        if ((oldPos.y > 0.0f && tr->position.y <= 0.0f) || (oldPos.y < 0.0f && tr->position.y >= 0.0f)) {
            Die();
            return;
        }
    }

    void OnCollision(Entity* other, const RC::Vector3& contactPoint = {}) override {
        if (isDead || !other) return;

        Entity* self = GetEntity();
        std::string myName = self ? self->GetName() : "";
        bool isPlayerBullet = (myName == "PlayerBullet");
        std::string targetName = other->GetName();

        // 当たった瞬間をログで確認
        printf("[Collision] %s hit %s at (%.2f, %.2f, %.2f)\n", 
               myName.c_str(), targetName.c_str(), 
               contactPoint.x, contactPoint.y, contactPoint.z);

        // 当たり判定対象の確認 (タグによる疎結合化)
        bool hitEnemy = (isPlayerBullet && (other->GetTagInt("is_enemy", 0) == 1));
        bool hitPlayer = (!isPlayerBullet && (other->GetTagInt("is_player", 0) == 1));
        bool hitTerrain = (other->GetTagInt("is_terrain", 0) == 1);
        // 宝箱などのアイテム。ダメージだけ積み、ポイント加算はアイテム側（TreasureChestScript）が行う
        bool hitItem = (isPlayerBullet && (other->GetTagInt("is_item", 0) == 1));

        if (hitItem) {
            int pendingDmg = other->GetTagInt("pending_damage", 0);
            other->SetTag("pending_damage", pendingDmg + damage);
            Die();
        } else if (hitEnemy || hitPlayer) {
            // ダメージ処理
            int pendingDmg = other->GetTagInt("pending_damage", 0);
            other->SetTag("pending_damage", pendingDmg + damage);

            // プレイヤースコア加算 (カメラをキャッシュしてパフォーマンス向上)
            if (hitEnemy) {
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
            // 地形に当たって消滅
            if (auto* tr = GetComponent<TransformComponent>()) {
                SpawnSplash(tr->position);
            }
            Die();
        }
    }

    private:
    float elapsed_ = 0.0f;
    bool markedForDestroy_ = false;
    std::weak_ptr<Entity> cachedCamera_;

    static bool isPlayerBulletSelf(Entity* self) {
        return self && self->GetName() == "PlayerBullet";
    }

    /// @brief このフレームの移動線分（from→to）の近くに自機の弾があれば、両方を消す
    /// @details 砲弾と自機の弾はどちらも速いので、点どうしの距離ではなく
    ///          自分の移動線分との距離で見る（すれ違いの取りこぼし防止）。
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

            // 線分上の最近点
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

    void SetVelocityTowardTarget(const std::string& targetName, float speed,
                                  const std::string& ownerName, const RC::Vector3& myPos) {
        Scene* scene = GetScene();
        if (!scene) return;

        // If owner specified, use owner's facing direction as fallback
        RC::Vector3 aimDir = { 0.0f, 0.0f, 1.0f };

        // Find target entity
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

        velocity = { aimDir.x * speed, 1.5f, aimDir.z * speed };
    }

    void DestroyBullet() {
        if (markedForDestroy_) return;
        markedForDestroy_ = true;

        Entity* self = GetEntity();
        if (self) {
            self->SetActive(false);
            // Do not delete or unload mesh so we can reuse the entity.
        }
    }

    uint64_t effectsFolderGuid_ = 0;

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

    void SpawnSplash(const RC::Vector3& pos) {
        Scene* scene = GetScene();
        if (!scene) return;

        float waveRadius = (bulletType == "heavy") ? 0.1f : 0.05f;
        float waveStrength = (bulletType == "heavy") ? 1.5f : 0.5f;

        // Strong water surface wave
        RC::WaveSource source;
        source.uv = RC::Vector2((pos.x / 100.0f) + 0.5f, (pos.z / 100.0f) + 0.5f);
        source.radius = waveRadius;
        source.strength = waveStrength;
        RC::AddWaveSource(source);

        int splashCount = (bulletType == "heavy") ? 0 : 12;
        int bubbleCount = (bulletType == "heavy") ? 8 : 4;
        
        // 水しぶき・泡・水柱は Entity を作らず、エンジンのエフェクト粒として出す。
        // （以前は 1 粒ごとに Entity＋専用メッシュ＋スクリプトを作っており、
        //   弾を撃つと数百個の Entity が増えて更新・描画・エディタ表示が重くなっていた）
        if (bulletType == "heavy") {
            // 水柱：太さ 1.5 の平たい状態から一気に伸びる（旧 HeavySplash の impact_factor = 150）
            RC::EffectParticleSpawn column;
            column.kind = RC::EffectParticleKind::HeavySplash;
            column.position = pos;
            column.scale = 1.5f;
            column.color = { 0.9f, 0.95f, 1.0f, 1.0f };
            column.impactFactor = 1.5f;
            RC::SpawnEffectParticle(column);
        } else {
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

        // アクセントの泡（全弾種）
        for (int i = 0; i < bubbleCount; ++i) {
            RC::EffectParticleSpawn bubble;
            bubble.kind = RC::EffectParticleKind::Bubble;
            bubble.position = pos;
            bubble.scale = 0.05f + (i % 3) * 0.05f;
            bubble.color = { 0.8f, 0.9f, 1.0f, 0.6f };
            RC::SpawnEffectParticle(bubble);
        }
    }
};

REGISTER_SCRIPT(WaterBullet)
