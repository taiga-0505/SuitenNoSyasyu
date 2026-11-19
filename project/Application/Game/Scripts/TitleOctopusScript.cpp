#include "ECS/ScriptableEntity.h"
#include "ECS/ScriptRegistry.h"
#include "ECS/TransformComponent.h"
#include "ECS/CameraComponent.h"
#include "ECS/PrimitiveMeshComponent.h"
#include "ECS/ModelRendererComponent.h"
#include "ECS/AnimationComponent.h"
#include "ECS/WaterComponent.h"
#include "Common/Water/WaterSurface.h"
#include "Common/Log/Log.h"
#include "Render/Systems/RenderInteractiveWater.h"
#include "RenderCommon.h"
#include "Engine/Render/RenderContext.h"
#include "Framework/App.h"
#include "Scene.h"
#include "Game/Framework/PartPose.h"
#include "Game/Framework/StateMachine.h"
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
//   - 見た目は modelPath（既定 Resources/model/octopus/octopus.glb）のスキン付きモデル 1 体。
//     胴・顔・目・黒目・足の節（8 本 × 6 節）はモデルの骨（head / mantle / eye_* / pupil_* / leg{t}_{k}）で、
//     下のふるまいで求めた各部位の姿勢を RC::SetModelJointOverride で骨へ書き込む（足は文字の形に沿わせるため
//     クリップではなくスクリプトで決める）。modelPath を空にすると以前のプリミティブの組み合わせに戻る：
//       胴（後ろに伸びた楕円球）＋ 顔 ＋ 目 2 つ ＋ 黒目 2 つ ＋ 足 8 本（球を 6 つずつ連ねる）。
//     墨はエンティティを作らず OnRender3D で水面より後に半透明で描く（水面の泡判定に巻き込まれないように）
//     真上から見下ろすカメラなので、足は水面（または文字の上面）に放射状に広がる形で見せる。
//   - ふるまい（状態遷移）：
//       Hidden   … 水中に隠れている（描かない）。interval 秒ごとに「顔を出す」か「文字にしがみつく」を選ぶ
//       Rising   … 深い所から影が近づき、泡の帯を一気に抜けて水面へ顔を出す（飛沫・波紋）
//       Peeking  … 水面に浮いてあたりを見回す。カーソルが近ければ目で追い、
//                   近くでじっとしていると寄ってくる。素早く近づける・クリックすると驚く
//       Climbing … 文字の画（縁取り込みの形。TitleBlackboard::LetterMask）の縁の水面から、画の上へよじ登る
//       Clinging … 文字にしがみつく（文字の浮き沈み・傾き・押しのけにそのまま乗る）。
//                   足は画の上に貼り付け、画から外れそうなら画に沿って曲げ、外れた足は縁から水面へ垂らす
//       Leaving  … 文字から水へ滑り降りる（驚いたときは墨を吐いて飛び降りる）
//       Diving   … 水面で身をすくめ、泡の帯（水面シェーダが水面すぐ下の物を白く塗る深さ）を一気に抜けて、
//                   その下を外套膜を先に・足を束ねてジェット推進で泳ぎ去る
//     驚くと墨（黒い円盤が広がって薄れる）を吐き、急いで潜る。
//   - 飛び込み・浮上・起動演出の最中（TitleBlackboard::State::LettersInteractive() が false）は潜って隠れる。
//   - エンティティは最初の数十フレームで少しずつ作り（1 フレームに作りすぎると止まる）、
//     出番が来たら SetActive で表示を切り替えて使い回す。"transient" タグ付きでシーン保存に残らない。
//   - 編集モード（再生していない）では何もしない。
//   - 上のふるまいは State パターン（StateMachine.h）で 1 状態 = 1 クラスに分けている。
//     以前は enum + switch で、「潜れ」と言われたときの反応（文字の上なら先に滑り降りる、
//     潜っている最中なら無視する…）も状態を if で並べて判定していた。状態ごとに違う反応は
//     状態クラスの仮想関数（OnDiveRequested / IsOnLetter など）にし、次の状態も状態クラス自身が決める。
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
  /// @brief 状態を「隠れている」で始める（OnCreate 前にインスペクタへ出しても状態名が出るように）
  TitleOctopusScript();

private:
  // 設定値はシーン JSON（Serialize / Deserialize）とインスペクタ（OnImGui）からだけ触るので外へは公開しない
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
  // ---- 水中 ----
  /// 水面シェーダが「岸辺の泡」で白く塗る深さ（m。RenderWater の foamParams.x と同じ値にする）。
  /// 水面すぐ下の物は泡で白い塊に見えるので、潜るときはこの帯を一瞬で抜け、これより深い所を泳ぐ
  float foamDepth = 2.0f;
  float swimSpeed = 3.2f;          ///< 驚いて水中を逃げるときの速さ（m/s。ジェット推進の平均）
  /// @brief タコのモデル（空ならプリミティブの組み合わせで代用）。色はモデルのテクスチャで決まる
  std::string modelPath = "Resources/model/octopus/octopus.glb";
  // 以下の色はプリミティブで代用するときだけ使う
  RC::Vector4 bodyColor = {0.93f, 0.40f, 0.33f, 1.0f};
  RC::Vector4 eyeColor = {0.98f, 0.98f, 0.96f, 1.0f};
  RC::Vector4 pupilColor = {0.05f, 0.05f, 0.07f, 1.0f};
  // ---- 墨 ----
  bool inkEnabled = true;
  float inkSize = 2.6f;            ///< 広がりきったときの半径（m）
  float inkLife = 2.5f;            ///< 消えるまでの秒数
  RC::Vector4 inkColor = {0.04f, 0.03f, 0.07f, 0.85f};

public:
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
    j["foamDepth"] = foamDepth;
    j["swimSpeed"] = swimSpeed;
    j["modelPath"] = modelPath;
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
    ReadF(j, "foamDepth", foamDepth);
    ReadF(j, "swimSpeed", swimSpeed);
    if (j.contains("modelPath") && j["modelPath"].is_string()) modelPath = j["modelPath"].get<std::string>();
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
    const OctopusState *cur = CurrentState();
    ImGui::Text("State: %s  (%.1fs)  parts %d / %d", fsm_.CurrentName(), cur ? cur->Elapsed() : 0.0f,
                static_cast<int>(parts_.size()), kPartCount);
    ImGui::TextDisabled("Model: %s", modelPath.empty() ? "(primitives)" : modelPath.c_str());
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
    ImGui::SeparatorText("Underwater");
    ImGui::DragFloat("Foam Depth", &foamDepth, 0.05f, 0.0f, 10.0f);
    ImGui::DragFloat("Swim Speed", &swimSpeed, 0.05f, 0.0f, 10.0f);
    ImGui::ColorEdit4("Body", &bodyColor.x);
    ImGui::ColorEdit4("Eye", &eyeColor.x);
    ImGui::ColorEdit4("Pupil", &pupilColor.x);
    ImGui::SeparatorText("Ink");
    ImGui::Checkbox("Ink", &inkEnabled);
    ImGui::DragFloat("Ink Size", &inkSize, 0.05f, 0.2f, 10.0f);
    ImGui::DragFloat("Ink Life", &inkLife, 0.05f, 0.2f, 10.0f);
    ImGui::ColorEdit4("Ink Color", &inkColor.x);
    ImGui::TextDisabled("colors are for primitives only / applied on scene reload");
  }
#endif

protected:
  /// @brief 乱数と最初の待ち時間を初期化する（部位は再生中の最初の Update から作る）
  void OnCreate() override {
    std::random_device rd;
    rng_.seed(rd());
    timer_ = firstDelay;
    fsm_.ChangeState<HiddenState>();
    fsm_.ApplyPending(*this);
    // 墨：単位立方体を薄く潰して丸い透かしテクスチャを貼り、水面より後に半透明で描く（OnRender3D）
    inkModel_ = RC::LoadModel(kInkModelPath);
    inkTex_ = RC::LoadTex(kInkTexPath);
    inkModelUnlit_ = false;
  }

  /// @brief 墨を 3D パスに積む（水面より後の半透明レイヤー。水面の泡判定に巻き込まれない）
  /// @details 以前は墨を水面すれすれの円柱エンティティで描いていたが、不透明で深度を書くため
  ///          水面シェーダが「岸辺の泡」と判定して円柱の範囲が白く塗られていた（白い丸の正体）。
  void OnRender3D() override {
    if (!inkActive_ || inkModel_ < 0 || !RC::IsModelReady(inkModel_)) return;
    if (!inkModelUnlit_) {
      RC::SetModelLightingMode(inkModel_, LightingMode::None);
      inkModelUnlit_ = true;
    }
    const float life = (std::max)(inkLife, 0.1f);
    const float t = std::clamp(inkT_ / life, 0.0f, 1.0f);
    const float fade = (1.0f - t) * (1.0f - t);
    // 濃い芯と薄い外側の 2 段（インスタンス描画は 1 回に 1 色なので分ける）
    inkCore_.clear();
    inkOuter_.clear();
    for (size_t i = 0; i < inkPuffs_.size(); ++i) {
      const InkPuff &p = inkPuffs_[i];
      const float u = std::clamp((inkT_ - p.delay) / (std::max)(life - p.delay, 0.1f), 0.0f, 1.0f);
      if (inkT_ < p.delay) continue;
      const float grow = 1.0f - (1.0f - u) * (1.0f - u) * (1.0f - u); // 素早く広がって止まる
      const float r = TitleOctopusDetail::Lerp(p.r0, p.r1, grow);
      // テクスチャの円（α 0.5 で切り抜き）は四角の幅の約 0.66 倍なので、半径 r の円になるよう広げる
      const float w = r * 2.0f / 0.66f;
      Transform tr;
      tr.scale = {w, 0.004f, w};
      tr.rotation = {0.0f, p.spin + 0.3f * u, 0.0f};
      tr.translation = {p.pos.x + p.vel.x * grow, waterY_ + 0.04f + 0.003f * static_cast<float>(i),
                        p.pos.z + p.vel.z * grow};
      (p.core ? inkCore_ : inkOuter_).push_back(tr);
    }
    RC::Vector4 c = inkColor;
    c.w = (std::min)(inkColor.w * fade, 0.99f); // α = 1 だと不透明の PSO で描かれる
    if (c.w > 0.01f && !inkCore_.empty()) RC::DrawModelBatchColored(inkModel_, inkCore_, c, inkTex_, true);
    c.w = (std::min)(inkColor.w * 0.55f * fade, 0.99f);
    if (c.w > 0.01f && !inkOuter_.empty()) RC::DrawModelBatchColored(inkModel_, inkOuter_, c, inkTex_, true);
  }

  /// @brief 再生中だけ、部位を少しずつ作り、状態を進めて姿勢を書く
  /// @param deltaTime 前フレームからの経過秒
  void OnUpdate(float deltaTime) override {
    SceneContext *ctx = GetSceneContext();
    if (!ctx || !ctx->isPlaying()) return;
    Scene *scene = GetScene();
    if (!scene) return;
    BuildSome(scene);
    if (static_cast<int>(parts_.size()) < kPartCount) return;

    const float dt = (std::max)(deltaTime, 0.0f);
    time_ += dt;
    hasWater_ = BuildWaterParams(scene, water_, waterY_);
    waterTime_ = RC::GetWaterTime();
    UpdateInk(dt);

    const TitleBlackboard::State &bb = TitleBlackboard::Get();
    if (debugRequest_ != 0) HandleDebugRequest(scene);
    // 状態の外（デバッグボタン・下の強制潜行）から予約した遷移はすぐに切り替え、
    // 続く判定と状態の更新が新しい状態を見るようにする（以前の「その場で state_ を書き換える」と同じ順序）
    fsm_.ApplyPending(*this);

    // 無効にされた・文字に触れられない（起動演出・飛び込み・浮上の最中）なら潜って隠れる
    // （隠れている・潜っている最中なら、その状態の OnDiveRequested が何もしない）
    const bool allowed = enabled && bb.LettersInteractive();
    allowed_ = allowed;
    if (!allowed) RequestDive(fsm_, true);
    fsm_.ApplyPending(*this);

    // 今の状態の OnUpdate をポリモーフィズムで呼ぶ（次の状態は状態クラス自身が決める）
    fsm_.Update(*this, dt);
    if (IsShown()) Pose(dt);
  }

  /// @brief 作った部位とフォルダを破棄する
  void OnDestroy() override {
    for (auto &p : parts_) {
      if (auto e = p.entity.lock()) e->Destroy();
    }
    parts_.clear();
    if (auto m = model_.lock()) m->Destroy();
    model_.reset();
    if (inkModel_ >= 0) RC::UnloadModel(inkModel_);
    inkModel_ = -1;
    inkActive_ = false;
    if (auto f = folder_.lock()) f->Destroy();
    folder_.reset();
  }

private:
  using Mat3 = PartPose::Mat3;
  using Machine = StateMachine<TitleOctopusScript>; ///< ふるまいのステートマシン

  /// @brief 部位 1 つ（エンティティと、胴に対する置き方）
  /// @details モデルで描くときは entity を持たず、joint（モデルの骨）へ姿勢を書き込む。
  struct Part {
    std::weak_ptr<Entity> entity;
    RC::Vector3 size{1, 1, 1}; ///< scale 1 のときの Transform.scale
    std::string joint;         ///< モデルの骨の名前（モデルで描くとき）
    Mat3 restRot = PartPose::Identity(); ///< モデルの rest でこの部位が向いている向き（胴のローカル）
    bool leg = false;          ///< 足の節か（足は伸び縮みの scale を骨に掛けない）
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
  static constexpr float kRiseDeepTime = 0.9f; ///< 深い所から泡の帯の手前まで上がってくる秒数（影が濃くなる）
  static constexpr float kRisePopTime = 0.22f; ///< 泡の帯を抜けて水面へ顔を出す秒数
  static constexpr float kRiseExtra = 1.2f;    ///< 浮かび始めの深さ（泡の帯の下端からさらに何 m 深いか）
  static constexpr float kClimbTime = 0.8f;    ///< よじ登る秒数
  static constexpr float kLeaveTime = 0.7f;    ///< 滑り降りる秒数
  static constexpr float kPlungeTime = 0.22f;  ///< 水面で身をすくめてから潜り込むまでの秒数
  static constexpr float kSwimTime = 1.5f;     ///< 潜ってから深い所を泳いで見えなくなるまでの秒数
  static constexpr float kSwimSink = 1.6f;     ///< 泳ぎながらさらに沈む深さ（m。深いほど水の色に溶ける）
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
    for (const auto &L : bb.Letters()) {
      if (L.visible && TitleBlackboard::CircleVsLetter(L, x, z, r, n, depth)) return true;
    }
    for (const auto &L : bb.Menu()) {
      if (L.visible && TitleBlackboard::CircleVsLetter(L, x, z, r, n, depth)) return true;
    }
    return false;
  }

  // ------------------------------------------------------------------
  // 部位の生成
  // ------------------------------------------------------------------

  /// @brief 部位を kBuildPerFrame 個ずつ作る（フォルダは最初に 1 つ）
  /// @details モデルで描くときはモデルのエンティティを 1 つ作り、部位は骨の名前などのデータだけなので一度に揃える。
  /// @param scene 対象のシーン
  void BuildSome(Scene *scene) {
    if (static_cast<int>(parts_.size()) >= kPartCount) return;
    if (!folder_.lock()) {
      auto folder = scene->CreateEntity("TitleOctopus_Parts");
      if (folder) {
        folder->SetIsFolder(true);
        folder->SetTag("transient", 1);
        folder_ = folder;
      }
    }
    const bool useModel = !modelPath.empty();
    if (useModel && !model_.lock()) model_ = MakeModel(scene);
    const RC::Vector4 skinDark = {bodyColor.x * 0.85f, bodyColor.y * 0.8f, bodyColor.z * 0.8f, bodyColor.w};
    const int perFrame = useModel ? kPartCount : kBuildPerFrame;
    for (int n = 0; n < perFrame; ++n) {
      const int i = static_cast<int>(parts_.size());
      if (i >= kPartCount) return;
      Part p;
      std::shared_ptr<Entity> e;
      // 大きさは scale 1 のとき（球は半径、円柱は半径 × 高さ）。+Z が顔の前、+Y が上
      // joint / restRot は octopus.glb の骨（scripts/modelgen/models.py の build_octopus と対応）
      // ※ この switch は部位ごとの「データ（大きさ・骨の名前・色）」を埋めるだけで、振る舞いは分岐しない。
      //   作った後の部位はすべて同じ PlacePart で扱うので、部位ごとの派生クラスにはせずデータで表す。
      switch (i) {
      case kMantle:
        if (!useModel) e = MakePrimitive(scene, "TitleOctopus_Mantle", PrimitiveType::Sphere, bodyColor);
        p.size = {0.46f, 0.40f, 0.58f};
        p.joint = "mantle";
        p.restRot = PartPose::RotX(0.55f); // モデルの外套膜は後ろを持ち上げた形で作ってある
        break;
      case kFace:
        if (!useModel) e = MakePrimitive(scene, "TitleOctopus_Face", PrimitiveType::Sphere, bodyColor);
        p.size = {0.42f, 0.36f, 0.40f};
        p.joint = "head";
        break;
      case kEyeL:
      case kEyeR:
        if (!useModel) e = MakePrimitive(scene, "TitleOctopus_Eye", PrimitiveType::Sphere, eyeColor);
        p.size = {0.12f, 0.11f, 0.12f};
        p.joint = (i == kEyeL) ? "eye_L" : "eye_R";
        break;
      case kPupilL:
      case kPupilR:
        if (!useModel) e = MakePrimitive(scene, "TitleOctopus_Pupil", PrimitiveType::Sphere, pupilColor);
        p.size = {0.06f, 0.05f, 0.07f};
        p.joint = (i == kPupilL) ? "pupil_L" : "pupil_R";
        break;
      default: {
        // 足：根元から先へ細くなる球。1 本おきに少し色を変えて足の区切りを見せる
        const int k = (i - kTentacleStart) % kSegments;
        const int t = (i - kTentacleStart) / kSegments;
        const float r = 0.15f - 0.018f * static_cast<float>(k);
        if (!useModel) {
          e = MakePrimitive(scene, "TitleOctopus_Leg", PrimitiveType::Sphere, (t % 2 == 0) ? bodyColor : skinDark);
        }
        p.size = {r, r * 0.75f, r};
        p.joint = "leg" + std::to_string(t) + "_" + std::to_string(k);
        // rest の足 t は +Z から (t + 0.5) × 45° 回した向きへ水平に伸びている
        p.restRot = PartPose::RotY((static_cast<float>(t) + 0.5f) * (PartPose::kTwoPi / static_cast<float>(kTentacles)));
        p.leg = true;
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

  /// @brief タコのモデルのエンティティを作る（非アクティブ・画面外で作る）
  /// @details 骨の姿勢はスクリプトが毎フレーム上書きするが、上書きはアニメーションの更新の中で反映されるので
  ///          待機のクリップを 1 本再生しておく（敵のタコと同じクリップ。上書きしない骨は無いので見た目には出ない）。
  /// @param scene 対象のシーン
  /// @return 作ったエンティティ（失敗したら nullptr）
  std::shared_ptr<Entity> MakeModel(Scene *scene) {
    auto e = scene->CreateEntity("TitleOctopus_Model");
    if (!e) return nullptr;
    if (auto f = folder_.lock()) e->SetParentGuid(f->Guid());
    e->SetTag("transient", 1); // シーン保存に含めない
    auto &tr = e->AddComponent<TransformComponent>();
    tr.position = {0.0f, -500.0f, 0.0f};
    tr.scale = {scale, scale, scale};
    auto &mr = e->AddComponent<ModelRendererComponent>();
    mr.modelPath = modelPath;
    auto &anim = e->AddComponent<AnimationComponent>();
    anim.clips = {{"Idle", "", 0, true, 1.0f}};
    anim.defaultClip = "Idle";
    scene->InitDynamicEntityRuntime(*e);
    e->SetActive(false);
    return e;
  }

  /// @brief 部位の表示・非表示をまとめて切り替える
  /// @param visible 表示するなら true
  void SetVisible(bool visible) {
    if (visible_ == visible) return;
    visible_ = visible;
    if (visible) legSnap_ = true; // 前回消えた場所から足が伸びてこないように
    for (auto &p : parts_) {
      if (auto e = p.entity.lock()) e->SetActive(visible);
    }
    if (auto m = model_.lock()) m->SetActive(visible);
  }

  /// @brief モデルの置き場所（胴の原点と向き）を決める。部位の姿勢はこれを基準に骨へ書く
  /// @param origin 足の付け根の高さの胴の原点（ワールド）
  /// @param body 胴の向き（ワールド）
  void PlaceModel(const RC::Vector3 &origin, const Mat3 &body) {
    modelOrigin_ = origin;
    modelBody_ = body;
    auto m = model_.lock();
    if (!m) return;
    auto *tr = m->GetComponent<TransformComponent>();
    if (!tr) return;
    tr->position = origin;
    tr->rotation = PartPose::ToEngineEuler(body);
    tr->scale = {scale, scale, scale};
    // 描画側へ即座に写す（Transform の同期は次フレーム冒頭なので、そのままだと 1 フレーム遅れる）
    if (auto *mr = m->GetComponent<ModelRendererComponent>()) {
      if (mr->HasModel()) {
        if (auto *mt = RC::GetModelTransformPtr(mr->modelHandle)) *mt = tr->ToTransform();
      }
    }
  }

  /// @brief 骨へ書いた姿勢を、このフレームのうちに描画へ反映させる（スケルトンを計算し直す）
  void FlushModelPose() {
    auto m = model_.lock();
    if (!m) return;
    if (auto *mr = m->GetComponent<ModelRendererComponent>()) {
      if (mr->HasModel() && RC::IsModelReady(mr->modelHandle)) RC::UpdateModelAnimation(mr->modelHandle, 0.0f);
    }
  }

  /// @brief 部位 1 つへワールド姿勢を書き込む
  /// @details モデルで描くときは、PlaceModel で決めた胴の座標へ直して骨の姿勢として書く。
  ///          骨はすべて root 直下（root は単位）なので、骨の姿勢＝モデル空間の姿勢。
  /// @param p 部位
  /// @param pos ワールド位置
  /// @param rot ワールド回転
  /// @param scl 大きさの倍率（size × scale にさらに掛ける。伸び縮み用）
  void PlacePart(const Part &p, const RC::Vector3 &pos, const Mat3 &rot, const RC::Vector3 &scl) {
    if (auto m = model_.lock()) {
      auto *mr = m->GetComponent<ModelRendererComponent>();
      if (!mr || !mr->HasModel()) return;
      const Mat3 inv = PartPose::Transpose(modelBody_);
      const float s = (std::max)(scale, 1e-3f);
      const RC::Vector3 d = {pos.x - modelOrigin_.x, pos.y - modelOrigin_.y, pos.z - modelOrigin_.z};
      const RC::Vector3 local = PartPose::ScaleV(PartPose::Apply(d, inv), 1.0f / s);
      // rest の向きを打ち消してから、胴の中での向きにする（行ベクトル規約なので左から順に効く）
      const Mat3 jointRot = PartPose::MatMul(PartPose::Transpose(p.restRot), PartPose::MatMul(rot, inv));
      const RC::Vector3 jointScale = p.leg ? RC::Vector3{1.0f, 1.0f, 1.0f} : scl;
      RC::SetModelJointOverride(mr->modelHandle, p.joint, local, PartPose::ToQuaternion(jointRot), jointScale);
      return;
    }
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
  // 出現（Hidden から次の状態へ）
  // ------------------------------------------------------------------

  /// @brief 文字・メニューの無い水面を選んで浮かび上がり始める（Rising へ）
  /// @param machine 遷移を予約するステートマシン
  /// @param scene 対象のシーン
  /// @return 始められたら true
  bool TryStartPeek(Machine &machine, Scene *scene) {
    float cx, cz, hw, hh;
    if (!ViewRect(scene, cx, cz, hw, hh)) return false;
    const float margin = 2.5f * scale + 1.0f;
    const TitleBlackboard::State &bb = TitleBlackboard::Get();
    for (int tries = 0; tries < 24; ++tries) {
      const float x = cx + Rand(-1.0f, 1.0f) * (std::max)(hw - margin, 0.5f);
      const float z = cz + Rand(-1.0f, 1.0f) * (std::max)(hh - margin, 0.5f);
      if (NearLetters(x, z, 1.6f * scale)) continue;
      // カーソルの真下には出ない（出た瞬間に驚いてしまう）
      if (bb.CursorValid()) {
        const float dx = x - bb.CursorPos().x, dz = z - bb.CursorPos().z;
        if (dx * dx + dz * dz < (startleRadius + 1.5f) * (startleRadius + 1.5f)) continue;
      }
      pos_ = {x, 0.0f, z};
      baseYaw_ = Rand(-PartPose::kPi, PartPose::kPi);
      yaw_ = baseYaw_;
      sink_ = DeepSink() + kRiseExtra / scale; // 泡の帯より深い所から（水越しにぼんやり見える）
      deep_ = true;
      jet_ = 0.6f;
      cursorStill_ = 0.0f;
      SetVisible(true);
      machine.ChangeState<RisingState>();
      return true;
    }
    return false;
  }

  /// @brief しがみつく文字と縁を選び、縁の外の水面からよじ登り始める（Climbing へ）
  /// @param machine 遷移を予約するステートマシン
  /// @param scene 対象のシーン
  /// @return 始められたら true
  bool TryStartCling(Machine &machine, Scene *scene) {
    const TitleBlackboard::State &bb = TitleBlackboard::Get();
    if (bb.Letters().empty()) return false;
    float cx, cz, hw, hh;
    if (!ViewRect(scene, cx, cz, hw, hh)) return false;
    std::vector<int> candidates;
    for (int i = 0; i < static_cast<int>(bb.Letters().size()); ++i) {
      if (bb.Letters()[i].visible) candidates.push_back(i);
    }
    if (candidates.empty()) return false;
    for (int tries = 0; tries < 16; ++tries) {
      std::uniform_int_distribution<size_t> pick(0, candidates.size() - 1);
      const int li = candidates[pick(rng_)];
      const TitleBlackboard::Letter &L = bb.Letters()[li];
      float lx = 0.0f, lz = 0.0f, ox = 0.0f, oz = 0.0f, outDist = 0.0f;
      if (!FindClingSpot(L, lx, lz, ox, oz, outDist)) continue;
      // 縁の外の水面が画面内で、他の文字・メニューにかぶらないか
      RC::Vector3 attach, water, outward;
      outDist_ = outDist;
      LetterFrame(L, lx, lz, ox, oz, attach, water, outward);
      if (std::fabs(water.x - cx) > hw - 1.0f || std::fabs(water.z - cz) > hh - 1.0f) continue;
      bool blocked = false;
      for (int k = 0; k < static_cast<int>(bb.Letters().size()) && !blocked; ++k) {
        if (k == li || !bb.Letters()[k].visible) continue;
        RC::Vector3 n{};
        float depth = 0.0f;
        blocked = TitleBlackboard::CircleVsLetter(bb.Letters()[k], water.x, water.z, 1.0f * scale, n, depth);
      }
      if (blocked) continue;
      letter_ = li;
      attachLx_ = lx;
      attachLz_ = lz;
      outLx_ = ox;
      outLz_ = oz;
      climb_ = 0.0f;
      sink_ = 0.0f; // 降りたあとは水面から潜る
      deep_ = false;
      jet_ = 0.0f;
      fastLeave_ = false;
      cursorStill_ = 0.0f;
      SetVisible(true);
      Splash(water.x, water.z, 0.6f);
      machine.ChangeState<ClimbingState>(kClimbTime);
      return true;
    }
    return false;
  }

  /// @brief 文字の形（距離場）から、しがみつく点と、そこから水面へ降りる向き・距離を選ぶ
  /// @param L 文字
  /// @param lx しがみつく点（ローカル X）の出力先
  /// @param lz しがみつく点（ローカル Z）の出力先
  /// @param ox 外向き（ローカル X）の出力先
  /// @param oz 外向き（ローカル Z）の出力先
  /// @param outDist しがみつく点 → 縁の外の水面の点 の距離（m）の出力先
  /// @return 胴が乗れる太さの画（縁取り込み）が見つかれば true
  /// @details 文字の外（下・左・右・上のどれか）から内へまっすぐ進み、最初に当たった画の中で
  ///          胴の幅ぶん内側に入れる所（無ければその画のいちばん太い所）を選ぶ。
  ///          降りる向きは距離場の勾配（いちばん近い縁へ向かう向き）にする。
  bool FindClingSpot(const TitleBlackboard::Letter &L, float &lx, float &lz, float &ox, float &oz,
                     float &outDist) {
    using TitleBlackboard::LetterDistanceLocal;
    // 縁：0 下 / 1 左 / 2 右 / 3 上（文字のローカル。下は画面の下側）。上は少なめ
    const float e = Rand01();
    const int edge = e < 0.4f ? 0 : e < 0.65f ? 1 : e < 0.9f ? 2 : 3;
    const float margin = 1.5f; // 縁取りぶん外から始める
    float px = 0.0f, pz = 0.0f, dx = 0.0f, dz = 0.0f, span = 0.0f;
    switch (edge) {
    case 0: px = Rand(-0.7f, 0.7f) * L.halfX; pz = -L.halfZ - margin; dz = 1.0f; span = L.halfZ * 2.0f; break;
    case 1: px = -L.halfX - margin; pz = Rand(-0.7f, 0.7f) * L.halfZ; dx = 1.0f; span = L.halfX * 2.0f; break;
    case 2: px = L.halfX + margin; pz = Rand(-0.7f, 0.7f) * L.halfZ; dx = -1.0f; span = L.halfX * 2.0f; break;
    default: px = Rand(-0.7f, 0.7f) * L.halfX; pz = L.halfZ + margin; dz = -1.0f; span = L.halfZ * 2.0f; break;
    }
    const float need = 0.34f * scale; // 胴の半幅ぶん内側に入れれば、胴が縁からはみ出さない
    const float step = 0.04f;
    const int steps = static_cast<int>((span + margin * 2.0f) / step);
    bool hit = false;
    float best = 1e9f, bx = 0.0f, bz = 0.0f, inside = 0.0f;
    for (int i = 0; i <= steps; ++i) {
      const float d = LetterDistanceLocal(L, px, pz);
      if (d < 0.0f) {
        hit = true;
        inside += step;
        if (d < best) {
          best = d;
          bx = px;
          bz = pz;
        }
        if (d <= -need) break; // 十分に内側
      } else if (hit && inside > 0.1f) {
        break; // 最初の画を抜けた
      }
      px += dx * step;
      pz += dz * step;
    }
    if (!hit || best > -0.15f * scale) return false; // 細すぎる画（点など）には乗らない
    lx = bx;
    lz = bz;
    // 降りる向き：距離場の勾配（いちばん近い縁へ）
    const float g = 0.05f;
    float gx = LetterDistanceLocal(L, bx + g, bz) - LetterDistanceLocal(L, bx - g, bz);
    float gz = LetterDistanceLocal(L, bx, bz + g) - LetterDistanceLocal(L, bx, bz - g);
    float gl = std::sqrt(gx * gx + gz * gz);
    if (gl < 1e-5f) {
      gx = -dx;
      gz = -dz;
      gl = 1.0f;
    }
    ox = gx / gl;
    oz = gz / gl;
    // 縁を越えて、足が壁に埋まらない所まで出た距離 ＋ 胴の半幅
    float t = 0.0f;
    const float clear = 0.2f * scale + 0.08f;
    while (t < 4.0f && LetterDistanceLocal(L, bx + ox * t, bz + oz * t) < clear) t += step;
    outDist = t + 0.35f * scale;
    return true;
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
    const float out = outDist_; // しがみつく点 → 縁 → 水面の点（FindClingSpot が決める）
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

  /// @brief 泡の帯より下（ここなら水面シェーダに白く塗られない）を保つ深さ（sink_ の単位 = m / scale）
  /// @return 胴の付け根の深さ（scale 1 換算）
  float DeepSink() const {
    // 胴のいちばん上（外套膜）は付け根から 0.8 × scale ほど上。波の山（≒ 0.35m）でも泡の帯に入らないように
    return ((std::max)(foamDepth, 0.0f) + 0.35f + 0.8f * scale) / (std::max)(scale, 0.05f);
  }

  // ------------------------------------------------------------------
  // 潜る・降りる・驚く（複数の状態から呼ばれる共通の動作）
  // ------------------------------------------------------------------

  /// @brief 「潜れ」と頼む。どう応じるかは今の状態が決める（OctopusState::OnDiveRequested）
  /// @param machine 遷移を予約するステートマシン
  /// @param fast 驚いたとき（短い時間で潜る）
  /// @details 文字の上なら先に滑り降りる・降りている途中なら降りきってから潜る・隠れている／潜っている
  ///          最中なら何もしない、と状態ごとに反応が違う。以前は if で状態を並べていたのを仮想関数にした。
  void RequestDive(Machine &machine, bool fast) {
    if (OctopusState *s = machine.As<OctopusState>()) s->OnDiveRequested(*this, machine, fast);
  }

  /// @brief 水面（または浮かび上がる途中の深い所）から潜り始める（Diving へ）
  /// @param machine 遷移を予約するステートマシン
  /// @param fast 驚いたとき（短い時間で潜る）
  void StartDive(Machine &machine, bool fast) {
    Splash(pos_.x, pos_.z, fast ? 0.9f : 0.5f);
    float plungeDur = 0.0f; // 水面で身をすくめて潜り込むまでの秒数
    float diveFrom = 0.0f;  // 潜り始めたときの sink_（水面から潜るときだけ使う）
    float swimVel = 0.0f;   // 泳ぎ始めの速さ（深い所から泳ぎ去るときだけ使う）
    if (deep_) {
      // 浮かび上がる途中（まだ深い所）：そのまま泳ぎ去る
      plungeDur = 0.0f;
      swimVel = fast ? swimSpeed : swimSpeed * 0.25f;
    } else {
      plungeDur = kPlungeTime * (fast ? kFastFactor : 1.0f);
      diveFrom = sink_;
    }
    machine.ChangeState<DivingState>(plungeDur, diveFrom, swimVel);
  }

  /// @brief 文字から降り始める（Leaving へ）
  /// @param machine 遷移を予約するステートマシン
  /// @param fast 驚いたとき（飛び降りる）
  void BeginLeave(Machine &machine, bool fast) {
    fastLeave_ = fast;
    if (fast) squash_ = 0.7f;
    // 今の高さ（climb_）から降り始める（よじ登っている途中で降りることもある）
    const float leaveFrom = std::clamp(climb_, 0.0f, 1.0f);
    const float dur = kLeaveTime * (fast ? kFastFactor : 1.0f) * (std::max)(leaveFrom, 0.2f);
    machine.ChangeState<LeavingState>(dur, leaveFrom);
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
  /// @param machine 遷移を予約するステートマシン
  void Startle(Machine &machine) {
    const TitleBlackboard::State &bb = TitleBlackboard::Get();
    Ink(pos_.x, pos_.z);
    squash_ = 0.7f;
    fleeing_ = true;
    float dx = pos_.x - bb.CursorPos().x, dz = pos_.z - bb.CursorPos().z;
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
    // 文字の上（Climbing / Clinging）なら飛び降り、水面なら潜る。どちらになるかは今の状態が決める
    RequestDive(machine, true);
  }

  /// @brief しがみついている文字（無ければ・見えなければ nullptr）
  /// @return 文字
  const TitleBlackboard::Letter *CurrentLetter() const {
    const TitleBlackboard::State &bb = TitleBlackboard::Get();
    if (letter_ < 0 || letter_ >= static_cast<int>(bb.Letters().size())) return nullptr;
    const TitleBlackboard::Letter &L = bb.Letters()[letter_];
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
    if (bb.CursorValid()) {
      const float dx = bb.CursorPos().x - attach.x, dz = bb.CursorPos().z - attach.z;
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

  /// @brief 墨を吐く（水面に黒い雲が、いくつかの塊に分かれてもくもくと広がって薄れる）
  /// @param x ワールド X
  /// @param z ワールド Z
  /// @details 墨は脅威（カーソル）の側へ吐きつけ、自分はその反対へ逃げる。
  void Ink(float x, float z) {
    Splash(x, z, 1.0f);
    if (!inkEnabled) return;
    inkT_ = 0.0f;
    inkActive_ = true;
    // 吐きつける向き（カーソルがあればそちら）
    float bx = 0.0f, bz = 0.0f;
    const TitleBlackboard::State &bb = TitleBlackboard::Get();
    if (bb.CursorValid()) {
      const float dx = bb.CursorPos().x - x, dz = bb.CursorPos().z - z;
      const float d = std::sqrt(dx * dx + dz * dz);
      if (d > 1e-3f) {
        bx = dx / d;
        bz = dz / d;
      }
    }
    const float size = (std::max)(inkSize, 0.3f) * scale;
    for (size_t i = 0; i < inkPuffs_.size(); ++i) {
      InkPuff &p = inkPuffs_[i];
      p.core = i < 3;
      const float ang = Rand(0.0f, PartPose::kTwoPi);
      const float off = Rand(0.0f, 0.25f) * scale;
      p.pos = {x + std::sin(ang) * off, 0.0f, z + std::cos(ang) * off};
      // 塊ごとに広がる向きと距離を変える（芯は近く、外側は遠くまで流れる）
      const float spread = (p.core ? Rand(0.05f, 0.25f) : Rand(0.25f, 0.55f)) * size;
      const float va = ang + Rand(-0.6f, 0.6f);
      p.vel = {std::sin(va) * spread + bx * size * 0.25f, 0.0f, std::cos(va) * spread + bz * size * 0.25f};
      p.r0 = Rand(0.15f, 0.3f) * scale;
      p.r1 = (p.core ? Rand(0.3f, 0.42f) : Rand(0.22f, 0.36f)) * size;
      p.delay = p.core ? Rand(0.0f, 0.08f) : Rand(0.05f, 0.35f);
      p.spin = Rand(0.0f, PartPose::kTwoPi);
    }
  }

  /// @brief 墨の時間を進める（描くのは OnRender3D）
  /// @param dt 経過秒
  void UpdateInk(float dt) {
    if (!inkActive_) return;
    inkT_ += dt;
    if (inkT_ >= (std::max)(inkLife, 0.1f)) inkActive_ = false;
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
    const OctopusState *curState = CurrentState();
    const bool onLetterStates = curState && curState->IsOnLetter(); // Climbing / Clinging / Leaving
    if (onLetterStates) L = CurrentLetter();
    if (L) {
      RC::Vector3 attach, water, outward;
      LetterFrame(*L, attachLx_, attachLz_, outLx_, outLz_, attach, water, outward);
      // 驚いて飛び降りるときは外へ大きく跳ぶ
      const float jumpOut = fastLeave_ && fsm_.IsIn<LeavingState>() ? 1.0f : 0.0f;
      water.x += outward.x * 0.8f * scale * jumpOut;
      water.z += outward.z * 0.8f * scale * jumpOut;
      const float c = TitleOctopusDetail::Smooth01(climb_);
      origin.x = TitleOctopusDetail::Lerp(water.x, attach.x, c);
      origin.z = TitleOctopusDetail::Lerp(water.z, attach.z, c);
      const float lowY = water.y - 0.15f * scale;
      // 顔（球）の下端が文字の上面に乗る高さ。低いと胴が文字に埋まって見える
      const float highY = LetterTopAt(*L, origin.x, origin.z) + 0.2f * scale;
      origin.y = TitleOctopusDetail::Lerp(lowY, highY, c) + (0.5f + 0.6f * jumpOut) * scale * std::sin(kPi * c);
      pos_ = {origin.x, 0.0f, origin.z};
      // 文字の上では文字と同じ傾きに乗る（水中側では水平）
      const float a = (L->rotation.x - kHalfPi) * c;
      const float b = L->rotation.z * c;
      tilt = MatMul(RotX(a), RotZ(b));
    } else if (deep_) {
      // 泡の出る深さより下を泳いでいる：波には乗らない（水面の上下で泡の帯に入らないように）
      origin = {pos_.x, waterY_ - sink_ * scale, pos_.z};
    } else {
      origin = {pos_.x, WaterHeight(pos_.x, pos_.z) + 0.05f * scale - sink_ * scale, pos_.z};
    }
    // 深い所を泳ぐときは胴を水平に寝かせ（外套膜が先）、足を後ろへなびかせる
    const Mat3 body = MatMul(BodyMatrix(yaw_, -0.25f * jet_, 0.0f), tilt);
    PlaceModel(origin, body); // モデルで描くときは、ここを基準に部位の姿勢を骨へ書く

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
    // 水面にいるときは水面に浮かせる。文字の上では：
    //   - 文字（縁取り込み）の上にある節は上面にぴったり貼り付ける（吸盤で張り付く）
    //   - しがみついている間は、次の節が文字から外れそうなら画に沿って曲げる（画を抱え込む）
    //   - それでも外れた節は縁から垂れ下がって水面へ。壁にめり込まないよう外へ押し出す
    // 深い所を泳ぐとき（jet_）は、足を顔の側（進む向きの反対）へ束ねてなびかせる
    const float spread = 1.0f - 0.35f * (std::max)(sq, 0.0f) + 0.25f * (std::max)(-sq, 0.0f); // 伸びると足をすぼめる
    const bool gripping = L && climb_ > 0.5f;
    const float wave = tentacleWave * (1.0f + 1.2f * swim_) * (gripping ? 0.45f : 1.0f);
    const float rootR = 0.28f * scale;
    const float spacing = 0.17f * scale * spread;
    const float trailYaw = yaw_; // 外套膜が先・足が後ろ（顔の向き）
    // ジェット推進の拍：吐き出す瞬間に足を閉じ、吸い込むときに少し開く
    const float jetPulse = 0.5f + 0.5f * std::sin(time_ * 7.0f);
    // しがみついている間は、着地した瞬間の足の形を文字のローカル座標に固定する（吸盤は滑らない）。
    // 毎フレーム形を探し直すと、曲げる向きの選択が切り替わって足がガクガク跳ねるため。
    // 動くのは先端の小さなうねりだけ。体の向き（カーソルを見る）が変わっても足は動かさない。
    const bool holdGrip = (curState && curState->HoldsGrip()) && L != nullptr; // Clinging
    if (!holdGrip) gripFrozen_ = false;
    const bool freezeNow = holdGrip && !gripFrozen_;
    // 足の節は胴からの相対位置をなめらかに追わせる（判定の切り替わりで 1 フレームだけ跳ねるのを消す）
    const float follow = legSnap_ ? 1.0f : 1.0f - std::exp(-dt * 18.0f);
    const float lc = L ? std::cos(L->yaw) : 1.0f, ls = L ? std::sin(L->yaw) : 0.0f;
    for (int t = 0; t < kTentacles; ++t) {
      std::array<RC::Vector3, kSegments> P{};
      if (holdGrip && gripFrozen_) {
        float prevY = origin.y;
        for (int k = 0; k < kSegments; ++k) {
          const LegGrip &g = grip_[t][k];
          const float r = parts_[kTentacleStart + t * kSegments + k].size.x * scale;
          float lx = g.lx, lz = g.lz;
          if (k >= 3) {
            // 先端だけ、ゆっくり探るように左右へ
            const float a = 0.03f * scale * static_cast<float>(k - 2) *
                            std::sin(time_ * 1.6f + static_cast<float>(t) * 1.3f - static_cast<float>(k) * 0.5f);
            lx += g.nx * a;
            lz += g.nz * a;
          }
          float x = 0.0f, z = 0.0f;
          TitleBlackboard::FromLetterLocal(*L, lx, lz, x, z);
          float y = 0.0f;
          if (g.onTop) {
            y = LetterTopAt(*L, x, z) + r * 0.7f;
          } else {
            y = HangY(*L, x, z, prevY, g.hanging);
          }
          P[k] = {x, y, z};
          prevY = y;
        }
      } else {
        const float baseAng = yaw_ + (static_cast<float>(t) + 0.5f) * (kTwoPi / static_cast<float>(kTentacles));
        float ang = baseAng;
        float x = origin.x + std::sin(ang) * rootR;
        float z = origin.z + std::cos(ang) * rootR;
        float prevY = origin.y;
        bool chainOnTop = true; // 付け根から途切れずに文字の上を這っているか
        int hanging = 0;        // 縁から垂れ下がって何節目か
        // 着地の瞬間の形を決めるときは、うねりを入れない（固定した形がうねりの途中で歪まないように）
        const float w = freezeNow ? 0.0f : wave;
        for (int k = 0; k < kSegments; ++k) {
          const float kk = static_cast<float>(k) / static_cast<float>(kSegments - 1);
          ang += w * std::sin(time_ * 2.3f + static_cast<float>(t) * 0.9f - static_cast<float>(k) * 0.7f) * (0.3f + kk);
          if (jet_ > 0.0f) {
            // 束ねる：後ろ向き ＋ 少しだけ扇に開く（拍に合わせて開閉）
            const float fan = (static_cast<float>(t) - 3.5f) * (0.05f + 0.07f * jetPulse);
            const float want = trailYaw + fan;
            ang += PartPose::WrapAngle(want - ang) * (std::min)(1.0f, jet_ * (0.45f + 0.35f * kk));
          }
          const float r = parts_[kTentacleStart + t * kSegments + k].size.x * scale;
          if (k > 0) {
            if (gripping && chainOnTop) {
              // 次の節が文字の上に残る向きを、今の向きに近い順に探す（画を抱え込むように曲がる）
              static constexpr float kBend[] = {0.0f, 0.3f, -0.3f, 0.6f, -0.6f, 0.95f, -0.95f};
              for (float b : kBend) {
                const float nx = x + std::sin(ang + b) * spacing, nz = z + std::cos(ang + b) * spacing;
                if (TitleBlackboard::LetterDistance(*L, nx, nz) < -r * 0.5f) {
                  ang += b;
                  break;
                }
              }
            }
            x += std::sin(ang) * spacing;
            z += std::cos(ang) * spacing;
          }
          float y = 0.0f;
          bool onTop = false;
          if (L) {
            const float d = TitleBlackboard::LetterDistance(*L, x, z);
            if (d < -r * 0.3f && (chainOnTop || climb_ < 0.5f)) {
              // 文字の上：上面に貼り付ける（球は縦 0.75 倍なので、その下端が上面に触れる高さ）
              y = LetterTopAt(*L, x, z) + r * 0.7f;
              onTop = true;
              hanging = 0;
            } else {
              chainOnTop = false;
              ++hanging;
              // 壁から外へ押し出す（垂れた足が縁取りの側面にめり込まないように）
              const float clear = r + 0.04f;
              if (d < clear) {
                const RC::Vector3 o = TitleBlackboard::LetterOutward(*L, x, z);
                x += o.x * (clear - d);
                z += o.z * (clear - d);
              }
              y = HangY(*L, x, z, prevY, hanging);
              if (k < 2 && climb_ < 0.5f) y = TitleOctopusDetail::Lerp(origin.y + r * 0.5f, y, 0.5f * static_cast<float>(k + 1));
            }
          } else {
            y = WaterHeight(x, z) + 0.02f - sink_ * scale;
            if (deep_) y = origin.y + r * 0.3f - 0.06f * scale * static_cast<float>(k) * jet_;
          }
          P[k] = {x, y, z};
          if (freezeNow) {
            // 文字のローカルへ写して覚える（先端を揺らす向き = 足に直交する向きも）
            LegGrip &g = grip_[t][k];
            TitleBlackboard::ToLetterLocal(*L, x, z, g.lx, g.lz);
            const float px = std::cos(ang), pz = -std::sin(ang);
            g.nx = px * lc - pz * ls;
            g.nz = px * ls + pz * lc;
            g.onTop = onTop;
            g.hanging = hanging;
          }
          prevY = y;
        }
      }

      // 胴からの相対位置をなめらかに追わせてから置く。向き・傾きは隣の節との差から決める
      RC::Vector3 prev = origin;
      for (int k = 0; k < kSegments; ++k) {
        RC::Vector3 &off = legOff_[t][k];
        const RC::Vector3 want = {P[k].x - origin.x, P[k].y - origin.y, P[k].z - origin.z};
        off.x += (want.x - off.x) * follow;
        off.y += (want.y - off.y) * follow;
        off.z += (want.z - off.z) * follow;
        const RC::Vector3 cur = AddV(origin, off);
        const Part &p = parts_[kTentacleStart + t * kSegments + k];
        const float r = p.size.x * scale;
        const float dx = cur.x - prev.x, dz = cur.z - prev.z;
        const float hl = std::sqrt(dx * dx + dz * dz);
        const float ang = hl > 1e-4f ? std::atan2(dx, dz) : yaw_;
        const float pitchUp = (k > 0) ? std::atan2(cur.y - prev.y, (std::max)(hl, 1e-3f)) : 0.0f;
        // 節は進む向きに伸ばしてつなげる（数珠玉に見えないように）。垂れる節は下を向く
        const float stretchZ = (std::max)(1.0f, 0.62f * spacing / (std::max)(r, 1e-3f));
        PlacePart(p, cur, BodyMatrix(ang, pitchUp, 0.0f), {1.0f, 1.0f, stretchZ});
        prev = cur;
      }
    }
    if (freezeNow) gripFrozen_ = true;
    legSnap_ = false;
    FlushModelPose();
  }

  /// @brief 文字の縁から垂れ下がる足の節の高さ
  /// @param L 文字
  /// @param x 節のワールド X
  /// @param z 節のワールド Z
  /// @param prevY 1 つ根元側の節の高さ
  /// @param hanging 縁から何節目か（1 から）
  /// @return 高さ（上の節が文字の上なら縁から垂れ始め、2 節ほどで水面へ）
  float HangY(const TitleBlackboard::Letter &L, float x, float z, float prevY, int hanging) const {
    const float waterH = WaterHeight(x, z) + 0.02f;
    const float topH = LetterTopAt(L, x, z);
    const float fromY = (std::max)(prevY, waterH);
    return (std::max)(waterH, (std::min)(fromY, topH) - (topH - waterH) * 0.6f * static_cast<float>(hanging));
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
      const OctopusState *cur = CurrentState();
      if (cur && cur->CanBeStartled()) { // Peeking / Clinging / Climbing
        Startle(fsm_);
        debugNote_ = "startled";
      } else {
        debugNote_ = "not visible";
      }
      return;
    }
    if (!fsm_.IsIn<HiddenState>()) {
      debugNote_ = "busy (wait until hidden)";
      return;
    }
    const bool ok = (req == 1) ? TryStartPeek(fsm_, scene) : TryStartCling(fsm_, scene);
    debugNote_ = ok ? "started" : "no place (letters not ready / camera moving?)";
  }

  // ------------------------------------------------------------------
  // 状態クラス（State パターン）
  //   状態 1 つ = クラス 1 つ。毎フレームの処理は OnUpdate、次の状態は machine.ChangeState で自分が決める。
  //   本体の private メンバへは、入れ子クラスなので friend 無しで触れる。
  // ------------------------------------------------------------------

  /// @brief タコの状態の基底。経過時間と長さ、状態ごとに答えの変わる問い合わせ（仮想関数）をまとめる
  class OctopusState : public IState<TitleOctopusScript> {
  public:
    /// @brief 状態を作る
    /// @param duration その状態の長さ（秒。使わない状態は 0）
    explicit OctopusState(float duration = 0.0f) : duration_(duration) {}

    /// @brief この状態に入ってからの経過秒（インスペクタの表示用）
    /// @return 経過秒
    float Elapsed() const { return elapsed_; }

    /// @brief 「潜れ」と頼まれたときの反応。既定は水面からそのまま潜る（Rising / Peeking）
    /// @param o タコ本体
    /// @param machine 遷移を予約するステートマシン
    /// @param fast 驚いたとき（短い時間で潜る）
    virtual void OnDiveRequested(TitleOctopusScript &o, Machine &machine, bool fast) { o.StartDive(machine, fast); }

    /// @brief 体を描くか（隠れている間だけ false）
    /// @return 描くなら true
    virtual bool IsShown() const { return true; }

    /// @brief 文字に乗っている／乗り降りしている状態か（Climbing / Clinging / Leaving）
    /// @return そうなら true
    virtual bool IsOnLetter() const { return false; }

    /// @brief 足の形を文字に固定する状態か（Clinging）
    /// @return そうなら true
    virtual bool HoldsGrip() const { return false; }

    /// @brief デバッグの Startle ボタンで驚かせられる状態か（Peeking / Clinging / Climbing）
    /// @return そうなら true
    virtual bool CanBeStartled() const { return false; }

  protected:
    /// @brief 経過時間を進める
    /// @param dt 経過秒
    void Advance(float dt) { elapsed_ += dt; }

    /// @brief その状態の長さ
    /// @return 秒
    float Duration() const { return duration_; }

  private:
    float elapsed_ = 0.0f;  ///< この状態に入ってからの経過秒
    float duration_ = 0.0f; ///< その状態の長さ（秒）
  };

  /// @brief 水中に隠れている（描かない）。待ち時間が明けたら「顔を出す」か「文字にしがみつく」を選ぶ
  class HiddenState final : public OctopusState {
  public:
    /// @brief 待ち時間を数え、明けたら出る場所を選んで Rising / Climbing へ
    /// @param o タコ本体
    /// @param machine 遷移を予約するステートマシン
    /// @param dt 経過秒
    void OnUpdate(TitleOctopusScript &o, Machine &machine, float dt) override {
      o.SetVisible(false);
      if (!o.allowed_) {
        o.timer_ = (std::max)(o.timer_, o.firstDelay); // 操作できるようになってから firstDelay 待つ
        return;
      }
      o.timer_ -= dt;
      if (o.timer_ > 0.0f) return;
      Scene *scene = o.GetScene();
      const bool wantCling = o.Rand01() < o.clingChance;
      if ((wantCling && o.TryStartCling(machine, scene)) || o.TryStartPeek(machine, scene) ||
          (!wantCling && o.TryStartCling(machine, scene))) {
        return;
      }
      o.timer_ = 1.0f; // 出られる場所が無かった：少し待ってやり直す
    }
    /// @brief 隠れているので潜りようがない（何もしない）
    void OnDiveRequested(TitleOctopusScript &, Machine &, bool) override {}
    /// @brief 隠れているので描かない
    /// @return false
    bool IsShown() const override { return false; }
    /// @brief 状態名
    /// @return "Hidden"
    const char *Name() const override { return "Hidden"; }
  };

  /// @brief 浮かび上がる：深い所から影が近づき、泡の帯を一気に抜けて水面へ（抜けた瞬間に飛沫と波紋）
  /// @details 泡の帯（水面から foamDepth まで）の中にいると胴が白い塊に見えるので、そこは数フレームで抜ける。
  class RisingState final : public OctopusState {
  public:
    /// @brief 浮かび上がりの長さで作る
    RisingState() : OctopusState(kRiseDeepTime + kRisePopTime) {}
    /// @brief 深い所 → 泡の帯を抜ける → 水面。顔を出しきったら Peeking へ
    /// @param o タコ本体
    /// @param machine 遷移を予約するステートマシン
    /// @param dt 経過秒
    void OnUpdate(TitleOctopusScript &o, Machine &machine, float dt) override {
      Advance(dt);
      if (Elapsed() < kRiseDeepTime) {
        // 深い所：足をなびかせながら上がってくる（水の色越しに影が濃くなる）
        const float u = TitleOctopusDetail::Smooth01(Elapsed() / kRiseDeepTime);
        o.sink_ = o.DeepSink() + (kRiseExtra / o.scale) * (1.0f - u);
        o.deep_ = true;
        o.jet_ = 0.6f * (1.0f - u) + 0.3f;
        return;
      }
      if (!breached_) {
        // 泡の帯を飛び越えて水面のすぐ下へ。飛沫で境目を隠す
        breached_ = true;
        o.deep_ = false;
        o.jet_ = 0.0f;
        o.sink_ = 0.3f;
        o.legSnap_ = true;
        o.Splash(o.pos_.x, o.pos_.z, 0.8f);
        o.squash_ = 0.6f; // 水を抜けた勢いで縦に伸びる
      }
      const float u = std::clamp((Elapsed() - kRiseDeepTime) / kRisePopTime, 0.0f, 1.0f);
      o.sink_ = 0.3f * (1.0f - TitleOctopusDetail::Smooth01(u));
      if (u >= 1.0f) machine.ChangeState<PeekingState>(o.Rand(o.peekMin, (std::max)(o.peekMin, o.peekMax)));
    }
    /// @brief 状態名
    /// @return "Rising"
    const char *Name() const override { return "Rising"; }

  private:
    bool breached_ = false; ///< 浮かび上がりで水面を抜けたか
  };

  /// @brief 水面に浮いている：見回す・カーソルを目で追う・寄ってくる・驚く
  class PeekingState final : public OctopusState {
  public:
    /// @brief 顔を出している長さで作る
    /// @param duration 顔を出している秒数
    explicit PeekingState(float duration) : OctopusState(duration) {}
    /// @brief カーソルへの反応と見回し。驚いたら・時間が来たら潜る（Diving へ）
    /// @param o タコ本体
    /// @param machine 遷移を予約するステートマシン
    /// @param dt 経過秒
    void OnUpdate(TitleOctopusScript &o, Machine &machine, float dt) override {
      Advance(dt);
      const TitleBlackboard::State &bb = TitleBlackboard::Get();
      float wantYaw = o.baseYaw_ + 0.7f * std::sin(o.time_ * 0.45f + o.baseYaw_);
      float lookX = std::sin(o.time_ * 0.9f) * 0.6f, lookZ = 0.8f; // 黒目の向き（胴のローカル xz）
      if (bb.CursorValid()) {
        const float dx = bb.CursorPos().x - o.pos_.x, dz = bb.CursorPos().z - o.pos_.z;
        const float d = std::sqrt(dx * dx + dz * dz);
        const float cs = std::sqrt(bb.CursorVel().x * bb.CursorVel().x + bb.CursorVel().z * bb.CursorVel().z);
        if ((d < o.startleRadius && cs > o.startleSpeed) || (bb.CursorClicked() && d < o.clickStartleRadius)) {
          o.Startle(machine);
          return;
        }
        if (d < o.lookRadius && d > 1e-3f) {
          wantYaw = std::atan2(dx, dz);
          o.LookLocal(dx / d, dz / d, lookX, lookZ);
          // 近くでじっとしているカーソルには寄っていく（文字やメニューの下へは行かない）
          o.cursorStill_ = (cs < 0.6f && d > o.approachStop) ? o.cursorStill_ + dt : 0.0f;
          if (o.cursorStill_ > o.approachDelay) {
            const float step = (std::min)(o.approachSpeed * dt, d - o.approachStop);
            const float nx = o.pos_.x + dx / d * step, nz = o.pos_.z + dz / d * step;
            if (step > 0.0f && !NearLetters(nx, nz, 1.4f * o.scale)) {
              o.pos_.x = nx;
              o.pos_.z = nz;
              o.swim_ = (std::min)(1.0f, o.swim_ + dt * 3.0f);
            }
          }
        } else {
          o.cursorStill_ = 0.0f;
        }
      }
      o.TurnTo(wantYaw, dt, 2.5f);
      o.lookX_ += (lookX - o.lookX_) * (std::min)(1.0f, dt * 8.0f);
      o.lookZ_ += (lookZ - o.lookZ_) * (std::min)(1.0f, dt * 8.0f);
      // ときどき小さな波紋（呼吸）
      o.rippleTimer_ -= dt;
      if (o.rippleTimer_ <= 0.0f) {
        RC::AddWaveSourceAtWorld(o.pos_.x, o.pos_.z, 0.01f, -0.03f);
        o.rippleTimer_ = o.Rand(0.7f, 1.1f);
      }
      if (Elapsed() >= Duration()) o.StartDive(machine, false);
    }
    /// @brief 水面にいるので驚かせられる
    /// @return true
    bool CanBeStartled() const override { return true; }
    /// @brief 状態名
    /// @return "Peeking"
    const char *Name() const override { return "Peeking"; }
  };

  /// @brief 文字の上（Climbing / Clinging）の共通の基底。「潜れ」と頼まれたら、まず滑り降りる
  class OnLetterState : public OctopusState {
  public:
    /// @brief 状態の長さで作る
    /// @param duration その状態の長さ（秒）
    explicit OnLetterState(float duration) : OctopusState(duration) {}
    /// @brief 文字の上から直接は潜れないので、いったん滑り降りる（Leaving へ）
    /// @param o タコ本体
    /// @param machine 遷移を予約するステートマシン
    /// @param fast 驚いたとき（飛び降りる）
    void OnDiveRequested(TitleOctopusScript &o, Machine &machine, bool fast) override { o.BeginLeave(machine, fast); }
    /// @brief 文字に乗っている
    /// @return true
    bool IsOnLetter() const override { return true; }
    /// @brief 文字の上にいるので驚かせられる
    /// @return true
    bool CanBeStartled() const override { return true; }
  };

  /// @brief よじ登る：縁の外の水面 → 文字の上（弧を描く）
  class ClimbingState final : public OnLetterState {
  public:
    /// @brief よじ登る長さで作る
    /// @param duration よじ登る秒数
    explicit ClimbingState(float duration) : OnLetterState(duration) {}
    /// @brief 登りきったら文字をぐらりと揺らして Clinging へ。文字が消えたら飛び降りる
    /// @param o タコ本体
    /// @param machine 遷移を予約するステートマシン
    /// @param dt 経過秒
    void OnUpdate(TitleOctopusScript &o, Machine &machine, float dt) override {
      Advance(dt);
      const TitleBlackboard::Letter *L = o.CurrentLetter();
      if (!L) {
        OnDiveRequested(o, machine, true);
        return;
      }
      o.climb_ = std::clamp(Elapsed() / (std::max)(Duration(), 0.05f), 0.0f, 1.0f);
      o.LookAtCursorWhileOnLetter(dt);
      if (o.climb_ >= 1.0f) {
        TitleBlackboard::PushLetter(static_cast<size_t>(o.letter_), {0.0f, 0.0f, 0.0f}, 0.05f); // 乗った重みでぐらり
        o.squash_ = -0.5f; // 着地でつぶれる
        machine.ChangeState<ClingingState>(o.Rand(o.clingMin, (std::max)(o.clingMin, o.clingMax)));
      }
    }
    /// @brief 状態名
    /// @return "Climbing"
    const char *Name() const override { return "Climbing"; }
  };

  /// @brief しがみついている：文字に乗ったまま、カーソルを目で追う。驚いたら墨を吐いて飛び降りる
  class ClingingState final : public OnLetterState {
  public:
    /// @brief しがみつく長さで作る
    /// @param duration しがみついている秒数
    explicit ClingingState(float duration) : OnLetterState(duration) {}
    /// @brief 驚いたら・時間が来たら・文字が消えたら Leaving へ
    /// @param o タコ本体
    /// @param machine 遷移を予約するステートマシン
    /// @param dt 経過秒
    void OnUpdate(TitleOctopusScript &o, Machine &machine, float dt) override {
      Advance(dt);
      const TitleBlackboard::Letter *L = o.CurrentLetter();
      if (!L) {
        o.BeginLeave(machine, true);
        return;
      }
      o.climb_ = 1.0f;
      const TitleBlackboard::State &bb = TitleBlackboard::Get();
      if (bb.CursorValid()) {
        RC::Vector3 attach, water, outward;
        o.LetterFrame(*L, o.attachLx_, o.attachLz_, o.outLx_, o.outLz_, attach, water, outward);
        const float dx = bb.CursorPos().x - attach.x, dz = bb.CursorPos().z - attach.z;
        const float d = std::sqrt(dx * dx + dz * dz);
        const float cs = std::sqrt(bb.CursorVel().x * bb.CursorVel().x + bb.CursorVel().z * bb.CursorVel().z);
        if ((d < o.startleRadius && cs > o.startleSpeed) || (bb.CursorClicked() && d < o.clickStartleRadius)) {
          o.Ink(attach.x, attach.z);
          o.BeginLeave(machine, true);
          return;
        }
      }
      o.LookAtCursorWhileOnLetter(dt);
      if (Elapsed() >= Duration()) o.BeginLeave(machine, false);
    }
    /// @brief しがみついている間は足の形を文字に固定する
    /// @return true
    bool HoldsGrip() const override { return true; }
    /// @brief 状態名
    /// @return "Clinging"
    const char *Name() const override { return "Clinging"; }
  };

  /// @brief 滑り降りる：文字の上 → 縁の外の水面。降りきったら潜る
  class LeavingState final : public OctopusState {
  public:
    /// @brief 降りる長さと降り始めの高さで作る
    /// @param duration 降りる秒数
    /// @param leaveFrom 降り始めたときの climb_
    LeavingState(float duration, float leaveFrom) : OctopusState(duration), leaveFrom_(leaveFrom) {}
    /// @brief 降り始めた高さから 0 へ。水面へ着いたら Diving へ
    /// @param o タコ本体
    /// @param machine 遷移を予約するステートマシン
    /// @param dt 経過秒
    void OnUpdate(TitleOctopusScript &o, Machine &machine, float dt) override {
      Advance(dt);
      const TitleBlackboard::Letter *L = o.CurrentLetter();
      // 降り始めた高さ（leaveFrom_）から 0 へ
      o.climb_ = leaveFrom_ * (1.0f - std::clamp(Elapsed() / (std::max)(Duration(), 0.05f), 0.0f, 1.0f));
      if (!L) o.climb_ = 0.0f; // 文字が消えた：その場（最後に置いた位置）から潜る
      if (o.climb_ <= 0.0f) {
        // 水面へ着いた：そこから潜る（位置は縁の外の水面。水しぶきは StartDive が出す）
        if (L) {
          RC::Vector3 attach, water, outward;
          o.LetterFrame(*L, o.attachLx_, o.attachLz_, o.outLx_, o.outLz_, attach, water, outward);
          o.pos_ = {water.x, 0.0f, water.z};
        }
        o.letter_ = -1;
        o.sink_ = 0.0f;
        OnDiveRequested(o, machine, o.fastLeave_);
      }
    }
    /// @brief 降りている途中：驚いたら急いで降りるよう覚えておき、降りきってから潜る
    /// @param o タコ本体
    /// @param machine 遷移を予約するステートマシン
    /// @param fast 驚いたとき
    void OnDiveRequested(TitleOctopusScript &o, Machine &machine, bool fast) override {
      o.fastLeave_ = o.fastLeave_ || fast;
      if (o.climb_ > 0.0f) return; // 降りきったら OnUpdate がもう一度頼む
      o.StartDive(machine, fast);
    }
    /// @brief 文字から降りている途中
    /// @return true
    bool IsOnLetter() const override { return true; }
    /// @brief 状態名
    /// @return "Leaving"
    const char *Name() const override { return "Leaving"; }

  private:
    float leaveFrom_ = 1.0f; ///< 降り始めたときの climb_
  };

  /// @brief 潜る：水面で身をすくめ → 泡の帯を一気に抜けて深い所へ → 外套膜を先にして泳ぎ去り、水の色に溶けて消える
  /// @details 本物のタコは逃げるとき、漏斗から水を噴いて外套膜を先に、足を後ろへ束ねて進む（ジェット推進）。
  ///          噴くたびに加速して惰性で減速する、を繰り返す。泳ぐのは泡の帯より下なので白い塊にならず、
  ///          屈折越しに水の色に染まった影として見える。
  class DivingState final : public OctopusState {
  public:
    /// @brief 潜り方を決めて作る
    /// @param plungeDur 水面で身をすくめて潜り込むまでの秒数（深い所から泳ぎ去るときは 0）
    /// @param diveFrom 潜り始めたときの sink_
    /// @param swimVel 泳ぎ始めの速さ（m/s）
    DivingState(float plungeDur, float diveFrom, float swimVel)
        : OctopusState(plungeDur + kSwimTime), plungeDur_(plungeDur), diveFrom_(diveFrom), swimVel_(swimVel) {}
    /// @brief 潜り込み → ジェット推進で泳ぎ去る。見えなくなったら Hidden へ
    /// @param o タコ本体
    /// @param machine 遷移を予約するステートマシン
    /// @param dt 経過秒
    void OnUpdate(TitleOctopusScript &o, Machine &machine, float dt) override {
      Advance(dt);
      if (!o.deep_) {
        // 水面：身をすくめて沈み込む（泡の帯の上端だけ。縁の泡は顔を出しているときと同じ）
        const float u = std::clamp(Elapsed() / (std::max)(plungeDur_, 0.02f), 0.0f, 1.0f);
        o.sink_ = TitleOctopusDetail::Lerp(diveFrom_, (std::max)(diveFrom_, 0.3f), TitleOctopusDetail::Smooth01(u));
        o.jet_ = 0.5f * u;
        if (u < 1.0f) return;
        // 泡の帯を飛び越えて深い所へ。水しぶき・墨で境目を隠す
        o.deep_ = true;
        o.sink_ = o.DeepSink();
        o.legSnap_ = true;
        swimVel_ = o.fleeing_ ? o.swimSpeed * 1.4f : o.swimSpeed * 0.25f;
        if (o.fleeing_) o.yaw_ = std::atan2(-o.fleeDir_.x, -o.fleeDir_.z); // 外套膜（顔の反対側）を進む向きへ
        o.Splash(o.pos_.x, o.pos_.z, o.fleeing_ ? 0.7f : 0.4f);
        RC::AddWaveSourceAtWorld(o.pos_.x, o.pos_.z, 0.02f * o.scale, -0.12f);
      }
      const float swimT = Elapsed() - plungeDur_;
      const float dur = (std::max)(Duration() - plungeDur_, 0.1f);
      const float u = std::clamp(swimT / dur, 0.0f, 1.0f);
      o.jet_ = (std::min)(1.0f, o.jet_ + dt * 4.0f);
      // さらに沈みながら（水の色に溶けていく）
      o.sink_ = o.DeepSink() + (kSwimSink / o.scale) * TitleOctopusDetail::Smooth01(u);
      // ジェット：一定間隔で噴いて加速し、水の抵抗で減速する
      const float target = o.fleeing_ ? o.swimSpeed : o.swimSpeed * 0.3f;
      pulseTimer_ -= dt;
      if (pulseTimer_ <= 0.0f) {
        swimVel_ = (std::max)(swimVel_, target * 1.6f);
        o.squash_ = -0.35f; // 噴く瞬間に外套膜が縮む
        pulseTimer_ = o.fleeing_ ? 0.42f : 0.7f;
      }
      swimVel_ += (target * 0.5f - swimVel_) * (std::min)(1.0f, dt * 2.5f);
      // 進む向き：外套膜の向き（顔の反対）
      const float mx = -std::sin(o.yaw_), mz = -std::cos(o.yaw_);
      o.pos_.x += mx * swimVel_ * dt;
      o.pos_.z += mz * swimVel_ * dt;
      o.swim_ = 1.0f;
      if (u >= 1.0f) {
        o.SetVisible(false);
        machine.ChangeState<HiddenState>();
        o.timer_ = o.Rand(o.intervalMin, (std::max)(o.intervalMin, o.intervalMax)) * (o.fleeing_ ? 1.3f : 1.0f);
        o.fleeing_ = false;
        o.deep_ = false;
        o.jet_ = 0.0f;
      }
    }
    /// @brief もう潜っているので何もしない
    void OnDiveRequested(TitleOctopusScript &, Machine &, bool) override {}
    /// @brief 状態名
    /// @return "Diving"
    const char *Name() const override { return "Diving"; }

  private:
    float plungeDur_ = 0.2f; ///< 水面で身をすくめて潜り込むまでの秒数
    float diveFrom_ = 0.0f;  ///< 潜り始めたときの sink_
    float swimVel_ = 0.0f;   ///< 水中を泳ぐ今の速さ（m/s）
    float pulseTimer_ = 0.0f; ///< 次に水を噴くまでの秒数
  };

  /// @brief 今の状態（状態が無ければ nullptr）
  /// @return 今の状態
  const OctopusState *CurrentState() const { return fsm_.As<OctopusState>(); }

  /// @brief 体を描く状態か（隠れている間は描かない）
  /// @return 描くなら true
  bool IsShown() const {
    const OctopusState *cur = CurrentState();
    return cur && cur->IsShown();
  }

  // ------------------------------------------------------------------
  // 状態
  // ------------------------------------------------------------------

  std::vector<Part> parts_;
  std::weak_ptr<Entity> model_;             ///< タコのモデル（modelPath が空なら作らない）
  RC::Vector3 modelOrigin_{};               ///< モデルを置いた胴の原点（PlaceModel）
  Mat3 modelBody_ = PartPose::Identity();   ///< モデルを置いた胴の向き（PlaceModel）
  std::weak_ptr<Entity> folder_;
  bool visible_ = true; ///< SetVisible の現在値（最初の SetVisible(false) で全部を隠すため true から始める）

  Machine fsm_;              ///< ふるまいの状態（Hidden / Rising / Peeking / Climbing / Clinging / Leaving / Diving）
  bool allowed_ = false;     ///< このフレーム出てきてよいか（有効かつ文字に触れられる。HiddenState が見る）
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
  float squash_ = 0.0f;    ///< 伸び縮み（正で縦に伸びる）
  float squashVel_ = 0.0f;
  float swim_ = 0.0f;      ///< 泳いでいる強さ（足のうねりを大きくする）
  float lookX_ = 0.0f;     ///< 黒目の向き（胴のローカル X）
  float lookZ_ = 1.0f;     ///< 黒目の向き（胴のローカル Z）
  float cursorStill_ = 0.0f; ///< カーソルが近くでじっとしている秒数
  float rippleTimer_ = 0.0f;
  bool fleeing_ = false;     ///< 驚いて逃げている
  RC::Vector3 fleeDir_{0.0f, 0.0f, 1.0f};
  // 水中（泡の帯より深い所）
  bool deep_ = false;        ///< 泡の帯より深い所にいる（波に乗らない・足を束ねる）
  float jet_ = 0.0f;         ///< 足を後ろへ束ねる強さ（0〜1。ジェット推進の姿勢）
  // 潜り込みの秒数・泳ぐ速さ・噴く間隔は DivingState が持つ

  // 足
  /// @brief しがみついている間の足の節 1 つ（文字のローカルに固定した位置）
  struct LegGrip {
    float lx = 0.0f, lz = 0.0f; ///< 文字のローカル位置（m）
    float nx = 1.0f, nz = 0.0f; ///< 先端を揺らす向き（足に直交。文字のローカル）
    bool onTop = false;         ///< 文字の上に貼り付いているか（false は縁から垂れている）
    int hanging = 0;            ///< 縁から何節目か
  };
  std::array<std::array<LegGrip, kSegments>, kTentacles> grip_{};
  bool gripFrozen_ = false;  ///< grip_ を決めたか（しがみつきに入った最初のフレームで決める）
  std::array<std::array<RC::Vector3, kSegments>, kTentacles> legOff_{}; ///< 胴からの相対位置（なめらかに追う）
  bool legSnap_ = true;      ///< 次のフレームは足をなめらかにせず目標へ合わせる（出現・瞬間移動の直後）

  // しがみつき
  int letter_ = -1;          ///< しがみついている文字の番号（TitleBlackboard::letters の添字）
  float attachLx_ = 0.0f;    ///< しがみつく点（文字のローカル X）
  float attachLz_ = 0.0f;    ///< しがみつく点（文字のローカル Z）
  float outLx_ = 0.0f;       ///< 外向き（文字のローカル X）
  float outLz_ = -1.0f;      ///< 外向き（文字のローカル Z）
  float outDist_ = 1.2f;     ///< しがみつく点 → 縁の外の水面の点 の距離（m）
  float climb_ = 0.0f;       ///< 0: 縁の外の水面 / 1: 文字の上
  bool fastLeave_ = false;   ///< 驚いて飛び降りている

  // 墨
  /// @brief 墨の塊 1 つ
  struct InkPuff {
    RC::Vector3 pos{};   ///< 吐いた位置（xz）
    RC::Vector3 vel{};   ///< 広がりきるまでに流れる量（xz、m）
    float r0 = 0.2f;     ///< 吐いた直後の半径（m）
    float r1 = 1.0f;     ///< 広がりきった半径（m）
    float delay = 0.0f;  ///< 吐いてから現れるまでの秒数
    float spin = 0.0f;   ///< テクスチャの向き（rad）
    bool core = false;   ///< 濃い芯か（false は薄い外側）
  };
  static constexpr const char *kInkModelPath = "Resources/model/cube/cube.obj"; ///< 単位立方体（上面に UV 0〜1）
  static constexpr const char *kInkTexPath = "Resources/Particle/circle2.png";  ///< 丸い透かし
  int inkModel_ = -1;
  int inkTex_ = -1;
  bool inkModelUnlit_ = false; ///< ライティングを切る設定を済ませたか（モデルの非同期ロード後に 1 回）
  bool inkActive_ = false;
  float inkT_ = 0.0f;
  std::array<InkPuff, 9> inkPuffs_{};
  std::vector<Transform> inkCore_;  ///< 描画用（毎フレーム詰め直す）
  std::vector<Transform> inkOuter_;

  // デバッグ
  int debugRequest_ = 0;     ///< 1: Peek / 2: Cling / 3: Startle
  std::string debugNote_;
};

TitleOctopusScript::TitleOctopusScript() {
  fsm_.ChangeState<HiddenState>();
  fsm_.ApplyPending(*this);
}

REGISTER_SCRIPT(TitleOctopusScript)
