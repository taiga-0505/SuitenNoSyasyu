#pragma once
#include "ECS/TransformComponent.h"
#include "ECS/PrimitiveMeshComponent.h"
#include "ECS/ModelRendererComponent.h"
#include "ECS/AnimationComponent.h"
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
#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

// ============================================================================
// ステージ開始の「サメに追われる → 振り向いて水の弾で倒す」三人称カットシーン
//
//   DeepRiseIntroScript が浮上の前に再生する（"sharkChase": { "enabled": true } のとき）。
//   深海から斜め上へ逃げるタコ（タイトルと同じ octopus.glb。足・目は骨を毎フレーム上書きして動かす。
//   octopusModel を空にすると以前のプリミティブの組み合わせに戻る）を、
//   後ろからサメ（shark_game.glb）が追う。2 秒ほど追われたら振り向き、水の弾（プレイヤーの弾と
//   同じ水の質感）を撃って倒し、
//   カメラがタコの頭へ寄って一人称に戻る。その位置から DeepRiseIntroScript の浮上が続く。
//
//   流れ（秒は既定値）:
//     Chase 2.0s : タコが startOffset → endOffset をジェット推進で逃げる（本物のタコの泳ぎ方）。
//                  外套膜を先に、目は後ろのサメを見て、足は後ろへ束ねる。jetRate 回/秒で水を噴き、
//                  噴くたびに足を閉じて加速 → 惰性で減速しながら足をゆるく開く。サメは一定の速さで追う。
//                  カメラは横から 2 匹を収める（深いうちはフォグでサメが影のように見える）。
//     Turn  0.45s: タコが泳ぎを止めて体を起こし、サメへ向いて足を体の下へスカート状に開く（威嚇）。
//                  サメは噛みつきに来る。
//                  カメラはタコの肩越しと 2 匹の真横引きの中間（camShootBlend）へ回り込み、
//                  タコを大きめに映しつつ間を弾が飛ぶのが見える構図にする。
//     Shoot      : shotCount 発の水の弾を shotInterval おきに撃つ。サメは突進してくるが、
//                  弾（泡の尾を引く）が当たるたびに赤く光ってのけぞり、水しぶきと泡が散る
//                  （WaterBullet の着弾と同じエフェクト粒）。最後の 1 発で撃破（大きなしぶき・泡、腹を見せて沈む）。
//                  2 匹の間を十分あけ（sharkLungeGap）、弾の飛ぶ時間も長め（shotTravel）にして、
//                  「撃った弾が届いて倒した」と分かるようにしている。
//     Merge 1.0s : タコが前へ向き直り、カメラがタコの頭の位置・浮上開始の向きへ寄って一人称へ。
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
  float shotInterval = 0.3f;   ///< 水の弾の発射間隔（1 発ずつ見分けられる間隔）
  float shotTravel = 0.4f;     ///< 水の弾がサメに届くまで（飛んでいく様子が見える長さ）
  float killHold = 0.6f;       ///< 倒してから一人称へ寄り始めるまで（裏返って沈むのを見せる）
  float mergeDuration = 1.0f;  ///< カメラがタコの頭へ寄る時間（引いた位置から戻るぶん長め）

  // ---- 経路（ゴール基準。x 右 / y 上 / z 前）----
  RC::Vector3 startOffset = {0.0f, -25.0f, -24.0f}; ///< 逃げ始める位置
  RC::Vector3 endOffset = {0.0f, -16.0f, -8.0f};    ///< 振り向き始める位置

  // ---- サメ ----
  std::string sharkModel = "Resources/model/shark/shark_game.glb";
  float sharkScale = 6.0f;         ///< WaveSpawner の enemyScale と同じ
  float sharkModelYawDeg = -90.0f; ///< モデルの頭は +X なので -90° で +Z に合わせる
  float sharkStartGap = 14.0f;     ///< 追い始めのタコとの距離（m）
  float sharkEndGap = 12.0f;       ///< 追い終わりの距離
  float sharkLungeGap = 11.0f;     ///< 振り向き終わり（撃ち始め）の距離。弾の飛ぶ距離になる
  float sharkChargeSpeed = 2.5f;   ///< 撃たれているあいだにサメが突進してくる速さ（m/s。当たるたび鈍る）
  float sharkMinGap = 4.0f;        ///< 突進してもこれ以上は近づかない距離
  float sharkAimForward = 1.2f;    ///< 弾の狙い（モデル原点から頭側へ何 m）

  // ---- タコ ----
  float octopusScale = 0.9f;
  float jetRate = 2.0f;     ///< 逃げるときに水を噴く回数（回/秒。追われている時間に収まる整数回へ丸める）
  float jetSurge = 0.65f;   ///< 噴いたときの加速の強さ（0 = 等速、1 未満。速さは平均の 1±jetSurge 倍で揺れる）
  float threatSplay = 0.85f; ///< 構えて撃つときの足の開き（rad。体の下へスカート状に開く。大きいと円盤になって横から潰れて見える）
  /// @brief タコのモデル（空ならプリミティブの組み合わせ）。骨の名前は TitleOctopusScript と同じ
  std::string octopusModel = "Resources/model/octopus/octopus.glb";
  RC::Vector4 bodyColor = {0.93f, 0.40f, 0.33f, 1.0f}; ///< プリミティブのときだけ使う
  RC::Vector4 shotColor = {0.2f, 0.6f, 1.0f, 0.85f}; ///< 水の弾の色（RailShooterController の自機弾と同じ）
  float shotSize = 0.45f;  ///< 水の弾の半径（引いた構図でも見えるよう通常弾 0.3 より大きめ）
  int shotTrailBubbles = 2; ///< 水の弾が 1 フレームに残す泡の数（弾の通り道を見せる。0 で無し）

  // ---- カメラ ----
  float camSide = 10.0f;   ///< 追跡中：横へ離れる距離（2 匹の間が開いたぶん広めに引く）
  float camHeight = 1.4f;  ///< 追跡中：上へずらす量
  float camLead = 2.0f;    ///< 追跡中：前へずらす量
  float camBack = 3.2f;        ///< 振り向き〜撃破（肩越し）：タコの後ろへ離れる距離
  float camShootDist = 20.0f;  ///< 振り向き〜撃破（真横引き）：2 匹の間の点から真横へ離れる距離
  float camShootHeight = 1.5f; ///< 振り向き〜撃破（真横引き）：上へずらす量
  float camShootFocus = 0.5f;  ///< 振り向き〜撃破（真横引き）：注視点（0 = タコ、1 = サメ）
  float camShootBlend = 0.5f;  ///< 振り向き〜撃破の構図（0 = 肩越し、1 = 真横引き。間は位置と注視点を補間）
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
        {"sharkChargeSpeed", sharkChargeSpeed},
        {"sharkMinGap", sharkMinGap},
        {"sharkAimForward", sharkAimForward},
        {"octopusScale", octopusScale},
        {"jetRate", jetRate},
        {"jetSurge", jetSurge},
        {"threatSplay", threatSplay},
        {"octopusModel", octopusModel},
        {"bodyColor", v4(bodyColor)},
        {"shotColor", v4(shotColor)},
        {"shotSize", shotSize},
        {"shotTrailBubbles", shotTrailBubbles},
        {"camSide", camSide},
        {"camHeight", camHeight},
        {"camLead", camLead},
        {"camBack", camBack},
        {"camShootDist", camShootDist},
        {"camShootHeight", camShootHeight},
        {"camShootFocus", camShootFocus},
        {"camShootBlend", camShootBlend},
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
    readF("sharkChargeSpeed", sharkChargeSpeed);
    readF("sharkMinGap", sharkMinGap);
    readF("sharkAimForward", sharkAimForward);
    readF("octopusScale", octopusScale);
    readF("jetRate", jetRate);
    readF("jetSurge", jetSurge);
    readF("threatSplay", threatSplay);
    readS("octopusModel", octopusModel);
    readV4("bodyColor", bodyColor);
    readV4("shotColor", shotColor);
    readF("shotSize", shotSize);
    readI("shotTrailBubbles", shotTrailBubbles);
    readF("camSide", camSide);
    readF("camHeight", camHeight);
    readF("camLead", camLead);
    readF("camBack", camBack);
    readF("camShootDist", camShootDist);
    readF("camShootHeight", camShootHeight);
    readF("camShootFocus", camShootFocus);
    readF("camShootBlend", camShootBlend);
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
    ImGui::DragFloat("Charge Speed", &sharkChargeSpeed, 0.05f, 0.0f, 20.0f);
    ImGui::DragFloat("Min Gap", &sharkMinGap, 0.1f, 0.5f, 40.0f);
    ImGui::DragFloat("Aim Forward", &sharkAimForward, 0.05f, -5.0f, 5.0f);
    ImGui::SeparatorText("Octopus");
    ImGui::DragFloat("Octopus Scale", &octopusScale, 0.01f, 0.2f, 4.0f);
    ImGui::DragFloat("Jet Rate (/s)", &jetRate, 0.05f, 0.2f, 6.0f);
    ImGui::SliderFloat("Jet Surge", &jetSurge, 0.0f, 0.95f);
    ImGui::DragFloat("Threat Splay (rad)", &threatSplay, 0.01f, 0.1f, 1.6f);
    ImGui::TextDisabled("Model: %s", octopusModel.empty() ? "(primitives)" : octopusModel.c_str());
    ImGui::ColorEdit4("Body (primitives)", &bodyColor.x);
    ImGui::ColorEdit4("Shot Color", &shotColor.x);
    ImGui::DragFloat("Shot Size", &shotSize, 0.01f, 0.05f, 2.0f);
    ImGui::SliderInt("Shot Trail Bubbles", &shotTrailBubbles, 0, 6);
    ImGui::SeparatorText("Camera");
    ImGui::DragFloat("Side", &camSide, 0.05f, 0.0f, 30.0f);
    ImGui::DragFloat("Height", &camHeight, 0.05f, -10.0f, 10.0f);
    ImGui::DragFloat("Lead", &camLead, 0.05f, -10.0f, 10.0f);
    ImGui::SliderFloat("Shoot Blend (0=shoulder 1=side)", &camShootBlend, 0.0f, 1.0f);
    ImGui::DragFloat("Back (shoulder)", &camBack, 0.05f, 0.5f, 20.0f);
    ImGui::DragFloat("Shoot Dist (side)", &camShootDist, 0.1f, 1.0f, 60.0f);
    ImGui::DragFloat("Shoot Height (side)", &camShootHeight, 0.05f, -10.0f, 10.0f);
    ImGui::SliderFloat("Shoot Focus (side)", &camShootFocus, 0.0f, 1.0f);
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
    octYaw_ = yaw_ + PartPose::kPi; // 外套膜を先に泳ぐ（顔は後ろ）
    octPitchUp_ = kSwimPitch;
    splay_ = 0.06f;
    legAxisTarget_ = WorldToBody(Scale(travelDir_, -1.0f));
    legAxis_ = legAxisTarget_;
    waveAmt_ = 0.12f;
    jetPulse_ = 0.0f;
    squashJet_ = 0.0f;
    lastJet_ = 0;
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

    // 足の向きの基準軸は段階の切り替わりでなめらかに追わせる
    {
      const float k = 1.0f - std::exp(-12.0f * dt);
      legAxis_ = Norm(LerpV(legAxis_, legAxisTarget_, k), legAxisTarget_);
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
    kill(octModel_);
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
    const bool useModel = !p_.octopusModel.empty();
    if (useModel) {
      // モデルで描くときは部位のエンティティは作らない（添字だけそろえて、あとは水の弾を作る）
      if (octModel_.expired()) octModel_ = MakeOctopusModel();
      while (built_ < kOctParts) {
        octParts_.emplace_back();
        ++built_;
      }
    }
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

  /// @brief タコのモデルを作る（非表示・画面外で作る）
  /// @details 骨の姿勢は PoseOctopus が毎フレーム上書きする。上書きはアニメーションの更新の中で反映されるので、
  ///          待機のクリップを 1 本再生しておく（全部の骨を上書きするので見た目には出ない）。
  std::shared_ptr<Entity> MakeOctopusModel() {
    if (!scene_) return nullptr;
    auto e = scene_->CreateEntity("SharkChase_Octopus");
    if (!e) return nullptr;
    if (auto f = folder_.lock()) e->SetParentGuid(f->Guid());
    e->SetTag("transient", 1);
    auto &tr = e->AddComponent<TransformComponent>();
    tr.position = {0.0f, -500.0f, 0.0f};
    tr.scale = {p_.octopusScale, p_.octopusScale, p_.octopusScale};
    auto &mr = e->AddComponent<ModelRendererComponent>();
    mr.modelPath = p_.octopusModel;
    auto &anim = e->AddComponent<AnimationComponent>();
    anim.clips = {{"Idle", "", 0, true, 1.0f}};
    anim.defaultClip = "Idle";
    scene_->InitDynamicEntityRuntime(*e);
    e->SetActive(false);
    return e;
  }

  /// @brief 単位ベクトル a を b へ回す最短の回転（クォータニオン）
  static RC::Quaternion ShortestArc(const RC::Vector3 &a, const RC::Vector3 &b) {
    const float d = a.x * b.x + a.y * b.y + a.z * b.z;
    if (d < -0.9999f) {
      // 真逆：a に直交する軸で 180°
      RC::Vector3 ax = {0.0f, -a.z, a.y};
      if (Len(ax) < 1e-3f) ax = {a.z, 0.0f, -a.x};
      ax = Norm(ax, {0.0f, 1.0f, 0.0f});
      return {ax.x, ax.y, ax.z, 0.0f};
    }
    RC::Quaternion q = {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x, 1.0f + d};
    const float l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return {q.x / l, q.y / l, q.z / l, q.w / l};
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

  /// @brief 足のなびく向きの既定（胴のローカル。後ろ下）。付け根の輪はこの軸に直交する輪に置く
  static RC::Vector3 LegAxisRest() { return {0.0f, -0.330f, -0.944f}; }

  /// @brief ワールドの向きを今のタコの胴のローカルへ写す（単位ベクトルにして返す）
  RC::Vector3 WorldToBody(const RC::Vector3 &w) const {
    const Mat3 body = PartPose::BodyMatrix(octYaw_, octPitchUp_, 0.0f);
    return Norm(PartPose::Apply(w, PartPose::Transpose(body)), LegAxisRest());
  }

  /// @brief 軸 axis に直交する輪の上で、角度 phi の向き
  /// @details axis = LegAxisRest() のとき、元の式（U1 = +X, U2 = (0, 0.944, -0.330)）と一致する
  static RC::Vector3 LegRadial(const RC::Vector3 &axis, float phi) {
    const float ax = axis.x;
    const RC::Vector3 u1 = Norm(Sub({1.0f, 0.0f, 0.0f}, Scale(axis, ax)), {1.0f, 0.0f, 0.0f});
    const RC::Vector3 u2 = {u1.y * axis.z - u1.z * axis.y, u1.z * axis.x - u1.x * axis.z,
                            u1.x * axis.y - u1.y * axis.x}; // u1 × axis
    return Add(Scale(u1, std::cos(phi)), Scale(u2, std::sin(phi)));
  }

  /// @brief 足を開く向き：付け根の外向き（rootRadial）を軸 axis に直交する面へ落としたもの
  /// @details 軸を大きく傾けても、上の付け根の足は上へ・下の付け根の足は下へ開く（足どうしが交差しない）。
  ///          付け根の外向きが軸とほぼ平行なときだけ、輪の上の角度 phi の向きで代用する。
  static RC::Vector3 LegSpreadDir(const RC::Vector3 &axis, const RC::Vector3 &rootRadial, float phi) {
    const float d = rootRadial.x * axis.x + rootRadial.y * axis.y + rootRadial.z * axis.z;
    const RC::Vector3 proj = Sub(rootRadial, Scale(axis, d));
    const RC::Vector3 fallback = LegRadial(axis, phi);
    const float l = Len(proj);
    if (l < 0.1f) return fallback;
    const RC::Vector3 n = Scale(proj, 1.0f / l);
    if (l >= 0.25f) return n;
    // 0.1〜0.25 は代用の向きとなめらかにつなぐ
    return Norm(LerpV(fallback, n, (l - 0.1f) / 0.15f), n);
  }

  /// @brief 水を噴いた瞬間：漏斗のあたり（タコの後ろ）から泡を吐く
  void SpawnJetBubbles() {
    const RC::Vector3 back = Scale(travelDir_, -1.0f);
    const RC::Vector3 at = Add(octPos_, Scale(back, 0.35f * p_.octopusScale));
    for (int i = 0; i < 6; ++i) {
      RC::EffectParticleSpawn bubble;
      bubble.kind = RC::EffectParticleKind::Bubble;
      bubble.position = Add(at, Scale(back, 0.12f * static_cast<float>(i)));
      bubble.scale = (0.05f + 0.03f * static_cast<float>(i % 3)) * p_.octopusScale;
      bubble.color = {0.8f, 0.9f, 1.0f, 0.6f};
      RC::SpawnEffectParticle(bubble);
    }
  }

  /// @brief 泳ぐときの体の傾き（顔を上へ。外套膜が進む向きの側へ倒れて先に立つ）
  static constexpr float kSwimPitch = 0.45f;

  /// @brief 追われている：ジェット推進で逃げる（本物のタコの泳ぎ方。TitleOctopusScript の潜行と同じ考え方）
  /// @details ・外套膜を先に、顔（目）は後ろ＝追ってくるサメの方を向く。足は後ろへ束ねてなびかせる。
  ///          ・漏斗から水を噴く瞬間に外套膜が縮んで足をぎゅっと閉じ、ぐんと加速する。
  ///            そのあと惰性で減速しながら足をゆるく開き、外套膜をふくらませて水を吸う。これを繰り返す。
  ///          ・進み方 s(u) = u - a·sin(2πNu)/(2πN)（N 回噴く）。速さは 1 - a·cos(2πNu) で、
  ///            始まり・終わりは一番遅い（滑空）ところ、噴くのは u = (k + 0.5)/N。端点は直線のときと同じ。
  void UpdateChase() {
    using namespace PartPose;
    const float u = std::clamp(t_ / (std::max)(p_.chaseDuration, 1e-3f), 0.0f, 1.0f);
    const int N = (std::max)(1, static_cast<int>(std::lround(p_.chaseDuration * p_.jetRate)));
    const float a = std::clamp(p_.jetSurge, 0.0f, 0.95f);
    const float w = kTwoPi * static_cast<float>(N);
    const float s = u - a * std::sin(w * u) / w;
    jetPulse_ = 0.5f - 0.5f * std::cos(w * u); // 1 = 噴いている瞬間
    // 噴く瞬間に漏斗のあたりから泡を吐く
    const int jetIndex = static_cast<int>(std::floor(static_cast<float>(N) * u + 0.5f));
    if (jetIndex != lastJet_ && u > 0.0f && u < 1.0f) {
      lastJet_ = jetIndex;
      SpawnJetBubbles();
    }

    octPos_ = LerpV(start_, end_, s);
    // 外套膜を先に：顔は後ろ（サメの方）、体を少し起こして外套膜を進む向きへ
    octYaw_ = yaw_ + kPi;
    octPitchUp_ = kSwimPitch;
    // 足：進む向きの反対へ束ねる。噴く瞬間に閉じ、惰性のあいだにゆるく開く。うねりは控えめ（流線形）
    legAxisTarget_ = WorldToBody(Scale(travelDir_, -1.0f));
    splay_ = 0.06f + 0.34f * (1.0f - jetPulse_);
    waveAmt_ = 0.12f;
    squashJet_ = 0.35f * (jetPulse_ - 0.4f); // 噴く瞬間に外套膜が縮んで細長くなる
    lookSide_ = 0.0f;                        // 後ろのサメをまっすぐ見ている

    // サメは一定の速さで追う（タコだけが噴くたびに引き離し、惰性のあいだに詰められる）
    const float gap = p_.sharkStartGap + (p_.sharkEndGap - p_.sharkStartGap) * Smooth01(u);
    sharkPos_ = Sub(LerpV(start_, end_, u), Scale(travelDir_, gap));
    sharkPos_ = Add(sharkPos_, Scale(R_, 0.5f * std::sin(time_ * kTwoPi * 1.1f)));

    if (u >= 1.0f) {
      turnFromYaw_ = octYaw_;
      turnFromPitch_ = octPitchUp_;
      turnFromSplay_ = splay_;
      Enter(Phase::Turn);
    }
  }

  /// @brief 構える：泳ぎを止め、体を起こしてサメへ向け、足をスカートのように下へ広げる。サメは噛みつきに来る
  void UpdateTurn() {
    const float T = (std::max)(p_.turnDuration, 1e-3f);
    const float u = std::clamp(t_ / T, 0.0f, 1.0f);
    const float e = Smooth01(u);
    // 滑空の速さから直線的に 0 まで落とす（移動距離 = v0·T·(u - u²/2)）
    const float glide = speed_ * (1.0f - std::clamp(p_.jetSurge, 0.0f, 0.95f));
    octPos_ = Add(end_, Scale(travelDir_, glide * T * (u - 0.5f * u * u)));
    if (turnDiff_ == 0.0f) { // 入った最初のフレームで、サメの方の向きを決める（Merge の回る向きにも使う）
      const RC::Vector3 d = ToShark();
      turnDiff_ = PartPose::WrapAngle(std::atan2(d.x, d.z) - yaw_);
      if (std::fabs(turnDiff_) < 0.01f) turnDiff_ = PartPose::kPi;
    }
    const RC::Vector3 d = ToShark();
    const float pitchToShark = std::asin(std::clamp(d.y, -1.0f, 1.0f));
    octYaw_ = LerpAngle(turnFromYaw_, yaw_ + turnDiff_, e);
    octPitchUp_ = turnFromPitch_ + (pitchToShark - turnFromPitch_) * e;
    // 足：後ろへ束ねた形 → 体の下へスカートのように開く（威嚇）。円盤にすると横から見て潰れるので円錐にとどめる
    legAxisTarget_ = {0.0f, -1.0f, 0.0f};
    splay_ = turnFromSplay_ + (p_.threatSplay - turnFromSplay_) * e;
    waveAmt_ = 0.12f + (0.3f - 0.12f) * e;
    jetPulse_ *= (1.0f - e);
    squashJet_ *= (1.0f - e);
    lookSide_ = 0.0f;

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
    // 足は下へ開いたまま。撃った反動（recoil_ < 0）で一瞬すぼめる
    legAxisTarget_ = {0.0f, -1.0f, 0.0f};
    splay_ = (std::max)(0.2f, p_.threatSplay + 0.5f * (std::min)(recoil_, 0.0f));
    waveAmt_ = 0.3f;
    jetPulse_ = 0.0f;
    squashJet_ = 0.0f;

    if (shotsFired_ < p_.shotCount && t_ >= static_cast<float>(shotsFired_) * p_.shotInterval) {
      FireShot(shotsFired_);
      ++shotsFired_;
    }

    const float timeout = static_cast<float>(p_.shotCount) * p_.shotInterval + p_.shotTravel + 1.0f;
    if (sharkAlive_ && t_ > timeout) Kill(); // 念のため（弾が作れなかった等）
    if (!sharkAlive_ && killTime_ >= 0.0f && t_ >= killTime_ + p_.killHold) {
      mergeFromYaw_ = octYaw_;
      mergeFromPitch_ = octPitchUp_;
      mergeFromSplay_ = splay_;
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
    // 足は元の「後ろ下へなびく」形へ戻す（浮上の一人称へつなぐ）
    legAxisTarget_ = LegAxisRest();
    splay_ = mergeFromSplay_ + (0.35f - mergeFromSplay_) * e;
    waveAmt_ = 0.3f;
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
      // 通り道に泡を残す（引いた構図でも弾がどこからどこへ飛んだか分かるように）
      for (int b = 0; b < p_.shotTrailBubbles; ++b) {
        RC::EffectParticleSpawn bubble;
        bubble.kind = RC::EffectParticleKind::Bubble;
        bubble.position = pos;
        bubble.scale = r * (0.25f + 0.1f * static_cast<float>(b % 3));
        bubble.color = {0.8f, 0.92f, 1.0f, 0.7f};
        RC::SpawnEffectParticle(bubble);
      }
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
        // 突進：タコへ向かって進む（当たるたびに鈍る。sharkMinGap より近づかない）
        const RC::Vector3 toOct = Sub(octPos_, sharkHold_);
        const float dist = Len(toOct);
        if (dist > p_.sharkMinGap) {
          const float hitRatio = static_cast<float>(hitsLanded_) / static_cast<float>((std::max)(p_.shotCount, 1));
          const float step = (std::min)(p_.sharkChargeSpeed * (1.0f - 0.6f * hitRatio) * dt, dist - p_.sharkMinGap);
          sharkHold_ = Add(sharkHold_, Scale(toOct, step / dist));
        }
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
    if (auto m = octModel_.lock()) {
      SetActive(octModel_, show);
      if (show) PoseOctopusModel(*m);
      return;
    }
    for (auto &w : octParts_) SetActive(w, show);
    if (!show) return;

    const float s = p_.octopusScale;
    const float sq = std::clamp(recoil_ + squashJet_ + 0.04f * std::sin(time_ * 9.0f), -0.6f, 0.8f);
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

    // 足：付け根の輪から legAxis_ の向きへたなびく。splay_ で開き具合、先へ行くほどうねる（waveAmt_）
    //   付け根の輪は既定の軸（LegAxisRest）に直交する輪のまま。向きだけ legAxis_ 基準で決める
    const RC::Vector3 A = legAxis_;
    const RC::Vector3 center = {0.0f, 0.02f, -0.08f};
    const float spacing = 0.21f;
    for (int t = 0; t < kTentacles; ++t) {
      const float phi = (static_cast<float>(t) + 0.5f) * (kTwoPi / static_cast<float>(kTentacles));
      const RC::Vector3 rootRadial = LegRadial(LegAxisRest(), phi);
      const RC::Vector3 radial = LegSpreadDir(A, rootRadial, phi);
      RC::Vector3 p = AddV(center, ScaleV(rootRadial, 0.2f));
      for (int k = 0; k < kSegments; ++k) {
        const float kk = static_cast<float>(k) / static_cast<float>(kSegments - 1);
        const float wave = waveAmt_ * std::sin(time_ * 7.0f + static_cast<float>(t) * 0.9f - static_cast<float>(k) * 0.8f);
        const float th = splay_ + wave * (0.3f + kk);
        const RC::Vector3 dir = AddV(ScaleV(A, std::cos(th)), ScaleV(radial, std::sin(th)));
        if (k > 0) p = AddV(p, ScaleV(dir, spacing));
        const float r = 0.14f - 0.022f * static_cast<float>(k);
        const RC::Vector3 world = AddV(octPos_, Apply({p.x * s, p.y * s, p.z * s}, body));
        Place(part(kTentacleStart + t * kSegments + k), world, body, {r * s, r * s, r * s * 1.25f});
      }
    }
  }

  /// @brief モデルのタコの姿勢を書く（プリミティブ版の PoseOctopus と同じ形を骨で作る）
  /// @details octopus.glb の骨はすべて root 直下なので、骨の姿勢＝モデル空間（scale 1）の姿勢。
  ///          胴・顔・目は伸び縮み（stretch）を位置と scale に掛ける。足は 1 本 6 節（プリミティブは 5 節）。
  ///          rest の足 m は水平に +Z から (m + 0.5) × 45° の向きへ伸びているので、ねじれが少ないよう
  ///          「今の足の向き（水平成分）」に近い rest の足を割り当ててから、rest の向き → 今の向きへ回す。
  void PoseOctopusModel(Entity &m) {
    using namespace PartPose;
    auto *tr = m.GetComponent<TransformComponent>();
    auto *mr = m.GetComponent<ModelRendererComponent>();
    if (!tr || !mr || !mr->HasModel()) return;
    const float s = p_.octopusScale;
    const float sq = std::clamp(recoil_ + squashJet_ + 0.04f * std::sin(time_ * 9.0f), -0.6f, 0.8f);
    const RC::Vector3 stretch = {1.0f - 0.35f * sq, 1.0f + 0.5f * sq, 1.0f - 0.35f * sq};
    const Mat3 body = BodyMatrix(octYaw_, octPitchUp_, 0.0f);

    // モデルの置き場所（描画側へも即座に写す）
    tr->position = octPos_;
    tr->rotation = ToEngineEuler(body);
    tr->scale = {s, s, s};
    const int h = mr->modelHandle;
    if (auto *mt = RC::GetModelTransformPtr(h)) *mt = tr->ToTransform();

    const RC::Quaternion qI = {0.0f, 0.0f, 0.0f, 1.0f};
    auto put = [&](const char *joint, const RC::Vector3 &l, const RC::Vector3 &scl) {
      RC::SetModelJointOverride(h, joint, {l.x * stretch.x, l.y * stretch.y, l.z * stretch.z}, qI, scl);
    };
    // 胴（モデルの外套膜は後ろを持ち上げた形で作ってあるので回さない）・顔
    put("mantle", {0.0f, 0.32f, -0.22f}, stretch);
    put("head", {0.0f, 0.14f, 0.12f}, stretch);
    // 目と黒目（顔の前面。lookSide_ で横目になる）
    const float px = 0.045f * lookSide_;
    put("eye_L", {-0.17f, 0.36f, 0.27f}, stretch);
    put("eye_R", {0.17f, 0.36f, 0.27f}, stretch);
    put("pupil_L", {-0.17f - px, 0.37f, 0.37f - 0.03f * lookSide_}, stretch);
    put("pupil_R", {0.17f + px, 0.37f, 0.37f - 0.03f * lookSide_}, stretch);

    // 足：付け根の輪から legAxis_ の向きへたなびく（PoseOctopus と同じ式。節の間隔はモデルに合わせる）
    constexpr int kModelSegments = 6;
    const RC::Vector3 A = legAxis_;
    const RC::Vector3 center = {0.0f, 0.02f, -0.08f};
    const float spacing = 0.17f;
    const float legStep = kTwoPi / static_cast<float>(kTentacles);

    // 足 t の付け根の向き（水平成分の角度）に近い rest の足を割り当てる（巡回の向き・ずらしを総当たり）
    std::array<float, kTentacles> alpha{};
    for (int t = 0; t < kTentacles; ++t) {
      const float phi = (static_cast<float>(t) + 0.5f) * legStep;
      const RC::Vector3 radial = LegRadial(LegAxisRest(), phi); // 付け根の輪で割り当てる（毎フレーム変わらない）
      alpha[t] = std::atan2(radial.x, radial.z);
    }
    int bestDir = 1, bestOfs = 0;
    float bestErr = 1e9f;
    for (int dir : {1, -1}) {
      for (int ofs = 0; ofs < kTentacles; ++ofs) {
        float err = 0.0f;
        for (int t = 0; t < kTentacles; ++t) {
          const int mi = ((dir * t + ofs) % kTentacles + kTentacles) % kTentacles;
          err += std::fabs(WrapAngle(alpha[t] - (static_cast<float>(mi) + 0.5f) * legStep));
        }
        if (err < bestErr) { bestErr = err; bestDir = dir; bestOfs = ofs; }
      }
    }

    for (int t = 0; t < kTentacles; ++t) {
      const int mi = ((bestDir * t + bestOfs) % kTentacles + kTentacles) % kTentacles;
      const float restAng = (static_cast<float>(mi) + 0.5f) * legStep;
      const RC::Vector3 restDir = {std::sin(restAng), 0.0f, std::cos(restAng)};
      const float phi = (static_cast<float>(t) + 0.5f) * legStep;
      const RC::Vector3 rootRadial = LegRadial(LegAxisRest(), phi);
      const RC::Vector3 radial = LegSpreadDir(A, rootRadial, phi);
      // 節の位置（最後の節の向きを決めるため 1 つ先まで求める）
      std::array<RC::Vector3, kModelSegments + 1> P{};
      RC::Vector3 p = AddV(center, ScaleV(rootRadial, 0.2f));
      for (int k = 0; k <= kModelSegments; ++k) {
        const float kk = static_cast<float>(k) / static_cast<float>(kModelSegments - 1);
        const float wave = waveAmt_ * std::sin(time_ * 7.0f + static_cast<float>(t) * 0.9f - static_cast<float>(k) * 0.8f);
        const float th = splay_ + wave * (0.3f + kk);
        const RC::Vector3 dir = AddV(ScaleV(A, std::cos(th)), ScaleV(radial, std::sin(th)));
        if (k > 0) p = AddV(p, ScaleV(dir, spacing));
        P[k] = p;
      }
      for (int k = 0; k < kModelSegments; ++k) {
        const RC::Vector3 tang = Norm(Sub(P[k + 1], k > 0 ? P[k - 1] : P[k]), A);
        const std::string joint = "leg" + std::to_string(mi) + "_" + std::to_string(k);
        RC::SetModelJointOverride(h, joint, P[k], ShortestArc(restDir, tang), {1.0f, 1.0f, 1.0f});
      }
    }
    // 書いた骨をこのフレームのうちに反映する（アニメーションの更新は次フレーム冒頭なので 1 フレーム遅れる）
    if (RC::IsModelReady(h)) RC::UpdateModelAnimation(h, 0.0f);
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
      // 「肩越し」と「真横から引き」の 2 つの構図を camShootBlend で混ぜる。
      //   肩越し    : タコの後ろ（サメと反対側）からサメを見る。タコが大きく映る
      //   真横引き  : 2 匹の間の点から真横へ離れる。弾が画面を横切って見える
      // 撃破後は沈むサメでなく最後に止まった位置を使う（向きが急に変わらないように）
      const RC::Vector3 shark = sharkAlive_ ? sharkPos_ : sharkHold_;
      const RC::Vector3 d = Sub(shark, octPos_);
      const RC::Vector3 dH = Norm({d.x, 0.0f, d.z}, Scale(F_, -1.0f)); // タコ→サメ（水平）
      RC::Vector3 side = {dH.z, 0.0f, -dH.x};                          // dH を水平に 90° 回した向き
      if (side.x * R_.x + side.z * R_.z < 0.0f) side = Scale(side, -1.0f); // 追跡中と同じ側

      // 肩越し（下がる向きは水平だけ。経路が斜め上なので 3D のまま下がると真上から見下ろす角度になる）
      const RC::Vector3 shoulderPos =
          Add(Add(Add(octPos_, Scale(dH, -p_.camBack)), Scale(up, 0.7f)), Scale(side, 1.3f));
      const RC::Vector3 shoulderLook = LerpV(octPos_, shark, 0.65f);
      // 真横引き
      const RC::Vector3 focus = LerpV(octPos_, shark, p_.camShootFocus);
      const RC::Vector3 sidePos = Add(Add(focus, Scale(side, p_.camShootDist)), Scale(up, p_.camShootHeight));

      const float b = std::clamp(p_.camShootBlend, 0.0f, 1.0f);
      pos = LerpV(shoulderPos, sidePos, b);
      look = LerpV(shoulderLook, focus, b);
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
  std::weak_ptr<Entity> octModel_; ///< タコのモデル（octopusModel が空なら作らない）
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
  RC::Vector3 legAxis_{0.0f, -0.330f, -0.944f};       ///< 足のなびく向き（胴のローカル。legAxisTarget_ を追う）
  RC::Vector3 legAxisTarget_{0.0f, -0.330f, -0.944f}; ///< 今の段階で狙う足の向き
  float waveAmt_ = 0.35f;   ///< 足のうねりの大きさ（rad）
  float jetPulse_ = 0.0f;   ///< ジェット推進の拍（1 = 水を噴いている瞬間）
  float squashJet_ = 0.0f;  ///< 噴くときの外套膜の縮み（stretch へ足す）
  int lastJet_ = 0;         ///< 最後に泡を吐いた噴射の番号
  float turnFromYaw_ = 0.0f;   ///< Turn の始まりの向き
  float turnFromPitch_ = 0.0f; ///< Turn の始まりの傾き
  float turnFromSplay_ = 0.0f; ///< Turn の始まりの足の開き

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
  float mergeFromSplay_ = 0.35f;
  RC::Vector3 mergeFromPos_{};
  RC::Vector3 mergeCamFrom_{};
  RC::Vector3 mergeRotFrom_{};

  // カメラ
  RC::Vector3 camPos_{}, camLook_{}, camRot_{};
  float shake_ = 0.0f;

  // 効果音
  int seShot_ = -1, seHit_ = -1, seKill_ = -1;
};
