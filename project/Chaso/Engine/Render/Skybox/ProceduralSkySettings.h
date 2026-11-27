#pragma once
#include "Math/MathTypes.h"

namespace RC {

/// @struct ProceduralSkySettings
/// @brief プロシージャル空（ProceduralSky）の見た目の設定
/// @details 色はリニア空間の値（RTV が sRGB ビューなので、画面ではガンマが掛かって明るく見える）。
///          SkyboxComponent がシーン JSON に保存し、毎フレーム RC::SetProceduralSky で渡す。
struct ProceduralSkySettings {
  // ── 空のグラデーション ──
  RC::Vector3 zenithColor{0.04f, 0.18f, 0.62f};  ///< 天頂の色
  RC::Vector3 horizonColor{0.45f, 0.70f, 0.92f}; ///< 地平線の色
  RC::Vector3 groundColor{0.20f, 0.40f, 0.50f};  ///< 地平線より下の色（水面の反射で少しだけ見える）
  float gradientExponent = 0.45f;                ///< 天頂へ向かうグラデーションの曲がり（小さいほど早く天頂色になる）
  float horizonHaze = 0.5f;                      ///< 地平線付近を白っぽくかすませる強さ（0〜1）

  // ── 太陽 ──
  bool sunFollowsLight = true;                   ///< true なら太陽の向きをアクティブな平行光源から取る
  RC::Vector3 sunDirection{0.5f, 0.8f, -0.5f};   ///< 太陽の方向（太陽へ向かう向き。sunFollowsLight が false のとき使う）
  RC::Vector3 sunColor{1.0f, 0.92f, 0.78f};      ///< 太陽の色
  float sunSizeDeg = 2.0f;                       ///< 太陽の見かけの半径（度）
  float sunIntensity = 8.0f;                     ///< 太陽本体の明るさ
  float sunGlow = 0.5f;                          ///< 太陽のまわりの光の広がりの強さ
  float sunGlowPower = 12.0f;                    ///< 太陽のまわりの光の鋭さ（大きいほど狭い）

  // ── 雲 ──
  float cloudCoverage = 0.45f;                   ///< 雲の量（0 で無し、1 で全天）
  float cloudSoftness = 0.25f;                   ///< 雲の縁のぼかし幅
  float cloudScale = 1.2f;                       ///< 雲の模様の細かさ（大きいほど小さな雲が多い）
  float cloudOpacity = 0.95f;                    ///< 雲の不透明度
  RC::Vector3 cloudColor{1.0f, 1.0f, 1.0f};      ///< 雲の日なた側の色
  RC::Vector3 cloudShadowColor{0.45f, 0.55f, 0.68f}; ///< 雲の影側の色
  float cloudSpeed = 0.01f;                      ///< 雲の流れる速さ（0 で止まり、空の再計算もしない）
  float cloudDirectionDeg = 30.0f;               ///< 雲の流れる向き（度。+X から +Z へ回る向き）
};

} // namespace RC
