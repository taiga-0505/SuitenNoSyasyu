#pragma once
#include "Graphics/PostProcess/PostProcess.h"
#include <algorithm>

/// @brief 水面を出入りした瞬間に、露出を一瞬だけ跳ねさせる
/// @details 着水では一瞬暗く沈み（負の EV）、浮上では一瞬白く飛ぶ（正の EV）。
///          立ち上がり kAttack 秒でピークへ、残りの時間で 2 次カーブで 0 へ戻る。
///
///          ColorGrade は Title（TitleScreenScript / TitleWeatherScript）も使っているので、
///          露出そのもの（SetGradeExposure）は触らず、PostProcess の「露出オフセット」に書く。
///          シェーダへは「シーンの露出 + オフセット」が渡るので、Title の色味を壊さずに重なる。
///
///          ColorGrade が積まれていないシーン（ステージ・セレクト・リザルト）では自分で積み、
///          色味が変わらないよう露出以外を素通しの値にしてから使う。終わったら自分で外す。
///          Dissolve（遷移の幕）が掛かっている間は何もしない。幕の後ろに ColorGrade を
///          積むと、幕の色まで明るさが変わってしまうため。
struct ExposureFlash {
  static constexpr float kAttack = 0.06f; ///< ピークまでの時間（秒）

  /// @brief 露出フラッシュを開始する（kAttack 秒でピークへ上げ、残り時間で 0 へ戻す）
  /// @param pp       対象の PostProcess（nullptr なら何もしない）
  /// @param ev       ピークの露出（EV。負で暗く、正で明るく。0 なら何もしない）
  /// @param duration 全体の長さ（秒）
  /// @note duration が kAttack 以下のとき、または Dissolve が掛かっているときは何もしない。
  void Trigger(PostProcess *pp, float ev, float duration) {
    if (!pp || ev == 0.0f || duration <= kAttack) return;
    if (pp->HasEffect(PostEffectType::Dissolve)) return;
    EnsureGrade(pp);
    peakEv_ = ev;
    duration_ = duration;
    timer_ = 0.0f;
    active_ = true;
    holding_ = false;
    pp->SetGradeExposureOffset(0.0f);
  }

  /// @brief ピークから始めて duration 秒で 0 へ戻す（立ち上がり無し）
  /// @details 白く飛んだ画面から開ける「空からの降下」用。シーンの OnCreate（切り替え直後、
  ///          Dissolve の幕が掛かっている間）に呼ぶ前提なので、Dissolve があっても積む。
  ///          SceneManager は OnCreate の後で Dissolve を末尾へ積み直すので、幕は必ず最後に掛かる。
  /// @param pp       対象の PostProcess（nullptr なら何もしない）
  /// @param ev       開始時（ピーク）の露出（EV。0 なら何もしない）
  /// @param duration 全体の長さ（秒。kAttack 以下なら何もしない）
  void TriggerFromPeak(PostProcess *pp, float ev, float duration) {
    if (!pp || ev == 0.0f || duration <= kAttack) return;
    EnsureGrade(pp);
    peakEv_ = ev;
    duration_ = duration;
    timer_ = kAttack; // 立ち上がりを飛ばして減衰から始める
    active_ = true;
    holding_ = false;
    pp->SetGradeExposureOffset(ev);
  }

  /// @brief 露出を ev で保持する（呼び出し側が毎フレーム値を決める。白へ飛ばしていく演出用）
  /// @details Update では減衰させない。止めるときは Clear。
  /// @param pp 対象の PostProcess（nullptr なら何もしない）
  /// @param ev 保持する露出オフセット（EV）
  /// @note 未開始の状態で Dissolve が掛かっている場合は開始しない。
  void Hold(PostProcess *pp, float ev) {
    if (!pp) return;
    if (!active_) {
      if (pp->HasEffect(PostEffectType::Dissolve)) return;
      EnsureGrade(pp);
      active_ = true;
    }
    holding_ = true;
    peakEv_ = ev;
    pp->SetGradeExposureOffset(ev);
  }

  /// @brief 経過時間を進め、露出オフセットをカーブに沿って更新する（毎フレーム呼ぶ）
  /// @param pp 対象の PostProcess（nullptr なら何もしない）
  /// @param dt 経過時間（秒）
  /// @note Hold 中は何もしない。duration を過ぎたら Clear して終了する。
  void Update(PostProcess *pp, float dt) {
    if (!active_ || !pp || holding_) return;
    timer_ += dt;
    if (timer_ >= duration_) {
      Clear(pp);
      return;
    }
    // 立ち上がりは smoothstep で 0→1、減衰は (1-u)^2 で 1→0。
    // 減衰を 2 次にすることで、ピーク直後に素早く戻り、終わり際はなだらかに消える。
    float k;
    if (timer_ < kAttack) {
      const float u = timer_ / kAttack;
      k = u * u * (3.0f - 2.0f * u);
    } else {
      const float u = std::clamp((timer_ - kAttack) / (duration_ - kAttack), 0.0f, 1.0f);
      k = (1.0f - u) * (1.0f - u);
    }
    pp->SetGradeExposureOffset(peakEv_ * k);
  }

  /// @brief 途中で止める（オフセットを 0 に戻し、自分で積んだ ColorGrade は外す）
  /// @param pp 対象の PostProcess（nullptr でも内部状態はリセットする）
  void Clear(PostProcess *pp) {
    if (pp && active_) {
      pp->SetGradeExposureOffset(0.0f);
      if (ownsGrade_) pp->RemoveEffect(PostEffectType::ColorGrade);
    }
    *this = ExposureFlash{};
  }

  /// @brief フラッシュ中（または Hold 中）かどうか
  /// @return 動作中なら true
  bool Active() const { return active_; }

private:
  /// @brief ColorGrade が無ければ積む（前のシーン＝Title の色味が残っているので素通しへ戻す）
  /// @param pp 対象の PostProcess（nullptr 不可）
  void EnsureGrade(PostProcess *pp) {
    if (pp->HasEffect(PostEffectType::ColorGrade)) return;
    pp->AddEffect(PostEffectType::ColorGrade);
    pp->SetGradeExposure(0.0f);
    pp->SetGradeContrast(1.0f);
    pp->SetGradeSaturation(1.0f);
    pp->SetGradeTemperature(0.0f);
    pp->SetGradeTint(0.0f);
    pp->SetGradeColorFilter(1.0f, 1.0f, 1.0f);
    pp->SetGradeLerpFactor(1.0f);
    ownsGrade_ = true;
  }

  float peakEv_ = 0.0f;
  float duration_ = 0.0f;
  float timer_ = 0.0f;
  bool active_ = false;
  bool holding_ = false;
  bool ownsGrade_ = false;
};

/// @brief 出入水フラッシュの既定値（WaterCameraFx と RailShooterController で揃える）
namespace ExposureFlashDefaults {
inline constexpr float kDiveEv = -1.2f;      ///< 着水：一瞬暗く沈む
inline constexpr float kDiveDuration = 0.35f; ///< 着水フラッシュの長さ（秒）
inline constexpr float kEmergeEv = 1.6f;     ///< 浮上：一瞬白く飛ぶ
inline constexpr float kEmergeDuration = 0.5f; ///< 浮上フラッシュの長さ（秒）
} // namespace ExposureFlashDefaults
