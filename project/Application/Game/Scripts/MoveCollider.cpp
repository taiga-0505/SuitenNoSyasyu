#include "ECS/ScriptableEntity.h"
#include "ECS/ScriptRegistry.h"
#include "ECS/TransformComponent.h"
#include "Input/Input.h"
#include "Game/Framework/GameInput.h"
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
#include <array>
#include <cmath>
#include <cstdlib>

namespace {

// ---------------------------------------------------------------------
// 移動入力の読み取り元（ポリモーフィズム）
// ---------------------------------------------------------------------
// 以前は OnUpdate の中にキーボードとコントローラーの読み取りが直書きされていた。
// 入力デバイスごとにクラスを分け、MoveCollider は「全部の読み取り元から足し合わせる」
// ことだけを知っている形にした。入力デバイスを足すときはクラスを 1 つ足して並べるだけで済む。

/// @brief 移動入力の読み取り元の基底クラス
class MoveInputSource {
public:
    virtual ~MoveInputSource() = default;

    /// @brief このデバイスの入力を移動量へ足し込む
    /// @param input 入力管理
    /// @param moveX X 方向の移動量（足し込み先）
    /// @param moveZ Z 方向の移動量（足し込み先）
    /// @note 足し算の順序を以前と同じにするため、合計を返すのではなく直接足し込む
    virtual void Accumulate(const Input& input, float& moveX, float& moveZ) const = 0;
};

/// @brief 入力アクション（MoveX / MoveY）
/// @details 以前はキーボード（WASD / 矢印）とコントローラー（左スティック・十字キー）を
///          別々のクラスで読んでいたが、どのキー・ボタンで動かすかは InputActions の割り当て
///          （GameInput::DefineDefaults）に移したので、ここはアクションの値を足すだけになった。
///          スティックのデッドゾーンも InputActions 側で掛かる。
class ActionMoveInput final : public MoveInputSource {
public:
    /// @brief MoveX / MoveY の値を足し込む
    /// @param input 入力管理（未使用。アクションは App が毎フレーム計算済み）
    /// @param moveX X 方向の移動量（足し込み先）
    /// @param moveZ Z 方向の移動量（足し込み先）
    void Accumulate(const Input& input, float& moveX, float& moveZ) const override {
        (void)input;
        moveX += GameInput::Actions().GetValue(GameInput::kMoveX);
        moveZ += GameInput::Actions().GetValue(GameInput::kMoveY);
    }
};

} // namespace

/// @brief プレイヤー移動用スクリプト
/// @details WASD、矢印キー、またはコントローラーで XZ 平面上を移動し、
///          移動中はインタラクティブ水面に波源を追加して波紋を立てる。
class MoveCollider : public ScriptableEntity {
public:
    /// @brief エディタ（インスペクタ）上で移動速度を調整できるようにする
    void OnImGui() override {
#if RC_ENABLE_IMGUI
        ImGui::DragFloat("Move Speed (移動速度)##MoveCollider", &speed_, 0.1f, 0.1f, 100.0f);
#endif
    }

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

        // 全デバイスの入力を順に足し込む（どのデバイスかは読み取り元クラスに任せる）
        for (const MoveInputSource* source : InputSources()) {
            source->Accumulate(*input, moveX, moveZ);
        }

        // 複数入力の重複などで1.0を超えないようクランプ
        if (moveX > 1.0f) moveX = 1.0f;
        if (moveX < -1.0f) moveX = -1.0f;
        if (moveZ > 1.0f) moveZ = 1.0f;
        if (moveZ < -1.0f) moveZ = -1.0f;

        // 移動適用
        tr->position.x += moveX * speed_ * deltaTime;
        tr->position.z += moveZ * speed_ * deltaTime;

        // 水面シミュレーションへの波源追加（速度に比例した強さで、前位置をへこませ現在位置を押し上げる）
        if (moveX != 0.0f || moveZ != 0.0f) {
            float velocity = std::sqrt(moveX * moveX + moveZ * moveZ) * speed_;

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

private:
    /// @brief 移動入力の読み取り元の一覧（足し込む順）
    /// @return 読み取り元（今は入力アクションの 1 つだけ。タッチなどを足すときはここに並べる）
    /// @note 読み取り元は状態を持たないので、全インスタンスで共有する
    static const std::array<const MoveInputSource*, 1>& InputSources() {
        static const ActionMoveInput kActions{};
        static const std::array<const MoveInputSource*, 1> kSources = {&kActions};
        return kSources;
    }

    float speed_ = 5.0f;                            ///< 移動速度（m/秒）。シリアライズはしない（インスペクタでの調整用）
    RC::Vector3 prevPosition_ = {0.0f, 0.0f, 0.0f}; ///< 1 フレーム前の位置（へこみの波源に使う）
    bool firstUpdate_ = true;                       ///< まだ一度も更新していないか（前位置が無効）
};

REGISTER_SCRIPT(MoveCollider)
