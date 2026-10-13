#include "ECS/ScriptableEntity.h"
#include "ECS/ScriptRegistry.h"
#include "ECS/TransformComponent.h"
#include "Common/Log/Log.h"
#include "Scene.h"
#include "Game/Framework/WaterCameraFx.h"
#include "Game/Framework/TransitionDemo.h"

#if RC_ENABLE_IMGUI
#include "imgui/imgui.h"
#endif

#include <algorithm>
#include <cmath>
#include <string>

/// @class DeathSinkScript
/// @brief 力尽きたとき、その視点のまま水へ沈み、水面を見上げながら暗くなって Result へ遷移する
/// @details
///   置き場所: Game の Camera エンティティ（RailShooterController と同じエンティティ）。
///
///   流れ:
///     1. 自エンティティに game_over タグが立ったら（RailShooterController::TakeDamage）開始。
///        "result_transition_owner" タグを立てて、DataDrivenScene の自動遷移（2.5 秒後の
///        Dissolve）を止める。
///     2. delay 秒だけその場で止まる（GAME OVER の表示を見せる）。
///     3. 水面より上にいれば、力が抜けたように水面へ落ちる（fallTime 秒）。
///        着水の演出（ラジアルブラー・水滴・水中エフェクト）は RailShooterController の
///        水中判定がそのまま出す。
///     4. 沈む（sinkTime 秒で depth m）。並行して水面を見上げる向きへ傾き（lookUpPitch）、
///        少しだけ横に傾く（roll）。深さに応じたフォグは RailShooterController が掛けるので、
///        深くなるほど暗くなる。
///     5. 深さが darkDepth（UnderwaterLook の deepDepth と揃える）に達したら
///        遷移表（Resources/SceneFlow.json）のきっかけ "gameover" で遷移する（通常は Result へ "dive"）。
///        Result 側は深海から浮上して始まる。
///
///   JSON (scriptDataList):
///     "enabled" / "delay" / "fallTime" / "sinkTime" / "depth" / "lookUpPitch" / "tiltTime" /
///     "roll" / "darkDepth" / "bubbles"
class DeathSinkScript : public ScriptableEntity {
public:
  bool enabled = true;
  float delay = 0.6f;         ///< 力尽きてから沈み始めるまで（秒）
  float fallTime = 0.8f;      ///< 水面より上にいたとき、水面まで落ちる時間（秒）
  float sinkTime = 3.2f;      ///< 着水から depth まで沈む時間（秒）
  float depth = 30.0f;        ///< 沈む深さ（m）
  float lookUpPitch = -1.05f; ///< 沈みながら向く角度（rad。負で見上げ。-π/2 で真上）
  float tiltTime = 2.2f;      ///< 見上げる向きへ傾くのにかける時間（秒）
  float roll = 0.18f;         ///< 沈みながら横に傾く量（rad）
  float darkDepth = 24.0f;    ///< この深さで遷移する（UnderwaterLook の deepDepth と揃える）
  bool bubbles = true;

  /// @brief 設定項目を JSON に書き出す
  /// @return 設定項目を格納した JSON
  nlohmann::json Serialize() override {
    return {{"enabled", enabled},   {"delay", delay},       {"fallTime", fallTime},
            {"sinkTime", sinkTime}, {"depth", depth},       {"lookUpPitch", lookUpPitch},
            {"tiltTime", tiltTime}, {"roll", roll},         {"darkDepth", darkDepth},
            {"bubbles", bubbles}};
  }

  /// @brief JSON から設定項目を読み込む
  /// @param j 読み込み元の JSON（存在し型が合うキーだけ反映する）
  void Deserialize(const nlohmann::json &j) override {
    // キーが存在し型が一致する場合だけ上書きするヘルパー（数値用 / 真偽値用）
    auto f = [&](const char *k, float &o) { if (j.contains(k) && j[k].is_number()) o = j[k].get<float>(); };
    auto b = [&](const char *k, bool &o) { if (j.contains(k) && j[k].is_boolean()) o = j[k].get<bool>(); };
    b("enabled", enabled);
    f("delay", delay);
    f("fallTime", fallTime);
    f("sinkTime", sinkTime);
    f("depth", depth);
    f("lookUpPitch", lookUpPitch);
    f("tiltTime", tiltTime);
    f("roll", roll);
    f("darkDepth", darkDepth);
    b("bubbles", bubbles);
  }

#if RC_ENABLE_IMGUI
  /// @brief インスペクタに現在の状態と各パラメータの編集 UI を表示する
  void OnImGui() override {
    static const char *kNames[] = {"idle", "wait", "fall", "sink", "done"};
    ImGui::Text("State: %s  t=%.2f", kNames[static_cast<int>(phase_)], t_);
    ImGui::Checkbox("Enabled", &enabled);
    ImGui::DragFloat("Delay (s)", &delay, 0.05f, 0.0f, 5.0f);
    ImGui::DragFloat("Fall (s)", &fallTime, 0.05f, 0.1f, 5.0f);
    ImGui::DragFloat("Sink (s)", &sinkTime, 0.05f, 0.2f, 10.0f);
    ImGui::DragFloat("Depth (m)", &depth, 0.5f, 1.0f, 200.0f);
    ImGui::DragFloat("Look Up Pitch", &lookUpPitch, 0.01f, -1.55f, 0.5f);
    ImGui::DragFloat("Tilt (s)", &tiltTime, 0.05f, 0.0f, 10.0f);
    ImGui::DragFloat("Roll", &roll, 0.01f, -1.0f, 1.0f);
    ImGui::DragFloat("Dark Depth (m)", &darkDepth, 0.5f, 1.0f, 200.0f);
    ImGui::Checkbox("Bubbles", &bubbles);
  }
#endif

protected:
  /// @brief 状態を初期化し、前回のタグを消して沈む間に出す泡を生成しておく
  void OnCreate() override {
    phase_ = Phase::Idle;
    demo_ = false;
    demoTimer_ = 0.0f;
    if (Entity *self = GetEntity()) self->ClearTag(kOwnerTag); // 前回のプレイで保存された分を消す
    if (enabled && bubbles) bubbles_.Spawn(GetScene(), "DeathBubbles");
  }

  /// @brief 力尽きたこと（またはデモ開始）を検知し、待機→落下→沈降→遷移要求の順にカメラを動かす
  /// @param dt 前フレームからの経過時間（秒）
  void OnUpdate(float dt) override {
    if (dt <= 0.0f || !enabled) return;
    Entity *self = GetEntity();
    auto *tr = GetComponent<TransformComponent>();
    if (!self || !tr) return;

    // 待機中: game_over タグかデモ開始を検知したら遷移を引き受けて演出を始める
    if (phase_ == Phase::Idle) {
      SceneContext *ctx = GetSceneContext();
      // 遷移の見せ用デモ：開始演出が終わって一定時間たったら、力尽きていなくても沈む
      bool demoStart = false;
      if (TransitionDemo::kEnabled && ctx && ctx->isPlaying() &&
          self->GetTagInt(kOwnerTag, 0) != 1) { // クリアの演出（ClearAscendScript）が先なら譲る
        if (self->GetTagInt("intro_playing", 0) == 1) {
          demoTimer_ = 0.0f;
        } else {
          demoTimer_ += dt;
          demoStart = demoTimer_ >= TransitionDemo::kDelayAfterStart;
        }
      }
      if (!demoStart && self->GetTagInt("game_over", 0) != 1) return;
      if (!ctx || !ctx->isPlaying()) return;
      demo_ = demoStart && self->GetTagInt("game_over", 0) != 1;
      self->SetTag(kOwnerTag, 1); // DataDrivenScene の自動遷移を止める（RailMovement / 視点操作も止まる）
      waterY_ = WaterCameraFx::FindWaterY(GetScene());
      phase_ = Phase::Wait;
      t_ = 0.0f;
      Log::Print(demo_ ? "[DeathSinkScript] begin (transition demo)" : "[DeathSinkScript] begin");
    }

    // 段階ごとの進行（各段階の経過時間は t_ で測り、段階が変わるたびに 0 へ戻す）
    t_ += dt;
    switch (phase_) {
    case Phase::Wait:
      // 待ち終わったら開始姿勢を記録し、水面より上なら落下、水中ならすぐ沈降へ
      if (t_ >= delay) {
        startPos_ = tr->position;
        startRot_ = tr->rotation;
        phase_ = (tr->position.y > waterY_) ? Phase::Fall : Phase::Sink;
        sinkStartY_ = (std::min)(tr->position.y, waterY_);
        sinkT_ = 0.0f;
        t_ = 0.0f;
        tiltT_ = 0.0f;
      }
      break;
    case Phase::Fall: {
      // 力が抜けたように、加速しながら水面へ
      const float u = t_ / (std::max)(fallTime, 1e-3f);
      tr->position.y = startPos_.y + (waterY_ - startPos_.y) * WaterCameraFx::EaseInQuad(u);
      UpdateTilt(*tr, dt);
      if (u >= 1.0f) {
        tr->position.y = waterY_ - 0.05f;
        sinkStartY_ = tr->position.y;
        phase_ = Phase::Sink;
        t_ = 0.0f;
      }
      break;
    }
    case Phase::Sink: {
      // 着水の勢いのまま沈み、水の抵抗でゆっくりになっていく
      // 沈み始めた最初のフレームだけ泡を放出する
      if (t_ <= dt * 1.5f && bubbles) bubbles_.SetEmission(WaterCameraFx::Bubbles::kEmitPerFrame);
      const float u = t_ / (std::max)(sinkTime, 1e-3f);
      const float targetY = waterY_ - depth;
      tr->position.y = sinkStartY_ + (targetY - sinkStartY_) * WaterCameraFx::EaseOutQuad(u);
      UpdateTilt(*tr, dt);
      if ((waterY_ - tr->position.y) >= darkDepth || u >= 1.0f) {
        phase_ = Phase::Done;
        t_ = 0.0f;
      }
      break;
    }
    case Phase::Done: {
      tr->position.y -= 2.0f * dt; // 遷移が始まるまで止まって見えないように
      UpdateTilt(*tr, dt);
      if (!requested_) {
        // 行き先と演出は遷移表のきっかけ "gameover"（GameMode の決着と同じ行）で引く
        if (resultTarget_.empty()) {
          if (demo_) {
            // デモは Result ではなく遷移表の "demo_end"（Title へ dive）。行が無ければ Title へ dive
            if (!LookupTransition(TransitionDemo::kTrigger, resultTarget_, resultTransition_) ||
                resultTarget_.empty()) {
              resultTarget_ = "Title";
              resultTransition_ = SceneTransitions::kDive;
            }
          } else {
            LookupTransition(kTriggerGameOver, resultTarget_, resultTransition_);
          }
        }
        requested_ = !resultTarget_.empty() && RequestSceneChange(resultTarget_, resultTransition_);
        if (!requested_ && t_ > 1.5f) {
          // 何らかの理由で通らない（遷移中など）。素の遷移で送る
          Log::Print("[DeathSinkScript] transition refused, falling back");
          requested_ = !resultTarget_.empty() && RequestSceneChange(resultTarget_);
          if (!requested_ && t_ > 3.0f) {
            self->ClearTag(kOwnerTag);
            requested_ = true; // 諦める（DataDrivenScene 側の要求はもう出ないので、ここで止める）
          }
        }
      }
      break;
    }
    case Phase::Idle:
      break;
    }

    // 泡をカメラに追従させる（Wait 中はまだ動かないので不要）
    if (phase_ != Phase::Wait) bubbles_.Follow(*tr, waterY_);
  }

  /// @brief 遷移担当タグを外し、泡を破棄する
  void OnDestroy() override {
    if (Entity *self = GetEntity()) self->ClearTag(kOwnerTag);
    bubbles_.Destroy();
  }

private:
  /// @brief 演出の進行段階
  enum class Phase { Idle = 0, Wait, Fall, Sink, Done };
  static constexpr const char *kOwnerTag = "result_transition_owner";

  /// @brief 水面を見上げる向きへ、少し横に傾きながらゆっくり回す
  /// @param tr 回転させるカメラの Transform
  /// @param dt 前フレームからの経過時間（秒）
  void UpdateTilt(TransformComponent &tr, float dt) {
    tiltT_ += dt;
    const float r = (tiltTime > 0.0f) ? WaterCameraFx::Smooth01(tiltT_ / tiltTime) : 1.0f;
    tr.rotation.x = startRot_.x + (lookUpPitch - startRot_.x) * r;
    tr.rotation.z = startRot_.z + roll * r;
  }

  Phase phase_ = Phase::Idle;
  float t_ = 0.0f;
  float tiltT_ = 0.0f;
  float sinkT_ = 0.0f;
  float waterY_ = 0.0f;
  float sinkStartY_ = 0.0f;
  RC::Vector3 startPos_{};
  RC::Vector3 startRot_{};
  bool requested_ = false;
  bool demo_ = false;        ///< 遷移の見せ用デモで沈んでいる（TransitionDemo）
  float demoTimer_ = 0.0f;   ///< 開始演出が終わってからの経過（デモ用）
  std::string resultTarget_;    ///< 遷移表で引いた行き先（Done に入ってから 1 回だけ引く）
  std::string resultTransition_; ///< 遷移表で引いた演出
  static constexpr const char *kTriggerGameOver = "gameover"; ///< 遷移表のきっかけ名
  WaterCameraFx::Bubbles bubbles_;
};

REGISTER_SCRIPT(DeathSinkScript)
