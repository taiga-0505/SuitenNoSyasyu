#include "ECS/ScriptableEntity.h"
#include "ECS/ScriptRegistry.h"
#include "Common/Log/Log.h"
#include "Input/Input.h"
#include "RenderCommon.h"
#include "Engine/Render/RenderContext.h"
#include "Framework/App.h"
#include "Scene.h"
#include "Game/Framework/ClearChartFx.h"
#include "Game/Framework/GameSession.h"
#include "Game/Framework/StageProgress.h"
#include "Game/Framework/StateMachine.h"
#include "Game/Framework/WaterCameraFx.h"

#if RC_ENABLE_IMGUI
#include "imgui/imgui.h"
#endif

#include <algorithm>
#include <string>

/// @class ClearAscendScript
/// @brief ステージクリアの演出を受け持ち、白へ飛ばしてから Result へ "ascend" 遷移する
/// @details
///   DeathSinkScript（力尽きて沈み、暗くなって Result へ）と対になる演出。
///   置き場所: ステージの Camera エンティティ（RailShooterController / DeathSinkScript と同じ）。
///   スクリプトの並びは RailShooterController より後にしておくこと（海図を HUD の上に描くため）。
///
///   演出は "style" で選ぶ:
///     "chart"（既定）: ステージセレクトと同じ見た目の海図を出し、今クリアした面から次の面まで
///                      航路を引いて「踏破」を押す（ClearChartFx）。航路を一本ずつ進んでいく本作の構成に合わせた演出。
///     "ascend"       : その場から空へ跳ね上がって白く飛ぶ（WaterCameraFx::AscendSequence。旧演出）。
///
///   流れ:
///     1. GameSession がクリアになったら（RailShooterGameMode がレール終点で決着させる）、
///        遷移表（Resources/SceneFlow.json）のきっかけ "cleared" を引く。
///        演出が "ascend" のときだけ自分が引き受け、"result_transition_owner" タグを立てて
///        DataDrivenScene の自動遷移（2.5 秒後）を止める。それ以外の演出なら何もしない。
///     2. chart : "clear_chart_playing" タグを立てて HUD（スコア・照準・緑の帯）を消し、海図を出す。
///                決定入力で早送りできる（1 回目で判まで、2 回目で白へ）。
///        ascend: delay 秒だけ STAGE CLEAR を見せてから、ため → 空へ跳ね上がる。
///     3. どちらも最後は画面を白く飛ばし、白くなりきったら "ascend" 遷移を要求する。
///        SceneManager は同じ白へ抜け、Result（ResultScreenScript）は白から開けて空から降りてくる。
///
///   実装（State パターン）:
///     以前は enum Phase（Idle / Wait / Ascend / Chart / Done / Off）+ switch で段階を切り替えていた。
///     段階ごとの処理と遷移条件が 1 つの関数に混ざっていたので、段階 1 つ = 状態クラス 1 つに分け、
///     次の段階は状態クラス自身が StateMachine::ChangeState で決めるようにした。
///     状態クラスは private な入れ子クラスなので、friend 無しで本体の private メンバを使える。
///
///   JSON (scriptDataList):
///     "enabled" / "style" / "fontPath"
///     chart : "chartHold" / "chartUnroll" / "chartInk" / "chartLinger" / "chartBleach" / "chartDim" / "chartSkippable"
///     ascend: "delay" / "anticipation" / "dip" / "launch" / "height" /
///             "lookUpPitch" / "fovBoost" / "whiteStart" / "whiteEv"
class ClearAscendScript : public ScriptableEntity {
public:
  /// @brief 待機状態（クリアを待つ）で作る
  ClearAscendScript() {
    fsm_.ChangeState<IdleState>();
    fsm_.ApplyPending(*this);
  }

  /// @brief 設定項目を JSON に書き出す
  /// @return 設定項目を格納した JSON
  nlohmann::json Serialize() override {
    nlohmann::json j = {{"enabled", enabled},
                        {"style", style},
                        {"fontPath", fontPath},
                        {"delay", delay},
                        {"anticipation", ascend.anticipation},
                        {"dip", ascend.dip},
                        {"launch", ascend.launch},
                        {"height", ascend.height},
                        {"lookUpPitch", ascend.lookUpPitch},
                        {"fovBoost", ascend.fovBoost},
                        {"whiteStart", ascend.whiteStart},
                        {"whiteEv", ascend.whiteEv}};
    chart.WriteJson(j);
    return j;
  }

  /// @brief JSON から設定項目を読み込む
  /// @param j 読み込み元の JSON（存在し型が合うキーだけ反映する）
  void Deserialize(const nlohmann::json &j) override {
    // キーが存在し数値の場合だけ上書きするヘルパー
    auto f = [&](const char *k, float &o) { if (j.contains(k) && j[k].is_number()) o = j[k].get<float>(); };
    if (j.contains("enabled") && j["enabled"].is_boolean()) enabled = j["enabled"].get<bool>();
    if (j.contains("style") && j["style"].is_string()) style = j["style"].get<std::string>();
    if (j.contains("fontPath") && j["fontPath"].is_string()) fontPath = j["fontPath"].get<std::string>();
    chart.ReadJson(j);
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
  /// @brief インスペクタに現在の状態と各パラメータの編集 UI を表示する
  void OnImGui() override {
    ImGui::Text("State: %s  t=%.2f  white=%.2f", fsm_.CurrentName(), t_, CurrentWhite());
    ImGui::Checkbox("Enabled", &enabled);
    int styleIndex = UseChart() ? 0 : 1;
    if (ImGui::Combo("Style", &styleIndex, "chart (海図)\0ascend (空へ)\0")) {
      style = (styleIndex == 0) ? "chart" : "ascend";
    }
    if (UseChart()) {
      chart.DrawImGui();
    } else {
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
  }
#endif

protected:
  /// @brief 状態を初期化し、前回のプレイで残った遷移担当タグを消して海図のフォントを読み込む
  void OnCreate() override {
    fsm_.ChangeState<IdleState>();
    fsm_.ApplyPending(*this);
    t_ = 0.0f;
    requested_ = false;
    // 前回のプレイで保存された分を消す（DeathSinkScript と同じタグ）
    if (Entity *self = GetEntity()) {
      self->ClearTag(kOwnerTag);
      self->ClearTag(kChartTag);
    }
    if (enabled && UseChart()) LoadChartFonts();
  }

  /// @brief クリアを検知して演出を引き受け、演出→遷移要求の順に進める
  /// @param dt 前フレームからの経過時間（秒）
  void OnUpdate(float dt) override {
    if (dt <= 0.0f || !enabled) return;
    Entity *self = GetEntity();
    if (!self) return;

    // 待機中: IdleState 自身がクリアを検知し、引き受けるなら Wait / Chart、引き受けないなら Off へ切り替える。
    // 引き受けたフレームは、そのまま同じフレームで次の段階も 1 回進める（以前の実装と同じタイミング）
    if (fsm_.IsIn<IdleState>()) {
      fsm_.Update(*this, dt);
      if (fsm_.IsIn<IdleState>() || fsm_.IsIn<OffState>()) return;
    }

    // 段階ごとの進行（各段階の経過時間は t_ で測り、段階が変わるたびに 0 へ戻す）
    t_ += dt;
    fsm_.Update(*this, dt);
  }

  /// @brief 海図（chart）または白い幕（ascend）を描く
  /// @note ポストプロセスの後に描くので、水中の色味や輪郭線に染まらない
  void OnOverlayRender() override {
    // 画面サイズはウィンドウから取得し、取れなければ 1280x720 を仮定する
    float w = 1280.0f, h = 720.0f;
    auto &rc = RC::GetRenderContext();
    if (rc.Ctx() && rc.Ctx()->app) {
      w = static_cast<float>(rc.Ctx()->app->width);
      h = static_cast<float>(rc.Ctx()->app->height);
    }
    if (chart_.IsActive()) {
      chart_.Draw(w, h); // 最後の白い幕も含む
      return;
    }
    const float a = ascend_.WhiteAlpha();
    if (a <= 0.0f) return;
    // SceneManager が Ascend 遷移で抜ける白（kAscendScreenColor）と揃える
    RC::DrawBox({0.0f, 0.0f}, {w, h}, {0.97f, 0.98f, 1.0f, a});
  }

  /// @brief 演出中なら打ち切り、遷移担当タグを外してフォントを解放する
  void OnDestroy() override {
    if (ascend_.IsActive()) ascend_.End(false);
    chart_.End();
    chart_.UnloadFonts();
    fontsLoaded_ = false;
    if (Entity *self = GetEntity()) {
      self->ClearTag(kOwnerTag);
      self->ClearTag(kChartTag);
    }
  }

private:
  // ---- 状態クラス（private な入れ子クラス。次の段階は各状態が ChangeState で決める）----

  /// @brief 待機：クリアになったら遷移表を引き、演出が ascend のときだけ自分が遷移を引き受ける
  class IdleState : public IState<ClearAscendScript> {
  public:
    /// @brief クリアを検知し、引き受けるなら Chart / Wait、引き受けないなら Off へ
    void OnUpdate(ClearAscendScript &o, StateMachine<ClearAscendScript> &m, float) override {
      Entity *self = o.GetEntity();
      SceneContext *ctx = o.GetSceneContext();
      if (!ctx || !ctx->isPlaying()) return;
      if (!GameSession::Get().IsCleared()) return;
      // 力尽きたほうが先なら DeathSinkScript の担当
      if (self->GetTagInt("game_over", 0) == 1 || self->GetTagInt(kOwnerTag, 0) == 1) {
        m.ChangeState<OffState>();
        return;
      }
      std::string transition;
      if (!o.LookupTransition(kTriggerCleared, o.target_, transition) || o.target_.empty() ||
          transition != SceneTransitions::kAscend) {
        // 遷移表の演出が ascend 以外なら DataDrivenScene の自動遷移に任せる
        m.ChangeState<OffState>();
        return;
      }
      self->SetTag(kOwnerTag, 1); // DataDrivenScene の自動遷移を止める
      o.t_ = 0.0f;
      if (o.UseChart()) {
        o.BeginChart(*self);
        m.ChangeState<ChartState>();
      } else {
        m.ChangeState<WaitState>();
      }
      Log::Print("[ClearAscendScript] begin (" + o.style + ") -> " + o.target_);
    }
    /// @brief 状態名
    const char *Name() const override { return "idle"; }
  };

  /// @brief ascend：delay 秒だけ STAGE CLEAR を見せる
  class WaitState : public IState<ClearAscendScript> {
  public:
    /// @brief 待ち終わったら跳ね上がりを始めて Ascend へ（カメラが無ければ演出なしで Done へ）
    void OnUpdate(ClearAscendScript &o, StateMachine<ClearAscendScript> &m, float) override {
      if (o.t_ < o.delay) return;
      Scene *scene = o.GetScene();
      if (!o.ascend_.Begin(WaterCameraFx::FindMainCamera(scene), WaterCameraFx::FindWaterY(scene), o.ascend)) {
        // カメラが見つからない。演出なしで送る
        Log::Print("[ClearAscendScript] camera not found, changing scene directly");
        o.t_ = 0.0f;
        m.ChangeState<DoneState>();
        return;
      }
      o.t_ = 0.0f;
      m.ChangeState<AscendState>();
    }
    /// @brief 状態名
    const char *Name() const override { return "wait"; }
  };

  /// @brief ascend：ため → 空へ跳ね上がり、白くなりきるのを待つ
  class AscendState : public IState<ClearAscendScript> {
  public:
    /// @brief 跳ね上がりを進め、白くなりきった（またはカメラが消えた）ら Done へ
    void OnUpdate(ClearAscendScript &o, StateMachine<ClearAscendScript> &m, float dt) override {
      if (!o.ascend_.Update(dt)) {
        o.ascend_.End(false);
        o.t_ = 0.0f;
        m.ChangeState<DoneState>();
        return;
      }
      if (o.ascend_.IsDone()) {
        o.t_ = 0.0f;
        m.ChangeState<DoneState>();
      }
    }
    /// @brief 状態名
    const char *Name() const override { return "ascend"; }
  };

  /// @brief chart：海図を出し、白くなりきるのを待つ（決定入力で早送り）
  class ChartState : public IState<ClearAscendScript> {
  public:
    /// @brief 海図を進め、白くなりきったら Done へ
    void OnUpdate(ClearAscendScript &o, StateMachine<ClearAscendScript> &m, float dt) override {
      o.chart_.Update(dt, o.SkipPressed());
      if (o.chart_.IsDone()) {
        o.t_ = 0.0f;
        m.ChangeState<DoneState>();
      }
    }
    /// @brief 状態名
    const char *Name() const override { return "chart"; }
  };

  /// @brief 白くなりきった：Ascend 遷移を要求する（通らなければ素の遷移、それでもだめなら諦める）
  class DoneState : public IState<ClearAscendScript> {
  public:
    /// @brief 遷移が通るまで要求を出し続ける
    void OnUpdate(ClearAscendScript &o, StateMachine<ClearAscendScript> &, float dt) override {
      if (o.ascend_.IsActive()) o.ascend_.Update(dt); // 遷移が始まるまで上がり続ける
      if (o.requested_) return;
      o.requested_ = o.RequestSceneChange(o.target_, SceneTransitions::kAscend);
      if (!o.requested_ && o.t_ > 1.5f) {
        // 何らかの理由で通らない（遷移中など）。素の遷移で送る
        Log::Print("[ClearAscendScript] transition refused, falling back");
        o.requested_ = o.RequestSceneChange(o.target_);
        if (!o.requested_ && o.t_ > 3.0f) {
          o.ascend_.End(true);
          o.chart_.End();
          if (Entity *self = o.GetEntity()) {
            self->ClearTag(kOwnerTag);
            self->ClearTag(kChartTag);
          }
          o.requested_ = true; // 諦める
        }
      }
    }
    /// @brief 状態名
    const char *Name() const override { return "done"; }
  };

  /// @brief 引き受けない：ascend 以外の遷移、または力尽きたほうが先。何もしない
  class OffState : public IState<ClearAscendScript> {
  public:
    /// @brief 何もしない
    void OnUpdate(ClearAscendScript &, StateMachine<ClearAscendScript> &, float) override {}
    /// @brief 状態名
    const char *Name() const override { return "off"; }
  };

  static constexpr const char *kOwnerTag = "result_transition_owner";
  static constexpr const char *kChartTag = "clear_chart_playing"; ///< RailShooterController が HUD を消す合図
  static constexpr const char *kTriggerCleared = "cleared";       ///< 遷移表のきっかけ名

  /// @brief 海図の演出を使うか
  bool UseChart() const { return style != "ascend"; }

  /// @brief いま画面に重ねている白の濃さ（インスペクタ表示用）
  float CurrentWhite() const { return chart_.IsActive() ? chart_.WhiteAlpha() : ascend_.WhiteAlpha(); }

  /// @brief 海図のフォントを実解像度に合わせて読み込む（読み込み済みなら何もしない）
  void LoadChartFonts() {
    if (fontsLoaded_) return;
    float w = 1280.0f, h = 720.0f;
    if (SceneContext *ctx = GetSceneContext()) {
      if (ctx->app && ctx->app->width > 0 && ctx->app->height > 0) {
        w = static_cast<float>(ctx->app->width);
        h = static_cast<float>(ctx->app->height);
      }
    }
    chart_.LoadFonts(fontPath, w, h);
    fontsLoaded_ = true;
  }

  /// @brief 海図の演出を始め、HUD を消す合図のタグを立てる（段階の切り替えは呼び出し側の状態が行う）
  /// @param self 自分のエンティティ
  void BeginChart(Entity &self) {
    LoadChartFonts();
    // ステージに入るたびに RailShooterGameMode が SetCurrentScene するので、ここで面の番号が引ける
    // （ステージ外なら -1。海図は航路の強調なしで出る）
    const int stage = StageProgress::Get().Current();
    chart_.Begin(chart, stage, GameSession::Get().Score());
    self.SetTag(kChartTag, 1);
  }

  /// @brief 早送りの決定入力（Space / Enter / 左クリック / A ボタン）があったか
  /// @return このフレームに押されたら true
  bool SkipPressed() const {
    SceneContext *ctx = GetSceneContext();
    if (!ctx || !ctx->input) return false;
    Input *in = ctx->input;
    if (in->IsKeyTrigger(DIK_SPACE) || in->IsKeyTrigger(DIK_RETURN) || in->IsMouseTrigger(0)) return true;
    return in->IsXInputConnected() && in->IsXInputButtonTrigger(XINPUT_GAMEPAD_A);
  }

  // ---- 設定項目（JSON / インスペクタから変える。外からは触らせない）----
  bool enabled = true;
  std::string style = "chart"; ///< 演出の種類（"chart" / "ascend"）
  std::string fontPath = "Resources/fonts/Kiwi_Maru/KiwiMaru-Medium.ttf";
  ClearChartFx::Params chart;
  float delay = 1.6f; ///< ascend: クリアしてから跳ね上がるまで（秒。STAGE CLEAR を見せる）
  WaterCameraFx::AscendParams ascend;

  // ---- 実行中の状態 ----
  StateMachine<ClearAscendScript> fsm_; ///< 演出の段階（IdleState 〜 OffState）
  float t_ = 0.0f;                      ///< 現在の段階に入ってからの経過（秒）
  bool requested_ = false;
  bool fontsLoaded_ = false;
  std::string target_;
  WaterCameraFx::AscendSequence ascend_;
  ClearChartFx::Sequence chart_;
};

REGISTER_SCRIPT(ClearAscendScript)
