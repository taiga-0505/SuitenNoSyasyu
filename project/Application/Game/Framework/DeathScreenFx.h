#pragma once
#include "Engine/Graphics/PostProcess/PostProcess.h"
#include "Engine/Render/RenderContext.h"
#include "Framework/App.h"
#include "RenderCommon.h"
#include "SceneManager.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <initializer_list>

#if RC_ENABLE_IMGUI
#include "imgui/imgui.h"
#endif

/// @brief 力尽きたときの画面演出（レンズの血・色が抜ける・瞬き・視界の滲み・水面の光）をまとめて管理する
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
///   ・視界の滲み（VisionBleed）：周辺から水彩のような滲み（なめらかなぼかし・色のにじみ・縁の濃淡・減光）が内側へ入り込む。
///     力尽きた瞬間に少し出て、沈むにつれて bleedEnd まで進み、瞼が閉じかけるたびに一段強くなる
///     （開き直すと少し引くが、元には戻らない）。
///     二重像は瞼が開き直る瞬間に跳ね上がり、ghostSettle 秒ほどで 1 枚に戻る（焦点が合わない感じ）。
///     眼の側の効果なので、景色（水中の歪みなど）より後・レンズの血と墨より前に置く。
///   ・水面の光が遠ざかる（LightShaft / Caustics）：沈むにつれて光柱を細く・弱くし、
///     深さ減衰を強めて「光の届く層」を上へ押し上げる。床の網目も消していく。
///     水中に入った直後の値を基準として覚え、Clear で戻す。
///   ・瞼：ポストエフェクトの後に 2D の円弧で上下から閉じる。開いている部分が「中央が広く目尻が細い」
///     アーモンド形になるよう、縁は上瞼が ∩、下瞼が ∪ の弧（大きな円の外側を太いリングで塗る）。
///     色は Dive 遷移の色と同じなので、
///     閉じきったところから Result の「深海からの浮上」へ継ぎ目なくつながる。
///   ・遷移の幕（Dissolve）が掛かっている間は、ColorGrade / VisionBleed を積んだり血を最後へ移したりしない
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

  bool bleed = true;            ///< 視界の滲みを出すか
  float bleedOnHit = 0.15f;     ///< 力尽きた直後の滲み（0〜1）
  float bleedEnd = 0.8f;        ///< 瞼が閉じきる頃の滲み（0〜1）
  float bleedLidGain = 0.3f;    ///< 瞼の閉じ具合 1 あたりに上乗せする滲み
  float bleedPigment = 0.5f;    ///< 濡れた領域の色ムラ（水彩の濃淡）の強さ（0〜1）
  bool doubleVision = true;     ///< 瞼が開き直るときに二重に見せるか
  float ghostMax = 0.018f;      ///< 二重像の最大のずれ（UV。画面幅に対する割合）
  float ghostGain = 2.2f;       ///< 瞼が開いた量 1 あたりに足す二重像の強さ
  float ghostSettle = 0.45f;    ///< 二重像が 1 枚に戻る速さ（時定数。秒）

  bool lightFade = true;        ///< 沈むにつれて水面の光を遠ざけるか
  float shaftIntensityTo = 0.2f;  ///< 閉じきる頃の光柱の強さ（基準に対する倍率）
  float shaftDensityMul = 3.0f;   ///< 閉じきる頃の深さ減衰（基準に対する倍率。大きいほど光が浅い層だけに残る）
  float shaftContrastMul = 1.8f;  ///< 閉じきる頃の光柱の断面コントラスト（基準に対する倍率。大きいほど細い）
  float causticsTo = 0.0f;        ///< 閉じきる頃の床の網目の強さ（基準に対する倍率）

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

  /// @brief 沈み始めてから瞼が閉じきるまでの進み具合
  /// @return 0（沈み始め・未開始）〜 1（閉じきった）
  float SinkProgress() const {
    if (!rt_.blinking) return 0.0f;
    return std::clamp(rt_.blinkT / (std::max)(rt_.blinkLen, 0.1f), 0.0f, 1.0f);
  }

  /// @brief 毎フレーム呼ぶ。時間を進め、彩度・血・滲み・水面の光を PostProcess に流し込む
  /// @param pp         反映先の PostProcess（nullptr なら時間だけ進める）
  /// @param dt         経過時間（秒）
  /// @param underwater カメラが水中にいるか（血を洗い流し始める・水面の光の基準を取るきっかけ）
  void Update(PostProcess *pp, float dt, bool underwater) {
    if (!rt_.active) return;
    rt_.t += dt;
    if (rt_.blinking) rt_.blinkT += dt;
    UpdateGhost(dt); // 瞼の動きは pp が無くても追っておく
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

    // --- 視界の滲み・二重像 ---
    if (bleed || doubleVision) {
      if (!pp->HasEffect(PostEffectType::VisionBleed) && !curtain) {
        pp->AddEffect(PostEffectType::VisionBleed);
        rt_.ownsVision = true;
      }
      const float b = BleedAmount();
      const float ghost = doubleVision ? ghostMax * rt_.ghost : 0.0f;
      const float pigment = bleed ? bleedPigment : 0.0f;
      pp->SetVisionBleed(b, ghost, pigment);
      rt_.visionTouched = true;
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
      }
    }

    // --- 水面の光が遠ざかる ---
    if (lightFade) UpdateSurfaceLight(pp, underwater);

    // 並びを整える（着水で水中エフェクトが後ろに積まれても、滲み→血→墨を最後に保つ）
    if (!curtain) EnsureTailOrder(pp);
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
      if (rt_.visionTouched) pp->SetVisionBleed(0.0f, 0.0f, 0.0f);
      if (rt_.ownsVision) pp->RemoveEffect(PostEffectType::VisionBleed);
      if (rt_.lightCaptured) {
        pp->SetLightShaftIntensity(rt_.baseShaftIntensity);
        pp->SetLightShaftDensity(rt_.baseShaftDensity);
        pp->SetLightShaftContrast(rt_.baseShaftContrast);
        pp->SetCausticsIntensity(rt_.baseCaustics);
      }
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
            {"blinkMin", blinkMin},
            {"bleed", bleed},
            {"bleedOnHit", bleedOnHit},
            {"bleedEnd", bleedEnd},
            {"bleedLidGain", bleedLidGain},
            {"bleedPigment", bleedPigment},
            {"doubleVision", doubleVision},
            {"ghostMax", ghostMax},
            {"ghostGain", ghostGain},
            {"ghostSettle", ghostSettle},
            {"lightFade", lightFade},
            {"shaftIntensityTo", shaftIntensityTo},
            {"shaftDensityMul", shaftDensityMul},
            {"shaftContrastMul", shaftContrastMul},
            {"causticsTo", causticsTo}};
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
    b("bleed", bleed);
    f("bleedOnHit", bleedOnHit);
    f("bleedEnd", bleedEnd);
    f("bleedLidGain", bleedLidGain);
    f("bleedPigment", bleedPigment);
    b("doubleVision", doubleVision);
    f("ghostMax", ghostMax);
    f("ghostGain", ghostGain);
    f("ghostSettle", ghostSettle);
    b("lightFade", lightFade);
    f("shaftIntensityTo", shaftIntensityTo);
    f("shaftDensityMul", shaftDensityMul);
    f("shaftContrastMul", shaftContrastMul);
    f("causticsTo", causticsTo);
  }

#if RC_ENABLE_IMGUI
  /// @brief 調整値の編集 UI を ImGui に描画する
  void DrawImGui() {
    ImGui::Text("t=%.2f  blink=%.2f/%.2f  lid=%.2f", rt_.t, rt_.blinkT, rt_.blinkLen, LidClosure());
    ImGui::Text("bleed=%.2f  ghost=%.2f  light=%s", BleedAmount(), rt_.ghost,
                rt_.lightCaptured ? "captured" : "-");
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
    ImGui::SeparatorText("Vision Bleed (視界の滲み・二重像)");
    ImGui::Checkbox("Bleed", &bleed);
    ImGui::SliderFloat("Bleed On Hit", &bleedOnHit, 0.0f, 1.0f);
    ImGui::SliderFloat("Bleed End", &bleedEnd, 0.0f, 1.0f);
    ImGui::SliderFloat("Bleed Lid Gain", &bleedLidGain, 0.0f, 1.0f);
    ImGui::SliderFloat("Pigment", &bleedPigment, 0.0f, 1.0f);
    ImGui::Checkbox("Double Vision", &doubleVision);
    ImGui::SliderFloat("Ghost Max (UV)", &ghostMax, 0.0f, 0.05f, "%.4f");
    ImGui::DragFloat("Ghost Gain", &ghostGain, 0.05f, 0.0f, 10.0f);
    ImGui::DragFloat("Ghost Settle (s)", &ghostSettle, 0.01f, 0.05f, 3.0f);
    ImGui::SeparatorText("Surface Light (水面の光が遠ざかる)");
    ImGui::Checkbox("Light Fade", &lightFade);
    ImGui::SliderFloat("Shaft Intensity To", &shaftIntensityTo, 0.0f, 1.0f);
    ImGui::DragFloat("Shaft Density x", &shaftDensityMul, 0.05f, 1.0f, 10.0f);
    ImGui::DragFloat("Shaft Contrast x", &shaftContrastMul, 0.05f, 0.5f, 5.0f);
    ImGui::SliderFloat("Caustics To", &causticsTo, 0.0f, 1.0f);
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
    bool visionTouched = false;  ///< VisionBleed の値を書いた（Clear で 0 に戻す）
    bool ownsVision = false;     ///< VisionBleed を自分で積んだ（Clear で外す）
    float prevLid = 0.0f;        ///< 前フレームの瞼の閉じ具合（開き直した量を測る）
    float ghost = 0.0f;          ///< 二重像の強さ（0〜1）
    int underwaterFrames = 0;    ///< 連続で水中にいるフレーム数（光の基準を取るタイミング）
    bool lightCaptured = false;  ///< 水面の光の基準値を覚えた（Clear で戻す）
    float baseShaftIntensity = 1.0f; ///< 基準：光柱の強さ
    float baseShaftDensity = 0.08f;  ///< 基準：深さ減衰
    float baseShaftContrast = 3.0f;  ///< 基準：断面のコントラスト
    float baseCaustics = 1.0f;       ///< 基準：床の網目の強さ
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

  /// @brief 今の滲みの強さ
  /// @details 力尽きた直後は bleedOnHit までふわっと出て、沈み始めると bleedEnd へ向かって進む。
  ///          そこへ瞼の閉じ具合ぶんを上乗せする（閉じかけるほど滲み、開き直すと少し引く）。
  /// @return 0〜1（bleed が無効なら 0）
  float BleedAmount() const {
    if (!bleed || !rt_.active) return 0.0f;
    float base = bleedOnHit * Smooth01(rt_.t / 0.4f);
    if (rt_.blinking) base = (std::max)(base, bleedOnHit + (bleedEnd - bleedOnHit) * Smooth01(SinkProgress()));
    return std::clamp(base + bleedLidGain * LidClosure(), 0.0f, 1.0f);
  }

  /// @brief 瞼の動きから二重像の強さを進める
  /// @details 開き直した量（閉じ具合が減った量）に比例して跳ね上げ、それ以外のフレームは
  ///          指数で 0 へ戻す。閉じかけ → 開き直す → しばらく二重 → 1 枚に戻る、を繰り返す。
  /// @param dt 経過時間（秒）
  void UpdateGhost(float dt) {
    const float c = LidClosure();
    const float dc = c - rt_.prevLid;
    rt_.prevLid = c;
    if (dc < 0.0f) {
      rt_.ghost = (std::min)(1.0f, rt_.ghost + (-dc) * ghostGain);
    } else if (dt > 0.0f) {
      rt_.ghost *= std::exp(-dt / (std::max)(ghostSettle, 0.01f));
    }
  }

  /// @brief 沈むにつれて水面の光（光柱・床の網目）を遠ざける
  /// @details 基準値は「水中に入って数フレーム後」の値を取る。着水した瞬間は
  ///          RailShooterController が水中の光の設定（SetupLight）を流し込むので、
  ///          その後でないと水上の古い値を基準にしてしまう。
  /// @param pp         反映先の PostProcess（nullptr 不可）
  /// @param underwater カメラが水中にいるか
  void UpdateSurfaceLight(PostProcess *pp, bool underwater) {
    rt_.underwaterFrames = underwater ? rt_.underwaterFrames + 1 : 0;
    if (!rt_.lightCaptured) {
      if (rt_.underwaterFrames < 3) return;
      rt_.baseShaftIntensity = pp->GetLightShaftIntensity();
      rt_.baseShaftDensity = pp->GetLightShaftDensity();
      rt_.baseShaftContrast = pp->GetLightShaftContrast();
      rt_.baseCaustics = pp->GetCausticsIntensity();
      rt_.lightCaptured = true;
    }
    // ease-out：沈み始めにぐっと遠ざかり、あとは暗いところでゆっくり消えていく
    const float p = SinkProgress();
    const float k = 1.0f - (1.0f - p) * (1.0f - p);
    auto mix = [k](float base, float mul) { return base * (1.0f + (mul - 1.0f) * k); };
    pp->SetLightShaftIntensity(mix(rt_.baseShaftIntensity, shaftIntensityTo));
    pp->SetLightShaftDensity(mix(rt_.baseShaftDensity, shaftDensityMul));
    pp->SetLightShaftContrast(mix(rt_.baseShaftContrast, shaftContrastMul));
    pp->SetCausticsIntensity(mix(rt_.baseCaustics, causticsTo));
  }

  /// @brief スタックの最後を [滲み, 血, 墨]（あるものだけ）の並びに保つ
  /// @details 滲みは眼の側、血と墨はレンズの表面なので、景色の後ろに並べる。
  ///          墨（InkScreenFx）も自分を最後へ移すので、墨は常に最後にしておく。
  ///          並びが正しいときは何もしない（Add/Remove はログが出るので毎フレームは呼ばない）。
  /// @param pp 対象の PostProcess（nullptr 不可）
  static void EnsureTailOrder(PostProcess *pp) {
    PostEffectType want[3];
    size_t k = 0;
    for (PostEffectType t : {PostEffectType::VisionBleed, PostEffectType::BloodOverlay,
                             PostEffectType::InkOverlay}) {
      if (pp->HasEffect(t)) want[k++] = t;
    }
    const auto &e = pp->GetEffects();
    const size_t n = e.size();
    bool ok = (n >= k);
    for (size_t i = 0; ok && i < k; ++i) ok = (e[n - k + i] == want[i]);
    if (ok) return;
    for (size_t i = 0; i < k; ++i) pp->RemoveEffect(want[i]);
    for (size_t i = 0; i < k; ++i) pp->AddEffect(want[i]);
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
