#pragma once
#include "Engine/Graphics/PostProcess/PostProcess.h"
#include "Engine/Render/RenderContext.h"
#include "Framework/App.h"
#include "RenderCommon.h"
#include "SceneManager.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>

#if RC_ENABLE_IMGUI
#include "imgui/imgui.h"
#endif

/// @brief 力尽きたときの画面演出（レンズの血・色が抜ける・瞬き）をまとめて管理する
/// @details 使い方（DeathSinkScript から呼ぶ）：
///   - 力尽きた瞬間に Begin()。以降は毎フレーム Update()。
///   - 沈み始めたら StartBlink(暗くなりきるまでの秒数)。瞼はその時間で 2 回半開きになりながら閉じきる。
///     深度フォグで画面が暗くなりきってから瞬いても見えないので、閉じきる時刻を暗転に合わせる。
///   - 遷移の要求は BlinkFinished() になってから出す（閉じきる前に画面が切り替わらないように）。
///   - OnOverlayRender で DrawLids()。GAME OVER の文字（RailShooterController）より後に描くため、
///     NativeScriptComponent のスクリプト順は RailShooterController → DeathSinkScript にしておくこと。
///   - シーンを抜けるときに Clear()。
///
///   ・血（BloodOverlay）：被弾の瞬間に強く出て、瀕死の脈を打ちながら残る。水に入ると洗い流される。
///     レンズに付いたものなので、水中の歪みや色が乗らないよう常にスタックの最後（墨があればその手前）に置く。
///   ・色が抜ける（ColorGrade の彩度）：desaturateTime 秒かけて saturationTo まで落とす。
///     ColorGrade が無ければ自分で積み、外すのも自分で積んだときだけ（ExposureFlash と同じ作法）。
///   ・瞼：ポストエフェクトの後に 2D の円弧で上下から閉じる。開いている部分が「中央が広く目尻が細い」
///     アーモンド形になるよう、縁は上瞼が ∩、下瞼が ∪ の弧（大きな円の外側を太いリングで塗る）。
///     色は Dive 遷移の色と同じなので、
///     閉じきったところから Result の「深海からの浮上」へ継ぎ目なくつながる。
///   ・遷移の幕（Dissolve）が掛かっている間は、ColorGrade を積んだり血を最後へ移したりしない
///     （幕の後ろへ回ると、幕の色まで灰色になったり血が幕の上に出たりするため）。
struct DeathScreenFx {
  // ---- 調整値（DeathSinkScript の JSON キー "screenFx"） ----
  bool blood = true;            ///< レンズの血を出すか
  float bloodLevel = 0.7f;      ///< 瀕死の血の強さ（BloodOverlay の lowHealth）
  float bloodFlashTime = 0.6f;  ///< 被弾の瞬間の赤（hitFlash）が引くまで（秒）
  float bloodMinShow = 0.35f;   ///< 水に入っても、最低この秒数は血を見せる
  float bloodWashTime = 0.8f;   ///< 水に入ってから血が洗い流されるまで（秒）
  bool desaturate = true;       ///< 色を抜くか
  float saturationTo = 0.15f;   ///< 最終的な彩度（1 で素通し、0 で白黒）
  float desaturateTime = 2.6f;  ///< 力尽きてから saturationTo になるまで（秒）
  bool blink = true;            ///< 瞬きするか（false なら沈みきった時点ですぐ遷移）
  float blinkDuration = 0.0f;   ///< 沈み始めてから瞼が閉じきるまで（秒）。0 以下なら暗くなりきる時刻に自動で合わせる
  float blinkMin = 1.2f;        ///< 自動のときの下限（秒。すでに深いところで力尽きたとき用）

  /// @brief 演出を開始する（力尽きた瞬間に 1 回呼ぶ）
  /// @param pp        対象の PostProcess（nullptr なら状態だけ進める）
  /// @param withBlood 血を出すか（遷移の見せ用デモでは false）
  void Begin(PostProcess *pp, bool withBlood) {
    rt_ = Runtime{};
    rt_.active = true;
    rt_.bloodOn = blood && withBlood;
    if (pp && rt_.bloodOn) {
      pp->RerollBloodOverlaySplatter(); // 毎回違う飛び散り方にする
      pp->SetBloodOverlayHitFlash(1.0f);
      pp->SetBloodOverlayLowHealth(0.0f);
      rt_.bloodTouched = true;
    }
  }

  /// @brief 沈み始めたら呼ぶ（瞬きのタイムラインを開始する。2 回目以降は無視）
  /// @param untilDark 今から画面が暗くなりきるまでの見込み（秒）。blinkDuration が 0 以下のときに使う
  void StartBlink(float untilDark) {
    if (!rt_.active || rt_.blinking) return;
    rt_.blinking = true;
    rt_.blinkT = 0.0f;
    rt_.blinkLen = (blinkDuration > 0.0f) ? blinkDuration : (std::max)(untilDark, blinkMin);
  }

  /// @brief 瞬きのタイムラインが始まっているか
  /// @return 開始済みなら true
  bool Blinking() const { return rt_.blinking; }

  /// @brief 瞼が閉じきったか（遷移を要求してよいか）
  /// @return 閉じきった、または瞬きが無効なら true
  bool BlinkFinished() const { return !blink || (rt_.blinking && rt_.blinkT >= rt_.blinkLen); }

  /// @brief 毎フレーム呼ぶ。時間を進め、彩度と血を PostProcess に流し込む
  /// @param pp         反映先の PostProcess（nullptr なら時間だけ進める）
  /// @param dt         経過時間（秒）
  /// @param underwater カメラが水中にいるか（血を洗い流し始めるきっかけ）
  void Update(PostProcess *pp, float dt, bool underwater) {
    if (!rt_.active) return;
    rt_.t += dt;
    if (rt_.blinking) rt_.blinkT += dt;
    if (!pp) return;
    const bool curtain = pp->HasEffect(PostEffectType::Dissolve);

    // --- 色が抜ける ---
    if (desaturate) {
      if (!pp->HasEffect(PostEffectType::ColorGrade) && !curtain) {
        // 無ければ積む（ExposureFlash が自分で積んだ分を外した直後もここで積み直す）
        pp->AddEffect(PostEffectType::ColorGrade);
        pp->SetGradeExposure(0.0f);
        pp->SetGradeContrast(1.0f);
        pp->SetGradeTemperature(0.0f);
        pp->SetGradeTint(0.0f);
        pp->SetGradeColorFilter(1.0f, 1.0f, 1.0f);
        pp->SetGradeLerpFactor(1.0f);
        rt_.ownsGrade = true;
      }
      if (pp->HasEffect(PostEffectType::ColorGrade)) {
        const float k = Smooth01(rt_.t / (std::max)(desaturateTime, 0.01f));
        pp->SetGradeSaturation(1.0f + (saturationTo - 1.0f) * k);
        rt_.saturationTouched = true;
      }
    }

    // --- レンズの血 ---
    if (rt_.bloodOn) {
      if (underwater && rt_.washT < 0.0f && rt_.t >= bloodMinShow) rt_.washT = 0.0f;
      if (rt_.washT >= 0.0f) rt_.washT += dt;
      const float wash =
          (rt_.washT >= 0.0f) ? 1.0f - Smooth01(rt_.washT / (std::max)(bloodWashTime, 0.01f)) : 1.0f;
      const float f = 1.0f - std::clamp(rt_.t / (std::max)(bloodFlashTime, 0.01f), 0.0f, 1.0f);
      const float flash = f * f * wash;
      const float level = bloodLevel * Smooth01(rt_.t / 0.25f) * wash;
      pp->SetBloodOverlayHitFlash(flash);
      pp->SetBloodOverlayLowHealth(level);
      rt_.bloodTouched = true;

      if (rt_.washT >= bloodWashTime) {
        // 洗い流しきった：外す
        pp->RemoveEffect(PostEffectType::BloodOverlay);
        rt_.bloodOn = false;
      } else if (!curtain) {
        EnsureBloodOnTop(pp);
      }
    }
  }

  /// @brief 瞼を描く（OnOverlayRender から呼ぶ）
  void DrawLids() const {
    const float c = LidClosure();
    if (c <= 0.001f) return;

    float w = 1280.0f, h = 720.0f;
    auto &rc = RC::GetRenderContext();
    if (rc.Ctx() && rc.Ctx()->app && rc.Ctx()->app->width > 0 && rc.Ctx()->app->height > 0) {
      w = static_cast<float>(rc.Ctx()->app->width);
      h = static_cast<float>(rc.Ctx()->app->height);
    }

    // 上瞼の縁は、画面の下の方に中心を置いた大きな円の上側の弧（∩：中央が高く、目尻が低い）。
    // その円の外側を太いリングで塗ると、弧より上がすべて瞼になる。下瞼は上下反転。
    //   apex : 弧の頂点（画面中央）の高さ
    //   sag  : 頂点と画面端（目尻）の高さの差。開いているときは目尻のぼかしまで画面外へ出す
    //   閉じきったときは上下の頂点が feather ぶん重なり、中央まで塗りつぶされる
    const float r = w;
    const float halfW = w * 0.5f;
    const float sag = r - std::sqrt(r * r - halfW * halfW);
    const float feather = h * 0.05f; // 縁のぼかし（開いている側へ広がる）
    const float apexOpen = -(sag + feather);
    const float apexClosed = h * 0.5f + feather * 0.5f;
    const float apex = apexOpen + (apexClosed - apexOpen) * c;

    RC::Vector4 col = Scene::SceneManager::DiveScreenColor();
    col.w = 1.0f;
    // リングの太さ：円の中心から画面の遠い角までを覆えれば十分
    const float cy = apex + r; // 上瞼の円の中心（下瞼は h - cy）
    const float thick = std::sqrt(halfW * halfW + cy * cy) - r + feather + 2.0f;
    RC::DrawRing({halfW, cy}, r, thick, col, feather);
    RC::DrawRing({halfW, h - cy}, r, thick, col, feather);
  }

  /// @brief 全部元に戻す（シーンを抜けるとき・作り直すとき）
  /// @param pp 反映先の PostProcess（nullptr なら内部状態だけ消す）
  void Clear(PostProcess *pp) {
    if (pp) {
      if (rt_.bloodTouched) {
        pp->SetBloodOverlayHitFlash(0.0f);
        pp->SetBloodOverlayLowHealth(0.0f);
        pp->RemoveEffect(PostEffectType::BloodOverlay);
      }
      if (rt_.saturationTouched) pp->SetGradeSaturation(1.0f);
      if (rt_.ownsGrade) pp->RemoveEffect(PostEffectType::ColorGrade);
    }
    rt_ = Runtime{};
  }

  /// @brief 調整値を JSON に変換する
  /// @return 調整値を持つ JSON オブジェクト
  nlohmann::json ToJson() const {
    return {{"blood", blood},
            {"bloodLevel", bloodLevel},
            {"bloodFlashTime", bloodFlashTime},
            {"bloodMinShow", bloodMinShow},
            {"bloodWashTime", bloodWashTime},
            {"desaturate", desaturate},
            {"saturationTo", saturationTo},
            {"desaturateTime", desaturateTime},
            {"blink", blink},
            {"blinkDuration", blinkDuration},
            {"blinkMin", blinkMin}};
  }

  /// @brief JSON から調整値を読み込む
  /// @param j 読み込む JSON（無いキー・型が違うキーは現在の値のまま）
  void FromJson(const nlohmann::json &j) {
    auto f = [&](const char *k, float &o) { if (j.contains(k) && j[k].is_number()) o = j[k].get<float>(); };
    auto b = [&](const char *k, bool &o) { if (j.contains(k) && j[k].is_boolean()) o = j[k].get<bool>(); };
    b("blood", blood);
    f("bloodLevel", bloodLevel);
    f("bloodFlashTime", bloodFlashTime);
    f("bloodMinShow", bloodMinShow);
    f("bloodWashTime", bloodWashTime);
    b("desaturate", desaturate);
    f("saturationTo", saturationTo);
    f("desaturateTime", desaturateTime);
    b("blink", blink);
    f("blinkDuration", blinkDuration);
    f("blinkMin", blinkMin);
  }

#if RC_ENABLE_IMGUI
  /// @brief 調整値の編集 UI を ImGui に描画する
  void DrawImGui() {
    ImGui::Text("t=%.2f  blink=%.2f/%.2f  lid=%.2f", rt_.t, rt_.blinkT, rt_.blinkLen, LidClosure());
    ImGui::Checkbox("Blood", &blood);
    ImGui::SliderFloat("Blood Level", &bloodLevel, 0.0f, 1.0f);
    ImGui::DragFloat("Blood Flash (s)", &bloodFlashTime, 0.05f, 0.05f, 3.0f);
    ImGui::DragFloat("Blood Min Show (s)", &bloodMinShow, 0.05f, 0.0f, 3.0f);
    ImGui::DragFloat("Blood Wash (s)", &bloodWashTime, 0.05f, 0.05f, 5.0f);
    ImGui::Checkbox("Desaturate", &desaturate);
    ImGui::SliderFloat("Saturation To", &saturationTo, 0.0f, 1.0f);
    ImGui::DragFloat("Desaturate (s)", &desaturateTime, 0.05f, 0.1f, 10.0f);
    ImGui::Checkbox("Blink", &blink);
    ImGui::DragFloat("Blink (s, 0=auto)", &blinkDuration, 0.05f, 0.0f, 10.0f);
    ImGui::DragFloat("Blink Min (s)", &blinkMin, 0.05f, 0.3f, 5.0f);
  }
#endif

private:
  /// @brief 実行中の状態（調整値とは別にまとめて初期化する）
  struct Runtime {
    bool active = false;
    float t = 0.0f;              ///< 力尽きてからの経過（秒）
    bool blinking = false;
    float blinkT = 0.0f;         ///< 沈み始めてからの経過（秒）
    float blinkLen = 0.0f;       ///< 瞼が閉じきるまでの長さ（StartBlink で決める）
    bool bloodOn = false;        ///< 血を流し込み中
    float washT = -1.0f;         ///< 洗い流し始めてからの経過（負なら未開始）
    bool bloodTouched = false;   ///< BloodOverlay の値を書いた（Clear で戻す）
    bool saturationTouched = false; ///< 彩度を書いた（Clear で 1 に戻す）
    bool ownsGrade = false;      ///< ColorGrade を自分で積んだ（Clear で外す）
  };

  /// @brief 瞼のキー（時刻は長さ kKeyEnd 秒のときの値。実際の長さに比例して伸び縮みする）
  struct LidKey {
    float t; ///< 時刻（秒）
    float c; ///< 閉じ具合（0: 開いている 〜 1: 閉じきった）
  };
  static constexpr LidKey kLidKeys[] = {
      {0.00f, 0.00f}, {0.45f, 0.00f},                 // 沈み始めはまだ開いている
      {0.60f, 0.70f}, {0.85f, 0.15f}, {1.15f, 0.15f}, // 1 回目：重く閉じかけて、開ききらない
      {1.35f, 0.90f}, {1.70f, 0.35f}, {1.95f, 0.35f}, // 2 回目：ほぼ閉じて、さっきより開かない
      {2.40f, 1.00f},                                 // ゆっくり閉じきる
  };
  static constexpr float kKeyEnd = 2.40f;

  /// @brief 現在の瞼の閉じ具合
  /// @return 0（開いている）〜 1（閉じきった）
  float LidClosure() const {
    if (!blink || !rt_.blinking) return 0.0f;
    const float u = rt_.blinkT / (std::max)(rt_.blinkLen, 0.1f) * kKeyEnd;
    constexpr int n = static_cast<int>(sizeof(kLidKeys) / sizeof(kLidKeys[0]));
    for (int i = 0; i + 1 < n; ++i) {
      const LidKey &a = kLidKeys[i];
      const LidKey &b = kLidKeys[i + 1];
      if (u <= b.t) {
        const float local = (u - a.t) / (std::max)(b.t - a.t, 1e-4f);
        return a.c + (b.c - a.c) * Smooth01(local);
      }
    }
    return 1.0f;
  }

  /// @brief 血をスタックの最後（墨があればその手前）に置く
  /// @details 墨（InkScreenFx）も自分を最後へ移すので、[…, 血, 墨] の並びは崩さない。
  ///          並びが正しいときは何もしない（Add/Remove はログが出るので毎フレームは呼ばない）。
  /// @param pp 対象の PostProcess（nullptr 不可）
  static void EnsureBloodOnTop(PostProcess *pp) {
    const auto &e = pp->GetEffects();
    const size_t n = e.size();
    if (n >= 1 && e[n - 1] == PostEffectType::BloodOverlay) return;
    if (n >= 2 && e[n - 1] == PostEffectType::InkOverlay && e[n - 2] == PostEffectType::BloodOverlay) return;
    pp->RemoveEffect(PostEffectType::BloodOverlay);
    pp->AddEffect(PostEffectType::BloodOverlay);
  }

  /// @brief smoothstep（0〜1 に丸めてから 3t^2 - 2t^3）
  /// @param t 進行度
  /// @return 0〜1
  static float Smooth01(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
  }

  Runtime rt_;
};
