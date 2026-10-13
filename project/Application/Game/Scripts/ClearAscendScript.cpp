#include "ECS/ScriptableEntity.h"
#include "ECS/ScriptRegistry.h"
#include "Common/Log/Log.h"
#include "RenderCommon.h"
#include "Engine/Render/RenderContext.h"
#include "Framework/App.h"
#include "Scene.h"
#include "Game/Framework/GameSession.h"
#include "Game/Framework/WaterCameraFx.h"

#if RC_ENABLE_IMGUI
#include "imgui/imgui.h"
#endif

#include <algorithm>
#include <string>

/// @class ClearAscendScript
/// @brief ステージクリアのとき、その場から空へ跳ね上がって白く飛び、Result へ "ascend" 遷移する
/// @details
///   DeathSinkScript（力尽きて沈み、暗くなって Result へ）と対になる演出。
///   置き場所: ステージの Camera エンティティ（RailShooterController / DeathSinkScript と同じ）。
///
///   流れ:
///     1. GameSession がクリアになったら（RailShooterGameMode がレール終点で決着させる）、
///        遷移表（Resources/SceneFlow.json）のきっかけ "cleared" を引く。
///        演出が "ascend" のときだけ自分が引き受け、"result_transition_owner" タグを立てて
///        DataDrivenScene の自動遷移（2.5 秒後）を止める。それ以外の演出なら何もしない。
///     2. delay 秒だけその場で止まる（STAGE CLEAR の表示を見せる）。
///     3. ため（少し沈み込む）→ 空へ跳ね上がる（見上げながら、画角が広がる）。
///        水中で終わったステージは水面を抜けて上がる（出水の演出は RailShooterController が出す）。
///     4. 跳ね上がりの後半で露出を上げ、白い幕を重ねて画面を白く飛ばす。
///     5. 白くなりきったら "ascend" 遷移を要求する。SceneManager は同じ白へ抜け、
///        Result（ResultScreenScript）は白から開けて空から降りてくる。
///
///   JSON (scriptDataList):
///     "enabled" / "delay" / "anticipation" / "dip" / "launch" / "height" /
///     "lookUpPitch" / "fovBoost" / "whiteStart" / "whiteEv"
class ClearAscendScript : public ScriptableEntity {
public:
  bool enabled = true;
  float delay = 1.6f; ///< クリアしてから跳ね上がるまで（秒。STAGE CLEAR を見せる）
  WaterCameraFx::AscendParams ascend;

  nlohmann::json Serialize() override {
    return {{"enabled", enabled},
            {"delay", delay},
            {"anticipation", ascend.anticipation},
            {"dip", ascend.dip},
            {"launch", ascend.launch},
            {"height", ascend.height},
            {"lookUpPitch", ascend.lookUpPitch},
            {"fovBoost", ascend.fovBoost},
            {"whiteStart", ascend.whiteStart},
            {"whiteEv", ascend.whiteEv}};
  }

  void Deserialize(const nlohmann::json &j) override {
    auto f = [&](const char *k, float &o) { if (j.contains(k) && j[k].is_number()) o = j[k].get<float>(); };
    if (j.contains("enabled") && j["enabled"].is_boolean()) enabled = j["enabled"].get<bool>();
    f("delay", delay);
    f("anticipation", ascend.anticipation);
    f("dip", ascend.dip);
    f("launch", ascend.launch);
    f("height", ascend.height);
    f("lookUpPitch", ascend.lookUpPitch);
    f("fovBoost", ascend.fovBoost);
    f("whiteStart", ascend.whiteStart);
    f("whiteEv", ascend.whiteEv);
  }

#if RC_ENABLE_IMGUI
  void OnImGui() override {
    static const char *kNames[] = {"idle", "wait", "ascend", "done", "off"};
    ImGui::Text("State: %s  t=%.2f  white=%.2f", kNames[static_cast<int>(phase_)], t_,
                ascend_.WhiteAlpha());
    ImGui::Checkbox("Enabled", &enabled);
    ImGui::DragFloat("Delay (s)", &delay, 0.05f, 0.0f, 5.0f);
    ImGui::DragFloat("Anticipation (s)", &ascend.anticipation, 0.01f, 0.0f, 2.0f);
    ImGui::DragFloat("Dip (m)", &ascend.dip, 0.05f, 0.0f, 5.0f);
    ImGui::DragFloat("Launch (s)", &ascend.launch, 0.05f, 0.2f, 5.0f);
    ImGui::DragFloat("Height (m)", &ascend.height, 0.5f, 1.0f, 200.0f);
    ImGui::DragFloat("Look Up Pitch", &ascend.lookUpPitch, 0.01f, -1.55f, 0.5f);
    ImGui::DragFloat("FOV Boost", &ascend.fovBoost, 0.01f, 1.0f, 2.0f);
    ImGui::SliderFloat("White Start", &ascend.whiteStart, 0.0f, 0.95f);
    ImGui::DragFloat("White EV", &ascend.whiteEv, 0.05f, 0.0f, 6.0f);
  }
#endif

protected:
  void OnCreate() override {
    phase_ = Phase::Idle;
    t_ = 0.0f;
    requested_ = false;
    // 前回のプレイで保存された分を消す（DeathSinkScript と同じタグ）
    if (Entity *self = GetEntity()) self->ClearTag(kOwnerTag);
  }

  void OnUpdate(float dt) override {
    if (dt <= 0.0f || !enabled) return;
    Entity *self = GetEntity();
    if (!self) return;

    if (phase_ == Phase::Idle) {
      SceneContext *ctx = GetSceneContext();
      if (!ctx || !ctx->isPlaying()) return;
      if (!GameSession::Get().IsCleared()) return;
      // 力尽きたほうが先なら DeathSinkScript の担当
      if (self->GetTagInt("game_over", 0) == 1 || self->GetTagInt(kOwnerTag, 0) == 1) {
        phase_ = Phase::Off;
        return;
      }
      std::string transition;
      if (!LookupTransition(kTriggerCleared, target_, transition) || target_.empty() ||
          transition != SceneTransitions::kAscend) {
        // 遷移表の演出が ascend 以外なら DataDrivenScene の自動遷移に任せる
        phase_ = Phase::Off;
        return;
      }
      self->SetTag(kOwnerTag, 1); // DataDrivenScene の自動遷移を止める
      phase_ = Phase::Wait;
      t_ = 0.0f;
      Log::Print("[ClearAscendScript] begin -> " + target_);
    }

    t_ += dt;
    switch (phase_) {
    case Phase::Wait:
      if (t_ >= delay) {
        Scene *scene = GetScene();
        if (!ascend_.Begin(WaterCameraFx::FindMainCamera(scene), WaterCameraFx::FindWaterY(scene),
                           ascend)) {
          // カメラが見つからない。演出なしで送る
          Log::Print("[ClearAscendScript] camera not found, changing scene directly");
          phase_ = Phase::Done;
          t_ = 0.0f;
          break;
        }
        phase_ = Phase::Ascend;
        t_ = 0.0f;
      }
      break;
    case Phase::Ascend:
      if (!ascend_.Update(dt)) {
        ascend_.End(false);
        phase_ = Phase::Done;
        t_ = 0.0f;
        break;
      }
      if (ascend_.IsDone()) {
        phase_ = Phase::Done;
        t_ = 0.0f;
      }
      break;
    case Phase::Done:
      if (ascend_.IsActive()) ascend_.Update(dt); // 遷移が始まるまで上がり続ける
      if (!requested_) {
        requested_ = RequestSceneChange(target_, SceneTransitions::kAscend);
        if (!requested_ && t_ > 1.5f) {
          // 何らかの理由で通らない（遷移中など）。素の遷移で送る
          Log::Print("[ClearAscendScript] transition refused, falling back");
          requested_ = RequestSceneChange(target_);
          if (!requested_ && t_ > 3.0f) {
            ascend_.End(true);
            self->ClearTag(kOwnerTag);
            requested_ = true; // 諦める
          }
        }
      }
      break;
    case Phase::Idle:
    case Phase::Off:
      break;
    }
  }

  /// @brief 白い幕（ポストプロセスの後に描くので、水中の色味や輪郭線に染まらない）
  void OnOverlayRender() override {
    const float a = ascend_.WhiteAlpha();
    if (a <= 0.0f) return;
    float w = 1280.0f, h = 720.0f;
    auto &rc = RC::GetRenderContext();
    if (rc.Ctx() && rc.Ctx()->app) {
      w = static_cast<float>(rc.Ctx()->app->width);
      h = static_cast<float>(rc.Ctx()->app->height);
    }
    // SceneManager が Ascend 遷移で抜ける白（kAscendScreenColor）と揃える
    RC::DrawBox({0.0f, 0.0f}, {w, h}, {0.97f, 0.98f, 1.0f, a});
  }

  void OnDestroy() override {
    if (ascend_.IsActive()) ascend_.End(false);
    if (Entity *self = GetEntity()) self->ClearTag(kOwnerTag);
  }

private:
  enum class Phase { Idle = 0, Wait, Ascend, Done, Off };
  static constexpr const char *kOwnerTag = "result_transition_owner";
  static constexpr const char *kTriggerCleared = "cleared"; ///< 遷移表のきっかけ名

  Phase phase_ = Phase::Idle;
  float t_ = 0.0f;
  bool requested_ = false;
  std::string target_;
  WaterCameraFx::AscendSequence ascend_;
};

REGISTER_SCRIPT(ClearAscendScript)
