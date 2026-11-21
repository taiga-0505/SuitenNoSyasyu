#pragma once

// ============================================================================
// Tween
// ----------------------------------------------------------------------------
// 値を「今の値 → 目標値」へ、指定した時間とイージングで動かす仕組み。
//
// 使い方（スクリプトから。Tweens() は自分のエンティティに付いた TweenComponent の中身）:
//   auto* tr = GetComponent<TransformComponent>();
//   Tweens().To(tr->scale, {1.2f, 1.2f, 1.2f}, 0.15f, Chaso::EaseType::OutBack)
//           .Loop(2, Chaso::LoopMode::Yoyo);                    // ぽよんと膨らんで戻る
//   Tweens().To(sprite->color, {1, 1, 1, 0}, 0.5f).Delay(1.0f)
//           .OnComplete([this] { GetEntity()->Destroy(); });    // 1 秒後に 0.5 秒でフェードして消える
//   Tweens().After(2.0f, [this] { RequestTransition("next"); }); // 2 秒後に呼ぶ（タイマー）
//   Tweens().Value(0.3f, [&](float k) { power = k * 100.0f; }); // 任意の処理に 0〜1 を渡す
//   Tweens().To(...).Unscaled();                                 // ヒットストップ・スローを無視（UI 用）
//
// 注意:
//   - To() に渡した変数は、Tween が終わるまで生きていること。自分のエンティティの
//     コンポーネントなら、エンティティと一緒に TweenComponent も消えるので安全。
//     他のエンティティの値を動かすなら、そのエンティティに Tween を付けること。
//   - To() の戻り値（TweenHandle）はその場で .Delay() などをつなぐためのもの。
//     後で止めたいときは .Id() を保存しておき、Kill(id) を呼ぶ。
//   - 開始値は「動き始めた瞬間（Delay の後）」の値。From() で明示もできる。
// ============================================================================

#include "Easing.h"
#include "Math/MathTypes.h"
#include <algorithm>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace Chaso {

/// @brief Tween の識別番号（0 は無効）
using TweenId = uint32_t;

/// @enum LoopMode
/// @brief 繰り返し方
enum class LoopMode {
  Restart, ///< 毎回最初から（0→1, 0→1, ...）
  Yoyo,    ///< 行って戻る（0→1, 1→0, ...）
};

class TweenRunner;

/// @class TweenHandle
/// @brief 作った Tween に設定をつなぐための一時的な窓口
/// @details TweenRunner が消えると使えなくなるので、変数に保存しないこと（保存するのは Id()）。
class TweenHandle {
public:
  TweenHandle() = default;
  TweenHandle(TweenRunner *runner, TweenId id) : runner_(runner), id_(id) {}

  /// @brief 動き始めるまでの待ち時間（秒）
  TweenHandle &Delay(float seconds);
  /// @brief イージングを変える
  TweenHandle &SetEase(EaseType type);
  /// @brief 繰り返す
  /// @param count 再生回数の合計（-1 で無限。1 なら繰り返さない）
  /// @param mode  繰り返し方
  TweenHandle &Loop(int count = -1, LoopMode mode = LoopMode::Restart);
  /// @brief 最後まで動き終わったときに呼ぶ処理（Kill されたときは呼ばない）
  TweenHandle &OnComplete(std::function<void()> fn);
  /// @brief タイムスケール（ヒットストップ・スロー）を無視して実時間で動かす
  TweenHandle &Unscaled(bool unscaled = true);
  /// @brief 開始値を明示する（float 版）
  TweenHandle &From(float v);
  /// @brief 開始値を明示する（Vector2 版）
  TweenHandle &From(const RC::Vector2 &v);
  /// @brief 開始値を明示する（Vector3 版）
  TweenHandle &From(const RC::Vector3 &v);
  /// @brief 開始値を明示する（Vector4 版。色など）
  TweenHandle &From(const RC::Vector4 &v);

  /// @brief 識別番号（後で Kill するために保存する）
  TweenId Id() const { return id_; }
  /// @brief 有効な Tween を指しているか
  bool IsValid() const { return runner_ && id_ != 0; }

private:
  TweenHandle &FromFloats_(const float *v, int n);
  TweenRunner *runner_ = nullptr;
  TweenId id_ = 0;
};

/// @class TweenRunner
/// @brief Tween の入れ物。Update(dt) を呼ぶと中の Tween がすべて進む
class TweenRunner {
public:
  TweenRunner() = default;
  TweenRunner(const TweenRunner &) = delete;            // TweenHandle が指しているのでコピーさせない
  TweenRunner &operator=(const TweenRunner &) = delete;

  /// @brief float を動かす
  TweenHandle To(float &target, float to, float duration, EaseType ease = EaseType::OutQuad) {
    return Add_(&target, &to, 1, duration, ease);
  }
  /// @brief Vector2 を動かす
  TweenHandle To(RC::Vector2 &target, const RC::Vector2 &to, float duration,
                 EaseType ease = EaseType::OutQuad) {
    static_assert(sizeof(RC::Vector2) == sizeof(float) * 2, "Vector2 は float 2 個の想定");
    return Add_(&target.x, &to.x, 2, duration, ease);
  }
  /// @brief Vector3 を動かす（位置・回転・スケール）
  TweenHandle To(RC::Vector3 &target, const RC::Vector3 &to, float duration,
                 EaseType ease = EaseType::OutQuad) {
    static_assert(sizeof(RC::Vector3) == sizeof(float) * 3, "Vector3 は float 3 個の想定");
    return Add_(&target.x, &to.x, 3, duration, ease);
  }
  /// @brief Vector4 を動かす（色など）
  TweenHandle To(RC::Vector4 &target, const RC::Vector4 &to, float duration,
                 EaseType ease = EaseType::OutQuad) {
    static_assert(sizeof(RC::Vector4) == sizeof(float) * 4, "Vector4 は float 4 個の想定");
    return Add_(&target.x, &to.x, 4, duration, ease);
  }

  /// @brief 任意の処理に 0〜1（イージング後）を毎フレーム渡す
  /// @param duration 長さ（秒）
  /// @param fn       void(float eased)
  /// @param ease     イージング
  TweenHandle Value(float duration, std::function<void(float)> fn, EaseType ease = EaseType::Linear) {
    TweenHandle h = Add_(nullptr, nullptr, 0, duration, ease);
    if (Tween *tw = Find_(h.Id())) tw->custom = std::move(fn);
    return h;
  }

  /// @brief seconds 秒後に fn を呼ぶ（タイマー）
  TweenHandle After(float seconds, std::function<void()> fn) {
    TweenHandle h = Add_(nullptr, nullptr, 0, 0.0f, EaseType::Linear);
    if (Tween *tw = Find_(h.Id())) {
      tw->delay = (std::max)(0.0f, seconds);
      tw->onComplete = std::move(fn);
    }
    return h;
  }

  /// @brief 止める
  /// @param id       止める Tween
  /// @param complete true なら最後の値まで進めて OnComplete も呼ぶ
  void Kill(TweenId id, bool complete = false) {
    Tween *tw = Find_(id);
    if (!tw || tw->dead) return;
    if (complete) {
      if (!tw->started) Start_(*tw);
      Apply_(*tw, EndProgress_(*tw));
      Finish_(*tw);
    } else {
      tw->dead = true;
    }
  }

  /// @brief すべて止める（OnComplete は呼ばない）
  void KillAll() {
    for (auto &tw : tweens_) tw.dead = true;
    for (auto &tw : pending_) tw.dead = true;
  }

  /// @brief まだ動いているか
  bool IsActive(TweenId id) const {
    for (const auto &tw : tweens_) if (tw.id == id) return !tw.dead;
    for (const auto &tw : pending_) if (tw.id == id) return !tw.dead;
    return false;
  }

  /// @brief 動いている Tween の数
  size_t Count() const {
    size_t n = 0;
    for (const auto &tw : tweens_) n += tw.dead ? 0 : 1;
    for (const auto &tw : pending_) n += tw.dead ? 0 : 1;
    return n;
  }

  /// @brief 進める
  /// @param deltaTime         ゲーム時間の経過秒（タイムスケール込み）
  /// @param unscaledDeltaTime 実時間の経過秒（Unscaled() の Tween が使う）
  void Update(float deltaTime, float unscaledDeltaTime) {
    // 前のフレームに足された分を合流させる（Update 中に足された分は次のフレームから動く）
    if (!pending_.empty()) {
      tweens_.insert(tweens_.end(), std::make_move_iterator(pending_.begin()),
                     std::make_move_iterator(pending_.end()));
      pending_.clear();
    }
    for (size_t i = 0; i < tweens_.size(); ++i) {
      Tween &tw = tweens_[i];
      if (tw.dead) continue;
      float dt = tw.unscaled ? unscaledDeltaTime : deltaTime;
      if (!(dt > 0.0f)) continue;

      if (tw.delay > 0.0f) {
        tw.delay -= dt;
        if (tw.delay > 0.0f) continue;
        dt = -tw.delay; // 待ち時間を超えた分だけ動かす
        tw.delay = 0.0f;
      }
      if (!tw.started) Start_(tw);

      tw.elapsed += dt;
      Step_(tw);
    }
    tweens_.erase(std::remove_if(tweens_.begin(), tweens_.end(), [](const Tween &t) { return t.dead; }),
                  tweens_.end());
  }

private:
  friend class TweenHandle;

  struct Tween {
    TweenId id = 0;
    float *target = nullptr;      ///< 動かす値の先頭（nullptr なら custom / タイマー）
    int comps = 0;                ///< 値の個数（1〜4）
    float from[4] = {};           ///< 開始値
    float to[4] = {};             ///< 目標値
    bool hasFrom = false;         ///< From() で開始値が指定されたか
    std::function<void(float)> custom; ///< Value() の処理
    std::function<void()> onComplete;  ///< 終わったときの処理
    float duration = 0.0f;        ///< 1 回分の長さ
    float delay = 0.0f;           ///< 残りの待ち時間
    float elapsed = 0.0f;         ///< 今の回の経過時間
    EaseType ease = EaseType::OutQuad;
    int loops = 1;                ///< 残りの再生回数（-1 で無限）
    LoopMode loopMode = LoopMode::Restart;
    bool reversed = false;        ///< Yoyo の戻り中か
    bool started = false;         ///< 開始値を取り込んだか
    bool unscaled = false;        ///< 実時間で動かすか
    bool dead = false;            ///< 終了済み（次の Update で取り除く）
  };

  TweenHandle Add_(float *target, const float *to, int comps, float duration, EaseType ease) {
    Tween tw;
    tw.id = ++nextId_;
    if (tw.id == 0) tw.id = ++nextId_; // 一周して 0 になったら飛ばす
    tw.target = target;
    tw.comps = comps;
    for (int i = 0; i < comps; ++i) tw.to[i] = to[i];
    tw.duration = (std::max)(0.0f, duration);
    tw.ease = ease;
    pending_.push_back(std::move(tw));
    return TweenHandle(this, pending_.back().id);
  }

  Tween *Find_(TweenId id) {
    if (id == 0) return nullptr;
    for (auto &tw : tweens_) if (tw.id == id) return &tw;
    for (auto &tw : pending_) if (tw.id == id) return &tw;
    return nullptr;
  }

  static void Start_(Tween &tw) {
    if (!tw.hasFrom && tw.target) {
      for (int i = 0; i < tw.comps; ++i) tw.from[i] = tw.target[i];
    }
    tw.started = true;
  }

  static void Apply_(Tween &tw, float progress) {
    const float k = Ease::Evaluate(tw.ease, progress);
    if (tw.target) {
      for (int i = 0; i < tw.comps; ++i) tw.target[i] = tw.from[i] + (tw.to[i] - tw.from[i]) * k;
    }
    if (tw.custom) tw.custom(k);
  }

  /// @brief 最後の回が終わったときの進み具合（Yoyo で戻りの回なら 0）
  static float EndProgress_(const Tween &tw) {
    if (tw.loopMode != LoopMode::Yoyo || tw.loops < 0) return tw.reversed ? 0.0f : 1.0f;
    // 残り回数から最後の向きを求める（今の向きから loops-1 回反転した先）
    const bool lastReversed = ((tw.loops - 1) % 2 == 0) ? tw.reversed : !tw.reversed;
    return lastReversed ? 0.0f : 1.0f;
  }

  void Finish_(Tween &tw) {
    tw.dead = true;
    if (tw.onComplete) {
      auto fn = std::move(tw.onComplete); // 呼んでいる途中に配列が動いても大丈夫なように取り出す
      tw.onComplete = nullptr;
      fn();
    }
  }

  void Step_(Tween &tw) {
    // 長さ 0 の Tween（タイマーなど）が無限ループすると止まらないので、回数に上限を設ける
    for (int guard = 0; guard < 1024; ++guard) {
      const float p = (tw.duration > 0.0f) ? (std::min)(tw.elapsed / tw.duration, 1.0f) : 1.0f;
      Apply_(tw, tw.reversed ? 1.0f - p : p);
      if (tw.duration > 0.0f && tw.elapsed < tw.duration) return;

      // 1 回分終わった
      if (tw.loops > 0) --tw.loops;
      if (tw.loops == 0) {
        Finish_(tw); // tw はこの後触らない（OnComplete が新しい Tween を足しても pending_ に入る）
        return;
      }
      tw.elapsed = (tw.duration > 0.0f) ? tw.elapsed - tw.duration : 0.0f;
      if (tw.loopMode == LoopMode::Yoyo) tw.reversed = !tw.reversed;
      if (tw.duration <= 0.0f) return; // 長さ 0 の無限ループは 1 フレーム 1 回
    }
  }

  std::vector<Tween> tweens_;  ///< 動いている Tween
  std::vector<Tween> pending_; ///< 足されたばかりの Tween（次の Update で合流）
  TweenId nextId_ = 0;         ///< 最後に振った番号
};

// ----------------------------------------------------------------------------
// TweenHandle の実装（TweenRunner の完全型が必要なのでここに置く）
// ----------------------------------------------------------------------------
inline TweenHandle &TweenHandle::Delay(float seconds) {
  if (runner_) if (auto *tw = runner_->Find_(id_)) tw->delay = (std::max)(0.0f, seconds);
  return *this;
}
inline TweenHandle &TweenHandle::SetEase(EaseType type) {
  if (runner_) if (auto *tw = runner_->Find_(id_)) tw->ease = type;
  return *this;
}
inline TweenHandle &TweenHandle::Loop(int count, LoopMode mode) {
  if (runner_) if (auto *tw = runner_->Find_(id_)) {
    tw->loops = (count == 0) ? 1 : count;
    tw->loopMode = mode;
  }
  return *this;
}
inline TweenHandle &TweenHandle::OnComplete(std::function<void()> fn) {
  if (runner_) if (auto *tw = runner_->Find_(id_)) tw->onComplete = std::move(fn);
  return *this;
}
inline TweenHandle &TweenHandle::Unscaled(bool unscaled) {
  if (runner_) if (auto *tw = runner_->Find_(id_)) tw->unscaled = unscaled;
  return *this;
}
inline TweenHandle &TweenHandle::FromFloats_(const float *v, int n) {
  if (runner_) if (auto *tw = runner_->Find_(id_)) {
    for (int i = 0; i < n && i < tw->comps; ++i) tw->from[i] = v[i];
    tw->hasFrom = true;
  }
  return *this;
}
inline TweenHandle &TweenHandle::From(float v) { return FromFloats_(&v, 1); }
inline TweenHandle &TweenHandle::From(const RC::Vector2 &v) { return FromFloats_(&v.x, 2); }
inline TweenHandle &TweenHandle::From(const RC::Vector3 &v) { return FromFloats_(&v.x, 3); }
inline TweenHandle &TweenHandle::From(const RC::Vector4 &v) { return FromFloats_(&v.x, 4); }

} // namespace Chaso
