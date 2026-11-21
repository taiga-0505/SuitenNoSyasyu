#pragma once

#include "IComponent.h"
#include "Math/MathTypes.h"
#include <nlohmann/json.hpp>

/// @class ModelRendererComponent
/// @brief 3D モデルを描画するコンポーネント
/// @details モデルハンドルを保持し、RenderSystem が自動で描画する。
class ModelRendererComponent : public IComponent {
public:
  int modelHandle = -1;   ///< モデルハンドル（RC::LoadModel() の戻り値）
  int texOverride = -1;   ///< 差し替えるテクスチャのハンドル（-1 なら既定のテクスチャ）
  int normalMapOverride = -1; ///< 差し替える法線マップのハンドル
  int roughnessMapOverride = -1; ///< 差し替えるラフネスマップのハンドル
  bool visible = true;    ///< 表示するか
  int blendMode = 0;      ///< ブレンドモード
  RC::Vector4 color = {1.0f, 1.0f, 1.0f, 1.0f}; ///< 乗算色
  float environmentCoeff = 0.0f; ///< 環境マップの映り込み係数
  int lightingMode = -1;  ///< -1: DirectionalLight の設定に従う / 0: なし 1: Lambert 2: Half Lambert

  /// @brief 有効なモデルが設定されているかを返す
  /// @return modelHandle が 0 以上なら true
  bool HasModel() const { return modelHandle >= 0; }

  std::string modelPath; ///< シリアライズ用のアセットパス
  std::string texturePath; ///< シリアライズ用のテクスチャのアセットパス
  std::string normalMapPath; ///< シリアライズ用の法線マップのアセットパス
  std::string roughnessMapPath; ///< シリアライズ用のラフネスマップのアセットパス

  /// @brief コンポーネントの型名を返す
  /// @return "ModelRendererComponent"
  const char* TypeName() const override { return "ModelRendererComponent"; }

  /// @brief アセットパスと描画設定を JSON に書き出す
  /// @return 各アセットパス・visible・blendMode・color・environmentCoeff・lightingMode を格納した JSON
  nlohmann::json Serialize() const override {
    return {
      {"modelPath", modelPath},
      {"texturePath", texturePath},
      {"normalMapPath", normalMapPath},
      {"roughnessMapPath", roughnessMapPath},
      {"visible", visible},
      {"blendMode", blendMode},
      {"color", {color.x, color.y, color.z, color.w}},
      {"environmentCoeff", environmentCoeff},
      {"lightingMode", lightingMode}
    };
  }

  /// @brief JSON からアセットパスと描画設定を読み込む
  /// @param j 読み込み元の JSON（含まれていないキーは現在の値のまま）
  void Deserialize(const nlohmann::json& j) override {
    if (j.contains("modelPath")) modelPath = j["modelPath"].get<std::string>();
    if (j.contains("texturePath")) texturePath = j["texturePath"].get<std::string>();
    if (j.contains("normalMapPath")) normalMapPath = j["normalMapPath"].get<std::string>();
    if (j.contains("roughnessMapPath")) roughnessMapPath = j["roughnessMapPath"].get<std::string>();
    // texOverride の後方互換が必要になったらここで扱う
    if (j.contains("visible")) visible = j["visible"].get<bool>();
    if (j.contains("blendMode")) blendMode = j["blendMode"].get<int>();
    if (j.contains("color")) {
      auto& c = j["color"];
      color = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>(), c[3].get<float>()};
    }
    if (j.contains("environmentCoeff")) environmentCoeff = j["environmentCoeff"].get<float>();
    if (j.contains("lightingMode")) lightingMode = j["lightingMode"].get<int>();
  }
};
