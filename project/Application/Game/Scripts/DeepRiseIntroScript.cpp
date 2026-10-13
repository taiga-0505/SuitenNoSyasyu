#include "ECS/ScriptableEntity.h"
#include "ECS/ScriptRegistry.h"
#include "ECS/TransformComponent.h"
#include "ECS/CameraComponent.h"
#include "ECS/WaterComponent.h"
#include "ECS/GPUParticleComponent.h"
#include "Particle/GPUParticle.h"
#include "Common/Math/MathUtils.h"
#include "Common/Log/Log.h"
#include "RenderCommon.h"
#include "Engine/Render/RenderContext.h"
#include "Framework/App.h"
#include "Game/Framework/GameModeBase.h"
#include "Game/Framework/StageProgress.h"
#include "Scene.h"

#if RC_ENABLE_IMGUI
#include "imgui/imgui.h"
#endif

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

/// @class DeepRiseIntroScript
/// @brief Game 開始時の「深海からゆっくり浮上して、水面に出たらゲームスタート」
/// @details
///   置き場所:
///     Camera エンティティ（RailShooterController / RailMovementScript が付いているもの）に、
///     それらより **前** に付ける。スクリプトは並び順に OnCreate → OnUpdate されるので、
///     先にカメラを深海へ置いておけば、RailShooterController は OnCreate の時点で
///     「最初から水中」と判断して、着水の演出なしに静かに水中エフェクトを積む。
///
///   いつ再生するか:
///     タイトルから "dive" 遷移（TitleScreenScript の飛び込み）で入ったときだけ。
///     SceneContext::lastTransition が Dive のときに再生し、Debug 起動で直接 Game に
///     入ったときやリザルトからのリトライは今までどおり即スタートする。
///     alwaysPlay を true にすると入り方に関係なく毎回再生する（調整用）。
///
///   流れ:
///     1. OnCreate : 開始位置から riseDepth だけ下へカメラを置き、少し見上げさせる。
///                   自エンティティに intro_playing = 1 を立てる。
///                   → RailMovementScript は動かず、RailShooterController は入力と HUD を止める。
///     2. OnUpdate : riseDuration 秒かけて元の位置・向きへ戻る（暗い深海を勢いよく抜け、
///                   水面が近づくにつれて減速し、出たあと静かに落ち着く。riseEase で調整）。
///                   深さに応じたフォグの明るさ・光の筋は RailShooterController が毎フレーム
///                   掛ける。水面を抜けた瞬間の水滴とラジアルブラーも RailShooterController の
///                   浮上演出がそのまま出る。
///     3. 完了     : intro_playing を外し、GameState の経過時間を 0 に戻し（浮上中の時間を
///                   リザルトに含めない）、GAME START を表示する。ここからレールが動く。
///
///   スクリプト間の契約（タグ）:
///     自エンティティ  intro_playing : 1 のあいだは「まだ始まっていない」。
///                                    RailMovementScript / RailShooterController が見る。
///
///   JSON (scriptDataList) で設定できる項目:
///     "alwaysPlay"      : 入り方に関係なく毎回再生する（既定 false）
///     "riseDepth"       : 開始位置から何 m 下から浮上するか（既定 26。水面より深くすること）
///     "riseDuration"    : 浮上にかける秒数（既定 3.2）
///     "riseEase"        : 浮上の速度カーブ。進行 = 1 - (1 - t)^riseEase。1 で等速、大きいほど
///                         「最初は速く、水面の手前で減速」が強くなる（既定 2.2。水面を抜けるのは
///                         全体の 7 割ほどの時点で、残りは水上で静かに落ち着く）
///     "risePitchStart"  : 浮上開始時に見上げる角度（rad。負で上向き。既定 -0.35）
///     "bubbles"         : 浮上中に泡（GPU パーティクル）を流すか（既定 true）
///     "showStartText"   : 水面に出たら GAME START を出すか（既定 true）
///     "startText"       : 表示する文字列（既定 "GAME START"）
///     "startTextDuration": 表示時間（秒。既定 1.4）
///     "startBandAlpha"  : 文字の後ろに敷く画面幅の暗い帯の濃さ（既定 0.72。0 で帯なし）
///     "startFontPath" / "startFontSize" : フォントとサイズ（px）
///     "showStageCard"   : 浮上の序盤、暗い深海にステージ名を浮かび上がらせるか（既定 true）。
///                         ステージ 1〜5（StageProgress に載っているシーン）のときだけ出る。
///     "cardDelay" / "cardFadeIn" / "cardHold" / "cardFadeOut" : ステージ名の出方（秒）。
///                         既定の合計 2.15 秒は、水面を抜ける（浮上全体の 7 割 ≒ 2.2 秒）前に消えきる長さ。
class DeepRiseIntroScript : public ScriptableEntity {
public:
  bool alwaysPlay = false;
  float riseDepth = 26.0f;
  float riseDuration = 3.2f;
  float riseEase = 2.2f;
  float risePitchStart = -0.35f;
  bool bubbles = true;
  bool showStartText = true;
  std::string startText = "GAME START";
  float startTextDuration = 1.4f;
  float startBandAlpha = 0.72f; ///< 文字の後ろに敷く画面幅の暗い帯の濃さ（0 で無し）
  std::string startFontPath = "Resources/fonts/Kiwi_Maru/KiwiMaru-Medium.ttf";
  float startFontSize = 72.0f;
  bool showStageCard = true;
  float cardDelay = 0.2f;   ///< 浮上が始まってから出始めるまで
  float cardFadeIn = 0.6f;  ///< 下から浮かび上がりながら現れる時間
  float cardHold = 0.9f;    ///< 出きったまま見せる時間
  float cardFadeOut = 0.45f;///< 上へ抜けながら消える時間

  nlohmann::json Serialize() override {
    return {
        {"alwaysPlay", alwaysPlay},
        {"riseDepth", riseDepth},
        {"riseDuration", riseDuration},
        {"riseEase", riseEase},
        {"risePitchStart", risePitchStart},
        {"bubbles", bubbles},
        {"showStartText", showStartText},
        {"startText", startText},
        {"startTextDuration", startTextDuration},
        {"startBandAlpha", startBandAlpha},
        {"startFontPath", startFontPath},
        {"startFontSize", startFontSize},
        {"showStageCard", showStageCard},
        {"cardDelay", cardDelay},
        {"cardFadeIn", cardFadeIn},
        {"cardHold", cardHold},
        {"cardFadeOut", cardFadeOut},
    };
  }

  void Deserialize(const nlohmann::json &j) override {
    auto readF = [&](const char *key, float &out) {
      if (j.contains(key) && j[key].is_number()) out = j[key].get<float>();
    };
    auto readB = [&](const char *key, bool &out) {
      if (j.contains(key) && j[key].is_boolean()) out = j[key].get<bool>();
    };
    auto readS = [&](const char *key, std::string &out) {
      if (j.contains(key) && j[key].is_string()) out = j[key].get<std::string>();
    };
    readB("alwaysPlay", alwaysPlay);
    readF("riseDepth", riseDepth);
    readF("riseDuration", riseDuration);
    readF("riseEase", riseEase);
    readF("risePitchStart", risePitchStart);
    readB("bubbles", bubbles);
    readB("showStartText", showStartText);
    readS("startText", startText);
    readF("startTextDuration", startTextDuration);
    readF("startBandAlpha", startBandAlpha);
    readS("startFontPath", startFontPath);
    readF("startFontSize", startFontSize);
    readB("showStageCard", showStageCard);
    readF("cardDelay", cardDelay);
    readF("cardFadeIn", cardFadeIn);
    readF("cardHold", cardHold);
    readF("cardFadeOut", cardFadeOut);
  }

#if RC_ENABLE_IMGUI
  void OnImGui() override {
    ImGui::Text("State: %s  t=%.2f / %.2f", playing_ ? "rising" : (finished_ ? "finished" : "idle"),
                t_, riseDuration);
    ImGui::Checkbox("Always Play", &alwaysPlay);
    ImGui::DragFloat("Rise Depth (m)", &riseDepth, 0.5f, 1.0f, 200.0f);
    ImGui::DragFloat("Rise Duration (s)", &riseDuration, 0.05f, 0.2f, 20.0f);
    ImGui::DragFloat("Rise Ease", &riseEase, 0.05f, 1.0f, 5.0f);
    ImGui::DragFloat("Pitch Start (rad)", &risePitchStart, 0.01f, -1.5f, 1.5f);
    ImGui::Checkbox("Bubbles", &bubbles);
    ImGui::Checkbox("Show Start Text", &showStartText);
    ImGui::DragFloat("Start Text Duration", &startTextDuration, 0.05f, 0.2f, 5.0f);
    ImGui::SliderFloat("Start Band Alpha", &startBandAlpha, 0.0f, 1.0f);
    ImGui::SeparatorText("Stage Card");
    ImGui::Text("stage index: %d  t=%.2f", cardStage_, cardTime_);
    ImGui::Checkbox("Show Stage Card", &showStageCard);
    ImGui::DragFloat("Card Delay (s)", &cardDelay, 0.01f, 0.0f, 3.0f);
    ImGui::DragFloat("Card Fade In (s)", &cardFadeIn, 0.01f, 0.05f, 3.0f);
    ImGui::DragFloat("Card Hold (s)", &cardHold, 0.01f, 0.0f, 5.0f);
    ImGui::DragFloat("Card Fade Out (s)", &cardFadeOut, 0.01f, 0.05f, 3.0f);
    if (ImGui::Button("Replay")) {
      // 元の位置に戻してからやり直す（レールが動いたあとなら現在地からの浮上になる）
      if (playing_) RestoreCamera();
      Begin();
    }
  }
#endif

protected:
  void OnCreate() override {
    if (showStartText) {
      startFont_ = RC::LoadFont(startFontPath, startFontSize);
      if (startFont_ < 0) {
        Log::Print("[DeepRiseIntroScript] failed to load font: " + startFontPath);
      }
    }

    // ステージ名カード：ステージ 1〜5 のときだけフォントを用意する（テスト用 Game では出さない）
    cardStage_ = -1;
    if (Scene *scene = GetScene()) cardStage_ = StageProgress::IndexOf(scene->Name());
    if (showStageCard && cardStage_ >= 0) LoadCardFonts();

    bool play = alwaysPlay;
    if (SceneContext *ctx = GetSceneContext()) {
      play = play || (ctx->lastTransition == SceneTransition::Dive);
      // 編集モード（Stopped）でもスクリプトは作られる。そこで再生すると、カメラを
      // 深海へ動かした状態やタグがエディタの保存で JSON に焼き付くので、再生中だけ始める
      if (!ctx->isPlaying()) play = false;
    }
    if (play) {
      Begin();
    } else {
      // 再生しないときも、前回のプレイで保存されたタグが残っていると
      // レールが永遠に動かないので必ず消す
      if (Entity *self = GetEntity()) self->ClearTag(kTagIntroPlaying);
    }
  }

  void OnUpdate(float deltaTime) override {
    // ポーズ中はカードを隠す（オーバーレイはポーズメニューより手前に出てしまうため）
    paused_ = (deltaTime <= 0.0f);
    if (deltaTime <= 0.0f) return;

    if (playing_) {
      UpdateRise(deltaTime);
    }
    if (cardActive_) {
      cardTime_ += deltaTime;
      if (cardTime_ >= CardTotal()) cardActive_ = false;
    }
    if (startTextTimer_ > 0.0f) {
      startTextTimer_ -= deltaTime;
      if (startTextTimer_ <= 0.0f) ClearOutlineExclusion();
    }
  }

  void OnDestroy() override {
    ClearOutlineExclusion();
    if (Entity *self = GetEntity()) self->ClearTag(kTagIntroPlaying);
    if (auto e = bubbles_.lock()) e->Destroy();
    bubbles_.reset();
    if (startFont_ >= 0) {
      RC::UnloadFont(startFont_);
      startFont_ = -1;
    }
    if (cardTitleFont_ >= 0) {
      RC::UnloadFont(cardTitleFont_);
      cardTitleFont_ = -1;
    }
    if (cardLabelFont_ >= 0) {
      RC::UnloadFont(cardLabelFont_);
      cardLabelFont_ = -1;
    }
    cardActive_ = false;
  }

  /// @brief ステージ名カード（ポストプロセスの後に描く）
  /// @details 深海では Underwater のフォグが距離で画面を塗り潰すので、OnRender（ポストプロセス前）
  ///          に描くと文字までフォグ色に沈む。オーバーレイなら輪郭線も乗らない。
  ///          レイアウトは RailShooterController の HUD と同じ 1280x720 基準（レターボックス分をずらす）。
  void OnOverlayRender() override {
    if (!cardActive_ || paused_ || cardStage_ < 0) return;
    if (cardTitleFont_ < 0 || cardLabelFont_ < 0) return;

    // ---- 時間 → 不透明度・縦の動き ----
    const float t = cardTime_ - cardDelay;
    if (t <= 0.0f) return;
    const float inEnd = cardFadeIn;
    const float outStart = cardFadeIn + cardHold;
    const float fadeIn = Smooth01(t / (std::max)(cardFadeIn, 1e-3f));
    const float fadeOut = Smooth01((t - outStart) / (std::max)(cardFadeOut, 1e-3f));
    const float alpha = fadeIn * (1.0f - fadeOut);
    if (alpha <= 0.001f) return;
    // 下から浮かび上がって止まり（泡のように）、消えるときはそのまま上へ抜ける
    const float rise = (1.0f - EaseOutPow(t / (std::max)(inEnd, 1e-3f), 3.0f)) * 18.0f - fadeOut * 10.0f;

    // ---- 1280x720 基準 → 実解像度 ----
    float screenW = kDesignW, screenH = kDesignH;
    auto &rc = RC::GetRenderContext();
    if (rc.Ctx() && rc.Ctx()->app) {
      screenW = static_cast<float>(rc.Ctx()->app->width);
      screenH = static_cast<float>(rc.Ctx()->app->height);
    }
    float s = (std::min)(screenW / kDesignW, screenH / kDesignH);
    if (s <= 0.0f) s = 1.0f;
    const float ox = (screenW - kDesignW * s) * 0.5f;
    const float oy = (screenH - kDesignH * s) * 0.5f;
    const float fs = (cardFontScale_ > 0.0f) ? (s / cardFontScale_) : 1.0f;
    auto P = [&](float x, float y) { return RC::Vector2{ox + x * s, oy + y * s}; };

    const StageProgress::StageInfo &info = StageProgress::Info(cardStage_);
    const std::string label = "STAGE " + std::to_string(cardStage_ + 1) + "  /  " + info.subtitle;
    const std::string title = info.title;

    const float labelH = RC::GetFontLineHeight(cardLabelFont_, fs) / s;
    const float titleH = RC::GetFontLineHeight(cardTitleFont_, fs) / s;
    const float gap = 6.0f;
    const float cx = kDesignW * 0.5f;
    const float top = kDesignH * 0.44f - (labelH + gap + titleH) * 0.5f + rise;

    // 添え書き（STAGE n / 欧文名）：淡い水色
    RC::DrawString(cardLabelFont_, label, P(cx, top), {0.62f, 0.86f, 1.0f, 0.85f * alpha}, fs,
                   TextAlign::Center);

    // ステージ名：周りに水色のにじみを 4 方向に敷いてから本体を重ねる（暗い水中で光って見える）
    const float titleY = top + labelH + gap;
    const float glow = 2.5f;
    const RC::Vector4 glowColor = {0.35f, 0.75f, 1.0f, 0.22f * alpha};
    RC::DrawString(cardTitleFont_, title, P(cx - glow, titleY), glowColor, fs, TextAlign::Center);
    RC::DrawString(cardTitleFont_, title, P(cx + glow, titleY), glowColor, fs, TextAlign::Center);
    RC::DrawString(cardTitleFont_, title, P(cx, titleY - glow), glowColor, fs, TextAlign::Center);
    RC::DrawString(cardTitleFont_, title, P(cx, titleY + glow), glowColor, fs, TextAlign::Center);
    RC::DrawString(cardTitleFont_, title, P(cx, titleY), {0.96f, 0.99f, 1.0f, alpha}, fs,
                   TextAlign::Center);

    // 下線：中央から左右へ伸びる細い光の線
    const float lineY = titleY + titleH + 8.0f;
    const float halfW = 190.0f * EaseOutPow(t / (std::max)(inEnd + 0.25f, 1e-3f), 3.0f);
    if (halfW > 0.5f) {
      const RC::Vector2 a = P(cx - halfW, lineY);
      const RC::Vector2 b = P(cx + halfW, lineY + 1.5f);
      RC::DrawBox(a, b, {0.55f, 0.85f, 1.0f, 0.7f * alpha});
    }
  }

  void OnRender() override {
    if (startTextTimer_ <= 0.0f || startFont_ < 0) return;

    float screenW = 1280.0f;
    float screenH = 720.0f;
    auto &rc = RC::GetRenderContext();
    if (rc.Ctx() && rc.Ctx()->app) {
      screenW = static_cast<float>(rc.Ctx()->app->width);
      screenH = static_cast<float>(rc.Ctx()->app->height);
    }

    // 出た瞬間に少し大きく → すぐ等倍、終わりは薄れて消える
    const float elapsed = startTextDuration - startTextTimer_;
    const float pop = 1.0f + 0.25f * (1.0f - Smooth01(elapsed / 0.18f));
    const float fadeIn = Smooth01(elapsed / 0.12f);
    const float fadeOut = 1.0f - Smooth01((elapsed - startTextDuration * 0.7f) /
                                          (startTextDuration * 0.3f));
    const float alpha = fadeIn * fadeOut;

    const float lineH = RC::GetFontLineHeight(startFont_, pop);
    const RC::Vector2 pos{screenW * 0.5f, screenH * 0.42f - lineH * 0.5f};

    // DepthBasedOutline は 2D まで描き終えた画に「深度の段差」で暗い線を乗せるので、
    // 岩の縁（特に斜めに見える側面）の上に文字を置くと文字が塗り潰される。
    // 除外矩形（SetOutlineExclusions）で素通しにするだけだと、矩形の中だけ縁が消えて
    // 透明な板が貼り付いたように見える。そこで画面幅いっぱいの暗い帯を敷き、
    // その帯の矩形をそのまま除外矩形にする。帯の中は元々暗くなるので継ぎ目が目立たない。
    const float bandPad = lineH * 0.35f;
    const float bandTop = pos.y - bandPad;
    const float bandBottom = pos.y + lineH + bandPad;
    if (startBandAlpha > 0.0f) {
      RC::DrawBox({0.0f, bandTop}, {screenW, bandBottom},
                  {0.02f, 0.05f, 0.10f, startBandAlpha * alpha});
    }

    RC::DrawString(startFont_, startText, {pos.x + 3.0f, pos.y + 3.0f},
                   {0.0f, 0.03f, 0.08f, 0.65f * alpha}, pop, TextAlign::Center);
    RC::DrawString(startFont_, startText, pos, {0.97f, 0.98f, 1.0f, alpha}, pop,
                   TextAlign::Center);

    if (auto *pp = rc.GetPostProcess()) {
      const float rect[4] = {0.0f, bandTop, screenW, bandBottom};
      pp->SetOutlineExclusions(rect, 1);
      outlineExcluded_ = true;
    }
  }

private:
  static constexpr const char *kTagIntroPlaying = "intro_playing";
  static constexpr uint32_t kBubbleEmitPerFrame = 4;
  /// @brief 水面からこの深さ（m）より浅くなったら泡の射出を止める
  /// @details GPU パーティクルは水面で消えないので、寿命いっぱい昇っても水面に届かない
  ///          深さで止める（最長寿命 2.4s × 2.4m/s ≒ 6m ＋ 余裕）。
  static constexpr float kBubbleStopDepth = 8.0f;

  // ---- ステージ名カード（1280x720 基準）----
  static constexpr float kDesignW = 1280.0f;
  static constexpr float kDesignH = 720.0f;
  static constexpr float kCardTitlePx = 64.0f; ///< ステージ名の文字サイズ
  static constexpr float kCardLabelPx = 22.0f; ///< 「STAGE n / 欧文名」の文字サイズ

  float CardTotal() const { return cardDelay + cardFadeIn + cardHold + cardFadeOut; }

  /// @brief カード用のフォントを実解像度に合わせた大きさでロードする（拡大でぼやけないように）
  void LoadCardFonts() {
    float w = kDesignW, h = kDesignH;
    auto &rc = RC::GetRenderContext();
    if (rc.Ctx() && rc.Ctx()->app && rc.Ctx()->app->width > 0 && rc.Ctx()->app->height > 0) {
      w = static_cast<float>(rc.Ctx()->app->width);
      h = static_cast<float>(rc.Ctx()->app->height);
    }
    cardFontScale_ = (std::min)(w / kDesignW, h / kDesignH);
    if (cardFontScale_ <= 0.0f) cardFontScale_ = 1.0f;
    // ステージ名は日本語（漢字）を含むので、アトラスは広めに取る
    cardTitleFont_ = RC::LoadFont(startFontPath, std::round(kCardTitlePx * cardFontScale_), 2048);
    cardLabelFont_ = RC::LoadFont(startFontPath, std::round(kCardLabelPx * cardFontScale_), 1024);
    if (cardTitleFont_ < 0 || cardLabelFont_ < 0) {
      Log::Print("[DeepRiseIntroScript] failed to load stage card font: " + startFontPath);
    }
  }

  static float Smooth01(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
  }

  /// @brief 最初は速く、終わりに向かって減速する（1 - (1-u)^k。k=1 で等速）
  /// @details 深海は真っ暗で見せるものが無いので、そこを長引かせないためにこの形。
  ///          水面に近づくほど遅くなるので、明るくなっていく過程はゆっくり見える。
  static float EaseOutPow(float u, float k) {
    u = std::clamp(u, 0.0f, 1.0f);
    k = (std::max)(k, 0.1f);
    return 1.0f - std::pow(1.0f - u, k);
  }

  void Begin() {
    auto *tr = GetComponent<TransformComponent>();
    if (!tr) return;

    goalPos_ = tr->position;
    goalRot_ = tr->rotation;
    startPos_ = {goalPos_.x, goalPos_.y - riseDepth, goalPos_.z};
    startRot_ = {risePitchStart, goalRot_.y, goalRot_.z};
    t_ = 0.0f;
    playing_ = true;
    finished_ = false;

    tr->position = startPos_;
    tr->rotation = startRot_;

    if (Entity *self = GetEntity()) self->SetTag(kTagIntroPlaying, 1);

    // 水面の高さはシーンの WaterComponent から（無ければ 0）
    waterY_ = 0.0f;
    if (Scene *scene = GetScene()) {
      for (const auto &e : scene->GetEntities()) {
        if (!e || !e->GetComponent<WaterComponent>()) continue;
        if (auto *wtr = e->GetComponent<TransformComponent>()) waterY_ = wtr->position.y;
        break;
      }
    }

    // ステージ名カード（フォントが用意できていて、ステージ 1〜5 のときだけ）
    cardTime_ = 0.0f;
    cardActive_ = showStageCard && cardStage_ >= 0 && cardTitleFont_ >= 0 && cardLabelFont_ >= 0;

    bubbleEmitting_ = false;
    if (bubbles && (waterY_ - startPos_.y) >= kBubbleStopDepth) {
      if (bubbles_.expired()) SpawnBubbleEmitter();
      SetBubbleEmission(kBubbleEmitPerFrame);
      bubbleEmitting_ = true;
    }
    Log::Print("[DeepRiseIntroScript] rise begin (depth " + std::to_string(riseDepth) + "m)");
  }

  /// @brief GAME START の帯に登録した輪郭の除外矩形を外す
  void ClearOutlineExclusion() {
    if (!outlineExcluded_) return;
    outlineExcluded_ = false;
    if (auto *pp = RC::GetRenderContext().GetPostProcess()) {
      pp->SetOutlineExclusions(nullptr, 0);
    }
  }

  void RestoreCamera() {
    if (auto *tr = GetComponent<TransformComponent>()) {
      tr->position = goalPos_;
      tr->rotation = goalRot_;
    }
  }

  void UpdateRise(float dt) {
    auto *tr = GetComponent<TransformComponent>();
    if (!tr) return;

    t_ += dt;
    const float u = t_ / (std::max)(riseDuration, 1e-3f);
    const float e = EaseOutPow(u, riseEase);
    tr->position = RC::Lerp(startPos_, goalPos_, e);
    // 向きは水面へ向かう途中で水平へ戻していく（位置より少し先に落ち着く）
    const float r = Smooth01(u / 0.85f);
    tr->rotation = RC::Lerp(startRot_, goalRot_, r);

    // 泡は水中でしか出さない。GPU パーティクルは水面で消えないので、
    // 湧かせ始めから寿命ぶん昇っても水面に届かない深さまでで射出を止める
    // （最長寿命 × 上昇速度 ≒ 6m。射出点は最高でカメラの 2.5m 上）。
    const float depth = waterY_ - tr->position.y;
    if (bubbleEmitting_ && depth < kBubbleStopDepth) {
      SetBubbleEmission(0);
      bubbleEmitting_ = false;
    }
    UpdateBubbleEmitter(*tr);

    if (u >= 1.0f) {
      Finish();
    }
  }

  void Finish() {
    auto *tr = GetComponent<TransformComponent>();
    if (tr) {
      tr->position = goalPos_;
      tr->rotation = goalRot_;
    }
    playing_ = false;
    finished_ = true;
    if (Entity *self = GetEntity()) self->ClearTag(kTagIntroPlaying);
    SetBubbleEmission(0);
    bubbleEmitting_ = false;

    // 浮上していた時間をプレイ時間に含めない
    if (Scene *scene = GetScene()) {
      if (GameModeBase *gm = scene->GetGameMode()) {
        if (GameStateBase *gs = gm->GetGameState()) gs->ResetElapsedTime();
      }
    }

    if (showStartText) startTextTimer_ = startTextDuration;
    Log::Print("[DeepRiseIntroScript] surfaced. GAME START");
  }

  // ---- 泡（GPU パーティクル）----

  void SpawnBubbleEmitter() {
    Scene *scene = GetScene();
    if (!scene) return;
    auto e = scene->CreateEntity("RiseBubbles");
    auto &etr = e->AddComponent<TransformComponent>();
    etr.position = startPos_;
    auto &gpu = e->AddComponent<GPUParticleComponent>();
    if (auto *ps = gpu.particleSystem.get()) {
      ps->SetPipelinePrefix("gpu_particle_bubble");
      ps->SetParticleType(ParticleType::Default);
      ps->SetBlendMode(kBlendModeNormal);
      ps->SetMaxParticles(1024);
      ps->SetEmitCount(0);
      ps->emitterShape_ = EmitterShape::Box;
      // 奥行きを薄くしてカメラの至近に湧かないようにする（至近の泡は画面いっぱいの玉になる）
      ps->shapeBoxSize_ = {12.0f, 5.0f, 6.0f};
      // 速度は 1 フレームあたりの移動量（UpdateParticle.CS は translate += velocity）
      ps->baseVelocity_ = {0.0f, 0.04f, 0.0f}; // ≒ 2.4 m/s で上昇
      ps->velocityVariance_ = 0.015f;
      ps->gravity_ = 0.0f;
      // 寿命 × 上昇速度 が kBubbleStopDepth を超えないように（水面から泡が飛び出さない）
      ps->minLifeTime_ = 1.2f;
      ps->maxLifeTime_ = 2.4f;
      ps->minScale_ = 0.06f;
      ps->maxScale_ = 0.2f;
      ps->startColor_ = {0.85f, 0.95f, 1.0f, 0.7f};
      ps->endColor_ = {0.85f, 0.95f, 1.0f, 0.0f};
    }
    scene->InitDynamicEntityRuntime(*e);
    bubbles_ = e;
  }

  void SetBubbleEmission(uint32_t perFrame) {
    auto e = bubbles_.lock();
    if (!e) return;
    if (auto *gpu = e->GetComponent<GPUParticleComponent>()) {
      if (gpu->particleSystem) gpu->particleSystem->SetEmitCount(perFrame);
    }
  }

  void UpdateBubbleEmitter(const TransformComponent &camTr) {
    auto e = bubbles_.lock();
    if (!e) return;
    auto *tr = e->GetComponent<TransformComponent>();
    if (!tr) return;
    // カメラの向いている方向（rotation.x = 0 で +Z、負で見上げ）の 10m 先、少し下から湧かせる。
    // 射出点の上端が水面へ近づきすぎないよう、箱の上端は水面より kBubbleStopDepth 下に抑える
    const float rx = camTr.rotation.x;
    const RC::Vector3 fwd = {0.0f, -std::sin(rx), std::cos(rx)};
    float y = camTr.position.y + fwd.y * 10.0f - 3.0f;
    const float boxHalfH = 2.5f;
    y = (std::min)(y, waterY_ - kBubbleStopDepth - boxHalfH);
    tr->position = {camTr.position.x + fwd.x * 10.0f, y, camTr.position.z + fwd.z * 10.0f};
  }

  // ---- 状態 ----
  bool playing_ = false;
  bool finished_ = false;
  float t_ = 0.0f;
  RC::Vector3 startPos_{};
  RC::Vector3 goalPos_{};
  RC::Vector3 startRot_{};
  RC::Vector3 goalRot_{};
  std::weak_ptr<Entity> bubbles_;
  bool bubbleEmitting_ = false;
  float waterY_ = 0.0f;
  int startFont_ = -1;
  float startTextTimer_ = 0.0f;
  bool outlineExcluded_ = false;

  // ステージ名カード
  int cardStage_ = -1;        ///< ステージ番号（0 始まり。ステージでなければ -1）
  bool cardActive_ = false;
  float cardTime_ = 0.0f;     ///< 浮上開始からの経過（ポーズ・遷移の幕の間は進まない）
  bool paused_ = false;
  int cardTitleFont_ = -1;
  int cardLabelFont_ = -1;
  float cardFontScale_ = 1.0f;
};

REGISTER_SCRIPT(DeepRiseIntroScript)
