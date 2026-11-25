#pragma once
#include "ECS/TransformComponent.h"
#include "ECS/ModelRendererComponent.h"
#include "ECS/ColliderComponent.h"
#include "ECS/NativeScriptComponent.h"
#include "Common/Log/Log.h"
#include "Common/SceneContext.h"
#include "Scene.h"
#include "Game/Framework/TransitionDemo.h"
#include "Game/Framework/ClearRunDemo.h"
#include "Game/Framework/GameSession.h"
#include "Game/Framework/StateMachine.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

// ============================================================================
// 見せ用デモ（Mode::Effects）：パーティクル・エフェクトの見せ場を順に流す
//
//   DeathSinkScript（ステージの Camera エンティティ）が TransitionDemo::IsEffectsDemo() のときだけ使う
//   （StageDemo.h の EffectsStageDemo 経由）。開始演出は DeepRiseIntroScript が飛ばす（SkipsStageIntro）。
//   レールは出発点で止めたまま（demo_rail_hold）、見せる相手（宝箱・鳥・タコ・サメ・船）はカメラの前方へ
//   その都度出す。出す敵はゲームのウェーブと同じ組み立て（WaveSpawnerScript::SpawnOne と同じ
//   Transform・モデル・球コライダー・スクリプト）で、設定値も 1 面のスポナーと同じ値を元にしている。
//   ほかのスクリプトとは ClearRunDemo.h と同じく Camera のタグだけでやり取りする（型は知らない）:
//
//     demo_invincible / demo_look / demo_look_yaw_mrad / demo_look_pitch_mrad / demo_fire /
//     demo_rail_hold / demo_skip_waves : ClearRunDemo.h の説明を参照
//     demo_weapon : 1:通常 / 2:拡散 / 3:重い弾 を持たせる（RailShooterController）
//
//   流れ（見せる順。見た目が静かなものから派手なものへ並べ、最後に船の撃沈で締める）:
//     WaitStart      : 操作できるようになるのを待つ（kFxSettle 秒）
//   どれも連打はせず 1 発ずつ撃つ（AimShootOnce。外れたときだけ撃ち直す）。敵の体力は 1 にしてあり、1 発で倒れる。
//
//     InkWater       : 水面へ 1 発撃ち、墨がにじむのを見せる。武器を 通常 → 拡散 → 重い弾 と替えて左・中央・右へ 1 発ずつ
//     InkWaterAfter  : にじんでいく墨を少し見せる
//     Chest          : 前方の宝箱（無ければ出す）を 1 発撃って開ける → ChestAfter で開封の演出を見せる
//     Bird           : 鳥を出し、近づいてきたところを偏差撃ちで 1 発で落とす → BirdAfter で羽毛が散って落ちるのを見せる
//     OctoShootDown  : タコを出し、吐いた墨玉を 1 発で撃ち落とす → OctoShootDownAfter
//     OctoInkHit     : 次の墨玉はあえて受ける（画面に墨が貼り付く。1 回だけ）→ OctoInkLinger
//     OctoKill       : タコを 1 発で倒す（この間に吐かれた墨玉は消す）→ OctoAfter
//     SharkHit       : 無敵を外してサメを出し、突進を 1 回だけ受ける（被弾の揺れ・ヒットストップ・HP 減少）→ SharkAfterHit
//     SharkKill      : 引き返して浮いてきたサメを 1 発で撃ち倒す（2 回目の突進の前に必ず倒す）→ SharkAfter
//     Ship           : 重い弾に持ち替えて船を出し、1 発で撃沈する（ヒットストップ＋スロー）→ ShipSink で沈むのを見せる
//     Outro          : Dive 遷移と同じ深海色へ暗転し、暗転しきったら WantsExit() を立てる
//                      （DeathSinkScript が遷移表のきっかけ kEffectsExitTrigger で Select へ遷移する）
//     Done           : 何もしない
//
//   どの見せ場にも「うまくいかなかったときの保険」（強制撃破・直接ダメージなど）を時間で入れてあり、
//   流れが途中で止まらないようにしている。
//
//   実装（State パターン）: ClearRunDemo.h と同じく「段階 1 つ = 状態クラス 1 つ」。次の段階は各状態クラスが
//   EffectsShowcase::Enter で決める。決着後の演出が始まった場合（力尽きたなど）は Update の入口で Done にする。
// ============================================================================

namespace TransitionDemo {

/// @brief エフェクトを見せるデモのステージ側の進行
class EffectsShowcase {
public:
  /// @brief 最初の段階（操作できるようになるのを待つ）で作る
  EffectsShowcase() {
    fsm_.ChangeState<WaitStartState>();
    fsm_.ApplyPending(*this);
  }

  /// @brief 状態を初期化し、前回のタグを消す（OnCreate で呼ぶ）
  /// @param cam Camera エンティティ（nullptr なら状態だけ初期化）
  void Reset(Entity *cam) {
    fsm_.ChangeState<WaitStartState>();
    fsm_.ApplyPending(*this);
    t_ = 0.0f;
    fade_ = 0.0f;
    exit_ = false;
    vulnerable_ = false;
    weapon_ = 1;
    inkIdx_ = 0;
    shots_ = 0;
    shotWait_ = 0.0f;
    target_.Clear();
    sub_.Clear();
    if (cam) ClearRun::ClearTags(*cam);
  }

  /// @brief デモを進める（再生中の毎フレーム、カメラが動いたあとに呼ぶ）
  /// @param scene  シーン
  /// @param ctx    シーンコンテキスト（生成する敵のスクリプトへ渡す。nullptr 可）
  /// @param cam    Camera エンティティ
  /// @param tr     カメラの Transform
  /// @param waterY 水面の高さ
  /// @param dt     経過時間（秒）
  void Update(Scene *scene, SceneContext *ctx, Entity &cam, const TransformComponent &tr, float waterY, float dt) {
    if (!scene || fsm_.IsIn<DoneState>()) return;

    // 決着後の演出（力尽きた演出など）が始まったら手を引く（どの段階からでも起きる割り込み）
    if (cam.GetTagInt(kOwnerTag, 0) == 1 || cam.GetTagInt("game_over", 0) == 1) {
      ClearRun::ClearTags(cam);
      fade_ = 0.0f;
      fsm_.ChangeState<DoneState>();
      fsm_.ApplyPending(*this);
      Log::Print("[EffectsShowcase] outro started, done");
      return;
    }
    Preload(*scene);

    // レールは出発点で止めたまま（ウェーブも始めない）。被弾してよいのはサメの突進を受ける間だけ
    cam.SetTag("demo_rail_hold", 1);
    cam.SetTag("demo_skip_waves", 1);
    cam.SetTag("demo_invincible", vulnerable_ ? 0 : 1);
    cam.SetTag("demo_weapon", weapon_);
    t_ += dt;

    // 段階ごとの処理と次の段階の決定は、現在の状態クラスに任せる
    frame_ = Frame{scene, ctx, &cam, &tr, waterY, dt};
    fsm_.Update(*this, dt);
    frame_ = Frame{};
  }

  /// @brief 暗転の濃さ（呼び出し側が OnOverlayRender で Dive 遷移の色を画面全体に敷く）
  /// @return 0〜1
  float FadeAlpha() const { return fade_; }

  /// @brief 見せ終わって、シーンを抜けたいか（呼び出し側が遷移表のきっかけ kEffectsExitTrigger で遷移する）
  /// @return 抜けたいなら true
  bool WantsExit() const { return exit_; }

  /// @brief 現在の段階名（インスペクタ・ログ表示用）
  /// @return 段階名
  const char *PhaseName() const { return fsm_.CurrentName(); }

  /// @brief 片付け（OnDestroy で呼ぶ）
  /// @param cam Camera エンティティ（nullptr ならタグは触らない）
  void End(Entity *cam) {
    if (cam) ClearRun::ClearTags(*cam);
    for (auto &w : spawned_) {
      if (auto e = w.lock()) {
        if (!e->IsPendingDestroy()) e->Destroy();
      }
    }
    spawned_.clear();
    target_.Clear();
    sub_.Clear();
    fsm_.ChangeState<DoneState>();
    fsm_.ApplyPending(*this);
    fade_ = 0.0f;
  }

private:
  /// @brief 照準を合わせたときに撃つかどうか
  enum class Fire {
    Hold,    ///< 撃たない（目で追うだけ）
    Aligned, ///< ずれが kFxAimTolerance 以内なら撃つ
  };

  /// @brief 追いかける相手と、その速度の見積もり（偏差撃ち用）
  struct Tracker {
    std::weak_ptr<Entity> e;   ///< 相手
    RC::Vector3 prev{};        ///< 前のフレームの位置
    RC::Vector3 vel{};         ///< 速度（なめらかにした見積もり）
    RC::Vector3 last{};        ///< 最後に見えた位置
    bool has = false;          ///< prev が入っているか

    /// @brief 相手を差し替える
    /// @param p 新しい相手
    void Set(const std::shared_ptr<Entity> &p) {
      e = p;
      has = false;
      vel = {0.0f, 0.0f, 0.0f};
    }
    /// @brief 相手を外す
    void Clear() { Set(nullptr); }
    /// @brief 生きている相手を返す
    /// @return 相手（消えていれば nullptr）
    std::shared_ptr<Entity> Get() const {
      auto s = e.lock();
      return (s && !s->IsPendingDestroy()) ? s : nullptr;
    }
    /// @brief このフレームの位置を記録し、速度の見積もりを更新する（1 フレーム 1 回）
    /// @param p 位置
    /// @param dt 経過時間（秒）
    void Sample(const RC::Vector3 &p, float dt) {
      if (has && dt > 1e-4f) {
        const RC::Vector3 v = {(p.x - prev.x) / dt, (p.y - prev.y) / dt, (p.z - prev.z) / dt};
        const float k = (std::min)(1.0f, dt * 8.0f);
        vel = {vel.x + (v.x - vel.x) * k, vel.y + (v.y - vel.y) * k, vel.z + (v.z - vel.z) * k};
      }
      prev = p;
      last = p;
      has = true;
    }
  };

  /// @brief 状態クラスが使う、このフレームの入力（Update の間だけ有効）
  struct Frame {
    Scene *scene = nullptr;                 ///< シーン
    SceneContext *ctx = nullptr;            ///< シーンコンテキスト
    Entity *cam = nullptr;                  ///< Camera エンティティ
    const TransformComponent *tr = nullptr; ///< カメラの Transform
    float waterY = 0.0f;                    ///< 水面の高さ
    float dt = 0.0f;                        ///< 経過時間（秒）
  };

  /// @brief 生成する敵に載せるスクリプト 1 本
  struct ScriptSpec {
    const char *name;      ///< REGISTER_SCRIPT の登録名
    nlohmann::json params; ///< pendingData（Deserialize へ渡す）
  };

  // ---- 状態クラス（private な入れ子クラスなので、EffectsShowcase の private メンバをそのまま使える）----
  using State = IState<EffectsShowcase>;
  using Machine = StateMachine<EffectsShowcase>;

  /// @brief 操作できるようになる（開始演出が終わり、少し落ち着く）のを待つ
  class WaitStartState : public State {
  public:
    /// @brief 落ち着いたら InkWater へ
    void OnUpdate(EffectsShowcase &o, Machine &m, float) override {
      if (o.frame_.cam->GetTagInt("intro_playing", 0) == 1) o.t_ = 0.0f;
      AimCenter(*o.frame_.cam);
      if (o.t_ >= kFxSettle) {
        o.Enter<InkWaterState>(m);
        Log::Print("[EffectsShowcase] start");
      }
    }
    const char *Name() const override { return "WaitStart"; }
  };

  /// @brief 水面へ 1 発撃ち、墨がにじむのを見せる。武器を 通常 → 拡散 → 重い弾 と替えて左・中央・右へ 1 発ずつ
  class InkWaterState : public State {
  public:
    /// @brief 照準を寄せて 1 発だけ撃ち、にじむのを見せたら次の武器へ。3 発撃ち終えたら InkWaterAfter へ
    void OnUpdate(EffectsShowcase &o, Machine &m, float) override {
      const int idx = (std::clamp)(o.inkIdx_, 0, 2);
      o.weapon_ = idx + 1;
      const float side = static_cast<float>(idx - 1) * kFxInkSpotSide;
      const bool aligned = AimAt(*o.frame_.cam, *o.frame_.tr, o.OnWater(kFxInkDistance, side), Fire::Hold);
      // 照準が寄ったら（寄りきらなくても少し待ったら）1 発だけ撃つ
      if (o.shots_ == 0 && o.t_ >= kFxInkAimLead && (aligned || o.t_ >= kFxInkAimLead + 0.4f)) {
        o.frame_.cam->SetTag("demo_fire", 1); // 次のフレームで RailShooterController が 1 発撃つ
        o.shots_ = 1;
        o.shotAt_ = o.t_;
      }
      if (o.shots_ > 0 && o.t_ - o.shotAt_ >= kFxInkWatch) {
        if (++o.inkIdx_ >= 3) {
          o.weapon_ = 1;
          o.Enter<InkWaterAfterState>(m);
        } else {
          o.Enter<InkWaterState>(m); // 次の武器で、次の狙う所へ
        }
      }
    }
    const char *Name() const override { return "InkWater"; }
  };

  /// @brief 撃つのをやめて、にじんでいく墨を見せる
  class InkWaterAfterState : public State {
  public:
    /// @brief 少し待ったら宝箱を用意して Chest へ
    void OnUpdate(EffectsShowcase &o, Machine &m, float) override {
      AimAt(*o.frame_.cam, *o.frame_.tr, o.OnWater(kFxInkDistance, 0.0f), Fire::Hold);
      if (o.t_ >= kFxInkAfter) {
        o.target_.Set(o.FindOrSpawnChest());
        o.hpBefore_ = -1; // 宝箱の HP は ChestState が最初のフレームに読む
        o.Enter<ChestState>(m);
      }
    }
    const char *Name() const override { return "InkWaterAfter"; }
  };

  /// @brief 宝箱を 1 発撃って開ける
  class ChestState : public State {
  public:
    /// @brief 開いたら ChestAfter へ。シーンの宝箱は何発も要るので、1 発当たったらそのまま開ける
    void OnUpdate(EffectsShowcase &o, Machine &m, float) override {
      auto c = o.target_.Get();
      if (!c || c->GetTagInt("is_item", 0) != 1) {
        o.Enter<ChestAfterState>(m);
        Log::Print("[EffectsShowcase] chest opened");
        return;
      }
      const int hp = c->GetTagInt("current_hp", -1); // DamageableScript が毎フレーム公開する
      if (o.hpBefore_ < 0) o.hpBefore_ = hp;
      if (hp >= 0 && o.hpBefore_ >= 0 && hp < o.hpBefore_) {
        c->SetTag("pending_damage", 999); // 1 発当たった。残りの HP に関係なく開ける
        return;
      }
      o.AimShootOnce(o.target_, 0.0f, 3.0f, true);
      if (o.ShotsUsedUp() || o.t_ >= kFxChestTimeout) c->SetTag("pending_damage", 999); // 保険
    }
    const char *Name() const override { return "Chest"; }
  };

  /// @brief 開いた宝箱（跳ねて回って縮む・泡）を見せる
  class ChestAfterState : public State {
  public:
    /// @brief 少し待ったら鳥を出して Bird へ
    void OnUpdate(EffectsShowcase &o, Machine &m, float) override {
      o.HoldOnLast(Fire::Hold);
      if (o.t_ >= kFxChestAfter) {
        o.target_.Set(o.SpawnBird());
        o.Enter<BirdState>(m);
      }
    }
    const char *Name() const override { return "ChestAfter"; }
  };

  /// @brief 鳥が近づいてきたら偏差撃ちで落とす
  class BirdState : public State {
  public:
    /// @brief 落ちたら BirdAfter へ（落とせなければ強制的に倒す）
    void OnUpdate(EffectsShowcase &o, Machine &m, float) override {
      auto b = o.target_.Get();
      if (!b || b->HasTag("enemy_defeated")) {
        o.Enter<BirdAfterState>(m);
        Log::Print("[EffectsShowcase] bird down");
        return;
      }
      const bool inRange = o.target_.has && o.Distance(o.target_.last) <= kFxBirdFireRange;
      o.AimShootOnce(o.target_, 0.2f, 3.0f, inRange);
      if (o.ShotsUsedUp() || o.t_ >= kFxBirdTimeout) b->SetTag("pending_damage", 999); // 保険
    }
    const char *Name() const override { return "Bird"; }
  };

  /// @brief 落ちていく鳥と散る羽毛を見せる
  class BirdAfterState : public State {
  public:
    /// @brief 少し待ったらタコを出して OctoShootDown へ
    void OnUpdate(EffectsShowcase &o, Machine &m, float) override {
      o.HoldOnLast(Fire::Hold);
      if (o.t_ >= kFxBirdAfter) {
        o.target_.Set(o.SpawnOctopus());
        o.sub_.Clear();
        o.Enter<OctoShootDownState>(m);
      }
    }
    const char *Name() const override { return "BirdAfter"; }
  };

  /// @brief タコが吐いた墨玉を撃ち落とす
  class OctoShootDownState : public State {
  public:
    /// @brief 墨玉を追って撃つ。消えたら、撃ち落としたなら OctoShootDownAfter、当たってしまったなら OctoInkLinger へ
    void OnUpdate(EffectsShowcase &o, Machine &m, float) override {
      if (!o.sub_.Get()) {
        if (o.sub_.has) { // 追っていた墨玉が消えた
          const bool hitMe = o.Distance(o.sub_.last) <= kFxBlobHitDistance;
          o.sub_.Clear();
          if (hitMe) {
            o.Enter<OctoInkLingerState>(m);
            Log::Print("[EffectsShowcase] blob hit before shot down");
          } else {
            o.Enter<OctoShootDownAfterState>(m);
            Log::Print("[EffectsShowcase] blob shot down");
          }
          return;
        }
        o.sub_.Set(o.FindNearestNamed("InkBlob"));
      }
      if (o.sub_.Get()) {
        // 水面から出てから撃つ（水中へ撃つと、下にいるタコに当たって先に倒してしまうことがある）
        const bool aboveWater = o.sub_.has && o.sub_.last.y > o.frame_.waterY + 0.3f;
        o.AimShootOnce(o.sub_, 0.0f, 3.0f, aboveWater);
        // 保険：近くまで来ても落とせていなければ、飛んでいる弾を墨玉へ寄せて撃ち落としを起こす
        if (o.Distance(o.sub_.last) <= kFxBlobRescue) o.PullBulletTo(o.sub_.last);
      } else {
        o.AimTrack(o.target_, 0.0f, Fire::Hold, 3.0f); // 吐くまではタコを目で追う
      }
      if (o.t_ >= kFxOctoShootDownTimeout && !o.sub_.Get()) o.Enter<OctoInkHitState>(m); // 保険
    }
    const char *Name() const override { return "OctoShootDown"; }
  };

  /// @brief 墨玉が弾けたところを見せる
  class OctoShootDownAfterState : public State {
  public:
    /// @brief 少し待ったら OctoInkHit へ
    void OnUpdate(EffectsShowcase &o, Machine &m, float) override {
      o.AimTrack(o.target_, 0.0f, Fire::Hold, 3.0f);
      if (o.t_ >= kFxOctoShootDownAfter) o.Enter<OctoInkHitState>(m);
    }
    const char *Name() const override { return "OctoShootDownAfter"; }
  };

  /// @brief 次の墨玉はあえて受ける（画面に墨が貼り付く）
  class OctoInkHitState : public State {
  public:
    /// @brief 墨玉を正面に捉えたまま撃たずに待ち、当たったら OctoInkLinger へ
    void OnUpdate(EffectsShowcase &o, Machine &m, float) override {
      if (!o.sub_.Get()) {
        if (o.sub_.has) { // 墨玉が消えた
          // 自機の手前で消えていなければ（寿命切れなど）、命中と同じタグで墨を直接貼る（保険）
          if (o.Distance(o.sub_.last) > kFxBlobHitDistance) {
            Entity &cam = *o.frame_.cam;
            cam.SetTag("pending_ink", cam.GetTagInt("pending_ink", 0) + 100);
          }
          o.sub_.Clear();
          o.Enter<OctoInkLingerState>(m);
          Log::Print("[EffectsShowcase] ink hit");
          return;
        }
        o.sub_.Set(o.FindNearestNamed("InkBlob"));
      }
      if (o.sub_.Get()) {
        o.AimTrack(o.sub_, 0.0f, Fire::Hold, 3.0f);
      } else {
        o.AimTrack(o.target_, 0.0f, Fire::Hold, 3.0f);
      }
      if (o.t_ >= kFxOctoInkTimeout && !o.sub_.Get()) {
        // 保険：墨玉が来なかった。墨を直接貼る（InkBlobScript の命中と同じタグ）
        Entity &cam = *o.frame_.cam;
        cam.SetTag("pending_ink", cam.GetTagInt("pending_ink", 0) + 100);
        o.Enter<OctoInkLingerState>(m);
      }
    }
    const char *Name() const override { return "OctoInkHit"; }
  };

  /// @brief 画面に貼り付いた墨を見せる
  class OctoInkLingerState : public State {
  public:
    /// @brief 少し待ったら OctoKill へ
    void OnUpdate(EffectsShowcase &o, Machine &m, float) override {
      o.RemoveNamed("InkBlob"); // 墨を受けるのは 1 回だけ。続けて吐かれた墨玉は（まだ水中のうちに）消す
      o.AimTrack(o.target_, 0.0f, Fire::Hold, 3.0f);
      if (o.t_ >= kFxOctoInkLinger) o.Enter<OctoKillState>(m);
    }
    const char *Name() const override { return "OctoInkLinger"; }
  };

  /// @brief タコを 1 発で倒す（水中にいて当たりにくいので、届かなければ強制的に倒す）
  class OctoKillState : public State {
  public:
    /// @brief 倒れたら OctoAfter へ
    void OnUpdate(EffectsShowcase &o, Machine &m, float) override {
      o.RemoveNamed("InkBlob");
      auto t = o.target_.Get();
      if (!t || t->HasTag("enemy_defeated")) {
        o.Enter<OctoAfterState>(m);
        Log::Print("[EffectsShowcase] octopus down");
        return;
      }
      o.AimShootOnce(o.target_, 0.0f, 3.0f, true);
      if (o.ShotsUsedUp() || o.t_ >= kFxOctoKillTimeout) t->SetTag("pending_damage", 999); // 保険
    }
    const char *Name() const override { return "OctoKill"; }
  };

  /// @brief しぼみながら沈むタコを見せる
  class OctoAfterState : public State {
  public:
    /// @brief 少し待ったら、無敵を外してサメを出し SharkHit へ
    void OnUpdate(EffectsShowcase &o, Machine &m, float) override {
      o.RemoveNamed("InkBlob");
      o.HoldOnLast(Fire::Hold);
      if (o.t_ >= kFxOctoAfter) {
        o.target_.Set(o.SpawnShark());
        o.hpBefore_ = GameSession::Get().PlayerHp();
        o.vulnerable_ = true;
        o.forcedHit_ = false;
        o.Enter<SharkHitState>(m);
      }
    }
    const char *Name() const override { return "OctoAfter"; }
  };

  /// @brief サメの突進を受ける（撃たずに目で追う）
  class SharkHitState : public State {
  public:
    /// @brief HP が減ったら無敵に戻して SharkAfterHit へ（当たらなければ直接ダメージを入れる）
    void OnUpdate(EffectsShowcase &o, Machine &m, float) override {
      o.AimTrack(o.target_, 0.4f, Fire::Hold, 3.0f);
      const bool hit = GameSession::Get().PlayerHp() < o.hpBefore_;
      if (hit || o.t_ >= kFxSharkHitTimeout + 1.0f) {
        o.vulnerable_ = false;
        o.Enter<SharkAfterHitState>(m);
        Log::Print(hit ? "[EffectsShowcase] shark hit" : "[EffectsShowcase] shark hit skipped");
        return;
      }
      if (o.t_ >= kFxSharkHitTimeout && !o.forcedHit_) {
        o.frame_.cam->SetTag("pending_damage", 1); // 保険：突進が当たらなかった
        o.forcedHit_ = true;
      }
    }
    const char *Name() const override { return "SharkHit"; }
  };

  /// @brief 被弾の揺れを見せる
  class SharkAfterHitState : public State {
  public:
    /// @brief 少し待ったら SharkKill へ
    void OnUpdate(EffectsShowcase &o, Machine &m, float) override {
      o.AimTrack(o.target_, 0.4f, Fire::Hold, 3.0f);
      if (o.t_ >= kFxSharkAfterHit) {
        o.sharkWentFar_ = false; // すれ違った直後はまだ近いので、一度離れるまでは強制撃破の判定をしない
        o.Enter<SharkKillState>(m);
      }
    }
    const char *Name() const override { return "SharkAfterHit"; }
  };

  /// @brief 引き返して浮いてきたサメを撃ち倒す（ClearRun と同じく、水面近くまで浮いたら撃つ）
  class SharkKillState : public State {
  public:
    /// @brief 倒れたら SharkAfter へ（倒せなければ強制的に倒す）
    void OnUpdate(EffectsShowcase &o, Machine &m, float) override {
      auto s = o.target_.Get();
      if (!s || s->HasTag("enemy_defeated")) {
        o.Enter<SharkAfterState>(m);
        Log::Print("[EffectsShowcase] shark down");
        return;
      }
      const auto *str = s->GetComponent<TransformComponent>();
      const bool ready = str && str->position.y >= o.frame_.waterY - kSharkReadyDepth;
      o.AimShootOnce(o.target_, 0.4f, 3.0f, ready);
      // 噛まれるのは 1 回だけ。一度離れてから 2 回目の突進の距離まで迫ったら、撃てていなくても倒す（保険）
      const float d = o.target_.has ? o.Distance(o.target_.last) : 1e9f;
      if (d > kFxSharkBiteGuard + 4.0f) o.sharkWentFar_ = true;
      const bool closing = o.sharkWentFar_ && d <= kFxSharkBiteGuard;
      if (closing || o.ShotsUsedUp() || o.t_ >= kFxSharkKillTimeout) s->SetTag("pending_damage", 999);
    }
    const char *Name() const override { return "SharkKill"; }
  };

  /// @brief 倒れたサメを見せる
  class SharkAfterState : public State {
  public:
    /// @brief 少し待ったら重い弾に持ち替え、船を出して Ship へ
    void OnUpdate(EffectsShowcase &o, Machine &m, float) override {
      o.HoldOnLast(Fire::Hold);
      if (o.t_ >= kFxSharkAfter) {
        o.weapon_ = 3;
        o.target_.Set(o.SpawnShip());
        o.Enter<ShipState>(m);
      }
    }
    const char *Name() const override { return "SharkAfter"; }
  };

  /// @brief 船を重い弾で撃沈する
  class ShipState : public State {
  public:
    /// @brief 姿を見せてから撃ち、撃沈したら ShipSink へ（沈まなければ強制的に撃沈する）
    void OnUpdate(EffectsShowcase &o, Machine &m, float) override {
      auto s = o.target_.Get();
      if (!s || s->HasTag("enemy_defeated")) {
        o.Enter<ShipSinkState>(m);
        Log::Print("[EffectsShowcase] ship sunk");
        return;
      }
      o.AimShootOnce(o.target_, 1.0f, 6.0f, o.t_ >= kFxShipLook);
      if (o.ShotsUsedUp() || o.t_ >= kFxShipLook + kFxShipTimeout) s->SetTag("pending_damage", 999); // 保険
    }
    const char *Name() const override { return "Ship"; }
  };

  /// @brief 浸水して沈んでいく船を見せる
  class ShipSinkState : public State {
  public:
    /// @brief 少し待ったら Outro へ
    void OnUpdate(EffectsShowcase &o, Machine &m, float) override {
      o.weapon_ = 1;
      o.HoldOnLast(Fire::Hold);
      if (o.t_ >= kFxShipSink) o.Enter<OutroState>(m);
    }
    const char *Name() const override { return "ShipSink"; }
  };

  /// @brief Dive 遷移と同じ深海色へ暗転し、暗転しきったらシーンを抜けたいと伝える
  class OutroState : public State {
  public:
    /// @brief 暗転を進め、暗転しきって kFxOutroHold 待ったら WantsExit を立てる
    void OnUpdate(EffectsShowcase &o, Machine &m, float) override {
      o.HoldOnLast(Fire::Hold);
      o.fade_ = Smooth01(o.t_ / (std::max)(kFxOutroFade, 1e-3f));
      if (o.t_ >= kFxOutroFade + kFxOutroHold) {
        o.fade_ = 1.0f;
        o.exit_ = true;
        m.ChangeState<DoneState>();
        Log::Print("[EffectsShowcase] finished, leaving");
      }
    }
    const char *Name() const override { return "Outro"; }
  };

  /// @brief 見せ終わった（または片付けた）。何もしない
  class DoneState : public State {
  public:
    /// @brief 何もしない
    void OnUpdate(EffectsShowcase &, Machine &, float) override {}
    const char *Name() const override { return "Done"; }
  };

  // ---- 定数 ----
  static constexpr const char *kOwnerTag = "result_transition_owner";
  // 見た目と当たり・AI の設定は 1 面のウェーブのスポナー（Stage1.json の WaveSpawner_*）と同じ値
  static constexpr const char *kSharkModel = "Resources/model/shark/shark_game.glb";
  static constexpr const char *kOctopusModel = "Resources/model/octopus/octopus_enemy.glb";
  static constexpr const char *kBirdModel = "Resources/model/bird/bird.glb";
  static constexpr const char *kShipModel = "Resources/model/ship/ship.glb";

  /// @brief 0〜1 をなめらかにする（smoothstep）
  static float Smooth01(float v) {
    v = (std::clamp)(v, 0.0f, 1.0f);
    return v * v * (3.0f - 2.0f * v);
  }
  /// @brief 角度を -π〜π へ丸める
  static float Wrap(float a) {
    while (a > 3.14159265f) a -= 6.28318530718f;
    while (a < -3.14159265f) a += 6.28318530718f;
    return a;
  }

  /// @brief 次の段階を予約し、経過時間を 0 に戻して撃つのをやめる（状態クラスから呼ぶ）
  /// @tparam S 次の状態クラス
  /// @param machine 状態を管理しているステートマシン
  template <class S>
  void Enter(Machine &machine) {
    machine.ChangeState<S>();
    t_ = 0.0f;
    shots_ = 0;
    shotAt_ = 0.0f;
    shotWait_ = 0.0f;
    if (frame_.cam) frame_.cam->SetTag("demo_fire", 0);
  }

  /// @brief 相手を狙い、照準が合ったら 1 発だけ撃つ（連打しない）
  /// @details 撃ったら弾が届く見込み時間＋kFxShotMargin 待ち、相手がまだ生きていれば（外れた）撃ち直す。
  ///          demo_fire は撃つフレームだけ 1 にする（次のフレームで RailShooterController が 1 発撃ち、
  ///          その後の AimTrack で 0 に戻る）。
  /// @param tk 相手
  /// @param yOffset 狙う高さのずれ（m）
  /// @param gravity 弾の重力の大きさ（通常 3 / 重い弾 6）
  /// @param allowed 撃ってよいか（false なら目で追うだけ）
  void AimShootOnce(Tracker &tk, float yOffset, float gravity, bool allowed) {
    shotWait_ -= frame_.dt;
    const bool aligned = AimTrack(tk, yOffset, Fire::Hold, gravity);
    if (!allowed || !aligned || shotWait_ > 0.0f || ShotsUsedUp() || !tk.has) return;
    frame_.cam->SetTag("demo_fire", 1);
    ++shots_;
    shotAt_ = t_;
    shotWait_ = Distance(tk.last) / kFxBulletSpeed + kFxShotMargin;
  }

  /// @brief 撃てる発数を撃ちきって、最後の弾が届く時間も過ぎたか（呼び出し側が強制的に倒す合図）
  /// @return 撃ちきったなら true
  bool ShotsUsedUp() const { return shots_ >= kFxMaxShots && shotWait_ <= 0.0f; }

  /// @brief 名前が name のエンティティをすべて消す（続けて吐かれた墨玉を片付ける）
  void RemoveNamed(const char *name) {
    for (const auto &e : frame_.scene->GetEntities()) {
      if (e && !e->IsPendingDestroy() && e->GetName() == name) e->Destroy();
    }
  }

  /// @brief 自機の正面（レールの基準の向き）のヨー角
  float BaseYaw() const {
    const Entity &cam = *frame_.cam;
    return cam.HasTag("base_yaw_mrad") ? static_cast<float>(cam.GetTagInt("base_yaw_mrad", 0)) / 1000.0f
                                       : frame_.tr->rotation.y;
  }

  /// @brief 出発点のカメラから見た前方 ahead・右 side の位置（高さは y）
  RC::Vector3 Ahead(float ahead, float side, float y) const {
    const float yaw = BaseYaw();
    const RC::Vector3 c = frame_.tr->position;
    return {c.x + std::sin(yaw) * ahead + std::cos(yaw) * side, y, c.z + std::cos(yaw) * ahead - std::sin(yaw) * side};
  }
  /// @brief 前方 ahead・右 side の水面上の位置
  RC::Vector3 OnWater(float ahead, float side) const { return Ahead(ahead, side, frame_.waterY); }

  /// @brief カメラからの距離
  float Distance(const RC::Vector3 &p) const {
    const RC::Vector3 c = frame_.tr->position;
    const float dx = p.x - c.x, dy = p.y - c.y, dz = p.z - c.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
  }

  /// @brief 照準を target へ寄せる（撃つかどうかは mode で決める）
  /// @return 照準のずれが kFxAimTolerance 以内なら true
  static bool AimAt(Entity &cam, const TransformComponent &tr, const RC::Vector3 &target, Fire mode) {
    const float dx = target.x - tr.position.x, dy = target.y - tr.position.y, dz = target.z - tr.position.z;
    const float hl = std::sqrt(dx * dx + dz * dz);
    if (hl < 1e-3f) return false;
    const float yaw = std::atan2(dx, dz);
    const float pitch = -std::atan2(dy, hl); // 正で下向き
    cam.SetTag("demo_look", 1);
    cam.SetTag("demo_look_yaw_mrad", static_cast<int>(std::lround(yaw * 1000.0f)));
    cam.SetTag("demo_look_pitch_mrad", static_cast<int>(std::lround(pitch * 1000.0f)));
    const float err = std::fabs(Wrap(yaw - tr.rotation.y)) + std::fabs(pitch - tr.rotation.x);
    const bool aligned = (err <= kFxAimTolerance);
    const bool fire = (mode == Fire::Aligned) && aligned;
    cam.SetTag("demo_fire", fire ? 1 : 0);
    return aligned;
  }

  /// @brief 照準を正面へ戻し、撃つのをやめる
  static void AimCenter(Entity &cam) {
    cam.SetTag("demo_look", 2);
    cam.SetTag("demo_fire", 0);
  }

  /// @brief 相手の狙う位置（コライダーの中心）
  static bool TargetPoint(const Entity &e, RC::Vector3 &out) {
    const auto *etr = e.GetComponent<TransformComponent>();
    if (!etr) return false;
    out = etr->position;
    if (const auto *col = e.GetComponent<ColliderComponent>()) {
      out = {out.x + col->center.x * etr->scale.x, out.y + col->center.y * etr->scale.y,
             out.z + col->center.z * etr->scale.z};
    }
    return true;
  }

  /// @brief 相手を偏差撃ちで狙う（弾の飛ぶ時間ぶん先を読み、重力で落ちるぶん上を狙う）
  /// @param tk 相手
  /// @param yOffset 狙う高さのずれ（m）
  /// @param mode 撃つかどうか
  /// @param gravity 弾の重力の大きさ（通常 3 / 重い弾 6。WaterBullet の弾種と同じ）
  /// @return 照準が合っているなら true
  bool AimTrack(Tracker &tk, float yOffset, Fire mode, float gravity) {
    auto s = tk.Get();
    if (!s) {
      HoldOnLast(Fire::Hold);
      return false;
    }
    RC::Vector3 p{};
    if (!TargetPoint(*s, p)) return false;
    p.y += yOffset;
    tk.Sample(p, frame_.dt);
    const float tt = Distance(p) / kFxBulletSpeed;
    const RC::Vector3 aim = {p.x + tk.vel.x * tt, p.y + tk.vel.y * tt + 0.5f * gravity * tt * tt,
                             p.z + tk.vel.z * tt};
    return AimAt(*frame_.cam, *frame_.tr, aim, mode);
  }

  /// @brief 最後に追っていた相手の位置を見続ける（相手がいなければ正面）
  void HoldOnLast(Fire mode) {
    if (target_.has) {
      AimAt(*frame_.cam, *frame_.tr, target_.last, mode);
    } else {
      AimCenter(*frame_.cam);
    }
  }

  /// @brief 名前が name の生きているエンティティのうち、カメラに最も近いもの
  std::shared_ptr<Entity> FindNearestNamed(const char *name) const {
    std::shared_ptr<Entity> best;
    float bestD = 1e9f;
    for (const auto &e : frame_.scene->GetEntities()) {
      if (!e || !e->IsActive() || e->IsPendingDestroy() || e->GetName() != name) continue;
      RC::Vector3 p{};
      if (!TargetPoint(*e, p)) continue;
      const float d = Distance(p);
      if (d < bestD) {
        bestD = d;
        best = e;
      }
    }
    return best;
  }

  /// @brief 飛んでいる自機の弾を 1 発、p へ寄せる（墨玉の撃ち落としの保険。判定は墨玉側がそのまま行う）
  /// @details 飛んでいる弾が無ければ 1 発撃たせ、次のフレームで寄せる。
  void PullBulletTo(const RC::Vector3 &p) {
    for (const auto &e : frame_.scene->GetEntities()) {
      if (!e || !e->IsActive() || e->IsPendingDestroy() || e->GetName() != "PlayerBullet") continue;
      if (auto *btr = e->GetComponent<TransformComponent>()) {
        btr->position = p;
        return;
      }
    }
    frame_.cam->SetTag("demo_fire", 1);
  }

  /// @brief 前方のまだ開いていない宝箱を探し、無ければ前方に出す
  std::shared_ptr<Entity> FindOrSpawnChest() {
    const float baseYaw = BaseYaw();
    std::shared_ptr<Entity> best;
    float bestD = kFxChestSearch;
    for (const auto &e : frame_.scene->GetEntities()) {
      if (!e || !e->IsActive() || e->IsPendingDestroy() || e->GetTagInt("is_item", 0) != 1) continue;
      RC::Vector3 p{};
      if (!TargetPoint(*e, p)) continue;
      const float dx = p.x - frame_.tr->position.x, dz = p.z - frame_.tr->position.z;
      const float d = std::sqrt(dx * dx + dz * dz);
      if (d >= bestD || d < 4.0f) continue;
      if (std::fabs(Wrap(std::atan2(dx, dz) - baseYaw)) > 1.0f) continue; // 正面からおよそ 57° 以内
      bestD = d;
      best = e;
    }
    if (best) {
      Log::Print("[EffectsShowcase] chest: " + best->GetName());
      return best;
    }
    // 見つからなければ 1 面の宝箱と同じ組み立てで出す（見た目の部位は TreasureChestScript が作る）
    auto e = frame_.scene->CreateEntity("TreasureChest_demo");
    if (!e) return nullptr;
    e->SetTag("transient", 1);
    auto &tr = e->AddComponent<TransformComponent>();
    tr.position = OnWater(kFxChestAhead, kFxChestSide);
    tr.position.y += 1.2f;
    tr.rotation = {0.0f, BaseYaw(), 0.0f};
    tr.scale = {1.4f, 1.0f, 1.0f};
    auto &col = e->AddComponent<ColliderComponent>();
    col.shape = ColliderComponent::Shape::Sphere;
    col.radius = 1.0f;
    col.isTrigger = false;
    auto &nsc = e->AddComponent<NativeScriptComponent>();
    nsc.AddScript("TreasureChestScript");
    if (!nsc.scripts.empty()) nsc.scripts.back().pendingData = {{"maxHp", 3}, {"points", 300}};
    nsc.SetScene(frame_.scene);
    if (frame_.ctx) nsc.SetSceneContext(frame_.ctx);
    frame_.scene->InitDynamicEntityRuntime(*e);
    spawned_.push_back(e);
    Log::Print("[EffectsShowcase] chest spawned");
    return e;
  }

  /// @brief 敵を 1 体出す（WaveSpawnerScript::SpawnOne と同じ組み立て）
  /// @param name エンティティ名
  /// @param model モデルのパス
  /// @param scale 大きさ
  /// @param colliderRadius 当たり半径（ローカル単位。× scale の最大成分が実際の半径）
  /// @param pos 位置
  /// @param yaw 向き（ヨー角）
  /// @param scripts 載せるスクリプトと pendingData
  /// @return 出した敵（失敗時は nullptr）
  std::shared_ptr<Entity> SpawnActor(const char *name, const char *model, const RC::Vector3 &scale,
                                     float colliderRadius, const RC::Vector3 &pos, float yaw,
                                     std::initializer_list<ScriptSpec> scripts) {
    Scene &scene = *frame_.scene;
    auto e = scene.CreateEntity(name);
    if (!e) return nullptr;
    e->SetTag("transient", 1); // エディタで保存しても JSON に残さない
    auto &tr = e->AddComponent<TransformComponent>();
    tr.position = pos;
    tr.scale = scale;
    tr.rotation = {0.0f, yaw, 0.0f};
    auto &mr = e->AddComponent<ModelRendererComponent>();
    mr.modelPath = model;
    auto &col = e->AddComponent<ColliderComponent>();
    col.shape = ColliderComponent::Shape::Sphere;
    col.radius = colliderRadius;
    col.isTrigger = false;
    auto &nsc = e->AddComponent<NativeScriptComponent>();
    for (const ScriptSpec &s : scripts) {
      nsc.AddScript(s.name);
      // スクリプト実体は次の Update で作られるので、pendingData で渡す（WaveSpawner と同じ）
      if (!nsc.scripts.empty()) nsc.scripts.back().pendingData = s.params;
    }
    nsc.SetScene(&scene);
    if (frame_.ctx) nsc.SetSceneContext(frame_.ctx);
    scene.InitDynamicEntityRuntime(*e);
    spawned_.push_back(e);
    Log::Print(std::string("[EffectsShowcase] spawned ") + name);
    return e;
  }

  /// @brief 自機の方を向く向き（出現位置 p から見て）
  float FacingCamera(const RC::Vector3 &p) const {
    return std::atan2(frame_.tr->position.x - p.x, frame_.tr->position.z - p.z);
  }

  /// @brief 鳥を出す（1 面の WaveSpawner_2_bird_7 と同じ設定。近くで見せるため距離と横のずれを縮め、1 発で落ちるよう体力を 1 にする）
  std::shared_ptr<Entity> SpawnBird() {
    const RC::Vector3 p = Ahead(kFxBirdAhead, kFxBirdSide, frame_.waterY + kFxBirdHeight);
    return SpawnActor("WaveBird_demo", kBirdModel, {0.95f, 0.95f, 0.95f}, 1.4f, p, FacingCamera(p),
                      {{"BirdEnemyScript",
                        {{"maxHp", 1}, {"detectDistance", 90.0f}, {"preferredDistance", 20.0f}, {"sideOffset", 4.0f},
                         {"flyHeight", 7.0f}, {"fireInterval", 3.2f}, {"firstFireDelay", 1.2f},
                         {"windupDuration", 0.75f}, {"featherCount", 5}, {"featherSpreadDeg", 9.0f},
                         {"featherSpeed", 17.0f}, {"featherDamage", 1}}}});
  }

  /// @brief タコを出す（1 面の WaveSpawner_1_octopus_2 と同じ設定。すぐ吐くよう間隔を縮め、1 発で倒れるよう体力を 1 にする）
  std::shared_ptr<Entity> SpawnOctopus() {
    const RC::Vector3 p = Ahead(kFxOctoAhead, kFxOctoSide, frame_.waterY - kFxOctoDepth);
    return SpawnActor("WaveOctopus_demo", kOctopusModel, {2.4f, 2.4f, 2.4f}, 0.5f, p, FacingCamera(p),
                      {{"OctopusEnemyScript",
                        {{"maxHp", 1}, {"preferredDistance", 16.0f}, {"sideOffset", 2.5f}, {"fireInterval", 3.0f},
                         {"firstFireDelay", 1.0f}, {"windupDuration", 0.9f}, {"inkAmount", 1.0f},
                         {"inkDamage", 0}}}});
  }

  /// @brief サメを出す（1 面の WaveSpawner_1_shark_1 と同じ設定。突進のあと早めに戻ってくるよう引き返しを縮め、1 発で倒れるよう体力を 1 にする）
  std::shared_ptr<Entity> SpawnShark() {
    const RC::Vector3 p = OnWater(kFxSharkAhead, kFxSharkSide);
    return SpawnActor("WaveShark_demo", kSharkModel, {kSharkScale, kSharkScale, kSharkScale}, 0.3333333f, p,
                      FacingCamera(p),
                      {{"SharkEnemyScript",
                        {{"detectDistance", 80.0f}, {"maxHp", 1}, {"modelRotOffset", {0.0f, -90.0f, 0.0f}},
                         {"retreatDistance", 26.0f}, {"retreatSide", 6.0f}, {"retreatMaxDuration", 2.5f},
                         {"cooldownDuration", 1.2f}}}});
  }

  /// @brief 船を出す（1 面の WaveSpawner_3_ship_5 と同じ 3 本のスクリプト。体力だけ kFxShipHp（重い弾 1 発）に下げる）
  std::shared_ptr<Entity> SpawnShip() {
    RC::Vector3 p = OnWater(kFxShipAhead, kFxShipSide);
    p.y += 0.5f;
    return SpawnActor(
        "WaveShip_demo", kShipModel, {1.0f, 1.0f, 1.0f}, 3.6f, p, FacingCamera(p),
        {{"ShipEnemyScript",
          {{"broadsideToleranceDeg", 35.0f}, {"bulletType", 0}, {"cruiseSpeed", 6.0f}, {"detectDistance", 60.0f},
           {"engageSpeed", 4.5f}, {"fireMaxDistance", 45.0f}, {"fireMinDistance", 12.0f}, {"gunHeight", 1.4f},
           {"gunOffset", 2.2f}, {"leadFactor", 0.6f}, {"maxHp", kFxShipHp}, {"patrolRadius", 18.0f},
           {"preferredDistance", 28.0f}, {"reloadDuration", 3.0f}, {"shellSpeed", 22.0f}, {"spreadDeg", 4.0f},
           {"turnRateDeg", 35.0f}, {"volleyCount", 3}, {"volleyInterval", 0.22f}}},
         {"BuoyancyScript",
          {{"alignToSurface", true}, {"debugDraw", false}, {"densityRatio", 0.5f}, {"floodDuration", 3.0f},
           {"floodedDensityRatio", 1.4f}, {"halfHeight", 0.8f}, {"linearDrag", 5.5f}, {"maxTiltDeg", 12.0f},
           {"quadraticDrag", 1.0f}, {"sampleRadius", 4.0f}, {"sampleRadiusSide", 1.5f}, {"tiltSharpness", 4.0f},
           {"useFourSamples", true}}},
         {"ShipVisualScript", nlohmann::json::object()}});
  }

  /// @brief 出す敵のモデルを、見えない所に先に読み込んでおく（出現の瞬間に止まったり、見た目が遅れて出ないように）
  /// @details SharkFrenzy::Prepare と同じやり方。1 回だけ作り、シーンを抜けると一緒に消える。
  void Preload(Scene &scene) {
    if (preloaded_) return;
    preloaded_ = true;
    for (const char *model : {kBirdModel, kOctopusModel, kSharkModel, kShipModel}) {
      auto e = scene.CreateEntity("DemoPreload");
      if (!e) continue;
      e->SetTag("transient", 1);
      auto &tr = e->AddComponent<TransformComponent>();
      tr.position = {0.0f, -500.0f, 0.0f};
      auto &mr = e->AddComponent<ModelRendererComponent>();
      mr.modelPath = model;
      scene.InitDynamicEntityRuntime(*e);
      e->SetActive(false);
      spawned_.push_back(e);
    }
  }

  StateMachine<EffectsShowcase> fsm_; ///< 段階（WaitStartState 〜 DoneState）
  Frame frame_;                       ///< このフレームの入力（Update の間だけ有効）
  Tracker target_;                    ///< いま見せている相手（宝箱・鳥・タコ・サメ・船）
  Tracker sub_;                       ///< タコの墨玉
  std::vector<std::weak_ptr<Entity>> spawned_; ///< デモが出したエンティティ（End で片付ける）
  float t_ = 0.0f;          ///< 現在の段階に入ってからの経過（秒）
  float fade_ = 0.0f;       ///< 暗転の濃さ（0〜1）
  int weapon_ = 1;          ///< 持たせる武器（1:通常 / 2:拡散 / 3:重い弾）
  int hpBefore_ = 0;        ///< サメの突進を受ける前の HP
  int inkIdx_ = 0;          ///< 水面の墨：何発目か（0:通常で左 / 1:拡散で中央 / 2:重い弾で右）
  int shots_ = 0;           ///< 今の段階で撃った発数
  float shotAt_ = 0.0f;     ///< 最後に撃ったときの t_
  float shotWait_ = 0.0f;   ///< 撃ち直せるまでの残り（秒）
  bool sharkWentFar_ = false; ///< 噛みついたサメが一度離れたか
  bool vulnerable_ = false; ///< 被弾してよいか（サメの突進を受ける間だけ true）
  bool forcedHit_ = false;  ///< 突進が当たらず、直接ダメージを入れたか
  bool exit_ = false;       ///< 見せ終わってシーンを抜けたいか
  bool preloaded_ = false;  ///< モデルの先読みを済ませたか
};

} // namespace TransitionDemo
