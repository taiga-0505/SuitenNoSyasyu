#include "ECS/ScriptableEntity.h"
#include "ECS/ScriptRegistry.h"
#include "Common/Log/Log.h"
#include "Input/Input.h"
#include "RenderCommon.h"
#include "Engine/Render/RenderContext.h"
#include "Framework/App.h"
#include "Game/Framework/StageProgress.h"
#include "Game/Framework/UnderwaterLook.h"
#include "Game/Framework/WaterCameraFx.h"
#include "Scene.h"
#include "SceneFlow.h"

#if RC_ENABLE_IMGUI
#include "imgui/imgui.h"
#endif

#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <string>

/// @class StageSelectScript
/// @brief ステージセレクト（航路選択）。5 つの面を「水面の断面図」に並べて選ばせる
/// @details
///   構成:
///     - 背景の波は Select.json 側の WaterComponent と真上視点のカメラ（Result と同じ作り）。
///       このスクリプトは 2D の HUD だけを描く。
///     - 上段の海図パネル: 水面の線を横に引き、水上の面は線の上、水中の面は線の下に丸を置く。
///       中間の洞窟 3 面（2〜4）は岩の天井と床で囲って「洞窟の中」であることを見せる。
///       丸どうしを航路の線で結び、クリア済みの区間は浅葱で塗る。
///     - 下段の情報パネル: 選択中の面の名前・欧文・環境（水上／水中・外洋／洞窟）・最高得点・状態。
///     - 進行（解放・クリア・最高得点）は StageProgress から読む。未解放の面は錠前を描いて選べない。
///   入力:
///     ←→ / A D / 十字キー / 左スティック で面を選ぶ（未解放の面は飛ばさず止まる）、
///     Space / Enter / A ボタン / クリックで出航、ESC / B ボタンでタイトルへ。
///   遷移:
///     Title から Dive 遷移で入ってきたときは深海から浮上してから UI を出す（Result と同じ RiseSequence）。
///     出航すると UI を消して水へ飛び込み（DiveSequence）、"dive" 遷移でステージへ。
///     ステージ側は DeepRiseIntroScript が深海からの浮上で受け取る。
///
///   JSON (scriptDataList) で設定できる項目:
///     "titleText" / "subtitleText" : 見出し
///   遷移は遷移表（Resources/SceneFlow.json）で決まる:
///     きっかけ "back"  : ESC で戻る
///     きっかけ "stage" : 出航（遷移先を $arg にすると、選んだステージのシーンへ行く）
///                        演出が "dive" のときだけ飛び込み演出を挟む
///     "fontPath"        : フォント
///     "dimAlpha" / "panelColor" / "accentColor" / "selectedColor"
///     "uiFadeTime"      : 出航を決めてから UI が消えきるまで（秒）
///     "dive*" / "rise*" / "underwater" : Result と同じキー
class StageSelectScript : public ScriptableEntity {
public:
  std::string titleText = "航路選択";
  std::string subtitleText = "STAGE SELECT";
  std::string fontPath = "Resources/fonts/Kiwi_Maru/KiwiMaru-Medium.ttf";

  float dimAlpha = 0.22f;
  RC::Vector4 panelColor = {0.05f, 0.06f, 0.09f, 0.78f};
  RC::Vector4 accentColor = {0.42f, 0.76f, 0.88f, 1.0f};
  RC::Vector4 selectedColor = {0.12f, 0.27f, 0.50f, 0.96f};

  float uiFadeTime = 0.35f;
  WaterCameraFx::DiveParams dive;
  WaterCameraFx::RiseParams rise;
  UnderwaterLook::Params look;

  nlohmann::json Serialize() override {
    auto v4 = [](const RC::Vector4 &c) { return nlohmann::json{c.x, c.y, c.z, c.w}; };
    nlohmann::json j = {
        {"titleText", titleText},
        {"subtitleText", subtitleText},
        {"fontPath", fontPath},
        {"dimAlpha", dimAlpha},
        {"panelColor", v4(panelColor)},
        {"accentColor", v4(accentColor)},
        {"selectedColor", v4(selectedColor)},
        {"uiFadeTime", uiFadeTime},
    };
    j["underwater"] = look.ToJson();
    dive.WriteJson(j);
    rise.WriteJson(j);
    return j;
  }

  void Deserialize(const nlohmann::json &j) override {
    auto readF = [&](const char *key, float &out) {
      if (j.contains(key) && j[key].is_number()) out = j[key].get<float>();
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
    readS("titleText", titleText);
    readS("subtitleText", subtitleText);
    readS("fontPath", fontPath);
    readF("dimAlpha", dimAlpha);
    readVec4("panelColor", panelColor);
    readVec4("accentColor", accentColor);
    readVec4("selectedColor", selectedColor);
    readF("uiFadeTime", uiFadeTime);
    if (j.contains("underwater")) look.FromJson(j["underwater"]);
    dive.ReadJson(j);
    rise.ReadJson(j);
  }

  void OnImGui() override {
#if RC_ENABLE_IMGUI
    StageProgress &prog = StageProgress::Get();
    ImGui::Text("selected=%d  highestUnlocked=%d", selected_ + 1, prog.HighestUnlocked() + 1);
    bool all = prog.UnlockAll();
    if (ImGui::Checkbox("Unlock All (debug, not saved)", &all)) prog.SetUnlockAll(all);
    if (ImGui::Button("Reset Progress")) prog.ResetAll();
    for (int i = 0; i < StageProgress::kStageCount; ++i) {
      ImGui::Text("%d %s  unlocked=%d cleared=%d best=%d", i + 1, StageProgress::Info(i).sceneName,
                  prog.IsUnlocked(i) ? 1 : 0, prog.IsCleared(i) ? 1 : 0, prog.BestScore(i));
    }
#endif
  }

protected:
  void OnCreate() override {
    RC::SetWaterObstacles(nullptr, 0);

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
      if (handle < 0) Log::Print("[StageSelectScript] failed to load font: " + fontPath);
      return handle;
    };
    headingFont_ = load(kHeadingPx, 1024);
    nameFont_ = load(kNamePx, 1024);
    nodeFont_ = load(kNodePx, 1024);
    labelFont_ = load(kLabelPx, 1024);

    StageProgress &prog = StageProgress::Get();
    // 戻ってきたときは前回の位置。今回のクリアで新しく開いた面があればそこへ
    selected_ = prog.LastSelected();
    highlight_ = -1;
    if (const int unlocked = prog.JustUnlocked(); unlocked >= 0) {
      selected_ = unlocked;
      highlight_ = unlocked;
      prog.ConsumeJustUnlocked();
    }
    if (!prog.IsUnlocked(selected_)) selected_ = prog.HighestUnlocked();

    if (Input *in = Input::GetInstance()) in->GetGameMousePosition(lastMouseX_, lastMouseY_);

    leaving_ = false;
    uiAlpha_ = 1.0f;
    uiFadeIn_ = -1.0f;
    if ((dive.enabled && dive.bubbles) || (rise.enabled && rise.bubbles)) {
      bubbles_.Spawn(GetScene(), "SelectBubbles");
    }
    if (SceneContext *ctx = GetSceneContext()) {
      if (rise.enabled && ctx->isPlaying() && ctx->lastTransition == SceneTransition::Dive) {
        Scene *scene = GetScene();
        if (rise_.Begin(WaterCameraFx::FindMainCamera(scene), WaterCameraFx::FindWaterY(scene), rise,
                        look, &bubbles_)) {
          uiAlpha_ = 0.0f;
          uiFadeIn_ = 0.0f;
        }
      }
    }
  }

  void OnUpdate(float deltaTime) override {
    if (deltaTime <= 0.0f) return;
    time_ += deltaTime;
    if (bump_ > 0.0f) bump_ = (std::max)(0.0f, bump_ - deltaTime / kBumpTime);
    if (shake_ > 0.0f) shake_ = (std::max)(0.0f, shake_ - deltaTime / kShakeTime);

    if (rise_.IsActive()) rise_.Update(deltaTime);
    if (rise_.IsMoving()) return;
    if (uiFadeIn_ >= 0.0f) {
      uiFadeIn_ += deltaTime;
      uiAlpha_ = WaterCameraFx::Smooth01(uiFadeIn_ / (std::max)(uiFadeTime, 1e-3f));
      if (uiAlpha_ >= 1.0f) { uiAlpha_ = 1.0f; uiFadeIn_ = -1.0f; }
    }

    if (leaving_) {
      UpdateLeave(deltaTime);
      return;
    }
    if (!decided_) HandleInput();
  }

  void OnDestroy() override {
    if (dive_.IsActive()) dive_.End(/*restoreCamera=*/true);
    if (rise_.IsActive()) rise_.End();
    bubbles_.Destroy();
    auto unload = [](int &handle) {
      if (handle >= 0) {
        RC::UnloadFont(handle);
        handle = -1;
      }
    };
    unload(headingFont_);
    unload(nameFont_);
    unload(nodeFont_);
    unload(labelFont_);
  }

  void OnRender() override {
    if (uiAlpha_ <= 0.0f) return;
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

    if (dimAlpha > 0.0f) UiBox({0.0f, 0.0f}, {screenW, screenH}, {0.02f, 0.03f, 0.06f, dimAlpha});

    DrawHeading();
    DrawChart();
    DrawInfo();
    DrawHint();
  }

private:
  // ------------------------------------------------------------------
  // レイアウト（1280x720 基準）
  // ------------------------------------------------------------------
  static constexpr float kDesignW = 1280.0f;
  static constexpr float kDesignH = 720.0f;

  // 海図パネル
  static constexpr float kChartL = 110.0f;
  static constexpr float kChartT = 128.0f;
  static constexpr float kChartR = 1170.0f;
  static constexpr float kChartB = 446.0f;
  static constexpr float kSurfaceY = 262.0f; ///< 水面の線
  static constexpr float kAboveY = 234.0f;   ///< 水上の面の丸（水面に浮かぶ位置）
  static constexpr float kBelowY = 350.0f;   ///< 水中の面の丸
  static constexpr float kNodeR = 26.0f;
  // 洞窟の岩（天井と床の帯）。面の名前と重ならないように、名前の外側に置く
  //   天井: kRoofT〜kRoofBase が岩の帯、そこから下へ最大 kToothMax の牙
  //         水上の面の名前は kAboveY - kNodeR - 12 - 22 = 174 から始まるので、牙の先（170）より下
  //   床  : kFloorBase〜kFloorB が岩の帯、そこから上へ最大 kToothMax の牙
  //         水中の面の名前は kBelowY + kNodeR + 12 = 388 から約 20px なので、牙の先（406）より上
  static constexpr float kRoofT = kChartT + 12.0f;
  static constexpr float kRoofBase = kChartT + 32.0f;
  static constexpr float kFloorBase = kChartB - 30.0f;
  static constexpr float kFloorB = kChartB - 12.0f;
  static constexpr float kToothMax = 10.0f;

  // 情報パネル
  static constexpr float kInfoL = 250.0f;
  static constexpr float kInfoT = 468.0f;
  static constexpr float kInfoR = 1030.0f;
  static constexpr float kInfoB = 640.0f;

  static constexpr float kHeadingPx = 44.0f;
  static constexpr float kNamePx = 40.0f;
  static constexpr float kNodePx = 26.0f;
  static constexpr float kLabelPx = 22.0f;

  static constexpr float kBumpTime = 0.16f;
  static constexpr float kShakeTime = 0.3f;
  static constexpr float kPulsePeriod = 1.4f;
  static constexpr SHORT kStickThreshold = 16000;
  static constexpr float kLeaveRetrySeconds = 1.0f;

  static constexpr RC::Vector4 kInk = {0.95f, 0.93f, 0.87f, 1.0f};
  static constexpr RC::Vector4 kMuted = {0.66f, 0.70f, 0.76f, 1.0f};
  static constexpr RC::Vector4 kDim = {0.40f, 0.44f, 0.50f, 1.0f};
  static constexpr RC::Vector4 kShu = {0.80f, 0.19f, 0.13f, 1.0f};
  static constexpr RC::Vector4 kGold = {0.88f, 0.74f, 0.38f, 1.0f};
  static constexpr RC::Vector4 kSea = {0.06f, 0.22f, 0.38f, 0.55f};
  static constexpr RC::Vector4 kRock = {0.16f, 0.14f, 0.13f, 0.95f};
  static constexpr RC::Vector4 kRockEdge = {0.42f, 0.37f, 0.32f, 1.0f};

  struct Layout {
    float s = 1.0f, ox = 0.0f, oy = 0.0f, fs = 1.0f;
  };
  Layout L_;

  float X(float x) const { return L_.ox + x * L_.s; }
  float Y(float y) const { return L_.oy + y * L_.s; }
  float S(float v) const { return v * L_.s; }
  RC::Vector2 P(float x, float y) const { return {X(x), Y(y)}; }
  static RC::Vector4 WithAlpha(const RC::Vector4 &c, float a) { return {c.x, c.y, c.z, c.w * a}; }

  /// @brief 面 i の丸の中心（基準座標）
  RC::Vector2 NodePos(int i) const {
    const float span = (kChartR - kChartL) - 240.0f;
    const float x = kChartL + 120.0f + span * static_cast<float>(i) / (StageProgress::kStageCount - 1);
    const float y = StageProgress::Info(i).underwater ? kBelowY : kAboveY;
    return {x, y};
  }

  void Text(int font, const std::string &text, float x, float y, const RC::Vector4 &color,
            TextAlign align = TextAlign::Left, float scale = 1.0f) const {
    if (font < 0 || text.empty()) return;
    const float sc = scale * L_.fs;
    const float sh = (std::max)(1.0f, S(1.0f));
    UiString(font, text, {x + sh, y + sh}, {0.0f, 0.02f, 0.05f, 0.55f * color.w}, sc, align);
    UiString(font, text, {x, y}, color, sc, align);
  }
  float LineH(int font, float scale = 1.0f) const {
    return (font >= 0) ? RC::GetFontLineHeight(font, scale * L_.fs) : S(kLabelPx) * scale;
  }
  float TextW(int font, const std::string &text, float scale = 1.0f) const {
    return (font >= 0) ? RC::MeasureString(font, text, scale * L_.fs).x : 0.0f;
  }

  // ------------------------------------------------------------------
  // 描画
  // ------------------------------------------------------------------

  void DrawFrame(float l, float t, float r, float b) const {
    const RC::Vector2 tl = P(l, t), br = P(r, b);
    UiBox(tl, br, panelColor);
    const float in1 = S(6.0f);
    UiBox({tl.x + in1, tl.y + in1}, {br.x - in1, br.y - in1}, WithAlpha(kInk, 0.4f), kWire);
    const float arm = S(22.0f), th = (std::max)(1.0f, S(2.0f)), o = S(2.0f);
    const RC::Vector4 c = WithAlpha(accentColor, 0.9f);
    UiLine({tl.x + o, tl.y + o}, {tl.x + o + arm, tl.y + o}, c, th);
    UiLine({tl.x + o, tl.y + o}, {tl.x + o, tl.y + o + arm}, c, th);
    UiLine({br.x - o, tl.y + o}, {br.x - o - arm, tl.y + o}, c, th);
    UiLine({br.x - o, tl.y + o}, {br.x - o, tl.y + o + arm}, c, th);
    UiLine({tl.x + o, br.y - o}, {tl.x + o + arm, br.y - o}, c, th);
    UiLine({tl.x + o, br.y - o}, {tl.x + o, br.y - o - arm}, c, th);
    UiLine({br.x - o, br.y - o}, {br.x - o - arm, br.y - o}, c, th);
    UiLine({br.x - o, br.y - o}, {br.x - o, br.y - o - arm}, c, th);
  }

  void DrawHeading() const {
    const float cx = X(kDesignW * 0.5f);
    const float top = Y(30.0f);
    const float hh = LineH(headingFont_);
    Text(headingFont_, titleText, cx, top, kInk, TextAlign::Center);
    const float halfW = TextW(headingFont_, titleText) * 0.5f + S(28.0f);
    const float ruleY = top + hh * 0.56f, ruleLen = S(150.0f), th = (std::max)(1.0f, S(1.0f));
    UiLine({cx - halfW - ruleLen, ruleY}, {cx - halfW, ruleY}, WithAlpha(kInk, 0.5f), th);
    UiLine({cx + halfW, ruleY}, {cx + halfW + ruleLen, ruleY}, WithAlpha(kInk, 0.5f), th);
    UiCircle({cx - halfW - ruleLen, ruleY}, S(2.5f), WithAlpha(kInk, 0.6f));
    UiCircle({cx + halfW + ruleLen, ruleY}, S(2.5f), WithAlpha(kInk, 0.6f));
    Text(labelFont_, subtitleText, cx, top + hh - S(2.0f), WithAlpha(kMuted, 0.85f), TextAlign::Center,
         0.75f);
  }

  /// @brief 海図：水面・海中・洞窟・航路・各面の丸
  void DrawChart() const {
    DrawFrame(kChartL, kChartT, kChartR, kChartB);
    const float pad = 12.0f;
    const float l = kChartL + pad, r = kChartR - pad, b = kChartB - pad;

    // 海中（水面より下を薄い藍で塗る）
    UiBox(P(l, kSurfaceY), P(r, b), kSea);

    // 洞窟（面 2〜4 を岩の天井・床・左右の口で囲む）
    DrawCave();

    // 水面の線（ゆっくり揺れる波線）
    {
      const int seg = 64;
      const float th = (std::max)(1.0f, S(2.0f));
      for (int k = 0; k < seg; ++k) {
        const float x0 = l + (r - l) * k / seg, x1 = l + (r - l) * (k + 1) / seg;
        const float y0 = kSurfaceY + 2.0f * std::sin(x0 * 0.05f + time_ * 1.6f);
        const float y1 = kSurfaceY + 2.0f * std::sin(x1 * 0.05f + time_ * 1.6f);
        UiLine(P(x0, y0), P(x1, y1), WithAlpha(accentColor, 0.75f), th);
      }
      Text(labelFont_, "水面", X(l + 10.0f), Y(kSurfaceY - 26.0f), WithAlpha(kMuted, 0.8f),
           TextAlign::Left, 0.75f);
      Text(labelFont_, "海中", X(l + 10.0f), Y(b - 26.0f), WithAlpha(kMuted, 0.6f), TextAlign::Left,
           0.75f);
    }

    // 航路（丸どうしを結ぶ）。クリア済みの面から出る区間は浅葱、それ以外は鼠の破線
    const StageProgress &prog = StageProgress::Get();
    for (int i = 0; i + 1 < StageProgress::kStageCount; ++i) {
      const RC::Vector2 a = NodePos(i), c = NodePos(i + 1);
      const bool done = prog.IsCleared(i);
      const float th = (std::max)(1.0f, S(done ? 4.0f : 2.0f));
      if (done) {
        UiLine(P(a.x, a.y), P(c.x, c.y), accentColor, th);
      } else {
        const int dashes = 10;
        for (int k = 0; k < dashes; k += 2) {
          const float t0 = static_cast<float>(k) / dashes, t1 = static_cast<float>(k + 1) / dashes;
          UiLine(P(a.x + (c.x - a.x) * t0, a.y + (c.y - a.y) * t0),
                 P(a.x + (c.x - a.x) * t1, a.y + (c.y - a.y) * t1), WithAlpha(kDim, 0.9f), th);
        }
      }
    }

    // 各面の丸
    const float pulse = 0.5f + 0.5f * std::sin(time_ * (6.28318530718f / kPulsePeriod));
    for (int i = 0; i < StageProgress::kStageCount; ++i) {
      RC::Vector2 c = NodePos(i);
      const bool sel = (i == selected_);
      const bool unlocked = prog.IsUnlocked(i);
      const bool cleared = prog.IsCleared(i);
      float rad = kNodeR * (sel ? (1.0f + 0.08f * bump_) : 0.82f);
      if (sel && shake_ > 0.0f) c.x += std::sin(shake_ * 40.0f) * 5.0f * shake_;

      if (sel) {
        UiCircle(P(c.x, c.y), S(rad + 9.0f + 3.0f * pulse), WithAlpha(accentColor, 0.25f + 0.2f * pulse));
      }
      if (i == highlight_) {
        UiCircle(P(c.x, c.y), S(rad + 16.0f + 6.0f * pulse), WithAlpha(kGold, 0.2f * pulse), kWire);
      }
      UiCircle(P(c.x, c.y), S(rad), unlocked ? kInk : WithAlpha(kDim, 0.9f));
      UiCircle(P(c.x, c.y), S(rad - 3.5f),
               unlocked ? (sel ? selectedColor : RC::Vector4{panelColor.x, panelColor.y, panelColor.z, 1.0f})
                        : RC::Vector4{0.08f, 0.09f, 0.11f, 1.0f});

      if (unlocked) {
        const std::string num = std::to_string(i + 1);
        Text(nodeFont_, num, X(c.x), Y(c.y) - LineH(nodeFont_) * 0.5f, sel ? kInk : kMuted,
             TextAlign::Center);
      } else {
        DrawLock(c);
      }
      // クリア印（右上の金の小丸）
      if (cleared) {
        UiCircle(P(c.x + rad * 0.72f, c.y - rad * 0.72f), S(7.0f), kGold);
        UiCircle(P(c.x + rad * 0.72f, c.y - rad * 0.72f), S(3.0f), WithAlpha(panelColor, 1.0f));
      }
      // 名前（水上は丸の上、水中は丸の下）
      const StageProgress::StageInfo &info = StageProgress::Info(i);
      const float ny = info.underwater ? (c.y + kNodeR + 12.0f) : (c.y - kNodeR - 12.0f - kLabelPx);
      Text(labelFont_, unlocked ? info.title : "???", X(c.x), Y(ny),
           WithAlpha(sel ? kInk : kMuted, unlocked ? 1.0f : 0.6f), TextAlign::Center, 0.85f);
    }
  }

  /// @brief 面 2〜4 を囲む洞窟（岩の天井・床と、左右の口）
  void DrawCave() const {
    int first = -1, last = -1;
    for (int i = 0; i < StageProgress::kStageCount; ++i) {
      if (!StageProgress::Info(i).cave) continue;
      if (first < 0) first = i;
      last = i;
    }
    if (first < 0) return;
    const float x0 = NodePos(first).x - 78.0f, x1 = NodePos(last).x + 78.0f;
    const float edge = (std::max)(1.0f, S(1.0f));

    // 洞窟の中は少し暗く
    UiBox(P(x0, kRoofBase), P(x1, kFloorBase), {0.0f, 0.0f, 0.0f, 0.28f});

    // 天井と床の帯
    UiBox(P(x0, kRoofT), P(x1, kRoofBase), kRock);
    UiBox(P(x0, kFloorBase), P(x1, kFloorB), kRock);

    // ギザギザの牙（天井は下向き、床は上向き）
    const int teeth = 18;
    const float w = (x1 - x0) / teeth;
    for (int k = 0; k < teeth; ++k) {
      const float a = x0 + w * k, c = a + w, m = (a + c) * 0.5f;
      const float drop = kToothMax * (0.4f + 0.6f * std::fabs(std::sin(k * 2.3f)));
      UiTriangle(P(a, kRoofBase), P(m, kRoofBase + drop), P(c, kRoofBase), kRock);
      UiLine(P(a, kRoofBase), P(m, kRoofBase + drop), WithAlpha(kRockEdge, 0.7f), edge);
      UiLine(P(m, kRoofBase + drop), P(c, kRoofBase), WithAlpha(kRockEdge, 0.7f), edge);
      const float rise = kToothMax * (0.4f + 0.6f * std::fabs(std::cos(k * 1.7f)));
      UiTriangle(P(a, kFloorBase), P(c, kFloorBase), P(m, kFloorBase - rise), kRock);
      UiLine(P(a, kFloorBase), P(m, kFloorBase - rise), WithAlpha(kRockEdge, 0.7f), edge);
      UiLine(P(m, kFloorBase - rise), P(c, kFloorBase), WithAlpha(kRockEdge, 0.7f), edge);
    }

    // 口（左右の縁）
    const float th = (std::max)(1.0f, S(2.0f));
    UiLine(P(x0, kRoofT), P(x0, kRoofBase), WithAlpha(kRockEdge, 0.9f), th);
    UiLine(P(x1, kRoofT), P(x1, kRoofBase), WithAlpha(kRockEdge, 0.9f), th);
    UiLine(P(x0, kFloorBase), P(x0, kFloorB), WithAlpha(kRockEdge, 0.9f), th);
    UiLine(P(x1, kFloorBase), P(x1, kFloorB), WithAlpha(kRockEdge, 0.9f), th);

    // 「洞窟」の文字は天井の岩の帯の中に置く（面の名前の列とは別の段）
    const float labelScale = 0.75f;
    const float ly = Y((kRoofT + kRoofBase) * 0.5f) - LineH(labelFont_, labelScale) * 0.5f;
    Text(labelFont_, "洞窟  CAVE", X((x0 + x1) * 0.5f), ly, WithAlpha(kMuted, 0.9f), TextAlign::Center,
         labelScale);
  }

  /// @brief 錠前（未解放の面）
  void DrawLock(const RC::Vector2 &c) const {
    const RC::Vector4 col = WithAlpha(kDim, 1.0f);
    const float bw = 11.0f, bh = 9.0f;
    UiBox(P(c.x - bw, c.y - 1.0f), P(c.x + bw, c.y - 1.0f + bh * 2.0f), col);
    UiCircle(P(c.x, c.y - 1.0f), S(8.0f), col, kWire);
    UiCircle(P(c.x, c.y + 7.0f), S(2.5f), {0.08f, 0.09f, 0.11f, 1.0f});
  }

  /// @brief 選択中の面の情報
  void DrawInfo() const {
    DrawFrame(kInfoL, kInfoT, kInfoR, kInfoB);
    const StageProgress &prog = StageProgress::Get();
    const StageProgress::StageInfo &info = StageProgress::Info(selected_);
    const bool unlocked = prog.IsUnlocked(selected_);
    const float left = X(kInfoL + 44.0f), right = X(kInfoR - 44.0f);

    // 第 N 面
    const std::string num = "第" + std::to_string(selected_ + 1) + "面";
    Text(labelFont_, num, left, Y(kInfoT + 22.0f), accentColor);

    // 名前 ＋ 欧文
    const float nameY = Y(kInfoT + 50.0f);
    Text(nameFont_, unlocked ? info.title : "未解放の海域", left, nameY, unlocked ? kInk : kDim);
    const float nameW = TextW(nameFont_, unlocked ? info.title : "未解放の海域");
    Text(labelFont_, unlocked ? info.subtitle : "LOCKED", left + nameW + S(16.0f),
         nameY + LineH(nameFont_) - LineH(labelFont_) - S(6.0f), WithAlpha(kMuted, 0.8f));

    // 環境タグ（水上／水中・外洋／洞窟）
    {
      float tx = left;
      const float ty = Y(kInfoT + 112.0f);
      auto tag = [&](const std::string &label, const RC::Vector4 &col) {
        const float w = TextW(labelFont_, label, 0.85f) + S(24.0f);
        const float h = LineH(labelFont_, 0.85f) + S(8.0f);
        UiBox({tx, ty}, {tx + w, ty + h}, WithAlpha(col, 0.25f));
        UiBox({tx, ty}, {tx + w, ty + h}, WithAlpha(col, 0.9f), kWire);
        Text(labelFont_, label, tx + w * 0.5f, ty + S(4.0f), kInk, TextAlign::Center, 0.85f);
        tx += w + S(12.0f);
      };
      // 水上／水中・外洋／洞窟は面の途中で入れ替わることがあるので、StageInfo の文言をそのまま出す
      tag(info.waterText, info.underwater ? RC::Vector4{0.20f, 0.45f, 0.85f, 1.0f}
                                          : RC::Vector4{0.42f, 0.76f, 0.88f, 1.0f});
      tag(info.placeText, info.cave ? kRockEdge : RC::Vector4{0.55f, 0.75f, 0.55f, 1.0f});
    }

    // 右側：状態と最高得点
    const float ry = Y(kInfoT + 26.0f);
    if (!unlocked) {
      const std::string need = "第" + std::to_string(selected_) + "面をクリアで解放";
      Text(labelFont_, need, right, ry, WithAlpha(kShu, 0.95f), TextAlign::Right);
    } else if (prog.IsCleared(selected_)) {
      Text(labelFont_, "踏破済み", right, ry, kGold, TextAlign::Right);
      Text(labelFont_, "最高得点", right, Y(kInfoT + 70.0f), kMuted, TextAlign::Right, 0.85f);
      Text(nameFont_, std::to_string(prog.BestScore(selected_)), right, Y(kInfoT + 96.0f), kInk,
           TextAlign::Right);
    } else {
      Text(labelFont_, "未踏破", right, ry, kMuted, TextAlign::Right);
    }
  }

  void DrawHint() const {
    const char *text = "←→ 選択　　SPACE / Enter 出航　　ESC タイトルへ";
    Text(labelFont_, text, X(kDesignW * 0.5f), Y(kInfoB + 30.0f), WithAlpha(kMuted, 0.85f),
         TextAlign::Center, 0.85f);
  }

  // ------------------------------------------------------------------
  // 入力
  // ------------------------------------------------------------------

  void HandleInput() {
    SceneContext *ctx = GetSceneContext();
    if (!ctx || !ctx->input) return;
    Input *in = ctx->input;

    bool confirm = in->IsKeyTrigger(DIK_SPACE) || in->IsKeyTrigger(DIK_RETURN);
    bool back = in->IsKeyTrigger(DIK_ESCAPE);
    int move = 0;
    if (in->IsKeyTrigger(DIK_LEFT) || in->IsKeyTrigger(DIK_A)) move -= 1;
    if (in->IsKeyTrigger(DIK_RIGHT) || in->IsKeyTrigger(DIK_D)) move += 1;
    if (in->IsXInputConnected()) {
      if (in->IsXInputButtonTrigger(XINPUT_GAMEPAD_A)) confirm = true;
      if (in->IsXInputButtonTrigger(XINPUT_GAMEPAD_B)) back = true;
      if (in->IsXInputButtonTrigger(XINPUT_GAMEPAD_DPAD_LEFT)) move -= 1;
      if (in->IsXInputButtonTrigger(XINPUT_GAMEPAD_DPAD_RIGHT)) move += 1;
      const SHORT lx = in->GetXInputThumbLX();
      int stickDir = 0;
      if (lx > kStickThreshold) stickDir = 1;
      else if (lx < -kStickThreshold) stickDir = -1;
      if (stickDir != 0 && stickDir != prevStickDir_) move += stickDir;
      prevStickDir_ = stickDir;
    }

    // マウス：動かしたときだけホバーで選択を移す
    float mx = lastMouseX_, my = lastMouseY_;
    in->GetGameMousePosition(mx, my);
    const bool mouseMoved = (std::fabs(mx - lastMouseX_) > 0.5f || std::fabs(my - lastMouseY_) > 0.5f);
    lastMouseX_ = mx;
    lastMouseY_ = my;
    const int hovered = HitTestNodes(mx, my);
    const StageProgress &prog = StageProgress::Get();
    if (mouseMoved && hovered >= 0 && hovered != selected_ && prog.IsUnlocked(hovered)) {
      SetSelected(hovered);
      move = 0;
    }
    if (in->IsMouseTrigger(0) && hovered >= 0) {
      if (prog.IsUnlocked(hovered)) {
        SetSelected(hovered);
        confirm = true;
      } else {
        shake_ = 1.0f;
      }
    }

    if (move != 0) {
      const int next = std::clamp(selected_ + move, 0, StageProgress::kStageCount - 1);
      if (next != selected_) {
        if (prog.IsUnlocked(next)) SetSelected(next);
        else shake_ = 1.0f; // 未解放の面へは進めない
      }
    }

    if (ctx && !ctx->isPlaying()) return; // 編集モードでは遷移しない

    if (back) {
      // 遷移表の演出が "dive" なら、出航と同じく UI を消して水へ飛び込んでから戻る
      // （Title 側は Dive 遷移で入ってくると深海から浮上して始まる）
      std::string backTarget;
      std::string backTransition;
      if (dive.enabled && LookupTransition(kTriggerBack, backTarget, backTransition) &&
          !backTarget.empty() && backTransition == SceneTransitions::kDive) {
        decided_ = true;
        leaving_ = true;
        leaveTarget_ = backTarget;
        leaveTime_ = 0.0f;
        leaveRequested_ = false;
        Log::Print("[StageSelectScript] back -> dive -> " + backTarget);
        return;
      }
      if (RequestTransition(kTriggerBack)) {
        decided_ = true;
        Log::Print("[StageSelectScript] back");
      }
      return;
    }
    if (!confirm) return;
    if (!prog.IsUnlocked(selected_)) {
      shake_ = 1.0f;
      return;
    }
    StageProgress::Get().SetLastSelected(selected_);
    // 行き先と演出は遷移表で引く（遷移先が $arg なら選んだステージのシーン名になる）
    std::string target;
    std::string transition;
    if (!LookupTransition(kTriggerStage, target, transition, StageProgress::Info(selected_).sceneName) ||
        target.empty()) {
      Log::Print("[StageSelectScript] scene flow has no target for 'stage'");
      return;
    }
    if (dive.enabled && transition == SceneTransitions::kDive) {
      decided_ = true;
      leaving_ = true;
      leaveTarget_ = target;
      leaveTime_ = 0.0f;
      leaveRequested_ = false;
      Log::Print("[StageSelectScript] start -> dive -> " + target);
      return;
    }
    if (RequestSceneChange(target)) {
      decided_ = true;
      Log::Print("[StageSelectScript] start -> " + target);
    } else {
      Log::Print("[StageSelectScript] scene change refused: " + target);
    }
  }

  int HitTestNodes(float mx, float my) const {
    for (int i = 0; i < StageProgress::kStageCount; ++i) {
      const RC::Vector2 c = NodePos(i);
      const float dx = mx - X(c.x), dy = my - Y(c.y);
      const float r = S(kNodeR + 10.0f);
      if (dx * dx + dy * dy <= r * r) return i;
    }
    return -1;
  }

  void SetSelected(int index) {
    if (index == selected_) return;
    selected_ = index;
    bump_ = 1.0f;
  }

  // ------------------------------------------------------------------
  // 出航：UI フェード → 飛び込み → 遷移（Result と同じ流れ）
  // ------------------------------------------------------------------

  void UpdateLeave(float dt) {
    leaveTime_ += dt;
    if (!dive_.IsActive()) {
      uiAlpha_ = 1.0f - WaterCameraFx::Smooth01(leaveTime_ / (std::max)(uiFadeTime, 1e-3f));
      if (uiAlpha_ > 0.0f) return;
      uiAlpha_ = 0.0f;
      Scene *scene = GetScene();
      if (!dive_.Begin(WaterCameraFx::FindMainCamera(scene), WaterCameraFx::FindWaterY(scene), dive,
                       look, &bubbles_)) {
        Log::Print("[StageSelectScript] dive could not start, changing scene directly");
        leaving_ = false;
        if (!RequestSceneChange(leaveTarget_)) CancelLeave();
      }
      return;
    }
    if (!dive_.Update(dt)) {
      dive_.End(false);
      leaving_ = false;
      if (!RequestSceneChange(leaveTarget_)) CancelLeave();
      return;
    }
    if (!dive_.IsDone() || leaveRequested_) return;
    leaveRequested_ = RequestSceneChange(leaveTarget_, SceneTransitions::kDive);
    if (!leaveRequested_ && dive_.DoneTime() > kLeaveRetrySeconds) {
      Log::Print("[StageSelectScript] scene change kept failing, giving up: " + leaveTarget_);
      dive_.End(/*restoreCamera=*/true);
      CancelLeave();
    }
  }

  void CancelLeave() {
    leaving_ = false;
    leaveRequested_ = false;
    decided_ = false;
    uiAlpha_ = 1.0f;
  }

  // UI 描画のラッパー（フェードで全体の不透明度を下げる）
  RC::Vector4 Ui(const RC::Vector4 &c) const { return {c.x, c.y, c.z, c.w * uiAlpha_}; }
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

  // ------------------------------------------------------------------
  // 状態
  // ------------------------------------------------------------------
  int headingFont_ = -1;
  int nameFont_ = -1;
  int nodeFont_ = -1;
  int labelFont_ = -1;
  float fontScale_ = 1.0f;

  int selected_ = 0;
  int highlight_ = -1; ///< 今回新しく解放された面（金の輪で知らせる）
  float time_ = 0.0f;
  float bump_ = 0.0f;
  float shake_ = 0.0f;
  bool decided_ = false;
  int prevStickDir_ = 0;
  float lastMouseX_ = 0.0f;
  float lastMouseY_ = 0.0f;

  bool leaving_ = false;
  std::string leaveTarget_;
  static constexpr const char *kTriggerBack = "back";   ///< 遷移表のきっかけ名（ESC）
  static constexpr const char *kTriggerStage = "stage"; ///< 遷移表のきっかけ名（出航）
  float leaveTime_ = 0.0f;
  bool leaveRequested_ = false;
  float uiAlpha_ = 1.0f;
  float uiFadeIn_ = -1.0f;
  WaterCameraFx::DiveSequence dive_;
  WaterCameraFx::RiseSequence rise_;
  WaterCameraFx::Bubbles bubbles_;
};

REGISTER_SCRIPT(StageSelectScript)
