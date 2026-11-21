#include "ECS/ScriptableEntity.h"
#include "ECS/ScriptRegistry.h"
#include "ECS/TransformComponent.h"
#include <iostream>

/// @brief サンプル用: 毎フレーム回転するスクリプト
class TestRotatorScript : public ScriptableEntity {
protected:
    /// @brief アタッチされたことをコンソールへ出力する
    void OnCreate() override {
        std::cout << "[TestRotatorScript] OnCreate: アタッチされました！\n";
    }

    /// @brief Y 軸まわりに毎秒約 1 ラジアン回転させる
    /// @param deltaTime 前フレームからの経過時間（秒）
    void OnUpdate(float deltaTime) override {
        if (auto* tr = GetComponent<TransformComponent>()) {
            tr->rotation.y += 1.0f * deltaTime;
        }
    }

    /// @brief 破棄されたことをコンソールへ出力する
    void OnDestroy() override {
        std::cout << "[TestRotatorScript] OnDestroy: 破棄されました！\n";
    }
};

// これを書くことで、自動的に ScriptRegistry に "TestRotatorScript" として登録され、
// インスペクタの Native Script のプルダウンに表示される。
REGISTER_SCRIPT(TestRotatorScript)
