#include "ECS/ScriptableEntity.h"
#include "ECS/ScriptRegistry.h"
#include "ECS/TransformComponent.h"
#include "Input/Input.h"
#include "Common/EngineConfig.h"
#if RC_ENABLE_IMGUI
#include "imgui/imgui.h"
#endif
#include <iostream>
#include "Render/Systems/RenderInteractiveWater.h"
#include "Scene.h"
#include "ECS/PrimitiveMeshComponent.h"
#include "ECS/NativeScriptComponent.h"
#include "RenderCommon.h"
#include <cstdlib>

/// @brief プレイヤー移動用スクリプト
/// @details WASD、矢印キー、またはコントローラーで XZ 平面上を移動し、
///          移動中はインタラクティブ水面に波源を追加して波紋を立てる。
class MoveCollider : public ScriptableEntity {
public:
    float speed = 5.0f; ///< 移動速度（m/秒）

protected:
    /// @brief アタッチされたことをコンソールへ出力する
    void OnCreate() override {
        std::cout << "[MoveCollider] OnCreate: アタッチされました！\n";
    }

    /// @brief 入力から移動量を求めて位置を更新し、移動中は水面に波源を追加する
    /// @param deltaTime 前フレームからの経過時間（秒）
    void OnUpdate(float deltaTime) override {
        auto* tr = GetComponent<TransformComponent>();
        if (!tr) return;

        Input* input = Input::GetInstance();
        if (!input) return;

        float moveX = 0.0f;
        float moveZ = 0.0f;

        // --- キーボード入力 (WASD / 矢印) ---
        if (input->IsKeyPressed(DIK_W) || input->IsKeyPressed(DIK_UP)) {
            moveZ += 1.0f;
        }
        if (input->IsKeyPressed(DIK_S) || input->IsKeyPressed(DIK_DOWN)) {
            moveZ -= 1.0f;
        }
        if (input->IsKeyPressed(DIK_D) || input->IsKeyPressed(DIK_RIGHT)) {
            moveX += 1.0f;
        }
        if (input->IsKeyPressed(DIK_A) || input->IsKeyPressed(DIK_LEFT)) {
            moveX -= 1.0f;
        }

        // --- コントローラー入力 ---
        if (input->IsXInputConnected()) {
            SHORT thumbX = input->GetXInputThumbLX();
            SHORT thumbY = input->GetXInputThumbLY();
            
            // デッドゾーンを設けて -1〜1 に正規化（32767 は SHORT の最大値）
            const float deadZone = 8000.0f;
            if (abs(thumbX) > deadZone) {
                moveX += static_cast<float>(thumbX) / 32767.0f;
            }
            if (abs(thumbY) > deadZone) {
                moveZ += static_cast<float>(thumbY) / 32767.0f; // Y軸スティックでZ移動
            }

            // 十字キー（D-Pad）
            if (input->IsXInputButtonPressed(XINPUT_GAMEPAD_DPAD_UP)) {
                moveZ += 1.0f;
            }
            if (input->IsXInputButtonPressed(XINPUT_GAMEPAD_DPAD_DOWN)) {
                moveZ -= 1.0f;
            }
            if (input->IsXInputButtonPressed(XINPUT_GAMEPAD_DPAD_RIGHT)) {
                moveX += 1.0f;
            }
            if (input->IsXInputButtonPressed(XINPUT_GAMEPAD_DPAD_LEFT)) {
                moveX -= 1.0f;
            }
        }

        // 複数入力の重複などで1.0を超えないようクランプ
        if (moveX > 1.0f) moveX = 1.0f;
        if (moveX < -1.0f) moveX = -1.0f;
        if (moveZ > 1.0f) moveZ = 1.0f;
        if (moveZ < -1.0f) moveZ = -1.0f;

        // 移動適用
        tr->position.x += moveX * speed * deltaTime;
        tr->position.z += moveZ * speed * deltaTime;

        // 水面シミュレーションへの波源追加（速度に比例した強さで、前位置をへこませ現在位置を押し上げる）
        if (moveX != 0.0f || moveZ != 0.0f) {
            float velocity = std::sqrt(moveX * moveX + moveZ * moveZ) * speed;

            if (!firstUpdate_) {
                // 1フレーム前の位置にマイナスの波源（へこみ）を追加
                RC::WaveSource sourcePrev;
                sourcePrev.uv = RC::Vector2((prevPosition_.x / 100.0f) + 0.5f, (prevPosition_.z / 100.0f) + 0.5f);
                sourcePrev.radius = 0.03f;
                sourcePrev.strength = -velocity * 0.03f; 
                RC::AddWaveSource(sourcePrev);
            }

            // 現在位置にプラスの波源（押し上げ）を追加
            RC::WaveSource source;
            // WaterPlaneのワールドサイズ(100m幅)でワールド座標からUVへ変換
            source.uv = RC::Vector2((tr->position.x / 100.0f) + 0.5f, (tr->position.z / 100.0f) + 0.5f);
            source.radius = 0.03f; // UV空間での半径 (100m平面上で3m程度の範囲)
            source.strength = velocity * 0.03f; // 波紋の強さ（速度に比例）
            RC::AddWaveSource(source);

        }
        
        prevPosition_ = tr->position;
        firstUpdate_ = false;
    }

    /// @brief 破棄されたことをコンソールへ出力する
    void OnDestroy() override {
        std::cout << "[MoveCollider] OnDestroy: 破棄されました！\n";
    }

public:
    /// @brief エディタ（インスペクタ）上で移動速度を調整できるようにする
    void OnImGui() override {
#if RC_ENABLE_IMGUI
        ImGui::DragFloat("Move Speed (移動速度)##MoveCollider", &speed, 0.1f, 0.1f, 100.0f);
#endif
    }

private:
    RC::Vector3 prevPosition_ = {0.0f, 0.0f, 0.0f}; ///< 1 フレーム前の位置（へこみの波源に使う）
    bool firstUpdate_ = true;                       ///< まだ一度も更新していないか（前位置が無効）
};

REGISTER_SCRIPT(MoveCollider)
