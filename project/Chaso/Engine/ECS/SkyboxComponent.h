#pragma once

#include "IComponent.h"
#include "Math/MathTypes.h"
#include "Skybox/ProceduralSkySettings.h"
#include <string>
#include <nlohmann/json.hpp>

/// @class SkyboxComponent
/// @brief スカイボックスを描画するコンポーネント
/// @details RC::CreateSkyBox()（DDS キューブマップ）か RC::CreateProceduralSkyBox()（プロシージャル空）で
///          作ったハンドルを保持する。procedural が true ならプロシージャル空を使い、skyboxPath は使わない。
class SkyboxComponent : public IComponent {
public:
  int skyboxHandle = -1;  ///< スカイボックスのハンドル（未設定なら -1）
  bool visible = true;    ///< 表示するか
  RC::Vector4 color = {1.0f, 1.0f, 1.0f, 1.0f}; ///< 乗算色

  /// @brief 有効なハンドルが設定されているかを返す
  /// @return skyboxHandle が 0 以上なら true
  bool HasSkybox() const { return skyboxHandle >= 0; }

  std::string skyboxPath; ///< シリアライズ用のアセットパス

  bool procedural = false;        ///< true ならプロシージャル空（グラデーション・太陽・雲を GPU で描く）を使う
  RC::ProceduralSkySettings sky;  ///< プロシージャル空の見た目（procedural のときだけ使う）

  /// @brief コンポーネントの型名を返す
  /// @return "SkyboxComponent"
  const char* TypeName() const override { return "SkyboxComponent"; }

  /// @brief パス・表示フラグ・色・プロシージャル空の設定を JSON に書き出す
  /// @return skyboxPath / visible / color / procedural / proceduralSky を格納した JSON
  nlohmann::json Serialize() const override {
    auto v3 = [](const RC::Vector3& v) { return nlohmann::json::array({v.x, v.y, v.z}); };
    return {
      {"skyboxPath", skyboxPath},
      {"visible", visible},
      {"color", {color.x, color.y, color.z, color.w}},
      {"procedural", procedural},
      {"proceduralSky", {
        {"zenithColor", v3(sky.zenithColor)},
        {"horizonColor", v3(sky.horizonColor)},
        {"groundColor", v3(sky.groundColor)},
        {"gradientExponent", sky.gradientExponent},
        {"horizonHaze", sky.horizonHaze},
        {"sunFollowsLight", sky.sunFollowsLight},
        {"sunDirection", v3(sky.sunDirection)},
        {"sunColor", v3(sky.sunColor)},
        {"sunSizeDeg", sky.sunSizeDeg},
        {"sunIntensity", sky.sunIntensity},
        {"sunGlow", sky.sunGlow},
        {"sunGlowPower", sky.sunGlowPower},
        {"cloudCoverage", sky.cloudCoverage},
        {"cloudSoftness", sky.cloudSoftness},
        {"cloudScale", sky.cloudScale},
        {"cloudOpacity", sky.cloudOpacity},
        {"cloudColor", v3(sky.cloudColor)},
        {"cloudShadowColor", v3(sky.cloudShadowColor)},
        {"cloudSpeed", sky.cloudSpeed},
        {"cloudDirectionDeg", sky.cloudDirectionDeg},
      }}
    };
  }

  /// @brief JSON からパス・表示フラグ・色・プロシージャル空の設定を読み込む
  /// @param j 読み込み元の JSON（含まれていないキーは現在の値のまま）
  void Deserialize(const nlohmann::json& j) override {
    if (j.contains("skyboxPath")) skyboxPath = j["skyboxPath"].get<std::string>();
    if (j.contains("visible")) visible = j["visible"].get<bool>();
    if (j.contains("color")) {
      auto& c = j["color"];
      color = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>(), c[3].get<float>()};
    }
    if (j.contains("procedural")) procedural = j["procedural"].get<bool>();
    if (j.contains("proceduralSky")) {
      const auto& p = j["proceduralSky"];
      auto rd3 = [&p](const char* key, RC::Vector3& out) {
        if (p.contains(key) && p[key].is_array() && p[key].size() >= 3) {
          out = {p[key][0].get<float>(), p[key][1].get<float>(), p[key][2].get<float>()};
        }
      };
      auto rdf = [&p](const char* key, float& out) {
        if (p.contains(key)) out = p[key].get<float>();
      };
      rd3("zenithColor", sky.zenithColor);
      rd3("horizonColor", sky.horizonColor);
      rd3("groundColor", sky.groundColor);
      rdf("gradientExponent", sky.gradientExponent);
      rdf("horizonHaze", sky.horizonHaze);
      if (p.contains("sunFollowsLight")) sky.sunFollowsLight = p["sunFollowsLight"].get<bool>();
      rd3("sunDirection", sky.sunDirection);
      rd3("sunColor", sky.sunColor);
      rdf("sunSizeDeg", sky.sunSizeDeg);
      rdf("sunIntensity", sky.sunIntensity);
      rdf("sunGlow", sky.sunGlow);
      rdf("sunGlowPower", sky.sunGlowPower);
      rdf("cloudCoverage", sky.cloudCoverage);
      rdf("cloudSoftness", sky.cloudSoftness);
      rdf("cloudScale", sky.cloudScale);
      rdf("cloudOpacity", sky.cloudOpacity);
      rd3("cloudColor", sky.cloudColor);
      rd3("cloudShadowColor", sky.cloudShadowColor);
      rdf("cloudSpeed", sky.cloudSpeed);
      rdf("cloudDirectionDeg", sky.cloudDirectionDeg);
    }
  }
};
