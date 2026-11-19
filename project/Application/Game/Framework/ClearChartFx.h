#pragma once
#include "RenderCommon.h"
#include "Game/Framework/StageProgress.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

#if RC_ENABLE_IMGUI
#include "imgui/imgui.h"
#endif

/// @brief ステージクリアの「海図」演出（航路選択と同じ海図を出し、踏破した区間に航路を引いて「踏破」を押す）
/// @details 使い方（ClearAscendScript から呼ぶ）:
///   - シーン開始時に LoadFonts()、終了時に UnloadFonts()。
///   - クリアしたら Begin()。以降は毎フレーム Update()、OnOverlayRender で Draw()。
///   - IsDone() になったら（画面が白くなりきったら）Ascend 遷移を要求する。
///     SceneManager は同じ白へ抜け、Result は白から開けて空から降りてくる。
///
///   見た目はステージセレクト（StageSelectScript）と揃えている:
///     レイアウト（見出し・海図パネル・情報パネルの位置）、色（パネル・浅葱・金・文字色）、
///     パネルの枠（細枠と四隅の L 字）、海図の並び（水面の上下・洞窟の岩・面の丸）はセレクトと同じ値。
///     ここで引いた浅葱の区間と、次の面の金の輪・選択色は、次にセレクトを開いたときの表示へそのままつながる。
///     セレクト側の見た目を変えたら、こちらの定数も合わせること。
///
///   流れ（各時間は Params）:
///     1. hold   : ゲーム画面を暗くする（HUD は RailShooterController 側で消える）
///     2. unroll : 海図パネルが中央から上下に開く
///     3. 見出しと中身が浮かび、今クリアした面から次の面まで浅葱の航路が伸びていく（inkTime）。
///        航路の先頭をタコが進む。最終面は洞窟を抜けた先の朝日まで引く。
///     4. 次の面が選択色に変わって金の輪が灯り、情報パネルに「踏破」の印が押される
///     5. linger のあいだ見せてから、bleach 秒で白へ飛ばす
///   描画はすべてポストプロセスの後（OnOverlayRender）で行うので、水中の歪みや輪郭線は乗らない。
///
///   設計:
///     - 「どの面をクリアしたか」で変わる部分（見出し・航路の行き先・次の面・情報パネル）は、
///       以前は final_ / stage_ >= 0 の if を各描画関数に散らして分けていた。これを Route の派生クラス
///       （ステージ外 / 通常の面 / 最終面）に分け、Sequence は route_ の virtual を呼ぶだけにした（ポリモーフィズム）。
///       どの Route を作るかは MakeRoute（Factory Method）1 か所で決める。
///     - 流れ 1〜5 は State パターンにしていない: 各段の開始時刻は経過時間 t_ と Params から計算で決まり、
///       早送りは t_ を先の時刻へ飛ばすだけで済む。状態クラスに分けると早送りのたびに状態を
///       順に渡り歩く必要があり、かえって複雑になるため、時間割（TUnroll0〜TEnd）の関数で表している。
namespace ClearChartFx {

/// @brief 海図演出の調整値（JSON キーは "chart*"。ClearAscendScript の scriptData に入る）
/// @details 不変条件の無い調整値の束（インスペクタで直接いじる）なので、public メンバの struct のままにしている。
struct Params {
  float hold = 0.35f;    ///< クリアしてから海図が開き始めるまで（秒）
  float unroll = 0.45f;  ///< 海図パネルが開ききるまで（秒）
  float inkTime = 1.1f;  ///< 航路が次の地点まで伸びる時間（秒）
  float linger = 1.4f;   ///< 「踏破」を押してから白へ飛ばし始めるまで（秒）
  float bleach = 0.6f;   ///< 白くなりきるまで（秒）
  float dimAlpha = 0.5f; ///< 背後のゲーム画面を暗くする濃さ（0〜1）
  bool skippable = true; ///< 決定入力（Space / Enter / クリック / A）で早送りできるか

  /// @brief 調整値を JSON に書き足す
  /// @param j 書き込み先
  void WriteJson(nlohmann::json &j) const {
    j["chartHold"] = hold;
    j["chartUnroll"] = unroll;
    j["chartInk"] = inkTime;
    j["chartLinger"] = linger;
    j["chartBleach"] = bleach;
    j["chartDim"] = dimAlpha;
    j["chartSkippable"] = skippable;
  }

  /// @brief JSON から調整値を読む（存在し型が合うキーだけ反映する）
  /// @param j 読み込み元
  void ReadJson(const nlohmann::json &j) {
    auto f = [&](const char *k, float &o) { if (j.contains(k) && j[k].is_number()) o = j[k].get<float>(); };
    f("chartHold", hold);
    f("chartUnroll", unroll);
    f("chartInk", inkTime);
    f("chartLinger", linger);
    f("chartBleach", bleach);
    f("chartDim", dimAlpha);
    if (j.contains("chartSkippable") && j["chartSkippable"].is_boolean()) skippable = j["chartSkippable"].get<bool>();
  }

#if RC_ENABLE_IMGUI
  /// @brief インスペクタに調整 UI を出す
  void DrawImGui() {
    ImGui::DragFloat("Chart Hold (s)", &hold, 0.01f, 0.0f, 3.0f);
    ImGui::DragFloat("Chart Unroll (s)", &unroll, 0.01f, 0.05f, 3.0f);
    ImGui::DragFloat("Chart Ink (s)", &inkTime, 0.01f, 0.1f, 5.0f);
    ImGui::DragFloat("Chart Linger (s)", &linger, 0.01f, 0.0f, 6.0f);
    ImGui::DragFloat("Chart Bleach (s)", &bleach, 0.01f, 0.05f, 3.0f);
    ImGui::SliderFloat("Chart Dim", &dimAlpha, 0.0f, 1.0f);
    ImGui::Checkbox("Chart Skippable", &skippable);
  }
#endif
};

/// @brief 海図演出の本体（状態・フォント・描画）
class Sequence {
public:
  /// @brief 海図で使うフォントを読み込む（実解像度に合わせた大きさで。拡大でぼやけないように）
  /// @param path    フォントファイル
  /// @param screenW 画面の幅（ピクセル）
  /// @param screenH 画面の高さ（ピクセル）
  void LoadFonts(const std::string &path, float screenW, float screenH) {
    UnloadFonts();
    fontScale_ = (std::min)(screenW / kDesignW, screenH / kDesignH);
    if (fontScale_ <= 0.0f) fontScale_ = 1.0f;
    auto load = [&](float px) { return RC::LoadFont(path, std::round(px * fontScale_), 1024); };
    headingFont_ = load(kHeadingPx);
    nameFont_ = load(kNamePx);
    nodeFont_ = load(kNodePx);
    labelFont_ = load(kLabelPx);
  }

  /// @brief LoadFonts で読み込んだフォントを解放する
  void UnloadFonts() {
    auto unload = [](int &h) {
      if (h >= 0) RC::UnloadFont(h);
      h = -1;
    };
    unload(headingFont_);
    unload(nameFont_);
    unload(nodeFont_);
    unload(labelFont_);
  }

  /// @brief 演出を開始する
  /// @param p     調整値（演出中は参照を保持するので寿命に注意）
  /// @param stage クリアした面の番号（0 始まり。ステージ外なら -1 で、航路の強調なしで出す）
  /// @param score 今回のスコア
  /// @note StageProgress::MarkCleared の後に呼ぶこと（解放の判定に JustUnlocked を使う）
  void Begin(const Params &p, int stage, int score) {
    p_ = &p;
    active_ = true;
    t_ = 0.0f;
    stage_ = (stage >= 0 && stage < StageProgress::kStageCount) ? stage : -1;
    score_ = score;
    const StageProgress &prog = StageProgress::Get();
    route_ = MakeRoute(stage_);
    unlocked_ = (stage_ >= 0 && prog.JustUnlocked() == stage_ + 1);
    best_ = (stage_ >= 0) ? prog.BestScore(stage_) : score;
  }

  /// @brief 時間を進める（毎フレーム呼ぶ）
  /// @param dt          経過時間（秒）
  /// @param skipPressed このフレームに決定入力があったか（海図が開ききってから受け付ける）
  void Update(float dt, bool skipPressed) {
    if (!active_ || !p_) return;
    t_ += dt;
    if (skipPressed && p_->skippable && t_ >= TOpen()) {
      // 1 回目: 航路と「踏破」を済ませた状態へ / 2 回目: すぐ白へ飛ばし始める
      if (t_ < TStamped()) {
        t_ = TStamped();
      } else if (t_ < TBleach0()) {
        t_ = TBleach0();
      }
    }
  }

  /// @brief 演出を打ち切る
  void End() {
    active_ = false;
    p_ = nullptr;
    t_ = 0.0f;
  }

  /// @brief 演出中か
  /// @return Begin 後、End されるまで true
  bool IsActive() const { return active_; }
  /// @brief 白くなりきったか（呼び出し側はここで Ascend 遷移を要求する）
  /// @return 白くなりきっていれば true
  bool IsDone() const { return active_ && p_ && t_ >= TEnd(); }
  /// @brief 画面全体に重ねる白の不透明度
  /// @return 0〜1
  float WhiteAlpha() const {
    if (!active_ || !p_) return 0.0f;
    return Smooth01((t_ - TBleach0()) / (std::max)(p_->bleach, 1e-3f));
  }

  /// @brief 海図を描く（OnOverlayRender から呼ぶ。最後に白の幕も重ねる）
  /// @param screenW 画面の幅（ピクセル）
  /// @param screenH 画面の高さ（ピクセル）
  void Draw(float screenW, float screenH) const {
    if (!active_ || !p_ || !route_) return;
    const Params &p = *p_;

    // 1280x720 基準 → 実画面（縦横の小さいほうに合わせ、余りは均等に空ける。セレクトと同じ）
    L_.s = (std::min)(screenW / kDesignW, screenH / kDesignH);
    if (L_.s <= 0.0f) L_.s = 1.0f;
    L_.ox = (screenW - kDesignW * L_.s) * 0.5f;
    L_.oy = (screenH - kDesignH * L_.s) * 0.5f;
    L_.fs = (fontScale_ > 0.0f) ? (L_.s / fontScale_) : 1.0f;

    // 背後を暗くする
    const float dim = p.dimAlpha * Smooth01(t_ / 0.35f);
    if (dim > 0.0f) RC::DrawBox({0.0f, 0.0f}, {screenW, screenH}, {0.02f, 0.03f, 0.06f, dim});

    // 海図パネル（中央から上下に開く）→ 中身 → 見出しと情報パネル
    const float open = EaseOutCubic((t_ - TUnroll0()) / (std::max)(p.unroll, 1e-3f));
    if (open > 0.0f) {
      ca_ = 1.0f;
      const float cy = (kChartT + kChartB) * 0.5f;
      const float hh = (kChartB - kChartT) * 0.5f * open;
      DrawFrame(kChartL, cy - hh, kChartR, cy + hh);
      ca_ = Smooth01((t_ - TOpen()) / kContentFade);
      if (ca_ > 0.0f) {
        DrawHeading();
        DrawChart();
        DrawInfo();
        DrawHint();
      }
    }

    // 白へ飛ばす
    const float w = WhiteAlpha();
    if (w > 0.0f) RC::DrawBox({0.0f, 0.0f}, {screenW, screenH}, {kWhite.x, kWhite.y, kWhite.z, w});
  }

private:
  // ------------------------------------------------------------------
  // 時間割（Params から決まる各段の開始時刻）
  // ------------------------------------------------------------------
  static constexpr float kContentFade = 0.25f; ///< パネルが開いてから中身が浮かびきるまで
  static constexpr float kArriveGap = 0.2f;    ///< 航路が着いてから「踏破」を押すまで
  static constexpr float kStampTime = 0.2f;    ///< 「踏破」が大きい状態から縮んで収まるまで

  float TUnroll0() const { return p_->hold; }
  float TOpen() const { return TUnroll0() + p_->unroll; }
  float TInk0() const { return TOpen() + kContentFade; }
  float TInk1() const { return TInk0() + p_->inkTime; }
  float TStamp() const { return TInk1() + kArriveGap; }
  float TStamped() const { return TStamp() + kStampTime; }
  float TBleach0() const { return TStamped() + p_->linger; }
  float TEnd() const { return TBleach0() + p_->bleach; }
  /// @brief 航路の進み具合（0〜1。ゆっくり出て、ゆっくり着く）
  float InkU() const { return Smooth01((t_ - TInk0()) / (std::max)(p_->inkTime, 1e-3f)); }
  /// @brief 航路が着いてからの現れ具合（0〜1）
  float Arrive() const { return Smooth01((t_ - TInk1()) / 0.3f); }

  // ------------------------------------------------------------------
  // レイアウト・色（1280x720 基準。StageSelectScript と同じ値）
  // ------------------------------------------------------------------
  static constexpr float kDesignW = 1280.0f;
  static constexpr float kDesignH = 720.0f;

  // 海図パネル
  static constexpr float kChartL = 110.0f;
  static constexpr float kChartT = 128.0f;
  static constexpr float kChartR = 1170.0f;
  static constexpr float kChartB = 446.0f;
  static constexpr float kSurfaceY = 262.0f;
  static constexpr float kAboveY = 234.0f;
  static constexpr float kBelowY = 350.0f;
  static constexpr float kNodeR = 26.0f;
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

  static constexpr RC::Vector4 kPanel = {0.05f, 0.06f, 0.09f, 0.78f};    ///< セレクトの panelColor
  static constexpr RC::Vector4 kAccent = {0.42f, 0.76f, 0.88f, 1.0f};    ///< セレクトの accentColor（浅葱）
  static constexpr RC::Vector4 kSelected = {0.12f, 0.27f, 0.50f, 0.96f}; ///< セレクトの selectedColor
  static constexpr RC::Vector4 kInk = {0.95f, 0.93f, 0.87f, 1.0f};
  static constexpr RC::Vector4 kMuted = {0.66f, 0.70f, 0.76f, 1.0f};
  static constexpr RC::Vector4 kDim = {0.40f, 0.44f, 0.50f, 1.0f};
  static constexpr RC::Vector4 kGold = {0.88f, 0.74f, 0.38f, 1.0f};
  static constexpr RC::Vector4 kSea = {0.06f, 0.22f, 0.38f, 0.55f};
  static constexpr RC::Vector4 kRock = {0.16f, 0.14f, 0.13f, 0.95f};
  static constexpr RC::Vector4 kRockEdge = {0.42f, 0.37f, 0.32f, 1.0f};
  static constexpr RC::Vector4 kOctopus = {0.95f, 0.45f, 0.36f, 1.0f}; ///< タコ（暗いパネルの上で目立つ珊瑚色）
  static constexpr RC::Vector4 kSun = {0.98f, 0.62f, 0.30f, 1.0f};     ///< 最終面の朝日
  static constexpr RC::Vector4 kWhite = {0.97f, 0.98f, 1.0f, 1.0f};    ///< SceneManager の kAscendScreenColor と同じ

  /// @brief 実画面への変換係数（Draw のたびに決める）
  struct Layout {
    float s = 1.0f, ox = 0.0f, oy = 0.0f, fs = 1.0f;
  };
  mutable Layout L_;
  mutable float ca_ = 0.0f; ///< いま描いている部品の不透明度（パネル枠は 1、中身はフェード量）

  float X(float x) const { return L_.ox + x * L_.s; }
  float Y(float y) const { return L_.oy + y * L_.s; }
  float S(float v) const { return v * L_.s; }
  RC::Vector2 P(float x, float y) const { return {X(x), Y(y)}; }
  RC::Vector2 P(const RC::Vector2 &v) const { return P(v.x, v.y); }
  static RC::Vector4 A(const RC::Vector4 &c, float a) { return {c.x, c.y, c.z, c.w * a}; }
  /// @brief 不透明度を掛け、さらにいまの部品のフェード量（ca_）を掛けた色
  RC::Vector4 C(const RC::Vector4 &c, float a = 1.0f) const { return {c.x, c.y, c.z, c.w * a * ca_}; }

  static float Clamp01(float v) { return (std::clamp)(v, 0.0f, 1.0f); }
  static float Smooth01(float v) {
    v = Clamp01(v);
    return v * v * (3.0f - 2.0f * v);
  }
  static float EaseOutCubic(float v) {
    v = 1.0f - Clamp01(v);
    return 1.0f - v * v * v;
  }
  static RC::Vector2 Lerp(const RC::Vector2 &a, const RC::Vector2 &b, float t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
  }

  /// @brief 面 i の丸の中心（基準座標。StageSelectScript::NodePos と同じ式）
  RC::Vector2 NodePos(int i) const {
    const float span = (kChartR - kChartL) - 240.0f;
    const float x = kChartL + 120.0f + span * static_cast<float>(i) / (StageProgress::kStageCount - 1);
    return {x, StageProgress::Info(i).underwater ? kBelowY : kAboveY};
  }
  /// @brief 最終面の航路の行き先（洞窟を抜けた先の朝日。5 面の右、海図の右端の手前）
  RC::Vector2 SunPos() const { return {kChartR - 46.0f, kSurfaceY - 22.0f}; }

  // ------------------------------------------------------------------
  // 航路の種類（ポリモーフィズム）
  // ------------------------------------------------------------------

  /// @brief 今回クリアした航路の種類ごとの違い（見出し・行き先・次の面・情報パネル）
  /// @details 既定の実装は「ステージ外（航路の強調なし）」の振る舞い。
  class Route {
  public:
    virtual ~Route() = default;

    /// @brief 見出しと欧文を決める
    /// @param title 見出しの出力先
    /// @param sub 欧文の出力先
    virtual void Heading(std::string &title, std::string &sub) const = 0;
    /// @brief 今回の航路の伸び具合（0〜1）
    /// @param s 演出本体
    /// @return 既定は 0（航路を引かない）
    virtual float InkProgress(const Sequence &s) const {
      (void)s;
      return 0.0f;
    }
    /// @brief 航路が着いてからの現れ具合（0〜1）
    /// @param s 演出本体
    /// @return 既定は 0（着かない）
    virtual float ArriveProgress(const Sequence &s) const {
      (void)s;
      return 0.0f;
    }
    /// @brief 航路が着いたら選択が移る面
    /// @return 面の番号。移らなければ -1
    virtual int NextIndex() const { return -1; }
    /// @brief 各区間の航路を描いた後に足す行き先（最終面の朝日など）
    /// @param s 演出本体
    /// @param u 今回の航路の伸び具合（0〜1）
    virtual void DrawDestination(const Sequence &s, float u) const {
      (void)s;
      (void)u;
    }
    /// @brief 航路の先頭を進むもの（タコ）を描く
    /// @param s 演出本体
    /// @param u 今回の航路の伸び具合（0〜1）
    virtual void DrawTraveler(const Sequence &s, float u) const {
      (void)s;
      (void)u;
    }
    /// @brief 情報パネルの左側（面の番号・名前・次の航路）を描く
    /// @param s 演出本体
    /// @param T 情報パネルの上端（基準座標。浮かび上がりのずれ込み）
    /// @param left 左端（実画面座標）
    virtual void DrawInfoLeft(const Sequence &s, float T, float left) const = 0;
    /// @brief 情報パネルに最高得点を添えるか
    /// @return 既定は false
    virtual bool ShowsBest() const { return false; }
  };

  /// @brief ステージ外から来たとき（エディタで直接開いたときなど）。航路の強調なしで出す
  class NoStageRoute : public Route {
  public:
    /// @brief 「航路 踏破 / STAGE CLEAR」
    void Heading(std::string &title, std::string &sub) const override {
      title = "航路 踏破";
      sub = "STAGE CLEAR";
    }
    /// @brief 「航路 踏破」だけを出す
    void DrawInfoLeft(const Sequence &s, float T, float left) const override {
      s.Text(s.nameFont_, "航路 踏破", left, s.Y(T + 50.0f), s.C(kInk));
    }
  };

  /// @brief 通常の面（1〜4 面）をクリアしたとき。次の面まで航路を引く
  class StageRoute : public Route {
  public:
    /// @brief クリアした面を受け取る
    /// @param index クリアした面の番号（0 始まり）
    explicit StageRoute(int index) : index_(index) {}

    /// @brief 「第 N 面　○○　踏破 / STAGE N  ○○  CLEAR」
    void Heading(std::string &title, std::string &sub) const override {
      const StageProgress::StageInfo &info = StageProgress::Info(index_);
      title = "第" + std::to_string(index_ + 1) + "面　" + info.title + "　踏破";
      sub = "STAGE " + std::to_string(index_ + 1) + "  " + info.subtitle + "  CLEAR";
    }
    float InkProgress(const Sequence &s) const override { return s.InkU(); }
    float ArriveProgress(const Sequence &s) const override { return s.Arrive(); }
    int NextIndex() const override { return index_ + 1; }

    /// @brief タコ（航路の先頭を進む）。丸と番号を隠さないよう、丸の縁の外から縁の手前までを進む
    void DrawTraveler(const Sequence &s, float u) const override {
      RC::Vector2 a, c;
      InkSegment(s, a, c);
      const float dx = c.x - a.x, dy = c.y - a.y;
      const float len = (std::max)(std::sqrt(dx * dx + dy * dy), 1e-3f);
      const RC::Vector2 dir = {dx / len, dy / len};
      const float margin = (std::min)(kNodeR + 12.0f, len * 0.4f);
      const RC::Vector2 from = {a.x + dir.x * margin, a.y + dir.y * margin};
      const RC::Vector2 to = TravelerEnd(c, dir, margin);
      s.DrawOctopus(Lerp(from, to, u));
    }

    /// @brief 第 N 面 ＋ 名前 ＋ 欧文と、航路が着いてからの「次の航路」
    void DrawInfoLeft(const Sequence &s, float T, float left) const override {
      const StageProgress::StageInfo &info = StageProgress::Info(index_);
      // 第 N 面 ＋ 名前 ＋ 欧文
      s.Text(s.labelFont_, "第" + std::to_string(index_ + 1) + "面", left, s.Y(T + 22.0f), s.C(kAccent));
      const float nameY = s.Y(T + 50.0f);
      s.Text(s.nameFont_, info.title, left, nameY, s.C(kInk));
      s.Text(s.labelFont_, info.subtitle, left + s.TextW(s.nameFont_, info.title) + s.S(16.0f),
             nameY + s.LineH(s.nameFont_) - s.LineH(s.labelFont_) - s.S(6.0f), s.C(kMuted, 0.8f));

      // 次の航路（航路が着いてから）。今回はじめて開いた面には金の「解放」の札
      const float k = s.Arrive();
      if (k > 0.0f) {
        const float ty = s.Y(T + 112.0f);
        const std::string next = NextRouteText();
        s.Text(s.labelFont_, next, left, ty + s.S(4.0f), s.C(kInk, k), TextAlign::Left, 0.85f);
        if (ShowsUnlockTag() && s.unlocked_) {
          const float tx = left + s.TextW(s.labelFont_, next, 0.85f) + s.S(14.0f);
          const float w = s.TextW(s.labelFont_, "解放", 0.85f) + s.S(24.0f);
          const float h = s.LineH(s.labelFont_, 0.85f) + s.S(8.0f);
          RC::DrawBox({tx, ty}, {tx + w, ty + h}, s.C(kGold, 0.25f * k));
          RC::DrawBox({tx, ty}, {tx + w, ty + h}, s.C(kGold, 0.9f * k), kWire);
          s.Text(s.labelFont_, "解放", tx + w * 0.5f, ty + s.S(4.0f), s.C(kInk, k), TextAlign::Center, 0.85f);
        }
      }
    }
    bool ShowsBest() const override { return true; }

  protected:
    /// @brief 今回の航路の始点と終点
    /// @param s 演出本体
    /// @param a 始点の出力先（クリアした面の丸）
    /// @param b 終点の出力先（InkEnd）
    void InkSegment(const Sequence &s, RC::Vector2 &a, RC::Vector2 &b) const {
      a = s.NodePos(index_);
      b = InkEnd(s);
    }
    /// @brief 今回の航路の終点
    /// @param s 演出本体
    /// @return 次の面の丸の中心
    virtual RC::Vector2 InkEnd(const Sequence &s) const { return s.NodePos(index_ + 1); }
    /// @brief タコが止まる位置
    /// @param c 航路の終点
    /// @param dir 航路の向き（単位ベクトル）
    /// @param margin 丸の縁から空ける距離
    /// @return 次の面の丸の手前（縁の外）
    virtual RC::Vector2 TravelerEnd(const RC::Vector2 &c, const RC::Vector2 &dir, float margin) const {
      return {c.x - dir.x * margin, c.y - dir.y * margin};
    }
    /// @brief 情報パネルの「次の航路」の文言
    /// @return 次の面の番号と名前
    virtual std::string NextRouteText() const {
      return "次の航路　第" + std::to_string(index_ + 2) + "面　" + StageProgress::Info(index_ + 1).title;
    }
    /// @brief 次の面が今回解放されたときに金の「解放」の札を出すか
    /// @return 通常の面は true
    virtual bool ShowsUnlockTag() const { return true; }

  private:
    int index_; ///< クリアした面（0 始まり）
  };

  /// @brief 最終面をクリアしたとき。次の面ではなく、洞窟を抜けた先の朝日まで航路を引く
  class FinalStageRoute : public StageRoute {
  public:
    /// @brief クリアした面（最終面）を受け取る
    /// @param index 最終面の番号（0 始まり）
    explicit FinalStageRoute(int index) : StageRoute(index) {}

    /// @brief 「全航路 踏破 / ALL CLEAR」
    void Heading(std::string &title, std::string &sub) const override {
      title = "全航路 踏破";
      sub = "ALL CLEAR";
    }
    /// @brief 最終面は選択を移さない（今回の面のまま）
    int NextIndex() const override { return -1; }
    /// @brief 朝日までの破線と浅葱の航路、朝日を描く
    void DrawDestination(const Sequence &s, float u) const override {
      RC::Vector2 a, c;
      InkSegment(s, a, c);
      s.DrawDashed(a, c);
      s.DrawRoute(a, c, u);
      s.DrawSun(s.Arrive());
    }

  protected:
    /// @brief 朝日の手前
    RC::Vector2 InkEnd(const Sequence &s) const override { return RC::Vector2{s.SunPos().x - 22.0f, kAboveY}; }
    /// @brief 朝日まで進みきる
    RC::Vector2 TravelerEnd(const RC::Vector2 &c, const RC::Vector2 &dir, float margin) const override {
      (void)dir;
      (void)margin;
      return c;
    }
    /// @brief 外洋へ抜ける一言
    std::string NextRouteText() const override { return "洞窟を抜け、夜明けの外洋へ"; }
    /// @brief 解放される面が無いので札は出さない
    bool ShowsUnlockTag() const override { return false; }
  };

  /// @brief クリアした面から Route を作る（Factory Method）
  /// @param stage クリアした面（0 始まり。ステージ外なら -1）
  /// @return ステージ外 / 通常の面 / 最終面 に応じた Route
  /// @details 種類の判断をここ 1 か所に集め、描画側は具体的な Route の型を知らなくて済むようにした。
  static std::unique_ptr<Route> MakeRoute(int stage) {
    if (stage < 0) return std::make_unique<NoStageRoute>();
    if (stage == StageProgress::kStageCount - 1) return std::make_unique<FinalStageRoute>(stage);
    return std::make_unique<StageRoute>(stage);
  }

  /// @brief 右下に 1px ずらした影付きで文字を描く（セレクトの Text と同じ）
  void Text(int font, const std::string &s, float x, float y, const RC::Vector4 &c,
            TextAlign align = TextAlign::Left, float scale = 1.0f) const {
    if (font < 0 || s.empty()) return;
    const float sc = scale * L_.fs;
    const float sh = (std::max)(1.0f, S(1.0f));
    RC::DrawString(font, s, {x + sh, y + sh}, {0.0f, 0.02f, 0.05f, 0.55f * c.w}, sc, align);
    RC::DrawString(font, s, {x, y}, c, sc, align);
  }
  float LineH(int font, float scale = 1.0f) const {
    return (font >= 0) ? RC::GetFontLineHeight(font, scale * L_.fs) : S(kLabelPx) * scale;
  }
  float TextW(int font, const std::string &s, float scale = 1.0f) const {
    return (font >= 0) ? RC::MeasureString(font, s, scale * L_.fs).x : 0.0f;
  }

  // ------------------------------------------------------------------
  // 描画
  // ------------------------------------------------------------------

  /// @brief パネルの下地・内側の細枠・四隅の L 字飾り（StageSelectScript::DrawFrame と同じ）
  void DrawFrame(float l, float t, float r, float b) const {
    const RC::Vector2 tl = P(l, t), br = P(r, b);
    RC::DrawBox(tl, br, C(kPanel));
    const float in1 = S(6.0f);
    if (br.y - tl.y > in1 * 2.0f) {
      RC::DrawBox({tl.x + in1, tl.y + in1}, {br.x - in1, br.y - in1}, C(kInk, 0.4f), kWire);
    }
    const float arm = (std::min)(S(22.0f), (br.y - tl.y) * 0.5f), th = (std::max)(1.0f, S(2.0f)), o = S(2.0f);
    const RC::Vector4 c = C(kAccent, 0.9f);
    RC::DrawLine({tl.x + o, tl.y + o}, {tl.x + o + S(22.0f), tl.y + o}, c, th);
    RC::DrawLine({tl.x + o, tl.y + o}, {tl.x + o, tl.y + o + arm}, c, th);
    RC::DrawLine({br.x - o, tl.y + o}, {br.x - o - S(22.0f), tl.y + o}, c, th);
    RC::DrawLine({br.x - o, tl.y + o}, {br.x - o, tl.y + o + arm}, c, th);
    RC::DrawLine({tl.x + o, br.y - o}, {tl.x + o + S(22.0f), br.y - o}, c, th);
    RC::DrawLine({tl.x + o, br.y - o}, {tl.x + o, br.y - o - arm}, c, th);
    RC::DrawLine({br.x - o, br.y - o}, {br.x - o - S(22.0f), br.y - o}, c, th);
    RC::DrawLine({br.x - o, br.y - o}, {br.x - o, br.y - o - arm}, c, th);
  }

  /// @brief 画面上部の見出し（第 N 面 ○○ 踏破 / 欧文。セレクトの「航路選択」と同じ位置・飾り）
  void DrawHeading() const {
    std::string title, sub;
    route_->Heading(title, sub); // 文言は航路の種類（Route）が決める
    const float cx = X(kDesignW * 0.5f);
    const float top = Y(30.0f + 8.0f * (1.0f - ca_)); // 少し下から浮かび上がる
    const float hh = LineH(headingFont_);
    Text(headingFont_, title, cx, top, C(kInk), TextAlign::Center);
    const float halfW = TextW(headingFont_, title) * 0.5f + S(28.0f);
    const float ruleY = top + hh * 0.56f, ruleLen = S(150.0f), th = (std::max)(1.0f, S(1.0f));
    RC::DrawLine({cx - halfW - ruleLen, ruleY}, {cx - halfW, ruleY}, C(kInk, 0.5f), th);
    RC::DrawLine({cx + halfW, ruleY}, {cx + halfW + ruleLen, ruleY}, C(kInk, 0.5f), th);
    RC::DrawCircle({cx - halfW - ruleLen, ruleY}, S(2.5f), C(kInk, 0.6f));
    RC::DrawCircle({cx + halfW + ruleLen, ruleY}, S(2.5f), C(kInk, 0.6f));
    Text(labelFont_, sub, cx, top + hh - S(2.0f), C(kMuted, 0.85f), TextAlign::Center, 0.75f);
  }

  /// @brief 海図（海中・洞窟・水面・航路・各面の丸・タコ）
  void DrawChart() const {
    const StageProgress &prog = StageProgress::Get();
    const float pad = 12.0f;
    const float l = kChartL + pad, r = kChartR - pad, b = kChartB - pad;

    // 海中（水面より下を薄い藍で）
    RC::DrawBox(P(l, kSurfaceY), P(r, b), C(kSea));
    DrawCave();

    // 水面の線（ゆっくり揺れる波線）
    {
      const int seg = 64;
      const float th = (std::max)(1.0f, S(2.0f));
      for (int k = 0; k < seg; ++k) {
        const float x0 = l + (r - l) * k / seg, x1 = l + (r - l) * (k + 1) / seg;
        const float y0 = kSurfaceY + 2.0f * std::sin(x0 * 0.05f + t_ * 1.6f);
        const float y1 = kSurfaceY + 2.0f * std::sin(x1 * 0.05f + t_ * 1.6f);
        RC::DrawLine(P(x0, y0), P(x1, y1), C(kAccent, 0.75f), th);
      }
      Text(labelFont_, "水面", X(l + 10.0f), Y(kSurfaceY - 26.0f), C(kMuted, 0.8f), TextAlign::Left, 0.75f);
      Text(labelFont_, "海中", X(l + 10.0f), Y(b - 26.0f), C(kMuted, 0.6f), TextAlign::Left, 0.75f);
    }

    // 航路。過去にクリアした区間は浅葱、まだの区間は鼠の破線（セレクトと同じ）。
    // 今回の区間は破線の上に浅葱が伸びていく
    const float u = route_->InkProgress(*this);
    for (int i = 0; i + 1 < StageProgress::kStageCount; ++i) {
      const RC::Vector2 a = NodePos(i), c = NodePos(i + 1);
      if (i == stage_) {
        DrawDashed(a, c);
        DrawRoute(a, c, u);
      } else if (prog.IsCleared(i)) {
        DrawRoute(a, c, 1.0f);
      } else {
        DrawDashed(a, c);
      }
    }
    // 最終面は、洞窟を抜けた先の朝日まで（行き先の追加は Route に任せる）
    route_->DrawDestination(*this, u);

    // 各面の丸。航路が着くまでは今回の面が選択中、着いたら次の面へ選択が移る
    // （セレクトは「今回解放された面」を選択し、金の輪を付けた状態で開くので、そこへつながる）
    const float arrive = route_->ArriveProgress(*this);
    const float pulse = 0.5f + 0.5f * std::sin(t_ * (6.28318530718f / 1.4f));
    const int nextIdx = route_->NextIndex();
    for (int i = 0; i < StageProgress::kStageCount; ++i) {
      const RC::Vector2 c = NodePos(i);
      const bool isNext = (i == nextIdx);
      // 今回はじめて開いた面は、航路が着くまで未解放のまま見せる（MarkCleared で解放済みになっているため）
      const bool unlocked = (isNext && unlocked_) ? (arrive > 0.0f) : prog.IsUnlocked(i);
      // 選択の重み（1 = 選択中）。今回の面 → 次の面へ arrive で移す。最終面は今回の面のまま
      float sel = 0.0f;
      if (i == stage_) sel = (nextIdx >= 0) ? 1.0f - arrive : 1.0f;
      if (isNext) sel = arrive;
      const float rad = kNodeR * (0.82f + 0.18f * sel);

      if (sel > 0.0f) {
        RC::DrawCircle(P(c), S(rad + 9.0f + 3.0f * pulse), C(kAccent, (0.25f + 0.2f * pulse) * sel));
      }
      if (isNext && unlocked_ && arrive > 0.0f) {
        RC::DrawCircle(P(c), S(rad + 16.0f + 6.0f * pulse), C(kGold, 0.2f + 0.25f * pulse * arrive), kWire);
      }
      RC::DrawCircle(P(c), S(rad), unlocked ? C(kInk) : C(kDim, 0.9f));
      const RC::Vector4 base = {kPanel.x, kPanel.y, kPanel.z, 1.0f};
      RC::DrawCircle(P(c), S(rad - 3.5f), unlocked ? C(Mix(base, kSelected, sel)) : C({0.08f, 0.09f, 0.11f, 1.0f}));

      if (unlocked) {
        Text(nodeFont_, std::to_string(i + 1), X(c.x), Y(c.y) - LineH(nodeFont_) * 0.5f,
             C(Mix(kMuted, kInk, sel)), TextAlign::Center);
      } else {
        DrawLock(c);
      }
      // クリア印（右上の金の小丸）。今回の面は「踏破」を押したときに付く（弾むように現れる）
      if (prog.IsCleared(i)) {
        float k = 1.0f;
        if (i == stage_) k = Clamp01((t_ - TStamp()) / kStampTime);
        if (k > 0.0f) {
          const float pop = 1.0f + 0.8f * (1.0f - k);
          const RC::Vector2 m = {c.x + rad * 0.72f, c.y - rad * 0.72f};
          RC::DrawCircle(P(m), S(7.0f * pop), C(kGold, k));
          RC::DrawCircle(P(m), S(3.0f * pop), C(base, k));
        }
      }
      // 名前（水上は丸の上、水中は丸の下）
      const StageProgress::StageInfo &info = StageProgress::Info(i);
      const float ny = info.underwater ? (c.y + kNodeR + 12.0f) : (c.y - kNodeR - 12.0f - kLabelPx);
      Text(labelFont_, unlocked ? info.title : "???", X(c.x), Y(ny),
           C(sel > 0.5f ? kInk : kMuted, unlocked ? 1.0f : 0.6f), TextAlign::Center, 0.85f);
    }

    // タコ（航路の先頭を進む）。ステージ外では出さない（Route に任せる）
    route_->DrawTraveler(*this, u);
  }

  /// @brief 洞窟（StageSelectScript::DrawCave と同じ岩の天井・床・口）
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

    RC::DrawBox(P(x0, kRoofBase), P(x1, kFloorBase), C({0.0f, 0.0f, 0.0f, 0.28f}));
    RC::DrawBox(P(x0, kRoofT), P(x1, kRoofBase), C(kRock));
    RC::DrawBox(P(x0, kFloorBase), P(x1, kFloorB), C(kRock));
    const int teeth = 18;
    const float w = (x1 - x0) / teeth;
    for (int k = 0; k < teeth; ++k) {
      const float a = x0 + w * k, c = a + w, m = (a + c) * 0.5f;
      const float drop = kToothMax * (0.4f + 0.6f * std::fabs(std::sin(k * 2.3f)));
      RC::DrawTriangle(P(a, kRoofBase), P(m, kRoofBase + drop), P(c, kRoofBase), C(kRock));
      RC::DrawLine(P(a, kRoofBase), P(m, kRoofBase + drop), C(kRockEdge, 0.7f), edge);
      RC::DrawLine(P(m, kRoofBase + drop), P(c, kRoofBase), C(kRockEdge, 0.7f), edge);
      const float rise = kToothMax * (0.4f + 0.6f * std::fabs(std::cos(k * 1.7f)));
      RC::DrawTriangle(P(a, kFloorBase), P(c, kFloorBase), P(m, kFloorBase - rise), C(kRock));
      RC::DrawLine(P(a, kFloorBase), P(m, kFloorBase - rise), C(kRockEdge, 0.7f), edge);
      RC::DrawLine(P(m, kFloorBase - rise), P(c, kFloorBase), C(kRockEdge, 0.7f), edge);
    }
    const float th = (std::max)(1.0f, S(2.0f));
    RC::DrawLine(P(x0, kRoofT), P(x0, kRoofBase), C(kRockEdge, 0.9f), th);
    RC::DrawLine(P(x1, kRoofT), P(x1, kRoofBase), C(kRockEdge, 0.9f), th);
    RC::DrawLine(P(x0, kFloorBase), P(x0, kFloorB), C(kRockEdge, 0.9f), th);
    RC::DrawLine(P(x1, kFloorBase), P(x1, kFloorB), C(kRockEdge, 0.9f), th);
    const float sc = 0.75f;
    const float ly = Y((kRoofT + kRoofBase) * 0.5f) - LineH(labelFont_, sc) * 0.5f;
    Text(labelFont_, "洞窟  CAVE", X((x0 + x1) * 0.5f), ly, C(kMuted, 0.9f), TextAlign::Center, sc);
  }

  /// @brief 通った区間の浅葱の線（u までを引き、先端に光の玉）
  void DrawRoute(const RC::Vector2 &a, const RC::Vector2 &b, float u) const {
    if (u <= 0.0f) return;
    const RC::Vector2 e = Lerp(a, b, u);
    RC::DrawLine(P(a), P(e), C(kAccent), (std::max)(1.0f, S(4.0f)));
    if (u < 1.0f) {
      RC::DrawCircle(P(e), S(9.0f), C(kAccent, 0.25f), kFill, S(6.0f));
      RC::DrawCircle(P(e), S(3.5f), C(kInk));
    }
  }

  /// @brief まだ通っていない区間の破線（セレクトと同じ）
  void DrawDashed(const RC::Vector2 &a, const RC::Vector2 &b) const {
    const int dashes = 10;
    const float th = (std::max)(1.0f, S(2.0f));
    for (int k = 0; k < dashes; k += 2) {
      const float t0 = static_cast<float>(k) / dashes, t1 = static_cast<float>(k + 1) / dashes;
      RC::DrawLine(P(Lerp(a, b, t0)), P(Lerp(a, b, t1)), C(kDim, 0.9f), th);
    }
  }

  /// @brief 未解放の面の丸の中の錠前（セレクトと同じ）
  void DrawLock(const RC::Vector2 &c) const {
    const RC::Vector4 col = C(kDim);
    const float bw = 11.0f, bh = 9.0f;
    RC::DrawBox(P(c.x - bw, c.y - 1.0f), P(c.x + bw, c.y - 1.0f + bh * 2.0f), col);
    RC::DrawCircle(P(c.x, c.y - 1.0f), S(8.0f), col, kWire);
    RC::DrawCircle(P(c.x, c.y + 7.0f), S(2.5f), C({0.08f, 0.09f, 0.11f, 1.0f}));
  }

  /// @brief 航路の先頭を進むタコ（丸い頭・目・揺れる足）
  /// @param pos 足の付け根の位置（基準座標）
  void DrawOctopus(const RC::Vector2 &pos) const {
    const RC::Vector4 body = C(kOctopus);
    const float th = (std::max)(1.0f, S(2.4f));
    for (int k = 0; k < 5; ++k) {
      const float x = pos.x - 8.0f + 4.0f * k;
      const float sway = 2.5f * std::sin(t_ * 9.0f + k * 1.3f);
      RC::DrawLine(P(x, pos.y - 4.0f), P(x + sway, pos.y + 8.0f), body, th);
    }
    RC::DrawCircle(P(pos.x, pos.y - 12.0f), S(11.0f), body);
    RC::DrawCircle(P(pos.x - 4.0f, pos.y - 12.0f), S(2.6f), C(kInk));
    RC::DrawCircle(P(pos.x + 4.0f, pos.y - 12.0f), S(2.6f), C(kInk));
    RC::DrawCircle(P(pos.x - 4.0f, pos.y - 11.5f), S(1.3f), C({0.05f, 0.06f, 0.09f, 1.0f}));
    RC::DrawCircle(P(pos.x + 4.0f, pos.y - 11.5f), S(1.3f), C({0.05f, 0.06f, 0.09f, 1.0f}));
  }

  /// @brief 最終面の行き先の朝日（昇る日と光の筋）
  /// @param k 現れ具合（0〜1。航路が着く前から薄く見せておく）
  void DrawSun(float k) const {
    const RC::Vector2 c = SunPos();
    const float faint = 0.3f + 0.7f * k;
    const float th = (std::max)(1.0f, S(2.0f));
    for (int r = 0; r < 8; ++r) {
      const float ang = 3.14159265f * (0.05f + 0.9f * r / 7.0f); // 上半分に扇状
      const float ca = std::cos(ang), sa = std::sin(ang);
      const float r1 = 28.0f + 6.0f * k;
      RC::DrawLine(P(c.x + ca * 20.0f, c.y - sa * 20.0f), P(c.x + ca * r1, c.y - sa * r1), C(kSun, 0.8f * faint), th);
    }
    RC::DrawCircle(P(c), S(15.0f), C(kSun, faint));
  }

  /// @brief 下段の情報パネル（今回の面・次の航路・「踏破」・得点。セレクトの情報パネルと同じ並び）
  void DrawInfo() const {
    // 少し下から上がってくる
    const float rise = 12.0f * (1.0f - ca_);
    DrawFrame(kInfoL, kInfoT + rise, kInfoR, kInfoB + rise);
    const float T = kInfoT + rise;
    const float left = X(kInfoL + 44.0f), right = X(kInfoR - 44.0f);

    // 左側（面の番号・名前・次の航路）は航路の種類ごとに違うので Route に任せる
    route_->DrawInfoLeft(*this, T, left);

    // 右側：「踏破」（セレクトの「踏破済み」と同じ金。押した瞬間は大きく、縮んで収まる）と今回の得点
    if (t_ >= TStamp()) {
      const float k = Clamp01((t_ - TStamp()) / kStampTime);
      const float sc = 1.0f + 0.7f * (1.0f - k) * (1.0f - k);
      const std::string mark = "踏破";
      const float w = TextW(labelFont_, mark, sc) + S(20.0f * sc);
      const float h = LineH(labelFont_, sc) + S(6.0f * sc);
      const float cx = right - TextW(labelFont_, mark) * 0.5f - S(10.0f);
      const float cy = Y(T + 26.0f) + LineH(labelFont_) * 0.5f;
      RC::DrawBox({cx - w * 0.5f, cy - h * 0.5f}, {cx + w * 0.5f, cy + h * 0.5f}, C(kGold, 0.18f * k));
      RC::DrawBox({cx - w * 0.5f, cy - h * 0.5f}, {cx + w * 0.5f, cy + h * 0.5f}, C(kGold, 0.9f * k), kWire);
      Text(labelFont_, mark, cx, cy - LineH(labelFont_, sc) * 0.5f, C(kGold, k), TextAlign::Center, sc);
    }
    Text(labelFont_, "今回の得点", right, Y(T + 70.0f), C(kMuted), TextAlign::Right, 0.85f);
    Text(nameFont_, std::to_string(score_), right, Y(T + 96.0f), C(kInk), TextAlign::Right);
    if (route_->ShowsBest()) {
      const std::string best = "最高 " + std::to_string(best_);
      Text(labelFont_, best, right - TextW(nameFont_, std::to_string(score_)) - S(18.0f),
           Y(T + 96.0f) + LineH(nameFont_) - LineH(labelFont_, 0.75f) - S(6.0f), C(kMuted, 0.8f), TextAlign::Right,
           0.75f);
    }
  }

  /// @brief 画面下部の操作説明（早送りできるときだけ）
  void DrawHint() const {
    if (!p_ || !p_->skippable) return;
    Text(labelFont_, "SPACE / Enter　早送り", X(kDesignW * 0.5f), Y(kInfoB + 30.0f), C(kMuted, 0.85f),
         TextAlign::Center, 0.85f);
  }

  /// @brief 2 色を混ぜる
  static RC::Vector4 Mix(const RC::Vector4 &a, const RC::Vector4 &b, float t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t};
  }

  // ------------------------------------------------------------------
  // 状態
  // ------------------------------------------------------------------
  const Params *p_ = nullptr;
  bool active_ = false;
  float t_ = 0.0f;        ///< 開始からの経過時間（秒）
  int stage_ = -1;        ///< クリアした面（0 始まり。ステージ外なら -1）
  std::unique_ptr<Route> route_; ///< 航路の種類（ステージ外 / 通常の面 / 最終面）。Begin で作る
  bool unlocked_ = false; ///< 今回のクリアで次の面が解放されたか
  int score_ = 0;
  int best_ = 0;

  float fontScale_ = 1.0f;
  int headingFont_ = -1;
  int nameFont_ = -1;
  int nodeFont_ = -1;
  int labelFont_ = -1;
};

} // namespace ClearChartFx
