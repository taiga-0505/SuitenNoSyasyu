#pragma once
#include "ECS/TransformComponent.h"
#include "ECS/ModelRendererComponent.h"
#include "ECS/ColliderComponent.h"
#include "ECS/NativeScriptComponent.h"
#include "Common/Log/Log.h"
#include "Scene.h"
#include "Game/Framework/TransitionDemo.h"
#include "Game/Framework/StateMachine.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <memory>
#include <string>

// ============================================================================
// 見せ用デモ（Mode::Clear）：序盤を少し遊んで見せ、暗転で終盤へ飛び、サメを撃ち倒してクリアする
//
//   DeathSinkScript（ステージの Camera エンティティ）が TransitionDemo::IsClearDemo() のときだけ使う。
//   ほかのスクリプトとは、Camera / WaveManager のタグだけでやり取りする（型は知らない）:
//
//     Camera に立てるタグ（読むのは括弧内）
//       demo_invincible     : 1 のあいだ被弾しない（RailShooterController::TakeDamage）
//       demo_look           : 1 = demo_look_yaw/pitch_mrad の向きへ照準を寄せる / 2 = 正面へ戻す
//                             （RailShooterController がマウスの代わりに使う）
//       demo_look_yaw_mrad  : 照準を向けたいヨー角（ワールド。ミリラジアン）
//       demo_look_pitch_mrad: 照準を向けたいピッチ角（ワールド。正で下向き。ミリラジアン）
//       demo_fire           : 1 のあいだ撃ち続ける（RailShooterController）
//       demo_rail_jump      : 値（cm）だけ終点の手前へレールを移す。読んだら消える（RailMovementScript）
//       demo_rail_hold      : 1 のあいだレールが進まない（RailMovementScript）
//       demo_skip_waves     : 1 ならウェーブ地点で止まらない（RailMovementScript）
//     WaveManager に立てるタグ
//       wave_demo_abort     : 1 で進行中のウェーブを黙って打ち切り、Idle へ戻す（WaveManagerScript）
//
//   流れ:
//     WaitIntro : 開始演出（intro_playing）が終わるのを待つ
//     Play      : kClearPlayTime 秒ふつうに進む。近くの敵へ照準を寄せ、合ったら撃つ
//     FadeOut   : Dive 遷移と同じ深海色へ暗転（描画は呼び出し側が FadeAlpha() で行う）
//     Black     : 暗転しきったら、敵とウェーブを片付けてレールを終点の手前へ移し、レールを止める
//     FadeIn    : 前方にサメを出して明ける。サメはゲームのウェーブと同じもの（SharkEnemyScript・同じ大きさ・
//                 当たり半径・体力）なので、潜ったまま蛇行して近づき、浮上しながら詰めて突進してくる
//     Fight     : サメが水面近くまで浮いてきたら（kSharkReadyDepth）照準を合わせて撃つ。
//                 倒せなければ kFightTimeout で強制撃破（突進が当たっても demo_invincible で被弾しない）
//     AfterKill : 少し見せてからレールを再開し、照準を正面へ戻す → 終点でクリア（ClearAscendScript）
//     Done      : 決着後の演出が始まった（result_transition_owner）。タグを片付けて何もしない
//
//   実装（State パターン）:
//     以前は enum Phase + switch で上の 9 段階を切り替えていた。段階ごとの遷移条件
//     （暗転しきったら / サメを倒したら …）が 1 つの関数に混ざっていたので、
//     「段階 1 つ = 状態クラス 1 つ」（WaitIntroState 〜 DoneState）に分け、
//     次の段階は各状態クラスが StateMachine::ChangeState で決めるようにした。
//     「決着後の演出が始まったら Done へ」はどの段階からでも起きる割り込みなので、Update の入口で扱う。
// ============================================================================

namespace TransitionDemo {

/// @brief クリアを見せるデモのステージ側の進行
class ClearRun {
public:
  /// @brief 最初の段階（開始演出の終わりを待つ）で作る
  ClearRun() {
    fsm_.ChangeState<WaitIntroState>();
    fsm_.ApplyPending(*this);
  }

  /// @brief 状態を初期化し、前回のタグを消す（OnCreate で呼ぶ）
  /// @param cam Camera エンティティ（nullptr なら状態だけ初期化）
  void Reset(Entity *cam) {
    fsm_.ChangeState<WaitIntroState>();
    fsm_.ApplyPending(*this);
    t_ = 0.0f;
    fightT_ = 0.0f;
    fade_ = 0.0f;
    shark_.reset();
    if (cam) ClearTags(*cam);
  }

  /// @brief デモを進める（再生中の毎フレーム、カメラが動いたあとに呼ぶ）
  /// @param scene  シーン
  /// @param cam    Camera エンティティ
  /// @param tr     カメラの Transform
  /// @param waterY 水面の高さ
  /// @param dt     経過時間（秒）
  void Update(Scene *scene, Entity &cam, const TransformComponent &tr, float waterY, float dt) {
    if (!scene || fsm_.IsIn<DoneState>()) return;

    // 決着後の演出（クリアの海図 / 力尽きた演出）が始まったら手を引く（どの段階からでも起きる割り込み）
    if (cam.GetTagInt(kOwnerTag, 0) == 1 || cam.GetTagInt("game_over", 0) == 1) {
      ClearTags(cam);
      fade_ = 0.0f;
      fsm_.ChangeState<DoneState>();
      fsm_.ApplyPending(*this);
      Log::Print("[ClearRunDemo] outro started, done");
      return;
    }
    cam.SetTag("demo_invincible", 1);
    t_ += dt;

    // 段階ごとの処理と次の段階の決定は、現在の状態クラスに任せる
    frame_ = Frame{scene, &cam, &tr, waterY, dt};
    fsm_.Update(*this, dt);
    frame_ = Frame{};

    // 明けきる前に次の段階へ進んだ場合（サメが途中で消えたなど）も、暗転は残さず明けていく
    const bool fightOrLater = fsm_.IsIn<FightState>() || fsm_.IsIn<AfterKillState>() || fsm_.IsIn<FinishState>();
    if (fightOrLater && fade_ > 0.0f) {
      fade_ = (std::max)(0.0f, fade_ - dt / (std::max)(kSkipFadeIn, 1e-3f));
    }
  }

  /// @brief 暗転の濃さ（呼び出し側が OnOverlayRender で Dive 遷移の色を画面全体に敷く）
  /// @return 0〜1
  float FadeAlpha() const { return fade_; }

  /// @brief 現在の段階名（インスペクタ・ログ表示用）
  /// @return 段階名
  const char *PhaseName() const { return fsm_.CurrentName(); }

  /// @brief 片付け（OnDestroy で呼ぶ）
  /// @param cam Camera エンティティ（nullptr ならタグは触らない）
  void End(Entity *cam) {
    if (cam) ClearTags(*cam);
    if (auto s = shark_.lock()) {
      if (!s->IsPendingDestroy()) s->Destroy();
    }
    shark_.reset();
    fsm_.ChangeState<DoneState>();
    fsm_.ApplyPending(*this);
    fade_ = 0.0f;
  }

  /// @brief デモが立てるタグをすべて消す
  /// @details エディタの保存で Camera にデモのタグが焼き付いていても、デモを流さないときに
  ///          無敵などが残らないよう、デモの種類に関係なく呼び出し側からも使えるようにしている。
  /// @param cam Camera エンティティ
  static void ClearTags(Entity &cam) {
    for (const char *k : {"demo_invincible", "demo_look", "demo_look_yaw_mrad", "demo_look_pitch_mrad", "demo_fire",
                          "demo_rail_jump", "demo_rail_hold", "demo_skip_waves", "demo_weapon"}) {
      cam.ClearTag(k);
    }
  }

private:
  /// @brief 状態クラスが使う、このフレームの入力（Update の間だけ有効）
  struct Frame {
    Scene *scene = nullptr;                ///< シーン
    Entity *cam = nullptr;                 ///< Camera エンティティ
    const TransformComponent *tr = nullptr; ///< カメラの Transform
    float waterY = 0.0f;                   ///< 水面の高さ
    float dt = 0.0f;                       ///< 経過時間（秒）
  };

  // ---- 状態クラス（private な入れ子クラスなので、ClearRun の private メンバをそのまま使える）----

  /// @brief 開始演出（intro_playing）が終わるのを待つ
  class WaitIntroState : public IState<ClearRun> {
  public:
    /// @brief 開始演出が終わったら Play へ
    void OnUpdate(ClearRun &o, StateMachine<ClearRun> &m, float) override {
      o.t_ = 0.0f;
      if (o.frame_.cam->GetTagInt("intro_playing", 0) == 1) return;
      m.ChangeState<PlayState>();
      Log::Print("[ClearRunDemo] play");
    }
    /// @brief 状態名
    const char *Name() const override { return "WaitIntro"; }
  };

  /// @brief kClearPlayTime 秒ふつうに進み、近くの敵へ照準を寄せて撃つ
  class PlayState : public IState<ClearRun> {
  public:
    /// @brief 近くの敵を狙い、時間が来たら FadeOut へ
    void OnUpdate(ClearRun &o, StateMachine<ClearRun> &m, float) override {
      Entity &cam = *o.frame_.cam;
      const TransformComponent &tr = *o.frame_.tr;
      // 近くの敵へ照準を寄せて撃つ（いなければ正面）
      RC::Vector3 target{};
      if (o.FindNearestEnemy(*o.frame_.scene, cam, tr, target)) {
        AimAt(cam, tr, target, true);
      } else {
        AimCenter(cam);
      }
      if (o.t_ >= kClearPlayTime) o.Enter<FadeOutState>(m, cam);
    }
    /// @brief 状態名
    const char *Name() const override { return "Play"; }
  };

  /// @brief Dive 遷移と同じ深海色へ暗転する
  class FadeOutState : public IState<ClearRun> {
  public:
    /// @brief 暗転を進め、暗転しきったら終点の手前へ移して Black へ
    void OnUpdate(ClearRun &o, StateMachine<ClearRun> &m, float) override {
      o.fade_ = Smooth01(o.t_ / (std::max)(kSkipFadeOut, 1e-3f));
      if (o.t_ >= kSkipFadeOut) {
        o.fade_ = 1.0f;
        o.JumpToFinal(*o.frame_.scene, *o.frame_.cam);
        o.Enter<BlackState>(m, *o.frame_.cam);
      }
    }
    /// @brief 状態名
    const char *Name() const override { return "FadeOut"; }
  };

  /// @brief 暗転しきったまま待つ（この間にレールが移る）
  class BlackState : public IState<ClearRun> {
  public:
    /// @brief 照準を正面へ戻し、待ち終わったらサメを出して FadeIn へ
    void OnUpdate(ClearRun &o, StateMachine<ClearRun> &m, float) override {
      Entity &cam = *o.frame_.cam;
      o.fade_ = 1.0f;
      AimCenter(cam); // 暗いうちに正面へ戻しておく
      if (o.t_ >= kSkipBlackHold) {
        o.SpawnShark(*o.frame_.scene, cam, *o.frame_.tr, o.frame_.waterY);
        o.Enter<FadeInState>(m, cam);
      }
    }
    /// @brief 状態名
    const char *Name() const override { return "Black"; }
  };

  /// @brief 前方にサメを出したまま明けていく（明けている間もサメを追う）
  class FadeInState : public IState<ClearRun> {
  public:
    /// @brief 明けを進め、サメが倒れたら AfterKill、明けきったら Fight へ
    void OnUpdate(ClearRun &o, StateMachine<ClearRun> &m, float dt) override {
      Entity &cam = *o.frame_.cam;
      o.fade_ = 1.0f - Smooth01(o.t_ / (std::max)(kSkipFadeIn, 1e-3f));
      if (o.UpdateFight(cam, *o.frame_.tr, dt)) {
        o.Enter<AfterKillState>(m, cam);
        return;
      }
      if (o.t_ >= kSkipFadeIn) {
        o.fade_ = 0.0f;
        o.Enter<FightState>(m, cam);
      }
    }
    /// @brief 状態名
    const char *Name() const override { return "FadeIn"; }
  };

  /// @brief サメが水面近くまで浮いてきたら照準を合わせて撃つ
  class FightState : public IState<ClearRun> {
  public:
    /// @brief サメを追い、倒れたら AfterKill へ
    void OnUpdate(ClearRun &o, StateMachine<ClearRun> &m, float dt) override {
      if (o.UpdateFight(*o.frame_.cam, *o.frame_.tr, dt)) o.Enter<AfterKillState>(m, *o.frame_.cam);
    }
    /// @brief 状態名
    const char *Name() const override { return "Fight"; }
  };

  /// @brief 倒したところを少し見せてからレールを再開する
  class AfterKillState : public IState<ClearRun> {
  public:
    /// @brief 照準を正面へ戻し、待ち終わったらレールを再開して Finish へ
    void OnUpdate(ClearRun &o, StateMachine<ClearRun> &m, float) override {
      Entity &cam = *o.frame_.cam;
      AimCenter(cam);
      if (o.t_ >= kAfterKillDelay) {
        cam.ClearTag("demo_rail_hold"); // 終点へ向けてレールを再開
        o.Enter<FinishState>(m, cam);
        Log::Print("[ClearRunDemo] shark down, rail resumes");
      }
    }
    /// @brief 状態名
    const char *Name() const override { return "AfterKill"; }
  };

  /// @brief 終点まで正面を向いて進む（クリアの演出が始まると Update の入口で Done になる）
  class FinishState : public IState<ClearRun> {
  public:
    /// @brief 照準を正面に保つ
    void OnUpdate(ClearRun &o, StateMachine<ClearRun> &, float) override { AimCenter(*o.frame_.cam); }
    /// @brief 状態名
    const char *Name() const override { return "Finish"; }
  };

  /// @brief 決着後の演出が始まった（または片付けた）。何もしない
  class DoneState : public IState<ClearRun> {
  public:
    /// @brief 何もしない
    void OnUpdate(ClearRun &, StateMachine<ClearRun> &, float) override {}
    /// @brief 状態名
    const char *Name() const override { return "Done"; }
  };

  static constexpr const char *kOwnerTag = "result_transition_owner";
  // サメの見た目と当たり・AI の設定は 1 面のウェーブのスポナー（Stage1.json の WaveShark）と同じ値
  static constexpr const char *kSharkModel = "Resources/model/shark/shark_game.glb";
  static constexpr const char *kSharkScript = "SharkEnemyScript";
  static constexpr float kSharkColliderRadius = 0.3333333f; ///< ローカル単位（× kSharkScale で 2m）
  static constexpr float kSharkDetectDistance = 80.0f;
  static constexpr float kMaxAimYaw = 1.2f; ///< 序盤に狙う敵の、正面からの最大角（rad。およそ 70°）

  static float Smooth01(float v) {
    v = (std::clamp)(v, 0.0f, 1.0f);
    return v * v * (3.0f - 2.0f * v);
  }
  static float Wrap(float a) {
    while (a > 3.14159265f) a -= 6.28318530718f;
    while (a < -3.14159265f) a += 6.28318530718f;
    return a;
  }

  /// @brief 次の段階を予約し、経過時間を 0 に戻して撃つのをやめる（状態クラスから呼ぶ）
  /// @tparam S 次の状態クラス
  /// @param machine 状態を管理しているステートマシン
  /// @param cam Camera エンティティ
  template <class S>
  void Enter(StateMachine<ClearRun> &machine, Entity &cam) {
    machine.ChangeState<S>();
    t_ = 0.0f;
    cam.SetTag("demo_fire", 0);
  }

  /// @brief 自機の正面（レールの基準の向き）のヨー角
  static float BaseYaw(const Entity &cam, const TransformComponent &tr) {
    return cam.HasTag("base_yaw_mrad") ? static_cast<float>(cam.GetTagInt("base_yaw_mrad", 0)) / 1000.0f
                                       : tr.rotation.y;
  }

  /// @brief 照準を target へ寄せ、ずれが小さければ撃つ
  /// @param allowFire 撃ってよいか（false なら寄せるだけ）
  static void AimAt(Entity &cam, const TransformComponent &tr, const RC::Vector3 &target, bool allowFire) {
    const float dx = target.x - tr.position.x, dy = target.y - tr.position.y, dz = target.z - tr.position.z;
    const float hl = std::sqrt(dx * dx + dz * dz);
    if (hl < 1e-3f) return;
    const float yaw = std::atan2(dx, dz);
    const float pitch = -std::atan2(dy, hl); // 正で下向き
    cam.SetTag("demo_look", 1);
    cam.SetTag("demo_look_yaw_mrad", static_cast<int>(std::lround(yaw * 1000.0f)));
    cam.SetTag("demo_look_pitch_mrad", static_cast<int>(std::lround(pitch * 1000.0f)));
    const float err = std::fabs(Wrap(yaw - tr.rotation.y)) + std::fabs(pitch - tr.rotation.x);
    cam.SetTag("demo_fire", (allowFire && err <= kAimTolerance) ? 1 : 0);
  }

  /// @brief 照準を正面へ戻し、撃つのをやめる
  static void AimCenter(Entity &cam) {
    cam.SetTag("demo_look", 2);
    cam.SetTag("demo_fire", 0);
  }

  /// @brief 序盤に狙う敵（生きていて近く、正面から大きく外れていない敵のうち最も近いもの）を探す
  /// @param out 狙う位置（コライダーの中心）
  /// @return 見つかったら true
  bool FindNearestEnemy(Scene &scene, const Entity &cam, const TransformComponent &tr, RC::Vector3 &out) const {
    const float baseYaw = BaseYaw(cam, tr);
    float best = kAimRange;
    bool found = false;
    for (const auto &e : scene.GetEntities()) {
      if (!e || !e->IsActive() || e->IsPendingDestroy()) continue;
      if (e->GetTagInt("is_enemy", 0) != 1 || e->HasTag("enemy_defeated")) continue;
      const auto *etr = e->GetComponent<TransformComponent>();
      if (!etr) continue;
      RC::Vector3 p = etr->position;
      if (const auto *col = e->GetComponent<ColliderComponent>()) {
        p = {p.x + col->center.x * etr->scale.x, p.y + col->center.y * etr->scale.y, p.z + col->center.z * etr->scale.z};
      }
      const float dx = p.x - tr.position.x, dz = p.z - tr.position.z;
      const float d = std::sqrt(dx * dx + dz * dz);
      if (d >= best || d < 2.0f) continue;
      if (std::fabs(Wrap(std::atan2(dx, dz) - baseYaw)) > kMaxAimYaw) continue;
      best = d;
      out = p;
      found = true;
    }
    return found;
  }

  /// @brief 暗転中の差し替え：敵とウェーブを片付け、レールを終点の手前へ移して止める
  void JumpToFinal(Scene &scene, Entity &cam) {
    // 生きている敵を消す（序盤の敵が終盤まで残ったり、サメへの弾を遮ったりしないように）
    int removed = 0;
    for (const auto &e : scene.GetEntities()) {
      if (!e || e->IsPendingDestroy()) continue;
      if (e->GetTagInt("is_enemy", 0) != 1 && e->GetTagInt("wave_id", 0) == 0) continue;
      if (e->HasTag("enemy_defeated")) continue;
      e->Destroy();
      ++removed;
    }
    // 進行中のウェーブを黙って打ち切る（WAVE の表示も消える）
    if (Entity *wm = scene.FindEntityByName("WaveManager")) wm->SetTag("wave_demo_abort", 1);
    // レールを終点の手前へ。以降のウェーブ地点では止まらず、サメを倒すまでは進まない
    cam.SetTag("demo_skip_waves", 1);
    cam.SetTag("demo_rail_hold", 1);
    cam.SetTag("demo_rail_jump", static_cast<int>(kFinalRunDistance * 100.0f));
    Log::Print("[ClearRunDemo] skip to final (removed " + std::to_string(removed) + " enemies)");
  }

  /// @brief 前方の水面にサメを出す
  /// @details WaveSpawnerScript::SpawnOne と同じ組み立て（Transform・モデル・球コライダー・SharkEnemyScript）で、
  ///          設定値も 1 面のウェーブのサメと同じにしている。置いた高さが SharkEnemyScript の「水面」になり、
  ///          生成直後に diveDepth だけ潜ってから近づいてくる。
  void SpawnShark(Scene &scene, const Entity &cam, const TransformComponent &tr, float waterY) {
    const float yaw = BaseYaw(cam, tr);
    const RC::Vector3 f = {std::sin(yaw), 0.0f, std::cos(yaw)};
    const RC::Vector3 r = {std::cos(yaw), 0.0f, -std::sin(yaw)};
    const RC::Vector3 c = tr.position;
    const RC::Vector3 start = {c.x + f.x * kSharkStartDistance + r.x * kSharkStartSide, waterY,
                               c.z + f.z * kSharkStartDistance + r.z * kSharkStartSide};

    auto e = scene.CreateEntity("WaveShark_demo");
    if (!e) return;
    e->SetTag("transient", 1); // エディタで保存しても JSON に残さない
    auto &str = e->AddComponent<TransformComponent>();
    str.position = start;
    str.scale = {kSharkScale, kSharkScale, kSharkScale};
    str.rotation = tr.rotation;
    auto &mr = e->AddComponent<ModelRendererComponent>();
    mr.modelPath = kSharkModel;
    auto &col = e->AddComponent<ColliderComponent>();
    col.shape = ColliderComponent::Shape::Sphere;
    col.radius = kSharkColliderRadius;
    col.isTrigger = false;
    auto &nsc = e->AddComponent<NativeScriptComponent>();
    nsc.AddScript(kSharkScript);
    if (!nsc.scripts.empty()) {
      // スクリプト実体は次の Update で作られるので、pendingData で渡す（WaveSpawner と同じ）
      nsc.scripts.back().pendingData = {
          {"detectDistance", kSharkDetectDistance},
          {"maxHp", kSharkHp},
          {"modelRotOffset", {0.0f, -90.0f, 0.0f}},
      };
    }
    scene.InitDynamicEntityRuntime(*e);
    shark_ = e;
    sharkWaterY_ = waterY;
    fightT_ = 0.0f;
    Log::Print("[ClearRunDemo] shark spawned");
  }

  /// @brief サメへ照準を寄せ、水面近くまで浮いてきたら撃つ
  /// @param cam Camera エンティティ
  /// @param tr カメラの Transform
  /// @param dt 経過時間（秒）
  /// @return サメが倒れた（消えた）なら true（呼び出し側の状態が AfterKill へ進む）
  bool UpdateFight(Entity &cam, const TransformComponent &tr, float dt) {
    auto s = shark_.lock();
    const bool gone = !s || s->IsPendingDestroy();
    if (gone || s->HasTag("enemy_defeated")) {
      return true;
    }
    const auto *str = s->GetComponent<TransformComponent>();
    if (!str) return false;
    // 潜っている間も目で追い、浮いてきたら撃つ（水中へ撃っても水面で止まるだけなので）
    const RC::Vector3 target = {str->position.x, str->position.y + 0.4f, str->position.z};
    const bool ready = str->position.y >= sharkWaterY_ - kSharkReadyDepth;
    AimAt(cam, tr, target, ready && fade_ <= 0.05f);
    fightT_ += dt;
    if (fightT_ >= kFightTimeout) {
      s->SetTag("pending_damage", 999); // 当たらないまま時間切れ。強制的に倒す（保険）
      Log::Print("[ClearRunDemo] fight timeout, forcing kill");
    }
    return false;
  }

  StateMachine<ClearRun> fsm_; ///< 段階（WaitIntroState 〜 DoneState）
  Frame frame_;                ///< このフレームの入力（Update の間だけ有効）
  float t_ = 0.0f;      ///< 現在の段階に入ってからの経過（秒）
  float fightT_ = 0.0f; ///< サメが出そろって撃ち始めてからの経過（秒）
  float fade_ = 0.0f;   ///< 暗転の濃さ（0〜1）
  float sharkWaterY_ = 0.0f; ///< サメを出したときの水面の高さ（撃ち始めの判定に使う）
  std::weak_ptr<Entity> shark_;
};

} // namespace TransitionDemo
