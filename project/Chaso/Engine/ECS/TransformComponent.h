#pragma once

#include "IComponent.h"
#include "Math/MathTypes.h"
#include "Math/Math.h"
#include <nlohmann/json.hpp>

/// @class TransformComponent
/// @brief 位置・回転・スケールを持つコンポーネント
/// @details 親子階層を持ち、ワールド行列は親をたどって計算する。
class TransformComponent : public IComponent {
public:
  RC::Vector3 position = {0.0f, 0.0f, 0.0f}; ///< 位置（x, y, z）
  RC::Vector3 rotation = {0.0f, 0.0f, 0.0f}; ///< 回転（オイラー角: x, y, z）
  RC::Vector3 scale = {1.0f, 1.0f, 1.0f};    ///< スケール（x, y, z）

  /// @brief コンポーネントの型名を返す
  /// @return "TransformComponent"
  const char* TypeName() const override { return "TransformComponent"; }

  /// @brief 位置・回転・スケールを JSON に書き出す
  /// @return position / rotation / scale を格納した JSON
  nlohmann::json Serialize() const override {
    return {
      {"position", {position.x, position.y, position.z}},
      {"rotation", {rotation.x, rotation.y, rotation.z}},
      {"scale",    {scale.x,    scale.y,    scale.z}}
    };
  }

  /// @brief JSON から位置・回転・スケールを読み込む
  /// @param j 読み込み元の JSON（含まれていないキーは現在の値のまま）
  /// @note 親子関係は保存されないので、ここでは設定しない。
  void Deserialize(const nlohmann::json& j) override {
    if (j.contains("position")) {
      auto& p = j["position"];
      position = {p[0].get<float>(), p[1].get<float>(), p[2].get<float>()};
    }
    if (j.contains("rotation")) {
      auto& r = j["rotation"];
      rotation = {r[0].get<float>(), r[1].get<float>(), r[2].get<float>()};
    }
    if (j.contains("scale")) {
      auto& s = j["scale"];
      scale = {s[0].get<float>(), s[1].get<float>(), s[2].get<float>()};
    }
  }

  /// @brief ワールド行列を計算して返す
  /// @return 親の Transform まで反映した最終的なワールド行列
  RC::Matrix4x4 GetWorldMatrix() const {
    RC::Matrix4x4 local = MakeAffineMatrix(scale, rotation, position);
    if (parent_) {
      return Multiply(local, parent_->GetWorldMatrix());
    }
    return local;
  }

  /// @brief Transform 構造体から値を設定する
  /// @param t コピー元の Transform 構造体
  void SetFromTransform(const Transform &t) {
    scale = t.scale;
    rotation = t.rotation;
    position = t.translation;
  }

  /// @brief Transform 構造体に変換する
  /// @return 現在の値を持つ Transform 構造体
  Transform ToTransform() const {
    Transform t;
    t.scale = scale;
    t.rotation = rotation;
    t.translation = position;
    return t;
  }

  /// @brief 親の Transform を設定する
  /// @param parent 親コンポーネントへのポインタ（nullptr で親なし）
  void SetParent(TransformComponent *parent) { parent_ = parent; }

  /// @brief 親の Transform を取得する
  /// @return 親コンポーネントへのポインタ（親なしなら nullptr）
  TransformComponent *GetParent() const { return parent_; }

private:
  TransformComponent *parent_ = nullptr; ///< 親へのポインタ（所有しない）
};
