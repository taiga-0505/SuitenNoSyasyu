#include "ECS/ScriptableEntity.h"
#include "ECS/ScriptRegistry.h"
#include "ECS/TransformComponent.h"
#include "Common/Log/Log.h"
#include "Scene.h"
#include "Game/Framework/WaterCameraFx.h"
#include "Game/Framework/TransitionDemo.h"
#include "Game/Framework/StageDemo.h"
#include "Game/Framework/DeathScreenFx.h"
#include "Game/Framework/StateMachine.h"
#include "Common/Tween/Easing.h"

#if RC_ENABLE_IMGUI
#include "imgui/imgui.h"
#endif

#include <algorithm>
#include <cmath>
#include <memory>
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
///   実装（State パターン）:
///     上の段階（待機 / 止まる / 落ちる / 沈む / 遷移待ち）を、以前は enum Phase + switch で切り替えていた。
///     段階ごとの処理と「次はどの段階か」が 1 つの関数に混ざっていたので、
///     段階 1 つ = 状態クラス 1 つ（IdleState / WaitState / FallState / SinkState / DoneState）に分け、
///     次の段階は状態クラス自身が StateMachine::ChangeState で決めるようにした。
///     状態クラスは private な入れ子クラスなので、friend 無しで本体の private メンバを使える。
///
///   画面演出（DeathScreenFx）:
///     力尽きた瞬間からレンズに血が付き（水に入ると洗い流される）、色が抜けていく。
///     視界の周辺から水彩のような滲み（なめらかなぼかし・色のにじみ・減光）が入り込み、沈むほど中央へ進む。
///     沈み始めると 2 回重い瞬きをして、darkDepth に着く（画面が暗くなりきる）頃に瞼が閉じきる。
///     瞼が開き直るたびに一瞬二重に見え、ゆっくり 1 枚に戻る。
///     沈むにつれて水面から差す光は細く弱くなり、上へ遠ざかっていく。
///     遷移は瞼が閉じきるまで待つ。
///     瞼は GAME OVER の文字より手前に描くので、スクリプト順は RailShooterController の後に置くこと。
///
///   最後の息（breath*）:
///     沈み始めてから瞼が閉じきるまでの breathAt の位置（既定は 1 回目の瞬きで瞼が開き直すところ。
///     瞼が閉じている間に吐くと見えない）で、口元から大きめの泡を吐き、しばらくゴボゴボと漏らす。
///     沈む速さが泡の上昇よりずっと速いので、泡はカメラの動きを引き継ぎ、カメラから見て
///     ゆっくり昇っていくようにしてある（WaterCameraFx::BreathBurst）。
///
///   見せ用デモ（TransitionDemo::kEnabled。ステージ側のデモはこのスクリプトが受け持つ）:
///     デモの種類ごとの振る舞いは TransitionDemo::StageDemo の派生クラス（StageDemo.h）にあり、
///     このスクリプトは CreateStageDemo()（Factory Method）が作ったものを StageDemo の参照経由で呼ぶだけ。
///     Mode::GameOver: 最初のウェーブが始まって少しすると、サメの群れ（SharkFrenzyDemo）が一斉に襲いかかり、
///       1 匹目が噛みついた時点で致死ダメージ（pending_damage）を入れる。以降は通常の力尽きた流れと同じ
///       （game_over → 沈む → "gameover" で Result へ）。サメはカメラに食らいついたまま一緒に沈む。
///     Mode::Clear: 序盤を少し遊んで見せ、暗転で終盤へ飛び、正面に出るサメを撃ち倒してクリアする
///       （ClearRunDemo。暗転の幕はこのスクリプトの OnOverlayRender で描く）。
///
///   JSON (scriptDataList):
///     "enabled" / "delay" / "fallTime" / "sinkTime" / "depth" / "lookUpPitch" / "tiltTime" /
///     "roll" / "darkDepth" / "bubbles" / "breath" / "breathAt" / "breathCount" / "breathTime" /
///     "breathTrail" / "screenFx"（DeathScreenFx の調整値）
class DeathSinkScript : public ScriptableEntity {
public:
  /// @brief 待機状態（力尽きるのを待つ）で作る
  DeathSinkScript() {
    fsm_.ChangeState<IdleState>();
    fsm_.ApplyPending(*this);
  }

  /// @brief 設定項目を JSON に書き出す
  /// @return 設定項目を格納した JSON
  nlohmann::json Serialize() override {
    return {{"enabled", enabled},     {"delay", delay},           {"fallTime", fallTime},
            {"sinkTime", sinkTime},   {"depth", depth},           {"lookUpPitch", lookUpPitch},
            {"tiltTime", tiltTime},   {"roll", roll},             {"darkDepth", darkDepth},
            {"bubbles", bubbles},     {"breath", breath},         {"breathAt", breathAt},
            {"breathCount", breathCount}, {"breathTime", breathTime}, {"breathTrail", breathTrail},
            {"screenFx", screenFx.ToJson()}};
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
    b("breath", breath);
    f("breathAt", breathAt);
    if (j.contains("breathCount") && j["breathCount"].is_number_integer()) breathCount = j["breathCount"].get<int>();
    f("breathTime", breathTime);
    f("breathTrail", breathTrail);
    if (j.contains("screenFx") && j["screenFx"].is_object()) screenFx.FromJson(j["screenFx"]);
  }

#if RC_ENABLE_IMGUI
  /// @brief インスペクタに現在の状態と各パラメータの編集 UI を表示する
  void OnImGui() override {
    ImGui::Text("State: %s  t=%.2f", fsm_.CurrentName(), t_);
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
    ImGui::SeparatorText("Last Breath (最後の息)");
    ImGui::Checkbox("Breath", &breath);
    ImGui::SliderFloat("Breath At (0-1)", &breathAt, 0.0f, 1.0f);
    ImGui::SliderInt("Breath Count / frame", &breathCount, 1, 40);
    ImGui::DragFloat("Breath Time (s)", &breathTime, 0.01f, 0.02f, 1.0f);
    ImGui::DragFloat("Breath Trail (s)", &breathTrail, 0.05f, 0.0f, 3.0f);
    ImGui::SeparatorText("Screen FX (血・色抜け・瞬き・滲み・水面の光)");
    screenFx.DrawImGui();
  }
#endif

protected:
  /// @brief 状態を初期化し、前回のタグを消して沈む間に出す泡を生成しておく
  void OnCreate() override {
    fsm_.ChangeState<IdleState>();
    fsm_.ApplyPending(*this);
    requestT_ = 0.0f;
    breathed_ = false;
    screenFx.Clear(nullptr); // 状態だけ初期化（PostProcess はシーン切り替えで片付いている）
    if (Entity *self = GetEntity()) self->ClearTag(kOwnerTag); // 前回のプレイで保存された分を消す
    stageDemo_->Reset(GetEntity());                            // 見せ用デモの状態と、前回残ったデモのタグ
    if (enabled && bubbles) bubbles_.Spawn(GetScene(), "DeathBubbles");
    if (enabled && breath) breath_.Spawn(GetScene(), "DeathBreath");
  }

  /// @brief 沈む演出を進め、見せ用デモも進める
  /// @param dt 前フレームからの経過時間（秒）
  void OnUpdate(float dt) override {
    UpdateSink(dt);
    // 見せ用デモ（カメラを動かしたあとに追従させる）。どのデモかは stageDemo_ の派生クラスが知っている
    if (dt <= 0.0f) return;
    Entity *self = GetEntity();
    auto *tr = GetComponent<TransformComponent>();
    if (!self || !tr) return;
    TransitionDemo::StageDemoFrame frame;
    frame.scene = GetScene();
    frame.ctx = GetSceneContext();
    frame.cam = self;
    frame.camTr = tr;
    frame.scriptEnabled = enabled;
    frame.outroStarted = !fsm_.IsIn<IdleState>();
    frame.dt = dt;
    stageDemo_->Update(frame);
  }

  /// @brief 瞼を描く（GAME OVER の文字より手前。スクリプト順で RailShooterController の後に呼ばれる）
  void OnOverlayRender() override {
    screenFx.DrawLids();
    // 見せ用デモ（クリア）の暗転。Dive 遷移と同じ深海色で、HUD ごと覆う
    const float fade = stageDemo_->FadeAlpha();
    if (fade > 0.0f) {
      float w = 1280.0f, h = 720.0f;
      auto &rc = RC::GetRenderContext();
      if (rc.Ctx() && rc.Ctx()->app) {
        w = static_cast<float>(rc.Ctx()->app->width);
        h = static_cast<float>(rc.Ctx()->app->height);
      }
      const RC::Vector4 &c = Scene::SceneManager::DiveScreenColor();
      RC::DrawBox({0.0f, 0.0f}, {w, h}, {c.x, c.y, c.z, fade});
    }
  }

  /// @brief 遷移担当タグを外し、泡を破棄し、画面演出を元に戻す
  void OnDestroy() override {
    if (Entity *self = GetEntity()) self->ClearTag(kOwnerTag);
    stageDemo_->End(GetEntity());
    bubbles_.Destroy();
    breath_.Destroy();
    screenFx.Clear(RC::GetRenderContext().GetPostProcess());
  }

private:
  // ---- 状態クラス（private な入れ子クラス。次の段階は各状態が ChangeState で決める）----

  /// @brief 待機：game_over タグを検知したら遷移を引き受けて Wait へ
  class IdleState : public IState<DeathSinkScript> {
  public:
    /// @brief 力尽きたかを調べ、力尽きていれば演出を始めて Wait へ
    void OnUpdate(DeathSinkScript &o, StateMachine<DeathSinkScript> &m, float) override {
      Entity *self = o.GetEntity();
      SceneContext *ctx = o.GetSceneContext();
      if (self->GetTagInt("game_over", 0) != 1) return;
      if (!ctx || !ctx->isPlaying()) return;
      o.BeginSink(*self);
      m.ChangeState<WaitState>();
    }
    /// @brief 状態名
    const char *Name() const override { return "idle"; }
  };

  /// @brief その場で止まる（GAME OVER の表示を見せる）
  class WaitState : public IState<DeathSinkScript> {
  public:
    /// @brief 待ち終わったら開始姿勢を記録し、水面より上なら Fall、水中ならすぐ Sink へ
    void OnUpdate(DeathSinkScript &o, StateMachine<DeathSinkScript> &m, float) override {
      auto *tr = o.GetComponent<TransformComponent>();
      if (!tr || o.t_ < o.delay) return;
      o.startPos_ = tr->position;
      o.startRot_ = tr->rotation;
      const bool aboveWater = tr->position.y > o.waterY_;
      o.sinkStartY_ = (std::min)(tr->position.y, o.waterY_);
      o.sinkT_ = 0.0f;
      o.t_ = 0.0f;
      o.tiltT_ = 0.0f;
      if (aboveWater) {
        m.ChangeState<FallState>();
      } else {
        m.ChangeState<SinkState>();
      }
    }
    /// @brief 状態名
    const char *Name() const override { return "wait"; }
  };

  /// @brief 力が抜けたように、加速しながら水面へ落ちる
  class FallState : public IState<DeathSinkScript> {
  public:
    /// @brief 水面まで落とし、着いたら Sink へ
    void OnUpdate(DeathSinkScript &o, StateMachine<DeathSinkScript> &m, float dt) override {
      auto *tr = o.GetComponent<TransformComponent>();
      if (!tr) return;
      const float u = o.t_ / (std::max)(o.fallTime, 1e-3f);
      tr->position.y = o.startPos_.y + (o.waterY_ - o.startPos_.y) * Chaso::Ease::Evaluate(Chaso::EaseType::InQuad, u);
      o.UpdateTilt(*tr, dt);
      if (u >= 1.0f) {
        tr->position.y = o.waterY_ - 0.05f;
        o.sinkStartY_ = tr->position.y;
        o.t_ = 0.0f;
        m.ChangeState<SinkState>();
      }
    }
    /// @brief 状態名
    const char *Name() const override { return "fall"; }
  };

  /// @brief 着水の勢いのまま沈み、水の抵抗でゆっくりになっていく
  class SinkState : public IState<DeathSinkScript> {
  public:
    /// @brief 沈めながら瞬きを始め、darkDepth に着くか沈みきったら Done へ
    void OnUpdate(DeathSinkScript &o, StateMachine<DeathSinkScript> &m, float dt) override {
      auto *tr = o.GetComponent<TransformComponent>();
      if (!tr) return;
      // 沈み始めた最初のフレームだけ泡を放出する
      if (o.t_ <= dt * 1.5f && o.bubbles) o.bubbles_.SetEmission(WaterCameraFx::Bubbles::kEmitPerFrame);
      o.screenFx.StartBlink(o.TimeUntilDark()); // 沈み始めたら瞬きを開始（2 回目以降は無視される）
      const float u = o.t_ / (std::max)(o.sinkTime, 1e-3f);
      const float targetY = o.waterY_ - o.depth;
      tr->position.y = o.sinkStartY_ + (targetY - o.sinkStartY_) * Chaso::Ease::Evaluate(Chaso::EaseType::OutQuad, u);
      o.UpdateTilt(*tr, dt);
      if ((o.waterY_ - tr->position.y) >= o.darkDepth || u >= 1.0f) {
        o.t_ = 0.0f;
        m.ChangeState<DoneState>();
      }
    }
    /// @brief 状態名
    const char *Name() const override { return "sink"; }
  };

  /// @brief 沈み続けながら瞼が閉じきるのを待ち、遷移を要求する（最後の段階）
  class DoneState : public IState<DeathSinkScript> {
  public:
    /// @brief 瞼が閉じきったら遷移表のきっかけ "gameover" で遷移を要求する
    void OnUpdate(DeathSinkScript &o, StateMachine<DeathSinkScript> &, float dt) override {
      auto *tr = o.GetComponent<TransformComponent>();
      Entity *self = o.GetEntity();
      if (!tr || !self) return;
      tr->position.y -= 2.0f * dt; // 遷移が始まるまで止まって見えないように
      o.UpdateTilt(*tr, dt);
      o.screenFx.StartBlink(0.0f); // Sink を 1 フレームで抜けた場合の保険（下限の長さで閉じる）
      // 瞼が閉じきるまでは遷移を要求しない（閉じきった色＝Dive 遷移の色なので、そのままつながる）
      if (o.requested_ || !o.screenFx.BlinkFinished()) return;
      o.requestT_ += dt;
      // 行き先と演出は遷移表のきっかけ "gameover"（GameMode の決着と同じ行）で引く
      if (o.resultTarget_.empty()) {
        o.LookupTransition(kTriggerGameOver, o.resultTarget_, o.resultTransition_);
      }
      o.requested_ = !o.resultTarget_.empty() && o.RequestSceneChange(o.resultTarget_, o.resultTransition_);
      if (!o.requested_ && o.requestT_ > 1.5f) {
        // 何らかの理由で通らない（遷移中など）。素の遷移で送る
        Log::Print("[DeathSinkScript] transition refused, falling back");
        o.requested_ = !o.resultTarget_.empty() && o.RequestSceneChange(o.resultTarget_);
        if (!o.requested_ && o.requestT_ > 3.0f) {
          self->ClearTag(kOwnerTag);
          o.requested_ = true; // 諦める（DataDrivenScene 側の要求はもう出ないので、ここで止める）
        }
      }
    }
    /// @brief 状態名
    const char *Name() const override { return "done"; }
  };

  static constexpr const char *kOwnerTag = "result_transition_owner";
  static constexpr const char *kTriggerGameOver = "gameover"; ///< 遷移表のきっかけ名

  /// @brief 力尽きたことを検知し、待機→落下→沈降→遷移要求の順にカメラを動かす
  /// @param dt 前フレームからの経過時間（秒）
  void UpdateSink(float dt) {
    if (dt <= 0.0f || !enabled) return;
    Entity *self = GetEntity();
    auto *tr = GetComponent<TransformComponent>();
    if (!self || !tr) return;

    // 待機中: IdleState 自身が game_over タグを検知して Wait へ切り替える。
    // 切り替わったフレームは、そのまま同じフレームで Wait も 1 回進める（以前の実装と同じタイミング）
    if (fsm_.IsIn<IdleState>()) {
      fsm_.Update(*this, dt);
      if (fsm_.IsIn<IdleState>()) return;
    }

    // 段階ごとの進行（各段階の経過時間は t_ で測り、段階が変わるたびに 0 へ戻す）
    t_ += dt;
    fsm_.Update(*this, dt);

    // 泡をカメラに追従させる（Wait 中はまだ動かないので不要）
    if (!fsm_.IsIn<WaitState>()) bubbles_.Follow(*tr, waterY_);

    // 最後の息：沈み始めてから breathAt の位置で 1 回だけ吐く（水中にいるときだけ）
    if (breath && !breathed_ && screenFx.Blinking() && screenFx.SinkProgress() >= breathAt &&
        tr->position.y < waterY_) {
      breath_.Trigger(static_cast<uint32_t>((std::max)(breathCount, 1)), breathTime, breathTrail);
      breathed_ = true;
    }
    breath_.Update(*tr, waterY_, dt);

    // 血・色抜け・瞬きの時間を進める（水に入ったら血を洗い流し始める）
    screenFx.Update(RC::GetRenderContext().GetPostProcess(), dt, tr->position.y < waterY_);
  }

  /// @brief 演出を始める（遷移を引き受け、画面演出を開始する）。IdleState から呼ぶ
  /// @param self 自エンティティ（Camera）
  void BeginSink(Entity &self) {
    self.SetTag(kOwnerTag, 1); // DataDrivenScene の自動遷移を止める（RailMovement / 視点操作も止まる）
    waterY_ = WaterCameraFx::FindWaterY(GetScene());
    t_ = 0.0f;
    requestT_ = 0.0f;
    breathed_ = false;
    screenFx.Begin(RC::GetRenderContext().GetPostProcess(), true);
    Log::Print("[DeathSinkScript] begin");
  }

  /// @brief 沈み始めてから darkDepth（画面が暗くなりきる深さ）に着くまでの見込み時間
  /// @details Sink の深さは sinkStartY_ → waterY_ - depth を EaseOutQuad(t / sinkTime) で進むので、
  ///          1 - (1 - u)^2 = frac を u について解いて sinkTime を掛ける。
  /// @return 秒（すでに darkDepth より深いところで沈み始めたなら 0）
  float TimeUntilDark() const {
    const float d0 = (std::max)(0.0f, waterY_ - sinkStartY_); // 沈み始めの深さ
    const float span = depth - d0;
    if (span <= 1e-3f) return 0.0f;
    const float frac = std::clamp((darkDepth - d0) / span, 0.0f, 1.0f);
    return sinkTime * (1.0f - std::sqrt(1.0f - frac));
  }

  /// @brief 水面を見上げる向きへ、少し横に傾きながらゆっくり回す
  /// @param tr 回転させるカメラの Transform
  /// @param dt 前フレームからの経過時間（秒）
  void UpdateTilt(TransformComponent &tr, float dt) {
    tiltT_ += dt;
    const float r = (tiltTime > 0.0f) ? WaterCameraFx::Smooth01(tiltT_ / tiltTime) : 1.0f;
    tr.rotation.x = startRot_.x + (lookUpPitch - startRot_.x) * r;
    tr.rotation.z = startRot_.z + roll * r;
  }

  // ---- 設定項目（JSON / インスペクタから変える。外からは触らせない）----
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
  bool breath = true;         ///< 最後の息（大きな泡を一度に吐く）を出すか
  float breathAt = 0.33f;     ///< 吐くタイミング（沈み始め 0 〜 瞼が閉じきる 1 の位置。既定は 1 回目の瞬きで開き直すところ）
  int breathCount = 16;       ///< 吐き出しの 1 フレームあたりの泡の数（漏れはこの 1/5）
  float breathTime = 0.1f;    ///< 吐き出しの長さ（秒）
  float breathTrail = 0.5f;   ///< 吐いたあとにゴボゴボと漏れ続ける長さ（秒）
  DeathScreenFx screenFx;     ///< 血・色が抜ける・瞬き・視界の滲み・水面の光

  // ---- 実行中の状態 ----
  StateMachine<DeathSinkScript> fsm_; ///< 演出の段階（IdleState 〜 DoneState）
  float t_ = 0.0f;                    ///< 現在の段階に入ってからの経過（秒）
  float tiltT_ = 0.0f;
  float sinkT_ = 0.0f;
  float waterY_ = 0.0f;
  float sinkStartY_ = 0.0f;
  RC::Vector3 startPos_{};
  RC::Vector3 startRot_{};
  bool requested_ = false;
  float requestT_ = 0.0f;    ///< 遷移の要求を出し始めてからの経過（要求が通らないときの保険用）
  /// @brief 見せ用デモ（ステージ側）。種類は Factory Method で決め、ここでは基底の参照として扱う
  std::unique_ptr<TransitionDemo::StageDemo> stageDemo_ = TransitionDemo::CreateStageDemo();
  std::string resultTarget_;    ///< 遷移表で引いた行き先（Done に入ってから 1 回だけ引く）
  std::string resultTransition_; ///< 遷移表で引いた演出
  WaterCameraFx::Bubbles bubbles_;
  WaterCameraFx::BreathBurst breath_; ///< 最後の息
  bool breathed_ = false;             ///< 今回の沈降で最後の息を吐いたか
};

REGISTER_SCRIPT(DeathSinkScript)
