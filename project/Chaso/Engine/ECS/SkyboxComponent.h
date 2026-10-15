#pragma once

#include "IComponent.h"
#include "Math/MathTypes.h"
#include <string>
#include <nlohmann/json.hpp>

/// @class SkyboxComponent
/// @brief スカイボックスを描画するコンポーネント
/// @details RC::CreateSkyBox() で作ったハンドルを保持する。
class SkyboxComponent : public IComponent {
public:
  int skyboxHandle = -1;  ///< スカイボックスのハンドル（未設定なら -1）
  bool visible = true;    ///< 表示するか
  RC::Vector4 color = {1.0f, 1.0f, 1.0f, 1.0f}; ///< 乗算色

  /// @brief 有効なハンドルが設定されているかを返す
  /// @return skyboxHandle が 0 以上なら true
  bool HasSkybox() const { return skyboxHandle >= 0; }

  std::string skyboxPath; ///< シリアライズ用のアセットパス

  /// @brief コンポーネントの型名を返す
  /// @return "SkyboxComponent"
  const char* TypeName() const override { return "SkyboxComponent"; }

  /// @brief パス・表示フラグ・色を JSON に書き出す
  /// @return skyboxPath / visible / color を格納した JSON
  nlohmann::json Serialize() const override {
    return {
      {"skyboxPath", skyboxPath},
      {"visible", visible},
      {"color", {color.x, color.y, color.z, color.w}}
    };
  }

  /// @brief JSON からパス・表示フラグ・色を読み込む
  /// @param j 読み込み元の JSON（含まれていないキーは現在の値のまま）
  void Deserialize(const nlohmann::json& j) override {
    if (j.contains("skyboxPath")) skyboxPath = j["skyboxPath"].get<std::string>();
    if (j.contains("visible")) visible = j["visible"].get<bool>();
    if (j.contains("color")) {
      auto& c = j["color"];
      color = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>(), c[3].get<float>()};
    }
  }
};
