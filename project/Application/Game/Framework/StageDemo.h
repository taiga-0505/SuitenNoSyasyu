#pragma once
#include "ECS/TransformComponent.h"
#include "Common/Log/Log.h"
#include "Common/SceneContext.h"
#include "Scene.h"
#include "Game/Framework/TransitionDemo.h"
#include "Game/Framework/ClearRunDemo.h"
#include "Game/Framework/SharkFrenzyDemo.h"
#include "Game/Framework/WaterCameraFx.h"

#include <memory>

// ============================================================================
// 見せ用デモのステージ側（Camera エンティティの DeathSinkScript が持つ）
//
//   以前は DeathSinkScript が TransitionDemo::kMode（enum Mode）を見て
//     if (IsGameOverDemo()) { サメの群れ … } if (IsClearDemo()) { ClearRun … }
//   と分岐し、両方のデモの状態（群れ・ClearRun・タイマー）を自分のメンバに抱えていた。
//   デモの種類ごとに振る舞いが丸ごと違うので、「ステージ側のデモ」という基底クラス StageDemo を作り、
//     ClearStageDemo    : 暗転で終盤へ飛び、サメを倒してクリア（ClearRun）
//     GameOverStageDemo : サメの群れに食われて力尽きる（SharkFrenzy）
//     NoStageDemo       : デモを流さない（TransitionDemo::kEnabled が false）
//   の 3 つに分けた（ポリモーフィズム）。DeathSinkScript は StageDemo の参照経由で呼ぶだけで、
//   どのデモかを知らない。
//
//   どれを作るかは CreateStageDemo()（Factory Method）1 か所だけで決める。
//   Mode による分岐はこの関数に閉じ込め、ほかのどこにも switch を書かない。
// ============================================================================

namespace TransitionDemo {

/// @brief ステージ側のデモに毎フレーム渡す入力
/// @details 不変条件の無い受け渡し用の値の束なので struct のままにしている。
struct StageDemoFrame {
  Scene *scene = nullptr;                    ///< シーン
  SceneContext *ctx = nullptr;               ///< シーンコンテキスト（再生中かの判定）
  Entity *cam = nullptr;                     ///< Camera エンティティ（nullptr にしない）
  const TransformComponent *camTr = nullptr; ///< カメラの Transform（nullptr にしない）
  bool scriptEnabled = true;                 ///< 呼び出し側スクリプトの enabled
  bool outroStarted = false;                 ///< 呼び出し側の「力尽きた演出」がもう始まっているか
  float dt = 0.0f;                           ///< 経過時間（秒。0 より大きいときだけ呼ばれる）
};

/// @brief ステージ側のデモの基底クラス
class StageDemo {
public:
  virtual ~StageDemo() = default;

  /// @brief 状態を初期化し、前回のプレイで残ったデモのタグを消す（OnCreate で呼ぶ）
  /// @param cam Camera エンティティ（nullptr なら状態だけ初期化）
  virtual void Reset(Entity *cam) = 0;

  /// @brief デモを 1 フレーム進める（カメラが動いたあとに呼ぶ）
  /// @param f このフレームの入力
  virtual void Update(const StageDemoFrame &f) = 0;

  /// @brief 片付け（OnDestroy で呼ぶ）
  /// @param cam Camera エンティティ（nullptr ならタグは触らない）
  virtual void End(Entity *cam) = 0;

  /// @brief 画面全体に敷く暗転の濃さ（暗転を使わないデモは 0）
  /// @return 0〜1
  virtual float FadeAlpha() const { return 0.0f; }

  /// @brief デモの名前（インスペクタ・ログ表示用）
  /// @return 名前の文字列リテラル
  virtual const char *Name() const = 0;
};

/// @brief デモを流さない（TransitionDemo::kEnabled が false のとき）
/// @details 何もしないが、エディタの保存で Camera に残ったデモのタグ（無敵など）は消しておく。
class NoStageDemo final : public StageDemo {
public:
  /// @brief 残ったデモのタグを消す
  /// @param cam Camera エンティティ（nullptr なら何もしない）
  void Reset(Entity *cam) override {
    if (cam) ClearRun::ClearTags(*cam);
  }
  /// @brief 何もしない
  void Update(const StageDemoFrame &) override {}
  /// @brief 残ったデモのタグを消す
  /// @param cam Camera エンティティ（nullptr なら何もしない）
  void End(Entity *cam) override {
    if (cam) ClearRun::ClearTags(*cam);
  }
  /// @brief デモの名前
  const char *Name() const override { return "none"; }
};

/// @brief 暗転で終盤へ飛び、正面に出るサメを撃ち倒してクリアするデモ（Mode::Clear）
class ClearStageDemo final : public StageDemo {
public:
  /// @brief ClearRun の状態とタグを初期化する
  /// @param cam Camera エンティティ（nullptr なら状態だけ初期化）
  void Reset(Entity *cam) override { run_.Reset(cam); }

  /// @brief 再生中だけ ClearRun を進める（編集モードではサメを作らない）
  /// @param f このフレームの入力
  void Update(const StageDemoFrame &f) override {
    if (!f.ctx || !f.ctx->isPlaying()) return;
    run_.Update(f.scene, *f.cam, *f.camTr, WaterCameraFx::FindWaterY(f.scene), f.dt);
  }

  /// @brief タグを消し、出したサメを片付ける
  /// @param cam Camera エンティティ（nullptr ならタグは触らない）
  void End(Entity *cam) override { run_.End(cam); }

  /// @brief 暗転の濃さ
  /// @return 0〜1
  float FadeAlpha() const override { return run_.FadeAlpha(); }

  /// @brief デモの名前
  const char *Name() const override { return "clear"; }

private:
  ClearRun run_; ///< 暗転で終盤へ飛び、サメを倒してクリアする進行
};

/// @brief 最初のウェーブが始まって少しすると、サメの群れに一気に食われて力尽きるデモ（Mode::GameOver）
/// @details 以降は通常の力尽きた流れ（game_over → 沈む → "gameover" で Result へ）。
///          サメはカメラに食らいついたまま一緒に沈む。
class GameOverStageDemo final : public StageDemo {
public:
  /// @brief タイマーを初期化し、残ったデモのタグを消す
  /// @param cam Camera エンティティ（nullptr なら状態だけ初期化）
  void Reset(Entity *cam) override {
    if (cam) ClearRun::ClearTags(*cam);
    timer_ = 0.0f;
    fallbackT_ = 0.0f;
    waveSeen_ = false;
  }

  /// @brief 最初のウェーブが始まったらサメの群れを出し、噛みついたら致死ダメージを入れる
  /// @param f このフレームの入力
  void Update(const StageDemoFrame &f) override {
    if (!f.scriptEnabled) return;
    Entity &self = *f.cam;
    const TransformComponent &tr = *f.camTr;
    Scene *scene = f.scene;
    if (!f.ctx || !f.ctx->isPlaying() || !scene) return; // 編集モードではサメを作らない
    frenzy_.Prepare(scene); // 出現の瞬間に止まらないよう先に作って隠しておく

    if (!frenzy_.Started()) {
      // クリアの演出（ClearAscendScript）や力尽きた演出が先に始まっていれば出さない
      if (f.outroStarted || self.GetTagInt(kOwnerTag, 0) == 1) return;
      if (self.GetTagInt("intro_playing", 0) == 1) return; // 開始演出の間は待つ
      if (!waveSeen_) {
        // WaveManager は Intro〜Cleared の間 wave_busy を 1 にする。最初に立ったものが WAVE 1
        const Entity *wm = scene->FindEntityByName("WaveManager");
        fallbackT_ += f.dt;
        if ((wm && wm->GetTagInt("wave_busy", 0) == 1) || fallbackT_ >= kWaveFallbackTime) {
          waveSeen_ = true;
          timer_ = 0.0f;
          Log::Print("[DeathSinkScript] demo: wave started, sharks incoming");
        }
        return;
      }
      timer_ += f.dt;
      if (timer_ >= kFrenzyDelay) {
        frenzy_.Begin(tr, WaterCameraFx::FindWaterY(scene));
      }
      return;
    }

    frenzy_.Update(tr, f.dt);
    // 噛みつかれたら力尽きるまで致死ダメージを積む（無敵時間中に弾かれても次のフレームで通る）
    if (frenzy_.Bitten() && !f.outroStarted && self.GetTagInt("game_over", 0) != 1) {
      self.SetTag("pending_damage", kLethalDamage);
    }
  }

  /// @brief 残ったデモのタグを消し、サメの群れを片付ける
  /// @param cam Camera エンティティ（nullptr ならタグは触らない）
  void End(Entity *cam) override {
    if (cam) ClearRun::ClearTags(*cam);
    frenzy_.Destroy();
  }

  /// @brief デモの名前
  const char *Name() const override { return "gameover"; }

private:
  static constexpr const char *kOwnerTag = "result_transition_owner"; ///< 決着後の演出を担当しているタグ

  SharkFrenzy frenzy_;      ///< サメの群れ
  float timer_ = 0.0f;      ///< 最初のウェーブが始まってからの経過（秒）
  float fallbackT_ = 0.0f;  ///< 開始演出が終わってからの経過（ウェーブが来ないときの保険）
  bool waveSeen_ = false;   ///< 最初のウェーブの開始を見たか
};

/// @brief 今のデモ設定（kEnabled / kMode）に合ったステージ側のデモを作る（Factory Method）
/// @details Mode による分岐はここ 1 か所だけ。呼び出し側は StageDemo の参照で扱うので、
///          デモの種類を増やしても DeathSinkScript は変更しなくてよい。
/// @return 作ったデモ（デモを流さないときは NoStageDemo）
inline std::unique_ptr<StageDemo> CreateStageDemo() {
  if constexpr (IsClearDemo()) {
    return std::make_unique<ClearStageDemo>();
  } else if constexpr (IsGameOverDemo()) {
    return std::make_unique<GameOverStageDemo>();
  } else {
    return std::make_unique<NoStageDemo>();
  }
}

} // namespace TransitionDemo
