#pragma once
#include "IComponent.h"
#include <nlohmann/json.hpp>

/// @struct CameraComponent
/// @brief カメラの投影パラメータを持つコンポーネント
/// @details TransformComponent と一緒に Entity へ付けると、その Entity がカメラとして働く。
struct CameraComponent : public IComponent {
    float fovY = 0.45f;       ///< 垂直方向の視野角（ラジアン）
    float nearZ = 0.1f;       ///< ニアクリップ距離
    float farZ = 100.0f;      ///< ファークリップ距離
    bool isMain = true;       ///< メインカメラかどうか（将来の複数カメラ対応用）
    RC::Vector3 shakeOffset = {0.0f, 0.0f, 0.0f}; ///< 位置に加えるカメラシェイクのオフセット（シリアライズしない）

    /// @brief コンポーネントの型名を返す
    /// @return "CameraComponent"
    const char* TypeName() const override { return "CameraComponent"; }

    /// @brief 投影パラメータを JSON に書き出す
    /// @return fovY / nearZ / farZ / isMain を格納した JSON
    nlohmann::json Serialize() const override {
      return {
        {"fovY", fovY}, {"nearZ", nearZ}, {"farZ", farZ}, {"isMain", isMain}
      };
    }

    /// @brief JSON から投影パラメータを読み込む
    /// @param j 読み込み元の JSON（含まれていないキーは現在の値のまま）
    void Deserialize(const nlohmann::json& j) override {
      if (j.contains("fovY"))   fovY   = j["fovY"].get<float>();
      if (j.contains("nearZ"))  nearZ  = j["nearZ"].get<float>();
      if (j.contains("farZ"))   farZ   = j["farZ"].get<float>();
      if (j.contains("isMain")) isMain = j["isMain"].get<bool>();
    }
};
