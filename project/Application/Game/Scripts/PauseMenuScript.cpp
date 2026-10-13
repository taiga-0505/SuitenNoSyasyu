#include "ECS/ScriptableEntity.h"
#include "ECS/ScriptRegistry.h"
#include "Common/Log/Log.h"
#include "Input/Input.h"
#include "RenderCommon.h"
#include "Engine/Render/RenderContext.h"
#include "Framework/App.h"
#include "Game/Framework/GameSettings.h"
#include "Game/Framework/UnderwaterLook.h"
#include "Scene.h"
#include "SceneFlow.h"

#if RC_ENABLE_IMGUI
#include "imgui/imgui.h"
#endif

#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

/// @class PauseMenuScript
/// @brief ESC（コントローラーは START）で開くポーズメニュー
/// @details
///   構成:
///     - メイン: つづける / 設定 / はじめから / タイトルへ
///     - 設定  : マウス感度 / スティック感度 / 上下反転 / 全体音量 / BGM 音量 / SE 音量 / もどる
///       値は GameSettings が持ち、変更があれば設定画面を閉じるときに保存する。
///   止め方:
///     - 開いている間は SceneContext::gamePaused を立てる。DataDrivenScene はこれを見て
///       スクリプト・アニメーション・GameMode を deltaTime = 0 で回すので、敵も弾もレールも止まる。
///       このスクリプト自身も OnUpdate の引数は 0 になるため、ctx.deltaTime を直接読んで動く。
///     - マウスカーソルは RailShooterController が「deltaTime = 0 のフレームはロックを外す」ので、
///       メニューが開くと自然に表示され、閉じると次のフレームで再びロックされる。
///   入力:
///     - ↑↓ / W S / 十字キー / 左スティック で選択、←→ / A D で値の増減、
///       Enter / Space / A ボタンで決定、ESC / B ボタンで戻る（メインなら再開）。
///     - マウスはホバーで選択、クリックで決定。スライダーはバーをクリックした位置の値になる。
///   Result 画面（ResultScreenScript）と同じフォント・墨色のパネル・生成りの文字で揃える。
///
///   JSON（scriptDataList）で設定できる項目:
///     "fontPath"   : 使うフォント（既定は Result と同じ KiwiMaru-Medium）
///     "titleLabel" : 4 番目の項目の文字（既定 "タイトルへ"。ステージでは "セレクトへ"）
///
///   遷移は遷移表（Resources/SceneFlow.json）で決まる:
///     きっかけ "pause_retry" : 「はじめから」（遷移先を $current にするといまのシーンをやり直す）
///     きっかけ "pause_exit"  : 4 番目の項目（ステージは Select。シーンごとに行を分けられる）
///     演出が "dive" のときは、ポーズを掛けたままメニューを消し、画面を深海色へ沈めてから遷移する
///     （遷移先は深海から浮上して始まる。SceneManager の Dive 遷移は同じ深海色へ抜ける）
///     "panelColor" / "accentColor" / "dimAlpha"
class PauseMenuScript : public ScriptableEntity {
public:
  std::string fontPath = "Resources/fonts/Kiwi_Maru/KiwiMaru-Medium.ttf";
  std::string titleLabel = "タイトルへ";
  RC::Vector4 panelColor = {0.06f, 0.08f, 0.11f, 0.92f}; ///< 墨
  RC::Vector4 accentColor = {0.88f, 0.74f, 0.38f, 1.0f}; ///< 金（選択中）
  float dimAlpha = 0.55f;                                ///< 背後の画面を沈める濃さ

  /// @brief 設定項目を JSON に書き出す
  /// @return 設定項目を格納した JSON
  nlohmann::json Serialize() override {
    return {
        {"fontPath", fontPath},
        {"titleLabel", titleLabel},
        {"panelColor", {panelColor.x, panelColor.y, panelColor.z, panelColor.w}},
        {"accentColor", {accentColor.x, accentColor.y, accentColor.z, accentColor.w}},
        {"dimAlpha", dimAlpha},
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
    readS("fontPath", fontPath);
    readS("titleLabel", titleLabel);
    readC("panelColor", panelColor);
    readC("accentColor", accentColor);
    if (j.contains("dimAlpha")) dimAlpha = j["dimAlpha"].get<float>();
  }

  /// @brief インスペクタにメニューの状態・開閉ボタン・見た目と GameSettings の編集 UI を表示する
  void OnImGui() override {
#if RC_ENABLE_IMGUI
    ImGui::Text("Pause: %s  page=%s  selected=%d", open_ ? "OPEN" : "closed",
                page_ == Page::Main ? "main" : "settings", selected_);
    if (ImGui::Button(open_ ? "Close Menu" : "Open Menu")) {
      if (open_) Close(); else Open();
    }
    ImGui::ColorEdit4("Panel Color", &panelColor.x);
    ImGui::ColorEdit4("Accent Color", &accentColor.x);
    ImGui::SliderFloat("Dim Alpha", &dimAlpha, 0.0f, 1.0f);
    ImGui::Separator();
    // GameSettings を直接編集し、変更があれば即反映して保存待ちにする
    ImGui::TextUnformatted("GameSettings (shared, saved to Resources/Setting/GameSettings.json)");
    GameSettings &gs = GameSettings::Get();
    bool changed = false;
    changed |= ImGui::SliderFloat("Mouse Sensitivity", &gs.mouseSensitivity, GameSettings::kSensitivityMin, GameSettings::kSensitivityMax);
    changed |= ImGui::SliderFloat("Stick Sensitivity", &gs.controllerSensitivity, GameSettings::kSensitivityMin, GameSettings::kSensitivityMax);
    changed |= ImGui::Checkbox("Invert Y", &gs.invertY);
    changed |= ImGui::SliderFloat("FOV (vertical deg)", &gs.fovDeg, GameSettings::kFovMin, GameSettings::kFovMax);
    changed |= ImGui::SliderFloat("Master Volume", &gs.masterVolume, 0.0f, 1.0f);
    changed |= ImGui::SliderFloat("BGM Volume", &gs.bgmVolume, 0.0f, 1.0f);
    changed |= ImGui::SliderFloat("SE Volume", &gs.seVolume, 0.0f, 1.0f);
    if (changed) {
      gs.Apply();
      settingsDirty_ = true;
    }
    if (ImGui::Button("Save Settings")) SaveSettingsIfDirty(true);
#endif
  }

protected:
  /// @brief 設定を読み込み、ポーズ状態を初期化してフォントを用意する
  void OnCreate() override {
    GameSettings::Get().EnsureLoaded();
    if (SceneContext *ctx = GetSceneContext()) ctx->gamePaused = false;
    open_ = false;
    decided_ = false;
    settingsDirty_ = false;

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
      const int handle = RC::LoadFont(fontPath, std::round(designPx * fontScale_), atlas);
      if (handle < 0) Log::Print("[PauseMenuScript] failed to load font: " + fontPath);
      return handle;
    };
    headingFont_ = load(kHeadingPx, 1024);
    itemFont_ = load(kItemPx, 1024);
    labelFont_ = load(kLabelPx, 1024);

    if (Input *in = Input::GetInstance()) in->GetGameMousePosition(lastMouseX_, lastMouseY_);
  }

  /// @brief ポーズを解除し、未保存の設定を保存してフォントを解放する
  void OnDestroy() override {
    // シーンをまたいでポーズが残らないように必ず下ろす
    if (SceneContext *ctx = GetSceneContext()) ctx->gamePaused = false;
    SaveSettingsIfDirty(false);
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

  /// @brief 閉じているときは開く操作を待ち、開いているときはアニメーションと入力を処理する
  /// @param deltaTime ポーズ中は 0 で来るので使わない。ctx.deltaTime（実時間）で動く
  void OnUpdate(float /*deltaTime*/) override {
    SceneContext *ctx = GetSceneContext();
    if (!ctx || !ctx->isPlaying() || !ctx->input) return; // エディタで停止・一時停止中は何もしない
    Input *in = ctx->input;
    const float dt = (std::max)(ctx->deltaTime, 0.0f);

    if (sinking_) {
      UpdateSink(dt);
      return;
    }
    if (decided_) return; // シーン遷移を要求済み（フェード中）。二重に受け付けない

    if (!open_) {
      if (WantOpen(in) && CanOpen()) Open();
      return;
    }

    // --- 開いている ---
    pulse_ += dt;
    anim_ = (std::min)(anim_ + dt / kFadeTime, 1.0f);
    if (bump_ > 0.0f) bump_ = (std::max)(0.0f, bump_ - dt / kBumpTime);

    HandleInput(in);
  }

  /// @brief メニュー（または遷移前の沈む幕）を描く
  /// @note ポストプロセスの後に描く（輪郭・水中・ビネットが UI に乗らないように）
  void OnOverlayRender() override {
    if (sinking_) {
      DrawSink();
      return;
    }
    if (!open_) return;

    // 1280x720 基準のレイアウトを、縦横の小さいほうの倍率で実解像度へ合わせる（余りは均等に空ける）
    float screenW = 1280.0f, screenH = 720.0f;
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

    const float a = EaseOutCubic(anim_);

    // 背後の画面を沈める
    RC::DrawBox({0.0f, 0.0f}, {screenW, screenH}, {0.02f, 0.03f, 0.06f, dimAlpha * a});

    if (page_ == Page::Main) DrawMain(a);
    else DrawSettings(a);
  }

private:
  /// @brief 表示中のページ（メインメニュー / 設定画面）
  enum class Page { Main, Settings };

  /// @brief メインメニューの項目
  enum MainItem { kResume = 0, kSettings, kRestart, kTitle, kMainCount };
  /// @brief 設定画面の行
  enum SettingRow { kMouseSens = 0, kStickSens, kInvertY, kFov, kMasterVol, kBgmVol, kSeVol, kBack, kSettingCount };

  static constexpr const char *kMainLabels[kMainCount] = {"つづける", "設定", "はじめから", "タイトルへ"};
  static constexpr const char *kSettingLabels[kSettingCount] = {
      "マウス感度", "スティック感度", "上下反転", "視野角", "全体音量", "BGM 音量", "SE 音量", "もどる"};

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
  static constexpr SHORT kStickThreshold = 16000;
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

  /// @brief 基準座標の X / Y を実画面座標へ変換する
  /// @param x 基準座標の X（Y は y）
  /// @return 実画面座標
  float X(float x) const { return L_.ox + x * L_.s; }
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
  /// @brief 終わりに向かって減速する 3 次のイージング（1 - (1 - t)^3）
  /// @param t 進行度（0〜1 に丸める）
  /// @return 補間後の進行度（0〜1）
  static float EaseOutCubic(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    const float u = 1.0f - t;
    return 1.0f - u * u * u;
  }

  /// @brief 表示中のページに応じた行数・パネル幅・行の高さ・行の間隔を返す
  /// @return 行数 / 基準座標での長さ（メインと設定で値が異なる）
  int RowCount() const { return (page_ == Page::Main) ? kMainCount : kSettingCount; }
  float PanelW() const { return (page_ == Page::Main) ? kMainPanelW : kSettingsPanelW; }
  float RowH() const { return (page_ == Page::Main) ? kMainRowH : kSettingsRowH; }
  float RowGap() const { return (page_ == Page::Main) ? kMainRowGap : kSettingsRowGap; }
  /// @brief パネルの高さ（上余白 + 見出し + 全行 + 行間 + 下余白）を求める
  /// @return パネルの高さ（基準座標）
  float PanelH() const {
    return kPanelPadTop + kHeadingBlockH + RowCount() * RowH() +
           (RowCount() - 1) * RowGap() + kPanelPadBottom;
  }
  /// @brief 画面中央に置いたパネルの左端 / 上端を求める
  /// @return パネルの左端 / 上端（基準座標）
  float PanelL() const { return (kDesignW - PanelW()) * 0.5f; }
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

  /// @brief 設定の行がスライダーで値を変える行か調べる
  /// @param row 設定画面の行番号（SettingRow）
  /// @return 感度・視野角・音量の行なら true（上下反転・もどるは false）
  bool IsSlider(int row) const {
    return row == kMouseSens || row == kStickSens || row == kFov ||
           row == kMasterVol || row == kBgmVol || row == kSeVol;
  }

  // ------------------------------------------------------------------
  // 開閉
  // ------------------------------------------------------------------

  /// @brief メニューを開く操作（ESC / START ボタン）がこのフレームにあったか調べる
  /// @param in 入力
  /// @return 開く操作があれば true
  bool WantOpen(Input *in) const {
    bool key = in->IsKeyTrigger(DIK_ESCAPE);
#if RC_ENABLE_IMGUI
    // インスペクタのテキスト入力中の ESC はエディタのもの
    if (ImGui::GetIO().WantCaptureKeyboard) key = false;
#endif
    if (in->IsXInputConnected() && in->IsXInputButtonTrigger(XINPUT_GAMEPAD_START)) key = true;
    return key;
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
  void Open() {
    if (SceneContext *ctx = GetSceneContext()) ctx->gamePaused = true;
    open_ = true;
    page_ = Page::Main;
    selected_ = kResume;
    anim_ = 0.0f;
    pulse_ = 0.0f;
    bump_ = 0.0f;
    holdDir_ = 0;
    holdTime_ = 0.0f;
    prevStickY_ = 0;
    prevStickX_ = 0;
    if (Input *in = Input::GetInstance()) in->GetGameMousePosition(lastMouseX_, lastMouseY_);
    Log::Print("[PauseMenuScript] pause");
  }

  /// @brief 未保存の設定を保存し、ポーズを解除してメニューを閉じる
  void Close() {
    SaveSettingsIfDirty(false);
    if (SceneContext *ctx = GetSceneContext()) ctx->gamePaused = false;
    open_ = false;
    Log::Print("[PauseMenuScript] resume");
  }

  /// @brief 設定に変更があれば GameSettings を保存する
  /// @param force true なら変更が無くても保存する
  void SaveSettingsIfDirty(bool force) {
    if (!settingsDirty_ && !force) return;
    settingsDirty_ = false;
    GameSettings::Get().Save();
  }

  // ------------------------------------------------------------------
  // 入力
  // ------------------------------------------------------------------

  /// @brief キーボード・ゲームパッド・マウスの入力をまとめ、表示中のページの処理へ渡す
  /// @param in 入力
  void HandleInput(Input *in) {
    // --- 決定 / 戻る ---
    bool confirm = in->IsKeyTrigger(DIK_RETURN) || in->IsKeyTrigger(DIK_SPACE);
    bool back = in->IsKeyTrigger(DIK_ESCAPE);
    if (in->IsXInputConnected()) {
      if (in->IsXInputButtonTrigger(XINPUT_GAMEPAD_A)) confirm = true;
      if (in->IsXInputButtonTrigger(XINPUT_GAMEPAD_B) || in->IsXInputButtonTrigger(XINPUT_GAMEPAD_START)) back = true;
    }

    // --- 上下 ---
    int move = 0;
    if (in->IsKeyTrigger(DIK_UP) || in->IsKeyTrigger(DIK_W)) move -= 1;
    if (in->IsKeyTrigger(DIK_DOWN) || in->IsKeyTrigger(DIK_S)) move += 1;
    // --- 左右（設定の値を動かす。長押しで連続） ---
    int adjust = 0;
    const bool leftHeld = in->IsKeyPressed(DIK_LEFT) || in->IsKeyPressed(DIK_A);
    const bool rightHeld = in->IsKeyPressed(DIK_RIGHT) || in->IsKeyPressed(DIK_D);
    if (in->IsKeyTrigger(DIK_LEFT) || in->IsKeyTrigger(DIK_A)) adjust -= 1;
    if (in->IsKeyTrigger(DIK_RIGHT) || in->IsKeyTrigger(DIK_D)) adjust += 1;

    // ゲームパッド: 十字キーはトリガー、スティックは倒した瞬間だけ 1 回（前フレームと比べる）
    int stickX = 0;
    if (in->IsXInputConnected()) {
      if (in->IsXInputButtonTrigger(XINPUT_GAMEPAD_DPAD_UP)) move -= 1;
      if (in->IsXInputButtonTrigger(XINPUT_GAMEPAD_DPAD_DOWN)) move += 1;
      if (in->IsXInputButtonTrigger(XINPUT_GAMEPAD_DPAD_LEFT)) adjust -= 1;
      if (in->IsXInputButtonTrigger(XINPUT_GAMEPAD_DPAD_RIGHT)) adjust += 1;
      const SHORT ly = in->GetXInputThumbLY();
      int stickY = 0;
      if (ly > kStickThreshold) stickY = -1;      // スティック上 = 上の項目
      else if (ly < -kStickThreshold) stickY = 1;
      if (stickY != 0 && stickY != prevStickY_) move += stickY;
      prevStickY_ = stickY;

      const SHORT lx = in->GetXInputThumbLX();
      if (lx > kStickThreshold) stickX = 1;
      else if (lx < -kStickThreshold) stickX = -1;
      if (stickX != 0 && stickX != prevStickX_) adjust += stickX;
      prevStickX_ = stickX;
    }

    // 長押しリピート（キー・十字キー・スティックのどれでも）。
    // 同じ向きを kRepeatDelay 秒押し続けたら、以後 kRepeatInterval 秒ごとに 1 刻み動かす
    int heldDir = 0;
    if (leftHeld || (in->IsXInputConnected() && in->IsXInputButtonPressed(XINPUT_GAMEPAD_DPAD_LEFT)) || stickX < 0) heldDir = -1;
    if (rightHeld || (in->IsXInputConnected() && in->IsXInputButtonPressed(XINPUT_GAMEPAD_DPAD_RIGHT)) || stickX > 0) heldDir = 1;
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
    // スライダーはドラッグ中も追従させる
    const bool mouseDragSlider = in->IsMousePressed(0) && page_ == Page::Settings &&
                                 IsSlider(selected_) && (mouseClick || draggingSlider_);

    // 上下移動は端で反対側へ回り込む
    if (move != 0) {
      const int n = RowCount();
      SetSelected((selected_ + move + n) % n);
    }

    if (page_ == Page::Main) {
      HandleMain(confirm || mouseClick, back);
    } else {
      HandleSettings(confirm, mouseClick, back, adjust, mouseDragSlider, mx);
    }
    if (!in->IsMousePressed(0)) draggingSlider_ = false;
  }

  /// @brief メインページの決定・戻るを処理する（再開 / 設定へ / はじめから / タイトル・セレクトへ）
  /// @param confirm 決定操作（キー・ボタン・クリック）があったか
  /// @param back 戻る操作があったか（メインでは再開）
  void HandleMain(bool confirm, bool back) {
    if (back) {
      Close();
      return;
    }
    if (!confirm) return;
    switch (selected_) {
    case kResume:
      Close();
      break;
    case kSettings:
      page_ = Page::Settings;
      selected_ = kMouseSens;
      bump_ = 1.0f;
      break;
    case kRestart:
      RequestChange(kTriggerRetry);
      break;
    case kTitle:
      RequestChange(kTriggerExit);
      break;
    default:
      break;
    }
  }

  /// @brief 設定ページの操作を処理する（値の増減・スライダーのドラッグ・上下反転の切り替え・もどる）
  /// @param confirm キー・ボタンでの決定操作があったか
  /// @param mouseClick このフレームに行をクリックしたか
  /// @param back 戻る操作があったか
  /// @param adjust 左右の値の増減（-1 / 0 / +1。長押しリピート込み）
  /// @param dragSlider マウスでスライダーを操作中か
  /// @param mouseX マウスの X（実画面座標）。スライダーの値の計算に使う
  void HandleSettings(bool confirm, bool mouseClick, bool back, int adjust, bool dragSlider, float mouseX) {
    GameSettings &gs = GameSettings::Get();

    // もどる: 変更を保存してメインページの「設定」へ戻る
    if (back || ((confirm || mouseClick) && selected_ == kBack)) {
      SaveSettingsIfDirty(false);
      page_ = Page::Main;
      selected_ = kSettings;
      bump_ = 1.0f;
      return;
    }

    bool changed = false;
    if (selected_ == kInvertY) {
      if (confirm || mouseClick || adjust != 0) {
        gs.invertY = !gs.invertY;
        changed = true;
      }
    } else if (IsSlider(selected_)) {
      float minV, maxV, step;
      float *v = SliderTarget(selected_, minV, maxV, step);
      if (v) {
        if (adjust != 0) {
          *v = std::clamp(*v + step * adjust, minV, maxV);
          // 0.1 刻みの丸め（浮動小数の積み残しで 0.30000001 のような表示になるのを防ぐ）
          *v = std::round(*v / step) * step;
          changed = true;
        }
        // ドラッグ中はマウス X のバー上の位置を値の範囲へ割り当て、刻みに丸める
        if (dragSlider) {
          float l, t, r, b;
          SliderRect(selected_, l, t, r, b);
          const float sl = X(l), sr = X(r);
          if (sr > sl) {
            const float u = std::clamp((mouseX - sl) / (sr - sl), 0.0f, 1.0f);
            float nv = minV + (maxV - minV) * u;
            nv = std::round(nv / step) * step;
            if (std::fabs(nv - *v) > 1e-4f) {
              *v = std::clamp(nv, minV, maxV);
              changed = true;
            }
          }
          draggingSlider_ = true;
        }
      }
    }

    if (changed) {
      gs.Apply(); // 音量は即反映（感度は RailShooterController が毎フレーム読む）
      settingsDirty_ = true;
    }
  }

  /// @brief 設定行に対応する GameSettings の値と範囲を取得する
  /// @param row 設定画面の行番号（SettingRow）
  /// @param minV 最小値の出力先
  /// @param maxV 最大値の出力先
  /// @param step 1 刻みの量の出力先
  /// @return 対応する GameSettings の値へのポインタ。スライダーの行でなければ nullptr（範囲はすべて 0）
  static float *SliderTarget(int row, float &minV, float &maxV, float &step) {
    GameSettings &gs = GameSettings::Get();
    switch (row) {
    case kMouseSens:
      minV = GameSettings::kSensitivityMin; maxV = GameSettings::kSensitivityMax; step = GameSettings::kSensitivityStep;
      return &gs.mouseSensitivity;
    case kStickSens:
      minV = GameSettings::kSensitivityMin; maxV = GameSettings::kSensitivityMax; step = GameSettings::kSensitivityStep;
      return &gs.controllerSensitivity;
    case kFov:
      minV = GameSettings::kFovMin; maxV = GameSettings::kFovMax; step = GameSettings::kFovStep;
      return &gs.fovDeg;
    case kMasterVol:
      minV = 0.0f; maxV = 1.0f; step = GameSettings::kVolumeStep;
      return &gs.masterVolume;
    case kBgmVol:
      minV = 0.0f; maxV = 1.0f; step = GameSettings::kVolumeStep;
      return &gs.bgmVolume;
    case kSeVol:
      minV = 0.0f; maxV = 1.0f; step = GameSettings::kVolumeStep;
      return &gs.seVolume;
    default:
      minV = maxV = step = 0.0f;
      return nullptr;
    }
  }

  static constexpr const char *kTriggerRetry = "pause_retry"; ///< 遷移表のきっかけ名（はじめから）
  static constexpr const char *kTriggerExit = "pause_exit";   ///< 遷移表のきっかけ名（タイトル／セレクトへ）

  /// @brief 遷移表に従って遷移を要求する
  /// @param trigger 遷移表のきっかけ名（kTriggerRetry / kTriggerExit）
  void RequestChange(const char *trigger) {
    // ポーズは掛けたまま抜ける（フェード中に敵が動かないように）。
    // gamePaused は OnExit → OnDestroy で必ず下ろされる。

    // 演出が "dive" なら、メニューを消して画面を深海色へ沈めてから遷移する（UpdateSink）
    std::string target;
    std::string transition;
    if (LookupTransition(trigger, target, transition) && !target.empty() &&
        transition == SceneTransitions::kDive) {
      decided_ = true;
      open_ = false;
      sinking_ = true;
      sinkTime_ = 0.0f;
      sinkRequested_ = false;
      sinkTarget_ = target;
      Log::Print(std::string("[PauseMenuScript] ") + trigger + " -> sink -> " + target);
      return;
    }

    if (RequestTransition(trigger)) {
      decided_ = true;
      // メニューはポストプロセスの後に描いているのでディゾルブに巻き込まれない。
      // 出しっぱなしだと画面が黒くなってもメニューだけ残るので、ここで消す。
      open_ = false;
      Log::Print(std::string("[PauseMenuScript] ") + trigger);
    } else {
      Log::Print(std::string("[PauseMenuScript] scene change refused: ") + trigger);
    }
  }

  // ------------------------------------------------------------------
  // 「はじめから」「セレクトへ」：画面を深海色へ沈めてから Dive 遷移
  // ------------------------------------------------------------------

  static constexpr float kSinkTime = 0.7f;          ///< 深海色に沈みきるまで（秒。実時間）
  static constexpr float kSinkRetrySeconds = 1.5f;  ///< 遷移が通らないときに素の遷移へ倒すまで
  static constexpr int kSinkBubbles = 18;            ///< 沈むあいだに昇る泡の数

  /// @brief 沈む幕の時間を進め、沈みきったら Dive 遷移を要求する
  /// @param dt 実時間の経過（秒）。ポーズ中なので ctx.deltaTime を渡す
  /// @note Dive 遷移が kSinkRetrySeconds 通らなければ素の遷移を試し、それも通らなければメニューへ戻す
  void UpdateSink(float dt) {
    sinkTime_ += dt;
    if (sinkTime_ < kSinkTime || sinkRequested_) return;
    // SceneManager の Dive 遷移は同じ深海色へ抜けるので、継ぎ目なく切り替わる。
    // 遷移先は深海から浮上して始まる（ステージ: DeepRiseIntroScript ／ セレクト: StageSelectScript）
    sinkRequested_ = RequestSceneChange(sinkTarget_, SceneTransitions::kDive);
    if (!sinkRequested_ && sinkTime_ > kSinkTime + kSinkRetrySeconds) {
      Log::Print("[PauseMenuScript] dive transition refused, falling back: " + sinkTarget_);
      sinkRequested_ = RequestSceneChange(sinkTarget_);
      if (!sinkRequested_) {
        // 諦めてメニューへ戻す（画面が沈んだまま操作できなくなるのを避ける）
        sinking_ = false;
        decided_ = false;
        open_ = true;
        anim_ = 1.0f;
      }
    }
  }

  /// @brief 深海色の幕と、下から昇って消えていく泡を描く
  void DrawSink() const {
    // 幕の濃さ a は経過時間を smoothstep で 0〜1 にしたもの
    float screenW = 1280.0f, screenH = 720.0f;
    auto &rc = RC::GetRenderContext();
    if (rc.Ctx() && rc.Ctx()->app) {
      screenW = static_cast<float>(rc.Ctx()->app->width);
      screenH = static_cast<float>(rc.Ctx()->app->height);
    }
    const float u = std::clamp(sinkTime_ / kSinkTime, 0.0f, 1.0f);
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
      const float y = screenH * (1.05f + fx * 0.25f) - screenH * speed * sinkTime_;
      const float x = screenW * (0.08f + fx * 0.84f) + std::sin(sinkTime_ * 6.0f + i) * 6.0f * s;
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
  // 描画
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
    RC::DrawBox(tl, br, WithAlpha(panelColor, a));

    const float in1 = S(6.0f), in2 = S(11.0f);
    RC::DrawBox({tl.x + in1, tl.y + in1}, {br.x - in1, br.y - in1}, WithAlpha(kInk, 0.55f * a), kWire);
    RC::DrawBox({tl.x + in2, tl.y + in2}, {br.x - in2, br.y - in2}, WithAlpha(kInk, 0.22f * a), kWire);

    // 四隅に長さ arm の L 字をアクセント色で描く（左上・右上・左下・右下の順に 2 本ずつ）
    const float arm = S(22.0f);
    const float th = (std::max)(1.0f, S(2.0f));
    const RC::Vector4 c = WithAlpha(accentColor, 0.9f * a);
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
    RC::DrawLine({cx - halfW, lineY}, {cx + halfW, lineY}, WithAlpha(accentColor, 0.7f * a), (std::max)(1.0f, S(1.5f)));
    Text(labelFont_, sub, cx, lineY + S(6.0f), WithAlpha(kMuted, 0.9f * a), TextAlign::Center, 0.85f);
  }

  /// @brief 行の下地（選択中は金の枠と左の三角マーカー）
  void DrawRowFrame(int i, float a) const {
    float l, t, r, b;
    RowRect(i, l, t, r, b);
    const RC::Vector2 tl = {X(l), Y(t)};
    const RC::Vector2 br = {X(r), Y(b)};
    const bool sel = (i == selected_);
    if (sel) {
      const float pulse = 0.5f + 0.5f * std::sin(pulse_ * 4.0f);
      const float borderA = (0.55f + 0.45f * pulse) * a;
      RC::DrawBox(tl, br, WithAlpha(accentColor, 0.12f * a));
      RC::DrawBox(tl, br, WithAlpha(accentColor, borderA), kWire);
      // 選び直した直後は外側にもう 1 本枠を出し、bump_ の減衰に合わせて縮める
      const float o = S(3.0f) * bump_;
      if (o > 0.0f) {
        RC::DrawBox({tl.x - o, tl.y - o}, {br.x + o, br.y + o}, WithAlpha(accentColor, 0.5f * borderA), kWire);
      }
      const float mx = tl.x - S(16.0f);
      const float my = (tl.y + br.y) * 0.5f;
      RC::DrawTriangle({mx - S(6.0f), my - S(7.0f)}, {mx + S(6.0f), my}, {mx - S(6.0f), my + S(7.0f)},
                       WithAlpha(accentColor, a));
    } else {
      RC::DrawBox(tl, br, WithAlpha(kDim, 0.35f * a), kWire);
    }
  }

  /// @brief メインページ（パネル・見出し・各項目・操作説明）を描く
  /// @param a 開くアニメーションの不透明度（0〜1）
  void DrawMain(float a) const {
    DrawPanel(a);
    DrawHeading("一時停止", "PAUSE", a);

    for (int i = 0; i < kMainCount; ++i) {
      DrawRowFrame(i, a);
      float l, t, r, b;
      RowRect(i, l, t, r, b);
      const bool sel = (i == selected_);
      const float lh = LineH(itemFont_);
      Text(itemFont_, (i == kTitle) ? titleLabel.c_str() : kMainLabels[i], X((l + r) * 0.5f), Y((t + b) * 0.5f) - lh * 0.5f,
           WithAlpha(sel ? kInk : kMuted, a), TextAlign::Center);
    }
    DrawHint("↑↓ 選択　　Enter / Space 決定　　ESC 再開", a);
  }

  /// @brief 設定ページ（各行の項目名・スライダー・ON/OFF・数値と操作説明）を描く
  /// @param a 開くアニメーションの不透明度（0〜1）
  void DrawSettings(float a) const {
    DrawPanel(a);
    DrawHeading("設定", "SETTINGS", a);

    const GameSettings &gs = GameSettings::Get();
    for (int i = 0; i < kSettingCount; ++i) {
      DrawRowFrame(i, a);
      float l, t, r, b;
      RowRect(i, l, t, r, b);
      const bool sel = (i == selected_);
      const RC::Vector4 ink = WithAlpha(sel ? kInk : kMuted, a);
      const float lh = LineH(itemFont_);
      const float textY = Y((t + b) * 0.5f) - lh * 0.5f;

      if (i == kBack) {
        Text(itemFont_, kSettingLabels[i], X((l + r) * 0.5f), textY, ink, TextAlign::Center);
        continue;
      }

      // 左：項目名
      Text(itemFont_, kSettingLabels[i], X(l + 22.0f), textY, ink, TextAlign::Left);

      if (i == kInvertY) {
        // 右：ON / OFF
        const char *label = gs.invertY ? "ON" : "OFF";
        Text(itemFont_, label, X(r - 18.0f), textY, WithAlpha(gs.invertY ? accentColor : kMuted, a), TextAlign::Right);
        continue;
      }

      float minV = 0.0f, maxV = 1.0f, step = 0.1f;
      const float *v = SliderTarget(i, minV, maxV, step);
      if (!v) continue;
      const float u = (maxV > minV) ? std::clamp((*v - minV) / (maxV - minV), 0.0f, 1.0f) : 0.0f;

      // 中：スライダー
      float sl, st, sr, sb;
      SliderRect(i, sl, st, sr, sb);
      const RC::Vector2 stl = {X(sl), Y(st)};
      const RC::Vector2 sbr = {X(sr), Y(sb)};
      RC::DrawBox(stl, sbr, WithAlpha(kDim, 0.45f * a));
      RC::DrawBox(stl, {stl.x + (sbr.x - stl.x) * u, sbr.y}, WithAlpha(sel ? accentColor : kMuted, 0.9f * a));
      RC::DrawBox(stl, sbr, WithAlpha(kInk, 0.35f * a), kWire);
      // つまみ
      const float kx = stl.x + (sbr.x - stl.x) * u;
      const float ky = (stl.y + sbr.y) * 0.5f;
      RC::DrawCircle({kx, ky}, S(8.0f), WithAlpha(sel ? kInk : kMuted, a));
      // 選択中は両端に ◀ ▶
      if (sel) {
        const float ay = ky;
        const float ax = stl.x - S(16.0f);
        RC::DrawTriangle({ax + S(5.0f), ay - S(6.0f)}, {ax - S(5.0f), ay}, {ax + S(5.0f), ay + S(6.0f)}, WithAlpha(accentColor, a));
        const float bx = sbr.x + S(16.0f);
        RC::DrawTriangle({bx - S(5.0f), ay - S(6.0f)}, {bx + S(5.0f), ay}, {bx - S(5.0f), ay + S(6.0f)}, WithAlpha(accentColor, a));
      }

      // 右：数値（感度は倍率、音量は %）
      char buf[32];
      if (i == kMouseSens || i == kStickSens) std::snprintf(buf, sizeof(buf), "x%.1f", *v);
      else if (i == kFov) std::snprintf(buf, sizeof(buf), "%d度", static_cast<int>(std::round(*v)));
      else std::snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(std::round(*v * 100.0f)));
      Text(itemFont_, buf, X(r - 18.0f), textY, ink, TextAlign::Right);
    }
    DrawHint("↑↓ 選択　　←→ 変更　　ESC もどる", a);
  }

  /// @brief パネルの下に操作説明を描く
  /// @param text 表示する操作説明
  /// @param a 開くアニメーションの不透明度（0〜1）
  void DrawHint(const char *text, float a) const {
    Text(labelFont_, text, X(kDesignW * 0.5f), Y(PanelT() + PanelH() + kHintGap),
         WithAlpha(kMuted, 0.8f * a), TextAlign::Center, 0.9f);
  }

  // ------------------------------------------------------------------
  // 状態
  // ------------------------------------------------------------------
  int headingFont_ = -1;
  int itemFont_ = -1;
  int labelFont_ = -1;
  float fontScale_ = 1.0f;

  bool open_ = false;
  bool decided_ = false;
  // 深海色へ沈めてから Dive 遷移（UpdateSink / DrawSink）
  bool sinking_ = false;
  bool sinkRequested_ = false;
  float sinkTime_ = 0.0f;
  std::string sinkTarget_;
  bool settingsDirty_ = false;
  bool draggingSlider_ = false;
  Page page_ = Page::Main;
  int selected_ = 0;

  float anim_ = 0.0f;
  float pulse_ = 0.0f;
  float bump_ = 0.0f;
  int holdDir_ = 0;
  float holdTime_ = 0.0f;
  int prevStickY_ = 0;
  int prevStickX_ = 0;
  float lastMouseX_ = 0.0f;
  float lastMouseY_ = 0.0f;
};

REGISTER_SCRIPT(PauseMenuScript)
