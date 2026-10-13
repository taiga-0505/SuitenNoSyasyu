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

/// @brief 水面の汎用敵 AI。初期位置の左右を往復しながらカメラを向き、水弾を撃つ
/// @details 空中にいる間は落下し、着水時に水しぶきを出す。撃破後は沈んでいく。
class EnemyAI : public EnemyBaseScript {
public:
    /// @brief 左右往復の速さ（往復角の進む速さに掛かる係数）
    float moveSpeed = 3.0f;
    /// @brief 射撃間隔の基準秒数
    float shootInterval = 1.5f;
    /// @brief 水弾の速度
    float bulletSpeed = 10.0f;
    float preferredDist = 15.0f;  ///< プレイヤーとの希望距離（現状の移動処理では未使用）
    float weight = 5.0f;          ///< 着水時の水しぶき計算に使う重さ

protected:
    /// @brief 基底の初期化を行い、射撃タイマーと往復角を初期化する
    void OnCreate() override {
        EnemyBaseScript::OnCreate();
        std::cout << "[EnemyAI] OnCreate\n";
        shootTimer_ = shootInterval * 0.5f; // 初弾は通常の半分の待ち時間で撃つ
        strafeAngle_ = 0.0f;
    }

    /// @brief 落下・着水処理、左右往復移動、カメラへの向き合わせ、水面波の発生、射撃を行う
    /// @param deltaTime 前フレームからの経過秒数
    void OnUpdate(float deltaTime) override {
        EnemyBaseScript::OnUpdate(deltaTime);

        auto* tr = GetComponent<TransformComponent>();
        if (!tr) return;

        float prevY = tr->position.y;

        // 撃破後は重力で沈めるだけにして、移動・射撃は行わない
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

        // 狙う対象のカメラを探す。毎フレームの全走査を避けるため見つけたものはキャッシュする
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

        // 往復移動の中心として、最初の更新時の位置を覚えておく
        if (firstUpdate_) {
            initialPos_ = tr->position;
            firstUpdate_ = false;
        }

        RC::Vector3 toCam = {
            camTr->position.x - tr->position.x,
            camTr->position.y - tr->position.y,
            camTr->position.z - tr->position.z
        };
        float distToCam = std::sqrt(toCam.x * toCam.x + toCam.z * toCam.z); // 水平距離のみ

        // 初期位置を中心に X 方向へ sin で左右往復する。
        // 目標点との差に比例して寄せることで、急な位置の跳びを抑える
        strafeAngle_ += deltaTime * moveSpeed * 0.5f;
        float targetX = initialPos_.x + std::sin(strafeAngle_) * 8.0f; // 振れ幅 8m
        float targetZ = initialPos_.z;

        float moveX = (targetX - tr->position.x) * deltaTime * 2.0f;
        float moveZ = (targetZ - tr->position.z) * deltaTime * 2.0f;

        tr->position.x += moveX;
        tr->position.z += moveZ;

        // カメラの方向を向く（距離がほぼ 0 のときは atan2 が不安定なので回さない）
        if (distToCam > 0.01f) {
            tr->rotation.y = std::atan2(toCam.x, toCam.z);
        }

        // 移動したら水面に波を立てる。前位置に負・現位置に正の波源を置いて進行方向の航跡を作る。
        // 座標 / 100 + 0.5 はワールド座標（水面 100m 四方を想定）を波シミュレーションの UV に変換している
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

        // 射撃。カメラが 60m 以内のときだけ撃つ。
        // 次弾までの間隔を sin で ±0.5 秒揺らし、複数体の射撃タイミングが揃わないようにする
        shootTimer_ -= deltaTime;
        if (shootTimer_ <= 0.0f && distToCam < 60.0f) {
            ShootAt(tr->position, camTr->position);
            shootTimer_ = shootInterval + (std::sin(strafeAngle_ * 3.0f) * 0.5f);
        }
    }

    /// @brief 描画処理（基底の処理をそのまま呼ぶ）
    void OnRender() override {
        EnemyBaseScript::OnRender();
    }

    /// @brief 破棄時にログを出す
    void OnDestroy() override {
        std::cout << "[EnemyAI] OnDestroy\n";
    }

public:
    /// @brief インスペクタに基底の HP と移動・射撃パラメータの編集欄を表示する
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

    /// @brief 敵弾をまとめるフォルダ "EnemyBullets" の GUID を取得する（無ければ作る）
    /// @param scene 検索・生成先のシーン
    /// @return フォルダエンティティの GUID（一度取得したらキャッシュする）
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

    /// @brief エフェクトをまとめるフォルダ "Effects" の GUID を取得する（無ければ作る）
    /// @param scene 検索・生成先のシーン
    /// @return フォルダエンティティの GUID（一度取得したらキャッシュする）
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

    /// @brief 指定位置から目標へ向けて水弾を 1 発撃つ
    /// @param origin 発射元の位置（自身の位置）
    /// @param target 狙う位置（カメラの位置）
    /// @note 非アクティブな "EnemyBullet" があれば再利用し、無ければ新規生成する。
    void ShootAt(const RC::Vector3& origin, const RC::Vector3& target) {
        Scene* scene = GetScene();
        if (!scene) return;

        // 目標への単位方向ベクトルを求める（近すぎる場合は撃たない）
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

        // 弾エンティティを用意する。使い終わって非アクティブになった弾をプールとして再利用する
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

        // 自身にめり込まないよう、少し前方・上方から発射する
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

        // 敵弾と分かるよう赤みがかった半透明の水色にする
        if (pm->meshHandle >= 0) {
            if (auto* mat = RC::GetPrimitiveMeshMaterialPtr(pm->meshHandle)) {
                mat->color = { 1.0f, 0.2f, 0.2f, 0.85f };
            }
        }

        // 当たり判定（トリガー）と弾の挙動スクリプトを付ける。再利用時は既存のものを使う
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

        // 弾の向き・速度を WaterBullet へタグで渡す。タグは int なので向きは 1000 倍、速度は 10 倍して固定小数で持たせる
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

    /// @brief 着水地点に波紋と水しぶきを発生させる
    /// @param pos 着水位置
    /// @param impactSpeed 着水時の落下速度の大きさ
    /// @param weightVal 重さ（大きいほど波紋・しぶきが強くなる）
    void SpawnSplash(const RC::Vector3& pos, float impactSpeed, float weightVal) {
        Scene* scene = GetScene();
        if (!scene) return;

        // 落下速度と重さから衝撃の強さを求め、0.1〜5.0 に収める
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

        // 水しぶきの粒数を衝撃の強さに応じて決める
        int splashCount = static_cast<int>(8 + (impactFactor * 8));
        if (splashCount > 30) splashCount = 30; // 上限

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
