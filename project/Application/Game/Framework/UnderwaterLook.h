#pragma once
#include "Common/EngineConfig.h"
#include "Common/Math/MathTypes.h"
#include "Graphics/PostProcess/PostProcess.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>

#if RC_ENABLE_IMGUI
#include "imgui/imgui.h"
#endif

/// @brief 水中の見た目（フォグ・光・深さの扱い）の共通定義
/// @details 3 か所が同じ「水中」を出す必要がある。
///            - Title の潜水（TitleScreenScript）        : 水面 → 深海へ暗くなっていく
///            - Game の浮上（DeepRiseIntroScript）       : 深海 → 水面へ明るくなっていく
///            - レール中の潜行（RailShooterController）  : 水面付近の水中
///          それぞれが別々の色や距離を持つと、Title で暗くなりきった画と Game が始まる画が
///          つながらない。ここで「深さ → フォグ」の対応を 1 つに決め、全員がこれを呼ぶ。
///
///          フォグは Underwater.PS.hlsl の
///            foggedColor = lerp(scene, fogColor, (dist - fogStart) / (fogEnd - fogStart))
///            underwater  = foggedColor * tintColor
///          で掛かる。深さ 0（水面直下）では shallow*、deepDepth 以深では deep* の値を使い、
///          その間はなめらかに補間する。最深部の画面色（deepFogColor * tintColor）は
///          kAbyssScreenColor に一致し、SceneManager の Dive 遷移はその色へ抜ける。
namespace UnderwaterLook {

// ------------------------------------------------------------------
// 既定値（Params の初期値。SceneManager が使う画面色もここから作る）
// ------------------------------------------------------------------
inline constexpr RC::Vector4 kDefaultShallowFog = {0.0f, 0.30f, 0.60f, 1.0f}; ///< 水面直下のフォグ色
inline constexpr RC::Vector4 kDefaultDeepFog = {0.0f, 0.012f, 0.03f, 1.0f};   ///< 深海のフォグ色
inline constexpr RC::Vector4 kDefaultTint = {0.2f, 0.5f, 1.0f, 1.0f};         ///< 水中の色味（乗算）

/// @brief 最深部で Underwater シェーダが出す画面色（= deepFog * tint）
/// @details SceneManager の Dive 遷移は Dissolve でこの色へ抜ける。Title 側は
///          この色まで暗くしてから遷移を要求し、Game 側はこの色から浮上を始めるので、
///          切り替えの継ぎ目が見えない。deepFogColor / tintColor の既定値を変えたら
///          ここも合わせること（JSON で個別に変えた場合は継ぎ目がわずかに見える）。
inline constexpr RC::Vector4 kAbyssScreenColor = {
    kDefaultDeepFog.x * kDefaultTint.x, kDefaultDeepFog.y * kDefaultTint.y,
    kDefaultDeepFog.z * kDefaultTint.z, 1.0f};

/// @brief 水中の見た目のパラメータ（JSON / ImGui から調整する）
struct Params {
  // ---- フォグ：水面直下 ----
  RC::Vector4 shallowFogColor = kDefaultShallowFog; ///< フォグ色
  float shallowFogStart = 10.0f;                    ///< フォグが掛かり始める距離（m）
  float shallowFogEnd = 150.0f;                     ///< フォグ色に塗りつぶされる距離（m）

  // ---- フォグ：深海 ----
  RC::Vector4 deepFogColor = kDefaultDeepFog; ///< フォグ色
  float deepFogStart = 0.0f;                  ///< フォグが掛かり始める距離（m）
  float deepFogEnd = 7.0f;                    ///< フォグ色に塗りつぶされる距離（m）

  /// @brief この深さ（m）で完全に深海の見た目になる
  float deepDepth = 24.0f;
  /// @brief 深さ→暗さの偏り。1 で等速、>1 だと浅いうちは明るいまま急に暗くなる
  float deepBias = 1.35f;

  // ---- Underwater シェーダのその他 ----
  RC::Vector4 tintColor = kDefaultTint; ///< フォグ後の色に乗算する色味
  /// @brief 屈折のうねりの振幅（UV。0.004 で 720p 時に約 3px）
  float distortionForce = 0.004f;

  // ---- Caustics（床の網目）/ LightShaft（降り注ぐ光）----
  /// @brief 水面からこの深さ（m）で網目が消えきる
  float causticsFadeDepth = 20.0f;
  /// @brief 網目の密度（ワールド 1m あたりのセル数。0.3 でセル 3m 強）
  float causticsScale = 0.3f;
  float causticsIntensity = 1.2f; ///< 網目の明るさ
  /// @brief カメラからこの距離（m）で網目が薄れ始め / 消えきる（遠景のモアレ防止）
  float causticsDistanceFadeStart = 25.0f;
  float causticsDistanceFadeEnd = 70.0f;
  float lightShaftIntensity = 1.0f; ///< 光柱の明るさ
  /// @brief 深さによる光柱の指数減衰。0.08 で 20m 下は 20% ほど
  float lightShaftDensity = 0.08f;
  float lightShaftMaxDistance = 80.0f; ///< 光柱を描く最大距離（m）

  // ---- JSON ----
  /// @brief JSON へ書き出す（呼び出し側は自分の Serialize の 1 キーに入れる）
  /// @return 全パラメータを持つ JSON オブジェクト（色は [r, g, b, a] の配列）
  nlohmann::json ToJson() const {
    auto v4 = [](const RC::Vector4 &c) { return nlohmann::json{c.x, c.y, c.z, c.w}; };
    return {
        {"shallowFogColor", v4(shallowFogColor)},
        {"shallowFogStart", shallowFogStart},
        {"shallowFogEnd", shallowFogEnd},
        {"deepFogColor", v4(deepFogColor)},
        {"deepFogStart", deepFogStart},
        {"deepFogEnd", deepFogEnd},
        {"deepDepth", deepDepth},
        {"deepBias", deepBias},
        {"tintColor", v4(tintColor)},
        {"distortionForce", distortionForce},
        {"causticsFadeDepth", causticsFadeDepth},
        {"causticsScale", causticsScale},
        {"causticsIntensity", causticsIntensity},
        {"causticsDistanceFadeStart", causticsDistanceFadeStart},
        {"causticsDistanceFadeEnd", causticsDistanceFadeEnd},
        {"lightShaftIntensity", lightShaftIntensity},
        {"lightShaftDensity", lightShaftDensity},
        {"lightShaftMaxDistance", lightShaftMaxDistance},
    };
  }

  /// @brief JSON から読む。無いキーは既定値のまま
  /// @param j 読み込む JSON（オブジェクトでなければ何もしない）
  void FromJson(const nlohmann::json &j) {
    if (!j.is_object()) return;
    auto readF = [&](const char *key, float &out) {
      if (j.contains(key) && j[key].is_number()) out = j[key].get<float>();
    };
    auto readV4 = [&](const char *key, RC::Vector4 &out) {
      if (!j.contains(key)) return;
      const auto &c = j[key];
      if (c.is_array() && c.size() >= 4) {
        out = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>(), c[3].get<float>()};
      }
    };
    readV4("shallowFogColor", shallowFogColor);
    readF("shallowFogStart", shallowFogStart);
    readF("shallowFogEnd", shallowFogEnd);
    readV4("deepFogColor", deepFogColor);
    readF("deepFogStart", deepFogStart);
    readF("deepFogEnd", deepFogEnd);
    readF("deepDepth", deepDepth);
    readF("deepBias", deepBias);
    readV4("tintColor", tintColor);
    readF("distortionForce", distortionForce);
    readF("causticsFadeDepth", causticsFadeDepth);
    readF("causticsScale", causticsScale);
    readF("causticsIntensity", causticsIntensity);
    readF("causticsDistanceFadeStart", causticsDistanceFadeStart);
    readF("causticsDistanceFadeEnd", causticsDistanceFadeEnd);
    readF("lightShaftIntensity", lightShaftIntensity);
    readF("lightShaftDensity", lightShaftDensity);
    readF("lightShaftMaxDistance", lightShaftMaxDistance);
  }
};

/// @brief 深さ（m、水面より下を正）を 0（水面直下）〜 1（深海）へ変換する
/// @param p     パラメータ（deepDepth / deepBias を使う）
/// @param depth 水面からの深さ（m）
/// @return 0〜1 の深さ係数
inline float DepthFactor(const Params &p, float depth) {
  // deepDepth で正規化 → deepBias 乗で偏らせる → smoothstep で両端をなめらかにする
  const float d = (p.deepDepth > 0.001f) ? depth / p.deepDepth : 1.0f;
  float t = std::clamp(d, 0.0f, 1.0f);
  if (p.deepBias > 0.0f && p.deepBias != 1.0f) t = std::pow(t, p.deepBias);
  return t * t * (3.0f - 2.0f * t); // smoothstep
}

/// @brief 深さに応じた Underwater のフォグ・色味を PostProcess へ書く
/// @param pp    書き込み先の PostProcess（nullptr なら何もしない）
/// @param p     パラメータ
/// @param depth 水面からの深さ（m）。水面より上なら 0 として扱う
/// @details 毎フレーム呼んでよい（値は定数バッファへ直接書かれる）。
///          Underwater の lerpFactor はここでは触らない（SetLerp を使う）。
inline void ApplyFogByDepth(PostProcess *pp, const Params &p, float depth) {
  if (!pp) return;
  const float t = DepthFactor(p, (std::max)(depth, 0.0f));
  auto mix = [t](float a, float b) { return a + (b - a) * t; };
  pp->SetUnderwaterFogColor(mix(p.shallowFogColor.x, p.deepFogColor.x),
                            mix(p.shallowFogColor.y, p.deepFogColor.y),
                            mix(p.shallowFogColor.z, p.deepFogColor.z), 1.0f);
  const float start = mix(p.shallowFogStart, p.deepFogStart);
  // fogEnd が fogStart 以下だとシェーダでゼロ割りになるので最低 0.5m は離す
  const float end = (std::max)(mix(p.shallowFogEnd, p.deepFogEnd), start + 0.5f);
  pp->SetUnderwaterFogRange(start, end);
  pp->SetUnderwaterTintColor(p.tintColor.x, p.tintColor.y, p.tintColor.z, p.tintColor.w);
  pp->SetUnderwaterDistortionForce(p.distortionForce);
}

/// @brief Caustics / LightShaft を実際の水面高さに合わせる
/// @param pp          書き込み先の PostProcess（nullptr なら何もしない）
/// @param p           パラメータ（Caustics / LightShaft の項目を使う）
/// @param waterHeight 水面のワールド Y。シーンの WaterComponent の Transform.y を渡す
/// @details PostProcess の既定値（水面 Y=150、距離減衰 300→150）は本作の水面（Y=0）と
///          合っておらず、そのままだと網目も光柱もほぼ出ない。ここで揃える。
/// @param sunDir  光の進む向き（DirectionalLightComponent::direction）。nullptr なら既定の斜め下
inline void SetupLight(PostProcess *pp, const Params &p, float waterHeight,
                       const RC::Vector3 *sunDir = nullptr) {
  if (!pp) return;
  if (sunDir) pp->SetLightShaftSunDirection(sunDir->x, sunDir->y, sunDir->z);
  pp->SetCausticsWater(waterHeight, p.causticsFadeDepth);
  pp->SetCausticsScale(p.causticsScale);
  pp->SetCausticsIntensity(p.causticsIntensity);
  pp->SetCausticsDistanceFade(p.causticsDistanceFadeStart, p.causticsDistanceFadeEnd);
  pp->SetLightShaftIntensity(p.lightShaftIntensity);
  pp->SetLightShaftDensity(p.lightShaftDensity);
  pp->SetLightShaftMaxDistance(p.lightShaftMaxDistance);
}

/// @brief 水中エフェクト一式をスタックへ積む
/// @details 追加順 = 適用順。Underwater は UV を歪めるため、Caustics / LightShaft より
///          後ろに置かないと模様がジオメトリからずれる（RailShooterController と同じ順）。
///          既に積まれているものは AddEffect 側が無視するので、重ねて呼んでも安全。
/// @param pp 対象の PostProcess（nullptr なら何もしない）
inline void AddStack(PostProcess *pp) {
  if (!pp) return;
  pp->AddEffect(PostEffectType::LightShaft);
  pp->AddEffect(PostEffectType::Caustics);
  pp->AddEffect(PostEffectType::Underwater);
  pp->AddEffect(PostEffectType::Vignette); // 密閉感・水圧
}

/// @brief 水中エフェクト一式をスタックから外す
/// @param pp 対象の PostProcess（nullptr なら何もしない）
inline void RemoveStack(PostProcess *pp) {
  if (!pp) return;
  pp->RemoveEffect(PostEffectType::Underwater);
  pp->RemoveEffect(PostEffectType::Caustics);
  pp->RemoveEffect(PostEffectType::LightShaft);
  pp->RemoveEffect(PostEffectType::Vignette);
}

/// @brief 水上⇔水中のブレンド率をまとめて書く（Underwater / Caustics / LightShaft）
/// @param pp   対象の PostProcess（nullptr なら何もしない）
/// @param lerp ブレンド率（0: 水上 〜 1: 完全に水中。範囲外は丸める）
inline void SetLerp(PostProcess *pp, float lerp) {
  if (!pp) return;
  lerp = std::clamp(lerp, 0.0f, 1.0f);
  pp->SetUnderwaterLerpFactor(lerp);
  pp->SetCausticsLerpFactor(lerp);
  pp->SetLightShaftLerpFactor(lerp);
}

/// @brief 0〜1 の直線的な進行度を S 字にする（水面の出入りのブレンドに使う）
/// @param t 進行度（範囲外は 0〜1 に丸める）
/// @return smoothstep を掛けた 0〜1 の値
inline float SmoothStep01(float t) {
  t = std::clamp(t, 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

#if RC_ENABLE_IMGUI
/// @brief Inspector 用の調整 UI（呼び出し側の OnImGui から呼ぶ）
/// @param p 編集するパラメータ
/// @return 何か変更されたら true
inline bool DrawImGui(Params &p) {
  bool changed = false;
  if (ImGui::TreeNode("Underwater Look")) {
    ImGui::TextDisabled("depth 0 = shallow, depth >= deepDepth = abyss");
    changed |= ImGui::ColorEdit3("Shallow Fog", &p.shallowFogColor.x);
    changed |= ImGui::DragFloat("Shallow Fog Start", &p.shallowFogStart, 0.5f, 0.0f, 100.0f);
    changed |= ImGui::DragFloat("Shallow Fog End", &p.shallowFogEnd, 1.0f, 1.0f, 1000.0f);
    changed |= ImGui::ColorEdit3("Deep Fog", &p.deepFogColor.x);
    changed |= ImGui::DragFloat("Deep Fog Start", &p.deepFogStart, 0.1f, 0.0f, 50.0f);
    changed |= ImGui::DragFloat("Deep Fog End", &p.deepFogEnd, 0.1f, 0.5f, 100.0f);
    changed |= ImGui::DragFloat("Deep Depth (m)", &p.deepDepth, 0.5f, 1.0f, 200.0f);
    changed |= ImGui::DragFloat("Deep Bias", &p.deepBias, 0.05f, 0.2f, 4.0f);
    changed |= ImGui::ColorEdit3("Tint", &p.tintColor.x);
    changed |= ImGui::DragFloat("Distortion", &p.distortionForce, 0.001f, 0.0f, 0.1f);
    ImGui::SeparatorText("Caustics / Light Shaft");
    changed |= ImGui::DragFloat("Caustics Fade Depth", &p.causticsFadeDepth, 0.5f, 1.0f, 200.0f);
    changed |= ImGui::DragFloat("Caustics Scale", &p.causticsScale, 0.01f, 0.01f, 5.0f);
    changed |= ImGui::DragFloat("Caustics Intensity", &p.causticsIntensity, 0.05f, 0.0f, 5.0f);
    changed |= ImGui::DragFloat("Caustics Dist Fade Start", &p.causticsDistanceFadeStart, 1.0f, 0.0f, 500.0f);
    changed |= ImGui::DragFloat("Caustics Dist Fade End", &p.causticsDistanceFadeEnd, 1.0f, 1.0f, 500.0f);
    changed |= ImGui::DragFloat("Shaft Intensity", &p.lightShaftIntensity, 0.05f, 0.0f, 5.0f);
    changed |= ImGui::DragFloat("Shaft Density", &p.lightShaftDensity, 0.005f, 0.0f, 1.0f);
    changed |= ImGui::DragFloat("Shaft Max Distance", &p.lightShaftMaxDistance, 1.0f, 1.0f, 500.0f);
    ImGui::TreePop();
  }
  return changed;
}
#endif

} // namespace UnderwaterLook
