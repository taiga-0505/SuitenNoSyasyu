#pragma once

#include "IComponent.h"
#include "Math/MathTypes.h"
#include "Common/Water/WaterSurface.h"
#include <string>
#include <nlohmann/json.hpp>

/// @class WaterComponent
/// @brief 水面（海・湖）を描画するコンポーネント
/// @details 波のシミュレーション・水の色・描画のパラメータを持つ。
///          実際のメッシュは実行時に生成する細かく分割した平面。
class WaterComponent : public IComponent {
public:
  int meshHandle = -1;     ///< 内部で生成したメッシュのハンドル（細分割した平面）
  int normalMapHandle = -1; ///< 法線マップのテクスチャハンドル
  bool visible = true;     ///< 表示するか

  // --- 波のパラメータ ---
  float waveHeight  = 0.3f;  ///< 第 1 波の振幅
  float waveSpeed   = 1.5f;  ///< 第 1 波の速さ
  float waveFreq    = 0.8f;  ///< 第 1 波の周波数
  float waveHeight2 = 0.15f; ///< 第 2 波の振幅
  float waveSpeed2  = 1.0f;  ///< 第 2 波の速さ
  float waveFreq2   = 1.2f;  ///< 第 2 波の周波数
  float waveSteepness = 0.4f; ///< Gerstner 波の尖り（0〜1）

  // --- 水の色 ---
  RC::Vector4 shallowColor = {0.1f, 0.5f, 0.6f, 0.85f}; ///< 浅瀬の色（RGBA）
  RC::Vector4 deepColor    = {0.02f, 0.1f, 0.25f, 0.95f}; ///< 深みの色（RGBA）

  // --- マテリアルのパラメータ ---
  float fresnelPower = 3.0f;      ///< フレネルの指数
  float specularPower = 128.0f;   ///< 鏡面ハイライトの鋭さ
  float normalScrollSpeed = 0.03f; ///< 法線マップのスクロール速度
  float normalStrength = 0.6f;    ///< 法線マップの強さ

  float environmentCoeff = 0.5f;  ///< 環境マップの映り込み係数
  float crestTint = 0.0f;         ///< 波の高さによる色付けの強さ（0 で無効。真上視点で山と谷を色で見せる）

  // --- 海のリアルさ（OceanWaves.hlsli / Water.PS.hlsl）---
  float detailStrength   = 1.0f;   ///< 詳細波（風向きのまわりの短い波 8 本）の強さ。0 で従来の 3 波だけ
  float choppiness       = 0.6f;   ///< 詳細波の尖り（0〜1。山が鋭く、谷が平らになる）
  float whitecapStrength = 0.8f;   ///< 波頭の白波の濃さ（0 で無効）
  float whitecapCoverage = 0.5f;   ///< 白波の量（0〜1。大きいほど広く出る）
  RC::Vector4 sssColor   = {0.10f, 0.62f, 0.52f, 0.9f}; ///< 波頭を透ける光の色 (rgb) と強さ (a)。a=0 で無効
  float clarity          = 3.0f;   ///< 透明度 m。奥の物体がこの距離より近いと浅瀬色に寄せて透かす（0 で無効）
  float detailFadeDistance = 150.0f; ///< この距離に向けて細かい法線を弱め、太陽の照り返しを広げる（0 で無効）
  float normalTileSize   = 0.0f;   ///< 法線マップ 1 枚のワールドサイズ m（0 で従来どおり平面の UV に貼る）

  // --- 屈折（スクリーンテクスチャ）---
  bool  refraction         = true;  ///< 水を描く前の画面を法線で歪めて透かす（false で従来の αブレンド）
  float refractionStrength = 0.04f; ///< 歪みの強さ（画面 UV）
  float edgeFade           = 0.5f;  ///< 物体との交差部をぼかす幅 m

  /// @brief 頂点で表現できる最短波長（頂点 4 つぶん）。これより短い詳細波は変位させず、法線だけで描く
  /// @return 平面の長辺を segments で割った間隔の 4 倍（ワールド単位）
  float MinDisplacedWavelength() const {
    const float size = (planeWidth > planeHeight) ? planeWidth : planeHeight;
    const float segs = (segments > 0) ? static_cast<float>(segments) : 1.0f;
    return 4.0f * size / segs;
  }

  /// @brief CPU 側の水面計算（RC::WaterSurface）用のパラメータを組み立てる
  /// @param baseHeight 水面エンティティのワールド Y（静水面）
  /// @return 波のパラメータを詰めた WaterWaveParams
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

  // --- メッシュ生成 ---
  float planeWidth  = 100.0f;  ///< 水面の幅
  float planeHeight = 100.0f;  ///< 水面の奥行き
  uint32_t segments = 128;     ///< 平面の分割数（1 軸あたり）

  std::string normalMapPath;  ///< シリアライズ用の法線マップのパス

  /// @brief 有効なメッシュが設定されているかを返す
  /// @return meshHandle が 0 以上なら true
  bool HasMesh() const { return meshHandle >= 0; }

  /// @brief コンポーネントの型名を返す
  /// @return "WaterComponent"
  const char* TypeName() const override { return "WaterComponent"; }

  /// @brief 波・色・マテリアル・メッシュ生成のパラメータを JSON に書き出す
  /// @return ハンドル以外の設定値をすべて格納した JSON
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

  /// @brief JSON から波・色・マテリアル・メッシュ生成のパラメータを読み込む
  /// @param j 読み込み元の JSON（含まれていないキーは現在の値のまま）
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
