#pragma once

// ============================================================================
// GameTime（Chaso::Time）
// ----------------------------------------------------------------------------
// ゲーム時間（タイムスケール・ヒットストップ・スローが掛かる時間）と、
// 実時間（UI・ポーズメニュー用。タイムスケールの影響を受けない時間）を分けて管理する。
//
//   毎フレーム App が Tick() を呼ぶ。シーンの更新（スクリプトの OnUpdate・Rigidbody・
//   アニメーション・パーティクル）にはゲーム時間の deltaTime が渡る。
//
// 使い方（スクリプトから）:
//   Chaso::Time::Get().HitStop(0.08f);               // 0.08 秒（実時間）だけ止める
//   Chaso::Time::Get().SlowMotion(0.3f, 1.5f, 0.5f); // 0.3 倍速を 1.5 秒、最後の 0.5 秒で等速へ戻す
//   Chaso::Time::Get().SetTimeScale(0.5f);           // ずっと半分の速さ（ポーズは SceneContext::gamePaused を使う）
//   float uiDt = Chaso::Time::Get().UnscaledDeltaTime(); // ヒットストップ中も動かしたい UI 用
//
// 最終的な倍率 = timeScale × スロー倍率 × ヒットストップ倍率
// ヒットストップとスローの残り時間は実時間で減る（止めている間も解除されるように）。
// ゲームが止まっている間（編集モード・一時停止・gamePaused）は残り時間も減らない。
// ※ ファイル名を Time.h にしないこと。Common/ がインクルードパスに入っているため、
//   Windows（大文字小文字を区別しない）では <time.h> の代わりに読まれてしまう。
// ============================================================================

#include <algorithm>
#include <cstdint>

namespace Chaso {

/// @class Time
/// @brief ゲーム時間と実時間の管理（シングルトン）
class Time {
public:
  /// @brief インスタンスを取得する
  /// @return 唯一のインスタンス
  static Time &Get() {
    static Time instance;
    return instance;
  }

  /// @brief 1 フレーム分進める（App が毎フレーム 1 回だけ呼ぶ）
  /// @param unscaledDeltaTime 前フレームからの実経過時間（秒）
  /// @param simulating ゲームが進んでいるか（再生中かつゲーム内ポーズでない）
  /// @param playing 再生中か（編集モードでは false。UI 用の実時間も 0 にする）
  void Tick(float unscaledDeltaTime, bool simulating, bool playing = true) {
    const float raw = (std::max)(0.0f, unscaledDeltaTime);
    realTime_ += raw;
    ++frameCount_;
    unscaledDeltaTime_ = playing ? raw : 0.0f;

    if (!simulating) {
      // 止まっている間は演出の残り時間も止める（ポーズ明けに続きから）
      deltaTime_ = 0.0f;
      return;
    }

    // --- ヒットストップ ---
    float hitStopScale = 1.0f;
    if (hitStopRemaining_ > 0.0f) {
      hitStopScale = hitStopScale_;
      hitStopRemaining_ -= raw;
      if (hitStopRemaining_ < 0.0f) hitStopRemaining_ = 0.0f;
    }

    // --- スローモーション（最後の recover 秒で 1.0 へ戻す） ---
    float slowScale = 1.0f;
    if (slowRemaining_ > 0.0f) {
      slowScale = slowScale_;
      if (slowRecover_ > 0.0f && slowRemaining_ < slowRecover_) {
        const float k = 1.0f - slowRemaining_ / slowRecover_; // 0 → 1
        slowScale = slowScale_ + (1.0f - slowScale_) * k;
      }
      // ヒットストップ中はスローの残りを減らさない（止まった分だけスローが削れないように）
      if (hitStopScale >= 1.0f) {
        slowRemaining_ -= raw;
        if (slowRemaining_ < 0.0f) slowRemaining_ = 0.0f;
      }
    }

    currentScale_ = timeScale_ * slowScale * hitStopScale;
    deltaTime_ = raw * currentScale_;
    gameTime_ += deltaTime_;
  }

  /// @brief ゲーム時間の経過秒（タイムスケール込み。止まっている間は 0）
  float DeltaTime() const { return deltaTime_; }

  /// @brief 実時間の経過秒（タイムスケールを無視。編集モードでは 0）
  float UnscaledDeltaTime() const { return unscaledDeltaTime_; }

  /// @brief 今フレームに掛かっている最終倍率（timeScale × スロー × ヒットストップ）
  float CurrentScale() const { return currentScale_; }

  /// @brief 基本のタイムスケール（演出とは別に、ずっと掛けておく倍率）
  float TimeScale() const { return timeScale_; }

  /// @brief 基本のタイムスケールを設定する
  /// @param scale 倍率（0 で停止、1 で等速。負の値は 0 にする）
  void SetTimeScale(float scale) { timeScale_ = (std::max)(0.0f, scale); }

  /// @brief ヒットストップ（一瞬止める）
  /// @param seconds 止める長さ（実時間の秒）。実行中なら長い方を残す
  /// @param scale   止めている間の倍率（0 で完全停止、0.05 などで「ほぼ止まる」）
  void HitStop(float seconds, float scale = 0.0f) {
    if (!(seconds > 0.0f)) return;
    hitStopRemaining_ = (std::max)(hitStopRemaining_, seconds);
    hitStopScale_ = std::clamp(scale, 0.0f, 1.0f);
  }

  /// @brief スローモーション
  /// @param scale   スロー中の倍率（0.3 なら 0.3 倍速）
  /// @param seconds 続ける長さ（実時間の秒）
  /// @param recover 終わりの何秒で等速へなめらかに戻すか（0 ならパッと戻る。seconds 以下に丸める）
  void SlowMotion(float scale, float seconds, float recover = 0.0f) {
    if (!(seconds > 0.0f)) return;
    slowScale_ = (std::max)(0.0f, scale);
    slowRemaining_ = seconds;
    slowRecover_ = std::clamp(recover, 0.0f, seconds);
  }

  /// @brief ヒットストップ・スローを打ち切る（タイムスケールはそのまま）
  void CancelEffects() {
    hitStopRemaining_ = 0.0f;
    slowRemaining_ = 0.0f;
  }

  /// @brief ヒットストップ中か
  bool IsHitStopping() const { return hitStopRemaining_ > 0.0f; }

  /// @brief スローモーション中か
  bool IsSlowMotion() const { return slowRemaining_ > 0.0f; }

  /// @brief ゲーム開始からのゲーム時間（秒。タイムスケール込み）
  double GameTime() const { return gameTime_; }

  /// @brief アプリ起動からの実時間（秒）
  double RealTime() const { return realTime_; }

  /// @brief Tick した回数（フレーム番号）
  uint64_t FrameCount() const { return frameCount_; }

  /// @brief 再生開始・停止・シーン切り替えのときに演出と倍率を初期状態へ戻す
  void Reset() {
    timeScale_ = 1.0f;
    currentScale_ = 1.0f;
    CancelEffects();
    gameTime_ = 0.0;
    deltaTime_ = 0.0f;
  }

private:
  Time() = default;

  float deltaTime_ = 0.0f;         ///< ゲーム時間の経過秒
  float unscaledDeltaTime_ = 0.0f; ///< 実時間の経過秒
  float timeScale_ = 1.0f;         ///< 基本のタイムスケール
  float currentScale_ = 1.0f;      ///< 今フレームの最終倍率

  float hitStopRemaining_ = 0.0f;  ///< ヒットストップの残り（実時間）
  float hitStopScale_ = 0.0f;      ///< ヒットストップ中の倍率

  float slowRemaining_ = 0.0f;     ///< スローの残り（実時間）
  float slowScale_ = 1.0f;         ///< スロー中の倍率
  float slowRecover_ = 0.0f;       ///< 等速へ戻す時間

  double gameTime_ = 0.0;          ///< 累積ゲーム時間
  double realTime_ = 0.0;          ///< 累積実時間
  uint64_t frameCount_ = 0;        ///< フレーム番号
};

} // namespace Chaso
