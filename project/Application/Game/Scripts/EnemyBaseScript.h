#pragma once
#include "ECS/ScriptableEntity.h"
#include "ECS/ScriptRegistry.h"
#include "ECS/TransformComponent.h"
#include "Scene.h"
#include "ECS/ModelRendererComponent.h"
#include "ECS/PrimitiveMeshComponent.h"
#include "ECS/GPUParticleComponent.h"
#include "ECS/NativeScriptComponent.h"
#include "Particle/GPUParticle.h"
#include "RenderCommon.h"
#include <iostream>
#include <format>
#include "Common/Log/Log.h"

#if RC_ENABLE_IMGUI
#include "imgui/imgui.h"
#endif

#include "Engine/Camera/CameraMath.h"
#include "ECS/CameraComponent.h"
#include "ECS/ColliderComponent.h"
#include "Engine/Render/RenderContext.h"
#include "Game/Framework/GameSession.h"


/// @brief 敵の共通基底クラス。体力・被ダメージ・撃破状態の管理を担う
/// @details 被弾フラッシュ、撃破時の当たり判定無効化と泡演出、撃破数の集計もここで行う。
class EnemyBaseScript : public ScriptableEntity {
public:
    /// @brief 現在の体力
    int hp = 10;
    /// @brief 最大体力
    int maxHp = 10;
    /// @brief 撃破済みか
    bool isDead = false;
    /// @brief 撃破されてからの経過秒数
    float deathTimer = 0.0f;
    /// @brief 撃破されてから実体を消すまでの秒数
    /// @details 沈む・崩れるといった演出の長さがタイプごとに違うため可変にした。
    ///          船（T-15）は浸水して沈みきるまでに時間がかかるので長めに取る。
    float deathDuration = 2.0f;
    
    /// @brief 被弾フラッシュの残り秒数（0 以下で元の色に戻す）
    float flashTimer = 0.0f;
    /// @brief フラッシュ前の元の色
    RC::Vector4 originalColor = {1.0f, 1.0f, 1.0f, 1.0f};
    /// @brief originalColor を保存済みか
    bool hasSavedColor = false;

    /// @brief 体力が JSON / スポナーから明示的に与えられたか
    /// @details 派生クラスは OnCreate で既定 HP を入れることが多いが、
    ///          それを無条件に代入するとデータ側の指定を潰してしまう。
    ///          A-03 でウェーブごとに敵の硬さを変えられるようにするため、
    ///          このフラグが立っているときは派生側の既定値を使わない。
    bool hpFromData = false;

    /// @brief deathDuration が JSON / スポナーから明示的に与えられたか
    /// @details hpFromData と同じ理由。派生クラスが OnCreate で既定値を入れるとき、
    ///          このフラグが立っていればデータ側の指定を尊重する。
    bool deathDurationFromData = false;

    /// @brief 撃破時に泡のパーティクル（GPUBubbleEmitter）を出すか
    /// @details 泡は水中・水面の敵向けの演出。空を飛ぶ鳥（BirdEnemyScript）では不自然なので、
    ///          派生クラスが OnCreate で false にして自前の演出に差し替える。
    bool spawnDeathBubbles = true;

protected:
    /// @brief 体力を JSON へ書き出す
    /// @details 派生クラスは自分の Serialize の先頭でこれを呼び、
    ///          返ってきた json に自分の項目を足していく。
    /// @return hp / maxHp / deathDuration を格納した json
    nlohmann::json Serialize() override {
        nlohmann::json j;
        j["hp"] = hp;
        j["maxHp"] = maxHp;
        j["deathDuration"] = deathDuration;
        return j;
    }

    /// @brief JSON から体力と撃破演出時間を読み込む
    /// @param j 読み込み元の json（hp / maxHp / deathDuration は任意）
    /// @note 指定があった項目は hpFromData / deathDurationFromData を立て、派生側の既定値で上書きされないようにする。
    void Deserialize(const nlohmann::json& j) override {
        // maxHp だけ指定された場合は hp も満タンとして揃える。
        // ウェーブ定義に "maxHp" とだけ書けるほうが素直なため。
        if (j.contains("maxHp")) {
            maxHp = j["maxHp"].get<int>();
            hp = maxHp;
            hpFromData = true;
        }
        if (j.contains("hp")) {
            hp = j["hp"].get<int>();
            if (!j.contains("maxHp")) maxHp = hp;
            hpFromData = true;
        }
        if (j.contains("deathDuration")) {
            deathDuration = j["deathDuration"].get<float>();
            deathDurationFromData = true;
        }
    }

    /// @brief 被弾フラッシュ前の元の色を保存する
    /// @note ModelRenderer を優先し、無ければ PrimitiveMesh のマテリアル色を使う。保存は初回のみ。
    void SaveOriginalColor() {
        if (hasSavedColor) return;
        if (Entity* self = GetEntity()) {
            if (auto* mr = self->GetComponent<ModelRendererComponent>()) {
                originalColor = mr->color;
                hasSavedColor = true;
            } else if (auto* pm = self->GetComponent<PrimitiveMeshComponent>()) {
                if (pm->meshHandle >= 0) {
                    if (auto* mat = RC::GetPrimitiveMeshMaterialPtr(pm->meshHandle)) {
                        originalColor = mat->color;
                        hasSavedColor = true;
                    }
                }
            }
        }
    }

    /// @brief 自身の描画色を変更する
    /// @param color 設定する色（RGBA）
    /// @note ModelRenderer を優先し、無ければ PrimitiveMesh のマテリアル色を変更する。
    void ApplyColor(const RC::Vector4& color) {
        if (Entity* self = GetEntity()) {
            if (auto* mr = self->GetComponent<ModelRendererComponent>()) {
                mr->color = color;
            } else if (auto* pm = self->GetComponent<PrimitiveMeshComponent>()) {
                if (pm->meshHandle >= 0) {
                    if (auto* mat = RC::GetPrimitiveMeshMaterialPtr(pm->meshHandle)) {
                        mat->color = color;
                    }
                }
            }
        }
    }

    /// @brief 保存しておいた元の色に戻す（未保存なら何もしない）
    void RestoreColor() {
        if (hasSavedColor) {
            ApplyColor(originalColor);
        }
    }

    /// @brief 敵タグを付け、コライダーが無ければ既定の球コライダーを追加する
    void OnCreate() override {
        if (Entity* self = GetEntity()) {
            self->SetTag("is_enemy", 1);
            
            // コライダーがなければフェールセーフとして追加
            if (!self->HasComponent<ColliderComponent>()) {
                auto* col = &self->AddComponent<ColliderComponent>();
                col->shape = ColliderComponent::Shape::Sphere;
                col->radius = 1.0f; // スケール依存
                col->isTrigger = false;
            }
        }
    }

    /// @brief タグ経由の被ダメージ反映、被弾フラッシュ、撃破後の消滅、HP の公開を行う
    /// @param deltaTime 前フレームからの経過秒数
    void OnUpdate(float deltaTime) override {
        // 他スクリプトが "pending_damage" タグに積んだダメージを取り出して反映する
        Entity* self = GetEntity();
        if (self) {
            int dmg = self->GetTagInt("pending_damage", 0);
            if (dmg > 0) {
                self->ClearTag("pending_damage");
                TakeDamage(dmg);
            }
            
            // 被弾フラッシュの時間を進め、終わったら元の色に戻す
            if (flashTimer > 0.0f) {
                flashTimer -= deltaTime;
                if (flashTimer <= 0.0f) {
                    RestoreColor();
                }
            }

            // 撃破後、deathDuration 秒経ったら実体を消す（仮の消滅処理）
            if (isDead) {
                deathTimer += deltaTime;
                if (deathTimer > deathDuration) {
                    self->Destroy();
                }
            }
            
            // HP情報をタグで公開（UI用）
            self->SetTag("current_hp", hp);
            self->SetTag("max_hp", maxHp);
        }
    }

    /// @brief 描画処理（このクラスでは何もしない）
    /// @note HP などの UI 描画は RailShooterController 側で一括して行う。
    void OnRender() override {
        // UI描画は RailShooterController 側で一括して行うように変更しました
    }

public:
    /// @brief インスペクタに HP の編集欄を表示する
    void OnImGui() override {
#if RC_ENABLE_IMGUI
        ImGui::DragInt("HP##EnemyBase", &hp, 1, 0, maxHp);
#endif
    }

    /// @brief ダメージを受ける。体力が 0 以下になったら撃破処理を行う
    /// @param damage 受けるダメージ量
    /// @note 撃破時は撃破数の加算、"enemy_defeated" タグ通知、当たり判定の無効化、
    ///       spawnDeathBubbles が真なら泡パーティクルの生成を行う。撃破済みなら何もしない。
    virtual void TakeDamage(int damage) {
        if (isDead) return;
        hp -= damage;
        
        Log::Print(std::format("[{}] Took {} damage! HP left: {}/{}", GetEntity()->GetName(), damage, hp, maxHp));
        
        // ダメージを受けた時に赤色にフラッシュさせる
        SaveOriginalColor();
        ApplyColor({1.0f, 0.2f, 0.2f, 1.0f}); // 一時的に赤色に
        flashTimer = 0.2f; // 0.2秒後に元の色に戻る

        if (hp <= 0) {
            hp = 0;
            isDead = true;
            deathTimer = 0.0f;
            // リザルトの「撃破数」。派生クラスが TakeDamage を上書きしていても
            // 基底のこの処理を通る（ShipEnemyScript も EnemyBaseScript::TakeDamage を呼ぶ）ので、
            // 撃破の数え上げはここ 1 箇所に置く。
            GameSession::Get().AddEnemyDefeated();
            // タグでシーン側に撃破を通知
            if (Entity* self = GetEntity()) {
                self->SetTag("enemy_defeated", 1);
                
                // 撃破後は弾が当たらないようにする
                self->ClearTag("is_enemy");
                if (auto* col = self->GetComponent<ColliderComponent>()) {
                    col->SetEnabled(false);
                }

                // GPU泡パーティクル発生
                Scene* scene = spawnDeathBubbles ? GetScene() : nullptr;
                if (scene) {
                    auto emitter = scene->CreateEntity("GPUBubbleEmitter");
                    auto* tr = &emitter->AddComponent<TransformComponent>();
                    if (auto* myTr = self->GetComponent<TransformComponent>()) {
                        tr->position = myTr->position;
                    }
                    auto* gpu = &emitter->AddComponent<GPUParticleComponent>();
                    if (gpu->particleSystem) {
                        gpu->particleSystem->SetPipelinePrefix("gpu_particle_bubble");
                    }
                    
                    auto* nsc = &emitter->AddComponent<NativeScriptComponent>();
                    nsc->AddScript("GPUBubbleEmitter");
                    nsc->SetScene(scene);
                    if (GetSceneContext()) nsc->SetSceneContext(GetSceneContext());

                    scene->InitDynamicEntityRuntime(*emitter);
                }
            }
        }
    }

};
