#pragma once

#include "IComponent.h"
#include "Math/MathTypes.h"
#include <string>
#include <nlohmann/json.hpp>

/// @class SkydomeComponent
/// @brief スカイドームを描画するコンポーネント
/// @details RC::GenerateSkydomeEx() などで作ったハンドルを保持する。
class SkydomeComponent : public IComponent {
public:
  int skydomeHandle = -1; ///< スカイドームのハンドル（未設定なら -1）
  bool visible = true;    ///< 表示するか
  RC::Vector4 color = {1.0f, 1.0f, 1.0f, 1.0f}; ///< 乗算色
  int texOverride = -1;   ///< 差し替えるテクスチャのハンドル（-1 なら既定のテクスチャ）

  /// @brief 有効なハンドルが設定されているかを返す
  /// @return skydomeHandle が 0 以上なら true
  bool HasSkydome() const { return skydomeHandle >= 0; }

  std::string skydomePath; ///< シリアライズ用のアセットパス
  std::string texturePath; ///< シリアライズ用のテクスチャのアセットパス

  /// @brief コンポーネントの型名を返す
  /// @return "SkydomeComponent"
  const char* TypeName() const override { return "SkydomeComponent"; }

  /// @brief パス・表示フラグ・色を JSON に書き出す
  /// @return skydomePath / visible / color / texturePath を格納した JSON
  nlohmann::json Serialize() const override {
    return {
      {"skydomePath", skydomePath},
      {"visible", visible},
      {"color", {color.x, color.y, color.z, color.w}},
      {"texturePath", texturePath}
    };
  }

  /// @brief JSON からパス・表示フラグ・色を読み込む
  /// @param j 読み込み元の JSON（含まれていないキーは現在の値のまま）
  void Deserialize(const nlohmann::json& j) override {
    if (j.contains("skydomePath")) skydomePath = j["skydomePath"].get<std::string>();
    if (j.contains("visible")) visible = j["visible"].get<bool>();
    if (j.contains("color")) {
      auto& c = j["color"];
      color = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>(), c[3].get<float>()};
    }
    if (j.contains("texturePath")) texturePath = j["texturePath"].get<std::string>();
  }
};
