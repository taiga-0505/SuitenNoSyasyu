#pragma once
#include "ECS/ScriptableEntity.h"
#include "ECS/TransformComponent.h"
#include "ECS/CameraComponent.h"
#include "Game/Framework/WaterCameraFx.h"
#include "Scene.h"

#if RC_ENABLE_IMGUI
#include "imgui/imgui.h"
#endif

#include <cmath>

/// @brief メインカメラの z が fromZ → toZ と進むあいだに、何かを 0 → 1 でなめらかに切り替えるスクリプトの基底
/// @details 洞窟の照明（CaveLightZoneScript）とヘッドライト（CaveHeadlampScript）は、
///          「再生中だけ」「メインカメラを探して」「z を fromZ〜toZ で 0〜1 に正規化（smoothstep）して」
///          「その値で何かを書き換える」という同じ流れを、それぞれに書き写していた。
///          流れを ApplyZone に固定し（Template Method）、違う部分
///          （書き換えてよい条件 CanApplyZone と、書き換える中身 OnZoneBlend）だけを
///          派生クラスが仮想関数で差し替える形にした（ポリモーフィズム）。
///
///          再生中だけ書き換えるのは、編集中に書き換えるとその値がシーンの保存で JSON に焼き付くため。
///          JSON のキー "fromZ" / "toZ" はこの基底クラスが読み書きする。
class CameraZoneScript : public ScriptableEntity {
protected:
  /// @brief 補間範囲の既定値を指定して作る
  /// @param fromZ 補間を始める z の既定値
  /// @param toZ 補間を終える z の既定値
  CameraZoneScript(float fromZ, float toZ) : fromZ_(fromZ), toZ_(toZ) {}

  /// @brief メインカメラの z から補間率を求め、OnZoneBlend へ渡す（流れは固定）
  /// @param dt 経過時間（秒）。OnZoneBlend へそのまま渡す
  void ApplyZone(float dt) {
    SceneContext *ctx = GetSceneContext();
    if (!ctx || !ctx->isPlaying()) return;
    if (!CanApplyZone()) return;
    auto cam = WaterCameraFx::FindMainCamera(GetScene());
    if (!cam) return;
    auto *camTr = cam->GetComponent<TransformComponent>();
    if (!camTr) return;

    // カメラの z を fromZ〜toZ で 0〜1 に正規化（逆向きも可）。範囲がほぼ 0 なら toZ を境に 0 / 1 で切り替える
    const float span = toZ_ - fromZ_;
    const float t = (std::fabs(span) < 1e-3f) ? (camTr->position.z >= toZ_ ? 1.0f : 0.0f)
                                              : WaterCameraFx::Smooth01((camTr->position.z - fromZ_) / span);
    blend_ = t;
    OnZoneBlend(*camTr, t, dt);
  }

  /// @brief 書き換えに必要なコンポーネントがそろっているか（カメラを探す前に見る）
  /// @return そろっていれば true（既定は true）
  virtual bool CanApplyZone() { return true; }

  /// @brief 補間率に応じて書き換える
  /// @param camTr メインカメラの Transform
  /// @param t 補間率（0: fromZ 側 〜 1: toZ 側）
  /// @param dt 経過時間（秒）
  virtual void OnZoneBlend(const TransformComponent &camTr, float t, float dt) = 0;

  /// @brief 補間範囲を JSON に書き足す
  /// @param j 書き込み先
  void WriteZone(nlohmann::json &j) const {
    j["fromZ"] = fromZ_;
    j["toZ"] = toZ_;
  }

  /// @brief 補間範囲を JSON から読み込む
  /// @param j 読み込む JSON（無いキーは現在の値のまま）
  void ReadZone(const nlohmann::json &j) {
    if (j.contains("fromZ")) fromZ_ = j["fromZ"].get<float>();
    if (j.contains("toZ")) toZ_ = j["toZ"].get<float>();
  }

  /// @brief Inspector に現在の補間率と補間範囲の編集 UI を表示する
  void DrawZoneImGui() {
#if RC_ENABLE_IMGUI
    ImGui::Text("blend = %.2f", blend_);
    ImGui::DragFloat("From Z", &fromZ_, 0.5f);
    ImGui::DragFloat("To Z", &toZ_, 0.5f);
#endif
  }

private:
  float fromZ_;        ///< 補間を始める z（カメラがここにいるとき 0）
  float toZ_;          ///< 補間を終える z（カメラがここにいるとき 1）
  float blend_ = 0.0f; ///< 現在の補間率（0〜1。Inspector 表示用）
};
