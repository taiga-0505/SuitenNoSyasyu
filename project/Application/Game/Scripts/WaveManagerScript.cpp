#include "ECS/ScriptableEntity.h"
#include "ECS/ScriptRegistry.h"
#include "Scene.h"
#include "RenderCommon.h"
#include "Engine/Render/RenderContext.h"
#include "Framework/App.h"
#include "Common/Log/Log.h"
#include "Game/Framework/StateMachine.h"

#if RC_ENABLE_IMGUI
#include "imgui/imgui.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

// =====================================================================
// A-03: ウェーブ戦闘の管理
// =====================================================================
// シーンに 1 つだけ置く。既定のエンティティ名は "WaveManager"。
//
// スクリプト同士は型ではなく Entity のタグで通信する。互いのヘッダを
// include せずに済み、片方だけをシーンに置いても壊れないため。
//
//   このエンティティのタグ（＝レールとスポナーが見る「掲示板」）
//     wave_request      : レールが立てる。開始してほしいウェーブ id。受理すると 0 へ戻す
//     wave_active       : 進行中のウェーブ id（Idle なら 0）。スポナーはこれを見て湧かせる
//     wave_cleared_id   : 直近にクリアしたウェーブ id。ウェーブ開始時に 0 へ戻す
//     waves_cleared     : 累計クリア数。レールの分岐条件 tag_at_least から参照できる
//     wave_force_clear  : デバッグ用。進行中のウェーブ id を書くと即クリアする
//
//   スポナー側のタグ（WaveSpawnerScript が書く）
//     wave_spawner      : 1 固定。スポナーの目印
//     spawner_wave_id   : そのスポナーが担当するウェーブ id
//     spawn_done        : 湧かせ終えたウェーブ id
//
//   敵側のタグ（WaveSpawnerScript が生成時に付ける）
//     wave_id           : 所属するウェーブ id
//
// 生存数はエンティティのポインタを保持せず、毎フレーム数え直す。
// Scene::CreateEntity で作った敵はそのフレームは pending にいて GetEntities()
// に現れないため、ポインタで台帳を持つと「湧く前に全滅」と誤判定しやすい。
// 代わりに「担当スポナーが全員 spawn_done を出す」まではクリア判定へ進まない。
//
// 進行段階（Idle → Intro → Spawning → Fighting → Cleared → Idle）は State パターンで持つ。
// 以前は enum class State + switch だったが、段階ごとの遷移条件と HUD の出し分けが
// OnUpdate / OnOverlayRender / DrawWaveBar の 3 か所の switch・if に散らばっていたため、
// 「段階 1 つ = クラス 1 つ」にまとめ、次の段階は各状態クラス自身が決めるようにした。

/// @brief ウェーブ戦闘の進行を管理するスクリプト
class WaveManagerScript : public ScriptableEntity {
public:
  // -------------------------------------------------------------------
  // 外部から参照する状態
  // -------------------------------------------------------------------
  /// @brief 進行中のウェーブ id（0 なら非戦闘中）
  /// @return 進行中のウェーブ id。非戦闘中は 0
  int ActiveWaveId() const { return activeWaveId_; }
  /// @brief 累計クリア数
  /// @return これまでにクリアしたウェーブの累計数
  int ClearedCount() const { return wavesCleared_; }

  /// @brief 演出と表示の設定を JSON に書き出す
  /// @return 設定項目を格納した JSON
  nlohmann::json Serialize() override {
    return {
        {"introDuration", introDuration_},
        {"clearDuration", clearDuration_},
        {"showHud", showHud_},
        {"showClearBanner", showClearBanner_},
        {"fontPath", fontPath_},
        {"panelColor", {panelColor_.x, panelColor_.y, panelColor_.z, panelColor_.w}},
        {"accentColor", {accentColor_.x, accentColor_.y, accentColor_.z, accentColor_.w}},
    };
  }

  /// @brief JSON から演出と表示の設定を読み込む
  /// @param j 読み込み元の JSON（存在するキーだけ反映する）
  void Deserialize(const nlohmann::json &j) override {
    if (j.contains("introDuration")) introDuration_ = j["introDuration"].get<float>();
    if (j.contains("clearDuration")) clearDuration_ = j["clearDuration"].get<float>();
    if (j.contains("showHud")) showHud_ = j["showHud"].get<bool>();
    if (j.contains("showClearBanner")) showClearBanner_ = j["showClearBanner"].get<bool>();
    if (j.contains("fontPath") && j["fontPath"].is_string()) fontPath_ = j["fontPath"].get<std::string>();
    if (j.contains("panelColor")) ReadColor(j["panelColor"], panelColor_);
    if (j.contains("accentColor")) ReadColor(j["accentColor"], accentColor_);
  }

#if RC_ENABLE_IMGUI
  /// @brief 状態表示・パラメータ編集と、デバッグ用のウェーブ要求／強制クリアのボタンを出す
  void OnImGui() override {
    ImGui::Text("State: %s", fsm_.CurrentName());
    ImGui::Text("Active Wave: %d", activeWaveId_);
    ImGui::Text("Remaining: %d / %d", remainingEnemies_, peakEnemies_);
    ImGui::Text("Cleared Count: %d", wavesCleared_);
    ImGui::Text("State Timer: %.2f", stateTimer_);

    ImGui::Separator();
    ImGui::DragFloat("Intro Duration", &introDuration_, 0.05f, 0.0f, 5.0f);
    ImGui::DragFloat("Clear Duration", &clearDuration_, 0.05f, 0.0f, 8.0f);
    ImGui::Checkbox("Show HUD", &showHud_);
    ImGui::Checkbox("Show Clear Banner", &showClearBanner_);
    ImGui::ColorEdit4("Panel Color", &panelColor_.x);
    ImGui::ColorEdit4("Accent Color", &accentColor_.x);

    ImGui::Separator();
    // 実機でウェーブの前後を行き来しながら調整するための操作
    ImGui::DragInt("Debug Wave Id", &debugWaveId_, 1.0f, 1, 99);
    if (ImGui::Button("Request Wave")) {
      if (Entity *self = GetEntity()) self->SetTag("wave_request", debugWaveId_);
    }
    ImGui::SameLine();
    if (ImGui::Button("Force Clear")) {
      if (activeWaveId_ != 0) {
        if (Entity *self = GetEntity()) self->SetTag("wave_force_clear", activeWaveId_);
      }
    }
  }
#endif

protected:
  /// @brief 前回プレイの残りタグを消して状態を初期化し、解像度に合わせて HUD 用のフォントを読み込む
  /// @note 状態クラスの完全型が要るので、定義は状態クラスの後に置く
  void OnCreate() override;

  /// @brief 読み込んだフォントを解放する
  void OnDestroy() override {
    auto unload = [](int &handle) {
      if (handle >= 0) { RC::UnloadFont(handle); handle = -1; }
    };
    unload(headingFont_);
    unload(itemFont_);
    unload(labelFont_);
  }

  /// @brief 強制クリア・デモ打ち切りの割り込みを処理し、現在の状態を 1 フレーム進める
  /// @param deltaTime 前フレームからの経過時間（秒）
  /// @note 状態クラスの完全型が要るので、定義は状態クラスの後に置く
  void OnUpdate(float deltaTime) override;

  /// @brief ポストプロセスの後に描く（水中の歪み・ビネットが UI に乗らないように。PauseMenu と同じ）
  /// @details 何を描くかは現在の状態クラスが決める（ポリモーフィズム）。
  void OnOverlayRender() override;

private:
  // -------------------------------------------------------------------
  // 状態クラス（State パターン）
  // -------------------------------------------------------------------
  // 入れ子の private クラスにしているのは、friend を使わずに本体の private メンバ
  // （タイマーや表示用の数値）へ触れるため。定義はクラス定義の後に置く。
  class WaveState;     ///< 全状態の共通基底（入ったときの共通処理と HUD の描き方）
  class IdleState;     ///< 開始要求待ち
  class IntroState;    ///< 開始を告げている（まだ湧かせない）
  class SpawningState; ///< スポナーが湧かせている最中
  class FightingState; ///< 全滅待ち
  class ClearedState;  ///< クリア表示中。終わったらレールを再開させる

  /// @brief JSON の配列から RGBA 色を読み込む
  /// @param j 読み込み元（要素数 4 以上の配列のときだけ反映する）
  /// @param out 書き込み先の色
  static void ReadColor(const nlohmann::json &j, RC::Vector4 &out) {
    if (j.is_array() && j.size() >= 4) {
      out = {j[0].get<float>(), j[1].get<float>(), j[2].get<float>(), j[3].get<float>()};
    }
  }

  /// @brief 状態に入ったときの共通処理。経過時間を 0 に戻し、wave_busy タグを更新する
  /// @param busy 新しい開始要求をまだ受け付けられない状態なら true
  void OnStateEntered(bool busy) {
    stateTimer_ = 0.0f;
    // Idle 以外は「新しい開始要求をまだ受け付けられない」ことを外へ出す。
    // wave_active だけでは足りない。Intro 中と Cleared 中は湧かせていないので
    // wave_active が 0 のままで、外からは Idle と区別が付かないため、
    // クリア演出中に次のウェーブを要求できてしまう。
    if (Entity *self = GetEntity()) {
      self->SetTag("wave_busy", busy ? 1 : 0);
    }
  }

  /// @brief 指定 id のウェーブを開始し、Intro 状態を予約する
  /// @param waveId 開始するウェーブ id
  void StartWave(int waveId);

  /// @brief クリア表示の状態を予約し、敵の募集を締め切る
  /// @note 予約だけを行う。状態クラスの外から呼んだときは呼び出し側で ApplyPending すること
  void EnterCleared();

  /// @brief クリア表示が終わったので、レールへ「終わった」と伝えて非戦闘状態の数値に戻す
  void FinishClearedWave() {
    // ここで初めてレールへ「終わった」と伝える。
    // クリア表示が出ている間はレールを止めたままにしたいので、
    // 全滅した瞬間ではなく表示が終わってから立てる。
    ++wavesCleared_;
    if (Entity *self = GetEntity()) {
      self->SetTag("wave_cleared_id", activeWaveId_);
      self->SetTag("waves_cleared", wavesCleared_);
      self->SetTag("wave_active", 0);
    }
    activeWaveId_ = 0;
    remainingEnemies_ = 0;
    peakEnemies_ = 0;
  }

  /// @brief 生存数を数え直して HUD 用の残り数・最大数を更新する
  /// @return 現在の生存数
  int RefreshRemainingEnemies() {
    const int alive = CountAliveEnemies(activeWaveId_);
    remainingEnemies_ = alive;
    if (alive > peakEnemies_) peakEnemies_ = alive;
    return alive;
  }

  /// @brief 指定ウェーブを担当するスポナーが全員湧かせ終えたか
  /// @param waveId 対象ウェーブ
  /// @param spawnerCountOut 見つかったスポナーの数（0 ならデータの設定漏れ）
  /// @return 担当スポナーが全員 spawn_done を出していれば true（担当が 0 個のときも true）。シーンが無ければ false
  bool AreSpawnersDone(int waveId, int &spawnerCountOut) {
    spawnerCountOut = 0;
    Scene *scene = GetScene();
    if (!scene) return false;

    bool allDone = true;
    for (const auto &e : scene->GetEntities()) {
      if (!e) continue;
      if (e->GetTagInt("wave_spawner", 0) != 1) continue;
      if (e->GetTagInt("spawner_wave_id", 0) != waveId) continue;
      ++spawnerCountOut;
      if (e->GetTagInt("spawn_done", 0) != waveId) allDone = false;
    }
    return allDone;
  }

  /// @brief 指定ウェーブの生存敵数
  /// @details 撃破された敵は enemy_defeated が立ち、2 秒後に実体が消える。
  ///          どちらの状態でも生存には数えない。
  /// @param waveId 対象ウェーブ id
  /// @return 生存している敵の数
  int CountAliveEnemies(int waveId) {
    Scene *scene = GetScene();
    if (!scene) return 0;

    int alive = 0;
    for (const auto &e : scene->GetEntities()) {
      if (!e) continue;
      if (e->GetTagInt("wave_id", 0) != waveId) continue;
      if (e->HasTag("enemy_defeated")) continue;
      ++alive;
    }
    return alive;
  }

  /// @brief 残っている敵を消す（デバッグの強制クリア用）
  /// @details 通常の撃破処理を通すとスコアが加算されてしまうため、実体だけを落とす。
  /// @param waveId 対象ウェーブ id
  void DespawnRemaining(int waveId) {
    Scene *scene = GetScene();
    if (!scene) return;
    for (const auto &e : scene->GetEntities()) {
      if (!e) continue;
      if (e->GetTagInt("wave_id", 0) != waveId) continue;
      if (e->HasTag("enemy_defeated")) continue;
      e->Destroy();
    }
  }

  // -------------------------------------------------------------------
  // 描画（PauseMenu と同じ意匠：1280x720 基準レイアウト・墨のパネル・金のアクセント・KiwiMaru）
  // -------------------------------------------------------------------
  static constexpr float kDesignW = 1280.0f;
  static constexpr float kDesignH = 720.0f;

  static constexpr float kHeadingPx = 40.0f;
  static constexpr float kItemPx = 26.0f;
  static constexpr float kLabelPx = 17.0f;

  static constexpr float kClearPanelW = 460.0f; ///< PauseMenu のメインパネルと同じ幅
  static constexpr float kClearPanelH = 168.0f;
  static constexpr float kClearPadTop = 30.0f;

  static constexpr float kBarW = 300.0f;        ///< 画面上部のウェーブ表示
  static constexpr float kBarH = 48.0f;
  static constexpr float kBarY = 14.0f;

  // 色（PauseMenu / Result と共通の生成り／鼠）
  static constexpr RC::Vector4 kInk = {0.95f, 0.93f, 0.87f, 1.0f};
  static constexpr RC::Vector4 kMuted = {0.66f, 0.70f, 0.76f, 1.0f};

  /// @brief 1280x720 基準の座標を実画面の座標へ変換するための倍率とオフセット
  struct Layout {
    float s = 1.0f;  ///< 1280x720 → 実解像度の倍率
    float ox = 0.0f; ///< レターボックスの横オフセット
    float oy = 0.0f; ///< レターボックスの縦オフセット
    float fs = 1.0f; ///< DrawString に渡す scale（ロード時スケールとの比）
  };
  Layout L_;

  /// @brief 1280x720 基準のレイアウトを実解像度へ合わせる倍率と、レターボックスのオフセットを求める
  void UpdateLayout() {
    float screenW = kDesignW, screenH = kDesignH;
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

  /// @brief 基準座標の X を画面座標へ変換する
  /// @param x 1280x720 基準の X
  /// @return 画面上の X（ピクセル）
  float X(float x) const { return L_.ox + x * L_.s; }
  /// @brief 基準座標の Y を画面座標へ変換する
  /// @param y 1280x720 基準の Y
  /// @return 画面上の Y（ピクセル）
  float Y(float y) const { return L_.oy + y * L_.s; }
  /// @brief 基準の長さを画面上の長さへ変換する
  /// @param v 1280x720 基準の長さ
  /// @return 画面上の長さ（ピクセル）
  float S(float v) const { return v * L_.s; }

  /// @brief 色のアルファに係数を掛けた色を返す
  /// @param c 元の色
  /// @param a アルファに掛ける係数
  /// @return アルファだけを変えた色
  static RC::Vector4 WithAlpha(const RC::Vector4 &c, float a) { return {c.x, c.y, c.z, c.w * a}; }
  /// @brief 3 次のイーズアウト（始めが速く終わりがゆっくり）
  /// @param t 進み具合（0〜1 にクランプされる）
  /// @return 補間係数（0〜1）
  static float EaseOutCubic(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    const float u = 1.0f - t;
    return 1.0f - u * u * u;
  }

  /// @brief 影付きテキスト（PauseMenu::Text と同じ）
  /// @param font フォントハンドル（負なら何もしない）
  /// @param text 描く文字列（空なら何もしない）
  /// @param x 描画位置の X（画面座標）
  /// @param y 描画位置の Y（画面座標）
  /// @param color 文字色（影の濃さもこのアルファに合わせる）
  /// @param align 文字揃え
  /// @param scale ロード時の大きさに対する追加の拡大率
  void Text(int font, const std::string &text, float x, float y, const RC::Vector4 &color,
            TextAlign align = TextAlign::Left, float scale = 1.0f) const {
    if (font < 0 || text.empty()) return;
    const float sc = scale * L_.fs;
    const float sh = S(2.0f);
    RC::DrawString(font, text, {x + sh, y + sh}, {0.0f, 0.02f, 0.05f, 0.55f * color.w}, sc, align);
    RC::DrawString(font, text, {x, y}, color, sc, align);
  }
  /// @brief フォントの行の高さを画面上の大きさで取得する
  /// @param font フォントハンドル
  /// @param scale ロード時の大きさに対する追加の拡大率
  /// @return 行の高さ（ピクセル）。フォントが無ければ kItemPx 相当で代用する
  float LineH(int font, float scale = 1.0f) const {
    return (font >= 0) ? RC::GetFontLineHeight(font, scale * L_.fs) : S(kItemPx) * scale;
  }

  /// @brief 墨のパネル（二重の細枠 ＋ 四隅の飾り。PauseMenu::DrawPanel と同じ）
  /// @param l,t,w,h 1280x720 基準の矩形
  /// @param a 不透明度の係数（0〜1）
  void DrawPanel(float l, float t, float w, float h, float a) const {
    const RC::Vector2 tl = {X(l), Y(t)};
    const RC::Vector2 br = {X(l + w), Y(t + h)};
    RC::DrawBox(tl, br, WithAlpha(panelColor_, a));

    // 内側に二重の細枠
    const float in1 = S(6.0f), in2 = S(11.0f);
    RC::DrawBox({tl.x + in1, tl.y + in1}, {br.x - in1, br.y - in1}, WithAlpha(kInk, 0.55f * a), kWire);
    RC::DrawBox({tl.x + in2, tl.y + in2}, {br.x - in2, br.y - in2}, WithAlpha(kInk, 0.22f * a), kWire);

    // 四隅の L 字の飾り（角ごとに横線と縦線を 1 本ずつ）
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

  /// @brief 画面上部にウェーブ番号と残り数を出す
  /// @param reveal せり出し具合（0〜1。1 未満のあいだは文字を出さない）
  /// @param showRemaining true なら残り数（Fighting 中）、false なら「出現中」を添える
  void DrawWaveBar(float reveal, bool showRemaining) const {
    const float h = kBarH * reveal;
    if (h < 2.0f) return;

    // 画面上部中央にパネルと内枠を描く
    const float l = (kDesignW - kBarW) * 0.5f;
    const RC::Vector2 tl = {X(l), Y(kBarY)};
    const RC::Vector2 br = {X(l + kBarW), Y(kBarY + h)};
    RC::DrawBox(tl, br, panelColor_);
    const float in1 = S(4.0f);
    RC::DrawBox({tl.x + in1, tl.y + in1}, {br.x - in1, br.y - in1}, WithAlpha(kInk, 0.4f), kWire);
    // 左端のアクセント帯
    RC::DrawBox(tl, {tl.x + S(6.0f), br.y}, accentColor_);

    // せり出しきるまでは文字を出さない
    if (reveal < 0.999f) return;

    char buf[32];
    std::snprintf(buf, sizeof(buf), "WAVE %d", activeWaveId_);
    const float textY = Y(kBarY) + (S(kBarH) - LineH(itemFont_)) * 0.5f;
    Text(itemFont_, buf, X(l + 22.0f), textY, kInk, TextAlign::Left);

    // 残り数：少ないうちは金の丸で、多いときは数字で
    if (showRemaining) {
      const float pipR = S(6.0f);
      const float gap = S(18.0f);
      const float pipY = (tl.y + br.y) * 0.5f;
      const float pipX = br.x - S(22.0f);
      if (remainingEnemies_ > 0 && remainingEnemies_ <= kMaxPips) {
        for (int i = 0; i < remainingEnemies_; ++i) {
          RC::DrawCircle({pipX - gap * static_cast<float>(i), pipY}, pipR, accentColor_);
        }
      } else if (remainingEnemies_ > kMaxPips) {
        std::snprintf(buf, sizeof(buf), "残り %d", remainingEnemies_);
        const float ly = Y(kBarY) + (S(kBarH) - LineH(labelFont_)) * 0.5f;
        Text(labelFont_, buf, br.x - S(18.0f), ly, kMuted, TextAlign::Right);
      }
    } else {
      // Spawning 中は「出現中」を小さく添える
      const float ly = Y(kBarY) + (S(kBarH) - LineH(labelFont_)) * 0.5f;
      Text(labelFont_, "出現中", br.x - S(18.0f), ly, WithAlpha(kMuted, 0.9f), TextAlign::Right);
    }
  }

  /// @brief クリア表示（"WAVE CLEAR" のパネルをフェードイン／アウト）
  void DrawClear() const {
    // 前後 25% をフェードに使い、真ん中は出したままにする
    float a = 1.0f;
    if (clearDuration_ > 0.0f) {
      const float t = stateTimer_ / clearDuration_;
      const float fade = 0.25f;
      if (t < fade)             a = EaseOutCubic(t / fade);
      else if (t > 1.0f - fade) a = EaseOutCubic((1.0f - t) / fade);
      a = std::clamp(a, 0.0f, 1.0f);
    }
    if (a <= 0.01f) return;

    // 背後をわずかに沈める（PauseMenu の dim より軽く。プレイは止まっていないので）
    RC::DrawBox({0.0f, 0.0f}, {X(kDesignW) + L_.ox, Y(kDesignH) + L_.oy},
                {0.02f, 0.03f, 0.06f, 0.30f * a});

    // 画面中央にパネルを置く
    const float l = (kDesignW - kClearPanelW) * 0.5f;
    const float t = (kDesignH - kClearPanelH) * 0.5f;
    DrawPanel(l, t, kClearPanelW, kClearPanelH, a);

    // 見出し ＋ 下線 ＋ 小さな添え字（PauseMenu::DrawHeading と同じ組み方）
    const float cx = X(kDesignW * 0.5f);
    const float top = Y(t + kClearPadTop);
    Text(headingFont_, "WAVE CLEAR", cx, top, WithAlpha(kInk, a), TextAlign::Center);

    const float lineY = top + LineH(headingFont_) + S(4.0f);
    const float halfW = S(kClearPanelW * 0.5f - 40.0f);
    RC::DrawLine({cx - halfW, lineY}, {cx + halfW, lineY}, WithAlpha(accentColor_, 0.7f * a),
                 (std::max)(1.0f, S(1.5f)));

    char buf[32];
    std::snprintf(buf, sizeof(buf), "WAVE %d", activeWaveId_);
    Text(labelFont_, buf, cx, lineY + S(8.0f), WithAlpha(kMuted, 0.9f * a), TextAlign::Center, 0.85f);

    // 再進行を示す帯。右へ流れて「この先へ進む」ことを伝える。
    const float sweep = (clearDuration_ > 0.0f) ? std::clamp(stateTimer_ / clearDuration_, 0.0f, 1.0f) : 1.0f;
    const float barY = Y(t + kClearPanelH) + S(18.0f);
    const float barW = S(kClearPanelW * 0.7f);
    const float barX = cx - barW * 0.5f;
    RC::DrawBox({barX, barY}, {barX + barW * sweep, barY + S(4.0f)}, WithAlpha(accentColor_, a));
  }

  static constexpr int kMaxPips = 8; ///< 丸で残り数を出す上限

  // --- インスペクタ / JSON から設定する項目 ---
  float introDuration_ = 0.8f;   ///< ウェーブ開始を告げてから敵を湧かせるまでの時間（秒）
  float clearDuration_ = 1.6f;   ///< 全滅してからレールを再開するまでの時間（秒）。クリア表示の長さでもある
  bool showHud_ = true;          ///< 画面上部のウェーブ表示を出すか
  bool showClearBanner_ = true;  ///< クリア時に "WAVE CLEAR" のパネルを出すか
  /// @brief 使うフォント（PauseMenu / Result と同じ KiwiMaru）
  std::string fontPath_ = "Resources/fonts/Kiwi_Maru/KiwiMaru-Medium.ttf";
  RC::Vector4 panelColor_ = {0.06f, 0.08f, 0.11f, 0.92f};  ///< パネルの色（PauseMenu と同じ墨）
  RC::Vector4 accentColor_ = {0.88f, 0.74f, 0.38f, 1.0f};  ///< アクセント色（PauseMenu と同じ金）

  // --- 実行時の状態 ---
  StateMachine<WaveManagerScript> fsm_; ///< 進行段階のステートマシン
  int activeWaveId_ = 0;                ///< 進行中のウェーブ id（0 なら非戦闘中）
  int wavesCleared_ = 0;                ///< 累計クリア数
  int remainingEnemies_ = 0;            ///< HUD 用の残り数
  int peakEnemies_ = 0;                 ///< HUD 用の最大数
  float stateTimer_ = 0.0f;             ///< 現在の状態に入ってからの経過秒数

  int headingFont_ = -1;
  int itemFont_ = -1;
  int labelFont_ = -1;
  float fontScale_ = 1.0f;

#if RC_ENABLE_IMGUI
  int debugWaveId_ = 1;
#endif
};

// =====================================================================
// 状態クラスの定義
// =====================================================================

/// @brief ウェーブ進行の全状態の共通基底
/// @details 入ったときの共通処理（経過時間のリセット・wave_busy の更新）を OnEnter に固定し、
///          「受け付け中か」「HUD をどう描くか」だけを派生クラスが差し替える。
class WaveManagerScript::WaveState : public IState<WaveManagerScript> {
public:
  /// @brief 状態に入ったときの共通処理（経過時間を 0 に戻し、wave_busy を更新する）
  /// @param owner 本体
  /// @param machine ステートマシン（未使用）
  void OnEnter(WaveManagerScript &owner, StateMachine<WaveManagerScript> &machine) override {
    (void)machine;
    owner.OnStateEntered(IsBusy());
  }

  /// @brief 新しい開始要求をまだ受け付けられない状態か
  /// @return 受け付けられないなら true（Idle 以外はすべて true）
  virtual bool IsBusy() const { return true; }

  /// @brief この状態で HUD を出すか
  /// @return 出すなら true
  virtual bool HasHud() const { return true; }

  /// @brief この状態の HUD を描く（レイアウトは本体が計算済み）
  /// @param owner 本体
  virtual void DrawHud(const WaveManagerScript &owner) const = 0;
};

/// @brief 開始要求待ち。レールからの wave_request を受理したら Intro へ
class WaveManagerScript::IdleState final : public WaveManagerScript::WaveState {
public:
  /// @brief 開始要求を受理する（受理したら要求タグは 0 へ戻す）
  /// @param owner 本体
  /// @param machine ステートマシン
  /// @param deltaTime 経過秒数（未使用）
  void OnUpdate(WaveManagerScript &owner, StateMachine<WaveManagerScript> &machine,
                float deltaTime) override {
    (void)machine;
    (void)deltaTime;
    Entity *self = owner.GetEntity();
    if (!self) return;
    const int requested = self->GetTagInt("wave_request", 0);
    if (requested != 0) {
      self->SetTag("wave_request", 0);
      owner.StartWave(requested); // Intro を予約する
    }
  }
  /// @brief Idle は新しい要求を受け付けられる
  /// @return 常に false
  bool IsBusy() const override { return false; }
  /// @brief Idle 中は HUD を出さない
  /// @return 常に false
  bool HasHud() const override { return false; }
  /// @brief 何も描かない
  /// @param owner 本体（未使用）
  void DrawHud(const WaveManagerScript &owner) const override { (void)owner; }
  /// @brief 状態名
  /// @return "Idle"
  const char *Name() const override { return "Idle"; }
};

/// @brief 開始を告げている。introDuration 経ったら wave_active を公開して Spawning へ
class WaveManagerScript::IntroState final : public WaveManagerScript::WaveState {
public:
  /// @brief Intro の時間が経ったらスポナーへ募集を公開する
  /// @param owner 本体
  /// @param machine ステートマシン
  /// @param deltaTime 経過秒数（未使用。経過時間は本体が数えている）
  void OnUpdate(WaveManagerScript &owner, StateMachine<WaveManagerScript> &machine,
                float deltaTime) override;
  /// @brief せり出してくるウェーブ表示を描く（0→1 で高さが伸びる）
  /// @param owner 本体
  void DrawHud(const WaveManagerScript &owner) const override {
    float reveal = 1.0f;
    if (owner.introDuration_ > 0.0f) {
      reveal = EaseOutCubic(owner.stateTimer_ / owner.introDuration_);
    }
    owner.DrawWaveBar(reveal, false);
  }
  /// @brief 状態名
  /// @return "Intro"
  const char *Name() const override { return "Intro"; }
};

/// @brief スポナーが湧かせている最中。全員 spawn_done を出したら Fighting へ
class WaveManagerScript::SpawningState final : public WaveManagerScript::WaveState {
public:
  /// @brief 担当スポナーの湧き終わりを待つ
  /// @param owner 本体
  /// @param machine ステートマシン
  /// @param deltaTime 経過秒数（未使用）
  void OnUpdate(WaveManagerScript &owner, StateMachine<WaveManagerScript> &machine,
                float deltaTime) override;
  /// @brief ウェーブ表示に「出現中」を添えて描く
  /// @param owner 本体
  void DrawHud(const WaveManagerScript &owner) const override { owner.DrawWaveBar(1.0f, false); }
  /// @brief 状態名
  /// @return "Spawning"
  const char *Name() const override { return "Spawning"; }
};

/// @brief 全滅待ち。生存数が 0 になったら Cleared へ
class WaveManagerScript::FightingState final : public WaveManagerScript::WaveState {
public:
  /// @brief 生存数を数え直し、0 になったらクリア表示へ
  /// @param owner 本体
  /// @param machine ステートマシン（未使用。遷移は EnterCleared が予約する）
  /// @param deltaTime 経過秒数（未使用）
  void OnUpdate(WaveManagerScript &owner, StateMachine<WaveManagerScript> &machine,
                float deltaTime) override {
    (void)machine;
    (void)deltaTime;
    if (owner.RefreshRemainingEnemies() == 0) {
      owner.EnterCleared();
    }
  }
  /// @brief ウェーブ表示に残り数を添えて描く
  /// @param owner 本体
  void DrawHud(const WaveManagerScript &owner) const override { owner.DrawWaveBar(1.0f, true); }
  /// @brief 状態名
  /// @return "Fighting"
  const char *Name() const override { return "Fighting"; }
};

/// @brief クリア表示中。clearDuration 経ったらレールへ知らせて Idle へ
class WaveManagerScript::ClearedState final : public WaveManagerScript::WaveState {
public:
  /// @brief クリア表示が終わったらレールを再開させる
  /// @param owner 本体
  /// @param machine ステートマシン
  /// @param deltaTime 経過秒数（未使用）
  void OnUpdate(WaveManagerScript &owner, StateMachine<WaveManagerScript> &machine,
                float deltaTime) override;
  /// @brief "WAVE CLEAR" のパネルを描く（バナー無効なら何も描かない）
  /// @param owner 本体
  void DrawHud(const WaveManagerScript &owner) const override {
    if (owner.showClearBanner_) owner.DrawClear();
  }
  /// @brief 状態名
  /// @return "Cleared"
  const char *Name() const override { return "Cleared"; }
};

// ---------------------------------------------------------------------
// 次の状態を参照する関数（遷移先の状態クラスが完全型になってから定義する）
// ---------------------------------------------------------------------

void WaveManagerScript::IntroState::OnUpdate(WaveManagerScript &owner,
                                             StateMachine<WaveManagerScript> &machine,
                                             float deltaTime) {
  (void)deltaTime;
  if (owner.stateTimer_ >= owner.introDuration_) {
    // スポナーはこのタグを見て湧かせるので、Intro が終わってから公開する
    if (Entity *self = owner.GetEntity()) {
      self->SetTag("wave_active", owner.activeWaveId_);
    }
    machine.ChangeState<SpawningState>();
  }
}

void WaveManagerScript::SpawningState::OnUpdate(WaveManagerScript &owner,
                                                StateMachine<WaveManagerScript> &machine,
                                                float deltaTime) {
  (void)deltaTime;
  int spawnerCount = 0;
  const bool allDone = owner.AreSpawnersDone(owner.activeWaveId_, spawnerCount);
  if (spawnerCount == 0) {
    // 担当スポナーが 1 つも無い＝データの設定漏れ。
    // ここで待ち続けるとレールが永久に止まって原因が見えなくなるため、
    // 警告を出して素通りさせる。
    Log::Print("[WaveManagerScript] no spawner for wave " +
               std::to_string(owner.activeWaveId_) + ". skipping.");
    owner.EnterCleared();
    return;
  }
  if (allDone) {
    machine.ChangeState<FightingState>();
  }
}

void WaveManagerScript::ClearedState::OnUpdate(WaveManagerScript &owner,
                                               StateMachine<WaveManagerScript> &machine,
                                               float deltaTime) {
  (void)deltaTime;
  if (owner.stateTimer_ >= owner.clearDuration_) {
    owner.FinishClearedWave();
    machine.ChangeState<IdleState>();
  }
}

void WaveManagerScript::OnCreate() {
  // ランタイム専用タグの消し込み。
  // タグは Entity::Serialize で JSON に往復するため、プレイ中に保存されたシーンでは
  // waves_cleared などが焼き付いている可能性がある。残っているとリトライ時に
  // 「もうクリア済み」と誤判定してウェーブが素通りする。
  if (Entity *self = GetEntity()) {
    self->ClearTag("wave_request");
    self->ClearTag("wave_force_clear");
    self->SetTag("wave_active", 0);
    self->SetTag("wave_cleared_id", 0);
    self->SetTag("waves_cleared", 0);
    self->SetTag("wave_busy", 0);
  }
  activeWaveId_ = 0;
  wavesCleared_ = 0;
  // 初期状態は Idle。Update を待たずに今すぐ入れる（OnEnter で経過時間も 0 になる）
  fsm_.ChangeState<IdleState>();
  fsm_.ApplyPending(*this);

  // フォントは基準解像度（1280x720）のサイズ × 実解像度のスケールでロードする（PauseMenu と同じ）
  float w = kDesignW, h = kDesignH;
  if (SceneContext *ctx = GetSceneContext()) {
    if (ctx->app && ctx->app->width > 0 && ctx->app->height > 0) {
      w = static_cast<float>(ctx->app->width);
      h = static_cast<float>(ctx->app->height);
    }
  }
  fontScale_ = (std::min)(w / kDesignW, h / kDesignH);
  if (fontScale_ <= 0.0f) fontScale_ = 1.0f;

  // 基準解像度でのサイズ（px）にスケールを掛けてフォントを読み込むヘルパー
  auto load = [&](float designPx) {
    const int handle = RC::LoadFont(fontPath_, std::round(designPx * fontScale_), 1024);
    if (handle < 0) Log::Print("[WaveManagerScript] failed to load font: " + fontPath_);
    return handle;
  };
  headingFont_ = load(kHeadingPx);
  itemFont_ = load(kItemPx);
  labelFont_ = load(kLabelPx);
}

void WaveManagerScript::OnUpdate(float deltaTime) {
  Entity *self = GetEntity();
  if (!self) return;

  stateTimer_ += deltaTime;

  // デバッグ用の強制クリア（レール側の Skip Wave ボタンからも書かれる）
  // どの状態からでも割り込むので、状態クラスではなく本体で受ける。
  // 予約した Cleared には今すぐ入れ、このフレームの Update は Cleared として回す（以前と同じ順序）。
  const int forced = self->GetTagInt("wave_force_clear", 0);
  if (forced != 0) {
    self->ClearTag("wave_force_clear");
    if (activeWaveId_ != 0 && forced == activeWaveId_) {
      DespawnRemaining(activeWaveId_);
      EnterCleared();
      fsm_.ApplyPending(*this);
    }
  }

  // 見せ用デモ（ClearRunDemo）の打ち切り：残りの敵を黙って消し、クリア表示も出さずに Idle へ戻す
  if (self->GetTagInt("wave_demo_abort", 0) == 1) {
    self->ClearTag("wave_demo_abort");
    if (activeWaveId_ != 0) DespawnRemaining(activeWaveId_);
    Log::Print("[WaveManagerScript] demo abort (wave " + std::to_string(activeWaveId_) + ")");
    activeWaveId_ = 0;
    remainingEnemies_ = 0;
    peakEnemies_ = 0;
    self->SetTag("wave_request", 0);
    self->SetTag("wave_active", 0); // スポナーはこれで湧かせ途中でも打ち切る
    // HUD がこのフレームから消えるよう、予約ではなく今すぐ Idle に入れる
    fsm_.ChangeState<IdleState>();
    fsm_.ApplyPending(*this);
    return;
  }

  // 状態ごとの進行はポリモーフィズムで現在の状態クラスに任せる
  fsm_.Update(*this, deltaTime);
}

void WaveManagerScript::OnOverlayRender() {
  if (!showHud_) return;
  const WaveState *state = fsm_.As<WaveState>();
  if (!state || !state->HasHud()) return;

  UpdateLayout();
  state->DrawHud(*this);
}

void WaveManagerScript::StartWave(int waveId) {
  activeWaveId_ = waveId;
  remainingEnemies_ = 0;
  peakEnemies_ = 0;
  if (Entity *self = GetEntity()) {
    // 待ち合わせは wave_cleared_id の「一致」で見るので、開始時に必ず 0 へ戻す。
    // 同じ id のウェーブを 2 度回したときに、前回の値で即クリア扱いになるのを防ぐ。
    self->SetTag("wave_cleared_id", 0);
    self->SetTag("wave_active", 0); // Intro の間はまだ湧かせない
  }
  fsm_.ChangeState<IntroState>();
  Log::Print("[WaveManagerScript] wave " + std::to_string(waveId) + " start");
}

void WaveManagerScript::EnterCleared() {
  remainingEnemies_ = 0;
  if (Entity *self = GetEntity()) {
    // 湧かせ直しを防ぐため、クリア表示に入った時点で募集を締め切る
    self->SetTag("wave_active", 0);
  }
  fsm_.ChangeState<ClearedState>();
  Log::Print("[WaveManagerScript] wave " + std::to_string(activeWaveId_) + " cleared");
}

REGISTER_SCRIPT(WaveManagerScript)
