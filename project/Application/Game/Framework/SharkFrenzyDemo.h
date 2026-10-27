#pragma once
#include "ECS/TransformComponent.h"
#include "ECS/ModelRendererComponent.h"
#include "Common/Log/Log.h"
#include "Scene.h"
#include "Game/Framework/PartPose.h"
#include "Game/Framework/TransitionDemo.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <string>

// ============================================================================
// 見せ用デモ：WAVE 1 が始まった直後に、サメの群れが一斉に襲いかかって一気に食われる
//
//   DeathSinkScript（Game の Camera エンティティ）が TransitionDemo::kEnabled のときだけ使う。
//   ・サメは見た目だけ（敵スクリプト・コライダー・wave_id なし）。撃破数・ウェーブの生存数には数えない。
//   ・作るエンティティは "transient" タグ付き（エディタで保存しても JSON に残らない）。
//   ・出現の瞬間に止まらないよう、Prepare で先に作って非表示にしておき、Begin で出す。
//
//   流れ:
//     Begin  : カメラの前方に扇状（左右 ±75°・20〜31m 先）、水面の下に並べる。
//     突進   : 少しずつ時間差を付けて、加速しながら水中を詰め、後半で水面から飛び出して噛みつく
//              （カメラの前方 ±40° 程度・kBiteDistance m の位置に集まる）。
//     噛みつき: 1 匹目が届いた時点で Bitten() が true になる（呼び出し側が致死ダメージを入れる）。
//              噛みついたサメはカメラ位置に食らいついたまま首を振って暴れ、
//              カメラが沈めば（DeathSinkScript）一緒に水中へ引きずり込んでいく。
// ============================================================================

namespace TransitionDemo {

/// @brief WAVE 1 開始直後にサメの群れが一斉に襲いかかる演出
class SharkFrenzy {
public:
  static constexpr int kMax = 8; ///< 出せるサメの上限（kLayout の数）

  /// @brief 見た目だけのサメを非表示で作っておく（2 回目以降は何もしない）
  /// @param scene 作成先のシーン
  void Prepare(Scene *scene) {
    if (prepared_ || !scene) return;
    for (int i = 0; i < Count(); ++i) {
      auto e = scene->CreateEntity("DemoFrenzyShark");
      if (!e) continue;
      e->SetTag("transient", 1);
      auto &tr = e->AddComponent<TransformComponent>();
      tr.position = {0.0f, -500.0f, 0.0f};
      tr.scale = {kSharkScale, kSharkScale, kSharkScale};
      auto &mr = e->AddComponent<ModelRendererComponent>();
      mr.modelPath = kModelPath;
      scene->InitDynamicEntityRuntime(*e);
      e->SetActive(false);
      sharks_[static_cast<size_t>(i)].e = e;
    }
    prepared_ = true;
  }

  /// @brief 群れを出して突進を始める
  /// @param cam 襲われるカメラ
  /// @param waterY 水面の高さ
  /// @return 始めたら true（未準備・開始済みなら false）
  bool Begin(const TransformComponent &cam, float waterY) {
    if (!prepared_ || started_) return false;
    const float camYaw = cam.rotation.y;
    const RC::Vector3 camPos = cam.position;
    // 水面より上にいれば水中から飛び出させ、水中にいれば少し下から突き上げる
    const float approachY = (std::min)(camPos.y, waterY) - kApproachDepth;
    for (int i = 0; i < Count(); ++i) {
      const Layout &L = kLayout[static_cast<size_t>(i)];
      Shark &s = sharks_[static_cast<size_t>(i)];
      const float a0 = camYaw + L.angleDeg * PartPose::kDeg;          // 出現する向き
      const float a1 = camYaw + L.angleDeg * PartPose::kDeg * 0.55f;  // 噛みつく向き（視野の内側へ寄せる）
      s.start = {camPos.x + std::sin(a0) * L.startDist, approachY + L.depthJitter,
                 camPos.z + std::cos(a0) * L.startDist};
      const float biteDist = kBiteDistance * L.biteScale;
      s.biteOffset = {std::sin(a1) * biteDist, L.biteHeight, std::cos(a1) * biteDist};
      s.delay = L.delay;
      s.pos = s.start;
      s.yaw = std::atan2(camPos.x - s.start.x, camPos.z - s.start.z);
      s.pitchUp = 0.0f;
      s.roll = 0.0f;
      s.arrived = false;
      s.holdT = 0.0f;
      s.phase = 1.7f * static_cast<float>(i);
      if (auto e = s.e.lock()) e->SetActive(true);
      Pose(s);
    }
    started_ = true;
    bitten_ = false;
    t_ = 0.0f;
    Log::Print("[SharkFrenzy] begin (" + std::to_string(Count()) + " sharks)");
    return true;
  }

  /// @brief 突進・噛みつき・暴れる動きを進める
  /// @param cam 襲われているカメラ（沈んでいく位置にも追従する）
  /// @param dt 前フレームからの経過時間（秒）
  void Update(const TransformComponent &cam, float dt) {
    if (!started_) return;
    t_ += dt;
    for (int i = 0; i < Count(); ++i) {
      Shark &s = sharks_[static_cast<size_t>(i)];
      if (!s.arrived) {
        const float u = std::clamp((t_ - s.delay) / (std::max)(kLungeTime, 1e-3f), 0.0f, 1.0f);
        const RC::Vector3 bite = {cam.position.x + s.biteOffset.x, cam.position.y + s.biteOffset.y,
                                  cam.position.z + s.biteOffset.z};
        // 水平は加速しながら詰め、高さは前半は水中を進んで後半で一気に飛び出す
        const float h = 0.3f * u + 0.7f * u * u;
        const float v = Smooth01((u - 0.45f) / 0.55f);
        const RC::Vector3 p = {s.start.x + (bite.x - s.start.x) * h, s.start.y + (bite.y - s.start.y) * v,
                               s.start.z + (bite.z - s.start.z) * h};
        const RC::Vector3 d = {p.x - s.pos.x, p.y - s.pos.y, p.z - s.pos.z};
        const float hl = std::sqrt(d.x * d.x + d.z * d.z);
        if (hl > 1e-4f) {
          s.yaw = std::atan2(d.x, d.z);
          s.pitchUp = std::atan2(d.y, hl);
        }
        s.pos = p;
        s.wag = 0.18f * std::sin(t_ * 9.0f + s.phase); // 尾を振って泳ぐ
        s.roll = 0.10f * std::sin(t_ * 6.0f + s.phase);
        if (u >= 1.0f) {
          s.arrived = true;
          s.holdT = 0.0f;
          if (!bitten_) Log::Print("[SharkFrenzy] bite");
          bitten_ = true;
        }
      } else {
        // 食らいついたまま首を振って暴れる。位置はカメラに追従（沈めば一緒に引きずり込む）
        s.holdT += dt;
        RC::Vector3 p = {cam.position.x + s.biteOffset.x, cam.position.y + s.biteOffset.y,
                         cam.position.z + s.biteOffset.z};
        p.x += 0.15f * std::sin(s.holdT * 21.0f + s.phase);
        p.y += 0.12f * std::sin(s.holdT * 17.0f + s.phase * 0.7f);
        p.z += 0.15f * std::cos(s.holdT * 19.0f + s.phase);
        s.pos = p;
        const RC::Vector3 d = {cam.position.x - p.x, cam.position.y - p.y, cam.position.z - p.z};
        const float hl = std::sqrt(d.x * d.x + d.z * d.z);
        if (hl > 1e-4f) {
          s.yaw = std::atan2(d.x, d.z);
          s.pitchUp = std::atan2(d.y, hl);
        }
        s.wag = 0.35f * std::sin(s.holdT * 16.0f + s.phase);
        s.roll = 0.45f * std::sin(s.holdT * 12.0f + s.phase);
      }
      Pose(s);
    }
  }

  /// @brief 作ったサメを片付ける
  void Destroy() {
    for (auto &s : sharks_) {
      if (auto e = s.e.lock()) e->Destroy();
      s.e.reset();
    }
    prepared_ = false;
    started_ = false;
    bitten_ = false;
  }

  /// @brief 群れを出したか
  /// @return Begin 済みなら true
  bool Started() const { return started_; }
  /// @brief 1 匹目が噛みついたか
  /// @return 噛みついていれば true
  bool Bitten() const { return bitten_; }

private:
  /// @brief 1 匹ぶんの出現位置・噛みつく位置・時間差（カメラの向き基準）
  struct Layout {
    float angleDeg;    ///< 出現する向き（カメラ正面からの角度。正で右）
    float startDist;   ///< 出現する距離（m）
    float depthJitter; ///< 出現する深さのずれ（m）
    float delay;       ///< 動き出すまでの時間差（秒）
    float biteScale;   ///< kBiteDistance に掛ける倍率
    float biteHeight;  ///< 噛みつく高さ（カメラからの差。m）
  };
  static constexpr std::array<Layout, kMax> kLayout = {{
      {-34.0f, 27.0f, 0.0f, 0.00f, 1.00f, -0.5f},
      {28.0f, 25.0f, -0.5f, 0.07f, 0.95f, 0.4f},
      {-10.0f, 31.0f, 0.6f, 0.13f, 1.10f, 0.9f},
      {50.0f, 23.0f, -0.3f, 0.20f, 1.05f, -0.8f},
      {6.0f, 29.0f, 0.3f, 0.27f, 0.90f, -0.1f},
      {-58.0f, 22.0f, -0.6f, 0.33f, 1.15f, 0.5f},
      {70.0f, 20.0f, 0.2f, 0.40f, 1.10f, 0.2f},
      {-75.0f, 21.0f, -0.2f, 0.46f, 1.10f, -0.4f},
  }};

  struct Shark {
    std::weak_ptr<Entity> e;
    RC::Vector3 start{};
    RC::Vector3 biteOffset{}; ///< 噛みついたときのカメラからの位置（ワールドの向き）
    RC::Vector3 pos{};
    float delay = 0.0f;
    float yaw = 0.0f;
    float pitchUp = 0.0f;
    float roll = 0.0f;
    float wag = 0.0f;
    float phase = 0.0f;
    float holdT = 0.0f;
    bool arrived = false;
  };

  static constexpr const char *kModelPath = "Resources/model/shark/shark_game.glb";
  static constexpr float kModelYawDeg = -90.0f; ///< モデルの頭は +X なので -90° で +Z に合わせる
  static constexpr float kApproachDepth = 3.5f; ///< 水面（またはカメラ）からどれだけ下を泳いでくるか（m）

  static int Count() { return std::clamp(kSharkCount, 1, kMax); }

  static float Smooth01(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
  }

  /// @brief サメの姿勢を Transform へ書く（SharkChaseCutscene::PoseShark と同じ合成）
  static void Pose(const Shark &s) {
    auto e = s.e.lock();
    if (!e) return;
    auto *tr = e->GetComponent<TransformComponent>();
    if (!tr) return;
    using namespace PartPose;
    tr->position = s.pos;
    const Mat3 m = MatMul(RotY(kModelYawDeg * kDeg), BodyMatrix(s.yaw + s.wag, s.pitchUp, s.roll));
    tr->rotation = ToEngineEuler(m);
    tr->scale = {kSharkScale, kSharkScale, kSharkScale};
  }

  std::array<Shark, kMax> sharks_{};
  bool prepared_ = false;
  bool started_ = false;
  bool bitten_ = false;
  float t_ = 0.0f;
};

} // namespace TransitionDemo
