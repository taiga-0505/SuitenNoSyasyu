#include "ECS/ScriptableEntity.h"
#include "ECS/ScriptRegistry.h"
#include "ECS/TransformComponent.h"
#include "ECS/ColliderComponent.h"
#include "ECS/NativeScriptComponent.h"
#include "ECS/PrimitiveMeshComponent.h"
#include "Input/Input.h"
#include "Common/EngineConfig.h"
#include "RenderCommon.h"
#include "Render/Systems/RenderInteractiveWater.h"
#include "Scene.h"
#include <iostream>
#include <cmath>

#if RC_ENABLE_IMGUI
#include "imgui/imgui.h"
#endif

/// @brief プレイヤーの移動・被ダメージ・HUD 表示を担うスクリプト
/// @details 水面上を移動して波を立て、タグ経由で受け取ったダメージ／スコアを反映する。
///          HP が 0 になると落下・着水し、シーン側へ "game_over" タグで通知する。
class PlayerController : public ScriptableEntity {
public:
    float speed = 5.0f;
    int hp = 5;
    int maxHp = 5;
    float invincibleTimer = 0.0f;
    float invincibleDuration = 1.0f;
    int score = 0;
    float weight = 5.0f;
    bool isDead = false;

    protected:
    /// @brief 生成時にログを出力する
    void OnCreate() override {
        std::cout << "[PlayerController] OnCreate\n";
    }

    /// @brief 落下・着水、タグ経由のダメージ／スコア反映、入力による移動、水面への波生成を行う
    /// @param deltaTime 前フレームからの経過時間（秒）
    void OnUpdate(float deltaTime) override {
        auto* tr = GetComponent<TransformComponent>();
        if (!tr) return;

        float prevY = tr->position.y;

        // 死亡後は重力で落下させるだけで、入力や被ダメージ処理は行わない
        if (isDead) {
            verticalVel_ -= 9.8f * deltaTime;
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

        // 他スクリプトがタグで積んだダメージ／スコア加算を取り出して反映し、タグを消す
        Entity* self = GetEntity();
        if (self) {
            int dmg = self->GetTagInt("pending_damage", 0);
            if (dmg > 0) {
                self->ClearTag("pending_damage");
                TakeDamage(dmg);
            }
            int scoreAdd = self->GetTagInt("score_add", 0);
            if (scoreAdd > 0) {
                self->ClearTag("score_add");
                score += scoreAdd;
            }
        }

        Input* input = Input::GetInstance();
        if (!input) return;

        // === 無敵時間の減算 ===
        if (invincibleTimer > 0.0f) {
            invincibleTimer -= deltaTime;
        }

        // === 移動 ===
        float moveX = 0.0f;
        float moveZ = 0.0f;

        // キーボード（WASD / 矢印キー）入力
        if (input->IsKeyPressed(DIK_W) || input->IsKeyPressed(DIK_UP))    moveZ += 1.0f;
        if (input->IsKeyPressed(DIK_S) || input->IsKeyPressed(DIK_DOWN))  moveZ -= 1.0f;
        if (input->IsKeyPressed(DIK_D) || input->IsKeyPressed(DIK_RIGHT)) moveX += 1.0f;
        if (input->IsKeyPressed(DIK_A) || input->IsKeyPressed(DIK_LEFT))  moveX -= 1.0f;

        // ゲームパッド入力（左スティックはデッドゾーン 8000 を超えたときだけ -1〜1 に正規化して加算、十字キーも加算）
        if (input->IsXInputConnected()) {
            SHORT thumbX = input->GetXInputThumbLX();
            SHORT thumbY = input->GetXInputThumbLY();
            const float deadZone = 8000.0f;
            if (abs(thumbX) > deadZone) moveX += static_cast<float>(thumbX) / 32767.0f;
            if (abs(thumbY) > deadZone) moveZ += static_cast<float>(thumbY) / 32767.0f;

            if (input->IsXInputButtonPressed(XINPUT_GAMEPAD_DPAD_UP))    moveZ += 1.0f;
            if (input->IsXInputButtonPressed(XINPUT_GAMEPAD_DPAD_DOWN))  moveZ -= 1.0f;
            if (input->IsXInputButtonPressed(XINPUT_GAMEPAD_DPAD_RIGHT)) moveX += 1.0f;
            if (input->IsXInputButtonPressed(XINPUT_GAMEPAD_DPAD_LEFT))  moveX -= 1.0f;
        }

        // キーボードとパッドの同時入力で 1 を超えないよう各軸をクランプしてから移動
        if (moveX >  1.0f) moveX =  1.0f;
        if (moveX < -1.0f) moveX = -1.0f;
        if (moveZ >  1.0f) moveZ =  1.0f;
        if (moveZ < -1.0f) moveZ = -1.0f;

        tr->position.x += moveX * speed * deltaTime;
        tr->position.z += moveZ * speed * deltaTime;

        // プレイエリア（±45）内に位置を制限する
        const float areaLimit = 45.0f;
        if (tr->position.x > areaLimit)  tr->position.x = areaLimit;
        if (tr->position.x < -areaLimit) tr->position.x = -areaLimit;
        if (tr->position.z > areaLimit)  tr->position.z = areaLimit;
        if (tr->position.z < -areaLimit) tr->position.z = -areaLimit;

        // 移動中は水面に波を立てる
        // ワールド座標 (-50〜50) を水面 UV (0〜1) に変換し、前フレーム位置に負・現在位置に正の波源を置いて航跡を表現する
        if (moveX != 0.0f || moveZ != 0.0f) {
            float velocity = std::sqrt(moveX * moveX + moveZ * moveZ) * speed;

            // 初回フレームは前フレーム位置が無効なので負の波源を置かない
            if (!firstUpdate_) {
                RC::WaveSource sourcePrev;
                sourcePrev.uv = RC::Vector2((prevPosition_.x / 100.0f) + 0.5f, (prevPosition_.z / 100.0f) + 0.5f);
                sourcePrev.radius = 0.03f;
                sourcePrev.strength = -velocity * 0.03f;
                RC::AddWaveSource(sourcePrev);
            }

            RC::WaveSource source;
            source.uv = RC::Vector2((tr->position.x / 100.0f) + 0.5f, (tr->position.z / 100.0f) + 0.5f);
            source.radius = 0.03f;
            source.strength = velocity * 0.03f;
            RC::AddWaveSource(source);

        }
        
        prevPosition_ = tr->position;
        firstUpdate_ = false;
    }

    /// @brief HUD（HP バー・スコア・死亡オーバーレイ）を描画する
    void OnRender() override {
        DrawHUD();
    }

    /// @brief 破棄時にログを出力する
    void OnDestroy() override {
        std::cout << "[PlayerController] OnDestroy\n";
    }

public:
    /// @brief 速度・HP・重さの調整 UI とスコア表示を ImGui に出す
    void OnImGui() override {
#if RC_ENABLE_IMGUI
        ImGui::DragFloat("Speed##PC", &speed, 0.1f, 0.1f, 50.0f);
        ImGui::DragInt("HP##PC", &hp, 1, 0, maxHp);
        ImGui::DragFloat("Weight##PC", &weight, 0.1f, 0.1f, 50.0f);
        ImGui::Text("Score: %d", score);
#endif
    }

    /// @brief ダメージを受けて HP を減らし、無敵時間を開始する
    /// @param damage 減らす HP 量
    /// @note 無敵時間中または死亡済みなら何もしない。HP が 0 以下になると死亡し "game_over" タグを立てる
    void TakeDamage(int damage) {
        if (invincibleTimer > 0.0f || isDead) return;
        hp -= damage;
        invincibleTimer = invincibleDuration;
        if (hp <= 0) {
            hp = 0;
            isDead = true;
            // タグでシーン側に死亡を通知
            if (Entity* self = GetEntity()) {
                self->SetTag("game_over", 1);
            }
        }
    }

private:
    RC::Vector3 prevPosition_ = {0.0f, 0.0f, 0.0f};
    bool firstUpdate_ = true;
    float verticalVel_ = 0.0f;

    /// @brief 画面左下に HP バー、右上にスコア（丸の個数）、死亡時は中央に赤帯を描画する
    void DrawHUD() {
        SceneContext* ctx = GetSceneContext();
        if (!ctx) return;

        float screenW = static_cast<float>(ctx->app->width);
        float screenH = static_cast<float>(ctx->app->height);

        // === HP バー（画面左下） ===
        float barX = 20.0f;
        float barY = screenH - 50.0f;
        float barW = 200.0f;
        float barH = 20.0f;

        // 背景（暗い赤）
        RC::DrawBox({ barX, barY }, { barX + barW, barY + barH },
                    { 0.3f, 0.05f, 0.05f, 0.8f });

        // 残り HP（HP 割合で緑→赤に変化し、幅も割合に比例）
        float hpRatio = static_cast<float>(hp) / static_cast<float>(maxHp);
        RC::Vector4 hpColor = { 1.0f - hpRatio, hpRatio, 0.1f, 0.9f };
        RC::DrawBox({ barX, barY }, { barX + barW * hpRatio, barY + barH }, hpColor);

        // 枠線
        RC::DrawBox({ barX, barY }, { barX + barW, barY + barH },
                    { 1.0f, 1.0f, 1.0f, 0.5f }, kWire);

        // === スコア表示（画面右上に丸を並べる。10 個で折り返し、最大 20 個まで） ===
        float scoreX = screenW - 220.0f;
        float scoreY = 30.0f;
        for (int i = 0; i < score && i < 20; ++i) {
            float cx = scoreX + (i % 10) * 20.0f;
            float cy = scoreY + (i / 10) * 20.0f;
            RC::DrawCircle({ cx, cy }, 7.0f, { 0.3f, 0.7f, 1.0f, 0.9f });
        }

        // === ゲームオーバー時のオーバーレイ（画面中央の赤帯） ===
        if (isDead) {
            RC::DrawBox({ 0.0f, screenH * 0.4f }, { screenW, screenH * 0.6f },
                        { 0.8f, 0.1f, 0.1f, 0.7f });
        }
    }

    /// @brief 着水点に強い波と水しぶきパーティクルを発生させる
    /// @param pos 着水位置（ワールド座標）
    /// @param impactSpeed 着水時の落下速度の絶対値
    /// @param weightVal 落下物の重さ（大きいほど波・しぶきが大きくなる）
    void SpawnSplash(const RC::Vector3& pos, float impactSpeed, float weightVal) {
        Scene* scene = GetScene();
        if (!scene) return;

        // 速度と重さから衝撃係数を求め、0.1〜5.0 に制限して以降の波・しぶきの大きさに使う
        float impactFactor = (impactSpeed * 0.1f) * (weightVal * 0.2f);
        if (impactFactor < 0.1f) impactFactor = 0.1f;
        if (impactFactor > 5.0f) impactFactor = 5.0f;

        // 衝撃係数に応じた強い波を水面に加える
        RC::WaveSource source;
        source.uv = RC::Vector2((pos.x / 100.0f) + 0.5f, (pos.z / 100.0f) + 0.5f);
        source.radius = 0.05f + (impactFactor * 0.02f);
        source.strength = 0.5f + (impactFactor * 0.5f);
        RC::AddWaveSource(source);

        // 見た目用の水しぶき粒数（衝撃係数に比例、上限 30）
        int splashCount = static_cast<int>(8 + (impactFactor * 8));
        if (splashCount > 30) splashCount = 30;

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

REGISTER_SCRIPT(PlayerController)
