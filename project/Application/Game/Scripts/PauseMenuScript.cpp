#include "ECS/ScriptableEntity.h"
#include "ECS/ScriptRegistry.h"
#include "Common/Log/Log.h"
#include "Input/Input.h"
#include "RenderCommon.h"
#include "Engine/Render/RenderContext.h"
#include "Framework/App.h"
#include "Game/Framework/GameInput.h"
#include "Game/Framework/GameSettings.h"
#include "Game/Framework/StateMachine.h"
#include "Game/Framework/UnderwaterLook.h"
#include "Scene.h"
#include "SceneFlow.h"
#include "Common/Tween/Easing.h"

#if RC_ENABLE_IMGUI
#include "imgui/imgui.h"
#endif

#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

/// @class PauseMenuScript
/// @brief ESC（コントローラーは START）で開くポーズメニュー
/// @details
///   構成:
///     - メイン   : つづける / 設定 / キー設定 / はじめから / タイトルへ
///     - 設定     : マウス感度 / スティック感度 / 上下反転 / 全体音量 / BGM 音量 / SE 音量 / もどる
///       値は GameSettings が持ち、変更があれば設定画面を閉じるときに保存する。
///     - キー設定 : 射撃 / 前の武器 / 次の武器 / ポーズ / 決定 / 戻る / 初期設定に戻す / もどる
///       行を決定すると次に押したキー・ボタンをその操作に割り当てる（キーボード・マウスとパッドは別々に持つ）。
///       割り当ては Chaso::InputActions が持ち、ページを閉じるときに Resources/Setting/InputActions.json へ保存する。
///   設計（クラス設計の課題として導入したパターン）:
///     - State パターン: メニューの状態（閉じている / メイン / 設定 / 深海へ沈む幕 / 遷移待ち）を
///       StateMachine<PauseMenuScript> の状態クラスに分けた。以前は open_ / decided_ / sinking_ の bool と
///       enum class Page { Main, Settings } の組み合わせで分岐しており、「開いているが設定ページで、
///       沈んでいる途中」のようなあり得ない組み合わせも表現できてしまっていた。状態クラスにすると
///       同時に 1 つしか成り立たず、状態ごとの変数（沈む幕の経過時間など）もその状態の中に閉じ込められる。
///       メイン / 設定ページは PageState を共通の親にし、行数・パネル幅・見出しなどを virtual で返す。
///     - Command パターン: メインページの各項目（つづける / 設定 / はじめから / タイトルへ）を
///       MainCommand の派生クラスにした。以前は選択番号の switch で処理を書き分けていたが、
///       「項目 = 1 つのコマンドオブジェクト」にすると、項目の追加・並べ替えが配列への追加だけで済み、
///       決定処理（Execute）とラベル（Label）が項目ごとにまとまる。
///     - ポリモーフィズム: 設定ページの各行は SettingRow の派生（スライダー / ON・OFF / もどる）にし、
///       行ごとの入力処理と描画を virtual で呼び分ける。スライダーはさらに感度 / 視野角 / 音量の
///       派生で範囲と数値の書式を変える（以前は行番号の switch と if の連鎖で分けていた）。
///   止め方:
///     - 開いている間は SceneContext::gamePaused を立てる。DataDrivenScene はこれを見て
///       スクリプト・アニメーション・GameMode を deltaTime = 0 で回すので、敵も弾もレールも止まる。
///       このスクリプト自身も OnUpdate の引数は 0 になるため、ctx.deltaTime を直接読んで動く。
///     - マウスカーソルは RailShooterController が「deltaTime = 0 のフレームはロックを外す」ので、
///       メニューが開くと自然に表示され、閉じると次のフレームで再びロックされる。
///   入力（GameInput のアクション。既定の割り当て。決定・戻る・ポーズはキー設定で変えられる）:
///     - ↑↓ / W S / 十字キー / 左スティック で選択、←→ / A D で値の増減、
///       Enter / Space / A ボタンで決定、ESC / B ボタンで戻る（メインなら再開）。
///     - マウスはホバーで選択、クリックで決定。スライダーはバーをクリックした位置の値になる。
///   Result 画面（ResultScreenScript）と同じフォント・墨色のパネル・生成りの文字で揃える。
///
///   JSON（scriptDataList）で設定できる項目:
///     "fontPath"   : 使うフォント（既定は Result と同じ KiwiMaru-Medium）
///     "titleLabel" : 4 番目の項目の文字（既定 "タイトルへ"。ステージでは "セレクトへ"）
///     "menuMode"   : "pause"（既定。ESC / START で開くポーズメニュー）/
///                    "shore"（浜辺シーン Shore.json の設定・操作説明。シーンが始まって "shoreOpenDelay" 秒で
///                    ひとりでに開き、ポーズは掛けない（波は動いたまま）。項目は 操作説明 / 設定 / キー設定 / セレクトへ。
///                    「セレクトへ」と ESC は遷移表のきっかけ "back" を出す）
///     "shoreOpenDelay" : 浜辺モードで、シーンが始まってからメニューを開くまで（秒。UI のない砂浜を少し見せてから開く）
///
///   遷移は遷移表（Resources/SceneFlow.json）で決まる:
///     きっかけ "pause_retry" : 「はじめから」（遷移先を $current にするといまのシーンをやり直す）
///     きっかけ "pause_exit"  : 4 番目の項目（ステージは Select。シーンごとに行を分けられる）
///     演出が "dive" のときは、ポーズを掛けたままメニューを消し、画面を深海色へ沈めてから遷移する
///     （遷移先は深海から浮上して始まる。SceneManager の Dive 遷移は同じ深海色へ抜ける）
///     "panelColor" / "accentColor" / "dimAlpha"
class PauseMenuScript : public ScriptableEntity {
public:
  /// @brief 設定項目を JSON に書き出す
  /// @return 設定項目を格納した JSON
  nlohmann::json Serialize() override {
    return {
        {"fontPath", fontPath_},
        {"titleLabel", titleLabel_},
        {"panelColor", {panelColor_.x, panelColor_.y, panelColor_.z, panelColor_.w}},
        {"accentColor", {accentColor_.x, accentColor_.y, accentColor_.z, accentColor_.w}},
        {"dimAlpha", dimAlpha_},
        {"menuMode", shoreMode_ ? "shore" : "pause"},
        {"shoreOpenDelay", shoreOpenDelay_},
    };
  }

  /// @brief JSON から設定項目を読み込む
  /// @param j 読み込み元の JSON（存在し型が合うキーだけ反映する）
  void Deserialize(const nlohmann::json &j) override {
    // キーが存在し型が一致する場合だけ上書きするヘルパー（文字列 / RGBA 配列）
    auto readS = [&](const char *key, std::string &dst) {
      if (j.contains(key) && j[key].is_string()) dst = j[key].get<std::string>();
    };
    auto readC = [&](const char *key, RC::Vector4 &dst) {
      if (!j.contains(key)) return;
      const auto &c = j[key];
      if (c.is_array() && c.size() >= 4) {
        dst = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>(), c[3].get<float>()};
      }
    };
    readS("fontPath", fontPath_);
    readS("titleLabel", titleLabel_);
    readC("panelColor", panelColor_);
    readC("accentColor", accentColor_);
    if (j.contains("dimAlpha")) dimAlpha_ = j["dimAlpha"].get<float>();
    std::string mode = shoreMode_ ? "shore" : "pause";
    readS("menuMode", mode);
    shoreMode_ = (mode == "shore");
    if (j.contains("shoreOpenDelay") && j["shoreOpenDelay"].is_number()) {
      shoreOpenDelay_ = j["shoreOpenDelay"].get<float>();
    }
  }

  /// @brief インスペクタにメニューの状態・開閉ボタン・見た目と GameSettings の編集 UI を表示する
  void OnImGui() override;

protected:
  /// @brief 設定を読み込み、ポーズ状態を初期化してフォントを用意する
  void OnCreate() override;

  /// @brief ポーズを解除し、未保存の設定を保存してフォントを解放する
  void OnDestroy() override {
    // シーンをまたいでポーズが残らないように必ず下ろす
    if (SceneContext *ctx = GetSceneContext()) ctx->gamePaused = false;
    SaveSettingsIfDirty(false);
    SaveKeyConfigIfDirty();
    auto unload = [](int &handle) {
      if (handle >= 0) {
        RC::UnloadFont(handle);
        handle = -1;
      }
    };
    unload(headingFont_);
    unload(itemFont_);
    unload(labelFont_);
  }

  /// @brief 現在の状態（閉じている / メイン / 設定 / 沈む幕 / 遷移待ち）を 1 フレーム進める
  /// @param deltaTime ポーズ中は 0 で来るので使わない。ctx.deltaTime（実時間）で動く
  void OnUpdate(float /*deltaTime*/) override {
    SceneContext *ctx = GetSceneContext();
    if (!ctx || !ctx->isPlaying() || !ctx->input) return; // エディタで停止・一時停止中は何もしない
    const float dt = (std::max)(ctx->deltaTime, 0.0f);
    fsm_.Update(*this, dt);
  }

  /// @brief 現在の状態に応じてメニュー（または遷移前の沈む幕）を描く
  /// @note ポストプロセスの後に描く（輪郭・水中・ビネットが UI に乗らないように）
  void OnOverlayRender() override;

private:
  // ------------------------------------------------------------------
  // 状態（State パターン。定義はクラスの後ろ）
  // ------------------------------------------------------------------
  class PauseState;        ///< このメニューの状態の共通の親（描画フックを足したもの）
  class ClosedState;       ///< 閉じている（開く操作を待つ）
  class PageState;         ///< 開いているページの共通の親（メイン / 設定）
  class MainPageState;     ///< メインページ
  class SettingsPageState; ///< 設定ページ
  class KeyConfigPageState; ///< キー設定ページ
  class HowToPlayPageState; ///< 操作説明ページ（浜辺モードのみ）
  class SinkingState;     ///< 深海色へ沈めてから Dive 遷移する幕
  class DecidedState;      ///< 遷移を要求済み（フェード待ち）

  // ------------------------------------------------------------------
  // メインページの項目（Command パターン。定義はクラスの後ろ）
  // ------------------------------------------------------------------
  class MainCommand;         ///< 項目の基底
  class ResumeCommand;       ///< つづける
  class OpenSettingsCommand; ///< 設定
  class OpenKeyConfigCommand; ///< キー設定
  class OpenHowToPlayCommand; ///< 操作説明（浜辺モードのみ）
  class TransitionCommand;   ///< 遷移表のきっかけを引く項目の共通部分
  class RetryCommand;        ///< はじめから
  class ExitCommand;         ///< タイトルへ / セレクトへ

  // ------------------------------------------------------------------
  // 設定ページの行（ポリモーフィズム。定義はクラスの後ろ）
  // ------------------------------------------------------------------
  class SettingRow;     ///< 行の基底
  class SliderRow;      ///< 値をスライダーで変える行の共通部分
  class SensitivityRow; ///< 感度（x1.0 表示）
  class FovRow;         ///< 視野角（度表示）
  class VolumeRow;      ///< 音量（% 表示）
  class InvertYRow;     ///< 上下反転（ON / OFF）
  class BackRow;        ///< もどる
  class KeyBindRow;     ///< キー設定：操作 1 つの割り当て
  class ResetBindingsRow; ///< キー設定：初期設定に戻す
  class InfoRow;        ///< 操作説明：操作 1 つの説明（選んでも何も起きない）

  /// @brief 1 フレーム分のメニュー操作（HandleInput がまとめ、ページへ渡す）
  /// @details 不変条件の無い受け渡し用のデータなので struct のまま。
  struct MenuInput {
    bool confirm = false;    ///< キー・ボタンでの決定
    bool mouseClick = false; ///< このフレームに行をクリックしたか
    bool back = false;       ///< 戻る操作
    int adjust = 0;          ///< 左右の値の増減（-1 / 0 / +1。長押しリピート込み）
    bool dragSlider = false; ///< マウスでスライダーを操作中か
    float mouseX = 0.0f;     ///< マウスの X（実画面座標）
  };

  static constexpr int kMainSettingsIndex = 1;  ///< メインページで「設定」が並ぶ行（設定から戻ったときの選択位置）
  static constexpr int kMainKeyConfigIndex = 2; ///< メインページで「キー設定」が並ぶ行
  static constexpr int kSettingsFirstIndex = 0; ///< 設定ページを開いたときの選択行
  static constexpr int kMainHowToPlayIndex = 0; ///< 浜辺モードのメインページで「操作説明」が並ぶ行

  // ------------------------------------------------------------------
  // レイアウト（1280x720 基準）
  // ------------------------------------------------------------------
  static constexpr float kDesignW = 1280.0f;
  static constexpr float kDesignH = 720.0f;

  static constexpr float kHeadingPx = 40.0f;
  static constexpr float kItemPx = 26.0f;
  static constexpr float kLabelPx = 17.0f;

  static constexpr float kMainPanelW = 460.0f;
  static constexpr float kSettingsPanelW = 700.0f;
  static constexpr float kPanelPadTop = 34.0f;   ///< パネル上端から見出しまで
  /// @brief 見出しブロック（見出し ＋ 下線 ＋ 欧文）の高さ。この下から行が始まる
  /// @details 見出しの行送りはフォント px より大きい（約 1.4 倍）ので、px の合計ではなく
  ///          実際の描画高さに余裕を足した値にしておく（欧文が 1 行目に食い込まないように）
  static constexpr float kHeadingBlockH = 104.0f;
  static constexpr float kMainRowH = 56.0f;      ///< メインの行の高さ
  static constexpr float kMainRowGap = 8.0f;     ///< メインの行の間隔
  static constexpr float kSettingsRowH = 46.0f;  ///< 設定の行の高さ（行数が多いので詰める）
  static constexpr float kSettingsRowGap = 6.0f; ///< 設定の行の間隔
  static constexpr float kPanelPadBottom = 34.0f;
  static constexpr float kHintGap = 26.0f;       ///< パネル下端から操作ヒントまで
  static constexpr float kSliderW = 260.0f;      ///< スライダーの幅
  static constexpr float kValueW = 70.0f;        ///< 数値表示の幅

  static constexpr float kFadeTime = 0.15f;
  static constexpr float kBumpTime = 0.16f;
  static constexpr float kRepeatDelay = 0.35f;   ///< ←→ 長押しで値を動かし始めるまで
  static constexpr float kRepeatInterval = 0.06f; ///< 長押し中の刻み

  // 色（Result と共通の生成り／鼠）
  static constexpr RC::Vector4 kInk = {0.95f, 0.93f, 0.87f, 1.0f};
  static constexpr RC::Vector4 kMuted = {0.66f, 0.70f, 0.76f, 1.0f};
  static constexpr RC::Vector4 kDim = {0.40f, 0.44f, 0.50f, 1.0f};

  /// @brief 1280x720 基準座標を実画面座標へ変換するための倍率とオフセット
  /// @details s: 拡大率 / ox, oy: レターボックスのずれ / fs: フォント描画時の倍率（s ÷ ロード時の倍率）
  struct Layout {
    float s = 1.0f;
    float ox = 0.0f;
    float oy = 0.0f;
    float fs = 1.0f;
  };
  Layout L_;

  /// @brief 実画面の大きさを取得し、1280x720 基準のレイアウトを縦横の小さいほうの倍率で合わせる（余りは均等に空ける）
  /// @param screenW 画面の幅の出力先（取れなければ 1280）
  /// @param screenH 画面の高さの出力先（取れなければ 720）
  void BeginLayout(float &screenW, float &screenH) {
    screenW = 1280.0f;
    screenH = 720.0f;
    auto &rc = RC::GetRenderContext();
    if (rc.Ctx() && rc.Ctx()->app) {
      screenW = static_cast<float>(rc.Ctx()->app->width);
      screenH = static_cast<float>(rc.Ctx()->app->height);
    }
    L_.s = (std::min)(screenW / kDesignW, screenH / kDesignH);
    if (L_.s <= 0.0f) L_.s = 1.0f;
    L_.ox = (screenW - kDesignW * L_.s) * 0.5f;
    L_.oy = (screenH - kDesignH * L_.s) * 0.5f;
    L_.fs = (fontScale_ > 0.0f) ? (L_.s / fontScale_) : 1.0f;
  }

  /// @brief 基準座標の X を実画面座標へ変換する
  /// @param x 基準座標の X
  /// @return 実画面座標
  float X(float x) const { return L_.ox + x * L_.s; }
  /// @brief 基準座標の Y を実画面座標へ変換する
  /// @param y 基準座標の Y
  /// @return 実画面座標
  float Y(float y) const { return L_.oy + y * L_.s; }
  /// @brief 基準座標系の長さを実画面の長さへ変換する
  /// @param v 基準座標系での長さ
  /// @return 実画面での長さ
  float S(float v) const { return v * L_.s; }

  /// @brief 色の不透明度に係数を掛けた色を作る
  /// @param c 元の色
  /// @param a 不透明度に掛ける係数
  /// @return 不透明度を変えた色
  static RC::Vector4 WithAlpha(const RC::Vector4 &c, float a) { return {c.x, c.y, c.z, c.w * a}; }

  /// @brief 表示中のページ（状態クラス）を取得する
  /// @return 現在のページ。ページを開いていなければメインページ相当の既定値
  const PageState &CurrentPage() const;
  /// @brief メニュー（いずれかのページ）を開いているか
  /// @return メイン / 設定ページの状態なら true
  bool IsOpen() const;

  /// @brief 表示中のページの行数・パネル幅・行の高さ・行の間隔を返す（ページ側の virtual に委ねる）
  /// @return 行数 / 基準座標での長さ
  int RowCount() const;
  float PanelW() const;
  float RowH() const;
  float RowGap() const;
  /// @brief パネルの高さ（上余白 + 見出し + 全行 + 行間 + 下余白）を求める
  /// @return パネルの高さ（基準座標）
  float PanelH() const {
    return kPanelPadTop + kHeadingBlockH + RowCount() * RowH() +
           (RowCount() - 1) * RowGap() + kPanelPadBottom;
  }
  /// @brief 画面中央に置いたパネルの左端を求める
  /// @return パネルの左端（基準座標）
  float PanelL() const { return (kDesignW - PanelW()) * 0.5f; }
  /// @brief 画面中央に置いたパネルの上端を求める
  /// @return パネルの上端（基準座標）
  float PanelT() const { return (kDesignH - PanelH()) * 0.5f; }
  /// @brief i 行目の上端を求める
  /// @param i 行番号（0 始まり）
  /// @return 行の上端（基準座標）
  float RowTop(int i) const {
    return PanelT() + kPanelPadTop + kHeadingBlockH + i * (RowH() + RowGap());
  }
  /// @brief 行の矩形（基準座標）を求める。パネルの左右から 28px 内側に置く
  /// @param i 行番号（0 始まり）
  /// @param l 左端の出力先
  /// @param t 上端の出力先
  /// @param r 右端の出力先
  /// @param b 下端の出力先
  void RowRect(int i, float &l, float &t, float &r, float &b) const {
    const float inset = 28.0f;
    l = PanelL() + inset;
    r = PanelL() + PanelW() - inset;
    t = RowTop(i);
    b = t + RowH();
  }
  /// @brief 設定行のスライダー矩形（基準座標）を求める。右寄せで数値の左に置く
  /// @param i 行番号（0 始まり）
  /// @param l 左端の出力先
  /// @param t 上端の出力先
  /// @param r 右端の出力先
  /// @param b 下端の出力先（高さ 12px で行の縦中央にそろえる）
  void SliderRect(int i, float &l, float &t, float &r, float &b) const {
    float rl, rt, rr, rb;
    RowRect(i, rl, rt, rr, rb);
    r = rr - 18.0f - kValueW;
    l = r - kSliderW;
    t = (rt + rb) * 0.5f - 6.0f;
    b = t + 12.0f;
  }

  // ------------------------------------------------------------------
  // 開閉
  // ------------------------------------------------------------------

  /// @brief いまの入力を取得する
  /// @return 入力。シーンコンテキストが無ければ nullptr
  Input *CurrentInput() const {
    SceneContext *ctx = GetSceneContext();
    return ctx ? ctx->input : nullptr;
  }

  /// @brief メニューを開く操作（アクション "Pause"。既定は ESC / START ボタン）がこのフレームにあったか調べる
  /// @param in 入力
  /// @return 開く操作があれば true
  bool WantOpen(Input *in) const {
    (void)in;
    // 浜辺モードは操作では開かない（ClosedState がシーン開始から shoreOpenDelay_ 秒で開く）
    if (shoreMode_) return false;
    if (!GameInput::Triggered(GameInput::kPause)) return false;
#if RC_ENABLE_IMGUI
    // インスペクタのテキスト入力中のキー（ESC など）はエディタのもの。パッドで押したときだけ開く
    if (ImGui::GetIO().WantCaptureKeyboard) return GameInput::PressedOnPad(GameInput::kPause);
#endif
    return true;
  }

  /// @brief いまメニューを開いてよいか調べる
  /// @return 開いてよければ true
  /// @note 決着がついた後（ゲームオーバー／終点到達）はリザルトへ流れるので開かない
  bool CanOpen() const {
    Scene *scene = GetScene();
    if (!scene) return true;
    for (auto &e : scene->GetEntities()) {
      if (!e) continue;
      if (e->GetTagInt("game_over", 0) == 1) return false;
      if (e->HasTag("rail_finished")) return false;
    }
    return true;
  }

  /// @brief ポーズを掛けてメニューを開く（メインページの先頭を選んだ状態から始める）
  /// @param machine 遷移を予約するステートマシン
  void OpenMenu(StateMachine<PauseMenuScript> &machine);
  /// @brief 未保存の設定を保存し、ポーズを解除してメニューを閉じる
  /// @param machine 遷移を予約するステートマシン
  void CloseMenu(StateMachine<PauseMenuScript> &machine);
  /// @brief 設定・キー設定ページから、変更を保存してメインページへ戻る
  /// @param machine 遷移を予約するステートマシン
  /// @param index 戻ったときに選ぶメインページの行（既定は「設定」）
  void BackToMain(StateMachine<PauseMenuScript> &machine, int index = kMainSettingsIndex);

  /// @brief キーの割り当てに変更があれば Resources/Setting/InputActions.json へ保存する
  void SaveKeyConfigIfDirty() {
    if (!keyConfigDirty_) return;
    keyConfigDirty_ = false;
    if (!GameInput::Save()) Log::Print("[PauseMenuScript] failed to save InputActions.json");
  }

  /// @brief キー設定の「入力を待っている」状態を始める
  /// @param row 割り当てを変える行
  /// @details 決定したフレームの押下を拾わないよう、実際に拾うのは次のフレームから（KeyConfigPageState）
  void BeginCapture(int row) { capturingRow_ = row; }

  /// @brief 設定に変更があれば GameSettings を保存する
  /// @param force true なら変更が無くても保存する
  void SaveSettingsIfDirty(bool force) {
    if (!settingsDirty_ && !force) return;
    settingsDirty_ = false;
    GameSettings::Get().Save();
  }

  /// @brief 設定が変わったことを記録する（音量は即反映。感度は RailShooterController が毎フレーム読む）
  void MarkSettingsChanged() {
    GameSettings::Get().Apply();
    settingsDirty_ = true;
  }

  // ------------------------------------------------------------------
  // 入力
  // ------------------------------------------------------------------

  /// @brief キーボード・ゲームパッド・マウスの入力をまとめ、表示中のページの処理へ渡す
  /// @param in 入力
  /// @param page 表示中のページ
  /// @param machine 遷移を予約するステートマシン
  void HandleInput(Input *in, PageState &page, StateMachine<PauseMenuScript> &machine);

  static constexpr const char *kTriggerRetry = "pause_retry"; ///< 遷移表のきっかけ名（はじめから）
  static constexpr const char *kTriggerExit = "pause_exit";   ///< 遷移表のきっかけ名（タイトル／セレクトへ）
  static constexpr const char *kTriggerShoreBack = "back";  ///< 遷移表のきっかけ名（浜辺シーンからセレクトへ）

  /// @brief 遷移表に従って遷移を要求する（演出が dive なら沈む幕へ、それ以外は遷移待ちへ）
  /// @param trigger 遷移表のきっかけ名（kTriggerRetry / kTriggerExit）
  /// @param machine 遷移を予約するステートマシン
  void RequestChange(const char *trigger, StateMachine<PauseMenuScript> &machine);

  // ------------------------------------------------------------------
  // 「はじめから」「セレクトへ」：画面を深海色へ沈めてから Dive 遷移
  // ------------------------------------------------------------------

  static constexpr float kSinkTime = 0.7f;          ///< 深海色に沈みきるまで（秒。実時間）
  static constexpr float kSinkRetrySeconds = 1.5f;  ///< 遷移が通らないときに素の遷移へ倒すまで
  static constexpr int kSinkBubbles = 18;            ///< 沈むあいだに昇る泡の数

  /// @brief 深海色の幕と、下から昇って消えていく泡を描く
  /// @param sinkTime 沈み始めてからの経過時間（秒）
  void DrawSink(float sinkTime) const {
    // 幕の濃さ a は経過時間を smoothstep で 0〜1 にしたもの
    float screenW = 1280.0f, screenH = 720.0f;
    auto &rc = RC::GetRenderContext();
    if (rc.Ctx() && rc.Ctx()->app) {
      screenW = static_cast<float>(rc.Ctx()->app->width);
      screenH = static_cast<float>(rc.Ctx()->app->height);
    }
    const float u = std::clamp(sinkTime / kSinkTime, 0.0f, 1.0f);
    const float a = u * u * (3.0f - 2.0f * u);

    // 先に水の青を薄く掛けてから深海色へ（いきなり黒くならず「水に沈む」色の流れにする）
    const RC::Vector4 &abyss = UnderwaterLook::kAbyssScreenColor;
    const RC::Vector4 &shallow = UnderwaterLook::kDefaultShallowFog;
    const float blueA = 0.55f * std::sin(3.14159265f * (std::min)(u * 1.4f, 1.0f));
    RC::DrawBox({0.0f, 0.0f}, {screenW, screenH}, {shallow.x, shallow.y, shallow.z, blueA * (1.0f - a)});
    RC::DrawBox({0.0f, 0.0f}, {screenW, screenH}, {abyss.x, abyss.y, abyss.z, a});

    // 泡：位置と速さは番号から決める（毎回同じ並び）。幕が濃くなるにつれて薄れる
    const float s = (std::min)(screenW / kDesignW, screenH / kDesignH);
    const float bubbleA = 0.7f * (1.0f - a * a);
    if (bubbleA <= 0.01f) return;
    for (int i = 0; i < kSinkBubbles; ++i) {
      // fract(sin(i * a) * b) 型の擬似乱数で、番号ごとに固定の横位置・大きさ・速さを作る
      const float h1 = std::fmod(std::sin(i * 12.9898f) * 43758.5453f, 1.0f);
      const float h2 = std::fmod(std::sin(i * 78.233f) * 12345.678f, 1.0f);
      const float fx = std::fabs(h1);
      const float fs = std::fabs(h2);
      const float speed = 0.9f + fs * 0.9f;                 // 画面の高さ / 秒
      const float r = (3.0f + fs * 7.0f) * s;
      const float y = screenH * (1.05f + fx * 0.25f) - screenH * speed * sinkTime;
      const float x = screenW * (0.08f + fx * 0.84f) + std::sin(sinkTime * 6.0f + i) * 6.0f * s;
      if (y < -r) continue;
      RC::DrawCircle({x, y}, r, {0.85f, 0.95f, 1.0f, bubbleA * 0.35f});
      RC::DrawCircle({x, y}, r, {0.9f, 0.97f, 1.0f, bubbleA}, kWire);
    }
  }

  /// @brief 選択中の行を変え、変わったときは明滅をやり直して枠を弾ませる
  /// @param index 新しく選ぶ行番号
  void SetSelected(int index) {
    if (index == selected_) return;
    selected_ = index;
    pulse_ = 0.0f;
    bump_ = 1.0f;
  }

  /// @brief マウス座標（ゲーム解像度基準）がどの行の上か調べる
  /// @param mx マウスの X（実画面座標）
  /// @param my マウスの Y（実画面座標）
  /// @return 行番号。どの行の上でもなければ -1
  int HitTestRows(float mx, float my) const {
    for (int i = 0; i < RowCount(); ++i) {
      float l, t, r, b;
      RowRect(i, l, t, r, b);
      if (mx >= X(l) && mx <= X(r) && my >= Y(t) && my <= Y(b)) return i;
    }
    return -1;
  }

  // ------------------------------------------------------------------
  // 描画（ページ共通の部品。ページ固有の行は各ページ・各行クラスが描く）
  // ------------------------------------------------------------------

  /// @brief 右下に 2px ずらした影付きで文字列を描く
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
    const float sh = S(2.0f);
    RC::DrawString(font, text, {x + sh, y + sh}, {0.0f, 0.02f, 0.05f, 0.55f * color.w}, sc, align);
    RC::DrawString(font, text, {x, y}, color, sc, align);
  }
  /// @brief フォントの 1 行の高さ（実画面座標）を求める
  /// @param font フォントハンドル
  /// @param scale ロードしたサイズに対する倍率
  /// @return 行の高さ。フォントが無ければ kItemPx を基準にした概算値
  float LineH(int font, float scale = 1.0f) const {
    return (font >= 0) ? RC::GetFontLineHeight(font, scale * L_.fs) : S(kItemPx) * scale;
  }

  /// @brief 墨のパネル（二重の細枠 ＋ 四隅の飾り。Result と同じ意匠）を描く
  /// @param a 開くアニメーションの不透明度（0〜1）
  void DrawPanel(float a) const {
    const RC::Vector2 tl = {X(PanelL()), Y(PanelT())};
    const RC::Vector2 br = {X(PanelL() + PanelW()), Y(PanelT() + PanelH())};
    RC::DrawBox(tl, br, WithAlpha(panelColor_, a));

    const float in1 = S(6.0f), in2 = S(11.0f);
    RC::DrawBox({tl.x + in1, tl.y + in1}, {br.x - in1, br.y - in1}, WithAlpha(kInk, 0.55f * a), kWire);
    RC::DrawBox({tl.x + in2, tl.y + in2}, {br.x - in2, br.y - in2}, WithAlpha(kInk, 0.22f * a), kWire);

    // 四隅に長さ arm の L 字をアクセント色で描く（左上・右上・左下・右下の順に 2 本ずつ）
    const float arm = S(22.0f);
    const float th = (std::max)(1.0f, S(2.0f));
    const RC::Vector4 c = WithAlpha(accentColor_, 0.9f * a);
    const float o = S(2.0f);
    RC::DrawLine({tl.x + o, tl.y + o}, {tl.x + o + arm, tl.y + o}, c, th);
    RC::DrawLine({tl.x + o, tl.y + o}, {tl.x + o, tl.y + o + arm}, c, th);
    RC::DrawLine({br.x - o, tl.y + o}, {br.x - o - arm, tl.y + o}, c, th);
    RC::DrawLine({br.x - o, tl.y + o}, {br.x - o, tl.y + o + arm}, c, th);
    RC::DrawLine({tl.x + o, br.y - o}, {tl.x + o + arm, br.y - o}, c, th);
    RC::DrawLine({tl.x + o, br.y - o}, {tl.x + o, br.y - o - arm}, c, th);
    RC::DrawLine({br.x - o, br.y - o}, {br.x - o - arm, br.y - o}, c, th);
    RC::DrawLine({br.x - o, br.y - o}, {br.x - o, br.y - o - arm}, c, th);
  }

  /// @brief パネル上部に見出し・下線・欧文の副題を描く
  /// @param title 見出しの文字列
  /// @param sub 副題（欧文）の文字列
  /// @param a 開くアニメーションの不透明度（0〜1）
  void DrawHeading(const char *title, const char *sub, float a) const {
    const float cx = X(kDesignW * 0.5f);
    const float top = Y(PanelT() + kPanelPadTop);
    Text(headingFont_, title, cx, top, WithAlpha(kInk, a), TextAlign::Center);
    // 見出し下の細い線と欧文
    const float lineY = top + LineH(headingFont_) + S(4.0f);
    const float halfW = S(PanelW() * 0.5f - 40.0f);
    RC::DrawLine({cx - halfW, lineY}, {cx + halfW, lineY}, WithAlpha(accentColor_, 0.7f * a), (std::max)(1.0f, S(1.5f)));
    Text(labelFont_, sub, cx, lineY + S(6.0f), WithAlpha(kMuted, 0.9f * a), TextAlign::Center, 0.85f);
  }

  /// @brief 行の下地（選択中は金の枠と左の三角マーカー）
  /// @param i 行番号（0 始まり）
  /// @param a 開くアニメーションの不透明度（0〜1）
  void DrawRowFrame(int i, float a) const {
    float l, t, r, b;
    RowRect(i, l, t, r, b);
    const RC::Vector2 tl = {X(l), Y(t)};
    const RC::Vector2 br = {X(r), Y(b)};
    const bool sel = (i == selected_);
    if (sel) {
      const float pulse = 0.5f + 0.5f * std::sin(pulse_ * 4.0f);
      const float borderA = (0.55f + 0.45f * pulse) * a;
      RC::DrawBox(tl, br, WithAlpha(accentColor_, 0.12f * a));
      RC::DrawBox(tl, br, WithAlpha(accentColor_, borderA), kWire);
      // 選び直した直後は外側にもう 1 本枠を出し、bump_ の減衰に合わせて縮める
      const float o = S(3.0f) * bump_;
      if (o > 0.0f) {
        RC::DrawBox({tl.x - o, tl.y - o}, {br.x + o, br.y + o}, WithAlpha(accentColor_, 0.5f * borderA), kWire);
      }
      const float mx = tl.x - S(16.0f);
      const float my = (tl.y + br.y) * 0.5f;
      RC::DrawTriangle({mx - S(6.0f), my - S(7.0f)}, {mx + S(6.0f), my}, {mx - S(6.0f), my + S(7.0f)},
                       WithAlpha(accentColor_, a));
    } else {
      RC::DrawBox(tl, br, WithAlpha(kDim, 0.35f * a), kWire);
    }
  }

  /// @brief パネルの下に操作説明を描く
  /// @param text 表示する操作説明
  /// @param a 開くアニメーションの不透明度（0〜1）
  void DrawHint(const char *text, float a) const {
    Text(labelFont_, text, X(kDesignW * 0.5f), Y(PanelT() + PanelH() + kHintGap),
         WithAlpha(kMuted, 0.8f * a), TextAlign::Center, 0.9f);
  }

  // ------------------------------------------------------------------
  // 設定項目（JSON / インスペクタ）
  // ------------------------------------------------------------------
  std::string fontPath_ = "Resources/fonts/Kiwi_Maru/KiwiMaru-Medium.ttf";
  std::string titleLabel_ = "タイトルへ";
  RC::Vector4 panelColor_ = {0.06f, 0.08f, 0.11f, 0.92f}; ///< 墨
  RC::Vector4 accentColor_ = {0.88f, 0.74f, 0.38f, 1.0f}; ///< 金（選択中）
  float dimAlpha_ = 0.55f;                                ///< 背後の画面を沈める濃さ
  bool shoreMode_ = false; ///< 浜辺シーンの設定・操作説明として使うか（JSON の "menuMode"）
  float shoreOpenDelay_ = 0.2f;  ///< 浜辺モードで、シーン開始からメニューを開くまで（秒）
  float shoreWait_ = 0.0f;       ///< 浜辺モードで、シーン開始からの経過時間（秒）

  // ------------------------------------------------------------------
  // 状態
  // ------------------------------------------------------------------
  int headingFont_ = -1;
  int itemFont_ = -1;
  int labelFont_ = -1;
  float fontScale_ = 1.0f;

  StateMachine<PauseMenuScript> fsm_; ///< 閉じている / メイン / 設定 / 沈む幕 / 遷移待ち
  bool settingsDirty_ = false;
  bool keyConfigDirty_ = false; ///< キーの割り当てを変えたが、まだ保存していないか
  int capturingRow_ = -1;       ///< キー設定で入力を待っている行（待っていなければ -1）
  /// @brief 割り当てた直後、そのキーを離すまでメニュー操作を止めるための記録
  /// @details 「決定」を K に割り当てた直後は K が押されたままなので、次のフレームに決定として扱われ、
  ///          また入力待ちに戻ってしまう。離すまで待つ。
  const char *waitReleaseAction_ = nullptr;
  Chaso::InputBinding waitReleaseBinding_{};
  bool draggingSlider_ = false;
  int selected_ = 0;

  float anim_ = 0.0f;
  float pulse_ = 0.0f;
  float bump_ = 0.0f;
  int holdDir_ = 0;
  float holdTime_ = 0.0f;
  float lastMouseX_ = 0.0f;
  float lastMouseY_ = 0.0f;
};

// ======================================================================
// 状態クラス（State パターン）
// ======================================================================

/// @brief ポーズメニューの状態の共通の親
/// @details IState に「この状態での描画」を足したもの。OnOverlayRender は
///          現在の状態の Draw を呼ぶだけになり、状態ごとの描き分けは派生クラスが持つ。
class PauseMenuScript::PauseState : public IState<PauseMenuScript> {
public:
  /// @brief この状態での描画（既定は何も描かない）
  /// @param owner ポーズメニュー本体（レイアウトを更新するので非 const）
  virtual void Draw(PauseMenuScript &owner) const { (void)owner; }
};

/// @brief メニューを閉じている状態。ESC / START で開く
class PauseMenuScript::ClosedState : public PauseMenuScript::PauseState {
public:
  /// @brief 開く操作があり、開いてよい場面ならメインページへ
  void OnUpdate(PauseMenuScript &owner, StateMachine<PauseMenuScript> &machine, float deltaTime) override;
  /// @brief 状態名
  /// @return "Closed"
  const char *Name() const override { return "Closed"; }
};

/// @brief 遷移を要求済みの状態。フェードを待つだけで入力は受け付けない（二重に遷移させない）
class PauseMenuScript::DecidedState : public PauseMenuScript::PauseState {
public:
  /// @brief 何もしない
  void OnUpdate(PauseMenuScript &owner, StateMachine<PauseMenuScript> &machine, float deltaTime) override {
    (void)owner;
    (void)machine;
    (void)deltaTime;
  }
  /// @brief 状態名
  /// @return "Decided"
  const char *Name() const override { return "Decided"; }
};

/// @brief 画面を深海色へ沈めてから Dive 遷移を要求する状態
/// @details 経過時間・遷移先・要求済みフラグはこの状態だけが使うので、本体ではなくここに持つ。
class PauseMenuScript::SinkingState : public PauseMenuScript::PauseState {
public:
  /// @brief 遷移先を受け取って沈み始める
  /// @param target 沈みきった後に遷移するシーン名
  explicit SinkingState(std::string target) : target_(std::move(target)) {}
  /// @brief 沈む幕の時間を進め、沈みきったら Dive 遷移を要求する
  /// @note Dive 遷移が kSinkRetrySeconds 通らなければ素の遷移を試し、それも通らなければメニューへ戻す
  void OnUpdate(PauseMenuScript &owner, StateMachine<PauseMenuScript> &machine, float deltaTime) override;
  /// @brief 深海色の幕と泡を描く
  /// @param owner ポーズメニュー本体
  void Draw(PauseMenuScript &owner) const override { owner.DrawSink(time_); }
  /// @brief 状態名
  /// @return "Sinking"
  const char *Name() const override { return "Sinking"; }

private:
  std::string target_;     ///< 沈みきった後に遷移するシーン名
  float time_ = 0.0f;      ///< 沈み始めてからの経過時間（秒。実時間）
  bool requested_ = false; ///< 遷移の要求が通ったか
};

// ======================================================================
// メインページの項目（Command パターン）
// ======================================================================

/// @brief メインページの 1 項目を表すコマンド
/// @details 「決定されたら何をするか」をオブジェクトにしたもの。メインページは選択中のコマンドの
///          Execute を呼ぶだけで、項目ごとの処理の違いを知らない（switch が不要になる）。
class PauseMenuScript::MainCommand {
public:
  virtual ~MainCommand() = default;
  /// @brief 項目に表示する文字列
  /// @param owner ポーズメニュー本体（JSON で文言を変えられる項目が読む）
  /// @return 表示する文字列
  virtual std::string Label(const PauseMenuScript &owner) const = 0;
  /// @brief 項目が決定されたときの処理
  /// @param owner ポーズメニュー本体
  /// @param machine 遷移を予約するステートマシン
  virtual void Execute(PauseMenuScript &owner, StateMachine<PauseMenuScript> &machine) = 0;
};

/// @brief 「つづける」: メニューを閉じて再開する
class PauseMenuScript::ResumeCommand : public PauseMenuScript::MainCommand {
public:
  /// @brief 表示文字列
  /// @param owner ポーズメニュー本体
  /// @return "つづける"（浜辺モードでは "セレクトへ"）
  std::string Label(const PauseMenuScript &owner) const override {
    return owner.shoreMode_ ? "セレクトへ" : "つづける";
  }
  /// @brief メニューを閉じる（浜辺モードではセレクトへ戻る遷移を出す）
  void Execute(PauseMenuScript &owner, StateMachine<PauseMenuScript> &machine) override;
};

/// @brief 「設定」: 設定ページを開く
class PauseMenuScript::OpenSettingsCommand : public PauseMenuScript::MainCommand {
public:
  /// @brief 表示文字列
  /// @return "設定"
  std::string Label(const PauseMenuScript &owner) const override {
    (void)owner;
    return "設定";
  }
  /// @brief 設定ページへ切り替える
  void Execute(PauseMenuScript &owner, StateMachine<PauseMenuScript> &machine) override;
};

/// @brief 「キー設定」: キー設定ページを開く
class PauseMenuScript::OpenKeyConfigCommand : public PauseMenuScript::MainCommand {
public:
  /// @brief 表示文字列
  /// @return "キー設定"
  std::string Label(const PauseMenuScript &owner) const override {
    (void)owner;
    return "キー設定";
  }
  /// @brief キー設定ページへ切り替える
  void Execute(PauseMenuScript &owner, StateMachine<PauseMenuScript> &machine) override;
};

/// @brief 「操作説明」: 操作説明ページを開く（浜辺モードのみ）
class PauseMenuScript::OpenHowToPlayCommand : public PauseMenuScript::MainCommand {
public:
  /// @brief 表示文字列
  /// @return "操作説明"
  std::string Label(const PauseMenuScript &owner) const override {
    (void)owner;
    return "操作説明";
  }
  /// @brief 操作説明ページへ切り替える
  void Execute(PauseMenuScript &owner, StateMachine<PauseMenuScript> &machine) override;
};

/// @brief 遷移表のきっかけを引いてシーンを切り替える項目の共通部分
class PauseMenuScript::TransitionCommand : public PauseMenuScript::MainCommand {
public:
  /// @brief きっかけ名を受け取る
  /// @param trigger 遷移表のきっかけ名
  explicit TransitionCommand(const char *trigger) : trigger_(trigger) {}
  /// @brief 遷移表に従って遷移を要求する
  void Execute(PauseMenuScript &owner, StateMachine<PauseMenuScript> &machine) override;

private:
  const char *trigger_; ///< 遷移表のきっかけ名
};

/// @brief 「はじめから」: いまのシーンをやり直す（きっかけ "pause_retry"）
class PauseMenuScript::RetryCommand : public PauseMenuScript::TransitionCommand {
public:
  RetryCommand() : TransitionCommand(kTriggerRetry) {}
  /// @brief 表示文字列
  /// @return "はじめから"
  std::string Label(const PauseMenuScript &owner) const override {
    (void)owner;
    return "はじめから";
  }
};

/// @brief 「タイトルへ」/「セレクトへ」: メニューから抜ける（きっかけ "pause_exit"。文言は JSON の titleLabel）
class PauseMenuScript::ExitCommand : public PauseMenuScript::TransitionCommand {
public:
  ExitCommand() : TransitionCommand(kTriggerExit) {}
  /// @brief 表示文字列
  /// @param owner ポーズメニュー本体
  /// @return JSON で設定された titleLabel
  std::string Label(const PauseMenuScript &owner) const override { return owner.titleLabel_; }
};

// ======================================================================
// 設定ページの行（ポリモーフィズム）
// ======================================================================

/// @brief 設定ページの 1 行
/// @details 行ごとに「入力をどう解釈するか」「右側に何を描くか」が違うので、それを virtual にした。
///          Draw は行の枠など共通部分を描いてから DrawContents を呼ぶ Template Method。
class PauseMenuScript::SettingRow {
public:
  /// @brief 行の描画に使う、共通部分で求めた値
  /// @details 不変条件の無い受け渡し用のデータなので struct のまま。
  struct DrawContext {
    int index = 0;         ///< 行番号
    float l = 0.0f;        ///< 行の左端（基準座標）
    float t = 0.0f;        ///< 行の上端（基準座標）
    float r = 0.0f;        ///< 行の右端（基準座標）
    float b = 0.0f;        ///< 行の下端（基準座標）
    bool selected = false; ///< 選択中か
    RC::Vector4 ink{};     ///< 項目名の色（選択中は生成り、それ以外は鼠）
    float textY = 0.0f;    ///< 文字の上端（実画面座標。行の縦中央にそろえた値）
    float a = 1.0f;        ///< 開くアニメーションの不透明度
  };

  /// @brief 項目名を受け取る
  /// @param label 項目名
  explicit SettingRow(const char *label) : label_(label) {}
  virtual ~SettingRow() = default;

  /// @brief スライダーで値を変える行か（マウスのドラッグを受け付けるか）
  /// @return スライダーなら true
  virtual bool IsSlider() const { return false; }

  /// @brief この行が選ばれているときの入力を処理する
  /// @param owner ポーズメニュー本体
  /// @param machine 遷移を予約するステートマシン
  /// @param input このフレームの操作
  /// @return 設定値を変えたら true（呼び出し側が反映と保存予約を行う）
  virtual bool Handle(PauseMenuScript &owner, StateMachine<PauseMenuScript> &machine,
                      const MenuInput &input) = 0;

  /// @brief 行を描く（枠 → 行ごとの中身）
  /// @param owner ポーズメニュー本体
  /// @param index 行番号
  /// @param a 開くアニメーションの不透明度（0〜1）
  void Draw(const PauseMenuScript &owner, int index, float a) const {
    owner.DrawRowFrame(index, a);
    DrawContext c;
    c.index = index;
    owner.RowRect(index, c.l, c.t, c.r, c.b);
    c.selected = (index == owner.selected_);
    c.ink = WithAlpha(c.selected ? kInk : kMuted, a);
    const float lh = owner.LineH(owner.itemFont_);
    c.textY = owner.Y((c.t + c.b) * 0.5f) - lh * 0.5f;
    c.a = a;
    DrawContents(owner, c);
  }

protected:
  /// @brief 行の中身（項目名と値）を描く
  /// @param owner ポーズメニュー本体
  /// @param c 共通部分で求めた値
  virtual void DrawContents(const PauseMenuScript &owner, const DrawContext &c) const = 0;

  /// @brief 項目名を行の左に描く
  /// @param owner ポーズメニュー本体
  /// @param c 共通部分で求めた値
  void DrawLabelLeft(const PauseMenuScript &owner, const DrawContext &c) const {
    owner.Text(owner.itemFont_, label_, owner.X(c.l + 22.0f), c.textY, c.ink, TextAlign::Left);
  }

  /// @brief 項目名を取得する
  /// @return 項目名
  const char *Label() const { return label_; }

private:
  const char *label_; ///< 項目名
};

/// @brief GameSettings の値をスライダーで変える行
/// @details どの値を触るかはメンバ関数ポインタ（Getter / Setter）で受け取り、
///          範囲と刻みはコンストラクタで、数値の書式は派生クラスの FormatValue で決める。
class PauseMenuScript::SliderRow : public PauseMenuScript::SettingRow {
public:
  using Getter = float (GameSettings::*)() const; ///< 値を読む GameSettings のメンバ関数
  using Setter = void (GameSettings::*)(float);   ///< 値を書く GameSettings のメンバ関数

  /// @brief 触る値と範囲を受け取る
  /// @param label 項目名
  /// @param getter 値を読むメンバ関数
  /// @param setter 値を書くメンバ関数
  /// @param minV 最小値
  /// @param maxV 最大値
  /// @param step 1 刻みの量
  SliderRow(const char *label, Getter getter, Setter setter, float minV, float maxV, float step)
      : SettingRow(label), getter_(getter), setter_(setter), min_(minV), max_(maxV), step_(step) {}

  /// @brief スライダーの行
  /// @return 常に true
  bool IsSlider() const override { return true; }

  /// @brief ←→ で 1 刻み動かし、ドラッグ中はマウス X のバー上の位置を値にする
  bool Handle(PauseMenuScript &owner, StateMachine<PauseMenuScript> &machine, const MenuInput &input) override;

protected:
  /// @brief 右端に出す数値の文字列を作る
  /// @param v 値
  /// @return 表示用の文字列
  virtual std::string FormatValue(float v) const = 0;

  /// @brief 項目名・スライダー・数値を描く
  void DrawContents(const PauseMenuScript &owner, const DrawContext &c) const override;

private:
  /// @brief いまの値
  /// @return GameSettings の値
  float Value() const { return (GameSettings::Get().*getter_)(); }
  /// @brief 値を書き込む
  /// @param v 新しい値
  void SetValue(float v) const { (GameSettings::Get().*setter_)(v); }

  Getter getter_; ///< 値を読むメンバ関数
  Setter setter_; ///< 値を書くメンバ関数
  float min_;     ///< 最小値
  float max_;     ///< 最大値
  float step_;    ///< 1 刻みの量
};

/// @brief 感度の行（倍率を x1.0 の形で出す）
class PauseMenuScript::SensitivityRow : public PauseMenuScript::SliderRow {
public:
  /// @brief 触る感度を受け取る（範囲と刻みは感度共通）
  /// @param label 項目名
  /// @param getter 値を読むメンバ関数
  /// @param setter 値を書くメンバ関数
  SensitivityRow(const char *label, Getter getter, Setter setter)
      : SliderRow(label, getter, setter, GameSettings::kSensitivityMin, GameSettings::kSensitivityMax,
                  GameSettings::kSensitivityStep) {}

protected:
  /// @brief "x1.0" の形にする
  std::string FormatValue(float v) const override {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "x%.1f", v);
    return buf;
  }
};

/// @brief 視野角の行（度で出す）
class PauseMenuScript::FovRow : public PauseMenuScript::SliderRow {
public:
  FovRow()
      : SliderRow("視野角", &GameSettings::FovDeg, &GameSettings::SetFovDeg, GameSettings::kFovMin,
                  GameSettings::kFovMax, GameSettings::kFovStep) {}

protected:
  /// @brief "26度" の形にする
  std::string FormatValue(float v) const override {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d度", static_cast<int>(std::round(v)));
    return buf;
  }
};

/// @brief 音量の行（% で出す）
class PauseMenuScript::VolumeRow : public PauseMenuScript::SliderRow {
public:
  /// @brief 触る音量を受け取る（範囲 0〜1 と刻みは音量共通）
  /// @param label 項目名
  /// @param getter 値を読むメンバ関数
  /// @param setter 値を書くメンバ関数
  VolumeRow(const char *label, Getter getter, Setter setter)
      : SliderRow(label, getter, setter, 0.0f, 1.0f, GameSettings::kVolumeStep) {}

protected:
  /// @brief "100%" の形にする
  std::string FormatValue(float v) const override {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(std::round(v * 100.0f)));
    return buf;
  }
};

/// @brief 上下反転の行（決定・左右・クリックで ON / OFF を切り替える）
class PauseMenuScript::InvertYRow : public PauseMenuScript::SettingRow {
public:
  InvertYRow() : SettingRow("上下反転") {}

  /// @brief 決定・クリック・左右のどれかで切り替える
  bool Handle(PauseMenuScript &owner, StateMachine<PauseMenuScript> &machine, const MenuInput &input) override {
    (void)owner;
    (void)machine;
    if (!(input.confirm || input.mouseClick || input.adjust != 0)) return false;
    GameSettings &gs = GameSettings::Get();
    gs.SetInvertY(!gs.InvertY());
    return true;
  }

protected:
  /// @brief 項目名と ON / OFF を描く
  void DrawContents(const PauseMenuScript &owner, const DrawContext &c) const override {
    DrawLabelLeft(owner, c);
    const bool on = GameSettings::Get().InvertY();
    owner.Text(owner.itemFont_, on ? "ON" : "OFF", owner.X(c.r - 18.0f), c.textY,
               WithAlpha(on ? owner.accentColor_ : kMuted, c.a), TextAlign::Right);
  }
};

/// @brief 「もどる」の行（決定・クリックでメインページへ）
class PauseMenuScript::BackRow : public PauseMenuScript::SettingRow {
public:
  /// @brief 戻ったときに選ぶメインページの行を受け取る
  /// @param returnIndex メインページの行（既定は「設定」）
  explicit BackRow(int returnIndex = kMainSettingsIndex) : SettingRow("もどる"), returnIndex_(returnIndex) {}

  /// @brief 決定・クリックでメインページへ戻る
  bool Handle(PauseMenuScript &owner, StateMachine<PauseMenuScript> &machine, const MenuInput &input) override {
    if (input.confirm || input.mouseClick) owner.BackToMain(machine, returnIndex_);
    return false;
  }

protected:
  /// @brief 項目名を行の中央に描く
  void DrawContents(const PauseMenuScript &owner, const DrawContext &c) const override {
    owner.Text(owner.itemFont_, Label(), owner.X((c.l + c.r) * 0.5f), c.textY, c.ink, TextAlign::Center);
  }

private:
  int returnIndex_; ///< 戻ったときに選ぶメインページの行
};

/// @brief キー設定の 1 行（操作 1 つ。左に操作名、右にキーボード・マウスとパッドの割り当て）
/// @details 決定・クリックで「入力待ち」になり、次に押したキー・ボタンをその操作に割り当てる
///          （KeyConfigPageState が拾う）。値はこの行ではなく Chaso::InputActions が持つ。
class PauseMenuScript::KeyBindRow : public PauseMenuScript::SettingRow {
public:
  /// @brief 対象の操作を受け取る
  /// @param action アクション名（GameInput::kFire など）
  /// @param label 画面に出す操作名
  KeyBindRow(const char *action, const char *label) : SettingRow(label), action_(action) {}

  /// @brief アクション名
  /// @return GameInput のアクション名
  const char *Action() const { return action_; }

  /// @brief 決定・クリックで入力待ちを始める
  bool Handle(PauseMenuScript &owner, StateMachine<PauseMenuScript> &machine, const MenuInput &input) override {
    (void)machine;
    if (input.confirm || input.mouseClick) owner.BeginCapture(owner.selected_);
    return false;
  }

protected:
  /// @brief 操作名と、キーボード・マウス／パッドの割り当て（入力待ちなら案内）を描く
  void DrawContents(const PauseMenuScript &owner, const DrawContext &c) const override {
    DrawLabelLeft(owner, c);
    const float lh = owner.LineH(owner.labelFont_);
    const float y = owner.Y((c.t + c.b) * 0.5f) - lh * 0.5f;
    if (owner.capturingRow_ == c.index) {
      // 点滅させて「押してください」を目立たせる
      const float blink = 0.6f + 0.4f * std::sin(owner.pulse_ * 8.0f);
      owner.Text(owner.labelFont_, "キー・ボタンを押してください（ESC / BACK で取り消し）", owner.X(c.r - 18.0f), y,
                 WithAlpha(owner.accentColor_, blink * c.a), TextAlign::Right);
      return;
    }
    // 中：キーボード・マウス　右：パッド
    owner.Text(owner.labelFont_, GameInput::BindingsText(action_, true), owner.X(c.l + kKeyColumnX), y, c.ink,
               TextAlign::Left);
    owner.Text(owner.labelFont_, GameInput::BindingsText(action_, false), owner.X(c.r - 18.0f), y,
               WithAlpha(kMuted, c.a), TextAlign::Right);
  }

private:
  static constexpr float kKeyColumnX = 250.0f; ///< キーボード・マウスの割り当てを書き始める位置（行の左端から）
  const char *action_;                         ///< 対象のアクション名
};

/// @brief 「初期設定に戻す」の行（決定・クリックですべての割り当てを既定に戻す）
class PauseMenuScript::ResetBindingsRow : public PauseMenuScript::SettingRow {
public:
  ResetBindingsRow() : SettingRow("初期設定に戻す") {}

  /// @brief 決定・クリックで既定の割り当てへ戻し、保存を予約する
  bool Handle(PauseMenuScript &owner, StateMachine<PauseMenuScript> &machine, const MenuInput &input) override {
    (void)machine;
    if (!(input.confirm || input.mouseClick)) return false;
    GameInput::Actions().ResetAllToDefault();
    owner.keyConfigDirty_ = true;
    owner.bump_ = 1.0f;
    return false;
  }

protected:
  /// @brief 項目名を行の中央に描く
  void DrawContents(const PauseMenuScript &owner, const DrawContext &c) const override {
    owner.Text(owner.itemFont_, Label(), owner.X((c.l + c.r) * 0.5f), c.textY, c.ink, TextAlign::Center);
  }
};

/// @brief 操作説明の 1 行（左に操作名、中にキーボード・マウス、右にパッド）。選んでも何も起きない
/// @details 割り当てはキー設定で変えられるので、文字列は固定せず描くたびに GameInput から引く。
///          アクションを持たない行（照準など）は固定の文字列を出す。
class PauseMenuScript::InfoRow : public PauseMenuScript::SettingRow {
public:
  /// @brief アクションの割り当てを出す行
  /// @param label 画面に出す操作名
  /// @param action 1 つ目のアクション名
  /// @param action2 2 つ目のアクション名（"前の武器 / 次の武器" のように並べるとき。無ければ nullptr）
  InfoRow(const char *label, const char *action, const char *action2 = nullptr)
      : SettingRow(label), action_(action), action2_(action2) {}
  /// @brief 固定の文字列を出す行
  /// @param label 画面に出す操作名
  /// @param keyboardText キーボード・マウス側の文字列
  /// @param padText パッド側の文字列
  /// @param fixed 固定文字列の行であることを示す目印（値は使わない）
  InfoRow(const char *label, const char *keyboardText, const char *padText, bool fixed)
      : SettingRow(label), keyboardText_(keyboardText), padText_(padText) {
    (void)fixed;
  }

  /// @brief 何もしない（説明を読むだけの行）
  bool Handle(PauseMenuScript &owner, StateMachine<PauseMenuScript> &machine, const MenuInput &input) override {
    (void)owner;
    (void)machine;
    (void)input;
    return false;
  }

protected:
  /// @brief 操作名と、キーボード・マウス／パッドの操作を描く
  void DrawContents(const PauseMenuScript &owner, const DrawContext &c) const override {
    DrawLabelLeft(owner, c);
    const float lh = owner.LineH(owner.labelFont_);
    const float y = owner.Y((c.t + c.b) * 0.5f) - lh * 0.5f;
    owner.Text(owner.labelFont_, BindText(true), owner.X(c.l + kKeyColumnX), y, c.ink, TextAlign::Left);
    owner.Text(owner.labelFont_, BindText(false), owner.X(c.r - 18.0f), y, WithAlpha(kMuted, c.a), TextAlign::Right);
  }

private:
  /// @brief キーボード・マウス側（またはパッド側）に出す文字列を作る
  /// @param keyboardMouse true ならキーボード・マウス側
  /// @return 表示する文字列
  std::string BindText(bool keyboardMouse) const {
    if (!action_) return keyboardMouse ? keyboardText_ : padText_;
    std::string s = GameInput::BindingsText(action_, keyboardMouse);
    if (action2_) s += "  /  " + GameInput::BindingsText(action2_, keyboardMouse);
    return s;
  }

  static constexpr float kKeyColumnX = 250.0f; ///< キーボード・マウスの操作を書き始める位置（行の左端から）
  const char *action_ = nullptr;               ///< 割り当てを引くアクション（固定文字列の行は nullptr）
  const char *action2_ = nullptr;              ///< 並べて出す 2 つ目のアクション
  const char *keyboardText_ = "";              ///< 固定文字列の行：キーボード・マウス側
  const char *padText_ = "";                   ///< 固定文字列の行：パッド側
};

// ======================================================================
// ページ（State パターン。メイン / 設定の共通の親）
// ======================================================================

/// @brief メニューを開いている状態（ページ）の共通の親
/// @details 開くアニメーション・入力の集約・パネルの描画はページ共通なのでここに置き、
///          行数・パネル幅・見出し・行の描画・決定処理を virtual で派生ページに任せる（Template Method）。
class PauseMenuScript::PageState : public PauseMenuScript::PauseState {
public:
  /// @brief 開くアニメーションを進め、入力をまとめてページへ渡す
  void OnUpdate(PauseMenuScript &owner, StateMachine<PauseMenuScript> &machine, float deltaTime) override {
    owner.pulse_ += deltaTime;
    owner.anim_ = (std::min)(owner.anim_ + deltaTime / kFadeTime, 1.0f);
    if (owner.bump_ > 0.0f) owner.bump_ = (std::max)(0.0f, owner.bump_ - deltaTime / kBumpTime);
    if (Input *in = owner.CurrentInput()) owner.HandleInput(in, *this, machine);
  }

  /// @brief 背後を沈め、パネル・見出し・行・操作説明を描く（行の中身は DrawRows）
  /// @param owner ポーズメニュー本体
  void Draw(PauseMenuScript &owner) const override {
    float screenW = 1280.0f, screenH = 720.0f;
    owner.BeginLayout(screenW, screenH);
    const float a = Chaso::Ease::Evaluate(Chaso::EaseType::OutCubic, owner.anim_);
    // 背後の画面を沈める
    RC::DrawBox({0.0f, 0.0f}, {screenW, screenH}, {0.02f, 0.03f, 0.06f, owner.dimAlpha_ * a});
    owner.DrawPanel(a);
    owner.DrawHeading(Heading(), SubHeading(), a);
    DrawRows(owner, a);
    owner.DrawHint(Hint(), a);
  }

  /// @brief 行数
  /// @return このページの行数
  virtual int RowCount() const = 0;
  /// @brief パネルの幅（基準座標）
  /// @return パネル幅
  virtual float PanelW() const = 0;
  /// @brief 行の高さ（基準座標）
  /// @return 行の高さ
  virtual float RowH() const = 0;
  /// @brief 行の間隔（基準座標）
  /// @return 行の間隔
  virtual float RowGap() const = 0;
  /// @brief 指定した行がスライダーか（マウスのドラッグを受け付けるか）
  /// @param row 行番号
  /// @return スライダーの行なら true（既定は false）
  virtual bool IsSliderRow(int row) const {
    (void)row;
    return false;
  }
  /// @brief このページでの決定・戻る・値の増減を処理する
  /// @param owner ポーズメニュー本体
  /// @param machine 遷移を予約するステートマシン
  /// @param input このフレームの操作
  virtual void HandlePageInput(PauseMenuScript &owner, StateMachine<PauseMenuScript> &machine,
                               const MenuInput &input) = 0;

protected:
  /// @brief 見出し
  /// @return 見出しの文字列
  virtual const char *Heading() const = 0;
  /// @brief 見出しの下の欧文
  /// @return 欧文の文字列
  virtual const char *SubHeading() const = 0;
  /// @brief パネルの下の操作説明
  /// @return 操作説明の文字列
  virtual const char *Hint() const = 0;
  /// @brief 各行を描く
  /// @param owner ポーズメニュー本体
  /// @param a 開くアニメーションの不透明度（0〜1）
  virtual void DrawRows(const PauseMenuScript &owner, float a) const = 0;
};

/// @brief メインページ（つづける / 設定 / キー設定 / はじめから / タイトルへ）
/// @details 浜辺モードでは 操作説明 / 設定 / キー設定 / セレクトへ
class PauseMenuScript::MainPageState : public PauseMenuScript::PageState {
public:
  /// @brief 項目のコマンドを並べる（並び順 = 表示順）
  /// @param shore 浜辺モードの並びにするか
  explicit MainPageState(bool shore = false);

  int RowCount() const override { return static_cast<int>(commands_.size()); }
  float PanelW() const override { return kMainPanelW; }
  float RowH() const override { return kMainRowH; }
  float RowGap() const override { return kMainRowGap; }

  /// @brief 戻るで再開、決定で選択中のコマンドを実行する
  void HandlePageInput(PauseMenuScript &owner, StateMachine<PauseMenuScript> &machine,
                       const MenuInput &input) override;

  /// @brief 状態名
  /// @return "Main"
  const char *Name() const override { return "Main"; }

protected:
  const char *Heading() const override { return shore_ ? "浜辺" : "一時停止"; }
  const char *SubHeading() const override { return shore_ ? "BEACH" : "PAUSE"; }
  const char *Hint() const override {
    return shore_ ? "↑↓ 選択　　Enter / Space 決定　　ESC セレクトへ" : "↑↓ 選択　　Enter / Space 決定　　ESC 再開";
  }
  /// @brief 各項目のラベルを行の中央に描く
  void DrawRows(const PauseMenuScript &owner, float a) const override;

private:
  std::vector<std::unique_ptr<MainCommand>> commands_; ///< 項目（Command）
  bool shore_ = false;                                 ///< 浜辺モードの並びか
};

/// @brief 設定ページ（感度・反転・視野角・音量・もどる）
class PauseMenuScript::SettingsPageState : public PauseMenuScript::PageState {
public:
  /// @brief 行を並べる（並び順 = 表示順）
  SettingsPageState();

  int RowCount() const override { return static_cast<int>(rows_.size()); }
  float PanelW() const override { return kSettingsPanelW; }
  float RowH() const override { return kSettingsRowH; }
  float RowGap() const override { return kSettingsRowGap; }
  /// @brief 指定した行がスライダーか
  /// @param row 行番号
  /// @return スライダーの行なら true
  bool IsSliderRow(int row) const override {
    return row >= 0 && row < RowCount() && rows_[static_cast<size_t>(row)]->IsSlider();
  }

  /// @brief 戻るでメインへ、それ以外は選択中の行に処理させ、値が変われば反映して保存を予約する
  void HandlePageInput(PauseMenuScript &owner, StateMachine<PauseMenuScript> &machine,
                       const MenuInput &input) override;

  /// @brief 状態名
  /// @return "Settings"
  const char *Name() const override { return "Settings"; }

protected:
  const char *Heading() const override { return "設定"; }
  const char *SubHeading() const override { return "SETTINGS"; }
  const char *Hint() const override { return "↑↓ 選択　　←→ 変更　　ESC もどる"; }
  /// @brief 各行に自分を描かせる
  void DrawRows(const PauseMenuScript &owner, float a) const override {
    for (int i = 0; i < RowCount(); ++i) rows_[static_cast<size_t>(i)]->Draw(owner, i, a);
  }

private:
  std::vector<std::unique_ptr<SettingRow>> rows_; ///< 行（ポリモーフィズム）
};

/// @brief キー設定ページ（GameInput::Rebindable の操作 / 初期設定に戻す / もどる）
/// @details 行の入力待ち（capturingRow_）の間はメニュー操作を止め、次に押されたキー・ボタンを拾って割り当てる。
class PauseMenuScript::KeyConfigPageState : public PauseMenuScript::PageState {
public:
  /// @brief 行を並べる（並び順 = 表示順）
  KeyConfigPageState();

  int RowCount() const override { return static_cast<int>(rows_.size()); }
  float PanelW() const override { return kSettingsPanelW; }
  float RowH() const override { return kSettingsRowH; }
  float RowGap() const override { return kSettingsRowGap; }

  /// @brief 入力待ちなら次の入力を拾い、それ以外は通常のメニュー操作をする
  void OnUpdate(PauseMenuScript &owner, StateMachine<PauseMenuScript> &machine, float deltaTime) override;

  /// @brief 戻るでメインへ、それ以外は選択中の行に処理させる
  void HandlePageInput(PauseMenuScript &owner, StateMachine<PauseMenuScript> &machine,
                       const MenuInput &input) override;

  /// @brief 状態名
  /// @return "KeyConfig"
  const char *Name() const override { return "KeyConfig"; }

protected:
  const char *Heading() const override { return "キー設定"; }
  const char *SubHeading() const override { return "CONTROLS"; }
  const char *Hint() const override { return "↑↓ 選択　　決定 で割り当てを変更　　ESC もどる"; }
  /// @brief 各行に自分を描かせる
  void DrawRows(const PauseMenuScript &owner, float a) const override {
    for (int i = 0; i < RowCount(); ++i) rows_[static_cast<size_t>(i)]->Draw(owner, i, a);
  }

private:
  std::vector<std::unique_ptr<SettingRow>> rows_; ///< 行（KeyBindRow ×操作数 / ResetBindingsRow / BackRow）
};

/// @brief 操作説明ページ（照準・射撃・武器・ポーズ・決定と戻る / もどる）。浜辺モードから開く
class PauseMenuScript::HowToPlayPageState : public PauseMenuScript::PageState {
public:
  /// @brief 行を並べる（並び順 = 表示順）
  HowToPlayPageState();

  int RowCount() const override { return static_cast<int>(rows_.size()); }
  float PanelW() const override { return kSettingsPanelW; }
  float RowH() const override { return kSettingsRowH; }
  float RowGap() const override { return kSettingsRowGap; }

  /// @brief 戻るでメインへ、それ以外は選択中の行に処理させる（説明の行は何もしない）
  void HandlePageInput(PauseMenuScript &owner, StateMachine<PauseMenuScript> &machine,
                       const MenuInput &input) override;

  /// @brief 状態名
  /// @return "HowToPlay"
  const char *Name() const override { return "HowToPlay"; }

protected:
  const char *Heading() const override { return "操作説明"; }
  const char *SubHeading() const override { return "HOW TO PLAY　　キーボード・マウス ／ パッド"; }
  const char *Hint() const override { return "割り当てはキー設定で変えられます　　ESC もどる"; }
  /// @brief 各行に自分を描かせる
  void DrawRows(const PauseMenuScript &owner, float a) const override {
    for (int i = 0; i < RowCount(); ++i) rows_[static_cast<size_t>(i)]->Draw(owner, i, a);
  }

private:
  std::vector<std::unique_ptr<SettingRow>> rows_; ///< 行（InfoRow ×操作数 / BackRow）
};

// ======================================================================
// 定義（状態・コマンド・行がすべて完全型になってから書く）
// ======================================================================

void PauseMenuScript::OnImGui() {
#if RC_ENABLE_IMGUI
  ImGui::Text("Pause: %s  state=%s  selected=%d", IsOpen() ? "OPEN" : "closed", fsm_.CurrentName(),
              selected_);
  if (ImGui::Button(IsOpen() ? "Close Menu" : "Open Menu")) {
    // 沈む幕・遷移待ちの最中は開き直さない（遷移の途中でメニューが重なるのを避ける）
    if (IsOpen()) {
      CloseMenu(fsm_);
    } else if (!fsm_.HasState() || fsm_.IsIn<ClosedState>()) {
      OpenMenu(fsm_);
    }
    // インスペクタは Update の外なので、予約した遷移をここで確定させる
    fsm_.ApplyPending(*this);
  }
  ImGui::ColorEdit4("Panel Color", &panelColor_.x);
  ImGui::ColorEdit4("Accent Color", &accentColor_.x);
  ImGui::SliderFloat("Dim Alpha", &dimAlpha_, 0.0f, 1.0f);
  ImGui::Separator();
  // GameSettings を編集し、変更があれば即反映して保存待ちにする（値は private なので写して書き戻す）
  ImGui::TextUnformatted("GameSettings (shared, saved to Resources/Setting/GameSettings.json)");
  GameSettings &gs = GameSettings::Get();
  auto slider = [&gs](const char *label, float (GameSettings::*get)() const, void (GameSettings::*set)(float),
                      float minV, float maxV) {
    float v = (gs.*get)();
    if (!ImGui::SliderFloat(label, &v, minV, maxV)) return false;
    (gs.*set)(v);
    return true;
  };
  bool changed = false;
  changed |= slider("Mouse Sensitivity", &GameSettings::MouseSensitivity, &GameSettings::SetMouseSensitivity,
                    GameSettings::kSensitivityMin, GameSettings::kSensitivityMax);
  changed |= slider("Stick Sensitivity", &GameSettings::ControllerSensitivity,
                    &GameSettings::SetControllerSensitivity, GameSettings::kSensitivityMin,
                    GameSettings::kSensitivityMax);
  bool invertY = gs.InvertY();
  if (ImGui::Checkbox("Invert Y", &invertY)) {
    gs.SetInvertY(invertY);
    changed = true;
  }
  changed |= slider("FOV (vertical deg)", &GameSettings::FovDeg, &GameSettings::SetFovDeg, GameSettings::kFovMin,
                    GameSettings::kFovMax);
  changed |= slider("Master Volume", &GameSettings::MasterVolume, &GameSettings::SetMasterVolume, 0.0f, 1.0f);
  changed |= slider("BGM Volume", &GameSettings::BgmVolume, &GameSettings::SetBgmVolume, 0.0f, 1.0f);
  changed |= slider("SE Volume", &GameSettings::SeVolume, &GameSettings::SetSeVolume, 0.0f, 1.0f);
  if (changed) MarkSettingsChanged();
  if (ImGui::Button("Save Settings")) SaveSettingsIfDirty(true);
#endif
}

void PauseMenuScript::OnCreate() {
  GameSettings::Get().EnsureLoaded();
  if (SceneContext *ctx = GetSceneContext()) ctx->gamePaused = false;
  settingsDirty_ = false;
  shoreWait_ = 0.0f;
  // 閉じた状態から始める
  fsm_.ChangeState<ClosedState>();
  fsm_.ApplyPending(*this);

  // フォントは基準解像度（1280x720）のサイズ × 実解像度のスケールでロードする（Result と同じ）
  float w = 1280.0f, h = 720.0f;
  if (SceneContext *ctx = GetSceneContext()) {
    if (ctx->app && ctx->app->width > 0 && ctx->app->height > 0) {
      w = static_cast<float>(ctx->app->width);
      h = static_cast<float>(ctx->app->height);
    }
  }
  fontScale_ = (std::min)(w / kDesignW, h / kDesignH);
  if (fontScale_ <= 0.0f) fontScale_ = 1.0f;

  auto load = [&](float designPx, uint32_t atlas) {
    const int handle = RC::LoadFont(fontPath_, std::round(designPx * fontScale_), atlas);
    if (handle < 0) Log::Print("[PauseMenuScript] failed to load font: " + fontPath_);
    return handle;
  };
  headingFont_ = load(kHeadingPx, 1024);
  itemFont_ = load(kItemPx, 1024);
  labelFont_ = load(kLabelPx, 1024);

  if (Input *in = Input::GetInstance()) in->GetGameMousePosition(lastMouseX_, lastMouseY_);
}

void PauseMenuScript::OnOverlayRender() {
  if (const PauseState *state = fsm_.As<PauseState>()) state->Draw(*this);
}

const PauseMenuScript::PageState &PauseMenuScript::CurrentPage() const {
  if (const PageState *page = fsm_.As<PageState>()) return *page;
  // ページを開いていないときにレイアウトを聞かれた場合の既定値（メインページと同じ寸法）
  static const MainPageState kFallback;
  return kFallback;
}

bool PauseMenuScript::IsOpen() const { return fsm_.IsIn<PageState>(); }

int PauseMenuScript::RowCount() const { return CurrentPage().RowCount(); }
float PauseMenuScript::PanelW() const { return CurrentPage().PanelW(); }
float PauseMenuScript::RowH() const { return CurrentPage().RowH(); }
float PauseMenuScript::RowGap() const { return CurrentPage().RowGap(); }

void PauseMenuScript::OpenMenu(StateMachine<PauseMenuScript> &machine) {
  // 浜辺モードはシーンそのものがメニューなので、ポーズを掛けずに波や波紋を動かしたままにする
  if (!shoreMode_) {
    if (SceneContext *ctx = GetSceneContext()) ctx->gamePaused = true;
  }
  selected_ = 0;
  anim_ = 0.0f;
  pulse_ = 0.0f;
  bump_ = 0.0f;
  holdDir_ = 0;
  holdTime_ = 0.0f;
  if (Input *in = Input::GetInstance()) in->GetGameMousePosition(lastMouseX_, lastMouseY_);
  machine.ChangeState<MainPageState>(shoreMode_);
  Log::Print("[PauseMenuScript] pause");
}

void PauseMenuScript::CloseMenu(StateMachine<PauseMenuScript> &machine) {
  SaveSettingsIfDirty(false);
  if (SceneContext *ctx = GetSceneContext()) ctx->gamePaused = false;
  machine.ChangeState<ClosedState>();
  Log::Print("[PauseMenuScript] resume");
}

void PauseMenuScript::BackToMain(StateMachine<PauseMenuScript> &machine, int index) {
  SaveSettingsIfDirty(false);
  SaveKeyConfigIfDirty();
  capturingRow_ = -1;
  waitReleaseAction_ = nullptr;
  selected_ = index;
  bump_ = 1.0f;
  machine.ChangeState<MainPageState>(shoreMode_);
}

void PauseMenuScript::HandleInput(Input *in, PageState &page, StateMachine<PauseMenuScript> &machine) {
  // --- 決定 / 戻る（アクション。割り当ては GameInput::DefineDefaults / キーコンフィグ） ---
  const bool confirm = GameInput::Triggered(GameInput::kConfirm);
  const bool back = GameInput::Triggered(GameInput::kBack) || GameInput::Triggered(GameInput::kPause);

  // --- 上下・左右（キー・十字キー・スティックの片側がそれぞれのアクションに割り当ててある。
  //     スティックも倒した瞬間だけ IsTriggered になるので、前フレームとの比較は要らない） ---
  int move = GameInput::TriggeredAxis(GameInput::kMenuUp, GameInput::kMenuDown);
  int adjust = GameInput::TriggeredAxis(GameInput::kMenuLeft, GameInput::kMenuRight);

  // 長押しリピート（キー・十字キー・スティックのどれでも）。
  // 同じ向きを kRepeatDelay 秒押し続けたら、以後 kRepeatInterval 秒ごとに 1 刻み動かす
  const int heldDir = GameInput::PressedAxis(GameInput::kMenuLeft, GameInput::kMenuRight);
  if (heldDir != 0 && heldDir == holdDir_) {
    SceneContext *ctx = GetSceneContext();
    holdTime_ += ctx ? (std::max)(ctx->deltaTime, 0.0f) : 0.0f;
    if (holdTime_ >= kRepeatDelay) {
      holdTime_ -= kRepeatInterval;
      if (adjust == 0) adjust = heldDir;
    }
  } else {
    holdDir_ = heldDir;
    holdTime_ = 0.0f;
  }

  // --- マウス：動かしたときだけホバーで選択を移す（キー操作を邪魔しない） ---
  float mx = lastMouseX_, my = lastMouseY_;
  in->GetGameMousePosition(mx, my);
  const bool mouseMoved = (std::fabs(mx - lastMouseX_) > 0.5f || std::fabs(my - lastMouseY_) > 0.5f);
  lastMouseX_ = mx;
  lastMouseY_ = my;
  const int hovered = HitTestRows(mx, my);
  if (mouseMoved && hovered >= 0 && hovered != selected_) {
    SetSelected(hovered);
    move = 0;
  }
  bool mouseClick = false;
  if (in->IsMouseTrigger(0) && hovered >= 0) {
    SetSelected(hovered);
    mouseClick = true;
  }
  // スライダーはドラッグ中も追従させる（スライダーの行があるのは設定ページだけ）
  const bool mouseDragSlider = in->IsMousePressed(0) && page.IsSliderRow(selected_) &&
                               (mouseClick || draggingSlider_);

  // 上下移動は端で反対側へ回り込む
  if (move != 0) {
    const int n = page.RowCount();
    SetSelected((selected_ + move + n) % n);
  }

  // 決定・戻る・値の増減の解釈はページごとに違うので、ページ（状態クラス）に任せる
  MenuInput input;
  input.confirm = confirm;
  input.mouseClick = mouseClick;
  input.back = back;
  input.adjust = adjust;
  input.dragSlider = mouseDragSlider;
  input.mouseX = mx;
  page.HandlePageInput(*this, machine, input);
  if (!in->IsMousePressed(0)) draggingSlider_ = false;
}

void PauseMenuScript::RequestChange(const char *trigger, StateMachine<PauseMenuScript> &machine) {
  // ポーズは掛けたまま抜ける（フェード中に敵が動かないように）。
  // gamePaused は OnExit → OnDestroy で必ず下ろされる。

  // 演出が "dive" なら、メニューを消して画面を深海色へ沈めてから遷移する（SinkingState）
  std::string target;
  std::string transition;
  if (LookupTransition(trigger, target, transition) && !target.empty() &&
      transition == SceneTransitions::kDive) {
    machine.ChangeState<SinkingState>(target);
    Log::Print(std::string("[PauseMenuScript] ") + trigger + " -> sink -> " + target);
    return;
  }

  if (RequestTransition(trigger)) {
    // メニューはポストプロセスの後に描いているのでディゾルブに巻き込まれない。
    // 出しっぱなしだと画面が黒くなってもメニューだけ残るので、遷移待ち（何も描かない状態）へ移る。
    machine.ChangeState<DecidedState>();
    Log::Print(std::string("[PauseMenuScript] ") + trigger);
  } else {
    Log::Print(std::string("[PauseMenuScript] scene change refused: ") + trigger);
  }
}

// ---- 状態 ----

void PauseMenuScript::ClosedState::OnUpdate(PauseMenuScript &owner, StateMachine<PauseMenuScript> &machine,
                                            float deltaTime) {
  // 浜辺モード：シーンが始まって shoreOpenDelay_ 秒たったら、ひとりでに開く
  if (owner.shoreMode_) {
    owner.shoreWait_ += (std::max)(deltaTime, 0.0f);
    if (owner.shoreWait_ >= owner.shoreOpenDelay_) owner.OpenMenu(machine);
    return;
  }
  Input *in = owner.CurrentInput();
  if (in && owner.WantOpen(in) && owner.CanOpen()) owner.OpenMenu(machine);
}

void PauseMenuScript::SinkingState::OnUpdate(PauseMenuScript &owner, StateMachine<PauseMenuScript> &machine,
                                             float deltaTime) {
  time_ += deltaTime;
  if (time_ < kSinkTime || requested_) return;
  // SceneManager の Dive 遷移は同じ深海色へ抜けるので、継ぎ目なく切り替わる。
  // 遷移先は深海から浮上して始まる（ステージ: DeepRiseIntroScript ／ セレクト: StageSelectScript）
  requested_ = owner.RequestSceneChange(target_, SceneTransitions::kDive);
  if (!requested_ && time_ > kSinkTime + kSinkRetrySeconds) {
    Log::Print("[PauseMenuScript] dive transition refused, falling back: " + target_);
    requested_ = owner.RequestSceneChange(target_);
    if (!requested_) {
      // 諦めてメニューへ戻す（画面が沈んだまま操作できなくなるのを避ける）
      owner.anim_ = 1.0f;
      machine.ChangeState<MainPageState>(owner.shoreMode_);
    }
  }
}

// ---- メインページ ----

PauseMenuScript::MainPageState::MainPageState(bool shore) : shore_(shore) {
  if (shore_) {
    // 浜辺：シーンを抜ける項目は「セレクトへ」だけ
    commands_.push_back(std::make_unique<OpenHowToPlayCommand>()); // kMainHowToPlayIndex
    commands_.push_back(std::make_unique<OpenSettingsCommand>());  // kMainSettingsIndex
    commands_.push_back(std::make_unique<OpenKeyConfigCommand>()); // kMainKeyConfigIndex
    commands_.push_back(std::make_unique<ResumeCommand>());        // "セレクトへ"
    return;
  }
  commands_.push_back(std::make_unique<ResumeCommand>());
  commands_.push_back(std::make_unique<OpenSettingsCommand>()); // kMainSettingsIndex
  commands_.push_back(std::make_unique<OpenKeyConfigCommand>()); // kMainKeyConfigIndex
  commands_.push_back(std::make_unique<RetryCommand>());
  commands_.push_back(std::make_unique<ExitCommand>());
}

void PauseMenuScript::MainPageState::HandlePageInput(PauseMenuScript &owner,
                                                     StateMachine<PauseMenuScript> &machine,
                                                     const MenuInput &input) {
  if (input.back) {
    // 浜辺モードには閉じた状態が無いので、ESC はセレクトへ戻る
    if (owner.shoreMode_) {
      owner.SaveSettingsIfDirty(false);
      owner.RequestChange(kTriggerShoreBack, machine);
    } else {
      owner.CloseMenu(machine);
    }
    return;
  }
  if (!(input.confirm || input.mouseClick)) return;
  const int i = owner.selected_;
  if (i < 0 || i >= RowCount()) return;
  commands_[static_cast<size_t>(i)]->Execute(owner, machine);
}

void PauseMenuScript::MainPageState::DrawRows(const PauseMenuScript &owner, float a) const {
  for (int i = 0; i < RowCount(); ++i) {
    owner.DrawRowFrame(i, a);
    float l, t, r, b;
    owner.RowRect(i, l, t, r, b);
    const bool sel = (i == owner.selected_);
    const float lh = owner.LineH(owner.itemFont_);
    owner.Text(owner.itemFont_, commands_[static_cast<size_t>(i)]->Label(owner), owner.X((l + r) * 0.5f),
               owner.Y((t + b) * 0.5f) - lh * 0.5f, WithAlpha(sel ? kInk : kMuted, a), TextAlign::Center);
  }
}

// ---- 設定ページ ----

PauseMenuScript::SettingsPageState::SettingsPageState() {
  rows_.push_back(std::make_unique<SensitivityRow>("マウス感度", &GameSettings::MouseSensitivity,
                                                   &GameSettings::SetMouseSensitivity));
  rows_.push_back(std::make_unique<SensitivityRow>("スティック感度", &GameSettings::ControllerSensitivity,
                                                   &GameSettings::SetControllerSensitivity));
  rows_.push_back(std::make_unique<InvertYRow>());
  rows_.push_back(std::make_unique<FovRow>());
  rows_.push_back(std::make_unique<VolumeRow>("全体音量", &GameSettings::MasterVolume, &GameSettings::SetMasterVolume));
  rows_.push_back(std::make_unique<VolumeRow>("BGM 音量", &GameSettings::BgmVolume, &GameSettings::SetBgmVolume));
  rows_.push_back(std::make_unique<VolumeRow>("SE 音量", &GameSettings::SeVolume, &GameSettings::SetSeVolume));
  rows_.push_back(std::make_unique<BackRow>());
}

void PauseMenuScript::SettingsPageState::HandlePageInput(PauseMenuScript &owner,
                                                         StateMachine<PauseMenuScript> &machine,
                                                         const MenuInput &input) {
  // 戻る: 変更を保存してメインページの「設定」へ戻る
  if (input.back) {
    owner.BackToMain(machine);
    return;
  }
  const int i = owner.selected_;
  if (i < 0 || i >= RowCount()) return;
  if (rows_[static_cast<size_t>(i)]->Handle(owner, machine, input)) owner.MarkSettingsChanged();
}

// ---- キー設定ページ ----

PauseMenuScript::KeyConfigPageState::KeyConfigPageState() {
  for (const auto &a : GameInput::Rebindable()) rows_.push_back(std::make_unique<KeyBindRow>(a.action, a.label));
  rows_.push_back(std::make_unique<ResetBindingsRow>());
  rows_.push_back(std::make_unique<BackRow>(kMainKeyConfigIndex));
}

void PauseMenuScript::KeyConfigPageState::OnUpdate(PauseMenuScript &owner, StateMachine<PauseMenuScript> &machine,
                                                   float deltaTime) {
  // 割り当てた直後：そのキー・ボタンを離すまではメニュー操作をしない（アニメーションだけ進める）
  if (owner.waitReleaseAction_) {
    const Chaso::InputBinding held = owner.waitReleaseBinding_;
    const bool stillHeld = GameInput::PressedIf(
        owner.waitReleaseAction_, [&held](const Chaso::InputBinding &b) { return b == held; });
    if (stillHeld) {
      owner.pulse_ += deltaTime;
      return;
    }
    owner.waitReleaseAction_ = nullptr;
  }

  const int row = owner.capturingRow_;
  if (row < 0 || row >= RowCount()) {
    PageState::OnUpdate(owner, machine, deltaTime);
    return;
  }

  // 入力待ち：ESC / パッドの BACK で取り消し、それ以外に押されたキー・マウスボタン・パッドのボタン／トリガーを割り当てる
  owner.pulse_ += deltaTime;
  Input *in = owner.CurrentInput();
  if (in && (in->IsKeyTrigger(DIK_ESCAPE) || in->IsXInputButtonTrigger(XINPUT_GAMEPAD_BACK))) {
    owner.capturingRow_ = -1;
    return;
  }
  // 先頭から GameInput::Rebindable() の順に KeyBindRow が並んでいる（コンストラクタ）
  const auto &list = GameInput::Rebindable();
  if (row >= static_cast<int>(list.size())) {
    owner.capturingRow_ = -1;
    return;
  }
  Chaso::InputBinding binding;
  if (!GameInput::Actions().CaptureNextBinding(binding, /*includeAxes=*/true)) return;
  // マウスを割り当てられない操作では、クリック・ホイールは無視して待ち続ける
  if (GameInput::IsMouse(binding) && !list[static_cast<size_t>(row)].allowMouse) return;
  const char *action = list[static_cast<size_t>(row)].action;
  GameInput::Rebind(action, binding);
  owner.keyConfigDirty_ = true;
  owner.capturingRow_ = -1;
  owner.waitReleaseAction_ = action;
  owner.waitReleaseBinding_ = binding;
  owner.bump_ = 1.0f;
  Log::Print(std::string("[PauseMenuScript] rebind ") + action + " -> " + binding.ToString());
}

void PauseMenuScript::KeyConfigPageState::HandlePageInput(PauseMenuScript &owner,
                                                          StateMachine<PauseMenuScript> &machine,
                                                          const MenuInput &input) {
  if (input.back) {
    owner.BackToMain(machine, kMainKeyConfigIndex);
    return;
  }
  const int i = owner.selected_;
  if (i < 0 || i >= RowCount()) return;
  rows_[static_cast<size_t>(i)]->Handle(owner, machine, input);
}

// ---- 操作説明ページ ----

PauseMenuScript::HowToPlayPageState::HowToPlayPageState() {
  rows_.push_back(std::make_unique<InfoRow>("照準", "マウス", "右スティック", true));
  rows_.push_back(std::make_unique<InfoRow>("射撃", GameInput::kFire));
  rows_.push_back(std::make_unique<InfoRow>("武器の切り替え", GameInput::kWeaponPrev, GameInput::kWeaponNext));
  rows_.push_back(std::make_unique<InfoRow>("ポーズ", GameInput::kPause));
  rows_.push_back(std::make_unique<InfoRow>("決定", GameInput::kConfirm));
  rows_.push_back(std::make_unique<InfoRow>("戻る", GameInput::kBack));
  rows_.push_back(std::make_unique<BackRow>(kMainHowToPlayIndex));
}

void PauseMenuScript::HowToPlayPageState::HandlePageInput(PauseMenuScript &owner,
                                                          StateMachine<PauseMenuScript> &machine,
                                                          const MenuInput &input) {
  if (input.back) {
    owner.BackToMain(machine, kMainHowToPlayIndex);
    return;
  }
  const int i = owner.selected_;
  if (i < 0 || i >= RowCount()) return;
  rows_[static_cast<size_t>(i)]->Handle(owner, machine, input);
}

// ---- コマンド ----

void PauseMenuScript::OpenHowToPlayCommand::Execute(PauseMenuScript &owner,
                                                    StateMachine<PauseMenuScript> &machine) {
  // 説明の行は選んでも何も起きないので、決定ですぐ戻れるよう「もどる」を選んだ状態で開く
  owner.selected_ = HowToPlayPageState().RowCount() - 1;
  owner.bump_ = 1.0f;
  machine.ChangeState<HowToPlayPageState>();
}

void PauseMenuScript::ResumeCommand::Execute(PauseMenuScript &owner, StateMachine<PauseMenuScript> &machine) {
  if (owner.shoreMode_) {
    owner.SaveSettingsIfDirty(false);
    owner.RequestChange(kTriggerShoreBack, machine);
    return;
  }
  owner.CloseMenu(machine);
}

void PauseMenuScript::OpenSettingsCommand::Execute(PauseMenuScript &owner,
                                                   StateMachine<PauseMenuScript> &machine) {
  owner.selected_ = kSettingsFirstIndex;
  owner.bump_ = 1.0f;
  machine.ChangeState<SettingsPageState>();
}

void PauseMenuScript::OpenKeyConfigCommand::Execute(PauseMenuScript &owner,
                                                    StateMachine<PauseMenuScript> &machine) {
  owner.selected_ = 0;
  owner.capturingRow_ = -1;
  owner.waitReleaseAction_ = nullptr;
  owner.bump_ = 1.0f;
  machine.ChangeState<KeyConfigPageState>();
}

void PauseMenuScript::TransitionCommand::Execute(PauseMenuScript &owner,
                                                 StateMachine<PauseMenuScript> &machine) {
  owner.RequestChange(trigger_, machine);
}

// ---- 設定の行 ----

bool PauseMenuScript::SliderRow::Handle(PauseMenuScript &owner, StateMachine<PauseMenuScript> &machine,
                                        const MenuInput &input) {
  (void)machine;
  bool changed = false;
  if (input.adjust != 0) {
    float v = std::clamp(Value() + step_ * input.adjust, min_, max_);
    // 0.1 刻みの丸め（浮動小数の積み残しで 0.30000001 のような表示になるのを防ぐ）
    v = std::round(v / step_) * step_;
    SetValue(v);
    changed = true;
  }
  // ドラッグ中はマウス X のバー上の位置を値の範囲へ割り当て、刻みに丸める
  if (input.dragSlider) {
    float l, t, r, b;
    owner.SliderRect(owner.selected_, l, t, r, b);
    const float sl = owner.X(l), sr = owner.X(r);
    if (sr > sl) {
      const float u = std::clamp((input.mouseX - sl) / (sr - sl), 0.0f, 1.0f);
      float nv = min_ + (max_ - min_) * u;
      nv = std::round(nv / step_) * step_;
      if (std::fabs(nv - Value()) > 1e-4f) {
        SetValue(std::clamp(nv, min_, max_));
        changed = true;
      }
    }
    owner.draggingSlider_ = true;
  }
  return changed;
}

void PauseMenuScript::SliderRow::DrawContents(const PauseMenuScript &owner, const DrawContext &c) const {
  // 左：項目名
  DrawLabelLeft(owner, c);

  const float v = Value();
  const float u = (max_ > min_) ? std::clamp((v - min_) / (max_ - min_), 0.0f, 1.0f) : 0.0f;

  // 中：スライダー
  float sl, st, sr, sb;
  owner.SliderRect(c.index, sl, st, sr, sb);
  const RC::Vector2 stl = {owner.X(sl), owner.Y(st)};
  const RC::Vector2 sbr = {owner.X(sr), owner.Y(sb)};
  RC::DrawBox(stl, sbr, WithAlpha(kDim, 0.45f * c.a));
  RC::DrawBox(stl, {stl.x + (sbr.x - stl.x) * u, sbr.y}, WithAlpha(c.selected ? owner.accentColor_ : kMuted, 0.9f * c.a));
  RC::DrawBox(stl, sbr, WithAlpha(kInk, 0.35f * c.a), kWire);
  // つまみ
  const float kx = stl.x + (sbr.x - stl.x) * u;
  const float ky = (stl.y + sbr.y) * 0.5f;
  RC::DrawCircle({kx, ky}, owner.S(8.0f), WithAlpha(c.selected ? kInk : kMuted, c.a));
  // 選択中は両端に ◀ ▶
  if (c.selected) {
    const float ay = ky;
    const float ax = stl.x - owner.S(16.0f);
    RC::DrawTriangle({ax + owner.S(5.0f), ay - owner.S(6.0f)}, {ax - owner.S(5.0f), ay},
                     {ax + owner.S(5.0f), ay + owner.S(6.0f)}, WithAlpha(owner.accentColor_, c.a));
    const float bx = sbr.x + owner.S(16.0f);
    RC::DrawTriangle({bx - owner.S(5.0f), ay - owner.S(6.0f)}, {bx + owner.S(5.0f), ay},
                     {bx - owner.S(5.0f), ay + owner.S(6.0f)}, WithAlpha(owner.accentColor_, c.a));
  }

  // 右：数値（書式は派生クラスが決める）
  owner.Text(owner.itemFont_, FormatValue(v), owner.X(c.r - 18.0f), c.textY, c.ink, TextAlign::Right);
}

REGISTER_SCRIPT(PauseMenuScript)
