#pragma once

#include "IComponent.h"
#include "Math/MathTypes.h"
#include "Common/Water/WaterSurface.h"
#include <string>
#include <nlohmann/json.hpp>

/// @brief Water (ocean/sea) surface rendering component.
/// Holds parameters for wave simulation, water color, and rendering.
/// The actual mesh is a high-subdivision plane generated at runtime.
class WaterComponent : public IComponent {
public:
  int meshHandle = -1;     ///< Internal mesh handle (high-subdiv plane)
  int normalMapHandle = -1; ///< Normal map texture handle
  bool visible = true;     ///< Visibility flag

  // --- Wave Parameters ---
  float waveHeight  = 0.3f;  ///< Primary wave amplitude
  float waveSpeed   = 1.5f;  ///< Primary wave speed
  float waveFreq    = 0.8f;  ///< Primary wave frequency
  float waveHeight2 = 0.15f; ///< Secondary wave amplitude
  float waveSpeed2  = 1.0f;  ///< Secondary wave speed
  float waveFreq2   = 1.2f;  ///< Secondary wave frequency
  float waveSteepness = 0.4f; ///< Gerstner steepness (0..1)

  // --- Water Color ---
  RC::Vector4 shallowColor = {0.1f, 0.5f, 0.6f, 0.85f}; ///< Shallow water color (RGBA)
  RC::Vector4 deepColor    = {0.02f, 0.1f, 0.25f, 0.95f}; ///< Deep water color (RGBA)

  // --- Material Parameters ---
  float fresnelPower = 3.0f;      ///< Fresnel exponent
  float specularPower = 128.0f;   ///< Specular highlight sharpness
  float normalScrollSpeed = 0.03f; ///< Normal map scroll speed
  float normalStrength = 0.6f;    ///< Normal map intensity

  float environmentCoeff = 0.5f;  ///< Environment map reflection coefficient
  float crestTint = 0.0f;         ///< 波の高さによる色付けの強さ（0 で無効。真上視点で山と谷を色で見せる）

  // --- Ocean Realism（OceanWaves.hlsli / Water.PS.hlsl）---
  float detailStrength   = 1.0f;   ///< 詳細波（風向きのまわりの短い波 8 本）の強さ。0 で従来の 3 波だけ
  float choppiness       = 0.6f;   ///< 詳細波の尖り 0..1（山が鋭く、谷が平らになる）
  float whitecapStrength = 0.8f;   ///< 波頭の白波の濃さ（0 で無効）
  float whitecapCoverage = 0.5f;   ///< 白波の量 0..1（大きいほど広く出る）
  RC::Vector4 sssColor   = {0.10f, 0.62f, 0.52f, 0.9f}; ///< 波頭を透ける光の色 (rgb) と強さ (a)。a=0 で無効
  float clarity          = 3.0f;   ///< 透明度 m。奥の物体がこの距離より近いと浅瀬色に寄せて透かす（0 で無効）
  float detailFadeDistance = 150.0f; ///< この距離に向けて細かい法線を弱め、太陽の照り返しを広げる（0 で無効）
  float normalTileSize   = 0.0f;   ///< 法線マップ 1 枚のワールドサイズ m（0 で従来どおり平面の UV に貼る）

  // --- 屈折（スクリーンテクスチャ）---
  bool  refraction         = true;  ///< 水を描く前の画面を法線で歪めて透かす（false で従来の αブレンド）
  float refractionStrength = 0.04f; ///< 歪みの強さ（画面 UV）
  float edgeFade           = 0.5f;  ///< 物体との交差部をぼかす幅 m

  /// @brief 頂点で表現できる最短波長（頂点 4 つぶん）。これより短い詳細波は変位させず、法線だけで描く
  float MinDisplacedWavelength() const {
    const float size = (planeWidth > planeHeight) ? planeWidth : planeHeight;
    const float segs = (segments > 0) ? static_cast<float>(segments) : 1.0f;
    return 4.0f * size / segs;
  }

  /// @brief CPU 側の水面計算（RC::WaterSurface）用のパラメータを組み立てる
  /// @param baseHeight 水面エンティティのワールド Y（静水面）
  RC::WaterWaveParams ToWaveParams(float baseHeight) const {
    RC::WaterWaveParams p;
    p.waveHeight    = waveHeight;
    p.waveSpeed     = waveSpeed;
    p.waveFreq      = waveFreq;
    p.waveHeight2   = waveHeight2;
    p.waveSpeed2    = waveSpeed2;
    p.waveFreq2     = waveFreq2;
    p.waveSteepness = waveSteepness;
    p.baseHeight    = baseHeight;
    p.detail        = detailStrength;
    p.choppiness    = choppiness;
    p.minWavelength = MinDisplacedWavelength();
    return p;
  }

  // --- Mesh Generation ---
  float planeWidth  = 100.0f;  ///< Water plane width
  float planeHeight = 100.0f;  ///< Water plane depth
  uint32_t segments = 128;     ///< Plane subdivision count (per axis)

  std::string normalMapPath;  ///< Normal map texture path for serialization

  /// @brief Check if a valid mesh is assigned
  bool HasMesh() const { return meshHandle >= 0; }

  const char* TypeName() const override { return "WaterComponent"; }

  nlohmann::json Serialize() const override {
    return {
      {"visible", visible},
      {"waveHeight", waveHeight},
      {"waveSpeed", waveSpeed},
      {"waveFreq", waveFreq},
      {"waveHeight2", waveHeight2},
      {"waveSpeed2", waveSpeed2},
      {"waveFreq2", waveFreq2},
      {"waveSteepness", waveSteepness},
      {"shallowColor", {shallowColor.x, shallowColor.y, shallowColor.z, shallowColor.w}},
      {"deepColor", {deepColor.x, deepColor.y, deepColor.z, deepColor.w}},
      {"fresnelPower", fresnelPower},
      {"specularPower", specularPower},
      {"normalScrollSpeed", normalScrollSpeed},
      {"normalStrength", normalStrength},
      {"environmentCoeff", environmentCoeff},
      {"crestTint", crestTint},
      {"detailStrength", detailStrength},
      {"choppiness", choppiness},
      {"whitecapStrength", whitecapStrength},
      {"whitecapCoverage", whitecapCoverage},
      {"sssColor", {sssColor.x, sssColor.y, sssColor.z, sssColor.w}},
      {"clarity", clarity},
      {"detailFadeDistance", detailFadeDistance},
      {"normalTileSize", normalTileSize},
      {"refraction", refraction},
      {"refractionStrength", refractionStrength},
      {"edgeFade", edgeFade},
      {"planeWidth", planeWidth},
      {"planeHeight", planeHeight},
      {"segments", segments},
      {"normalMapPath", normalMapPath}
    };
  }

  void Deserialize(const nlohmann::json& j) override {
    if (j.contains("visible")) visible = j["visible"].get<bool>();
    if (j.contains("waveHeight")) waveHeight = j["waveHeight"].get<float>();
    if (j.contains("waveSpeed")) waveSpeed = j["waveSpeed"].get<float>();
    if (j.contains("waveFreq")) waveFreq = j["waveFreq"].get<float>();
    if (j.contains("waveHeight2")) waveHeight2 = j["waveHeight2"].get<float>();
    if (j.contains("waveSpeed2")) waveSpeed2 = j["waveSpeed2"].get<float>();
    if (j.contains("waveFreq2")) waveFreq2 = j["waveFreq2"].get<float>();
    if (j.contains("waveSteepness")) waveSteepness = j["waveSteepness"].get<float>();
    if (j.contains("shallowColor")) {
      auto& c = j["shallowColor"];
      shallowColor = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>(), c[3].get<float>()};
    }
    if (j.contains("deepColor")) {
      auto& c = j["deepColor"];
      deepColor = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>(), c[3].get<float>()};
    }
    if (j.contains("fresnelPower")) fresnelPower = j["fresnelPower"].get<float>();
    if (j.contains("specularPower")) specularPower = j["specularPower"].get<float>();
    if (j.contains("normalScrollSpeed")) normalScrollSpeed = j["normalScrollSpeed"].get<float>();
    if (j.contains("normalStrength")) normalStrength = j["normalStrength"].get<float>();
    if (j.contains("environmentCoeff")) environmentCoeff = j["environmentCoeff"].get<float>();
    if (j.contains("crestTint")) crestTint = j["crestTint"].get<float>();
    if (j.contains("detailStrength")) detailStrength = j["detailStrength"].get<float>();
    if (j.contains("choppiness")) choppiness = j["choppiness"].get<float>();
    if (j.contains("whitecapStrength")) whitecapStrength = j["whitecapStrength"].get<float>();
    if (j.contains("whitecapCoverage")) whitecapCoverage = j["whitecapCoverage"].get<float>();
    if (j.contains("sssColor")) {
      auto& c = j["sssColor"];
      sssColor = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>(), c[3].get<float>()};
    }
    if (j.contains("clarity")) clarity = j["clarity"].get<float>();
    if (j.contains("detailFadeDistance")) detailFadeDistance = j["detailFadeDistance"].get<float>();
    if (j.contains("normalTileSize")) normalTileSize = j["normalTileSize"].get<float>();
    if (j.contains("refraction")) refraction = j["refraction"].get<bool>();
    if (j.contains("refractionStrength")) refractionStrength = j["refractionStrength"].get<float>();
    if (j.contains("edgeFade")) edgeFade = j["edgeFade"].get<float>();
    if (j.contains("planeWidth")) planeWidth = j["planeWidth"].get<float>();
    if (j.contains("planeHeight")) planeHeight = j["planeHeight"].get<float>();
    if (j.contains("segments")) segments = j["segments"].get<uint32_t>();
    if (j.contains("normalMapPath")) normalMapPath = j["normalMapPath"].get<std::string>();
  }
};
