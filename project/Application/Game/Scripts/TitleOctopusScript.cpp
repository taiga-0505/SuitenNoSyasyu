#include "ECS/ScriptableEntity.h"
#include "ECS/ScriptRegistry.h"
#include "ECS/TransformComponent.h"
#include "ECS/CameraComponent.h"
#include "ECS/PrimitiveMeshComponent.h"
#include "ECS/WaterComponent.h"
#include "Common/Water/WaterSurface.h"
#include "Common/Log/Log.h"
#include "Render/Systems/RenderInteractiveWater.h"
#include "RenderCommon.h"
#include "Engine/Render/RenderContext.h"
#include "Framework/App.h"
#include "Scene.h"
#include "Game/Framework/PartPose.h"
#include "Game/Framework/TitleBlackboard.h"

#if RC_ENABLE_IMGUI
#include "imgui/imgui.h"
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <random>
#include <string>
#include <vector>

// ============================================================================
// タイトル画面のタコ（このゲームのプレイヤーキャラ）
//
//   - Title.json の空エンティティに NativeScript として 1 つ付けるだけで動く。
//     他のタイトル用スクリプトとは TitleBlackboard（値だけの掲示板）でつながる：
//       文字・メニューの位置 … TitleScreenScript が書く（しがみつく先・よける先）
//       カーソル          … TitleMouseRippleScript が書く（目で追う・驚く・寄ってくる）
//   - 見た目はプリミティブの組み合わせ（モデルが無いため）：
//       胴（後ろに伸びた楕円球）＋ 顔 ＋ 目 2 つ ＋ 黒目 2 つ ＋ 足 8 本（球を 6 つずつ連ねる）＋ 墨の円盤
//     真上から見下ろすカメラなので、足は水面（または文字の上面）に放射状に広がる形で見せる。
//   - ふるまい（状態遷移）：
//       Hidden   … 水中に隠れている（描かない）。interval 秒ごとに「顔を出す」か「文字にしがみつく」を選ぶ
//       Rising   … 文字・メニューの無い水面から浮かび上がる（飛沫・波紋）
//       Peeking  … 水面に浮いてあたりを見回す。カーソルが近ければ目で追い、
//                   近くでじっとしていると寄ってくる。素早く近づける・クリックすると驚く
//       Climbing … 文字の縁（下・左・右）の水面から文字の上へよじ登る
//       Clinging … 文字にしがみつく（文字の浮き沈み・傾き・押しのけにそのまま乗る）
//       Leaving  … 文字から水へ滑り降りる（驚いたときは墨を吐いて飛び降りる）
//       Diving   … 潜って消える
//     驚くと墨（黒い円盤が広がって薄れる）を吐き、急いで潜る。
//   - 飛び込み・浮上・起動演出の最中（TitleBlackboard::lettersInteractive = false）は潜って隠れる。
//   - エンティティは最初の数十フレームで少しずつ作り（1 フレームに作りすぎると止まる）、
//     出番が来たら SetActive で表示を切り替えて使い回す。"transient" タグ付きでシーン保存に残らない。
//   - 編集モード（再生していない）では何もしない。
//
//   ※ エンジンの描画は Transform の親子を解決しないので、部位のワールド姿勢は自前で合成して書く
//     （TitleAmbientScript と同じ PartPose を使う）。
// ============================================================================

namespace TitleOctopusDetail {

using namespace PartPose;

/// @brief 今の Transform を PrimitiveMesh の描画側へ即座に写す（1 フレーム遅れて見えないように）
/// @param e 対象のエンティティ
inline void SyncRender(Entity &e) {
  auto *tr = e.GetComponent<TransformComponent>();
  if (!tr) return;
  if (auto *pm = e.GetComponent<PrimitiveMeshComponent>()) {
    if (pm->meshHandle >= 0) {
      if (auto *p = RC::GetPrimitiveMeshTransformPtr(pm->meshHandle)) *p = tr->ToTransform();
    }
  }
}

/// @brief 0〜1 にクランプしてから smoothstep
/// @param t 入力
/// @return 0〜1
inline float Smooth01(float t) {
  t = std::clamp(t, 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

/// @brief スカラーの線形補間
/// @param a t = 0 の値
/// @param b t = 1 の値
/// @param t 補間係数
/// @return 補間した値
inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }

} // namespace TitleOctopusDetail

/// @class TitleOctopusScript
/// @brief タイトル画面に、プリミティブで組んだタコを出す（顔を出す・カーソルに反応する・文字にしがみつく）
/// @details JSON (scriptDataList) で設定できる項目は Serialize の中身を参照。
class TitleOctopusScript : public ScriptableEntity {
public:
  // ---- 全体 ----
  bool enabled = true;
  float scale = 0.85f;         ///< 大きさ（1 で胴の幅 ≒ 1m、足を広げて差し渡し ≒ 2.6m）
  float firstDelay = 3.0f;     ///< 操作できるようになってから最初に出るまでの秒数
  float intervalMin = 5.0f;    ///< 隠れてから次に出るまでの秒数（この範囲で乱数）
  float intervalMax = 10.0f;
  float clingChance = 0.45f;   ///< 出るとき「文字にしがみつく」を選ぶ確率（残りは水面から顔を出す）
  float peekMin = 5.0f;        ///< 顔を出している秒数
  float peekMax = 8.0f;
  float clingMin = 6.0f;       ///< しがみついている秒数
  float clingMax = 10.0f;
  // ---- カーソルへの反応 ----
  float lookRadius = 7.0f;         ///< この距離のカーソルを目で追う（m）
  float startleRadius = 1.8f;      ///< この距離でカーソルが素早く動くと驚く（m）
  float startleSpeed = 4.0f;       ///< 驚くカーソルの速さ（m/s）
  float clickStartleRadius = 2.6f; ///< この距離でクリックされると驚く（m）
  float approachSpeed = 0.7f;      ///< カーソルが近くでじっとしているとき寄ってくる速さ（m/s）
  float approachDelay = 1.2f;      ///< カーソルが止まってから寄り始めるまでの秒数
  float approachStop = 2.2f;       ///< この距離まで寄ったら止まる（m）
  // ---- 見た目 ----
  float tentacleWave = 0.32f;      ///< 足のうねりの強さ（rad）
  RC::Vector4 bodyColor = {0.93f, 0.40f, 0.33f, 1.0f};
  RC::Vector4 eyeColor = {0.98f, 0.98f, 0.96f, 1.0f};
  RC::Vector4 pupilColor = {0.05f, 0.05f, 0.07f, 1.0f};
  // ---- 墨 ----
  bool inkEnabled = true;
  float inkSize = 2.6f;            ///< 広がりきったときの半径（m）
  float inkLife = 2.5f;            ///< 消えるまでの秒数
  RC::Vector4 inkColor = {0.04f, 0.03f, 0.07f, 0.85f};

  /// @brief 全設定を JSON にする
  /// @return JSON オブジェクト
  nlohmann::json Serialize() override {
    nlohmann::json j;
    j["enabled"] = enabled;
    j["scale"] = scale;
    j["firstDelay"] = firstDelay;
    j["intervalMin"] = intervalMin;
    j["intervalMax"] = intervalMax;
    j["clingChance"] = clingChance;
    j["peekMin"] = peekMin;
    j["peekMax"] = peekMax;
    j["clingMin"] = clingMin;
    j["clingMax"] = clingMax;
    j["lookRadius"] = lookRadius;
    j["startleRadius"] = startleRadius;
    j["startleSpeed"] = startleSpeed;
    j["clickStartleRadius"] = clickStartleRadius;
    j["approachSpeed"] = approachSpeed;
    j["approachDelay"] = approachDelay;
    j["approachStop"] = approachStop;
    j["tentacleWave"] = tentacleWave;
    j["bodyColor"] = J4(bodyColor);
    j["eyeColor"] = J4(eyeColor);
    j["pupilColor"] = J4(pupilColor);
    j["inkEnabled"] = inkEnabled;
    j["inkSize"] = inkSize;
    j["inkLife"] = inkLife;
    j["inkColor"] = J4(inkColor);
    return j;
  }

  /// @brief JSON から全設定を読み込む（無いキーは現在値のまま）
  /// @param j 読み込み元の JSON
  void Deserialize(const nlohmann::json &j) override {
    ReadB(j, "enabled", enabled);
    ReadF(j, "scale", scale);
    ReadF(j, "firstDelay", firstDelay);
    ReadF(j, "intervalMin", intervalMin);
    ReadF(j, "intervalMax", intervalMax);
    ReadF(j, "clingChance", clingChance);
    ReadF(j, "peekMin", peekMin);
    ReadF(j, "peekMax", peekMax);
    ReadF(j, "clingMin", clingMin);
    ReadF(j, "clingMax", clingMax);
    ReadF(j, "lookRadius", lookRadius);
    ReadF(j, "startleRadius", startleRadius);
    ReadF(j, "startleSpeed", startleSpeed);
    ReadF(j, "clickStartleRadius", clickStartleRadius);
    ReadF(j, "approachSpeed", approachSpeed);
    ReadF(j, "approachDelay", approachDelay);
    ReadF(j, "approachStop", approachStop);
    ReadF(j, "tentacleWave", tentacleWave);
    ReadV4(j, "bodyColor", bodyColor);
    ReadV4(j, "eyeColor", eyeColor);
    ReadV4(j, "pupilColor", pupilColor);
    ReadB(j, "inkEnabled", inkEnabled);
    ReadF(j, "inkSize", inkSize);
    ReadF(j, "inkLife", inkLife);
    ReadV4(j, "inkColor", inkColor);
    scale = std::clamp(scale, 0.2f, 4.0f);
  }

#if RC_ENABLE_IMGUI
  /// @brief 状態の表示・設定の編集・その場で動かすボタン
  void OnImGui() override {
    ImGui::Text("State: %s  (%.1fs)  parts %d / %d", StateName(state_), stateT_, static_cast<int>(parts_.size()),
                kPartCount);
    ImGui::Text("Next in %.1fs", (std::max)(0.0f, timer_));
    ImGui::Checkbox("Enabled", &enabled);
    if (ImGui::Button("Peek Now")) debugRequest_ = 1;
    ImGui::SameLine();
    if (ImGui::Button("Cling Now")) debugRequest_ = 2;
    ImGui::SameLine();
    if (ImGui::Button("Startle")) debugRequest_ = 3;
    ImGui::TextDisabled("%s", debugNote_.c_str());
    ImGui::DragFloat("Scale", &scale, 0.01f, 0.2f, 4.0f);
    ImGui::DragFloatRange2("Interval", &intervalMin, &intervalMax, 0.1f, 0.5f, 60.0f);
    ImGui::DragFloat("Cling Chance", &clingChance, 0.01f, 0.0f, 1.0f);
    ImGui::DragFloatRange2("Peek Time", &peekMin, &peekMax, 0.1f, 0.5f, 60.0f);
    ImGui::DragFloatRange2("Cling Time", &clingMin, &clingMax, 0.1f, 0.5f, 60.0f);
    ImGui::SeparatorText("Cursor");
    ImGui::DragFloat("Look Radius", &lookRadius, 0.1f, 0.0f, 30.0f);
    ImGui::DragFloat("Startle Radius", &startleRadius, 0.05f, 0.0f, 10.0f);
    ImGui::DragFloat("Startle Speed", &startleSpeed, 0.1f, 0.0f, 40.0f);
    ImGui::DragFloat("Click Startle Radius", &clickStartleRadius, 0.05f, 0.0f, 10.0f);
    ImGui::DragFloat("Approach Speed", &approachSpeed, 0.05f, 0.0f, 5.0f);
    ImGui::DragFloat("Approach Delay", &approachDelay, 0.05f, 0.0f, 10.0f);
    ImGui::DragFloat("Approach Stop", &approachStop, 0.05f, 0.5f, 10.0f);
    ImGui::SeparatorText("Look");
    ImGui::DragFloat("Tentacle Wave", &tentacleWave, 0.01f, 0.0f, 1.0f);
    ImGui::ColorEdit4("Body", &bodyColor.x);
    ImGui::ColorEdit4("Eye", &eyeColor.x);
    ImGui::ColorEdit4("Pupil", &pupilColor.x);
    ImGui::SeparatorText("Ink");
    ImGui::Checkbox("Ink", &inkEnabled);
    ImGui::DragFloat("Ink Size", &inkSize, 0.05f, 0.2f, 10.0f);
    ImGui::DragFloat("Ink Life", &inkLife, 0.05f, 0.2f, 10.0f);
    ImGui::ColorEdit4("Ink Color", &inkColor.x);
    ImGui::TextDisabled("colors are applied on scene reload");
  }
#endif

protected:
  /// @brief 乱数と最初の待ち時間を初期化する（部位は再生中の最初の Update から作る）
  void OnCreate() override {
    std::random_device rd;
    rng_.seed(rd());
    timer_ = firstDelay;
    state_ = State::Hidden;
  }

  /// @brief 再生中だけ、部位を少しずつ作り、状態を進めて姿勢を書く
  /// @param deltaTime 前フレームからの経過秒
  void OnUpdate(float deltaTime) override {
    SceneContext *ctx = GetSceneContext();
    if (!ctx || !ctx->isPlaying()) return;
    Scene *scene = GetScene();
    if (!scene) return;
    BuildSome(scene);
    if (static_cast<int>(parts_.size()) < kPartCount || !ink_.lock()) return;

    const float dt = (std::max)(deltaTime, 0.0f);
    time_ += dt;
    hasWater_ = BuildWaterParams(scene, water_, waterY_);
    waterTime_ = RC::GetWaterTime();
    UpdateInk(dt);

    const TitleBlackboard::State &bb = TitleBlackboard::Get();
    if (debugRequest_ != 0) HandleDebugRequest(scene);

    // 無効にされた・文字に触れられない（起動演出・飛び込み・浮上の最中）なら潜って隠れる
    const bool allowed = enabled && bb.lettersInteractive;
    if (!allowed && state_ != State::Hidden && state_ != State::Diving) BeginDive(true);

    switch (state_) {
    case State::Hidden: UpdateHidden(scene, dt, allowed); break;
    case State::Rising: UpdateRising(dt); break;
    case State::Peeking: UpdatePeeking(dt); break;
    case State::Climbing: UpdateClimbing(dt); break;
    case State::Clinging: UpdateClinging(dt); break;
    case State::Leaving: UpdateLeaving(dt); break;
    case State::Diving: UpdateDiving(dt); break;
    }
    if (state_ != State::Hidden) Pose(dt);
  }

  /// @brief 作った部位とフォルダを破棄する
  void OnDestroy() override {
    for (auto &p : parts_) {
      if (auto e = p.entity.lock()) e->Destroy();
    }
    parts_.clear();
    if (auto e = ink_.lock()) e->Destroy();
    ink_.reset();
    if (auto f = folder_.lock()) f->Destroy();
    folder_.reset();
  }

private:
  using Mat3 = PartPose::Mat3;

  /// @brief ふるまいの状態
  enum class State { Hidden, Rising, Peeking, Climbing, Clinging, Leaving, Diving };

  /// @brief 部位 1 つ（エンティティと、胴に対する置き方）
  struct Part {
    std::weak_ptr<Entity> entity;
    RC::Vector3 size{1, 1, 1}; ///< scale 1 のときの Transform.scale
  };

  // 部位の並び（parts_ の添字）
  static constexpr int kMantle = 0;
  static constexpr int kFace = 1;
  static constexpr int kEyeL = 2;
  static constexpr int kEyeR = 3;
  static constexpr int kPupilL = 4;
  static constexpr int kPupilR = 5;
  static constexpr int kTentacleStart = 6;
  static constexpr int kTentacles = 8;
  static constexpr int kSegments = 6;
  static constexpr int kPartCount = kTentacleStart + kTentacles * kSegments;
  static constexpr int kBuildPerFrame = 3;     ///< 1 フレームに作る部位の数
  static constexpr float kHideDepth = 1.8f;    ///< 隠れているときの深さ（m、scale 1）
  static constexpr float kRiseTime = 1.0f;     ///< 浮かび上がる秒数
  static constexpr float kClimbTime = 0.8f;    ///< よじ登る秒数
  static constexpr float kLeaveTime = 0.7f;    ///< 滑り降りる秒数
  static constexpr float kDiveTime = 0.7f;     ///< 潜る秒数
  static constexpr float kFastFactor = 0.45f;  ///< 驚いたときの所要時間の倍率

  // ------------------------------------------------------------------
  // JSON ヘルパー
  // ------------------------------------------------------------------

  static nlohmann::json J4(const RC::Vector4 &v) { return {v.x, v.y, v.z, v.w}; }
  static void ReadF(const nlohmann::json &j, const char *key, float &out) {
    if (j.contains(key) && j[key].is_number()) out = j[key].get<float>();
  }
  static void ReadB(const nlohmann::json &j, const char *key, bool &out) {
    if (j.contains(key) && j[key].is_boolean()) out = j[key].get<bool>();
  }
  static void ReadV4(const nlohmann::json &j, const char *key, RC::Vector4 &out) {
    if (!j.contains(key) || !j[key].is_array() || j[key].size() < 4) return;
    const auto &c = j[key];
    out = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>(), c[3].get<float>()};
  }

  /// @brief 状態の表示名
  /// @param s 状態
  /// @return 名前
  static const char *StateName(State s) {
    switch (s) {
    case State::Hidden: return "Hidden";
    case State::Rising: return "Rising";
    case State::Peeking: return "Peeking";
    case State::Climbing: return "Climbing";
    case State::Clinging: return "Clinging";
    case State::Leaving: return "Leaving";
    case State::Diving: return "Diving";
    }
    return "?";
  }

  // ------------------------------------------------------------------
  // 乱数・環境
  // ------------------------------------------------------------------

  float Rand(float a, float b) {
    if (b <= a) return a;
    std::uniform_real_distribution<float> d(a, b);
    return d(rng_);
  }
  float Rand01() { return Rand(0.0f, 1.0f); }

  /// @brief シーン内で最初に見つかった水面から波のパラメータを作る
  /// @param scene 対象のシーン
  /// @param out 波のパラメータ
  /// @param outY 水面の高さ
  /// @return 水面があれば true
  static bool BuildWaterParams(Scene *scene, RC::WaterWaveParams &out, float &outY) {
    for (const auto &e : scene->GetEntities()) {
      if (!e) continue;
      auto *w = e->GetComponent<WaterComponent>();
      if (!w) continue;
      auto *tr = e->GetComponent<TransformComponent>();
      outY = tr ? tr->position.y : 0.0f;
      out = w->ToWaveParams(outY);
      return true;
    }
    return false;
  }

  /// @brief 水面の高さ（Gerstner ＋ 波紋）
  /// @param x ワールド X
  /// @param z ワールド Z
  /// @return 高さ
  float WaterHeight(float x, float z) const {
    if (!hasWater_) return waterY_;
    float h = RC::WaterSurface::SampleHeight(water_, waterTime_, x, z, nullptr, 0, 1.0f, 3.0f, 1);
    h += RC::SampleInteractiveWaterHeight(x, z);
    return h;
  }

  /// @brief メインカメラから、水面の高さで見えている範囲を求める
  /// @param scene 対象のシーン
  /// @param cx 中心 X
  /// @param cz 中心 Z
  /// @param hw 半幅
  /// @param hh 半高さ
  /// @return カメラが見つかれば true
  bool ViewRect(Scene *scene, float &cx, float &cz, float &hw, float &hh) const {
    for (const auto &e : scene->GetEntities()) {
      if (!e || !e->IsActive()) continue;
      auto *cam = e->GetComponent<CameraComponent>();
      auto *tr = e->GetComponent<TransformComponent>();
      if (!cam || !cam->isMain || !tr) continue;
      float aspect = 16.0f / 9.0f;
      auto &rc = RC::GetRenderContext();
      if (rc.Ctx() && rc.Ctx()->app && rc.Ctx()->app->height > 0) {
        aspect = static_cast<float>(rc.Ctx()->app->width) / static_cast<float>(rc.Ctx()->app->height);
      }
      cx = tr->position.x;
      cz = tr->position.z;
      hh = (std::max)(tr->position.y - waterY_, 1.0f) * std::tan((std::max)(cam->fovY, 0.05f) * 0.5f);
      hw = hh * aspect;
      return true;
    }
    return false;
  }

  /// @brief 点のまわり（半径 r）に文字・メニューがあるか
  /// @param x ワールド X
  /// @param z ワールド Z
  /// @param r 半径（m）
  /// @return 重なっていれば true
  static bool NearLetters(float x, float z, float r) {
    const TitleBlackboard::State &bb = TitleBlackboard::Get();
    RC::Vector3 n{};
    float depth = 0.0f;
    for (const auto &L : bb.letters) {
      if (L.visible && TitleBlackboard::CircleVsLetter(L, x, z, r, n, depth)) return true;
    }
    for (const auto &L : bb.menu) {
      if (L.visible && TitleBlackboard::CircleVsLetter(L, x, z, r, n, depth)) return true;
    }
    return false;
  }

  // ------------------------------------------------------------------
  // 部位の生成
  // ------------------------------------------------------------------

  /// @brief 部位を kBuildPerFrame 個ずつ作る（フォルダは最初に 1 つ）
  /// @param scene 対象のシーン
  void BuildSome(Scene *scene) {
    if (static_cast<int>(parts_.size()) >= kPartCount && ink_.lock()) return;
    if (!folder_.lock()) {
      auto folder = scene->CreateEntity("TitleOctopus_Parts");
      if (folder) {
        folder->SetIsFolder(true);
        folder->SetTag("transient", 1);
        folder_ = folder;
      }
    }
    const RC::Vector4 skinDark = {bodyColor.x * 0.85f, bodyColor.y * 0.8f, bodyColor.z * 0.8f, bodyColor.w};
    for (int n = 0; n < kBuildPerFrame; ++n) {
      const int i = static_cast<int>(parts_.size());
      if (i >= kPartCount) {
        if (!ink_.lock()) {
          // 墨：ごく薄い円柱（半透明。広がりながら薄れる）
          auto e = MakePrimitive(scene, "TitleOctopus_Ink", PrimitiveType::Cylinder, inkColor);
          ink_ = e;
        }
        return;
      }
      Part p;
      std::shared_ptr<Entity> e;
      // 大きさは scale 1 のとき（球は半径、円柱は半径 × 高さ）。+Z が顔の前、+Y が上
      switch (i) {
      case kMantle:
        e = MakePrimitive(scene, "TitleOctopus_Mantle", PrimitiveType::Sphere, bodyColor);
        p.size = {0.46f, 0.40f, 0.58f};
        break;
      case kFace:
        e = MakePrimitive(scene, "TitleOctopus_Face", PrimitiveType::Sphere, bodyColor);
        p.size = {0.42f, 0.36f, 0.40f};
        break;
      case kEyeL:
      case kEyeR:
        e = MakePrimitive(scene, "TitleOctopus_Eye", PrimitiveType::Sphere, eyeColor);
        p.size = {0.12f, 0.11f, 0.12f};
        break;
      case kPupilL:
      case kPupilR:
        e = MakePrimitive(scene, "TitleOctopus_Pupil", PrimitiveType::Sphere, pupilColor);
        p.size = {0.06f, 0.05f, 0.07f};
        break;
      default: {
        // 足：根元から先へ細くなる球。1 本おきに少し色を変えて足の区切りを見せる
        const int k = (i - kTentacleStart) % kSegments;
        const int t = (i - kTentacleStart) / kSegments;
        const float r = 0.15f - 0.018f * static_cast<float>(k);
        e = MakePrimitive(scene, "TitleOctopus_Leg", PrimitiveType::Sphere, (t % 2 == 0) ? bodyColor : skinDark);
        p.size = {r, r * 0.75f, r};
        break;
      }
      }
      p.entity = e;
      parts_.push_back(p);
    }
  }

  /// @brief プリミティブのエンティティを 1 つ作る（非アクティブ・画面外で作る）
  /// @param scene 対象のシーン
  /// @param name エンティティ名
  /// @param type 形状
  /// @param color 色
  /// @return 作ったエンティティ（失敗したら nullptr）
  std::shared_ptr<Entity> MakePrimitive(Scene *scene, const std::string &name, PrimitiveType type,
                                        const RC::Vector4 &color) {
    auto e = scene->CreateEntity(name);
    if (!e) return nullptr;
    if (auto f = folder_.lock()) e->SetParentGuid(f->Guid());
    e->SetTag("transient", 1); // シーン保存に含めない
    auto &tr = e->AddComponent<TransformComponent>();
    tr.position = {0.0f, -500.0f, 0.0f};
    tr.scale = {0.01f, 0.01f, 0.01f};
    auto &pm = e->AddComponent<PrimitiveMeshComponent>();
    pm.type = type;
    pm.color = color;
    pm.shininess = 24.0f;
    scene->InitDynamicEntityRuntime(*e);
    e->SetActive(false);
    return e;
  }

  /// @brief 部位の表示・非表示をまとめて切り替える
  /// @param visible 表示するなら true
  void SetVisible(bool visible) {
    if (visible_ == visible) return;
    visible_ = visible;
    for (auto &p : parts_) {
      if (auto e = p.entity.lock()) e->SetActive(visible);
    }
  }

  /// @brief 部位 1 つへワールド姿勢を書き込む
  /// @param p 部位
  /// @param pos ワールド位置
  /// @param rot ワールド回転
  /// @param scl 大きさの倍率（size × scale にさらに掛ける。伸び縮み用）
  void PlacePart(const Part &p, const RC::Vector3 &pos, const Mat3 &rot, const RC::Vector3 &scl) {
    auto e = p.entity.lock();
    if (!e) return;
    auto *tr = e->GetComponent<TransformComponent>();
    if (!tr) return;
    tr->position = pos;
    tr->rotation = PartPose::ToEngineEuler(rot);
    tr->scale = {p.size.x * scale * scl.x, p.size.y * scale * scl.y, p.size.z * scale * scl.z};
    TitleOctopusDetail::SyncRender(*e);
  }

  // ------------------------------------------------------------------
  // 状態ごとの更新
  // ------------------------------------------------------------------

  /// @brief 状態を切り替える（経過時間を 0 に戻す）
  /// @param s 次の状態
  /// @param duration その状態の長さ（秒。使わない状態は 0）
  void Enter(State s, float duration = 0.0f) {
    state_ = s;
    stateT_ = 0.0f;
    stateDur_ = duration;
  }

  /// @brief 隠れているあいだ：待ち時間が明けたら、顔を出すか文字にしがみつくかを選んで出る
  /// @param scene 対象のシーン
  /// @param dt 経過秒
  /// @param allowed 出てよいか
  void UpdateHidden(Scene *scene, float dt, bool allowed) {
    SetVisible(false);
    if (!allowed) {
      timer_ = (std::max)(timer_, firstDelay); // 操作できるようになってから firstDelay 待つ
      return;
    }
    timer_ -= dt;
    if (timer_ > 0.0f) return;
    const bool wantCling = Rand01() < clingChance;
    if ((wantCling && TryStartCling(scene)) || TryStartPeek(scene) || (!wantCling && TryStartCling(scene))) return;
    timer_ = 1.0f; // 出られる場所が無かった：少し待ってやり直す
  }

  /// @brief 文字・メニューの無い水面を選んで浮かび上がり始める
  /// @param scene 対象のシーン
  /// @return 始められたら true
  bool TryStartPeek(Scene *scene) {
    float cx, cz, hw, hh;
    if (!ViewRect(scene, cx, cz, hw, hh)) return false;
    const float margin = 2.5f * scale + 1.0f;
    const TitleBlackboard::State &bb = TitleBlackboard::Get();
    for (int tries = 0; tries < 24; ++tries) {
      const float x = cx + Rand(-1.0f, 1.0f) * (std::max)(hw - margin, 0.5f);
      const float z = cz + Rand(-1.0f, 1.0f) * (std::max)(hh - margin, 0.5f);
      if (NearLetters(x, z, 1.6f * scale)) continue;
      // カーソルの真下には出ない（出た瞬間に驚いてしまう）
      if (bb.cursorValid) {
        const float dx = x - bb.cursorPos.x, dz = z - bb.cursorPos.z;
        if (dx * dx + dz * dz < (startleRadius + 1.5f) * (startleRadius + 1.5f)) continue;
      }
      pos_ = {x, 0.0f, z};
      baseYaw_ = Rand(-PartPose::kPi, PartPose::kPi);
      yaw_ = baseYaw_;
      sink_ = kHideDepth;
      breached_ = false;
      cursorStill_ = 0.0f;
      SetVisible(true);
      Enter(State::Rising, kRiseTime);
      return true;
    }
    return false;
  }

  /// @brief しがみつく文字と縁を選び、縁の外の水面からよじ登り始める
  /// @param scene 対象のシーン
  /// @return 始められたら true
  bool TryStartCling(Scene *scene) {
    const TitleBlackboard::State &bb = TitleBlackboard::Get();
    if (bb.letters.empty()) return false;
    float cx, cz, hw, hh;
    if (!ViewRect(scene, cx, cz, hw, hh)) return false;
    std::vector<int> candidates;
    for (int i = 0; i < static_cast<int>(bb.letters.size()); ++i) {
      if (bb.letters[i].visible) candidates.push_back(i);
    }
    if (candidates.empty()) return false;
    for (int tries = 0; tries < 8; ++tries) {
      std::uniform_int_distribution<size_t> pick(0, candidates.size() - 1);
      const int li = candidates[pick(rng_)];
      const TitleBlackboard::Letter &L = bb.letters[li];
      // 縁：0 下 / 1 左 / 2 右（文字のローカル。下は画面の下側）
      std::uniform_int_distribution<int> edgePick(0, 2);
      const int edge = edgePick(rng_);
      const float inset = 0.45f * scale;
      float lx = 0.0f, lz = 0.0f, ox = 0.0f, oz = 0.0f;
      if (edge == 0) {
        lx = Rand(-0.45f, 0.45f) * L.halfX;
        lz = -L.halfZ + inset;
        oz = -1.0f;
      } else {
        lx = (edge == 1 ? -1.0f : 1.0f) * (L.halfX - inset);
        lz = Rand(-0.3f, 0.3f) * L.halfZ;
        ox = (edge == 1 ? -1.0f : 1.0f);
      }
      // 縁の外の水面が画面内で、他の文字・メニューにかぶらないか
      RC::Vector3 attach, water, outward;
      LetterFrame(L, lx, lz, ox, oz, attach, water, outward);
      if (std::fabs(water.x - cx) > hw - 1.0f || std::fabs(water.z - cz) > hh - 1.0f) continue;
      bool blocked = false;
      for (int k = 0; k < static_cast<int>(bb.letters.size()) && !blocked; ++k) {
        if (k == li || !bb.letters[k].visible) continue;
        RC::Vector3 n{};
        float depth = 0.0f;
        blocked = TitleBlackboard::CircleVsLetter(bb.letters[k], water.x, water.z, 1.0f * scale, n, depth);
      }
      if (blocked) continue;
      letter_ = li;
      attachLx_ = lx;
      attachLz_ = lz;
      outLx_ = ox;
      outLz_ = oz;
      climb_ = 0.0f;
      sink_ = 0.0f; // 降りたあとは水面から潜る
      fastLeave_ = false;
      cursorStill_ = 0.0f;
      SetVisible(true);
      Splash(water.x, water.z, 0.6f);
      Enter(State::Climbing, kClimbTime);
      return true;
    }
    return false;
  }

  /// @brief 文字のローカル位置から、しがみつく点・縁の外の水面の点・外向きを求める
  /// @param L 文字
  /// @param lx しがみつく点（文字のローカル X）
  /// @param lz しがみつく点（文字のローカル Z ＝ 文字の上方向）
  /// @param ox 外向き（ローカル X）
  /// @param oz 外向き（ローカル Z）
  /// @param attach しがみつく点（ワールド。y は文字の上面）
  /// @param water 縁の外の水面の点（ワールド。y は水面）
  /// @param outward 外向き（ワールド xz）
  void LetterFrame(const TitleBlackboard::Letter &L, float lx, float lz, float ox, float oz, RC::Vector3 &attach,
                   RC::Vector3 &water, RC::Vector3 &outward) const {
    // 文字のローカル X → ワールド (cos, 0, -sin)、ローカル Z（文字の上）→ (sin, 0, cos)
    const float c = std::cos(L.yaw), s = std::sin(L.yaw);
    attach = {L.center.x + lx * c + lz * s, 0.0f, L.center.z - lx * s + lz * c};
    attach.y = LetterTopAt(L, attach.x, attach.z);
    outward = {ox * c + oz * s, 0.0f, -ox * s + oz * c};
    const float out = 0.45f * scale + 0.9f * scale; // しがみつく点 → 縁 → 水面の点
    water = {attach.x + outward.x * out, 0.0f, attach.z + outward.z * out};
    water.y = WaterHeight(water.x, water.z);
  }

  /// @brief 文字の上面の高さ（傾き込み）
  /// @param L 文字
  /// @param x ワールド X
  /// @param z ワールド Z
  /// @return 高さ
  static float LetterTopAt(const TitleBlackboard::Letter &L, float x, float z) {
    // TitleScreenScript は rotation = (π/2 + a, yaw, b) で上面の法線を (-cos a·sin b, cos a·cos b, sin a) に合わせている
    const float a = L.rotation.x - PartPose::kHalfPi;
    const float b = L.rotation.z;
    const float nx = -std::cos(a) * std::sin(b);
    const float ny = (std::max)(std::cos(a) * std::cos(b), 0.3f);
    const float nz = std::sin(a);
    return L.topY - (nx * (x - L.center.x) + nz * (z - L.center.z)) / ny;
  }

  /// @brief 浮かび上がる：深さを 0 へ。水面を抜けた瞬間に飛沫と波紋
  /// @param dt 経過秒
  void UpdateRising(float dt) {
    stateT_ += dt;
    const float u = std::clamp(stateT_ / (std::max)(stateDur_, 0.05f), 0.0f, 1.0f);
    sink_ = kHideDepth * (1.0f - TitleOctopusDetail::Smooth01(u));
    if (!breached_ && sink_ < 0.35f) {
      breached_ = true;
      Splash(pos_.x, pos_.z, 0.8f);
      squash_ = 0.6f; // 水を抜けた勢いで縦に伸びる
    }
    if (u >= 1.0f) Enter(State::Peeking, Rand(peekMin, (std::max)(peekMin, peekMax)));
  }

  /// @brief 水面に浮いている：見回す・カーソルを目で追う・寄ってくる・驚く
  /// @param dt 経過秒
  void UpdatePeeking(float dt) {
    stateT_ += dt;
    const TitleBlackboard::State &bb = TitleBlackboard::Get();
    float wantYaw = baseYaw_ + 0.7f * std::sin(time_ * 0.45f + baseYaw_);
    float lookX = std::sin(time_ * 0.9f) * 0.6f, lookZ = 0.8f; // 黒目の向き（胴のローカル xz）
    if (bb.cursorValid) {
      const float dx = bb.cursorPos.x - pos_.x, dz = bb.cursorPos.z - pos_.z;
      const float d = std::sqrt(dx * dx + dz * dz);
      const float cs = std::sqrt(bb.cursorVel.x * bb.cursorVel.x + bb.cursorVel.z * bb.cursorVel.z);
      if ((d < startleRadius && cs > startleSpeed) || (bb.cursorClicked && d < clickStartleRadius)) {
        Startle();
        return;
      }
      if (d < lookRadius && d > 1e-3f) {
        wantYaw = std::atan2(dx, dz);
        LookLocal(dx / d, dz / d, lookX, lookZ);
        // 近くでじっとしているカーソルには寄っていく（文字やメニューの下へは行かない）
        cursorStill_ = (cs < 0.6f && d > approachStop) ? cursorStill_ + dt : 0.0f;
        if (cursorStill_ > approachDelay) {
          const float step = (std::min)(approachSpeed * dt, d - approachStop);
          const float nx = pos_.x + dx / d * step, nz = pos_.z + dz / d * step;
          if (step > 0.0f && !NearLetters(nx, nz, 1.4f * scale)) {
            pos_.x = nx;
            pos_.z = nz;
            swim_ = (std::min)(1.0f, swim_ + dt * 3.0f);
          }
        }
      } else {
        cursorStill_ = 0.0f;
      }
    }
    TurnTo(wantYaw, dt, 2.5f);
    lookX_ += (lookX - lookX_) * (std::min)(1.0f, dt * 8.0f);
    lookZ_ += (lookZ - lookZ_) * (std::min)(1.0f, dt * 8.0f);
    // ときどき小さな波紋（呼吸）
    rippleTimer_ -= dt;
    if (rippleTimer_ <= 0.0f) {
      RC::AddWaveSourceAtWorld(pos_.x, pos_.z, 0.01f, -0.03f);
      rippleTimer_ = Rand(0.7f, 1.1f);
    }
    if (stateT_ >= stateDur_) BeginDive(false);
  }

  /// @brief よじ登る：縁の外の水面 → 文字の上（弧を描く）
  /// @param dt 経過秒
  void UpdateClimbing(float dt) {
    stateT_ += dt;
    const TitleBlackboard::Letter *L = CurrentLetter();
    if (!L) {
      BeginDive(true);
      return;
    }
    climb_ = std::clamp(stateT_ / (std::max)(stateDur_, 0.05f), 0.0f, 1.0f);
    LookAtCursorWhileOnLetter(dt);
    if (climb_ >= 1.0f) {
      TitleBlackboard::PushLetter(static_cast<size_t>(letter_), {0.0f, 0.0f, 0.0f}, 0.05f); // 乗った重みでぐらり
      squash_ = -0.5f; // 着地でつぶれる
      Enter(State::Clinging, Rand(clingMin, (std::max)(clingMin, clingMax)));
    }
  }

  /// @brief しがみついている：文字に乗ったまま、カーソルを目で追う。驚いたら墨を吐いて飛び降りる
  /// @param dt 経過秒
  void UpdateClinging(float dt) {
    stateT_ += dt;
    const TitleBlackboard::Letter *L = CurrentLetter();
    if (!L) {
      BeginLeave(true);
      return;
    }
    climb_ = 1.0f;
    const TitleBlackboard::State &bb = TitleBlackboard::Get();
    if (bb.cursorValid) {
      RC::Vector3 attach, water, outward;
      LetterFrame(*L, attachLx_, attachLz_, outLx_, outLz_, attach, water, outward);
      const float dx = bb.cursorPos.x - attach.x, dz = bb.cursorPos.z - attach.z;
      const float d = std::sqrt(dx * dx + dz * dz);
      const float cs = std::sqrt(bb.cursorVel.x * bb.cursorVel.x + bb.cursorVel.z * bb.cursorVel.z);
      if ((d < startleRadius && cs > startleSpeed) || (bb.cursorClicked && d < clickStartleRadius)) {
        Ink(attach.x, attach.z);
        BeginLeave(true);
        return;
      }
    }
    LookAtCursorWhileOnLetter(dt);
    if (stateT_ >= stateDur_) BeginLeave(false);
  }

  /// @brief 滑り降りる：文字の上 → 縁の外の水面。降りきったら潜る
  /// @param dt 経過秒
  void UpdateLeaving(float dt) {
    stateT_ += dt;
    const TitleBlackboard::Letter *L = CurrentLetter();
    // 降り始めた高さ（leaveFrom_）から 0 へ
    climb_ = leaveFrom_ * (1.0f - std::clamp(stateT_ / (std::max)(stateDur_, 0.05f), 0.0f, 1.0f));
    if (!L) climb_ = 0.0f; // 文字が消えた：その場（最後に置いた位置）から潜る
    if (climb_ <= 0.0f) {
      // 水面へ着いた：そこから潜る（位置は縁の外の水面。水しぶきは BeginDive が出す）
      if (L) {
        RC::Vector3 attach, water, outward;
        LetterFrame(*L, attachLx_, attachLz_, outLx_, outLz_, attach, water, outward);
        pos_ = {water.x, 0.0f, water.z};
      }
      letter_ = -1;
      sink_ = 0.0f;
      BeginDive(fastLeave_);
    }
  }

  /// @brief 潜る：深さを kHideDepth へ。潜りきったら隠れる
  /// @param dt 経過秒
  void UpdateDiving(float dt) {
    stateT_ += dt;
    const float u = std::clamp(stateT_ / (std::max)(stateDur_, 0.05f), 0.0f, 1.0f);
    sink_ = (std::max)(sink_, kHideDepth * TitleOctopusDetail::Smooth01(u));
    // 驚いて潜るときはカーソルから離れる向きへ泳いで逃げる
    if (fleeing_) {
      pos_.x += fleeDir_.x * 3.0f * dt;
      pos_.z += fleeDir_.z * 3.0f * dt;
    }
    if (u >= 1.0f) {
      SetVisible(false);
      Enter(State::Hidden);
      timer_ = Rand(intervalMin, (std::max)(intervalMin, intervalMax)) * (fleeing_ ? 1.3f : 1.0f);
      fleeing_ = false;
    }
  }

  /// @brief 潜り始める
  /// @param fast 驚いたとき（短い時間で潜る）
  void BeginDive(bool fast) {
    if (state_ == State::Clinging || state_ == State::Climbing) {
      // 文字の上から直接は潜れないので、いったん滑り降りる
      BeginLeave(fast);
      return;
    }
    if (state_ == State::Leaving) {
      // 降りている途中：降りきってから潜る（UpdateLeaving が BeginDive を呼ぶ）
      fastLeave_ = fastLeave_ || fast;
      if (climb_ > 0.0f) return;
    }
    if (state_ == State::Hidden) return;
    Splash(pos_.x, pos_.z, fast ? 0.9f : 0.5f);
    Enter(State::Diving, kDiveTime * (fast ? kFastFactor : 1.0f));
  }

  /// @brief 文字から降り始める
  /// @param fast 驚いたとき（飛び降りる）
  void BeginLeave(bool fast) {
    fastLeave_ = fast;
    if (fast) squash_ = 0.7f;
    // 今の高さ（climb_）から降り始める（よじ登っている途中で降りることもある）
    leaveFrom_ = std::clamp(climb_, 0.0f, 1.0f);
    const float dur = kLeaveTime * (fast ? kFastFactor : 1.0f) * (std::max)(leaveFrom_, 0.2f);
    Enter(State::Leaving, dur);
    if (fast) {
      fleeing_ = true;
      const TitleBlackboard::Letter *L = CurrentLetter();
      if (L) {
        RC::Vector3 attach, water, outward;
        LetterFrame(*L, attachLx_, attachLz_, outLx_, outLz_, attach, water, outward);
        fleeDir_ = outward;
      }
    }
  }

  /// @brief 驚く：墨を吐き、身を縮めて、カーソルから離れる向きへ潜って逃げる
  void Startle() {
    const TitleBlackboard::State &bb = TitleBlackboard::Get();
    Ink(pos_.x, pos_.z);
    squash_ = 0.7f;
    fleeing_ = true;
    float dx = pos_.x - bb.cursorPos.x, dz = pos_.z - bb.cursorPos.z;
    const float d = std::sqrt(dx * dx + dz * dz);
    if (d > 1e-3f) {
      dx /= d;
      dz /= d;
    } else {
      dx = std::sin(yaw_);
      dz = std::cos(yaw_);
    }
    fleeDir_ = {dx, 0.0f, dz};
    yaw_ = std::atan2(dx, dz); // 逃げる向きへ向き直る
    if (state_ == State::Clinging || state_ == State::Climbing) {
      BeginLeave(true);
    } else {
      BeginDive(true);
    }
  }

  /// @brief しがみついている文字（無ければ・見えなければ nullptr）
  /// @return 文字
  const TitleBlackboard::Letter *CurrentLetter() const {
    const TitleBlackboard::State &bb = TitleBlackboard::Get();
    if (letter_ < 0 || letter_ >= static_cast<int>(bb.letters.size())) return nullptr;
    const TitleBlackboard::Letter &L = bb.letters[letter_];
    return L.visible ? &L : nullptr;
  }

  /// @brief 文字の上にいるあいだの向き（文字の内側を向き、カーソルが近ければそちらへ少し首を振る）
  /// @param dt 経過秒
  void LookAtCursorWhileOnLetter(float dt) {
    const TitleBlackboard::Letter *L = CurrentLetter();
    if (!L) return;
    RC::Vector3 attach, water, outward;
    LetterFrame(*L, attachLx_, attachLz_, outLx_, outLz_, attach, water, outward);
    const float inwardYaw = std::atan2(-outward.x, -outward.z);
    float want = inwardYaw + 0.35f * std::sin(time_ * 0.6f);
    float lookX = std::sin(time_ * 1.1f) * 0.5f, lookZ = 0.8f;
    const TitleBlackboard::State &bb = TitleBlackboard::Get();
    if (bb.cursorValid) {
      const float dx = bb.cursorPos.x - attach.x, dz = bb.cursorPos.z - attach.z;
      const float d = std::sqrt(dx * dx + dz * dz);
      if (d < lookRadius && d > 1e-3f) {
        // 体は内側から ±70° まで。黒目はそのままカーソルへ
        const float rel = PartPose::WrapAngle(std::atan2(dx, dz) - inwardYaw);
        want = inwardYaw + std::clamp(rel, -1.2f, 1.2f);
        LookLocal(dx / d, dz / d, lookX, lookZ);
      }
    }
    TurnTo(want, dt, 3.0f);
    lookX_ += (lookX - lookX_) * (std::min)(1.0f, dt * 8.0f);
    lookZ_ += (lookZ - lookZ_) * (std::min)(1.0f, dt * 8.0f);
  }

  /// @brief 向きを目標へ角速度 rate で寄せる
  /// @param want 目標の向き（rad）
  /// @param dt 経過秒
  /// @param rate 寄せる速さ（1/s）
  void TurnTo(float want, float dt, float rate) {
    yaw_ = PartPose::WrapAngle(yaw_ + PartPose::WrapAngle(want - yaw_) * (std::min)(1.0f, dt * rate));
  }

  /// @brief ワールドの向き (wx, wz) を胴のローカル xz（+Z が前）へ直す（黒目を動かすため）
  /// @param wx 向き X
  /// @param wz 向き Z
  /// @param lx ローカル X の出力先
  /// @param lz ローカル Z の出力先
  void LookLocal(float wx, float wz, float &lx, float &lz) const {
    // 胴の前 = (sin yaw, cos yaw)、右 = (cos yaw, -sin yaw)
    const float c = std::cos(yaw_), s = std::sin(yaw_);
    lx = wx * c - wz * s;
    lz = wx * s + wz * c;
  }

  // ------------------------------------------------------------------
  // 演出
  // ------------------------------------------------------------------

  /// @brief 水しぶき・波紋
  /// @param x ワールド X
  /// @param z ワールド Z
  /// @param power 強さ（1 で驚いて潜るくらい）
  void Splash(float x, float z, float power) {
    const int n = std::clamp(static_cast<int>(8.0f * power + 0.5f), 0, 24);
    for (int i = 0; i < n; ++i) {
      RC::EffectParticleSpawn sp;
      sp.kind = RC::EffectParticleKind::Wake;
      sp.position = {x + Rand(-0.3f, 0.3f) * scale, waterY_ + 0.05f, z + Rand(-0.3f, 0.3f) * scale};
      sp.scale = Rand(0.06f, 0.12f) * scale;
      sp.color = {0.88f, 0.94f, 1.0f, 0.9f};
      RC::SpawnEffectParticle(sp);
    }
    RC::AddWaveSourceAtWorld(x, z, 0.018f * scale, -0.18f * power);
    RC::AddCrestFoamSourceAtWorld(x, z, 0.01f * scale, 0.6f * power);
  }

  /// @brief 墨を吐く（水面に黒い円盤が広がって薄れる）
  /// @param x ワールド X
  /// @param z ワールド Z
  void Ink(float x, float z) {
    Splash(x, z, 1.0f);
    if (!inkEnabled) return;
    inkPos_ = {x, 0.0f, z};
    inkT_ = 0.0f;
    inkActive_ = true;
    if (auto e = ink_.lock()) e->SetActive(true);
    UpdateInk(0.0f);
  }

  /// @brief 墨の円盤を広げて薄れさせる（消えたら非アクティブ）
  /// @param dt 経過秒
  void UpdateInk(float dt) {
    if (!inkActive_) return;
    auto e = ink_.lock();
    if (!e) return;
    inkT_ += dt;
    const float life = (std::max)(inkLife, 0.1f);
    const float t = std::clamp(inkT_ / life, 0.0f, 1.0f);
    if (t >= 1.0f) {
      inkActive_ = false;
      e->SetActive(false);
      return;
    }
    const float grow = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t); // 素早く広がって止まる
    const float r = (0.3f + (inkSize - 0.3f) * grow) * scale;
    const float alpha = inkColor.w * (1.0f - t) * (1.0f - t);
    if (auto *tr = e->GetComponent<TransformComponent>()) {
      tr->position = {inkPos_.x, WaterHeight(inkPos_.x, inkPos_.z) + 0.06f, inkPos_.z};
      tr->rotation = {0.0f, 0.0f, 0.0f};
      tr->scale = {r, 0.02f, r};
      TitleOctopusDetail::SyncRender(*e);
    }
    if (auto *pm = e->GetComponent<PrimitiveMeshComponent>()) {
      pm->color.w = alpha;
      if (pm->meshHandle >= 0) {
        if (auto *mat = RC::GetPrimitiveMeshMaterialPtr(pm->meshHandle)) mat->color.w = alpha;
      }
    }
  }

  // ------------------------------------------------------------------
  // 姿勢
  // ------------------------------------------------------------------

  /// @brief 今の状態から胴・顔・目・足の姿勢を求めて書き込む
  /// @param dt 経過秒（伸び縮みの戻りに使う）
  void Pose(float dt) {
    using namespace PartPose;
    // 伸び縮み（squash_ > 0 で縦に伸びる / < 0 でつぶれる）はばねのように揺れながら 0 へ
    squashVel_ += (-60.0f * squash_ - 7.0f * squashVel_) * dt;
    squash_ += squashVel_ * dt;
    swim_ = (std::max)(0.0f, swim_ - dt * 1.5f);
    const float breath = 0.04f * std::sin(time_ * 2.2f);
    const float sq = std::clamp(squash_ + breath, -0.6f, 0.8f);
    const RC::Vector3 stretch = {1.0f - 0.35f * sq, 1.0f + 0.5f * sq, 1.0f - 0.35f * sq};

    // ---- 胴の位置と傾き ----
    RC::Vector3 origin{}; // 足の付け根の高さ（胴のローカル原点）
    Mat3 tilt = Identity();
    const TitleBlackboard::Letter *L = nullptr;
    const bool onLetterStates = (state_ == State::Climbing || state_ == State::Clinging || state_ == State::Leaving);
    if (onLetterStates) L = CurrentLetter();
    if (L) {
      RC::Vector3 attach, water, outward;
      LetterFrame(*L, attachLx_, attachLz_, outLx_, outLz_, attach, water, outward);
      // 驚いて飛び降りるときは外へ大きく跳ぶ
      const float jumpOut = fastLeave_ && state_ == State::Leaving ? 1.0f : 0.0f;
      water.x += outward.x * 0.8f * scale * jumpOut;
      water.z += outward.z * 0.8f * scale * jumpOut;
      const float c = TitleOctopusDetail::Smooth01(climb_);
      origin.x = TitleOctopusDetail::Lerp(water.x, attach.x, c);
      origin.z = TitleOctopusDetail::Lerp(water.z, attach.z, c);
      const float lowY = water.y - 0.15f * scale;
      const float highY = LetterTopAt(*L, origin.x, origin.z) + 0.04f * scale;
      origin.y = TitleOctopusDetail::Lerp(lowY, highY, c) + (0.5f + 0.6f * jumpOut) * scale * std::sin(kPi * c);
      pos_ = {origin.x, 0.0f, origin.z};
      // 文字の上では文字と同じ傾きに乗る（水中側では水平）
      const float a = (L->rotation.x - kHalfPi) * c;
      const float b = L->rotation.z * c;
      tilt = MatMul(RotX(a), RotZ(b));
    } else {
      origin = {pos_.x, WaterHeight(pos_.x, pos_.z) + 0.05f * scale - sink_ * scale, pos_.z};
    }
    const Mat3 body = MatMul(BodyMatrix(yaw_, 0.0f, 0.0f), tilt);

    // ---- 胴（後ろへ持ち上がった楕円球）・顔・目・黒目 ----
    auto at = [&](const RC::Vector3 &local) {
      const RC::Vector3 l = {local.x * scale * stretch.x, local.y * scale * stretch.y, local.z * scale * stretch.z};
      return AddV(origin, Apply(l, body));
    };
    // RotX(+) で後ろ（-Z）側が持ち上がる
    PlacePart(parts_[kMantle], at({0.0f, 0.32f, -0.22f}), MatMul(RotX(0.55f), body), stretch);
    PlacePart(parts_[kFace], at({0.0f, 0.14f, 0.12f}), body, stretch);
    const float px = std::clamp(lookX_, -1.0f, 1.0f) * 0.035f;
    const float pz = std::clamp(lookZ_, -1.0f, 1.0f) * 0.035f;
    for (int side = 0; side < 2; ++side) {
      const float sx = (side == 0) ? -0.17f : 0.17f;
      PlacePart(parts_[side == 0 ? kEyeL : kEyeR], at({sx, 0.36f, 0.27f}), body, stretch);
      PlacePart(parts_[side == 0 ? kPupilL : kPupilR], at({sx + px, 0.45f, 0.29f + pz}), body, stretch);
    }

    // ---- 足：付け根から放射状に。うねりは先へ行くほど大きい ----
    // 水面にいるときは水面に浮かせ、文字の上では文字の上面に沿わせる（縁から外は水面へ垂れる）
    const float spread = 1.0f - 0.35f * (std::max)(sq, 0.0f) + 0.25f * (std::max)(-sq, 0.0f); // 伸びると足をすぼめる
    const float wave = tentacleWave * (1.0f + 1.2f * swim_);
    const float rootR = 0.28f * scale;
    const float spacing = 0.17f * scale * spread;
    for (int t = 0; t < kTentacles; ++t) {
      float ang = yaw_ + (static_cast<float>(t) + 0.5f) * (kTwoPi / static_cast<float>(kTentacles));
      float x = origin.x + std::sin(ang) * rootR;
      float z = origin.z + std::cos(ang) * rootR;
      for (int k = 0; k < kSegments; ++k) {
        const float kk = static_cast<float>(k) / static_cast<float>(kSegments - 1);
        ang += wave * std::sin(time_ * 2.3f + static_cast<float>(t) * 0.9f - static_cast<float>(k) * 0.7f) * (0.3f + kk);
        if (k > 0) {
          x += std::sin(ang) * spacing;
          z += std::cos(ang) * spacing;
        }
        const Part &p = parts_[kTentacleStart + t * kSegments + k];
        const float r = p.size.x * scale;
        float y = 0.0f;
        bool onTop = false;
        if (L && climb_ > 0.5f) {
          RC::Vector3 n{};
          float depth = 0.0f;
          onTop = TitleBlackboard::CircleVsLetter(*L, x, z, 0.02f, n, depth);
        }
        if (onTop) {
          y = LetterTopAt(*L, x, z) + r * 0.6f;
        } else {
          y = WaterHeight(x, z) + 0.02f - (L ? 0.0f : sink_ * scale);
          // 付け根の 2 つは胴の高さから水面へなめらかにつなぐ（文字の上から垂れ下がる見え方）
          if (k < 2) y = TitleOctopusDetail::Lerp(origin.y + r * 0.5f, y, 0.5f * static_cast<float>(k + 1));
        }
        PlacePart(p, {x, y, z}, BodyMatrix(ang, 0.0f, 0.0f), {1.0f, 1.0f, 1.0f});
      }
    }
  }

  // ------------------------------------------------------------------
  // デバッグ（ImGui のボタン）
  // ------------------------------------------------------------------

  /// @brief ImGui のボタンで頼まれた動きをその場で行う
  /// @param scene 対象のシーン
  void HandleDebugRequest(Scene *scene) {
    const int req = debugRequest_;
    debugRequest_ = 0;
    if (req == 3) {
      if (state_ == State::Peeking || state_ == State::Clinging || state_ == State::Climbing) {
        Startle();
        debugNote_ = "startled";
      } else {
        debugNote_ = "not visible";
      }
      return;
    }
    if (state_ != State::Hidden) {
      debugNote_ = "busy (wait until hidden)";
      return;
    }
    const bool ok = (req == 1) ? TryStartPeek(scene) : TryStartCling(scene);
    debugNote_ = ok ? "started" : "no place (letters not ready / camera moving?)";
  }

  // ------------------------------------------------------------------
  // 状態
  // ------------------------------------------------------------------

  std::vector<Part> parts_;
  std::weak_ptr<Entity> ink_;
  std::weak_ptr<Entity> folder_;
  bool visible_ = true; ///< SetVisible の現在値（最初の SetVisible(false) で全部を隠すため true から始める）

  State state_ = State::Hidden;
  float stateT_ = 0.0f;
  float stateDur_ = 0.0f;
  float timer_ = 3.0f;
  float time_ = 0.0f;
  std::mt19937 rng_{20261014u};

  // 水面
  RC::WaterWaveParams water_;
  bool hasWater_ = false;
  float waterY_ = 0.0f;
  float waterTime_ = 0.0f;

  // 胴
  RC::Vector3 pos_{};      ///< 水面にいるときの位置（xz）
  float yaw_ = 0.0f;       ///< 顔の向き（rad）
  float baseYaw_ = 0.0f;   ///< 見回すときの基準の向き
  float sink_ = 1.8f;      ///< 水面からの深さ（m、scale 1。0 で水面）
  bool breached_ = false;  ///< 浮かび上がりで水面を抜けたか
  float squash_ = 0.0f;    ///< 伸び縮み（正で縦に伸びる）
  float squashVel_ = 0.0f;
  float swim_ = 0.0f;      ///< 泳いでいる強さ（足のうねりを大きくする）
  float lookX_ = 0.0f;     ///< 黒目の向き（胴のローカル X）
  float lookZ_ = 1.0f;     ///< 黒目の向き（胴のローカル Z）
  float cursorStill_ = 0.0f; ///< カーソルが近くでじっとしている秒数
  float rippleTimer_ = 0.0f;
  bool fleeing_ = false;     ///< 驚いて逃げている
  RC::Vector3 fleeDir_{0.0f, 0.0f, 1.0f};

  // しがみつき
  int letter_ = -1;          ///< しがみついている文字の番号（TitleBlackboard::letters の添字）
  float attachLx_ = 0.0f;    ///< しがみつく点（文字のローカル X）
  float attachLz_ = 0.0f;    ///< しがみつく点（文字のローカル Z）
  float outLx_ = 0.0f;       ///< 外向き（文字のローカル X）
  float outLz_ = -1.0f;      ///< 外向き（文字のローカル Z）
  float climb_ = 0.0f;       ///< 0: 縁の外の水面 / 1: 文字の上
  float leaveFrom_ = 1.0f;   ///< 降り始めたときの climb_
  bool fastLeave_ = false;   ///< 驚いて飛び降りている

  // 墨
  bool inkActive_ = false;
  float inkT_ = 0.0f;
  RC::Vector3 inkPos_{};

  // デバッグ
  int debugRequest_ = 0;     ///< 1: Peek / 2: Cling / 3: Startle
  std::string debugNote_;
};

REGISTER_SCRIPT(TitleOctopusScript)
