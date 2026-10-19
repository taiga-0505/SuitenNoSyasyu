#pragma once
#include "ECS/TransformComponent.h"
#include "ECS/PrimitiveMeshComponent.h"
#include "ECS/ModelRendererComponent.h"
#include "ECS/GPUParticleComponent.h"
#include "ECS/NativeScriptComponent.h"
#include "Particle/GPUParticle.h"
#include "Audio/AudioEngine.h"
#include "Common/Log/Log.h"
#include "RenderCommon.h"
#include "Scene.h"
#include "Game/Framework/PartPose.h"

#include <nlohmann/json.hpp>

#if RC_ENABLE_IMGUI
#include "imgui/imgui.h"
#endif

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

// ============================================================================
// ステージ開始の「サメに追われる → 振り向いて水の弾で倒す」三人称カットシーン
//
//   DeepRiseIntroScript が浮上の前に再生する（"sharkChase": { "enabled": true } のとき）。
//   深海から斜め上へ逃げるタコ（タイトルと同じくプリミティブの組み合わせ）を、
//   後ろからサメ（shark_game.glb）が追う。2 秒ほど追われたら振り向き、水の弾（プレイヤーの弾と
//   同じ水の質感）を撃って倒し、
//   カメラがタコの頭へ寄って一人称に戻る。その位置から DeepRiseIntroScript の浮上が続く。
//
//   流れ（秒は既定値）:
//     Chase 2.0s : タコが startOffset → endOffset を逃げる。サメは後ろから距離を詰める。
//                  カメラは横から 2 匹を収める（深いうちはフォグでサメが影のように見える）。
//     Turn  0.45s: タコが減速しながら振り向き、足を広げて威嚇。サメは噛みつきに来る。
//                  カメラはタコの肩越しへ回り込む。
//     Shoot      : shotCount 発の水の弾を shotInterval おきに撃つ。当たるたびにサメが赤く光って
//                  のけぞり、水しぶきと泡が散る（WaterBullet の着弾と同じエフェクト粒）。
//                  最後の 1 発で撃破（大きなしぶき・泡、腹を見せて沈む）。
//     Merge 0.8s : タコが前へ向き直り、カメラがタコの頭の位置・浮上開始の向きへ寄って一人称へ。
//     Done       : カメラは浮上側へ返す。沈むサメはしばらく残し、消えたら後片付け。
//
//   ・座標は「ゴール（レールの開始位置）」基準。x = 右、y = 上、z = 前（ゴールの向き）。
//   ・作るエンティティは全部 "transient" タグ付き（エディタで保存しても JSON に残らない）。
//   ・プリミティブは 1 フレームに作りすぎると止まるので、kBuildPerFrame 個ずつ作る。
//   ・サメは敵スクリプトを付けない見た目だけのもの（撃破数・スコアには数えない）。
// ============================================================================

/// @brief サメ追跡カットシーンの設定（DeepRiseIntroScript の "sharkChase" に入る）
struct SharkChaseParams {
  bool enabled = false; ///< 浮上の前にこのカットシーンを再生するか

  // ---- 時間（秒）----
  float chaseDuration = 2.0f;  ///< 追われている時間
  float turnDuration = 0.45f;  ///< 振り向く時間
  int shotCount = 3;           ///< 水の弾を撃つ回数（最後の 1 発で倒す）
  float shotInterval = 0.17f;  ///< 水の弾の発射間隔
  float shotTravel = 0.2f;     ///< 水の弾がサメに届くまで
  float killHold = 0.35f;      ///< 倒してから一人称へ寄り始めるまで
  float mergeDuration = 0.8f;  ///< カメラがタコの頭へ寄る時間

  // ---- 経路（ゴール基準。x 右 / y 上 / z 前）----
  RC::Vector3 startOffset = {0.0f, -25.0f, -24.0f}; ///< 逃げ始める位置
  RC::Vector3 endOffset = {0.0f, -16.0f, -8.0f};    ///< 振り向き始める位置

  // ---- サメ ----
  std::string sharkModel = "Resources/model/shark/shark_game.glb";
  float sharkScale = 6.0f;         ///< WaveSpawner の enemyScale と同じ
  float sharkModelYawDeg = -90.0f; ///< モデルの頭は +X なので -90° で +Z に合わせる
  float sharkStartGap = 14.0f;     ///< 追い始めのタコとの距離（m）
  float sharkEndGap = 8.0f;        ///< 追い終わりの距離
  float sharkLungeGap = 6.0f;      ///< 振り向くあいだに噛みつきに来る距離
  float sharkAimForward = 1.2f;    ///< 弾の狙い（モデル原点から頭側へ何 m）

  // ---- タコ ----
  float octopusScale = 0.9f;
  RC::Vector4 bodyColor = {0.93f, 0.40f, 0.33f, 1.0f};
  RC::Vector4 shotColor = {0.2f, 0.6f, 1.0f, 0.85f}; ///< 水の弾の色（RailShooterController の自機弾と同じ）
  float shotSize = 0.3f;                             ///< 水の弾の半径（通常弾と同じ）

  // ---- カメラ ----
  float camSide = 8.5f;    ///< 追跡中：横へ離れる距離（2 匹の間が開いたぶん広めに引く）
  float camHeight = 1.4f;  ///< 追跡中：上へずらす量
  float camLead = 2.0f;    ///< 追跡中：前へずらす量
  float camBack = 3.2f;    ///< 肩越し：タコの後ろへ離れる距離
  float camFollow = 5.0f;  ///< カメラの追従の速さ（大きいほどきびきび）

  // ---- 効果音（空なら鳴らさない）----
  std::string seShot;
  std::string seHit;
  std::string seKill;

  /// @brief JSON へ書き出す
  /// @return 全項目を持つ JSON オブジェクト
  nlohmann::json ToJson() const {
    auto v3 = [](const RC::Vector3 &v) { return nlohmann::json{v.x, v.y, v.z}; };
    auto v4 = [](const RC::Vector4 &c) { return nlohmann::json{c.x, c.y, c.z, c.w}; };
    return {
        {"enabled", enabled},
        {"chaseDuration", chaseDuration},
        {"turnDuration", turnDuration},
        {"shotCount", shotCount},
        {"shotInterval", shotInterval},
        {"shotTravel", shotTravel},
        {"killHold", killHold},
        {"mergeDuration", mergeDuration},
        {"startOffset", v3(startOffset)},
        {"endOffset", v3(endOffset)},
        {"sharkModel", sharkModel},
        {"sharkScale", sharkScale},
        {"sharkModelYawDeg", sharkModelYawDeg},
        {"sharkStartGap", sharkStartGap},
        {"sharkEndGap", sharkEndGap},
        {"sharkLungeGap", sharkLungeGap},
        {"sharkAimForward", sharkAimForward},
        {"octopusScale", octopusScale},
        {"bodyColor", v4(bodyColor)},
        {"shotColor", v4(shotColor)},
        {"shotSize", shotSize},
        {"camSide", camSide},
        {"camHeight", camHeight},
        {"camLead", camLead},
        {"camBack", camBack},
        {"camFollow", camFollow},
        {"seShot", seShot},
        {"seHit", seHit},
        {"seKill", seKill},
    };
  }

  /// @brief JSON から読む。無いキー・型の違うキーは既定値のまま
  /// @param j 読み込む JSON（オブジェクトでなければ何もしない）
  void FromJson(const nlohmann::json &j) {
    if (!j.is_object()) return;
    auto readF = [&](const char *key, float &out) {
      if (j.contains(key) && j[key].is_number()) out = j[key].get<float>();
    };
    auto readI = [&](const char *key, int &out) {
      if (j.contains(key) && j[key].is_number()) out = j[key].get<int>();
    };
    auto readB = [&](const char *key, bool &out) {
      if (j.contains(key) && j[key].is_boolean()) out = j[key].get<bool>();
    };
    auto readS = [&](const char *key, std::string &out) {
      if (j.contains(key) && j[key].is_string()) out = j[key].get<std::string>();
    };
    auto readV3 = [&](const char *key, RC::Vector3 &out) {
      if (!j.contains(key)) return;
      const auto &a = j[key];
      if (a.is_array() && a.size() >= 3) out = {a[0].get<float>(), a[1].get<float>(), a[2].get<float>()};
    };
    auto readV4 = [&](const char *key, RC::Vector4 &out) {
      if (!j.contains(key)) return;
      const auto &a = j[key];
      if (a.is_array() && a.size() >= 4) {
        out = {a[0].get<float>(), a[1].get<float>(), a[2].get<float>(), a[3].get<float>()};
      }
    };
    readB("enabled", enabled);
    readF("chaseDuration", chaseDuration);
    readF("turnDuration", turnDuration);
    readI("shotCount", shotCount);
    readF("shotInterval", shotInterval);
    readF("shotTravel", shotTravel);
    readF("killHold", killHold);
    readF("mergeDuration", mergeDuration);
    readV3("startOffset", startOffset);
    readV3("endOffset", endOffset);
    readS("sharkModel", sharkModel);
    readF("sharkScale", sharkScale);
    readF("sharkModelYawDeg", sharkModelYawDeg);
    readF("sharkStartGap", sharkStartGap);
    readF("sharkEndGap", sharkEndGap);
    readF("sharkLungeGap", sharkLungeGap);
    readF("sharkAimForward", sharkAimForward);
    readF("octopusScale", octopusScale);
    readV4("bodyColor", bodyColor);
    readV4("shotColor", shotColor);
    readF("shotSize", shotSize);
    readF("camSide", camSide);
    readF("camHeight", camHeight);
    readF("camLead", camLead);
    readF("camBack", camBack);
    readF("camFollow", camFollow);
    readS("seShot", seShot);
    readS("seHit", seHit);
    readS("seKill", seKill);
    shotCount = std::clamp(shotCount, 1, kMaxShots);
  }

#if RC_ENABLE_IMGUI
  /// @brief Inspector 用の調整 UI
  void DrawImGui() {
    if (!ImGui::TreeNode("Shark Chase")) return;
    ImGui::Checkbox("Enabled##chase", &enabled);
    ImGui::SeparatorText("Timing");
    ImGui::DragFloat("Chase (s)", &chaseDuration, 0.05f, 0.2f, 10.0f);
    ImGui::DragFloat("Turn (s)", &turnDuration, 0.01f, 0.05f, 3.0f);
    ImGui::SliderInt("Shots", &shotCount, 1, kMaxShots);
    ImGui::DragFloat("Shot Interval (s)", &shotInterval, 0.01f, 0.02f, 1.0f);
    ImGui::DragFloat("Shot Travel (s)", &shotTravel, 0.01f, 0.02f, 1.0f);
    ImGui::DragFloat("Kill Hold (s)", &killHold, 0.01f, 0.0f, 3.0f);
    ImGui::DragFloat("Merge (s)", &mergeDuration, 0.01f, 0.1f, 3.0f);
    ImGui::SeparatorText("Path (goal frame: x right / y up / z fwd)");
    ImGui::DragFloat3("Start Offset", &startOffset.x, 0.1f);
    ImGui::DragFloat3("End Offset", &endOffset.x, 0.1f);
    ImGui::SeparatorText("Shark");
    ImGui::DragFloat("Shark Scale", &sharkScale, 0.05f, 0.1f, 30.0f);
    ImGui::DragFloat("Shark Model Yaw (deg)", &sharkModelYawDeg, 1.0f, -360.0f, 360.0f);
    ImGui::DragFloat("Gap Start", &sharkStartGap, 0.1f, 0.5f, 40.0f);
    ImGui::DragFloat("Gap End", &sharkEndGap, 0.1f, 0.5f, 40.0f);
    ImGui::DragFloat("Gap Lunge", &sharkLungeGap, 0.1f, 0.5f, 40.0f);
    ImGui::DragFloat("Aim Forward", &sharkAimForward, 0.05f, -5.0f, 5.0f);
    ImGui::SeparatorText("Octopus");
    ImGui::DragFloat("Octopus Scale", &octopusScale, 0.01f, 0.2f, 4.0f);
    ImGui::ColorEdit4("Body", &bodyColor.x);
    ImGui::ColorEdit4("Shot Color", &shotColor.x);
    ImGui::DragFloat("Shot Size", &shotSize, 0.01f, 0.05f, 2.0f);
    ImGui::SeparatorText("Camera");
    ImGui::DragFloat("Side", &camSide, 0.05f, 0.0f, 30.0f);
    ImGui::DragFloat("Height", &camHeight, 0.05f, -10.0f, 10.0f);
    ImGui::DragFloat("Lead", &camLead, 0.05f, -10.0f, 10.0f);
    ImGui::DragFloat("Back (over shoulder)", &camBack, 0.05f, 0.5f, 20.0f);
    ImGui::DragFloat("Follow", &camFollow, 0.1f, 0.5f, 30.0f);
    ImGui::TextDisabled("colors / model / SE are applied on Replay");
    ImGui::TreePop();
  }
#endif

  static constexpr int kMaxShots = 8;
};

/// @class SharkChaseCutscene
/// @brief 「サメに追われる → 振り向いて水の弾で倒す」を再生する（DeepRiseIntroScript が持つ）
/// @details 使い方:
///            Begin(...)       : 開始。カメラを最初の構図へ置く
///            Update(dt, &cam) : 毎フレーム。IsDirecting() のあいだはカメラを書き換える。
///                               Done 後も nullptr で呼び続けると、沈むサメを最後まで動かして片付ける
///            Cleanup()        : 途中でやめる・シーン終了時に全部消す
class SharkChaseCutscene {
public:
  enum class Phase { Idle, Chase, Turn, Shoot, Merge, Done };

  ~SharkChaseCutscene() = default;

  /// @brief 再生を始める
  /// @param scene     エンティティを作るシーン
  /// @param ctx       シーンコンテキスト（泡のスクリプトに渡す。nullptr 可）
  /// @param p         設定
  /// @param goalPos   レールの開始位置（カメラが最終的に戻る場所）
  /// @param endCamRot 一人称に戻ったときのカメラの向き（浮上の開始の向き）
  /// @param cam       カメラの Transform（最初の構図を書き込む）
  void Begin(Scene *scene, SceneContext *ctx, const SharkChaseParams &p, const RC::Vector3 &goalPos,
             const RC::Vector3 &endCamRot, TransformComponent &cam) {
    Cleanup();
    scene_ = scene;
    ctx_ = ctx;
    p_ = p;
    p_.shotCount = std::clamp(p_.shotCount, 1, SharkChaseParams::kMaxShots);
    endRot_ = endCamRot;

    // ゴールの向き（ヨー）で x 右 / z 前 を決める
    yaw_ = endCamRot.y;
    F_ = {std::sin(yaw_), 0.0f, std::cos(yaw_)};
    R_ = {std::cos(yaw_), 0.0f, -std::sin(yaw_)};
    start_ = FromGoal(goalPos, p_.startOffset);
    end_ = FromGoal(goalPos, p_.endOffset);
    RC::Vector3 d = Sub(end_, start_);
    const float len = Len(d);
    travelDir_ = (len > 1e-4f) ? Scale(d, 1.0f / len) : F_;
    speed_ = len / (std::max)(p_.chaseDuration, 1e-3f);

    // 状態
    phase_ = Phase::Chase;
    t_ = 0.0f;
    time_ = 0.0f;
    octPos_ = start_;
    octYaw_ = yaw_;
    octPitchUp_ = 0.0f;
    splay_ = 0.3f;
    recoil_ = 0.0f;
    recoilVel_ = 0.0f;
    lookSide_ = 0.0f;
    octVisible_ = true;
    sharkPos_ = Sub(start_, Scale(travelDir_, p_.sharkStartGap));
    sharkYaw_ = yaw_;
    sharkPitchUp_ = 0.0f;
    sharkRoll_ = 0.0f;
    sharkAlive_ = true;
    sharkDeadT_ = 0.0f;
    sinkVel_ = 0.0f;
    flashTimer_ = 0.0f;
    knock_ = {0, 0, 0};
    knockVel_ = {0, 0, 0};
    shotsFired_ = 0;
    hitsLanded_ = 0;
    killTime_ = -1.0f;
    shake_ = 0.0f;
    built_ = 0;

    // 効果音
    seShot_ = LoadSe(p_.seShot);
    seHit_ = LoadSe(p_.seHit);
    seKill_ = LoadSe(p_.seKill);

    SpawnShark();
    BuildSome();

    // カメラ：最初の構図へそのまま置く
    RC::Vector3 pos, look;
    DesiredCamera(pos, look);
    camPos_ = pos;
    camLook_ = look;
    WriteCamera(cam, camPos_, LookRot(camPos_, camLook_, 0.0f));
    Log::Print("[SharkChaseCutscene] begin");
  }

  /// @brief 1 フレーム進める
  /// @param dt  経過秒（0 以下なら何もしない）
  /// @param cam カメラ。IsDirecting() のあいだだけ書き換える（nullptr 可）
  void Update(float dt, TransformComponent *cam) {
    if (phase_ == Phase::Idle || dt <= 0.0f) return;
    time_ += dt;
    t_ += dt;
    BuildSome();

    switch (phase_) {
    case Phase::Chase: UpdateChase(); break;
    case Phase::Turn: UpdateTurn(); break;
    case Phase::Shoot: UpdateShoot(dt); break;
    case Phase::Merge: UpdateMerge(); break;
    default: break;
    }

    UpdateShark(dt);
    UpdateShots(dt);
    UpdateOctopusSpring(dt);
    PoseOctopus();
    PoseShark();

    if (cam && phase_ != Phase::Done) UpdateCamera(dt, *cam);
    if (phase_ == Phase::Merge && mergeDone_) {
      Enter(Phase::Done);
      PoseOctopus(); // タコを隠す
      Log::Print("[SharkChaseCutscene] handed camera back");
    }

    // 演出が終わり、サメも消えたら後片付け
    if (phase_ == Phase::Done && sharkEntity_.expired()) {
      Cleanup();
    }
  }

  /// @brief カメラを握っているか（Chase〜Merge のあいだ）
  bool IsDirecting() const {
    return phase_ == Phase::Chase || phase_ == Phase::Turn || phase_ == Phase::Shoot || phase_ == Phase::Merge;
  }

  /// @brief 再生中か（演出後の後片付け待ちも含む）
  bool IsActive() const { return phase_ != Phase::Idle; }

  /// @brief 泡を湧かせる基準位置（タコの位置）
  const RC::Vector3 &OctopusPos() const { return octPos_; }

  /// @brief 今の段階の名前（ImGui 表示用）
  const char *PhaseName() const {
    switch (phase_) {
    case Phase::Idle: return "idle";
    case Phase::Chase: return "chase";
    case Phase::Turn: return "turn";
    case Phase::Shoot: return "shoot";
    case Phase::Merge: return "merge";
    case Phase::Done: return "done";
    }
    return "?";
  }

  /// @brief 作ったエンティティと効果音をすべて片付けて Idle に戻す
  void Cleanup() {
    auto kill = [](std::weak_ptr<Entity> &w) {
      if (auto e = w.lock()) e->Destroy();
      w.reset();
    };
    for (auto &w : octParts_) kill(w);
    octParts_.clear();
    for (auto &s : shots_) kill(s.e);
    shots_.clear();
    kill(sharkEntity_);
    kill(folder_);
    UnloadSe(seShot_);
    UnloadSe(seHit_);
    UnloadSe(seKill_);
    built_ = 0;
    phase_ = Phase::Idle;
  }

private:
  using Mat3 = PartPose::Mat3;

  // ---- 部位の並び（octParts_ の添字）----
  static constexpr int kMantle = 0;
  static constexpr int kFace = 1;
  static constexpr int kEyeL = 2;
  static constexpr int kEyeR = 3;
  static constexpr int kPupilL = 4;
  static constexpr int kPupilR = 5;
  static constexpr int kTentacleStart = 6;
  static constexpr int kTentacles = 8;
  static constexpr int kSegments = 5;
  static constexpr int kOctParts = kTentacleStart + kTentacles * kSegments;
  static constexpr int kBuildPerFrame = 16;

  struct Shot {
    std::weak_ptr<Entity> e;
    bool active = false;
    float t = 0.0f;
    RC::Vector3 from{};
  };

  // ------------------------------------------------------------------
  // ベクトルの小道具（RC の演算子に頼らない）
  // ------------------------------------------------------------------
  static RC::Vector3 Add(const RC::Vector3 &a, const RC::Vector3 &b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
  static RC::Vector3 Sub(const RC::Vector3 &a, const RC::Vector3 &b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
  static RC::Vector3 Scale(const RC::Vector3 &a, float s) { return {a.x * s, a.y * s, a.z * s}; }
  static float Len(const RC::Vector3 &a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }
  static RC::Vector3 Norm(const RC::Vector3 &a, const RC::Vector3 &fallback) {
    const float l = Len(a);
    return (l > 1e-5f) ? Scale(a, 1.0f / l) : fallback;
  }
  static RC::Vector3 LerpV(const RC::Vector3 &a, const RC::Vector3 &b, float t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
  }
  static float Smooth01(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
  }
  /// @brief 角度の補間（差を -π〜π に折り返してから）
  static float LerpAngle(float a, float b, float t) { return a + PartPose::WrapAngle(b - a) * t; }

  /// @brief ゴール基準のオフセット（x 右 / y 上 / z 前）をワールド座標にする
  RC::Vector3 FromGoal(const RC::Vector3 &goal, const RC::Vector3 &o) const {
    return {goal.x + R_.x * o.x + F_.x * o.z, goal.y + o.y, goal.z + R_.z * o.x + F_.z * o.z};
  }

  /// @brief from から to を見るカメラの回転（エンジンのオイラー角。x 負で見上げ）
  /// @details 回転 Rx(a)·Ry(b) の前方は (cos a sin b, -sin a, cos a cos b)
  static RC::Vector3 LookRot(const RC::Vector3 &from, const RC::Vector3 &to, float roll) {
    const RC::Vector3 d = Norm(Sub(to, from), {0.0f, 0.0f, 1.0f});
    return {-std::asin(std::clamp(d.y, -1.0f, 1.0f)), std::atan2(d.x, d.z), roll};
  }

  static void WriteCamera(TransformComponent &cam, const RC::Vector3 &pos, const RC::Vector3 &rot) {
    cam.position = pos;
    cam.rotation = rot;
  }

  // ------------------------------------------------------------------
  // 効果音
  // ------------------------------------------------------------------
  static int LoadSe(const std::string &path) {
    if (path.empty()) return -1;
    return AudioEngine::Get().LoadClip(path);
  }
  static void UnloadSe(int &clip) {
    if (clip >= 0) AudioEngine::Get().UnloadClip(clip);
    clip = -1;
  }
  static void PlaySe(int clip, float volume = 1.0f) {
    if (clip >= 0) AudioEngine::Get().PlaySe(clip, volume);
  }

  // ------------------------------------------------------------------
  // エンティティの生成
  // ------------------------------------------------------------------

  /// @brief プリミティブを 1 つ作る（非表示・画面外で作る）
  std::shared_ptr<Entity> MakePrimitive(const std::string &name, const RC::Vector4 &color, float shininess,
                                        PrimitiveDrawStyle style = PrimitiveDrawStyle::Default) {
    if (!scene_) return nullptr;
    auto e = scene_->CreateEntity(name);
    if (!e) return nullptr;
    if (auto f = folder_.lock()) e->SetParentGuid(f->Guid());
    e->SetTag("transient", 1);
    auto &tr = e->AddComponent<TransformComponent>();
    tr.position = {0.0f, -500.0f, 0.0f};
    tr.scale = {0.01f, 0.01f, 0.01f};
    auto &pm = e->AddComponent<PrimitiveMeshComponent>();
    pm.type = PrimitiveType::Sphere;
    pm.color = color;
    pm.shininess = shininess;
    pm.drawStyle = style;
    scene_->InitDynamicEntityRuntime(*e);
    e->SetActive(false);
    return e;
  }

  /// @brief タコの部位・水の弾を kBuildPerFrame 個ずつ作る
  void BuildSome() {
    if (!scene_) return;
    const int total = kOctParts + p_.shotCount;
    if (built_ >= total) return;
    if (!folder_.lock()) {
      auto folder = scene_->CreateEntity("SharkChase_Parts");
      if (folder) {
        folder->SetIsFolder(true);
        folder->SetTag("transient", 1);
        folder_ = folder;
      }
    }
    const RC::Vector4 body = p_.bodyColor;
    const RC::Vector4 skinDark = {body.x * 0.85f, body.y * 0.8f, body.z * 0.8f, body.w};
    const RC::Vector4 eye = {0.98f, 0.98f, 0.96f, 1.0f};
    const RC::Vector4 pupil = {0.05f, 0.05f, 0.07f, 1.0f};
    for (int n = 0; n < kBuildPerFrame && built_ < total; ++n, ++built_) {
      const int i = built_;
      if (i < kOctParts) {
        std::shared_ptr<Entity> e;
        if (i == kMantle || i == kFace) e = MakePrimitive("SharkChase_OctBody", body, 24.0f);
        else if (i == kEyeL || i == kEyeR) e = MakePrimitive("SharkChase_OctEye", eye, 48.0f);
        else if (i == kPupilL || i == kPupilR) e = MakePrimitive("SharkChase_OctPupil", pupil, 48.0f);
        else {
          const int t = (i - kTentacleStart) / kSegments;
          e = MakePrimitive("SharkChase_OctLeg", (t % 2 == 0) ? body : skinDark, 24.0f);
        }
        octParts_.push_back(e);
      } else {
        // 水の弾：自機弾と同じく水の質感（PrimitiveDrawStyle::Water）で描く
        Shot s;
        s.e = MakePrimitive("SharkChase_WaterShot", p_.shotColor, 64.0f, PrimitiveDrawStyle::Water);
        shots_.push_back(s);
      }
    }
  }

  /// @brief 見た目だけのサメを作る（敵スクリプト・コライダーは付けない）
  void SpawnShark() {
    if (!scene_) return;
    auto e = scene_->CreateEntity("SharkChase_Shark");
    if (!e) return;
    e->SetTag("transient", 1);
    auto &tr = e->AddComponent<TransformComponent>();
    tr.position = sharkPos_;
    tr.scale = {p_.sharkScale, p_.sharkScale, p_.sharkScale};
    auto &mr = e->AddComponent<ModelRendererComponent>();
    mr.modelPath = p_.sharkModel;
    scene_->InitDynamicEntityRuntime(*e);
    sharkEntity_ = e;
  }

  /// @brief 撃破時の泡（敵の撃破と同じ GPUBubbleEmitter）
  void SpawnBubbleBurst(const RC::Vector3 &pos) {
    if (!scene_) return;
    auto emitter = scene_->CreateEntity("SharkChase_Bubbles");
    if (!emitter) return;
    emitter->SetTag("transient", 1);
    auto &tr = emitter->AddComponent<TransformComponent>();
    tr.position = pos;
    auto &gpu = emitter->AddComponent<GPUParticleComponent>();
    if (gpu.particleSystem) gpu.particleSystem->SetPipelinePrefix("gpu_particle_bubble");
    auto &nsc = emitter->AddComponent<NativeScriptComponent>();
    nsc.AddScript("GPUBubbleEmitter");
    nsc.SetScene(scene_);
    if (ctx_) nsc.SetSceneContext(ctx_);
    scene_->InitDynamicEntityRuntime(*emitter);
  }

  /// @brief 部位へワールド姿勢を書き、描画側へも即座に写す
  static void Place(const std::weak_ptr<Entity> &w, const RC::Vector3 &pos, const Mat3 &rot,
                    const RC::Vector3 &scl) {
    auto e = w.lock();
    if (!e) return;
    auto *tr = e->GetComponent<TransformComponent>();
    if (!tr) return;
    tr->position = pos;
    tr->rotation = PartPose::ToEngineEuler(rot);
    tr->scale = scl;
    if (auto *pm = e->GetComponent<PrimitiveMeshComponent>()) {
      if (pm->meshHandle >= 0) {
        if (auto *t = RC::GetPrimitiveMeshTransformPtr(pm->meshHandle)) *t = tr->ToTransform();
      }
    }
  }

  static void SetActive(const std::weak_ptr<Entity> &w, bool on) {
    if (auto e = w.lock()) {
      if (e->IsActive() != on) e->SetActive(on);
    }
  }

  // ------------------------------------------------------------------
  // 段階ごとの更新
  // ------------------------------------------------------------------

  /// @brief タコからサメへの向き
  RC::Vector3 ToShark() const { return Norm(Sub(sharkPos_, octPos_), Scale(travelDir_, -1.0f)); }

  /// @brief 追われている：経路を逃げる。足は泳ぐたびに開いて閉じる
  void UpdateChase() {
    const float u = std::clamp(t_ / (std::max)(p_.chaseDuration, 1e-3f), 0.0f, 1.0f);
    const float pulse = 0.5f + 0.5f * std::sin(time_ * PartPose::kTwoPi * 2.2f);
    octPos_ = LerpV(start_, end_, u);
    octPos_.y += 0.08f * std::sin(time_ * PartPose::kTwoPi * 2.2f);
    octYaw_ = yaw_;
    octPitchUp_ = std::asin(std::clamp(travelDir_.y, -1.0f, 1.0f)) * 0.7f;
    splay_ = 0.25f + 0.3f * pulse;
    // 終わりぎわに「後ろが気になる」：黒目を横へ
    lookSide_ = Smooth01((u - 0.55f) / 0.25f);

    const float gap = p_.sharkStartGap + (p_.sharkEndGap - p_.sharkStartGap) * Smooth01(u);
    sharkPos_ = Sub(octPos_, Scale(travelDir_, gap));
    sharkPos_ = Add(sharkPos_, Scale(R_, 0.5f * std::sin(time_ * PartPose::kTwoPi * 1.1f)));

    if (u >= 1.0f) Enter(Phase::Turn);
  }

  /// @brief 振り向く：減速しながら向きを変え、足を広げる。サメは噛みつきに来る
  void UpdateTurn() {
    const float T = (std::max)(p_.turnDuration, 1e-3f);
    const float u = std::clamp(t_ / T, 0.0f, 1.0f);
    const float e = Smooth01(u);
    // 速度を直線的に 0 まで落とす（移動距離 = v0·T·(u - u²/2)）
    octPos_ = Add(end_, Scale(travelDir_, speed_ * T * (u - 0.5f * u * u)));
    if (turnDiff_ == 0.0f) { // 入った最初のフレームで、サメの方への回り角を決める
      const RC::Vector3 d = ToShark();
      turnDiff_ = PartPose::WrapAngle(std::atan2(d.x, d.z) - yaw_);
      if (std::fabs(turnDiff_) < 0.01f) turnDiff_ = PartPose::kPi;
    }
    octYaw_ = yaw_ + turnDiff_ * e;
    const RC::Vector3 d = ToShark();
    const float pitchToShark = std::asin(std::clamp(d.y, -1.0f, 1.0f));
    const float chasePitch = std::asin(std::clamp(travelDir_.y, -1.0f, 1.0f)) * 0.7f;
    octPitchUp_ = chasePitch + (pitchToShark - chasePitch) * e;
    splay_ = 0.4f + 0.85f * e; // 威嚇：傘のように足を広げる
    lookSide_ = 1.0f - e;

    const float gap = p_.sharkEndGap + (p_.sharkLungeGap - p_.sharkEndGap) * Smooth01(u);
    sharkPos_ = Sub(octPos_, Scale(travelDir_, gap));

    if (u >= 1.0f) {
      sharkHold_ = sharkPos_;
      octHold_ = octPos_;
      Enter(Phase::Shoot);
    }
  }

  /// @brief 水の弾を撃つ：shotInterval おきに発射。全部当たったら倒れ、killHold 後に Merge へ
  void UpdateShoot(float dt) {
    (void)dt;
    octPos_ = Sub(octHold_, Scale(ToShark(), 0.12f * (std::max)(recoil_, 0.0f)));
    const RC::Vector3 d = ToShark();
    octYaw_ = yaw_ + turnDiff_;
    octPitchUp_ = std::asin(std::clamp(d.y, -1.0f, 1.0f));
    splay_ = 1.25f;

    if (shotsFired_ < p_.shotCount && t_ >= static_cast<float>(shotsFired_) * p_.shotInterval) {
      FireShot(shotsFired_);
      ++shotsFired_;
    }

    const float timeout = static_cast<float>(p_.shotCount) * p_.shotInterval + p_.shotTravel + 1.0f;
    if (sharkAlive_ && t_ > timeout) Kill(); // 念のため（弾が作れなかった等）
    if (!sharkAlive_ && killTime_ >= 0.0f && t_ >= killTime_ + p_.killHold) {
      mergeFromYaw_ = octYaw_;
      mergeFromPitch_ = octPitchUp_;
      mergeFromPos_ = octPos_;
      mergeCamFrom_ = camPos_;
      mergeRotFrom_ = camRot_;
      Enter(Phase::Merge);
    }
  }

  /// @brief 前へ向き直り、カメラがタコの頭へ寄る
  void UpdateMerge() {
    const float u = std::clamp(t_ / (std::max)(p_.mergeDuration, 1e-3f), 0.0f, 1.0f);
    const float e = Smooth01(u);
    octPos_ = Add(mergeFromPos_, {0.0f, 0.6f * e, 0.0f});
    // 振り向いたのと同じ回転方向へ回りきって前を向く（逆回しにしない）
    const float sign = (turnDiff_ >= 0.0f) ? 1.0f : -1.0f;
    const float target = yaw_ + sign * PartPose::kTwoPi;
    octYaw_ = mergeFromYaw_ + (target - mergeFromYaw_) * e;
    octPitchUp_ = mergeFromPitch_ + (-endRot_.x - mergeFromPitch_) * e;
    splay_ = 1.25f + (0.35f - 1.25f) * e;
    // Done へはカメラを最終位置へ書いたあと（Update の最後）で移る
    if (u >= 1.0f) mergeDone_ = true;
  }

  void Enter(Phase p) {
    phase_ = p;
    t_ = 0.0f;
    if (p == Phase::Turn) turnDiff_ = 0.0f;
    if (p == Phase::Merge) mergeDone_ = false;
  }

  // ------------------------------------------------------------------
  // 水の弾
  // ------------------------------------------------------------------

  /// @brief サメの狙う点（頭のあたり）
  RC::Vector3 SharkAim() const {
    const RC::Vector3 fwd = {std::sin(sharkYaw_) * std::cos(sharkPitchUp_), std::sin(sharkPitchUp_),
                             std::cos(sharkYaw_) * std::cos(sharkPitchUp_)};
    return Add(Add(sharkPos_, Scale(fwd, p_.sharkAimForward)), {0.0f, 0.3f, 0.0f});
  }

  /// @brief タコの口（顔の前）
  RC::Vector3 OctMouth() const {
    const Mat3 body = PartPose::BodyMatrix(octYaw_, octPitchUp_, 0.0f);
    return Add(octPos_, PartPose::Apply({0.0f, 0.05f * p_.octopusScale, 0.5f * p_.octopusScale}, body));
  }

  void FireShot(int i) {
    recoil_ = -0.45f; // 撃った反動で一瞬つぶれる
    recoilVel_ = 0.0f;
    shake_ = (std::max)(shake_, 0.05f);
    PlaySe(seShot_);
    if (i < 0 || i >= static_cast<int>(shots_.size())) {
      OnShotHit(); // 弾の実体がまだ無い：当たったことにする
      return;
    }
    Shot &s = shots_[i];
    s.active = true;
    s.t = 0.0f;
    s.from = OctMouth();
    SetActive(s.e, true);
  }

  void UpdateShots(float dt) {
    using namespace PartPose;
    for (auto &s : shots_) {
      if (!s.active) continue;
      s.t += dt;
      const float k = std::clamp(s.t / (std::max)(p_.shotTravel, 1e-3f), 0.0f, 1.0f);
      RC::Vector3 pos = LerpV(s.from, SharkAim(), k);
      pos.y += 0.35f * std::sin(kPi * k); // 少し山なり
      const float r = p_.shotSize;
      Place(s.e, pos, Identity(), {r, r, r});
      if (k >= 1.0f) {
        s.active = false;
        SetActive(s.e, false);
        OnShotHit();
      }
    }
  }

  /// @brief 水の弾がサメに当たった：赤く光らせ、のけぞらせ、しぶきを散らす。最後の 1 発で倒す
  void OnShotHit() {
    ++hitsLanded_;
    const RC::Vector3 aim = SharkAim();
    SpawnWaterBurst(aim, 12, 4, 1.0f);
    if (!sharkAlive_) return;
    flashTimer_ = 0.1f;
    knockVel_ = Add(knockVel_, Scale(Norm(Sub(sharkPos_, octPos_), F_), 5.0f));
    shake_ = (std::max)(shake_, 0.14f);
    PlaySe(seHit_);
    if (hitsLanded_ >= p_.shotCount) Kill();
  }

  void Kill() {
    if (!sharkAlive_) return;
    sharkAlive_ = false;
    sharkDeadT_ = 0.0f;
    sinkVel_ = 0.0f;
    killTime_ = t_;
    flashTimer_ = 0.0f;
    const RC::Vector3 aim = SharkAim();
    SpawnWaterBurst(aim, 24, 10, 1.8f);
    SpawnWaterBurst(Add(aim, {0.8f, 0.5f, 0.0f}), 12, 4, 1.4f);
    SpawnBubbleBurst(sharkPos_);
    shake_ = (std::max)(shake_, 0.3f);
    PlaySe(seKill_);
    Log::Print("[SharkChaseCutscene] shark down");
  }

  /// @brief 着弾のしぶきと泡（WaterBullet::SpawnSplash の通常弾と同じエフェクト粒。水面の波は出さない）
  /// @param pos     着弾位置
  /// @param splashes しぶきの粒数
  /// @param bubbles  泡の粒数
  /// @param size     粒の大きさの倍率
  static void SpawnWaterBurst(const RC::Vector3 &pos, int splashes, int bubbles, float size) {
    for (int i = 0; i < splashes; ++i) {
      RC::EffectParticleSpawn splash;
      splash.kind = RC::EffectParticleKind::Splash;
      splash.position = pos;
      splash.scale = (0.15f + static_cast<float>(i % 4) * 0.05f) * size;
      const float r = 0.3f + static_cast<float>(i % 3) * 0.15f;
      const float g = 0.6f + static_cast<float>(i % 2) * 0.2f;
      splash.color = {r, g, 1.0f, 0.85f};
      splash.impactFactor = size;
      RC::SpawnEffectParticle(splash);
    }
    for (int i = 0; i < bubbles; ++i) {
      RC::EffectParticleSpawn bubble;
      bubble.kind = RC::EffectParticleKind::Bubble;
      bubble.position = pos;
      bubble.scale = (0.05f + static_cast<float>(i % 3) * 0.05f) * size;
      bubble.color = {0.8f, 0.9f, 1.0f, 0.6f};
      RC::SpawnEffectParticle(bubble);
    }
  }

  // ------------------------------------------------------------------
  // サメ
  // ------------------------------------------------------------------

  void UpdateShark(float dt) {
    if (sharkAlive_) {
      // のけぞり（ばねで戻る）
      knockVel_ = Add(knockVel_, Scale(knock_, -90.0f * dt));
      knockVel_ = Scale(knockVel_, std::exp(-9.0f * dt));
      knock_ = Add(knock_, Scale(knockVel_, dt));
      if (phase_ == Phase::Shoot) {
        sharkPos_ = Add(sharkHold_, knock_);
        sharkPos_.y += 0.06f * std::sin(time_ * 47.0f) * (flashTimer_ > 0.0f ? 1.0f : 0.0f);
      }
      // タコの方を向き、尾を振る
      const RC::Vector3 d = Norm(Sub(octPos_, sharkPos_), travelDir_);
      const float wag = (phase_ == Phase::Shoot ? 0.05f : 0.12f) * std::sin(time_ * PartPose::kTwoPi * 2.4f);
      sharkYaw_ = std::atan2(d.x, d.z) + wag;
      sharkPitchUp_ = std::asin(std::clamp(d.y, -1.0f, 1.0f));
      sharkRoll_ = 0.0f;
      if (flashTimer_ > 0.0f) flashTimer_ -= dt;
    } else {
      // 撃破：腹を見せて裏返り、頭を下げながら沈む
      sharkDeadT_ += dt;
      sinkVel_ += 3.0f * dt;
      sharkPos_.y -= sinkVel_ * dt;
      sharkPos_ = Add(sharkPos_, Scale(Norm(Sub(sharkPos_, octPos_), F_), 0.6f * dt));
      sharkRoll_ = PartPose::kPi * Smooth01(sharkDeadT_ / 0.9f);
      sharkPitchUp_ += (-0.5f - sharkPitchUp_) * (std::min)(1.0f, dt * 1.5f);
      if (sharkDeadT_ > 3.5f) {
        if (auto e = sharkEntity_.lock()) e->Destroy();
        sharkEntity_.reset();
      }
    }
  }

  void PoseShark() {
    auto e = sharkEntity_.lock();
    if (!e) return;
    using namespace PartPose;
    if (auto *tr = e->GetComponent<TransformComponent>()) {
      tr->position = sharkPos_;
      const Mat3 m = MatMul(RotY(p_.sharkModelYawDeg * kDeg), BodyMatrix(sharkYaw_, sharkPitchUp_, sharkRoll_));
      tr->rotation = ToEngineEuler(m);
      tr->scale = {p_.sharkScale, p_.sharkScale, p_.sharkScale};
    }
    if (auto *mr = e->GetComponent<ModelRendererComponent>()) {
      if (flashTimer_ > 0.0f) {
        mr->color = {1.0f, 0.2f, 0.2f, 1.0f}; // 被弾の赤（敵と同じ）
      } else if (!sharkAlive_) {
        const float k = 1.0f - 0.3f * Smooth01(sharkDeadT_ / 0.6f); // 力尽きて少し沈んだ色に
        mr->color = {k, k, k, 1.0f};
      } else {
        mr->color = {1.0f, 1.0f, 1.0f, 1.0f};
      }
    }
  }

  // ------------------------------------------------------------------
  // タコ
  // ------------------------------------------------------------------

  void UpdateOctopusSpring(float dt) {
    recoilVel_ += (-120.0f * recoil_ - 10.0f * recoilVel_) * dt;
    recoil_ += recoilVel_ * dt;
  }

  /// @brief 胴・顔・目・足の姿勢を書く
  void PoseOctopus() {
    using namespace PartPose;
    const bool show = octVisible_ && phase_ != Phase::Done &&
                      !(phase_ == Phase::Merge && t_ / (std::max)(p_.mergeDuration, 1e-3f) > 0.72f);
    for (auto &w : octParts_) SetActive(w, show);
    if (!show) return;

    const float s = p_.octopusScale;
    const float sq = std::clamp(recoil_ + 0.04f * std::sin(time_ * 9.0f), -0.6f, 0.8f);
    const RC::Vector3 stretch = {1.0f - 0.35f * sq, 1.0f + 0.5f * sq, 1.0f - 0.35f * sq};
    const Mat3 body = BodyMatrix(octYaw_, octPitchUp_, 0.0f);
    auto at = [&](const RC::Vector3 &l) {
      return AddV(octPos_, Apply({l.x * s * stretch.x, l.y * s * stretch.y, l.z * s * stretch.z}, body));
    };
    auto sz = [&](float x, float y, float z) { return RC::Vector3{x * s * stretch.x, y * s * stretch.y, z * s * stretch.z}; };
    auto part = [&](int i) -> const std::weak_ptr<Entity> & {
      static const std::weak_ptr<Entity> kNone;
      return (i < static_cast<int>(octParts_.size())) ? octParts_[i] : kNone;
    };

    // 胴（後ろへ持ち上がった楕円球）・顔
    Place(part(kMantle), at({0.0f, 0.32f, -0.22f}), MatMul(RotX(0.55f), body), sz(0.46f, 0.40f, 0.58f));
    Place(part(kFace), at({0.0f, 0.14f, 0.12f}), body, sz(0.42f, 0.36f, 0.40f));
    // 目と黒目（顔の前面。lookSide_ で横目になる）
    const float px = 0.045f * lookSide_;
    for (int side = 0; side < 2; ++side) {
      const float sx = (side == 0) ? -0.17f : 0.17f;
      Place(part(side == 0 ? kEyeL : kEyeR), at({sx, 0.36f, 0.27f}), body, sz(0.12f, 0.11f, 0.12f));
      Place(part(side == 0 ? kPupilL : kPupilR), at({sx + (side == 0 ? -px : px), 0.37f, 0.37f - 0.03f * lookSide_}),
            body, sz(0.065f, 0.065f, 0.05f));
    }

    // 足：付け根の輪から後ろ下（A）へたなびく。splay_ で開き具合、先へ行くほどうねる
    const RC::Vector3 A = {0.0f, -0.330f, -0.944f};
    const RC::Vector3 U1 = {1.0f, 0.0f, 0.0f};
    const RC::Vector3 U2 = {0.0f, 0.944f, -0.330f};
    const RC::Vector3 center = {0.0f, 0.02f, -0.08f};
    const float spacing = 0.21f;
    for (int t = 0; t < kTentacles; ++t) {
      const float phi = (static_cast<float>(t) + 0.5f) * (kTwoPi / static_cast<float>(kTentacles));
      const RC::Vector3 radial = AddV(ScaleV(U1, std::cos(phi)), ScaleV(U2, std::sin(phi)));
      RC::Vector3 p = AddV(center, ScaleV(radial, 0.2f));
      for (int k = 0; k < kSegments; ++k) {
        const float kk = static_cast<float>(k) / static_cast<float>(kSegments - 1);
        const float wave = 0.35f * std::sin(time_ * 7.0f + static_cast<float>(t) * 0.9f - static_cast<float>(k) * 0.8f);
        const float th = splay_ + wave * (0.3f + kk);
        const RC::Vector3 dir = AddV(ScaleV(A, std::cos(th)), ScaleV(radial, std::sin(th)));
        if (k > 0) p = AddV(p, ScaleV(dir, spacing));
        const float r = 0.14f - 0.022f * static_cast<float>(k);
        const RC::Vector3 world = AddV(octPos_, Apply({p.x * s, p.y * s, p.z * s}, body));
        Place(part(kTentacleStart + t * kSegments + k), world, body, {r * s, r * s, r * s * 1.25f});
      }
    }
  }

  // ------------------------------------------------------------------
  // カメラ
  // ------------------------------------------------------------------

  /// @brief 今の段階で狙いたいカメラの位置と注視点
  void DesiredCamera(RC::Vector3 &pos, RC::Vector3 &look) const {
    const RC::Vector3 up = {0.0f, 1.0f, 0.0f};
    if (phase_ == Phase::Chase) {
      // 横から 2 匹を収める（タコ寄りの点を見る）
      const RC::Vector3 mid = LerpV(octPos_, sharkPos_, 0.35f);
      pos = Add(Add(Add(mid, Scale(R_, p_.camSide)), Scale(up, p_.camHeight)), Scale(F_, p_.camLead));
      look = mid;
    } else {
      // 肩越し：タコの後ろ（サメと反対側）から、サメを見る。
      // 下がる向きは水平だけにする（経路が斜め上なので、3D のまま下がると真上から見下ろす角度になる）
      const RC::Vector3 d = ToShark();
      const RC::Vector3 dH = Norm({d.x, 0.0f, d.z}, Scale(F_, -1.0f));
      pos = Add(Add(Add(octPos_, Scale(dH, -p_.camBack)), Scale(up, 0.7f)), Scale(R_, 1.3f));
      look = LerpV(octPos_, sharkAlive_ ? sharkPos_ : sharkHold_, 0.65f);
    }
  }

  void UpdateCamera(float dt, TransformComponent &cam) {
    using namespace PartPose;
    shake_ *= std::exp(-7.0f * dt);
    const float handheld = (phase_ == Phase::Chase) ? 0.035f : 0.0f;
    const float amp = shake_ + handheld;
    const RC::Vector3 shake = Add(Scale(R_, amp * std::sin(time_ * 53.0f)),
                                  {0.0f, amp * std::sin(time_ * 61.0f + 1.3f), 0.0f});

    if (phase_ == Phase::Merge) {
      // タコの頭へ寄りながら、浮上開始の向きへ回す
      const float u = std::clamp(t_ / (std::max)(p_.mergeDuration, 1e-3f), 0.0f, 1.0f);
      const float e = Smooth01(u);
      const RC::Vector3 head = Add(octPos_, {0.0f, 0.3f * p_.octopusScale, 0.0f});
      const RC::Vector3 pos = LerpV(mergeCamFrom_, head, e);
      const RC::Vector3 rot = {mergeRotFrom_.x + (endRot_.x - mergeRotFrom_.x) * e,
                               LerpAngle(mergeRotFrom_.y, endRot_.y, e),
                               mergeRotFrom_.z + (endRot_.z - mergeRotFrom_.z) * e};
      camPos_ = pos;
      camRot_ = rot;
      WriteCamera(cam, Add(pos, Scale(shake, 1.0f - e)), rot);
      return;
    }

    RC::Vector3 want, wantLook;
    DesiredCamera(want, wantLook);
    const float follow = p_.camFollow * (phase_ == Phase::Turn ? 1.4f : 1.0f);
    const float a = 1.0f - std::exp(-follow * dt);
    camPos_ = LerpV(camPos_, want, a);
    camLook_ = LerpV(camLook_, wantLook, (std::min)(1.0f, a * 1.6f));
    const float roll = (phase_ == Phase::Chase) ? 0.05f * std::sin(time_ * 2.1f) : 0.0f;
    camRot_ = LookRot(camPos_, camLook_, roll);
    WriteCamera(cam, Add(camPos_, shake), camRot_);
  }

  // ------------------------------------------------------------------
  // 状態
  // ------------------------------------------------------------------
  Scene *scene_ = nullptr;
  SceneContext *ctx_ = nullptr;
  SharkChaseParams p_;
  Phase phase_ = Phase::Idle;
  float t_ = 0.0f;    ///< 今の段階に入ってからの秒数
  float time_ = 0.0f; ///< 開始からの秒数（揺れ用）

  // ゴールの向きと経路
  float yaw_ = 0.0f;
  RC::Vector3 F_{0, 0, 1}, R_{1, 0, 0};
  RC::Vector3 endRot_{};
  RC::Vector3 start_{}, end_{};
  RC::Vector3 travelDir_{0, 0, 1};
  float speed_ = 0.0f;

  // タコ
  std::vector<std::weak_ptr<Entity>> octParts_;
  std::weak_ptr<Entity> folder_;
  int built_ = 0;
  RC::Vector3 octPos_{};
  RC::Vector3 octHold_{};
  float octYaw_ = 0.0f;
  float octPitchUp_ = 0.0f;
  float splay_ = 0.3f;
  float recoil_ = 0.0f;
  float recoilVel_ = 0.0f;
  float lookSide_ = 0.0f;
  float turnDiff_ = 0.0f;
  bool octVisible_ = true;

  // サメ
  std::weak_ptr<Entity> sharkEntity_;
  RC::Vector3 sharkPos_{};
  RC::Vector3 sharkHold_{};
  float sharkYaw_ = 0.0f;
  float sharkPitchUp_ = 0.0f;
  float sharkRoll_ = 0.0f;
  bool sharkAlive_ = true;
  float sharkDeadT_ = 0.0f;
  float sinkVel_ = 0.0f;
  float flashTimer_ = 0.0f;
  RC::Vector3 knock_{}, knockVel_{};

  // 水の弾
  std::vector<Shot> shots_;
  int shotsFired_ = 0;
  int hitsLanded_ = 0;
  float killTime_ = -1.0f;

  // Merge の始点
  bool mergeDone_ = false;
  float mergeFromYaw_ = 0.0f;
  float mergeFromPitch_ = 0.0f;
  RC::Vector3 mergeFromPos_{};
  RC::Vector3 mergeCamFrom_{};
  RC::Vector3 mergeRotFrom_{};

  // カメラ
  RC::Vector3 camPos_{}, camLook_{}, camRot_{};
  float shake_ = 0.0f;

  // 効果音
  int seShot_ = -1, seHit_ = -1, seKill_ = -1;
};
