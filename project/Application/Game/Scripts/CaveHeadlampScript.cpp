#include "ECS/ScriptableEntity.h"
#include "ECS/ScriptRegistry.h"
#include "ECS/LightComponent.h"
#include "ECS/TransformComponent.h"
#include "ECS/CameraComponent.h"
#include "Game/Framework/WaterCameraFx.h"
#include "Scene.h"
#include "CameraZoneScript.h"

#if RC_ENABLE_IMGUI
#include "imgui/imgui.h"
#endif

#include <algorithm>
#include <cmath>

/// @class CaveHeadlampScript
/// @brief 洞窟の中で、メインカメラに追従するヘッドライト（影ありのスポットライト）を点ける
/// @details
///   洞窟には人工的な光源を置かず、入口・出口から差す外光と、このヘッドライトだけで照らす。
///   スポットライトは castShadow にしておけば岩に遮られるので、柱や鍾乳石の影が光の動きに合わせて伸びる。
///
///   - 位置: 毎フレーム、メインカメラの位置・回転へ合わせる（SpotLightComponent::offset で頭の少し右上へずらす）
///   - 向き: カメラの前方へ指数補間で追従させる（手で持ったライトのように、視線よりわずかに遅れる）
///   - 明るさ: カメラの z が fromZ → toZ と進むあいだに 0 → intensity へなめらかに上げる
///             （fromZ > toZ なら逆向き。洞窟から出ていく面で消すときに使う）
///
///   置き場所: SpotLightComponent を持つエンティティ（ルート直下。親を持たせない）。
///   再生中だけ書き換える。編集中に書き換えると、その値がシーンの保存で JSON に焼き付くため。
///   「再生中だけ・カメラの z で 0〜1」の流れは基底の CameraZoneScript（Template Method）が持ち、
///   このクラスはライトへの書き込み（OnZoneBlend）だけを担当する。
///
///   JSON (scriptDataList):
///     "fromZ" / "toZ" : 点灯を始める／点ききる z
///     "intensity"     : 点ききったときのスポットライトの強さ
///     "flicker"       : 明るさのゆらぎ（0 で無し。0.03 なら ±3% 程度）
///     "followRate"    : 向きがカメラへ追いつく速さ（1/秒。大きいほど遅れが小さい）
class CaveHeadlampScript : public CameraZoneScript {
public:
  /// @brief 既定の補間範囲（z = 0 → 1）で作る
  CaveHeadlampScript() : CameraZoneScript(0.0f, 1.0f) {}

  /// @brief 設定値を JSON に書き出す
  /// @return 設定値を持つ JSON オブジェクト
  nlohmann::json Serialize() override {
    nlohmann::json j = {{"intensity", intensity_}, {"flicker", flicker_}, {"followRate", followRate_}};
    WriteZone(j);
    return j;
  }
  /// @brief JSON から設定値を読み込む
  /// @param j 読み込む JSON（無いキーは現在の値のまま）
  void Deserialize(const nlohmann::json &j) override {
    ReadZone(j);
    if (j.contains("intensity")) intensity_ = j["intensity"].get<float>();
    if (j.contains("flicker")) flicker_ = j["flicker"].get<float>();
    if (j.contains("followRate")) followRate_ = j["followRate"].get<float>();
  }

  /// @brief Inspector に現在の点灯率と設定値の編集 UI を表示する
  void OnImGui() override {
#if RC_ENABLE_IMGUI
    DrawZoneImGui();
    ImGui::DragFloat("Intensity", &intensity_, 0.05f, 0.0f, 20.0f);
    ImGui::DragFloat("Flicker", &flicker_, 0.005f, 0.0f, 0.5f);
    ImGui::DragFloat("Follow Rate", &followRate_, 0.1f, 0.5f, 60.0f);
#endif
  }

protected:
  /// @brief 生成直後の時点のカメラ位置でライトを合わせる（向きは補間せず一致させる）
  void OnCreate() override {
    snapDirection_ = true;
    ApplyZone(0.0f);
  }
  /// @brief 毎フレーム、カメラに合わせてライトの位置・向き・明るさを更新する
  /// @param dt 経過時間（秒）
  void OnUpdate(float dt) override { ApplyZone(dt); }

  /// @brief スポットライトと Transform がそろっているときだけ書き換える
  /// @return 両方あれば true
  bool CanApplyZone() override {
    return GetComponent<SpotLightComponent>() != nullptr && GetComponent<TransformComponent>() != nullptr;
  }

  /// @brief メインカメラへ追従させ、z に応じた明るさを SpotLightComponent へ書き込む
  /// @param camTr メインカメラの Transform
  /// @param t 点灯率（0〜1）
  /// @param dt 経過時間（秒。0 なら向きはカメラの前方へそのまま合わせる）
  void OnZoneBlend(const TransformComponent &camTr, float t, float dt) override {
    auto *spot = GetComponent<SpotLightComponent>();
    auto *tr = GetComponent<TransformComponent>();
    if (!spot || !tr) return;

    // 位置と回転はカメラに合わせる（offset はこの回転で回されて頭の右上へ付く）
    tr->position = camTr.position;
    tr->rotation = camTr.rotation;

    // カメラの前方（rotation.x = ピッチ、rotation.y = ヨー）
    const float rx = camTr.rotation.x;
    const float ry = camTr.rotation.y;
    const RC::Vector3 fwd = {std::sin(ry) * std::cos(rx), -std::sin(rx), std::cos(ry) * std::cos(rx)};
    if (snapDirection_ || dt <= 0.0f) {
      dir_ = fwd;
      snapDirection_ = false;
    } else {
      // フレームレートに依らない指数補間
      const float k = 1.0f - std::exp(-(std::max)(followRate_, 0.0f) * dt);
      dir_ = {dir_.x + (fwd.x - dir_.x) * k, dir_.y + (fwd.y - dir_.y) * k, dir_.z + (fwd.z - dir_.z) * k};
      const float len = std::sqrt(dir_.x * dir_.x + dir_.y * dir_.y + dir_.z * dir_.z);
      dir_ = (len > 1e-4f) ? RC::Vector3{dir_.x / len, dir_.y / len, dir_.z / len} : fwd;
    }
    spot->direction = dir_;

    // 電池式のライトらしい、ごくわずかなゆらぎ（周期の違う sin を重ねる）
    time_ += dt;
    const float wobble = 1.0f + flicker_ * (0.6f * std::sin(time_ * 7.3f) + 0.4f * std::sin(time_ * 17.9f + 1.3f));
    spot->intensity = intensity_ * t * wobble;
    // 消えている間は影のパスを使わないように無効にする
    spot->visible = t > 0.001f;
  }

private:
  float intensity_ = 2.0f;   ///< 点ききったときの強さ
  float flicker_ = 0.03f;    ///< 明るさのゆらぎの大きさ（割合）
  float followRate_ = 12.0f; ///< 向きがカメラの前方へ追いつく速さ（1/秒）

  float time_ = 0.0f;                    ///< ゆらぎ用の経過時間（秒）
  RC::Vector3 dir_ = {0.0f, 0.0f, 1.0f}; ///< 現在のライトの向き（カメラの前方へ遅れて追従）
  bool snapDirection_ = true;            ///< 次の更新で向きを補間せずにカメラへ合わせるか
};

REGISTER_SCRIPT(CaveHeadlampScript)
