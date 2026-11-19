#include "ECS/ScriptableEntity.h"
#include "ECS/ScriptRegistry.h"
#include "ECS/LightComponent.h"
#include "ECS/TransformComponent.h"
#include "ECS/WaterComponent.h"
#include "ECS/CameraComponent.h"
#include "Game/Framework/WaterCameraFx.h"
#include "Scene.h"
#include "CameraZoneScript.h"

#if RC_ENABLE_IMGUI
#include "imgui/imgui.h"
#endif

#include <algorithm>
#include <cmath>

/// @class CaveLightZoneScript
/// @brief 屋外 ⇔ 洞窟を 1 面の中で行き来するとき、カメラの位置で太陽光と環境光を切り替える
/// @details
///   平行光源には影が無いので、洞窟の中に入っても天井に遮られず外と同じ明るさで照らされてしまう。
///   そこで、メインカメラの z が fromZ → toZ と進むあいだに、平行光源の強さ・色・環境光と
///   水面の映り込み（WaterComponent::environmentCoeff）を a → b へなめらかに補間する。
///     Stage2（洞窟に入っていく）: a = 屋外, b = 洞窟内
///     Stage5（洞窟から抜ける）  : a = 洞窟内, b = 屋外
///
///   置き場所: Directional Light のエンティティ（DirectionalLightComponent を持つもの）。
///   再生中だけ書き換える。編集中に書き換えると、その値がシーンの保存で JSON に焼き付くため。
///   「再生中だけ・カメラの z で 0〜1」の流れは基底の CameraZoneScript（Template Method）が持ち、
///   このクラスは照明と水面への書き込み（OnZoneBlend）だけを担当する。
///
///   JSON (scriptDataList):
///     "fromZ" / "toZ" : 補間を始める／終える z（fromZ > toZ でもよい）
///     "a" / "b"       : { "intensity", "color"[4], "ambientColor"[3], "ambientIntensity", "waterEnvironment",
///                          "waterShallow"[4], "waterDeep"[4] }（水面の色は a / b 両方にあるときだけ補間）
class CaveLightZoneScript : public CameraZoneScript {
public:
  /// @brief 既定の補間範囲（z = 0 → 30）で作る
  CaveLightZoneScript() : CameraZoneScript(0.0f, 30.0f) {}

  /// @brief 補間範囲と a / b の見た目を JSON に書き出す
  /// @return 設定値を持つ JSON オブジェクト
  nlohmann::json Serialize() override {
    nlohmann::json j = {{"a", a_.ToJson()}, {"b", b_.ToJson()}};
    WriteZone(j);
    return j;
  }
  /// @brief JSON から補間範囲と a / b の見た目を読み込む
  /// @param j 読み込む JSON（無いキーは現在の値のまま）
  void Deserialize(const nlohmann::json &j) override {
    ReadZone(j);
    if (j.contains("a")) a_.FromJson(j["a"]);
    if (j.contains("b")) b_.FromJson(j["b"]);
  }

  /// @brief Inspector に現在の補間率と、補間範囲・a / b の照明の編集 UI を表示する
  /// @note 水面の色（waterShallow / waterDeep）は UI からは編集できない。
  void OnImGui() override {
#if RC_ENABLE_IMGUI
    DrawZoneImGui();
    auto edit = [](const char *label, Look &l) {
      if (ImGui::TreeNode(label)) {
        ImGui::DragFloat("Intensity", &l.intensity, 0.01f, 0.0f, 4.0f);
        ImGui::ColorEdit4("Color", &l.color.x);
        ImGui::ColorEdit3("Ambient Color", &l.ambientColor.x);
        ImGui::DragFloat("Ambient Intensity", &l.ambientIntensity, 0.01f, 0.0f, 2.0f);
        ImGui::DragFloat("Water Environment", &l.waterEnvironment, 0.01f, 0.0f, 1.0f);
        ImGui::TreePop();
      }
    };
    edit("A (from)", a_);
    edit("B (to)", b_);
#endif
  }

protected:
  /// @brief 生成直後の時点のカメラ位置で照明を反映する
  void OnCreate() override { ApplyZone(0.0f); }
  /// @brief 毎フレーム、カメラ位置に応じて照明を反映する
  /// @param dt 経過時間（秒）
  void OnUpdate(float dt) override { ApplyZone(dt); }

  /// @brief 平行光源と水面へ a → b の補間値を書き込む
  /// @param camTr メインカメラの Transform（未使用）
  /// @param t 補間率（0: a 〜 1: b）
  /// @param dt 経過時間（未使用）
  void OnZoneBlend(const TransformComponent &camTr, float t, float dt) override {
    (void)camTr;
    (void)dt;
    // 自分（Directional Light）の強さ・色・環境光を補間
    if (auto *light = GetComponent<DirectionalLightComponent>()) {
      light->intensity = Lerp(a_.intensity, b_.intensity, t);
      light->color = {Lerp(a_.color.x, b_.color.x, t), Lerp(a_.color.y, b_.color.y, t),
                      Lerp(a_.color.z, b_.color.z, t), Lerp(a_.color.w, b_.color.w, t)};
      light->ambientColor = {Lerp(a_.ambientColor.x, b_.ambientColor.x, t),
                             Lerp(a_.ambientColor.y, b_.ambientColor.y, t),
                             Lerp(a_.ambientColor.z, b_.ambientColor.z, t)};
      light->ambientIntensity = Lerp(a_.ambientIntensity, b_.ambientIntensity, t);
    }
    // シーン内で最初に見つかった水面の映り込み（と、a / b 両方にあれば水の色）を補間
    if (Scene *scene = GetScene()) {
      for (const auto &e : scene->GetEntities()) {
        if (!e) continue;
        if (auto *water = e->GetComponent<WaterComponent>()) {
          water->environmentCoeff = Lerp(a_.waterEnvironment, b_.waterEnvironment, t);
          if (a_.hasWaterColor && b_.hasWaterColor) {
            auto lerp4 = [&](const RC::Vector4 &x, const RC::Vector4 &y) {
              return RC::Vector4{Lerp(x.x, y.x, t), Lerp(x.y, y.y, t), Lerp(x.z, y.z, t), Lerp(x.w, y.w, t)};
            };
            water->shallowColor = lerp4(a_.waterShallow, b_.waterShallow);
            water->deepColor = lerp4(a_.waterDeep, b_.waterDeep);
          }
          break;
        }
      }
    }
  }

private:
  /// @brief 補間の端点（屋外 / 洞窟内）の照明と水面の見た目
  /// @details シーン JSON の "a" / "b" と 1 対 1 の素朴な設定値の束なので struct のままにしている
  ///          （不変条件が無く、ImGui から各値を直接編集するため）。外からは見えないよう private に置く。
  struct Look {
    float intensity = 1.0f;                       ///< 平行光源の強さ
    RC::Vector4 color = {1.0f, 1.0f, 1.0f, 1.0f}; ///< 平行光源の色
    RC::Vector3 ambientColor = {1.0f, 1.0f, 1.0f}; ///< 環境光の色
    float ambientIntensity = 0.0f;                ///< 環境光の強さ
    float waterEnvironment = 0.6f;                ///< 水面の映り込み（WaterComponent::environmentCoeff）
    /// @brief 水面の色（浅い所・深い所）。両方の Look で hasWaterColor が true のときだけ補間する
    bool hasWaterColor = false;
    RC::Vector4 waterShallow = {0.10f, 0.50f, 0.60f, 0.85f}; ///< 水面の浅い所の色
    RC::Vector4 waterDeep = {0.02f, 0.10f, 0.25f, 0.95f};    ///< 水面の深い所の色

    /// @brief JSON に書き出す
    /// @return 照明の値を持つ JSON オブジェクト（水面の色は hasWaterColor のときだけ含める）
    nlohmann::json ToJson() const {
      nlohmann::json j = {{"intensity", intensity},
              {"color", {color.x, color.y, color.z, color.w}},
              {"ambientColor", {ambientColor.x, ambientColor.y, ambientColor.z}},
              {"ambientIntensity", ambientIntensity},
              {"waterEnvironment", waterEnvironment}};
      if (hasWaterColor) {
        j["waterShallow"] = {waterShallow.x, waterShallow.y, waterShallow.z, waterShallow.w};
        j["waterDeep"] = {waterDeep.x, waterDeep.y, waterDeep.z, waterDeep.w};
      }
      return j;
    }
    /// @brief JSON から読み込む
    /// @param j 読み込む JSON（オブジェクトでなければ何もしない。無いキーは現在の値のまま）
    /// @note waterShallow / waterDeep が両方そろっているときだけ hasWaterColor が true になる。
    void FromJson(const nlohmann::json &j) {
      if (!j.is_object()) return;
      if (j.contains("intensity")) intensity = j["intensity"].get<float>();
      if (j.contains("color") && j["color"].size() >= 4) {
        const auto &c = j["color"];
        color = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>(), c[3].get<float>()};
      }
      if (j.contains("ambientColor") && j["ambientColor"].size() >= 3) {
        const auto &c = j["ambientColor"];
        ambientColor = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>()};
      }
      if (j.contains("ambientIntensity")) ambientIntensity = j["ambientIntensity"].get<float>();
      if (j.contains("waterEnvironment")) waterEnvironment = j["waterEnvironment"].get<float>();
      auto readV4 = [&](const char *key, RC::Vector4 &out) {
        if (!j.contains(key) || !j[key].is_array() || j[key].size() < 4) return false;
        const auto &c = j[key];
        out = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>(), c[3].get<float>()};
        return true;
      };
      const bool s = readV4("waterShallow", waterShallow);
      const bool d = readV4("waterDeep", waterDeep);
      hasWaterColor = s && d;
    }
  };

  /// @brief 線形補間
  /// @param x t = 0 のときの値
  /// @param y t = 1 のときの値
  /// @param t 補間率
  /// @return x + (y - x) * t
  static float Lerp(float x, float y, float t) { return x + (y - x) * t; }

  Look a_; ///< fromZ 側の見た目（既定は屋外）
  Look b_{0.3f, {0.7f, 0.72f, 0.8f, 1.0f}, {0.5f, 0.5f, 0.58f}, 0.25f, 0.12f}; ///< toZ 側の見た目（既定は洞窟内）
};

REGISTER_SCRIPT(CaveLightZoneScript)
