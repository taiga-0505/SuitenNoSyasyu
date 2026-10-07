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
#include <cmath>
#include <iostream>

#if RC_ENABLE_IMGUI
#include "imgui/imgui.h"
#endif

#include "EnemyBaseScript.h"

/// @brief Enemy AI: chases player, shoots water bullets, takes damage
class EnemyAI : public EnemyBaseScript {
public:
    float moveSpeed = 3.0f;
    float shootInterval = 1.5f;
    float bulletSpeed = 10.0f;
    float preferredDist = 15.0f;  ///< Desired distance to player
    float weight = 5.0f;          ///< Weight for splash calculation

protected:
    void OnCreate() override {
        EnemyBaseScript::OnCreate();
        std::cout << "[EnemyAI] OnCreate\n";
        shootTimer_ = shootInterval * 0.5f; // Start with half delay
        strafeAngle_ = 0.0f;
    }

    void OnUpdate(float deltaTime) override {
        EnemyBaseScript::OnUpdate(deltaTime);

        auto* tr = GetComponent<TransformComponent>();
        if (!tr) return;

        float prevY = tr->position.y;

        if (isDead) {
            verticalVel_ -= 9.8f * deltaTime; // 沈む時の重力（水中の抵抗などを考慮しても良いが一旦通常重力）
            tr->position.y += verticalVel_ * deltaTime;
            // 空中から水面に落ちた時だけ水しぶきを上げる
            if (prevY > 0.1f && tr->position.y <= 0.1f) {
                float impactSpeed = std::abs(verticalVel_);
                SpawnSplash({tr->position.x, 0.0f, tr->position.z}, impactSpeed, weight);
            }
            return;
        }

        // 生きている時も空中にいれば落下する
        if (tr->position.y > 0.0f) {
            verticalVel_ -= 9.8f * deltaTime;
            tr->position.y += verticalVel_ * deltaTime;

            // 水面に着水
            if (tr->position.y <= 0.0f) {
                tr->position.y = 0.0f;
                if (prevY > 0.1f) {
                    float impactSpeed = std::abs(verticalVel_);
                    SpawnSplash({tr->position.x, 0.0f, tr->position.z}, impactSpeed, weight);
                }
                verticalVel_ = 0.0f;
            }
        } else {
            tr->position.y = 0.0f;
            verticalVel_ = 0.0f;
        }

        Scene* scene = GetScene();
        if (!scene) return;

        // Find Camera (cached)
        std::shared_ptr<Entity> mainCamera = cachedTarget_.lock();
        if (!mainCamera || mainCamera->IsPendingDestroy() || !mainCamera->IsActive()) {
            mainCamera = nullptr;
            for (auto& e : scene->GetEntities()) {
                if (e->HasComponent<CameraComponent>()) {
                    mainCamera = e;
                    cachedTarget_ = mainCamera;
                    break;
                }
            }
        }
        if (!mainCamera) return;

        auto* camTr = mainCamera->GetComponent<TransformComponent>();
        if (!camTr) return;

        if (firstUpdate_) {
            initialPos_ = tr->position;
            firstUpdate_ = false;
        }

        RC::Vector3 toCam = {
            camTr->position.x - tr->position.x,
            camTr->position.y - tr->position.y,
            camTr->position.z - tr->position.z
        };
        float distToCam = std::sqrt(toCam.x * toCam.x + toCam.z * toCam.z);

        // Strafe movement (left/right around initial position)
        strafeAngle_ += deltaTime * moveSpeed * 0.5f;
        float targetX = initialPos_.x + std::sin(strafeAngle_) * 8.0f; // 8 meters strafe
        float targetZ = initialPos_.z;

        float moveX = (targetX - tr->position.x) * deltaTime * 2.0f;
        float moveZ = (targetZ - tr->position.z) * deltaTime * 2.0f;

        tr->position.x += moveX;
        tr->position.z += moveZ;

        // Face toward camera
        if (distToCam > 0.01f) {
            tr->rotation.y = std::atan2(toCam.x, toCam.z);
        }

        // Water wave interaction
        if (std::abs(moveX) > 0.001f || std::abs(moveZ) > 0.001f) {
            float velocity = std::sqrt(moveX * moveX + moveZ * moveZ) / deltaTime;

            RC::WaveSource sourcePrev;
            sourcePrev.uv = RC::Vector2((prevPosition_.x / 100.0f) + 0.5f, (prevPosition_.z / 100.0f) + 0.5f);
            sourcePrev.radius = 0.035f;
            sourcePrev.strength = -velocity * 0.02f;
            RC::AddWaveSource(sourcePrev);

            RC::WaveSource source;
            source.uv = RC::Vector2((tr->position.x / 100.0f) + 0.5f, (tr->position.z / 100.0f) + 0.5f);
            source.radius = 0.035f;
            source.strength = velocity * 0.02f;
            RC::AddWaveSource(source);
        }
        prevPosition_ = tr->position;

        // === Shooting ===
        shootTimer_ -= deltaTime;
        if (shootTimer_ <= 0.0f && distToCam < 60.0f) {
            ShootAt(tr->position, camTr->position);
            shootTimer_ = shootInterval + (std::sin(strafeAngle_ * 3.0f) * 0.5f);
        }
    }

    void OnRender() override {
        EnemyBaseScript::OnRender();
    }

    void OnDestroy() override {
        std::cout << "[EnemyAI] OnDestroy\n";
    }

public:
    void OnImGui() override {
        EnemyBaseScript::OnImGui();
#if RC_ENABLE_IMGUI
        ImGui::DragFloat("Move Speed##E", &moveSpeed, 0.1f, 0.1f, 20.0f);
        ImGui::DragFloat("Shoot Interval##E", &shootInterval, 0.05f, 0.1f, 5.0f);
        ImGui::DragFloat("Bullet Speed##E", &bulletSpeed, 0.1f, 1.0f, 50.0f);
        ImGui::DragFloat("Preferred Dist##E", &preferredDist, 0.5f, 5.0f, 40.0f);
        ImGui::DragFloat("Weight##E", &weight, 0.1f, 0.1f, 50.0f);
#endif
    }

private:
    float shootTimer_ = 0.0f;
    float strafeAngle_ = 0.0f;
    RC::Vector3 initialPos_ = {0.0f, 0.0f, 0.0f};
    RC::Vector3 prevPosition_ = {0.0f, 0.0f, 0.0f};
    bool firstUpdate_ = true;
    float verticalVel_ = 0.0f;
    std::weak_ptr<Entity> cachedTarget_;

    uint64_t enemyBulletsFolderGuid_ = 0;
    uint64_t effectsFolderGuid_ = 0;

    uint64_t GetEnemyBulletsFolder(Scene* scene) {
        if (enemyBulletsFolderGuid_ != 0) return enemyBulletsFolderGuid_;
        for (auto& e : scene->GetEntities()) {
            if (e->GetName() == "EnemyBullets" && e->IsFolder()) {
                enemyBulletsFolderGuid_ = e->Guid();
                return enemyBulletsFolderGuid_;
            }
        }
        auto folder = scene->CreateEntity("EnemyBullets");
        folder->SetIsFolder(true);
        enemyBulletsFolderGuid_ = folder->Guid();
        return enemyBulletsFolderGuid_;
    }

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

    void ShootAt(const RC::Vector3& origin, const RC::Vector3& target) {
        Scene* scene = GetScene();
        if (!scene) return;

        // Direction to camera
        RC::Vector3 dir = {
            target.x - origin.x,
            target.y - origin.y,
            target.z - origin.z
        };
        float len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
        if (len < 0.01f) return;
        dir.x /= len;
        dir.y /= len;
        dir.z /= len;

        // Create bullet entity
        std::shared_ptr<Entity> bullet = nullptr;
        for (auto& e : scene->GetEntities()) {
            if (e->GetName() == "EnemyBullet" && !e->IsActive() && !e->IsPendingDestroy()) {
                bullet = e;
                bullet->SetActive(true);
                bullet->SetTag("reused", 1);
                break;
            }
        }
        bool isNew = false;
        if (!bullet) {
            bullet = scene->CreateEntity("EnemyBullet");
            isNew = true;
        }

        auto* tr = bullet->GetComponent<TransformComponent>();
        if (!tr) tr = &bullet->AddComponent<TransformComponent>();
        tr->position = { origin.x + dir.x * 1.5f, origin.y + 0.5f, origin.z + dir.z * 1.5f };
        bullet->SetParentGuid(GetEnemyBulletsFolder(scene));
        tr->scale = { 0.25f, 0.25f, 0.25f };

        auto* pm = bullet->GetComponent<PrimitiveMeshComponent>();
        if (!pm) {
            pm = &bullet->AddComponent<PrimitiveMeshComponent>();
            pm->type = PrimitiveType::Sphere;
            pm->meshHandle = RC::GenerateSphere(1.0f);
        } else if (pm->meshHandle < 0) {
            pm->meshHandle = RC::GenerateSphere(1.0f);
        }
        // 水の質感で描く（以前は DataDrivenScene がエンティティ名で判定していた）
        pm->drawStyle = PrimitiveDrawStyle::Water;

        // Set color (reddish water)
        if (pm->meshHandle >= 0) {
            if (auto* mat = RC::GetPrimitiveMeshMaterialPtr(pm->meshHandle)) {
                mat->color = { 1.0f, 0.2f, 0.2f, 0.85f };
            }
        }

        auto* col = bullet->GetComponent<ColliderComponent>();
        if (!col) col = &bullet->AddComponent<ColliderComponent>();
        col->shape = ColliderComponent::Shape::Sphere;
        col->radius = 1.0f;
        col->isTrigger = true;

        auto* nsc = bullet->GetComponent<NativeScriptComponent>();
        if (!nsc) {
            nsc = &bullet->AddComponent<NativeScriptComponent>();
            nsc->AddScript("WaterBullet");
            nsc->SetScene(scene);
            if (GetSceneContext()) nsc->SetSceneContext(GetSceneContext());
        }

        // 弾の向きタグを設定
        bullet->SetTag("dir_x", static_cast<int>(dir.x * 1000.0f));
        bullet->SetTag("dir_y", static_cast<int>(dir.y * 1000.0f));
        bullet->SetTag("dir_z", static_cast<int>(dir.z * 1000.0f));
        bullet->SetTag("bullet_speed", static_cast<int>(bulletSpeed * 10.0f));
        // プールは船と共有。船の砲弾で立てた「撃ち落とせる」印を持ち越さない
        bullet->ClearTag("shootable");

        if (isNew) {
            scene->InitDynamicEntityRuntime(*bullet);
        }

        // 即座に PrimitiveMesh の Transform を同期して原点でのチラつきを防ぐ
        if (pm->meshHandle >= 0) {
            if (auto* pmTr = RC::GetPrimitiveMeshTransformPtr(pm->meshHandle)) {
                pmTr->scale = tr->scale;
                pmTr->rotation = tr->rotation;
                pmTr->translation = tr->position;
            }
        }
    }

    void SpawnSplash(const RC::Vector3& pos, float impactSpeed, float weightVal) {
        Scene* scene = GetScene();
        if (!scene) return;

        // 計算式のベース
        // impactSpeed は大体 1.0 ~ 20.0 などを想定
        // weightVal は 1.0 ~ 10.0 などを想定
        float impactFactor = (impactSpeed * 0.1f) * (weightVal * 0.2f);
        if (impactFactor < 0.1f) impactFactor = 0.1f;
        if (impactFactor > 5.0f) impactFactor = 5.0f;

        // 水面の波紋を強く発生させる
        RC::WaveSource source;
        source.uv = RC::Vector2((pos.x / 100.0f) + 0.5f, (pos.z / 100.0f) + 0.5f);
        source.radius = 0.05f + (impactFactor * 0.02f);
        source.strength = 0.5f + (impactFactor * 0.5f);
        RC::AddWaveSource(source);

        // 水しぶきパーティクルを複数生成
        int splashCount = static_cast<int>(8 + (impactFactor * 8));
        if (splashCount > 30) splashCount = 30; // Max

        // 水しぶきは Entity を作らず、エンジンのエフェクト粒として出す
        // （以前は 1 粒ごとに Entity＋専用メッシュ＋スクリプトを作っていて、数が増えると重かった）
        for (int i = 0; i < splashCount; ++i) {
            RC::EffectParticleSpawn splash;
            splash.kind = RC::EffectParticleKind::Splash;
            splash.position = pos;
            splash.scale = (0.15f + (i % 4) * 0.05f) * (1.0f + impactFactor * 0.3f);
            const float r = 0.3f + (i % 3) * 0.15f;
            const float g = 0.6f + (i % 2) * 0.2f;
            splash.color = { r, g, 1.0f, 0.85f };
            splash.impactFactor = impactFactor;
            RC::SpawnEffectParticle(splash);
        }
    }
};

REGISTER_SCRIPT(EnemyAI)
