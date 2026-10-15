#pragma once
#include "IComponent.h"
#include "Entity.h"
#include "TransformComponent.h"
#include "Math/MathTypes.h"
#include <nlohmann/json.hpp>

/// @class RigidbodyComponent
/// @brief 重力と速度で TransformComponent の位置を動かす簡易剛体コンポーネント
class RigidbodyComponent : public IComponent {
public:
  bool useGravity = true;       ///< 重力の影響を受けるか
  bool isKinematic = false;     ///< true なら物理で位置を更新しない
  float mass = 1.0f;            ///< 質量
  RC::Vector3 velocity = {0.0f, 0.0f, 0.0f}; ///< 現在の速度

  /// @brief コンポーネントの型名を返す
  /// @return "RigidbodyComponent"
  const char* TypeName() const override { return "RigidbodyComponent"; }

  /// @brief 重力で速度を更新し、速度で位置を進める
  /// @param deltaTime 前フレームからの経過時間（秒）
  /// @note 無効時と isKinematic 時は何もしない。
  void Update(float deltaTime) override {
    if (!enabled_ || isKinematic) return;

    // 重力を加える
    if (useGravity) {
      // 簡易的な重力加速度 (m/s^2)
      constexpr float gravityAcc = -9.81f;
      velocity.y += gravityAcc * deltaTime;
    }

    // 位置を更新する
    if (entity_) {
      if (auto* tr = entity_->GetComponent<TransformComponent>()) {
        tr->position.x += velocity.x * deltaTime;
        tr->position.y += velocity.y * deltaTime;
        tr->position.z += velocity.z * deltaTime;
      }
    }
  }

  /// @brief 剛体パラメータを JSON に書き出す
  /// @return useGravity / isKinematic / mass / velocity を格納した JSON
  nlohmann::json Serialize() const override {
    return {
      {"useGravity", useGravity},
      {"isKinematic", isKinematic},
      {"mass", mass},
      {"velocity", {velocity.x, velocity.y, velocity.z}}
    };
  }

  /// @brief JSON から剛体パラメータを読み込む
  /// @param j 読み込み元の JSON（含まれていないキーは現在の値のまま）
  void Deserialize(const nlohmann::json& j) override {
    if (j.contains("useGravity")) useGravity = j["useGravity"].get<bool>();
    if (j.contains("isKinematic")) isKinematic = j["isKinematic"].get<bool>();
    if (j.contains("mass")) mass = j["mass"].get<float>();
    if (j.contains("velocity")) {
      auto& v = j["velocity"];
      velocity = {v[0].get<float>(), v[1].get<float>(), v[2].get<float>()};
    }
  }
};
