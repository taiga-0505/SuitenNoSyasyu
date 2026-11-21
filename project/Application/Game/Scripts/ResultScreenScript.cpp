#include "ECS/ScriptableEntity.h"
#include "ECS/ScriptRegistry.h"
#include "Common/Log/Log.h"
#include "Input/Input.h"
#include "RenderCommon.h"
#include "Engine/Render/RenderContext.h"
#include "Framework/App.h"
#include "Game/Framework/GameInput.h"
#include "Game/Framework/GameSession.h"
#include "Game/Framework/StageProgress.h"
#include "Game/Framework/StateMachine.h"
#include "Game/Framework/TransitionDemo.h"
#include "Game/Framework/UnderwaterLook.h"
#include "Game/Framework/WaterCameraFx.h"
#include "Scene.h"
#include "SceneFlow.h"
#include "Common/Tween/Easing.h"

#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <utility>
#include <vector>

/// @class ResultScreenScript
/// @brief リザルト画面（波の上に「戦果」を掲げる）。クリア時もゲームオーバー時も同じ画面を使う
/// @details
///   構成:
///     - 背景の波は Result.json 側の WaterComponent と真上視点のカメラ（Title と同じ作り）。
///       このスクリプトは 2D の HUD だけを描く。
///     - 画面中央に墨色のパネルを敷き、上から順に
///         見出し（戦果 / RESULT または GAME OVER / 航路 踏破 or 途絶）
///         ウェイポイント達成率（大きな％ ＋ 通過数 ＋ 地点を並べた航路ゲージ）
///         撃破数 / 宝箱 / 被ダメージ（被ダメージには HP のコマも並べる）
///       を置く。下段に「もう一度」「タイトルへ」の 2 つのボタン。
///     - 値はすべて GameSession から読む（Game シーンが書き込んだもの）。
///       クリアかゲームオーバーかは GameSession::IsCleared() で見分け、見出し下の一言と欧文を切り替える。
///     - 入力: ←→ / A D / ↑↓ / W S / 十字キー / 左スティックで選択、
///             Space / Enter / A ボタン / 左クリックで決定。マウスを動かすとホバーで選択が移る。
///       演出中に決定を押すと演出をスキップし、もう一度押すと決定になる。
///     - 演出（カウントアップ、ゲージ、行のフェード、ボタンのフェード）は OnUpdate の時間で進める。
///       シーン切替直後の Dissolve / Grayscale 演出中は Update が止まっているので、
///       その間はパネルと見出しだけが静止して見え、色が戻ってから数字が動き出す。
///
///   注意:
///     - DataDrivenScene 側の「導線シーンで Space → 次へ」判定から Result を外してあること。
///       残っていると Space で決定した瞬間に Title へも飛び、二重判定になる。
///     - 画面サイズは 1280x720 を基準にレイアウトし、実解像度に合わせて等倍スケールする
///       （Debug は 1920x1080）。フォントも基準サイズ × スケールでロードする。
///     - フォントは Title の 3D 文字（TextMesh）と同じ KiwiMaru-Medium を使う。
///
///   JSON (scriptDataList) で設定できる項目:
///     "titleText"       : 見出し（既定 "戦果"）
///     "subtitleText"    : クリア時に見出しの下へ出す欧文（既定 "RESULT"）
///     "failSubtitleText": 未クリア時に見出しの下へ出す欧文（既定 "GAME OVER"）
///     "clearText"       : クリア時の一言（既定 "航路 踏破"）
///     "failText"        : 未クリア時の一言（既定 "航路 途絶"）
///     "retryLabel" / "titleLabel"   : ボタンの文字
///     "nextLabel" / "selectLabel"   : ステージから来たときの「次のステージへ」「セレクトへ」の文字
///   各ボタンの遷移先と演出は遷移表（Resources/SceneFlow.json）のきっかけで決まる:
///     "next" / "retry" / "select" / "title"（演出が "dive" のときだけ飛び込み演出を挟む）
///   ステージ（Stage1〜5）から来たときのボタン:
///     クリア      : 次のステージへ ／ もう一度 ／ セレクトへ（"next" の行き先が無ければ 次へ を出さない）
///     ゲームオーバー: もう一度 ／ セレクトへ
///   ステージ以外から来たとき（エディタで直接開いたときなど）は もう一度 ／ タイトルへ の 2 つ。
///     "fontPath"        : 画面全体で使うフォント（既定は Title の 3D 文字と同じ KiwiMaru-Medium）
///     "showScore"       : 得点の行を足すか
///     "showHint"        : 画面下の操作ヒントを出すか
///     "animate"         : 演出を行うか（false なら最初から確定表示）
///     "dimAlpha"        : 水面全体に掛ける墨の濃さ（0 で掛けない）
///     "panelColor"      : パネルの色 RGBA
///     "accentColor"     : 強調色（達成率ラベル・ゲージ・選択マーカー）RGBA
///     "selectedColor"   : 選択中ボタンの塗り RGBA
///     [決定後の飛び込み]
///     "uiFadeTime"      : 決定してから UI が消えきるまで（秒）。消えきってから水へ飛び込む
///     "dive*"           : 飛び込みの調整値（Title と同じキー。WaterCameraFx::DiveParams）
///     "underwater"      : 水中の見た目（UnderwaterLook::Params。Title / Game と同じキー）
///     飛び込みで暗くなりきったら "dive" 遷移でボタンの行き先へ。
///     "rise*"           : Dive 遷移（DeathSinkScript：力尽きて沈んだとき）で入ってきたときの浮上。
///                         浮上しきってから UI をフェードインし、成績の演出を始める
///     "descend*"        : Ascend 遷移（ClearAscendScript：クリアして空へ抜けたとき）で入ってきたときの降下。
///                         白く飛んだ空から真上視点へ降りきってから UI をフェードインする
///     Game は DeepRiseIntroScript、Title は TitleScreenScript が深海からの浮上で受け取る。
///
///   設計（クラス設計の課題として導入したパターン）:
///     - Command パターン: 下段のボタン 1 つ = ChoiceCommand 1 つ。決定されたら選択中のコマンドの
///       Execute を呼ぶだけにした。「素の遷移をするボタン」と「飛び込み演出を挟むボタン」の違いは
///       派生クラス（DirectChoiceCommand / DiveChoiceCommand）が持ち、どちらを作るかは
///       遷移表を引いた結果から MakeChoice（Factory Method）が決める。
///       以前は決定処理の中で「演出が dive か」を毎回 if で見ていた。
///     - State パターン: ボタンを選んでいる / 決定後の退出演出（UI フェード → 飛び込み）/ 遷移待ち、の
///       3 状態を StateMachine<ResultScreenScript> の状態クラスにした。以前は decided_ / leaving_ の
///       bool 2 つの組み合わせで表しており、退出演出だけが使う経過時間や遷移先も本体に散らばっていた。
class ResultScreenScript : public ScriptableEntity {
public:
  /// @brief 退出演出の状態・ボタンのコマンドを片付ける
  /// @note 状態クラス・コマンドはクラス定義の後ろで定義するので、デストラクタも後ろで定義する
  ///       （unique_ptr が不完全型を delete しないように）
  ~ResultScreenScript() override;

  /// @brief 設定項目を JSON に書き出す
  /// @return 文言・表示・色（SerializeBase）に、演出パラメータ dive / rise / descend / underwater を加えた JSON
  nlohmann::json Serialize() override {
    nlohmann::json j = SerializeBase();
    j["uiFadeTime"] = uiFadeTime_;
    j["underwater"] = look_.ToJson();
    diveParams_.WriteJson(j);
    riseParams_.WriteJson(j);
    descendParams_.WriteJson(j);
    return j;
  }

  /// @brief 文言・フォント・表示切り替え・色の設定項目を JSON にする
  /// @return 演出パラメータを除いた設定項目の JSON
  nlohmann::json SerializeBase() const {
    auto v4 = [](const RC::Vector4 &c) { return nlohmann::json{c.x, c.y, c.z, c.w}; };
    return {
        {"titleText", titleText_},
        {"subtitleText", subtitleText_},
        {"failSubtitleText", failSubtitleText_},
        {"clearText", clearText_},
        {"failText", failText_},
        {"retryLabel", retryLabel_},
        {"titleLabel", titleLabel_},
        {"nextLabel", nextLabel_},
        {"selectLabel", selectLabel_},
        {"fontPath", fontPath_},
        {"showScore", showScore_},
        {"showHint", showHint_},
        {"animate", animate_},
        {"dimAlpha", dimAlpha_},
        {"panelColor", v4(panelColor_)},
        {"accentColor", v4(accentColor_)},
        {"selectedColor", v4(selectedColor_)},
    };
  }

  /// @brief JSON から設定項目を読み込む
  /// @param j 読み込み元の JSON（存在し型が合うキーだけ反映する）
  void Deserialize(const nlohmann::json &j) override {
    // キーが存在し型が一致する場合だけ上書きするヘルパー（数値 / 真偽値 / 文字列 / RGBA 配列）
    auto readF = [&](const char *key, float &out) {
      if (j.contains(key) && j[key].is_number()) out = j[key].get<float>();
    };
    auto readB = [&](const char *key, bool &out) {
      if (j.contains(key) && j[key].is_boolean()) out = j[key].get<bool>();
    };
    auto readS = [&](const char *key, std::string &out) {
      if (j.contains(key) && j[key].is_string()) out = j[key].get<std::string>();
    };
    auto readVec4 = [&](const char *key, RC::Vector4 &out) {
      if (!j.contains(key)) return;
      const auto &c = j[key];
      if (c.is_array() && c.size() >= 4) {
        out = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>(), c[3].get<float>()};
      }
    };

    readS("titleText", titleText_);
    readS("subtitleText", subtitleText_);
    readS("failSubtitleText", failSubtitleText_);
    readS("clearText", clearText_);
    readS("failText", failText_);
    readS("retryLabel", retryLabel_);
    readS("titleLabel", titleLabel_);
    readS("nextLabel", nextLabel_);
    readS("selectLabel", selectLabel_);
    readS("fontPath", fontPath_);
    readB("showScore", showScore_);
    readB("showHint", showHint_);
    readB("animate", animate_);
    readF("dimAlpha", dimAlpha_);
    readVec4("panelColor", panelColor_);
    readVec4("accentColor", accentColor_);
    readVec4("selectedColor", selectedColor_);
    readF("uiFadeTime", uiFadeTime_);
    if (j.contains("underwater")) look_.FromJson(j["underwater"]);
    diveParams_.ReadJson(j);
    riseParams_.ReadJson(j);
    descendParams_.ReadJson(j);
  }

protected:
  /// @brief フォント・ボタン・泡を用意し、入ってきた遷移（Dive / Ascend）に応じて浮上・降下の演出を始める
  void OnCreate() override {
    // 前のシーン（Game）の障害物が水面の定数バッファに残っていると、
    // ここの水面にも岩の反射波が出てしまう。障害物は無いので空にする。
    RC::SetWaterObstacles(nullptr, 0);

    // 表示に使う値をログへ残す（ImGui を使わないので、Release でもここで突き合わせられる）
    {
      const GameSession &s = GameSession::Get();
      Log::Print("[ResultScreenScript] " + std::string(s.IsCleared() ? "cleared" : "game over") +
                 "  waypoints " + std::to_string(s.WaypointsReached()) + "/" +
                 std::to_string(s.WaypointsTotal()) + "  defeated " +
                 std::to_string(s.EnemiesDefeated()) + "  chests " +
                 std::to_string(s.ChestsCollected()) + "  damage " +
                 std::to_string(s.DamageTaken()) + "  hp " + std::to_string(s.PlayerHp()) + "/" +
                 std::to_string(s.PlayerMaxHp()) + "  score " + std::to_string(s.Score()));
    }

    // フォントは基準解像度（1280x720）のサイズ × 実解像度のスケールでロードする。
    // DrawString の scale で拡大するとにじむので、最初から実寸で作る。
    float w = 1280.0f, h = 720.0f;
    if (SceneContext *ctx = GetSceneContext()) {
      if (ctx->app && ctx->app->width > 0 && ctx->app->height > 0) {
        w = static_cast<float>(ctx->app->width);
        h = static_cast<float>(ctx->app->height);
      }
    }
    fontScale_ = (std::min)(w / kDesignW, h / kDesignH);
    if (fontScale_ <= 0.0f) fontScale_ = 1.0f;

    auto load = [&](const std::string &path, float designPx, uint32_t atlas) {
      const int handle = RC::LoadFont(path, std::round(designPx * fontScale_), atlas);
      if (handle < 0) {
        Log::Print("[ResultScreenScript] failed to load font: " + path);
      }
      return handle;
    };
    // すべて同じフォントファイル（Title の 3D 文字と同じ）をサイズ違いでロードする
    headingFont_ = load(fontPath_, kHeadingPx, 1024);
    numberFont_ = load(fontPath_, kNumberPx, 2048);
    valueFont_ = load(fontPath_, kValuePx, 1024);
    buttonFont_ = load(fontPath_, kButtonPx, 1024);
    labelFont_ = load(fontPath_, kLabelPx, 1024);

    // 演出の時刻・ボタン・選択を初期化する（animate が false なら最初から確定表示）
    anim_ = animate_ ? 0.0f : kAnimEnd;
    BuildChoices();
    selected_ = 0;
    demoT_ = 0.0f;
    // ボタンを選ぶ状態から始める
    EnterChoosing();

    if (Input *in = Input::GetInstance()) {
      in->GetGameMousePosition(lastMouseX_, lastMouseY_);
    }

    // 泡のエミッタは先に作っておく（初期化で一瞬止まるので、飛び込みの瞬間には作らない）
    uiAlpha_ = 1.0f;
    if ((diveParams_.enabled && diveParams_.bubbles) || (riseParams_.enabled && riseParams_.bubbles)) {
      bubbles_.Spawn(GetScene(), "ResultBubbles");
    }

    // 力尽きて沈んだ（Dive 遷移で入ってきた）ときは、深海から浮上してからパネルを出す。
    // 編集モードでもスクリプトは作られるので、再生中だけ
    if (SceneContext *ctx = GetSceneContext()) {
      if (riseParams_.enabled && ctx->isPlaying() && ctx->lastTransition == SceneTransition::Dive) {
        Scene *scene = GetScene();
        if (rise_.Begin(WaterCameraFx::FindMainCamera(scene), WaterCameraFx::FindWaterY(scene), riseParams_,
                        look_, &bubbles_)) {
          uiAlpha_ = 0.0f;
          uiFadeIn_ = 0.0f;
        }
      }
      // ステージクリアで空へ抜けた（Ascend 遷移で入ってきた）ときは、白から開けて
      // 空から真上視点へ降りてきてからパネルを出す
      if (descendParams_.enabled && ctx->isPlaying() && ctx->lastTransition == SceneTransition::Ascend) {
        if (descend_.Begin(WaterCameraFx::FindMainCamera(GetScene()), descendParams_)) {
          uiAlpha_ = 0.0f;
          uiFadeIn_ = 0.0f;
        }
      }
    }
  }

  /// @brief 浮上・降下、UI のフェードイン、成績の演出を進め、その後は現在の状態
  ///        （ボタン選択 / 退出演出 / 遷移待ち）に処理を任せる
  /// @param deltaTime 前フレームからの経過時間（秒）
  void OnUpdate(float deltaTime) override {
    if (deltaTime <= 0.0f) return; // 一時停止・編集中は演出も入力も止める
    pulse_ += deltaTime;
    // 浮上中は UI を出さず、演出の時間も進めない（水面の上に上がりきってから始める）
    if (rise_.IsActive()) rise_.Update(deltaTime);
    if (descend_.IsActive()) descend_.Update(deltaTime);
    if (rise_.IsMoving() || descend_.IsMoving()) return;
    if (uiFadeIn_ >= 0.0f) {
      uiFadeIn_ += deltaTime;
      uiAlpha_ = WaterCameraFx::Smooth01(uiFadeIn_ / (std::max)(uiFadeTime_, 1e-3f));
      if (uiAlpha_ >= 1.0f) { uiAlpha_ = 1.0f; uiFadeIn_ = -1.0f; }
    }
    // 演出の時刻を kAnimEnd まで進め、選択切替の弾みを減衰させる
    if (anim_ < kAnimEnd) anim_ = (std::min)(anim_ + deltaTime, kAnimEnd);
    if (bump_ > 0.0f) bump_ = (std::max)(0.0f, bump_ - deltaTime / kBumpTime);

    fsm_.Update(*this, deltaTime);
  }

  /// @brief 演出中なら終わらせ、泡とフォントを解放する
  void OnDestroy() override {
    // 飛び込みの途中でエディタから停止された場合はカメラと画面を戻す
    if (dive_.IsActive()) dive_.End(/*restoreCamera=*/true);
    if (rise_.IsActive()) rise_.End();
    if (descend_.IsActive()) descend_.End();
    bubbles_.Destroy();
    auto unload = [](int &handle) {
      if (handle >= 0) {
        RC::UnloadFont(handle);
        handle = -1;
      }
    };
    unload(headingFont_);
    unload(numberFont_);
    unload(valueFont_);
    unload(buttonFont_);
    unload(labelFont_);
  }

  /// @brief レイアウトを実解像度に合わせ、パネル・見出し・成績・ボタン・操作説明を描く
  void OnRender() override {
    if (uiAlpha_ <= 0.0f) return; // 飛び込みに入ったら UI は描かない
    float screenW = 1280.0f;
    float screenH = 720.0f;
    auto &rc = RC::GetRenderContext();
    if (rc.Ctx() && rc.Ctx()->app) {
      screenW = static_cast<float>(rc.Ctx()->app->width);
      screenH = static_cast<float>(rc.Ctx()->app->height);
    }
    // 1280x720 基準のレイアウトを等倍スケールして中央に置く
    L_.s = (std::min)(screenW / kDesignW, screenH / kDesignH);
    if (L_.s <= 0.0f) L_.s = 1.0f;
    L_.ox = (screenW - kDesignW * L_.s) * 0.5f;
    L_.oy = (screenH - kDesignH * L_.s) * 0.5f;
    // フォントはロード時のスケールで作ってあるので、差分だけ DrawString の scale で吸収する
    L_.fs = (fontScale_ > 0.0f) ? (L_.s / fontScale_) : 1.0f;

    const GameSession &session = GameSession::Get();

    // 水面全体を少し沈めて文字を読みやすくする
    if (dimAlpha_ > 0.0f) {
      UiBox({0.0f, 0.0f}, {screenW, screenH}, {0.02f, 0.03f, 0.06f, dimAlpha_});
    }

    DrawPanel();
    DrawHeading(session);
    DrawWaypointBlock(session);
    DrawStatRows(session);
    DrawButtons();
    if (showHint_) DrawHint();
  }

private:
  // ------------------------------------------------------------------
  // 状態（State パターン。定義はクラスの後ろ）
  // ------------------------------------------------------------------
  class ChoosingState; ///< ボタンを選んでいる
  class LeavingState;  ///< 決定後の退出演出（UI フェード → 飛び込み → dive 遷移の要求）
  class DecidedState;  ///< 遷移を要求済み（フェード待ち）

  // ------------------------------------------------------------------
  // 下段のボタン（Command パターン。定義はクラスの後ろ）
  // ------------------------------------------------------------------
  class ChoiceCommand;       ///< ボタン 1 つ分のコマンドの基底
  class DirectChoiceCommand; ///< 押したらそのまま遷移を要求するボタン
  class DiveChoiceCommand;   ///< 押したら UI を消して水へ飛び込んでから遷移するボタン

  std::vector<std::unique_ptr<ChoiceCommand>> choices_; ///< 並べるボタン（左から）
  /// @brief 並べるボタンの数を返す
  /// @return ボタンの数
  int ChoiceCount() const { return static_cast<int>(choices_.size()); }

  /// @brief 遷移表を引いてボタンのコマンドを作る（Factory Method）
  /// @param label ボタンの文字
  /// @param trigger 遷移表のきっかけ名
  /// @return 演出が dive なら DiveChoiceCommand、それ以外は DirectChoiceCommand。行き先が無ければ nullptr
  /// @details 「どの種類のボタンを作るか」の判断をここ 1 か所に集め、BuildChoices や決定処理が
  ///          具体的なクラス名を知らなくて済むようにした。
  std::unique_ptr<ChoiceCommand> MakeChoice(const std::string &label, const char *trigger);

  /// @brief ボタンのラベルを取得する
  /// @param index ボタンの番号（0 始まり）
  /// @return ラベル
  const std::string &ChoiceLabel(int index) const;
  /// @brief ボタンのきっかけ名を取得する
  /// @param index ボタンの番号（0 始まり）
  /// @return 遷移表のきっかけ名
  const char *ChoiceTrigger(int index) const;

  /// @brief どこから来たか・クリアしたかでボタンを組み立てる
  /// @note コマンドを破棄するコードを含むので、ChoiceCommand が完全型になるクラスの後ろで定義する
  void BuildChoices();

  // ------------------------------------------------------------------
  // レイアウト（1280x720 基準）
  // ------------------------------------------------------------------
  static constexpr float kDesignW = 1280.0f;
  static constexpr float kDesignH = 720.0f;

  // パネル
  static constexpr float kPanelL = 250.0f;
  static constexpr float kPanelT = 150.0f;
  static constexpr float kPanelR = 1030.0f;
  static constexpr float kPanelPad = 48.0f;
  static constexpr float kRowsTop = 250.0f;  ///< パネル上端から成績の行が始まるまで
  static constexpr float kRowH = 46.0f;      ///< 成績 1 行の高さ
  static constexpr float kButtonGap = 72.0f; ///< パネル下端からボタン中心までの距離
  /// @brief パネル下端（行数で伸び縮みする）を求める
  /// @return パネル下端の Y（基準座標）
  float PanelBottom() const {
    // 撃破数 / 宝箱 / 被ダメージ ＋ 任意で得点
    const float rows = showScore_ ? 4.0f : 3.0f;
    return kPanelT + kRowsTop + kRowH * rows + 18.0f;
  }

  // フォント（基準 px）
  static constexpr float kHeadingPx = 44.0f; ///< 見出し・％記号
  static constexpr float kNumberPx = 84.0f;  ///< 達成率の大きな数字
  static constexpr float kValuePx = 32.0f;   ///< 撃破数・被ダメージの数字
  static constexpr float kButtonPx = 28.0f;  ///< ボタン
  static constexpr float kLabelPx = 22.0f;   ///< ラベル・補足

  // 演出タイムライン（秒）
  static constexpr float kCountDuration = 1.1f;  ///< ％のカウントアップとゲージ
  static constexpr float kRowStart = 0.5f;       ///< 1 行目が出始める時刻
  static constexpr float kRowStep = 0.25f;       ///< 行ごとの遅れ
  static constexpr float kRowFade = 0.3f;        ///< 行のフェード時間
  static constexpr float kButtonStart = 1.3f;    ///< ボタンが出始める時刻
  static constexpr float kButtonFade = 0.3f;
  static constexpr float kAnimEnd = 1.7f;        ///< ここまで進んだら演出終了
  static constexpr float kPulsePeriod = 1.4f;    ///< 選択中ボタンの明滅周期
  static constexpr float kBumpTime = 0.16f;      ///< 選択切替時の弾み

  // 色
  static constexpr RC::Vector4 kInk = {0.95f, 0.93f, 0.87f, 1.0f};     ///< 生成り（文字）
  static constexpr RC::Vector4 kMuted = {0.66f, 0.70f, 0.76f, 1.0f};   ///< 鼠（補足文字）
  static constexpr RC::Vector4 kDim = {0.40f, 0.44f, 0.50f, 1.0f};     ///< 未到達・非選択
  static constexpr RC::Vector4 kShu = {0.80f, 0.19f, 0.13f, 1.0f};     ///< 朱（被弾・未クリアの一言）
  static constexpr RC::Vector4 kGold = {0.88f, 0.74f, 0.38f, 1.0f};    ///< 金（終点到達の芯）

  /// @brief 1280x720 基準座標を実画面座標へ変換するための倍率とオフセット
  struct Layout {
    float s = 1.0f;  ///< 基準 → 実ピクセルの倍率
    float ox = 0.0f; ///< 左右の余白（レターボックス）
    float oy = 0.0f; ///< 上下の余白
    float fs = 1.0f; ///< DrawString に渡す補正スケール
  };
  Layout L_;

  /// @brief 基準座標の X / Y を実画面座標へ変換する
  /// @param x 基準座標の X（Y は y）
  /// @return 実画面座標
  float X(float x) const { return L_.ox + x * L_.s; }
  float Y(float y) const { return L_.oy + y * L_.s; }
  /// @brief 基準座標系の長さを実画面の長さへ変換する
  /// @param v 基準座標系での長さ
  /// @return 実画面での長さ
  float S(float v) const { return v * L_.s; }
  /// @brief 基準座標の点を実画面座標の点へ変換する
  /// @param x 基準座標の X
  /// @param y 基準座標の Y
  /// @return 実画面座標の点
  RC::Vector2 P(float x, float y) const { return {X(x), Y(y)}; }

  /// @brief 色の不透明度に係数を掛けた色を作る
  /// @param c 元の色
  /// @param a 不透明度に掛ける係数
  /// @return 不透明度を変えた色
  static RC::Vector4 WithAlpha(const RC::Vector4 &c, float a) { return {c.x, c.y, c.z, c.w * a}; }
  /// @brief 値を 0〜1 に丸める
  /// @param v 元の値
  /// @return 0〜1 に収めた値
  static float Clamp01(float v) { return (std::min)(1.0f, (std::max)(0.0f, v)); }
  /// @brief 演出の時刻 anim_ における、start から dur 秒かけて 0→1 になる進行度を求める
  /// @param start 進み始める時刻（秒）
  /// @param dur 0→1 にかける時間（秒。0 以下なら start を境に 0 / 1 を切り替える）
  /// @return 進行度（0〜1）
  float Phase(float start, float dur) const {
    if (dur <= 0.0f) return anim_ >= start ? 1.0f : 0.0f;
    return Clamp01((anim_ - start) / dur);
  }
  /// @brief ボタンの操作を受け付けてよいか調べる
  /// @return ボタンが出始める時刻 kButtonStart を過ぎていれば true
  bool InputReady() const { return anim_ >= kButtonStart; }

  /// @brief 文字を 1px の影付きで描く（水面の上でも読めるように）
  /// @param font フォントハンドル（負なら何もしない）
  /// @param text 描く文字列（空なら何もしない）
  /// @param x 描画位置の X（実画面座標）
  /// @param y 描画位置の Y（実画面座標。文字の上端）
  /// @param color 文字色（影の濃さもこの不透明度に比例する）
  /// @param align 横方向のそろえ
  /// @param scale ロードしたサイズに対する倍率
  void Text(int font, const std::string &text, float x, float y, const RC::Vector4 &color,
            TextAlign align = TextAlign::Left, float scale = 1.0f) const {
    if (font < 0 || text.empty()) return;
    const float sc = scale * L_.fs;
    const float sh = (std::max)(1.0f, S(1.0f));
    UiString(font, text, {x + sh, y + sh}, {0.0f, 0.02f, 0.05f, 0.55f * color.w}, sc, align);
    UiString(font, text, {x, y}, color, sc, align);
  }
  /// @brief フォントの 1 行の高さ（実画面座標）を求める
  /// @param font フォントハンドル
  /// @param scale ロードしたサイズに対する倍率
  /// @return 行の高さ。フォントが無ければ kLabelPx を基準にした概算値
  float LineH(int font, float scale = 1.0f) const {
    return (font >= 0) ? RC::GetFontLineHeight(font, scale * L_.fs) : S(kLabelPx) * scale;
  }
  /// @brief 文字列の描画幅（実画面座標）を求める
  /// @param font フォントハンドル
  /// @param text 測る文字列
  /// @param scale ロードしたサイズに対する倍率
  /// @return 描画幅。フォントが無ければ 0
  float TextW(int font, const std::string &text, float scale = 1.0f) const {
    return (font >= 0) ? RC::MeasureString(font, text, scale * L_.fs).x : 0.0f;
  }

  // ------------------------------------------------------------------
  // 描画
  // ------------------------------------------------------------------

  /// @brief 墨のパネル（二重の細枠 ＋ 四隅の飾り）を描く
  void DrawPanel() const {
    const RC::Vector2 tl = P(kPanelL, kPanelT);
    const RC::Vector2 br = P(kPanelR, PanelBottom());
    UiBox(tl, br, panelColor_);

    // 内側と外側の細い枠。和綴じの表紙のように 2 本引く
    const float in1 = S(6.0f), in2 = S(11.0f);
    UiBox({tl.x + in1, tl.y + in1}, {br.x - in1, br.y - in1}, WithAlpha(kInk, 0.55f), kWire);
    UiBox({tl.x + in2, tl.y + in2}, {br.x - in2, br.y - in2}, WithAlpha(kInk, 0.22f), kWire);

    // 四隅の飾り（L 字）
    const float arm = S(22.0f);
    const float th = (std::max)(1.0f, S(2.0f));
    const RC::Vector4 c = WithAlpha(accentColor_, 0.9f);
    const float o = S(2.0f);
    UiLine({tl.x + o, tl.y + o}, {tl.x + o + arm, tl.y + o}, c, th);
    UiLine({tl.x + o, tl.y + o}, {tl.x + o, tl.y + o + arm}, c, th);
    UiLine({br.x - o, tl.y + o}, {br.x - o - arm, tl.y + o}, c, th);
    UiLine({br.x - o, tl.y + o}, {br.x - o, tl.y + o + arm}, c, th);
    UiLine({tl.x + o, br.y - o}, {tl.x + o + arm, br.y - o}, c, th);
    UiLine({tl.x + o, br.y - o}, {tl.x + o, br.y - o - arm}, c, th);
    UiLine({br.x - o, br.y - o}, {br.x - o - arm, br.y - o}, c, th);
    UiLine({br.x - o, br.y - o}, {br.x - o, br.y - o - arm}, c, th);
  }

  /// @brief 見出し（戦果）と欧文、クリア／未クリアの一言を描く。パネルの上に置く
  /// @param session 今回のプレイ結果（クリアしたか）
  void DrawHeading(const GameSession &session) const {
    const float cx = X(kDesignW * 0.5f);

    // 見出し：左右に細い罫を伸ばす
    const float headTop = Y(58.0f);
    const float headH = LineH(headingFont_);
    Text(headingFont_, titleText_, cx, headTop, kInk, TextAlign::Center);
    const float halfW = TextW(headingFont_, titleText_) * 0.5f + S(28.0f);
    const float ruleY = headTop + headH * 0.56f;
    const float ruleLen = S(150.0f);
    UiLine({cx - halfW - ruleLen, ruleY}, {cx - halfW, ruleY}, WithAlpha(kInk, 0.5f),
                 (std::max)(1.0f, S(1.0f)));
    UiLine({cx + halfW, ruleY}, {cx + halfW + ruleLen, ruleY}, WithAlpha(kInk, 0.5f),
                 (std::max)(1.0f, S(1.0f)));
    // 罫の外端に小さな点
    UiCircle({cx - halfW - ruleLen, ruleY}, S(2.5f), WithAlpha(kInk, 0.6f));
    UiCircle({cx + halfW + ruleLen, ruleY}, S(2.5f), WithAlpha(kInk, 0.6f));

    // 欧文（小さく、控えめに）。ゲームオーバー時は GAME OVER に差し替える
    const bool cleared = session.IsCleared();
    const std::string &subtitle = cleared ? subtitleText_ : failSubtitleText_;
    if (!subtitle.empty()) {
      Text(labelFont_, subtitle, cx, headTop + headH - S(2.0f),
           cleared ? WithAlpha(kMuted, 0.85f) : WithAlpha(kShu, 0.95f), TextAlign::Center, 0.75f);
    }

    // クリア／未クリアの一言（パネル内の最上段）。ステージから来たときは面の名前を前に添える
    std::string line = cleared ? clearText_ : failText_;
    if (const int stage = StageProgress::Get().Current(); stage >= 0) {
      line = "第" + std::to_string(stage + 1) + "面 " + StageProgress::Info(stage).title + "　" + line;
    }
    const RC::Vector4 col = cleared ? accentColor_ : WithAlpha(kShu, 0.9f);
    Text(labelFont_, line, cx, Y(kPanelT + 22.0f), col, TextAlign::Center, 1.05f);
  }

  /// @brief ウェイポイント達成率（ラベル・大きな％・通過数・航路ゲージ）を、カウントアップしながら描く
  /// @param session 今回のプレイ結果（通過数・総数・達成率）
  void DrawWaypointBlock(const GameSession &session) const {
    const float left = X(kPanelL + kPanelPad);
    const float right = X(kPanelR - kPanelPad);
    const float t = Chaso::Ease::Evaluate(Chaso::EaseType::OutCubic, Phase(0.0f, kCountDuration));

    const int total = session.WaypointsTotal();
    const int reached = (std::min)(session.WaypointsReached(), (std::max)(total, 0));
    const float rate = session.WaypointRate();

    // ラベル
    const float labelY = Y(kPanelT + 62.0f);
    Text(labelFont_, "航路達成率", left, labelY, accentColor_);
    Text(labelFont_, "WAYPOINT", left + TextW(labelFont_, "航路達成率") + S(14.0f),
         labelY + S(4.0f), WithAlpha(kMuted, 0.7f), TextAlign::Left, 0.7f);

    // 大きな％（カウントアップ）
    const int shown = static_cast<int>(std::floor(rate * 100.0f * t + 0.5f));
    const float numY = Y(kPanelT + 94.0f);
    const std::string num = std::to_string(shown);
    Text(numberFont_, num, left, numY, kInk);
    const float numW = TextW(numberFont_, num);
    const float numH = LineH(numberFont_);
    Text(headingFont_, "%", left + numW + S(6.0f), numY + numH - LineH(headingFont_) - S(10.0f),
         WithAlpha(kInk, 0.9f));

    // 通過数（％の右に添える）
    {
      std::string sub;
      if (total > 0) {
        const int shownReached = static_cast<int>(std::floor(static_cast<float>(reached) * t + 0.5f));
        sub = "通過 " + std::to_string(shownReached) + " / " + std::to_string(total) + " 地点";
      } else {
        sub = session.IsCleared() ? "航路の記録なし（クリア扱い）" : "航路の記録なし";
      }
      const float subX = left + numW + S(6.0f) + TextW(headingFont_, "%") + S(28.0f);
      Text(labelFont_, sub, subX, numY + numH - LineH(labelFont_) - S(16.0f), kMuted);
    }

    // 航路ゲージ：出発点 → 各地点 → 終点。到達したところまで浅葱で塗る
    const float gy = Y(kPanelT + 212.0f);
    const float gx0 = left + S(8.0f);
    const float gx1 = right - S(8.0f);
    const float thin = (std::max)(1.0f, S(3.0f));
    const float thick = (std::max)(1.0f, S(4.0f));
    UiLine({gx0, gy}, {gx1, gy}, WithAlpha(kDim, 0.55f), thin);

    // 塗る割合は通過数 / 総数（総数が無ければ達成率）に演出の進行度を掛けたもの
    const float fill = (total > 0) ? (static_cast<float>(reached) / static_cast<float>(total)) * t
                                   : rate * t;
    if (fill > 0.0f) {
      UiLine({gx0, gy}, {gx0 + (gx1 - gx0) * fill, gy}, accentColor_, thick);
    }

    // 出発点（小さな中空の丸）
    UiCircle({gx0, gy}, S(5.0f), WithAlpha(kInk, 0.9f));
    UiCircle({gx0, gy}, S(2.5f), panelColorOpaque());

    // 地点。多すぎるときは丸を間引く（線の塗りだけで割合は分かる）
    if (total > 0) {
      const int maxDots = 24;
      const int step = (total > maxDots) ? ((total + maxDots - 1) / maxDots) : 1;
      for (int i = 0; i < total; ++i) {
        const bool last = (i == total - 1);
        if (!last && (i % step) != 0) continue;
        const float pos = static_cast<float>(i + 1) / static_cast<float>(total);
        const float dx = gx0 + (gx1 - gx0) * pos;
        const bool lit = (i < reached) && (pos <= fill + 1e-4f);
        if (last) {
          // 終点：ひと回り大きく、到達していれば金色の芯を入れる
          UiCircle({dx, gy}, S(9.0f), lit ? kInk : WithAlpha(kDim, 0.9f));
          UiCircle({dx, gy}, S(5.5f), lit ? WithAlpha(selectedColor_, 1.0f) : panelColorOpaque());
          if (lit) UiCircle({dx, gy}, S(2.5f), kGold);
        } else {
          UiCircle({dx, gy}, S(6.0f), lit ? kInk : WithAlpha(kDim, 0.8f));
          UiCircle({dx, gy}, S(3.0f), lit ? WithAlpha(selectedColor_, 1.0f) : panelColorOpaque());
        }
      }
    }
    // 到達点のいちばん先に、いま塗っている先端の光
    if (fill > 0.0f && fill < 1.0f) {
      UiCircle({gx0 + (gx1 - gx0) * fill, gy}, S(4.0f), WithAlpha(kInk, 0.9f));
    }
  }

  /// @brief 撃破数・宝箱・被ダメージ（・得点）の行を、1 行ずつ遅らせてフェードインしながら描く
  /// @param session 今回のプレイ結果
  void DrawStatRows(const GameSession &session) const {
    const float left = X(kPanelL + kPanelPad);
    const float right = X(kPanelR - kPanelPad);
    const float rowH = S(kRowH);
    float rowY = Y(kPanelT + kRowsTop);
    int rowIndex = 0;

    // index 行目の不透明度（kRowStart から kRowStep ずつ遅れて kRowFade 秒でフェードイン）
    auto rowAlpha = [&](int index) {
      return Chaso::Ease::Evaluate(Chaso::EaseType::OutCubic, Phase(kRowStart + kRowStep * static_cast<float>(index), kRowFade));
    };

    // 行の下の薄い罫線
    auto drawRule = [&](float y, float a) {
      UiLine({left, y}, {right, y}, WithAlpha(kInk, 0.18f * a), (std::max)(1.0f, S(1.0f)));
    };

    // 行のラベル（和文 ＋ 小さな欧文）を左に置く
    auto drawLabel = [&](const std::string &jp, const std::string &en, float y, float a) {
      Text(labelFont_, jp, left, y, WithAlpha(kInk, a));
      Text(labelFont_, en, left + TextW(labelFont_, jp) + S(14.0f), y + S(4.0f),
           WithAlpha(kMuted, 0.7f * a), TextAlign::Left, 0.7f);
    };
    // 行の数値を右端に置き、その左端の x を返す（単位やコマを続けて置くため）
    const float valueY = (rowH - LineH(valueFont_)) * 0.5f;
    auto drawValue = [&](const std::string &value, float rightX, float y, const RC::Vector4 &color,
                         float a) {
      Text(valueFont_, value, rightX, y + valueY, WithAlpha(color, a), TextAlign::Right);
      return rightX - TextW(valueFont_, value);
    };

    // --- 撃破数 ---
    {
      const float a = rowAlpha(rowIndex);
      const float ty = rowY + (rowH - LineH(labelFont_)) * 0.5f;
      drawLabel("撃破数", "DEFEATED", ty, a);
      const std::string unit = "体";
      const float unitW = TextW(labelFont_, unit);
      Text(labelFont_, unit, right, ty, WithAlpha(kMuted, a), TextAlign::Right);
      drawValue(std::to_string(session.EnemiesDefeated()), right - unitW - S(8.0f), rowY, kInk, a);
      drawRule(rowY + rowH, a);
      rowY += rowH;
      ++rowIndex;
    }

    // --- 宝箱 ---
    {
      const float a = rowAlpha(rowIndex);
      const float ty = rowY + (rowH - LineH(labelFont_)) * 0.5f;
      drawLabel("宝箱", "TREASURE", ty, a);
      const std::string unit = "個";
      const float unitW = TextW(labelFont_, unit);
      Text(labelFont_, unit, right, ty, WithAlpha(kMuted, a), TextAlign::Right);
      const int chests = session.ChestsCollected();
      drawValue(std::to_string(chests), right - unitW - S(8.0f), rowY, chests > 0 ? kInk : kMuted, a);
      drawRule(rowY + rowH, a);
      rowY += rowH;
      ++rowIndex;
    }

    // --- 被ダメージ ---
    {
      const float a = rowAlpha(rowIndex);
      const float ty = rowY + (rowH - LineH(labelFont_)) * 0.5f;
      drawLabel("被ダメージ", "DAMAGE", ty, a);

      const int damage = session.DamageTaken();
      const int maxHp = session.PlayerMaxHp();
      const float valueLeft =
          drawValue(std::to_string(damage), right, rowY, damage > 0 ? kInk : kMuted, a);

      // HP のコマ：残った分は浅葱、削られた分は朱。maxHp が分からなければ出さない
      if (maxHp > 0 && maxHp <= 40) {
        // コマの列は数値の左に 20px 空けて右詰めで並べる
        const float pipW = S(12.0f), pipH = S(16.0f), gap = S(4.0f);
        const int lost = (std::min)(damage, maxHp);
        float px = valueLeft - S(20.0f) - (pipW + gap) * static_cast<float>(maxHp) + gap;
        const float py = rowY + (rowH - pipH) * 0.5f;
        for (int i = 0; i < maxHp; ++i) {
          const bool remain = i < (maxHp - lost);
          const RC::Vector4 c = remain ? WithAlpha(accentColor_, 0.95f * a) : WithAlpha(kShu, 0.95f * a);
          UiBox({px, py}, {px + pipW, py + pipH}, c);
          if (!remain) {
            // 削られたコマは中を抜いて「欠け」に見せる
            UiBox({px + S(3.0f), py + S(3.0f)}, {px + pipW - S(3.0f), py + pipH - S(3.0f)},
                        WithAlpha(panelColorOpaque(), a));
          }
          px += pipW + gap;
        }
      }
      drawRule(rowY + rowH, a);
      rowY += rowH;
      ++rowIndex;
    }

    // --- 得点（任意）---
    if (showScore_) {
      const float a = rowAlpha(rowIndex);
      const float ty = rowY + (rowH - LineH(labelFont_)) * 0.5f;
      drawLabel("得点", "SCORE", ty, a);
      drawValue(std::to_string(session.Score()), right, rowY, kInk, a);
      drawRule(rowY + rowH, a);
      rowY += rowH;
      ++rowIndex;
    }
  }

  /// @brief ボタンの矩形（基準座標）を求める
  /// @param index ボタンの番号（0 始まり、左から）
  /// @param l 左端の出力先
  /// @param t 上端の出力先
  /// @param r 右端の出力先
  /// @param b 下端の出力先
  void ButtonRect(int index, float &l, float &t, float &r, float &b) const {
    // ボタンが 3 つ以上なら幅と間隔を詰め、全体を画面の横中央にそろえる
    const int n = (std::max)(1, ChoiceCount());
    const float w = (n >= 3) ? 220.0f : 240.0f, h = 54.0f, gap = (n >= 3) ? 34.0f : 56.0f;
    const float span = w * n + gap * (n - 1);
    const float cx = kDesignW * 0.5f - span * 0.5f + w * 0.5f + (w + gap) * static_cast<float>(index);
    const float cy = PanelBottom() + kButtonGap; // パネルの丈に合わせて追従する
    l = cx - w * 0.5f;
    r = cx + w * 0.5f;
    t = cy - h * 0.5f;
    b = cy + h * 0.5f;
  }

  /// @brief 下段のボタン（次のステージへ / もう一度 / セレクトへ / タイトルへ のうち BuildChoices で並べたもの）を描く
  void DrawButtons() const {
    const float a = Chaso::Ease::Evaluate(Chaso::EaseType::OutCubic, Phase(kButtonStart, kButtonFade));
    if (a <= 0.0f) return;

    // pulse は kPulsePeriod 秒周期で 0〜1 を往復する明滅量
    const float pulse = 0.5f + 0.5f * std::sin(pulse_ * (6.28318530718f / kPulsePeriod));
    for (int i = 0; i < ChoiceCount(); ++i) {
      float l, t, r, b;
      ButtonRect(i, l, t, r, b);
      const bool sel = (i == selected_);

      // 選択切替の瞬間だけ少し膨らむ
      if (sel && bump_ > 0.0f) {
        const float k = 1.0f + 0.05f * bump_;
        const float cx = (l + r) * 0.5f, cy = (t + b) * 0.5f;
        l = cx + (l - cx) * k;
        r = cx + (r - cx) * k;
        t = cy + (t - cy) * k;
        b = cy + (b - cy) * k;
      }
      const RC::Vector2 tl = P(l, t);
      const RC::Vector2 br = P(r, b);

      // 選択中は藍の塗り・明滅する枠・外側の浅葱の枠・左の三角マーカー、非選択は墨の塗りと鼠の枠
      if (sel) {
        UiBox(tl, br, WithAlpha(selectedColor_, a));
        const float borderA = (0.6f + 0.4f * pulse) * a;
        UiBox(tl, br, WithAlpha(kInk, borderA), kWire);
        const float o = S(4.0f);
        UiBox({tl.x - o, tl.y - o}, {br.x + o, br.y + o}, WithAlpha(accentColor_, 0.5f * borderA),
                    kWire);
        // 左端の三角マーカー
        const float mx = tl.x - S(18.0f);
        const float my = (tl.y + br.y) * 0.5f;
        UiTriangle({mx - S(6.0f), my - S(7.0f)}, {mx + S(6.0f), my}, {mx - S(6.0f), my + S(7.0f)},
                         WithAlpha(accentColor_, a));
      } else {
        UiBox(tl, br, WithAlpha(panelColor_, a));
        UiBox(tl, br, WithAlpha(kDim, 0.8f * a), kWire);
      }

      const std::string &label = ChoiceLabel(i);
      const float lh = LineH(buttonFont_);
      Text(buttonFont_, label, (tl.x + br.x) * 0.5f, (tl.y + br.y) * 0.5f - lh * 0.5f,
           WithAlpha(sel ? kInk : kMuted, a), TextAlign::Center);
    }
  }

  /// @brief 画面下の操作ヒントと、新しく解放された面の知らせを描く
  void DrawHint() const {
    const float a = Chaso::Ease::Evaluate(Chaso::EaseType::OutCubic, Phase(kButtonStart, kButtonFade));
    if (a <= 0.0f) return;
    const char *text = InputReady() ? "←→ 選択　　SPACE / Enter 決定" : "";
    // 今回のクリアで次の面が開いたら、ボタンの上に一言添える
    const int unlocked = StageProgress::Get().JustUnlocked();
    if (unlocked >= 0 && GameSession::Get().IsCleared()) {
      const std::string notice = "第" + std::to_string(unlocked + 1) + "面「" +
                                 StageProgress::Info(unlocked).title + "」が解放された";
      Text(labelFont_, notice, X(kDesignW * 0.5f), Y(PanelBottom() + 14.0f), WithAlpha(kGold, a),
           TextAlign::Center, 0.9f);
    }
    Text(labelFont_, text, X(kDesignW * 0.5f), Y(PanelBottom() + kButtonGap + 54.0f),
         WithAlpha(kMuted, 0.8f * a), TextAlign::Center, 0.85f);
  }

  /// @brief パネル色を不透明にした色を返す（丸やコマの中を「抜いた」ように見せるのに使う）
  /// @return 不透明度 1 のパネル色
  RC::Vector4 panelColorOpaque() const { return {panelColor_.x, panelColor_.y, panelColor_.z, 1.0f}; }

  // ------------------------------------------------------------------
  // 入力
  // ------------------------------------------------------------------

  /// @brief キーボード・ゲームパッド・マウスの入力から、演出のスキップ・ボタンの選択・決定を処理する
  /// @param machine 遷移を予約するステートマシン（決定したボタンのコマンドに渡す）
  void HandleInput(StateMachine<ResultScreenScript> &machine) {
    SceneContext *ctx = GetSceneContext();
    if (!ctx || !ctx->input) return;
    Input *in = ctx->input;

    // 決定系の入力をまとめる（演出中はスキップに使う）
    bool confirm = GameInput::Triggered(GameInput::kConfirm);

    if (!InputReady()) {
      if (confirm || in->IsMouseTrigger(0)) {
        // 演出をスキップして確定表示にする（このフレームの押下は決定に使わない）
        anim_ = kAnimEnd;
      }
      return;
    }

    // 左右（上下でも切り替えられるようにしておく）。スティックも倒した瞬間だけ 1 つ動く
    int move = GameInput::TriggeredAxis(GameInput::kMenuLeft, GameInput::kMenuRight) +
               GameInput::TriggeredAxis(GameInput::kMenuUp, GameInput::kMenuDown);
    move = std::clamp(move, -1, 1);

    // マウス：動かしたときだけホバーで選択を移す（キー操作を邪魔しない）
    float mx = lastMouseX_, my = lastMouseY_;
    in->GetGameMousePosition(mx, my);
    const bool mouseMoved = (std::fabs(mx - lastMouseX_) > 0.5f || std::fabs(my - lastMouseY_) > 0.5f);
    lastMouseX_ = mx;
    lastMouseY_ = my;
    const int hovered = HitTestButtons(mx, my);
    if (mouseMoved && hovered >= 0 && hovered != selected_) {
      SetSelected(hovered);
      move = 0;
    }
    if (in->IsMouseTrigger(0) && hovered >= 0) {
      SetSelected(hovered);
      confirm = true;
    }

    // 選択の移動は端で反対側へ回り込む
    if (move != 0 && ChoiceCount() > 0) {
      SetSelected((selected_ + move + ChoiceCount()) % ChoiceCount());
    }

    // 見せ用デモ：kResultHold 秒たったら「セレクトへ」を選び、少し見せてから決定する
    if (TransitionDemo::kEnabled && demoT_ >= TransitionDemo::kResultHold) {
      int selectIndex = -1;
      for (int i = 0; i < ChoiceCount(); ++i) {
        if (std::string(ChoiceTrigger(i)) == "select") selectIndex = i;
      }
      if (selectIndex < 0) {
        // 「セレクトへ」が無い（ステージ以外から来た）ときは「タイトルへ」、それも無ければ先頭
        for (int i = 0; i < ChoiceCount(); ++i) {
          if (std::string(ChoiceTrigger(i)) == "title") selectIndex = i;
        }
      }
      if (selectIndex < 0 && ChoiceCount() > 0) selectIndex = 0;
      if (selectIndex >= 0) SetSelected(selectIndex);
      if (demoT_ >= TransitionDemo::kResultHold + TransitionDemo::kResultConfirmDelay) confirm = true;
    }

    // 決定: 選んだボタンのコマンドを実行する（素の遷移か、飛び込みを挟むかはコマンドの種類が決める）
    if (!confirm || choices_.empty()) return;
    SceneContext *sc = GetSceneContext();
    if (sc && !sc->isPlaying()) return; // 編集モードでは飛ばない（RequestSceneChange と同じ）
    ExecuteChoice(std::clamp(selected_, 0, ChoiceCount() - 1), machine);
  }

  /// @brief index 番目のボタンのコマンドを実行する
  /// @param index ボタンの番号（0 始まり。範囲内であること）
  /// @param machine 遷移を予約するステートマシン
  void ExecuteChoice(int index, StateMachine<ResultScreenScript> &machine);

  // ------------------------------------------------------------------
  // 決定後：UI フェード → 飛び込み → 遷移
  // ------------------------------------------------------------------

  static constexpr float kLeaveRetrySeconds = 1.0f; ///< 遷移要求が通らないとき諦めるまでの秒数

  // 退出演出そのもの（UI フェード → 飛び込み → dive 遷移の要求）は LeavingState::OnUpdate にある

  /// @brief ボタンを選ぶ状態から始める（OnCreate で呼ぶ）
  void EnterChoosing();

  /// @brief 演出なしで遷移を要求し、通れば遷移待ちへ、通らなければボタン操作へ戻す
  /// @param target 遷移先のシーン名
  /// @param machine 遷移を予約するステートマシン
  void LeaveDirectly(const std::string &target, StateMachine<ResultScreenScript> &machine);

  /// @brief 飛び込みを取りやめてボタンを操作できる状態へ戻す
  /// @param machine 遷移を予約するステートマシン
  void CancelLeave(StateMachine<ResultScreenScript> &machine);

  /// @brief 色の不透明度に UI 全体のフェード量（uiAlpha_）を掛ける
  /// @param c 元の色
  /// @return フェードを反映した色
  RC::Vector4 Ui(const RC::Vector4 &c) const { return {c.x, c.y, c.z, c.w * uiAlpha_}; }
  /// @brief UI のフェードを反映して RC::DrawString / DrawBox / DrawLine / DrawCircle / DrawTriangle を呼ぶラッパー群
  /// @note 引数は各 RC::Draw* と同じ（color だけ Ui() で不透明度を下げてから渡す）
  void UiString(int font, const std::string &text, const RC::Vector2 &pos, const RC::Vector4 &color,
                float scale = 1.0f, TextAlign align = TextAlign::Left) const {
    RC::DrawString(font, text, pos, Ui(color), scale, align);
  }
  void UiBox(const RC::Vector2 &a, const RC::Vector2 &b, const RC::Vector4 &color,
             kFillMode mode = kFill, float feather = 1.0f) const {
    RC::DrawBox(a, b, Ui(color), mode, feather);
  }
  void UiLine(const RC::Vector2 &a, const RC::Vector2 &b, const RC::Vector4 &color,
              float thickness = 1.0f, float feather = 1.0f) const {
    RC::DrawLine(a, b, Ui(color), thickness, feather);
  }
  void UiCircle(const RC::Vector2 &c, float r, const RC::Vector4 &color, kFillMode mode = kFill,
                float feather = 1.0f) const {
    RC::DrawCircle(c, r, Ui(color), mode, feather);
  }
  void UiTriangle(const RC::Vector2 &a, const RC::Vector2 &b, const RC::Vector2 &c,
                  const RC::Vector4 &color, kFillMode mode = kFill, float feather = 1.0f) const {
    RC::DrawTriangle(a, b, c, Ui(color), mode, feather);
  }

  /// @brief 選択中のボタンを変え、変わったときは明滅をやり直して弾ませる
  /// @param index 新しく選ぶボタンの番号
  void SetSelected(int index) {
    if (index == selected_) return;
    selected_ = index;
    pulse_ = 0.0f;
    bump_ = 1.0f;
  }

  /// @brief マウス座標（ゲーム解像度基準）がどのボタンの上か調べる
  /// @param mx マウスの X（実画面座標）
  /// @param my マウスの Y（実画面座標）
  /// @return ボタンの番号。どのボタンの上でもなければ -1
  int HitTestButtons(float mx, float my) const {
    for (int i = 0; i < ChoiceCount(); ++i) {
      float l, t, r, b;
      ButtonRect(i, l, t, r, b);
      if (mx >= X(l) && mx <= X(r) && my >= Y(t) && my <= Y(b)) return i;
    }
    return -1;
  }

  // ------------------------------------------------------------------
  // 設定項目（JSON。外から直接書き換えさせず、Deserialize だけが触る）
  // ------------------------------------------------------------------
  // ---- 文言 ----
  std::string titleText_ = "戦果";
  std::string subtitleText_ = "RESULT";
  std::string failSubtitleText_ = "GAME OVER";
  std::string clearText_ = "航路 踏破";
  std::string failText_ = "航路 途絶";
  std::string retryLabel_ = "もう一度";
  std::string titleLabel_ = "タイトルへ";
  std::string nextLabel_ = "次のステージへ";
  std::string selectLabel_ = "セレクトへ";

  // ---- フォント（Title の 3D 文字と同じもの）----
  std::string fontPath_ = "Resources/fonts/Kiwi_Maru/KiwiMaru-Medium.ttf";

  // ---- 表示の切り替え ----
  bool showScore_ = false;
  bool showHint_ = true;
  bool animate_ = true;

  // ---- 色（藍と墨）----
  float dimAlpha_ = 0.22f;
  RC::Vector4 panelColor_ = {0.05f, 0.06f, 0.09f, 0.74f};    ///< 墨
  RC::Vector4 accentColor_ = {0.42f, 0.76f, 0.88f, 1.0f};    ///< 浅葱
  RC::Vector4 selectedColor_ = {0.12f, 0.27f, 0.50f, 0.96f}; ///< 藍

  // ---- 決定後：UI を消してから水へ飛び込む ----
  float uiFadeTime_ = 0.35f;
  WaterCameraFx::DiveParams diveParams_;       ///< 決定後の飛び込み（JSON の "dive*"）
  WaterCameraFx::RiseParams riseParams_;       ///< Dive 遷移（力尽きて沈んだとき）で入ってきたときの浮上
  WaterCameraFx::DescendParams descendParams_; ///< Ascend 遷移（ステージクリアで空へ抜けたとき）で入ってきたときの降下
  UnderwaterLook::Params look_;                ///< 水中の見た目（JSON の "underwater"）

  // ------------------------------------------------------------------
  // 状態
  // ------------------------------------------------------------------
  int headingFont_ = -1;
  int numberFont_ = -1;
  int valueFont_ = -1;
  int buttonFont_ = -1;
  int labelFont_ = -1;
  float fontScale_ = 1.0f;

  StateMachine<ResultScreenScript> fsm_; ///< ボタン選択 / 退出演出 / 遷移待ち
  float anim_ = 0.0f;
  float pulse_ = 0.0f;
  float bump_ = 0.0f;
  float demoT_ = 0.0f; ///< 見せ用デモ：操作できるようになってからの経過（TransitionDemo）
  int selected_ = 0;
  float lastMouseX_ = 0.0f;
  float lastMouseY_ = 0.0f;

  float uiAlpha_ = 1.0f;          ///< UI 全体の不透明度
  WaterCameraFx::DiveSequence dive_;
  WaterCameraFx::RiseSequence rise_;
  WaterCameraFx::DescendSequence descend_;
  float uiFadeIn_ = -1.0f;        ///< 浮上後の UI フェードイン（負なら無効）
  WaterCameraFx::Bubbles bubbles_;
};

// ======================================================================
// 状態クラス（State パターン）
// ======================================================================

/// @brief ボタンを選んでいる状態。入力を受け付け、決定されたらボタンのコマンドが次の状態を決める
class ResultScreenScript::ChoosingState : public IState<ResultScreenScript> {
public:
  /// @brief 見せ用デモの時間を測り、入力を処理する
  void OnUpdate(ResultScreenScript &owner, StateMachine<ResultScreenScript> &machine, float deltaTime) override {
    // 見せ用デモ：成績が出そろって操作できるようになってからの時間を測る（自動で「セレクトへ」）
    if (TransitionDemo::kEnabled && owner.InputReady()) owner.demoT_ += deltaTime;
    owner.HandleInput(machine);
  }
  /// @brief 状態名
  /// @return "Choosing"
  const char *Name() const override { return "Choosing"; }
};

/// @brief 決定後の退出演出（UI フェード → 飛び込み → dive 遷移の要求）を進める状態
/// @details 経過時間・遷移先・要求済みフラグはこの状態だけが使うので、本体ではなくここに持つ。
class ResultScreenScript::LeavingState : public IState<ResultScreenScript> {
public:
  /// @brief 遷移先を受け取る
  /// @param target 飛び込み後に遷移するシーン名
  explicit LeavingState(std::string target) : target_(std::move(target)) {}
  /// @brief UI を消し、飛び込みを進め、暗くなりきったら遷移を要求する
  /// @note 飛び込みを始められないときは素の遷移で送り、遷移も通らなければボタン操作へ戻す
  void OnUpdate(ResultScreenScript &owner, StateMachine<ResultScreenScript> &machine, float deltaTime) override;
  /// @brief 状態名
  /// @return "Leaving"
  const char *Name() const override { return "Leaving"; }

private:
  std::string target_;     ///< 飛び込み後に遷移するシーン
  float time_ = 0.0f;      ///< 退出を始めてからの経過時間（秒）
  bool requested_ = false; ///< dive 遷移の要求が通ったか
};

/// @brief 遷移を要求済みの状態。フェードを待つだけで入力は受け付けない（二重に遷移させない）
class ResultScreenScript::DecidedState : public IState<ResultScreenScript> {
public:
  /// @brief 何もしない
  void OnUpdate(ResultScreenScript &owner, StateMachine<ResultScreenScript> &machine, float deltaTime) override {
    (void)owner;
    (void)machine;
    (void)deltaTime;
  }
  /// @brief 状態名
  /// @return "Decided"
  const char *Name() const override { return "Decided"; }
};

// ======================================================================
// 下段のボタン（Command パターン）
// ======================================================================

/// @brief 下段のボタン 1 つ分のコマンド
/// @details ラベル・きっかけ名・行き先を持ち、決定されたときの処理を Execute にまとめる。
///          本体は選択中のコマンドの Execute を呼ぶだけで、ボタンの種類を知らない。
class ResultScreenScript::ChoiceCommand {
public:
  /// @brief ボタンの中身を受け取る
  /// @param label ボタンの文字
  /// @param trigger 遷移表のきっかけ名（ログにも使う）
  /// @param target 遷移表で引いた行き先
  ChoiceCommand(std::string label, const char *trigger, std::string target)
      : label_(std::move(label)), trigger_(trigger), target_(std::move(target)) {}
  virtual ~ChoiceCommand() = default;

  /// @brief ボタンが決定されたときの処理
  /// @param owner リザルト画面本体
  /// @param machine 遷移を予約するステートマシン
  virtual void Execute(ResultScreenScript &owner, StateMachine<ResultScreenScript> &machine) const = 0;

  /// @brief ボタンの文字
  /// @return ラベル
  const std::string &Label() const { return label_; }
  /// @brief 遷移表のきっかけ名
  /// @return きっかけ名
  const char *Trigger() const { return trigger_; }
  /// @brief 遷移表で引いた行き先
  /// @return シーン名
  const std::string &Target() const { return target_; }

private:
  std::string label_;   ///< ボタンの文字
  const char *trigger_; ///< 遷移表のきっかけ名
  std::string target_;  ///< 遷移表で引いた行き先
};

/// @brief 押したらそのまま遷移を要求するボタン（遷移表の演出が dive 以外）
class ResultScreenScript::DirectChoiceCommand : public ResultScreenScript::ChoiceCommand {
public:
  /// @brief ボタンの中身を受け取る
  /// @param label ボタンの文字
  /// @param trigger 遷移表のきっかけ名
  /// @param target 遷移表で引いた行き先
  DirectChoiceCommand(std::string label, const char *trigger, std::string target)
      : ChoiceCommand(std::move(label), trigger, std::move(target)) {}
  /// @brief 遷移を要求し、通れば遷移待ちへ
  void Execute(ResultScreenScript &owner, StateMachine<ResultScreenScript> &machine) const override;
};

/// @brief 押したら UI を消して水へ飛び込んでから遷移するボタン（遷移表の演出が dive）
/// @details 飛び込みが無効（JSON の dive が off）のときは素の遷移と同じ動きにするため、DirectChoiceCommand を継承する。
class ResultScreenScript::DiveChoiceCommand : public ResultScreenScript::DirectChoiceCommand {
public:
  /// @brief ボタンの中身を受け取る
  /// @param label ボタンの文字
  /// @param trigger 遷移表のきっかけ名
  /// @param target 遷移表で引いた行き先
  DiveChoiceCommand(std::string label, const char *trigger, std::string target)
      : DirectChoiceCommand(std::move(label), trigger, std::move(target)) {}
  /// @brief 退出演出の状態へ移る（飛び込みが無効なら素の遷移）
  void Execute(ResultScreenScript &owner, StateMachine<ResultScreenScript> &machine) const override;
};

// ======================================================================
// 定義（状態・コマンドがすべて完全型になってから書く）
// ======================================================================

ResultScreenScript::~ResultScreenScript() = default;

void ResultScreenScript::BuildChoices() {
  choices_.clear();
  const StageProgress &prog = StageProgress::Get();
  const bool cleared = GameSession::Get().IsCleared();
  // 遷移表を引いて、行き先があるボタンだけ並べる
  auto add = [&](const std::string &label, const char *trigger) {
    if (std::unique_ptr<ChoiceCommand> c = MakeChoice(label, trigger)) {
      choices_.push_back(std::move(c));
    }
  };
  if (prog.Current() >= 0) {
    // ステージから来た（最終面は "next" の行き先 $nextStage が空になるので出ない）
    if (cleared) add(nextLabel_, "next");
    add(retryLabel_, "retry");
    add(selectLabel_, "select");
  } else {
    // ステージ以外から来た（エディタで Result を直接開いたときなど）
    add(retryLabel_, "retry");
    add(titleLabel_, "title");
  }
}

std::unique_ptr<ResultScreenScript::ChoiceCommand> ResultScreenScript::MakeChoice(const std::string &label,
                                                                                  const char *trigger) {
  std::string target;
  std::string transition;
  if (!LookupTransition(trigger, target, transition) || target.empty()) return nullptr;
  if (transition == SceneTransitions::kDive) return std::make_unique<DiveChoiceCommand>(label, trigger, target);
  return std::make_unique<DirectChoiceCommand>(label, trigger, target);
}

const std::string &ResultScreenScript::ChoiceLabel(int index) const {
  return choices_[static_cast<size_t>(index)]->Label();
}

const char *ResultScreenScript::ChoiceTrigger(int index) const {
  return choices_[static_cast<size_t>(index)]->Trigger();
}

void ResultScreenScript::ExecuteChoice(int index, StateMachine<ResultScreenScript> &machine) {
  choices_[static_cast<size_t>(index)]->Execute(*this, machine);
}

void ResultScreenScript::EnterChoosing() {
  fsm_.ChangeState<ChoosingState>();
  fsm_.ApplyPending(*this);
}

void ResultScreenScript::LeaveDirectly(const std::string &target, StateMachine<ResultScreenScript> &machine) {
  if (RequestSceneChange(target)) {
    machine.ChangeState<DecidedState>();
  } else {
    CancelLeave(machine);
  }
}

void ResultScreenScript::CancelLeave(StateMachine<ResultScreenScript> &machine) {
  uiAlpha_ = 1.0f;
  machine.ChangeState<ChoosingState>();
}

void ResultScreenScript::LeavingState::OnUpdate(ResultScreenScript &owner, StateMachine<ResultScreenScript> &machine,
                                                float deltaTime) {
  time_ += deltaTime;

  // 1) UI を消す
  if (!owner.dive_.IsActive()) {
    owner.uiAlpha_ = 1.0f - WaterCameraFx::Smooth01(time_ / (std::max)(owner.uiFadeTime_, 1e-3f));
    if (owner.uiAlpha_ > 0.0f) return;
    owner.uiAlpha_ = 0.0f;
    Scene *scene = owner.GetScene();
    if (!owner.dive_.Begin(WaterCameraFx::FindMainCamera(scene), WaterCameraFx::FindWaterY(scene),
                           owner.diveParams_, owner.look_, &owner.bubbles_)) {
      // カメラが無い等。演出なしで遷移する
      Log::Print("[ResultScreenScript] dive could not start, changing scene directly");
      owner.LeaveDirectly(target_, machine);
      return;
    }
    return;
  }

  // 2) 飛び込み。暗くなりきったら遷移を要求する
  if (!owner.dive_.Update(deltaTime)) {
    owner.dive_.End(false);
    owner.LeaveDirectly(target_, machine);
    return;
  }
  if (!owner.dive_.IsDone() || requested_) return;
  requested_ = owner.RequestSceneChange(target_, SceneTransitions::kDive);
  if (!requested_ && owner.dive_.DoneTime() > kLeaveRetrySeconds) {
    Log::Print("[ResultScreenScript] scene change kept failing, giving up: " + target_);
    owner.dive_.End(/*restoreCamera=*/true);
    owner.CancelLeave(machine);
  }
}

void ResultScreenScript::DirectChoiceCommand::Execute(ResultScreenScript &owner,
                                                      StateMachine<ResultScreenScript> &machine) const {
  if (owner.RequestSceneChange(Target())) {
    machine.ChangeState<DecidedState>();
    Log::Print("[ResultScreenScript] " + std::string(Trigger()) + " -> " + Target());
  } else {
    Log::Print("[ResultScreenScript] scene change refused: " + Target());
  }
}

void ResultScreenScript::DiveChoiceCommand::Execute(ResultScreenScript &owner,
                                                    StateMachine<ResultScreenScript> &machine) const {
  if (!owner.diveParams_.enabled) {
    DirectChoiceCommand::Execute(owner, machine);
    return;
  }
  // UI を消してから飛び込む。遷移の要求は暗くなりきったあと LeavingState が出す
  machine.ChangeState<LeavingState>(Target());
  Log::Print("[ResultScreenScript] " + std::string(Trigger()) + " -> dive -> " + Target());
}

REGISTER_SCRIPT(ResultScreenScript)
