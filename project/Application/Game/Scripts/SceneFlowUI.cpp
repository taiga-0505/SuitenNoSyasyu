#include "ECS/ScriptableEntity.h"
#include "ECS/ScriptRegistry.h"
#include "Input/Input.h"
#include "RenderCommon.h"
#include "Engine/Render/RenderContext.h"
#include "Framework/App.h"
#include "Game/Framework/GameSession.h"
#include "Scene.h"

#if RC_ENABLE_IMGUI
#include "imgui/imgui.h"
#endif

#include <string>
#include <vector>

/// @class SceneFlowUI
/// @brief タイトル・リザルト・ゲームオーバー画面の HUD を描画するスクリプト
/// @details Title → Game → Result / GameOver → Title の導線シーンに 1 つ置いて使う。
///          シーン遷移の入力判定そのものは DataDrivenScene 側が持っているため、
///          このスクリプトは表示だけを担当する（判定が二重にならないようにするため）。
///
///          Resources/UI 以下の画像は黒い文字が透過背景に乗った素材なので、
///          そのまま暗い 3D 背景に重ねると読めない。先に明るいパネルを敷いてから
///          バナーを重ねる構成にしている。
///
///          JSON 側の scriptDataList で次を設定できる:
///            "bannerPath"  : 画面中央に敷くバナー画像（1280x720 想定。空なら描画しない）
///            "showScore"   : スコアと評価ランクを表示するか
///            "showPrompt"  : 「スペースキーで次へ」の点滅プロンプトを出すか
///            "panelColor"  : バナーの下に敷くパネルの色 (RGBA)
class SceneFlowUI : public ScriptableEntity {
public:
  // --- インスペクタ / JSON から設定する項目 ---
  std::string bannerPath;                        ///< 中央に置くバナー画像のパス
  bool showScore = false;                        ///< スコアと評価を表示するか
  bool showPrompt = true;                        ///< 続行プロンプトを点滅表示するか
  RC::Vector4 panelColor = {0.92f, 0.95f, 1.0f, 0.82f}; ///< バナー背後のパネル色

  /// @brief 設定項目を JSON に書き出す
  /// @return 設定項目を格納した JSON
  nlohmann::json Serialize() override {
    return {
        {"bannerPath", bannerPath},
        {"showScore", showScore},
        {"showPrompt", showPrompt},
        {"panelColor", {panelColor.x, panelColor.y, panelColor.z, panelColor.w}},
    };
  }

  /// @brief JSON から設定項目を読み込む
  /// @param j 読み込み元の JSON（存在するキーだけ反映する。panelColor は要素 4 つ以上の配列のときだけ）
  void Deserialize(const nlohmann::json &j) override {
    if (j.contains("bannerPath")) bannerPath = j["bannerPath"].get<std::string>();
    if (j.contains("showScore")) showScore = j["showScore"].get<bool>();
    if (j.contains("showPrompt")) showPrompt = j["showPrompt"].get<bool>();
    if (j.contains("panelColor")) {
      const auto &c = j["panelColor"];
      if (c.is_array() && c.size() >= 4) {
        panelColor = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>(),
                      c[3].get<float>()};
      }
    }
  }

#if RC_ENABLE_IMGUI
  /// @brief インスペクタに表示設定と現在のセッションのスコア・評価を表示する
  void OnImGui() override {
    ImGui::Checkbox("Show Score", &showScore);
    ImGui::Checkbox("Show Prompt", &showPrompt);
    ImGui::ColorEdit4("Panel Color", &panelColor.x);
    ImGui::TextUnformatted(bannerPath.empty() ? "(no banner)" : bannerPath.c_str());
    ImGui::Separator();
    ImGui::Text("Session score: %d", GameSession::Get().Score());
    ImGui::Text("Session rank : %c", GameSession::Get().Rank());
  }
#endif

protected:
  /// @brief 設定に応じてバナー・数字・スコアラベルのスプライトを読み込む
  void OnCreate() override {
    SceneContext *ctx = GetSceneContext();
    if (!ctx) return;

    if (!bannerPath.empty()) {
      bannerHandle_ = RC::LoadSprite(bannerPath, *ctx);
    }
    if (showScore) {
      // 桁ごとに別のスプライトハンドルを持つ。
      // Sprite2D は 1 ハンドルにつき定数バッファを 1 つしか持たず、Draw は
      // コマンドリストへの「記録」でしかない。同じハンドルを 1 フレームに
      // 複数回描くと、GPU が実行する時点では最後に書いた座標と UV しか残らず、
      // 全桁が同じ位置に同じ数字で重なってしまう。
      for (int i = 0; i < kMaxDigits; ++i) {
        digitHandles_[i] = RC::LoadSprite("Resources/UI/number.png", *ctx);
      }
      scoreLabelHandle_ = RC::LoadSprite("Resources/UI/score.png", *ctx);
    }
  }

  /// @brief プロンプト点滅用のタイマーを進める
  /// @param deltaTime 前フレームからの経過時間（秒）
  void OnUpdate(float deltaTime) override { blinkTimer_ += deltaTime; }

  /// @brief 読み込んだスプライトをすべて解放する
  void OnDestroy() override {
    if (bannerHandle_ >= 0) { RC::UnloadSprite(bannerHandle_); bannerHandle_ = -1; }
    for (int i = 0; i < kMaxDigits; ++i) {
      if (digitHandles_[i] >= 0) { RC::UnloadSprite(digitHandles_[i]); digitHandles_[i] = -1; }
    }
    if (scoreLabelHandle_ >= 0) { RC::UnloadSprite(scoreLabelHandle_); scoreLabelHandle_ = -1; }
  }

  /// @brief 画面サイズを取得し、バナー・スコア・プロンプトを設定に応じて描く
  void OnRender() override {
    // 画面サイズはウィンドウから取得し、取れなければ 1280x720 を仮定する
    float screenW = 1280.0f;
    float screenH = 720.0f;
    auto &rc = RC::GetRenderContext();
    if (rc.Ctx() && rc.Ctx()->app) {
      screenW = static_cast<float>(rc.Ctx()->app->width);
      screenH = static_cast<float>(rc.Ctx()->app->height);
    }

    DrawBanner(screenW, screenH);
    if (showScore) {
      DrawScore(screenW, screenH);
    }
    if (showPrompt) {
      DrawPrompt(screenW, screenH);
    }
  }

private:
  /// @brief 中央のバナー（Clear / GameOver など）とその下敷きパネルを描く
  /// @param screenW 画面幅（ピクセル）
  /// @param screenH 画面高さ（ピクセル）
  void DrawBanner(float screenW, float screenH) {
    if (bannerHandle_ < 0) return;

    // 素材の文字が黒なので、読めるように明るいパネルを先に敷く
    const float panelH = screenH * 0.34f;
    const float panelY = screenH * 0.16f;
    if (panelColor.w > 0.0f) {
      RC::DrawBox({0.0f, panelY}, {screenW, panelY + panelH}, panelColor);
    }

    // バナーは 1280x720 のキャンバス中央に文字が置かれた素材なので、
    // 画面全体に引き伸ばして重ねる
    Transform t{};
    t.scale = {1.0f, 1.0f, 1.0f};
    t.rotation = {0.0f, 0.0f, 0.0f};
    t.translation = {0.0f, 0.0f, 0.0f};
    RC::SetSpriteTransform(bannerHandle_, t);
    RC::SetSpriteScreenSize(bannerHandle_, screenW, screenH);
    RC::SetSpriteColor(bannerHandle_, {1.0f, 1.0f, 1.0f, 1.0f});
    RC::DrawSprite(bannerHandle_);
  }

  /// @brief スコアラベルと数字、評価ランクを描く
  /// @param screenW 画面幅（ピクセル）
  /// @param screenH 画面高さ（ピクセル）
  void DrawScore(float screenW, float screenH) {
    const GameSession &session = GameSession::Get();

    const float digitW = 40.0f;
    const float digitH = 80.0f;
    const float baseY = screenH * 0.60f;

    // 桁を分解する（0 のときも 1 桁は出す）。
    // 用意したハンドル数までしか描けないので、桁あふれは切り捨てる。
    std::vector<int> digits;
    int value = session.Score();
    if (value < 0) value = 0;
    do {
      digits.push_back(value % 10);
      value /= 10;
    } while (value > 0 && digits.size() < static_cast<size_t>(kMaxDigits));

    // ラベル + 余白 16px + 数字列の合計幅から、全体が中央に来る開始 X を求める
    const float labelW = 150.0f;
    const float labelH = 60.0f;
    const float totalW = labelW + 16.0f + digitW * static_cast<float>(digits.size());
    float x = (screenW - totalW) * 0.5f;

    // 数字も黒素材なので、下敷きを一枚敷いてから描く
    RC::DrawBox({x - 24.0f, baseY - 12.0f},
                {x + totalW + 24.0f, baseY + digitH + 12.0f},
                {0.92f, 0.95f, 1.0f, 0.82f});

    // スコアラベルを数字の高さの中央にそろえて描く
    if (scoreLabelHandle_ >= 0) {
      Transform t{};
      t.scale = {1.0f, 1.0f, 1.0f};
      t.rotation = {0.0f, 0.0f, 0.0f};
      t.translation = {x, baseY + (digitH - labelH) * 0.5f, 0.0f};
      RC::SetSpriteTransform(scoreLabelHandle_, t);
      RC::SetSpriteScreenSize(scoreLabelHandle_, labelW, labelH);
      RC::SetSpriteColor(scoreLabelHandle_, {1.0f, 1.0f, 1.0f, 1.0f});
      RC::DrawSprite(scoreLabelHandle_);
    }
    x += labelW + 16.0f;

    // number.png は 32x32 の数字を 5 列 x 2 行に並べたシート（上段 0〜4 / 下段 5〜9）
    for (size_t i = 0; i < digits.size(); ++i) {
      const int handle = digitHandles_[i];
      if (handle < 0) continue;

      const int d = digits[digits.size() - 1 - i]; // 上位桁から描く
      const float srcX = static_cast<float>(d % 5) * kDigitSrcSize;
      const float srcY = static_cast<float>(d / 5) * kDigitSrcSize;

      Transform t{};
      t.scale = {1.0f, 1.0f, 1.0f};
      t.rotation = {0.0f, 0.0f, 0.0f};
      t.translation = {x + digitW * static_cast<float>(i), baseY, 0.0f};
      RC::SetSpriteTransform(handle, t);
      RC::SetSpriteScreenSize(handle, digitW, digitH);
      RC::SetSpriteColor(handle, {1.0f, 1.0f, 1.0f, 1.0f});
      RC::DrawSpriteRect(handle, srcX, srcY, kDigitSrcSize, kDigitSrcSize,
                         kDigitSheetW, kDigitSheetH, 0.5f);
    }

    // 評価ランクは文字素材が無いので、色付きバーの本数で表す（S=4 / A=3 / B=2 / C=1）
    const char rank = session.Rank();
    int barCount = 1;
    RC::Vector4 rankColor = {0.65f, 0.65f, 0.70f, 0.95f};
    if (rank == 'S') { barCount = 4; rankColor = {1.00f, 0.84f, 0.20f, 0.95f}; }
    else if (rank == 'A') { barCount = 3; rankColor = {0.95f, 0.45f, 0.30f, 0.95f}; }
    else if (rank == 'B') { barCount = 2; rankColor = {0.35f, 0.70f, 0.95f, 0.95f}; }

    // バー列を数字の下に中央寄せで並べる
    const float barW = 46.0f;
    const float barH = 14.0f;
    const float gap = 10.0f;
    const float rankTotalW = barCount * barW + (barCount - 1) * gap;
    float rx = (screenW - rankTotalW) * 0.5f;
    const float ry = baseY + digitH + 26.0f;
    for (int i = 0; i < barCount; ++i) {
      RC::DrawBox({rx, ry}, {rx + barW, ry + barH}, rankColor);
      rx += barW + gap;
    }
  }

  /// @brief 「スペースキーで次へ」を点滅するバーで示す
  /// @details 文字素材が無いため、キーの形を模した枠と中央のバーで表現している。
  /// @param screenW 画面幅（ピクセル）
  /// @param screenH 画面高さ（ピクセル）
  void DrawPrompt(float screenW, float screenH) {
    // 1.2 秒周期で明滅させる（周期の前 60% を明るく、残りを暗く表示）
    const float cycle = 1.2f;
    float phase = blinkTimer_ - cycle * static_cast<float>(static_cast<int>(blinkTimer_ / cycle));
    const bool bright = (phase < cycle * 0.6f);
    const float alpha = bright ? 0.95f : 0.35f;

    const float keyW = 260.0f;
    const float keyH = 46.0f;
    const float x = (screenW - keyW) * 0.5f;
    const float y = screenH - 130.0f;

    // スペースキーを模した枠
    RC::DrawBox({x, y}, {x + keyW, y + keyH}, {0.05f, 0.08f, 0.12f, alpha * 0.75f});
    RC::DrawBox({x, y}, {x + keyW, y + keyH}, {1.0f, 1.0f, 1.0f, alpha}, kWire);
    // 中央の横棒（キートップの刻印に見立てる）
    RC::DrawBox({x + 40.0f, y + keyH * 0.5f - 3.0f},
                {x + keyW - 40.0f, y + keyH * 0.5f + 3.0f},
                {1.0f, 1.0f, 1.0f, alpha});
  }

  static constexpr float kDigitSrcSize = 32.0f; ///< number.png の 1 文字あたりのピクセル数
  static constexpr float kDigitSheetW = 160.0f; ///< number.png の幅
  static constexpr float kDigitSheetH = 64.0f;  ///< number.png の高さ
  static constexpr int kMaxDigits = 8;          ///< 同時に描ける最大桁数（＝確保するハンドル数）

  int bannerHandle_ = -1;
  int digitHandles_[kMaxDigits] = {-1, -1, -1, -1, -1, -1, -1, -1};
  int scoreLabelHandle_ = -1;
  float blinkTimer_ = 0.0f;
};

REGISTER_SCRIPT(SceneFlowUI)
