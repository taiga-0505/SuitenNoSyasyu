#include "ECS/ScriptableEntity.h"
#include "ECS/ScriptRegistry.h"
#include "ECS/TransformComponent.h"
#include "ECS/CameraComponent.h"
#include "ECS/ModelRendererComponent.h"
#include "ECS/AnimationComponent.h"
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
#include "Game/Framework/StateMachine.h"
#include "Game/Framework/TitleBlackboard.h"

#if RC_ENABLE_IMGUI
#include "imgui/imgui.h"
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <vector>

// ============================================================================
// タイトル画面の「賑やかし」：サメ・漂流物・カモメ・船を画面外から定期的に流す
//
//   - Title.json の空エンティティに NativeScript として 1 つ付けるだけで動く。
//     TitleScreenScript（文字・メニュー・飛び込み）とは独立していて、互いに参照しない。
//   - 画面範囲は「真上から見下ろしているメインカメラ」から求める（位置・fovY・アスペクト比）。
//     浮上演出（Result → Title の Dive 遷移）でカメラが動いているあいだは出さず、
//     カメラが真下を向いて落ち着いた時点の画角を覚えて使う。
//   - 出てくるもの（種類ごとに JSON で on/off・頻度・速さ・大きさを変えられる）:
//       shark : shark.obj を水面すれすれで泳がせる。背中と背ビレだけが水面から出て、
//               水中の胴は水面シェーダの岸辺フォーム（深度差）で白く泡立って見える。
//       drift : 木箱・樽・流木。水面の波（WaterSurface）と波紋（RenderInteractiveWater の
//               読み戻し。タイトルでは TitleScreenScript が有効化）に合わせて浮き沈み・傾く。
//               サメと船は自分の航跡を拾って暴れるので波紋には乗せない（Gerstner のみ）
//       gull  : カモメ。bird.glb（敵の鳥と同じモデル）のクリップ Flap / Glide で羽ばたき・滑空する。群れで来ることがある
//       ship  : 帆船。ship.glb（敵の船と同じモデル。旗と帆はクリップ Sail）。波で揺れ、船首から航跡の波紋を出す
//               どちらも modelPath を空にすると以前のプリミティブの組み合わせに戻る
//       fish  : 小魚の群れ。背中だけ水面に出して泳ぎ、カーソルが近づくと散って逃げる（驚くと跳ねる）
//   - ぶつかり：船・漂流物はタイトル文字（TitleBlackboard 経由で位置を知る）に当たると文字を押しのけ、
//     漂流物どうしは弾き合う。当たった瞬間に水しぶきと波紋を出す。
//   - カーソルの波：船と漂流物は、カーソル（TitleMouseRippleScript が TitleBlackboard に書く）が
//     近くで動くと揺れる（漂流物は押し流される）。船は自分の航跡を拾わないよう波紋の高さを読まないので、
//     「近くでカーソルが動いた」ことを衝撃として揺れのばねに入れている。
//   - サメはたまに水面から跳ねる（跳ぶ道筋にタイトル文字・メニューが無いときだけ）。
//   - 水面に浮くもの（drift / ship）は、タイトル文字やメニューに重ならないよう
//     「レーン」（ワールド Z の帯）の中だけを横切らせる。サメとカモメは画面をどの向きにも横切る
//     （サメは文字の下に潜り、カモメは文字の上を飛ぶので重なっても破綻しない）。
//   - エンティティは最初にプールとして作っておき、出番が来たら SetActive(true) で使い回す。
//     実行中にモデル読み込みやメッシュ生成を走らせないため（出現の瞬間に止まらない）。
//     どれも "transient" タグ付きなので、エディタでシーンを保存しても JSON には残らない。
//   - 編集モード（再生していない）では何もしない。
//
//   ※ エンジンの描画は Transform の親子を解決しないので、部位のワールド姿勢は
//     BirdEnemyScript と同じ方法（行列で合成 → エンジンのオイラー順へ分解）で自前で書き込む。
//
//   クラス設計（確認課題 01_クラス設計）
//     - 01_02 ポリモーフィズム：以前は 1 つの構造体 Actor が全種類の変数を持ち、enum Kind の switch で
//       生成・更新・当たり判定を分岐していた。今は基底 AmbientActor ＋ 種類ごとの派生クラス
//       （SharkActor / DriftActor（CrateDrift / BarrelDrift / LogDrift）/ GullActor / ShipActor /
//       FishSchoolActor）に分け、本体は基底のポインタ経由で Tick / Spawn / IsReady などを呼ぶだけにした。
//       水面に浮いてぶつかり・カーソルの波に反応するもの（船・漂流物）は中間の FloatingActor にまとめた。
//       部位の役割（enum Role）による分岐も、役割ごとに持ち主のクラスの別の入れ物へ分けて無くした
//       （固定部位は AmbientActor、翼は GullActor、旗は ShipActor、小魚の胴・尾は FishSchoolActor）。
//     - 01_03 State Pattern：サメの「泳ぐ ⇔ 跳ぶ」を StateMachine<SharkActor> の状態クラスにした。
//     - 01_04 Factory Method（CreateActor）/ Template Method（AmbientActor::Tick / Spawn、
//       FloatingActor::StepReactions）/ 委譲（航跡の記録 WakeTrail をサメと船が部品として持つ）/
//       Object Pool（最初に作ったエンティティを SetActive で使い回す。もともとの設計）。
// ============================================================================

namespace TitleAmbientDetail {

// 行列・オイラー角の変換は共通ヘッダ（ShipVisualScript と共用）
using namespace PartPose;

/// @brief 今の Transform を描画側（PrimitiveMesh / Model）へ即座に写す
/// @details Model は DataDrivenScene がスクリプト更新後に同期するが、
///          PrimitiveMesh は次フレーム冒頭まで同期されないため 1 フレーム遅れて見える。
///          出現した瞬間に前回の位置で 1 フレーム映るのを防ぐ目的もある。
/// @param e 対象のエンティティ
inline void SyncRender(Entity &e) {
  auto *tr = e.GetComponent<TransformComponent>();
  if (!tr) return;
  if (auto *pm = e.GetComponent<PrimitiveMeshComponent>()) {
    if (pm->meshHandle >= 0) {
      if (auto *p = RC::GetPrimitiveMeshTransformPtr(pm->meshHandle)) *p = tr->ToTransform();
    }
  }
  if (auto *ren = e.GetComponent<ModelRendererComponent>()) {
    if (ren->modelHandle >= 0) {
      if (auto *p = RC::GetModelTransformPtr(ren->modelHandle)) *p = tr->ToTransform();
    }
  }
}

/// @brief 水面の波紋（RenderInteractiveWater）へ波源を入れる。UV はワールド 100m 四方の固定写像
/// @param x 波源のワールド X
/// @param z 波源のワールド Z
/// @param radius 半径（UV。0.01 = 1m）
/// @param strength 強さ（負なら押し下げる）
/// @return 入れられたら true
inline bool PushWaveSource(float x, float z, float radius, float strength) {
  return RC::AddWaveSourceAtWorld(x, z, radius, strength); // 範囲外・64 個超えは捨てられる（false）
}

} // namespace TitleAmbientDetail

/// @class TitleAmbientScript
/// @brief タイトル画面で、サメ・漂流物・カモメ・船を画面外から定期的に流す
/// @details JSON (scriptDataList) で設定できる項目:
///   [全体]
///   "enabled"       : false で何も出さない
///   "firstDelay"    : 再生開始（カメラが落ち着いて）から 1 体目までの秒数
///   "intervalMin" / "intervalMax" : 次を出すまでの間隔（秒、この範囲で乱数）
///   "maxActive"     : 同時に画面にいてよい数（全種合計。カモメの群れは 1 羽ずつ数える）
///   "screenMargin"  : 画面の外どれだけ離れたところから出し、どこまで行ったら消すか（m）
///   [航跡波（サメ・船共通）] 船首のへこみ ＋ 後方へ V 字に広がる 2 本の山（ケルビン波の見立て）
///   "wakeAngleDeg"   : V の半開き角（度。実際の船は約 19.5°）
///   "wakeBowStrength" / "wakeBowRadius" : 船首のへこみ（毎フレームの押し下げ量 / 半径 UV）
///   "wakeArmStrength" / "wakeArmMin" / "wakeArmRadius" : V の山（根元の強さ / 末端の強さ / 半径 UV）
///   "wakeSpacing"    : 通った道筋を記録する間隔（m）。小さいほど V が滑らかだが波源数が増える
///   ※ 強さは 1.5 m/s のときの値で、速さに比例して増減する。V の長さは種類ごと（shark/ship の "wakeLength"、m）
///   [引き波の泡（白波）] 波紋テクスチャの泡チャンネルへ置く。その場に残って滲みながら消える
///   "foamEnabled"
///   "foamStern" / "foamSternRadius" : 船尾の引き波（長く残る白濁。1 秒あたり、1.5 m/s 時）/ 半径（UV）
///   "foamBow" / "foamBowRadius"     : 船首の砕け波（すぐ消える。1 フレームあたり @60fps）/ 半径
///   "foamArm" / "foamArmRadius"     : V の腕の白い線（すぐ消える。根元の 1 フレームあたり @60fps）/ 半径
///   "foamArmLength" / "foamArmStep" : 腕のうち白い範囲（V の長さに対する割合）/ 線を描く点の間隔（m）
///   "foamDecay" / "foamSpread" / "crestDecay" : 引き波の残存率 / 滲み、波頭（船首・腕）の残存率（毎フレーム）
///   種類ごとの量の倍率は shark/ship の "foam"
///   [種類ごと] "shark" / "drift" / "gull" / "ship" の各オブジェクトに
///   "enabled" / "weight"（選ばれやすさ）/ "maxActive" / "poolSize"（最初に作っておく数）
///   "speedMin" / "speedMax"（m/s）/ "scaleMin" / "scaleMax"
///   "lanes" : [[zMin, zMax], ...] ワールド Z の帯。物体の端まで含めてこの帯に収まる位置を選び、
///             X 方向にまっすぐ横切らせる。空配列なら画面をどの向きにも横切る
///   ＋ 種類ごとの項目（Serialize の中身を参照）
/// @note クラス設計: 設定値はすべて private。画面を横切るもの 1 体は AmbientActor の派生クラスで、
///          本スクリプトの private な入れ子クラスとして宣言している（friend 無しで設定値・乱数に触れるため）。
class TitleAmbientScript : public ScriptableEntity {
public:
  /// @brief 全設定を JSON にする（種類ごとの設定は "shark" / "drift" / "gull" / "ship" のオブジェクト）
  /// @return JSON オブジェクト
  nlohmann::json Serialize() override {
    nlohmann::json j;
    j["enabled"] = enabled;
    j["firstDelay"] = firstDelay;
    j["intervalMin"] = intervalMin;
    j["intervalMax"] = intervalMax;
    j["maxActive"] = maxActive;
    j["screenMargin"] = screenMargin;
    j["wakeAngleDeg"] = wakeAngleDeg;
    j["wakeBowStrength"] = wakeBowStrength;
    j["wakeBowRadius"] = wakeBowRadius;
    j["wakeArmStrength"] = wakeArmStrength;
    j["wakeArmMin"] = wakeArmMin;
    j["wakeArmRadius"] = wakeArmRadius;
    j["wakeSpacing"] = wakeSpacing;
    j["foamEnabled"] = foamEnabled;
    j["foamStern"] = foamStern;
    j["foamSternRadius"] = foamSternRadius;
    j["foamBow"] = foamBow;
    j["foamBowRadius"] = foamBowRadius;
    j["foamArm"] = foamArm;
    j["foamArmLength"] = foamArmLength;
    j["foamArmRadius"] = foamArmRadius;
    j["foamArmStep"] = foamArmStep;
    j["foamDecay"] = foamDecay;
    j["foamSpread"] = foamSpread;
    j["crestDecay"] = crestDecay;

    // 種類ごと：共通項目（WriteKind）＋ 種類固有の項目
    nlohmann::json js = WriteKind(shark);
    js["modelPath"] = sharkModelPath;
    js["modelYawOffsetDeg"] = sharkModelYawOffsetDeg;
    js["sink"] = sharkSink;
    js["waveFollow"] = sharkWaveFollow;
    js["swayDeg"] = sharkSwayDeg;
    js["swayPeriod"] = sharkSwayPeriod;
    js["wagDeg"] = sharkWagDeg;
    js["wagFrequency"] = sharkWagFrequency;
    js["wake"] = sharkWake;
    js["wakeLength"] = sharkWakeLength;
    js["foam"] = sharkFoam;
    js["jump"] = sharkJump;
    js["jumpChance"] = sharkJumpChance;
    js["jumpHeight"] = sharkJumpHeight;
    js["jumpDuration"] = sharkJumpDuration;
    js["jumpCooldown"] = sharkJumpCooldown;
    js["jumpMax"] = sharkJumpMax;
    js["jumpSplash"] = sharkJumpSplash;
    j["shark"] = js;

    nlohmann::json jd = WriteKind(drift);
    jd["tiltScale"] = driftTiltScale;
    jd["spinDeg"] = driftSpinDeg;
    jd["floatBias"] = driftFloatBias;
    j["drift"] = jd;

    nlohmann::json jg = WriteKind(gull);
    jg["heightMin"] = gullHeightMin;
    jg["heightMax"] = gullHeightMax;
    jg["flapFrequency"] = gullFlapFrequency;
    jg["flapDeg"] = gullFlapDeg;
    jg["dihedralDeg"] = gullDihedralDeg;
    jg["glideChance"] = gullGlideChance;
    jg["flockMax"] = gullFlockMax;
    jg["turnDeg"] = gullTurnDeg;
    jg["modelPath"] = gullModelPath;
    j["gull"] = jg;

    nlohmann::json jp = WriteKind(ship);
    jp["tiltScale"] = shipTiltScale;
    jp["wake"] = shipWake;
    jp["wakeLength"] = shipWakeLength;
    jp["foam"] = shipFoam;
    jp["hullColor"] = {shipHullColor.x, shipHullColor.y, shipHullColor.z, shipHullColor.w};
    jp["sailColor"] = {shipSailColor.x, shipSailColor.y, shipSailColor.z, shipSailColor.w};
    jp["modelPath"] = shipModelPath;
    j["ship"] = jp;

    nlohmann::json jf = WriteKind(fish);
    jf["count"] = fishCount;
    jf["spread"] = fishSpread;
    jf["fleeRadius"] = fishFleeRadius;
    jf["fleeSpeed"] = fishFleeSpeed;
    jf["jumpChance"] = fishJumpChance;
    jf["idleJumpChance"] = fishIdleJumpChance;
    jf["jumpHeight"] = fishJumpHeight;
    jf["sink"] = fishSink;
    jf["color"] = {fishColor.x, fishColor.y, fishColor.z, fishColor.w};
    j["fish"] = jf;

    j["contact"] = {{"enabled", contactEnabled},
                    {"letterShake", contactLetterShake},
                    {"splash", contactSplash},
                    {"ripple", contactRipple},
                    {"driftRestitution", driftRestitution}};
    j["cursorRock"] = {{"enabled", cursorRock},
                       {"radius", cursorRockRadius},
                       {"strength", cursorRockStrength},
                       {"click", cursorClickRock},
                       {"push", cursorPush},
                       {"frequency", rockFrequency},
                       {"damping", rockDamping}};
    return j;
  }

  /// @brief JSON から全設定を読み込む（無いキーは現在値のまま）
  /// @param j 読み込み元の JSON
  void Deserialize(const nlohmann::json &j) override {
    ReadB(j, "enabled", enabled);
    ReadF(j, "firstDelay", firstDelay);
    ReadF(j, "intervalMin", intervalMin);
    ReadF(j, "intervalMax", intervalMax);
    ReadI(j, "maxActive", maxActive);
    ReadF(j, "screenMargin", screenMargin);
    ReadF(j, "wakeAngleDeg", wakeAngleDeg);
    ReadF(j, "wakeBowStrength", wakeBowStrength);
    ReadF(j, "wakeBowRadius", wakeBowRadius);
    ReadF(j, "wakeArmStrength", wakeArmStrength);
    ReadF(j, "wakeArmMin", wakeArmMin);
    ReadF(j, "wakeArmRadius", wakeArmRadius);
    ReadF(j, "wakeSpacing", wakeSpacing);
    ReadB(j, "foamEnabled", foamEnabled);
    ReadF(j, "foamStern", foamStern);
    ReadF(j, "foamSternRadius", foamSternRadius);
    ReadF(j, "foamBow", foamBow);
    ReadF(j, "foamBowRadius", foamBowRadius);
    ReadF(j, "foamArm", foamArm);
    ReadF(j, "foamArmLength", foamArmLength);
    ReadF(j, "foamArmRadius", foamArmRadius);
    ReadF(j, "foamArmStep", foamArmStep);
    ReadF(j, "foamDecay", foamDecay);
    ReadF(j, "foamSpread", foamSpread);
    ReadF(j, "crestDecay", crestDecay);

    // 種類ごと：共通項目（ReadKind）＋ 種類固有の項目
    if (j.contains("shark") && j["shark"].is_object()) {
      const auto &js = j["shark"];
      ReadKind(js, shark);
      if (js.contains("modelPath") && js["modelPath"].is_string()) sharkModelPath = js["modelPath"].get<std::string>();
      ReadF(js, "modelYawOffsetDeg", sharkModelYawOffsetDeg);
      ReadF(js, "sink", sharkSink);
      ReadF(js, "waveFollow", sharkWaveFollow);
      ReadF(js, "swayDeg", sharkSwayDeg);
      ReadF(js, "swayPeriod", sharkSwayPeriod);
      ReadF(js, "wagDeg", sharkWagDeg);
      ReadF(js, "wagFrequency", sharkWagFrequency);
      ReadB(js, "wake", sharkWake);
      ReadF(js, "wakeLength", sharkWakeLength);
      ReadF(js, "foam", sharkFoam);
      ReadB(js, "jump", sharkJump);
      ReadF(js, "jumpChance", sharkJumpChance);
      ReadF(js, "jumpHeight", sharkJumpHeight);
      ReadF(js, "jumpDuration", sharkJumpDuration);
      ReadF(js, "jumpCooldown", sharkJumpCooldown);
      ReadI(js, "jumpMax", sharkJumpMax);
      ReadF(js, "jumpSplash", sharkJumpSplash);
    }
    if (j.contains("drift") && j["drift"].is_object()) {
      const auto &jd = j["drift"];
      ReadKind(jd, drift);
      ReadF(jd, "tiltScale", driftTiltScale);
      ReadF(jd, "spinDeg", driftSpinDeg);
      ReadF(jd, "floatBias", driftFloatBias);
    }
    if (j.contains("gull") && j["gull"].is_object()) {
      const auto &jg = j["gull"];
      ReadKind(jg, gull);
      ReadF(jg, "heightMin", gullHeightMin);
      ReadF(jg, "heightMax", gullHeightMax);
      ReadF(jg, "flapFrequency", gullFlapFrequency);
      ReadF(jg, "flapDeg", gullFlapDeg);
      ReadF(jg, "dihedralDeg", gullDihedralDeg);
      ReadF(jg, "glideChance", gullGlideChance);
      ReadI(jg, "flockMax", gullFlockMax);
      ReadF(jg, "turnDeg", gullTurnDeg);
      ReadS(jg, "modelPath", gullModelPath);
    }
    if (j.contains("ship") && j["ship"].is_object()) {
      const auto &jp = j["ship"];
      ReadKind(jp, ship);
      ReadF(jp, "tiltScale", shipTiltScale);
      ReadB(jp, "wake", shipWake);
      ReadF(jp, "wakeLength", shipWakeLength);
      ReadF(jp, "foam", shipFoam);
      ReadVec4(jp, "hullColor", shipHullColor);
      ReadVec4(jp, "sailColor", shipSailColor);
      ReadS(jp, "modelPath", shipModelPath);
    }
    if (j.contains("fish") && j["fish"].is_object()) {
      const auto &jf = j["fish"];
      ReadKind(jf, fish);
      ReadI(jf, "count", fishCount);
      ReadF(jf, "spread", fishSpread);
      ReadF(jf, "fleeRadius", fishFleeRadius);
      ReadF(jf, "fleeSpeed", fishFleeSpeed);
      ReadF(jf, "jumpChance", fishJumpChance);
      ReadF(jf, "idleJumpChance", fishIdleJumpChance);
      ReadF(jf, "jumpHeight", fishJumpHeight);
      ReadF(jf, "sink", fishSink);
      ReadVec4(jf, "color", fishColor);
      fishCount = std::clamp(fishCount, 1, 24);
    }
    if (j.contains("contact") && j["contact"].is_object()) {
      const auto &jc = j["contact"];
      ReadB(jc, "enabled", contactEnabled);
      ReadF(jc, "letterShake", contactLetterShake);
      ReadF(jc, "splash", contactSplash);
      ReadF(jc, "ripple", contactRipple);
      ReadF(jc, "driftRestitution", driftRestitution);
    }
    if (j.contains("cursorRock") && j["cursorRock"].is_object()) {
      const auto &jr = j["cursorRock"];
      ReadB(jr, "enabled", cursorRock);
      ReadF(jr, "radius", cursorRockRadius);
      ReadF(jr, "strength", cursorRockStrength);
      ReadF(jr, "click", cursorClickRock);
      ReadF(jr, "push", cursorPush);
      ReadF(jr, "frequency", rockFrequency);
      ReadF(jr, "damping", rockDamping);
    }
  }

#if RC_ENABLE_IMGUI
  /// @brief 全体・航跡波・白波・種類ごとの設定を編集する UI と、種類ごとにその場で出すボタンを表示する
  void OnImGui() override {
    ImGui::Text("View: %s  (%.1f x %.1f m)  Active: %d / %d", view_.valid ? "ready" : "waiting",
                view_.halfW * 2.0f, view_.halfH * 2.0f, CountActive(), maxActive);
    ImGui::Text("Next spawn in %.1fs", (std::max)(0.0f, spawnTimer_));
    ImGui::Checkbox("Enabled##Ambient", &enabled);
    ImGui::DragFloat("Interval Min##Ambient", &intervalMin, 0.1f, 0.2f, 60.0f);
    ImGui::DragFloat("Interval Max##Ambient", &intervalMax, 0.1f, 0.2f, 60.0f);
    ImGui::DragInt("Max Active##Ambient", &maxActive, 1, 1, 20);
    ImGui::DragFloat("Screen Margin##Ambient", &screenMargin, 0.1f, 0.0f, 20.0f);
    ImGui::SeparatorText("Wake (shark / ship)");
    ImGui::Text("sources this frame: %d / 64 (shared with mouse ripple)", wakeSourcesThisFrame_);
    ImGui::DragFloat("V Angle (deg)##Wake", &wakeAngleDeg, 0.5f, 5.0f, 45.0f);
    ImGui::DragFloat("Bow Strength##Wake", &wakeBowStrength, 0.0005f, 0.0f, 0.05f, "%.4f");
    ImGui::DragFloat("Bow Radius (UV)##Wake", &wakeBowRadius, 0.001f, 0.002f, 0.05f, "%.3f");
    ImGui::DragFloat("Arm Strength##Wake", &wakeArmStrength, 0.0005f, 0.0f, 0.05f, "%.4f");
    ImGui::DragFloat("Arm Min##Wake", &wakeArmMin, 0.0005f, 0.0f, 0.05f, "%.4f");
    ImGui::DragFloat("Arm Radius (UV)##Wake", &wakeArmRadius, 0.001f, 0.002f, 0.05f, "%.3f");
    ImGui::DragFloat("Trail Spacing (m)##Wake", &wakeSpacing, 0.05f, 0.2f, 5.0f);
    ImGui::SeparatorText("Wake Foam (white water)");
    ImGui::Checkbox("Enabled##Foam", &foamEnabled);
    ImGui::SameLine();
    ImGui::Text("trail %d / 64, crest %d / 128", foamSourcesThisFrame_, crestSourcesThisFrame_);
    ImGui::TextDisabled("Trail (stays, milky band)");
    ImGui::DragFloat("Stern /s##Foam", &foamStern, 0.05f, 0.0f, 20.0f);
    ImGui::DragFloat("Stern Radius (UV)##Foam", &foamSternRadius, 0.0005f, 0.002f, 0.05f, "%.4f");
    ImGui::DragFloat("Trail Decay / frame##Foam", &foamDecay, 0.0005f, 0.9f, 1.0f, "%.4f");
    ImGui::DragFloat("Trail Spread / frame##Foam", &foamSpread, 0.005f, 0.0f, 0.25f, "%.3f");
    ImGui::TextDisabled("Crest (bow + V arms, redrawn every frame)");
    ImGui::DragFloat("Bow / frame##Foam", &foamBow, 0.005f, 0.0f, 2.0f, "%.3f");
    ImGui::DragFloat("Bow Radius (UV)##Foam", &foamBowRadius, 0.0005f, 0.002f, 0.05f, "%.4f");
    ImGui::DragFloat("Arm / frame##Foam", &foamArm, 0.005f, 0.0f, 2.0f, "%.3f");
    ImGui::DragFloat("Arm Length (ratio)##Foam", &foamArmLength, 0.01f, 0.0f, 1.0f);
    ImGui::DragFloat("Arm Radius (UV)##Foam", &foamArmRadius, 0.0005f, 0.002f, 0.05f, "%.4f");
    ImGui::DragFloat("Arm Step (m)##Foam", &foamArmStep, 0.05f, 0.2f, 2.0f);
    ImGui::DragFloat("Crest Decay / frame##Foam", &crestDecay, 0.01f, 0.0f, 0.98f, "%.2f");

    // 種類ごとの共通 UI（有効・重み・最大数・速さ・大きさ・Spawn Now）
    auto kindUi = [&](const char *label, Kind k, KindParams &p) {
      ImGui::PushID(label);
      ImGui::SeparatorText(label);
      ImGui::Checkbox("Enabled", &p.enabled);
      ImGui::SameLine();
      ImGui::Text("active %d / %d  (pool %d)", CountActive(k), p.maxActive, CountPool(k));
      ImGui::DragFloat("Weight", &p.weight, 0.05f, 0.0f, 10.0f);
      ImGui::DragInt("Max Active", &p.maxActive, 1, 0, 10);
      ImGui::DragFloatRange2("Speed", &p.speedMin, &p.speedMax, 0.05f, 0.1f, 30.0f);
      ImGui::DragFloatRange2("Scale", &p.scaleMin, &p.scaleMax, 0.01f, 0.05f, 20.0f);
      if (ImGui::Button("Spawn Now")) {
        if (!view_.valid) {
          spawnNote_ = std::string(label) + ": view not ready (camera still moving / not playing)";
        } else if (TrySpawn(k)) {
          spawnNote_ = std::string(label) + ": spawned";
        } else {
          spawnNote_ = std::string(label) + ": " + spawnFail_;
        }
      }
      ImGui::SameLine();
      ImGui::TextDisabled("%s", spawnNote_.c_str());
      ImGui::PopID();
    };
    kindUi("Shark", Kind::Shark, shark);
    ImGui::DragFloat("Sink##Shark", &sharkSink, 0.01f, -1.0f, 2.0f);
    ImGui::DragFloat("Wave Follow##Shark", &sharkWaveFollow, 0.01f, 0.0f, 1.0f);
    ImGui::DragFloat("Sway Deg##Shark", &sharkSwayDeg, 0.5f, 0.0f, 60.0f);
    ImGui::DragFloat("Wag Deg##Shark", &sharkWagDeg, 0.5f, 0.0f, 30.0f);
    ImGui::DragFloat("Wag Hz##Shark", &sharkWagFrequency, 0.05f, 0.0f, 5.0f);
    ImGui::DragFloat("Model Yaw Offset##Shark", &sharkModelYawOffsetDeg, 1.0f, -180.0f, 180.0f);
    ImGui::Checkbox("Wake##Shark", &sharkWake);
    ImGui::SameLine();
    ImGui::DragFloat("Len (m)##SharkWake", &sharkWakeLength, 0.1f, 0.5f, 30.0f);
    ImGui::DragFloat("Foam x##Shark", &sharkFoam, 0.01f, 0.0f, 5.0f);
    ImGui::Checkbox("Jump##Shark", &sharkJump);
    ImGui::SameLine();
    if (ImGui::Button("Jump Now##Shark")) ForceSharkJump();
    ImGui::DragFloat("Jump Chance /s##Shark", &sharkJumpChance, 0.01f, 0.0f, 5.0f);
    ImGui::DragFloat("Jump Height##Shark", &sharkJumpHeight, 0.05f, 0.2f, 8.0f);
    ImGui::DragFloat("Jump Duration##Shark", &sharkJumpDuration, 0.02f, 0.3f, 4.0f);
    ImGui::DragFloat("Jump Cooldown##Shark", &sharkJumpCooldown, 0.1f, 0.0f, 30.0f);
    ImGui::DragInt("Jump Max##Shark", &sharkJumpMax, 1, 0, 5);
    ImGui::DragFloat("Jump Splash x##Shark", &sharkJumpSplash, 0.05f, 0.0f, 5.0f);
    kindUi("Drift", Kind::Drift, drift);
    ImGui::DragFloat("Tilt##Drift", &driftTiltScale, 0.05f, 0.0f, 2.0f);
    ImGui::DragFloat("Float Bias##Drift", &driftFloatBias, 0.01f, -1.0f, 1.0f);
    kindUi("Gull", Kind::Gull, gull);
    ImGui::DragFloatRange2("Height##Gull", &gullHeightMin, &gullHeightMax, 0.1f, 1.0f, 30.0f);
    ImGui::DragFloat("Flap Hz##Gull", &gullFlapFrequency, 0.05f, 0.1f, 8.0f);
    ImGui::DragFloat("Flap Deg##Gull", &gullFlapDeg, 0.5f, 0.0f, 70.0f);
    ImGui::DragFloat("Glide Chance##Gull", &gullGlideChance, 0.01f, 0.0f, 1.0f);
    ImGui::DragInt("Flock Max##Gull", &gullFlockMax, 1, 1, 6);
    ImGui::TextDisabled("Model: %s (Flap Deg is for primitives only)",
                        gullModelPath.empty() ? "(primitives)" : gullModelPath.c_str());
    kindUi("Ship", Kind::Ship, ship);
    ImGui::DragFloat("Tilt##Ship", &shipTiltScale, 0.05f, 0.0f, 2.0f);
    ImGui::Checkbox("Wake##Ship", &shipWake);
    ImGui::SameLine();
    ImGui::DragFloat("Len (m)##ShipWake", &shipWakeLength, 0.1f, 0.5f, 30.0f);
    ImGui::DragFloat("Foam x##Ship", &shipFoam, 0.01f, 0.0f, 5.0f);
    ImGui::TextDisabled("Model: %s (hull / sail colors are for primitives only)",
                        shipModelPath.empty() ? "(primitives)" : shipModelPath.c_str());
    kindUi("Fish", Kind::Fish, fish);
    ImGui::DragFloat("Spread (m)##Fish", &fishSpread, 0.05f, 0.3f, 6.0f);
    ImGui::DragFloat("Flee Radius##Fish", &fishFleeRadius, 0.1f, 0.0f, 15.0f);
    ImGui::DragFloat("Flee Speed##Fish", &fishFleeSpeed, 0.1f, 0.5f, 15.0f);
    ImGui::DragFloat("Jump Chance (scared)##Fish", &fishJumpChance, 0.01f, 0.0f, 1.0f);
    ImGui::DragFloat("Jump Chance (idle) /s##Fish", &fishIdleJumpChance, 0.005f, 0.0f, 1.0f);
    ImGui::DragFloat("Jump Height##Fish", &fishJumpHeight, 0.02f, 0.05f, 3.0f);
    ImGui::DragFloat("Sink##Fish", &fishSink, 0.005f, -0.2f, 0.5f);
    ImGui::SeparatorText("Contact (ship / drift vs letters, drift vs drift)");
    ImGui::Checkbox("Enabled##Contact", &contactEnabled);
    ImGui::DragFloat("Letter Shake (rad)##Contact", &contactLetterShake, 0.005f, 0.0f, 0.3f);
    ImGui::DragFloat("Splash x##Contact", &contactSplash, 0.05f, 0.0f, 5.0f);
    ImGui::DragFloat("Ripple (m)##Contact", &contactRipple, 0.005f, 0.0f, 0.6f);
    ImGui::DragFloat("Drift Restitution##Contact", &driftRestitution, 0.01f, 0.0f, 1.0f);
    ImGui::SeparatorText("Cursor Rock (ship / drift)");
    ImGui::Checkbox("Enabled##Rock", &cursorRock);
    ImGui::DragFloat("Radius (m)##Rock", &cursorRockRadius, 0.1f, 0.5f, 20.0f);
    ImGui::DragFloat("Strength##Rock", &cursorRockStrength, 0.02f, 0.0f, 10.0f);
    ImGui::DragFloat("Click##Rock", &cursorClickRock, 0.01f, 0.0f, 3.0f);
    ImGui::DragFloat("Drift Push##Rock", &cursorPush, 0.02f, 0.0f, 5.0f);
    ImGui::DragFloat("Frequency (Hz)##Rock", &rockFrequency, 0.01f, 0.1f, 3.0f);
    ImGui::DragFloat("Damping##Rock", &rockDamping, 0.01f, 0.01f, 1.0f);
    ImGui::TextDisabled("poolSize / modelPath / lanes / fish count are applied on scene reload");
  }
#endif

protected:
  /// @brief 乱数を初期化し、最初の出現までの待ちを設定する
  void OnCreate() override {
    // プールは再生中の最初の Update で作る（編集モードで作ると Hierarchy が散らかるため）。
    std::random_device rd;
    rng_.seed(rd());
    spawnTimer_ = firstDelay;
  }

  /// @brief 再生中だけ、プールを少しずつ作り、画面範囲を確定させ、出ているものを動かし、間隔ごとに新しく出す
  /// @param deltaTime 前フレームからの経過秒
  void OnUpdate(float deltaTime) override {
    SceneContext *ctx = GetSceneContext();
    if (!ctx || !ctx->isPlaying() || deltaTime <= 0.0f) return;
    Scene *scene = GetScene();
    if (!scene) return;

    // プールは 1 フレーム 1 体ずつ作る（BuildPools で予定を積み、BuildNextPooled で進める）
    if (!poolBuilt_) BuildPools(scene);
    BuildNextPooled(scene);

    time_ += deltaTime;
    if (!view_.valid) TryCacheView(scene, deltaTime);

    // このフレームの水面の波パラメータ・時刻を取り、波源数の集計を戻す
    hasWater_ = BuildWaterParams(scene, water_);
    waterTime_ = RC::GetWaterTime();
    wakeSourcesThisFrame_ = 0;
    foamSourcesThisFrame_ = 0;
    crestSourcesThisFrame_ = 0;
    // 泡の残り方（全シーン共通の設定なので、このシーンにいる間だけ上書きし OnDestroy で戻す）
    if (!foamParamsSaved_) {
      RC::GetInteractiveFoamParams(savedFoamDecay_, savedFoamSpread_, savedCrestDecay_);
      foamParamsSaved_ = true;
    }
    RC::SetInteractiveFoamParams(foamDecay, foamSpread, crestDecay);

    // 種類は知らずに、基底のポインタ経由で更新する（中身は派生クラスの OnTick）
    for (auto &a : actors_) {
      if (!a->IsActive()) continue;
      UpdateActor(*a, deltaTime);
    }
    // カーソルの波で揺らす → ぶつかりを解く（どちらも次のフレームの姿勢に効く）
    ApplyCursorWaves(deltaTime);
    ResolveContacts(deltaTime);

    // 出現の間隔を数え、切れたら重み付きで抽選して出す
    if (!enabled || !view_.valid) return;
    spawnTimer_ -= deltaTime;
    if (spawnTimer_ > 0.0f) return;

    if (TrySpawnWeighted()) {
      spawnTimer_ = Rand(intervalMin, (std::max)(intervalMin, intervalMax));
    } else {
      spawnTimer_ = 0.5f; // 空きが無い・レーンが埋まっているときは少し待って出し直す
    }
  }

  /// @brief プールのエンティティとフォルダを破棄し、上書きした泡の残り方の設定を元に戻す
  void OnDestroy() override {
    for (const auto &a : actors_) a->DestroyParts();
    actors_.clear();
    floating_.clear();
    sharks_.clear();
    if (auto f = folder_.lock()) f->Destroy();
    poolBuilt_ = false;
    buildQueue_.clear();
    buildIndex_ = 0;
    if (foamParamsSaved_) {
      RC::SetInteractiveFoamParams(savedFoamDecay_, savedFoamSpread_, savedCrestDecay_);
      foamParamsSaved_ = false;
    }
  }

private:
  using Mat3 = TitleAmbientDetail::Mat3;
  static constexpr float kSharkJumpBoost = 1.6f; ///< 跳んでいるあいだの速さの倍率（勢いをつけて跳ぶ）
  static constexpr float kRockMax = 0.4f;        ///< 揺れの上限（rad）

  /// @brief bird.glb（胴の半長 = 1）を、プリミティブのカモメの胴（半長 0.72）に合わせる倍率
  static constexpr float kGullModelScale = 0.72f;
  /// @brief ship.glb（幅 3m）を、プリミティブの船（幅 1.6m）に合わせる倍率
  static constexpr float kShipModelScale = 0.53f;
  /// @brief bird.glb の Flap は 1 周 1/2.6 秒で作ってある（scripts/modelgen/models.py の BIRD_FLAP_HZ）
  static constexpr float kGullModelFlapHz = 2.6f;

  /// @brief 出てくるものの種類
  /// @details 振る舞いの分岐には使わない（それは派生クラスの override）。
  ///          設定表（Params）の添字・重み付き抽選・数の集計のための「識別子」としてだけ残している。
  enum class Kind { Shark = 0, Drift, Gull, Ship, Fish, Count };

  /// @brief 種類ごとの共通設定
  /// @details 不変条件の無い設定値の束なので struct のまま（01_01 の例外）。
  struct KindParams {
    bool enabled = true;
    float weight = 1.0f;
    int maxActive = 1;
    int poolSize = 2;
    float speedMin = 1.0f;
    float speedMax = 2.0f;
    float scaleMin = 1.0f;
    float scaleMax = 1.0f;
    std::vector<RC::Vector2> lanes; ///< 中心 Z の帯 [zMin, zMax]。空なら自由に横切る
  };

  /// @brief 1 体を構成する部位 1 つ（エンティティと、本体に対する配置）
  /// @details 不変条件の無い配置データの束なので struct のまま（01_01 の例外）。
  ///          以前は役割（enum Role: 固定 / 翼の内側 / 翼の外側 / 旗）を持ち、置く処理が役割で分岐していた。
  ///          今は役割ごとに持ち主のクラスの別の入れ物へ分けて持つので、部位自身は役割を知らない。
  struct Part {
    std::weak_ptr<Entity> entity;
    float side = 0.0f;           ///< 翼の左右（+1 右 / -1 左）
    RC::Vector3 offset{};        ///< 本体中心からの位置（本体ローカル、scale 1 のとき）
    RC::Vector3 scale{1, 1, 1};  ///< Transform の scale（scale 1 のとき）
    Mat3 local = TitleAmbientDetail::Identity(); ///< 本体に対する部位の向き
  };

  /// @brief 航跡の道筋 1 点：船首がそこを通ったときの位置・右方向・道のり（不変条件の無い値の束なので struct）
  struct WakeSample {
    RC::Vector3 pos{};
    RC::Vector3 right{};
    float dist = 0.0f;
  };

  /// @class WakeTrail
  /// @brief 船首（サメは頭）が通った道筋を wakeSpacing ごとに記録し、V の長さより古い点を捨てる
  /// @details サメと船が同じ仕組みで航跡を出すので、道筋の記録だけを 1 つのクラスにまとめ、
  ///          両者が部品として持つ（継承ではなく委譲。カモメ・漂流物・小魚には要らないので基底には置かない）。
  class WakeTrail {
  public:
    /// @brief 記録を消す（出現し直したとき）
    void Reset() {
      samples_.clear();
      path_ = 0.0f;
      hasLast_ = false;
    }

    /// @brief 船首の今の位置で道のりを積算し、spacing ごとに道筋を記録して、length より古い点を捨てる
    /// @param bow 船首のワールド位置
    /// @param fwd 進行方向（xz、正規化済み）
    /// @param spacing 記録する間隔（m）
    /// @param length V の長さ（m）
    void Advance(const RC::Vector3 &bow, const RC::Vector3 &fwd, float spacing, float length) {
      if (hasLast_) {
        const float dx = bow.x - last_.x;
        const float dz = bow.z - last_.z;
        path_ += std::sqrt(dx * dx + dz * dz);
      }
      last_ = bow;
      hasLast_ = true;
      if (samples_.empty() || path_ - samples_.back().dist >= spacing) {
        samples_.push_back({bow, {fwd.z, 0.0f, -fwd.x}, path_});
      }
      // V の長さより古い道筋は捨てる（先頭から順に古い）
      size_t drop = 0;
      while (drop < samples_.size() && path_ - samples_[drop].dist > length) ++drop;
      if (drop > 0) samples_.erase(samples_.begin(), samples_.begin() + static_cast<std::ptrdiff_t>(drop));
    }

    /// @brief 記録した道筋（古い順）
    /// @return 道筋の点の並び（読み取り専用）
    const std::vector<WakeSample> &Samples() const { return samples_; }

    /// @brief 船首が進んだ道のり（m）
    /// @return 道のり
    float Path() const { return path_; }

  private:
    std::vector<WakeSample> samples_; ///< 通った道筋（古い順）
    float path_ = 0.0f;               ///< 船首が進んだ道のり（m）
    RC::Vector3 last_{};              ///< 前フレームの船首位置
    bool hasLast_ = false;            ///< last_ が有効か
  };

  /// @brief 真上視点の画面範囲（水面の高さでの半幅・半高さ）
  /// @details 値を覚えておくだけの束（不変条件なし）なので struct のまま。計算の補助関数だけ持つ。
  struct ViewRect {
    bool valid = false;
    float cx = 0.0f, cz = 0.0f;
    float camY = 42.0f;
    float waterY = 0.0f;
    float tanHalf = 0.2288f; ///< tan(fovY / 2)
    float aspect = 16.0f / 9.0f;
    float halfW = 17.0f, halfH = 9.6f;

    /// @brief 高さ y の平面で見えている半高さ（カメラに近いほど狭い）
    /// @param y 平面の高さ
    /// @return 半高さ（m）
    float HalfHAt(float y) const { return (std::max)(camY - y, 1.0f) * tanHalf; }
    /// @brief 高さ y の平面で見えている半幅（半高さ × アスペクト比）
    /// @param y 平面の高さ
    /// @return 半幅（m）
    float HalfWAt(float y) const { return HalfHAt(y) * aspect; }
  };

  /// @brief 当たり判定の円（真上から見た船体・漂流物を円いくつかで覆う。値の束なので struct）
  struct Circle {
    float x = 0.0f, z = 0.0f, r = 0.0f;
  };

  // ==================================================================
  // 画面を横切るもの（基底クラス ＋ 種類ごとの派生クラス）
  // ==================================================================

  /// @class AmbientActor
  /// @brief 画面を横切るもの 1 体（プールの 1 要素）の基底クラス
  /// @details 以前は 1 つの構造体 Actor が全種類の変数を持ち、種類（enum Kind）の switch で
  ///          生成・更新・当たり判定を分岐していた。種類を足すたびに switch が増え、サメにしか
  ///          要らない変数がカモメにもある状態だったため、共通部分をこの基底に、種類ごとの振る舞いを
  ///          派生クラスの override に分けた（01_02）。本体（TitleAmbientScript）は基底のポインタ経由で
  ///          Tick / Spawn / IsReady などを呼ぶだけで、相手の種類を知らない。
  ///          派生クラスは本体の private な入れ子クラスなので、引数の TitleAmbientScript & 経由で
  ///          設定値・乱数などの private メンバに friend 無しで触れられる。
  class AmbientActor {
  public:
    /// @brief 部位を 1 つずつ受け取る関数
    using PartVisitor = std::function<void(const Part &)>;

    virtual ~AmbientActor() = default;

    /// @brief 種類（設定表の添字・抽選・数の集計に使う識別子）
    /// @return 種類
    virtual Kind GetKind() const = 0;

    /// @brief 部位のエンティティを作る（プール作成時に 1 回。どれも非アクティブで作る）
    /// @param s 本体（設定値・エンティティ生成の窓口）
    /// @param scene 対象のシーン
    virtual void Build(TitleAmbientScript &s, Scene *scene) = 0;

    /// @brief 出現させる（大きさ・速さを乱数で決め、出現位置を選んでアクティブにする）
    /// @param s 本体
    /// @return 出せたら true。失敗した理由は s.spawnFail_ に残す
    /// @details Template Method：共通の手順（大きさ → 速さ → 位相 → 種類ごとの準備 → 出現位置 →
    ///          種類ごとの後処理 → 出現）をここに固定し、種類ごとの差は OnSpawn / OnPlaced で埋める。
    ///          乱数を引く順番を種類によらず 1 か所で守るため。群れで出るカモメだけは丸ごと override する。
    virtual bool Spawn(TitleAmbientScript &s) {
      const KindParams &p = s.Params(GetKind());
      scale_ = s.Rand(p.scaleMin, p.scaleMax);
      speed_ = s.Rand(p.speedMin, p.speedMax);
      phase_ = s.Rand(0.0f, TitleAmbientDetail::kTwoPi);
      pos_.y = 0.0f;
      // 種類ごとの画面外判定の半径とレーンの半幅、外から受けた動きのリセット
      OnSpawn(s);
      if (!s.PlacePath(*this, p, s.view_.waterY)) {
        s.spawnFail_ = "lane busy (drift/ship already in every lane, or lane narrower than laneRadius)";
        return false;
      }
      OnPlaced(s);
      Activate(s);
      return true;
    }

    /// @brief 出してよい状態か（モデルは非同期ロードなので、読み終わるまで出さない）
    /// @return 出してよければ true（モデルで描くものは読み終わってから。プリミティブは常に true）
    virtual bool IsReady() const {
      if (parts_.empty()) return false;
      auto e = parts_[0].entity.lock();
      if (!e) return false;
      auto *ren = e->GetComponent<ModelRendererComponent>();
      if (!ren) return true; // プリミティブの組み合わせ
      return ren->modelHandle >= 0 && RC::IsModelReady(ren->modelHandle);
    }

    /// @brief 画面外判定に使う平面の高さ
    /// @param waterY 水面の高さ
    /// @return 平面の高さ（既定は水面。空を飛ぶものは自分の高さ）
    virtual float PlaneY(float waterY) const { return waterY; }

    /// @brief 持っている部位をすべて fn に渡す（出現・退場・破棄で使う）
    /// @param fn 部位を受け取る関数
    /// @details 既定は固定部位だけ。役割ごとに別の入れ物を持つ派生クラスはそれも渡すよう override する。
    virtual void ForEachPart(const PartVisitor &fn) const {
      for (const Part &p : parts_) fn(p);
    }

    /// @brief 1 フレーム進める（Template Method：出現からの秒数を数えてから、種類ごとの OnTick を呼ぶ）
    /// @param s 本体
    /// @param dt 経過秒（0 なら姿勢の書き込みだけ）
    void Tick(TitleAmbientScript &s, float dt) {
      t_ += dt;
      OnTick(s, dt);
    }

    /// @brief 出現させる（姿勢を書いてから部位をアクティブにする）
    /// @param s 本体
    void Activate(TitleAmbientScript &s) {
      active_ = true;
      t_ = 0.0f;
      OnActivate();
      yaw_ = heading_;
      // 先に姿勢を書いてからアクティブにする（前回の位置で 1 フレーム映らないように）
      Tick(s, 0.0f);
      ForEachPart([](const Part &p) {
        if (auto e = p.entity.lock()) e->SetActive(true);
      });
    }

    /// @brief 退場させる（部位を非アクティブにしてプールへ戻す）
    void Deactivate() {
      active_ = false;
      ForEachPart([](const Part &p) {
        if (auto e = p.entity.lock()) e->SetActive(false);
      });
    }

    /// @brief 部位のエンティティをすべて破棄する（シーンを抜けるとき）
    void DestroyParts() const {
      ForEachPart([](const Part &p) {
        if (auto e = p.entity.lock()) e->Destroy();
      });
    }

    // ---- 読み取り ----
    /// @brief 出ているか
    /// @return 出ていれば true
    bool IsActive() const { return active_; }
    /// @brief 本体中心（XZ。y は空を飛ぶものだけ使う）
    /// @return 位置
    const RC::Vector3 &Pos() const { return pos_; }
    /// @brief 基準の進行方位（rad）
    /// @return 方位
    float Heading() const { return heading_; }
    /// @brief 今の向き（蛇行・旋回込み。退場判定に使う）
    /// @return 向き（rad）
    float Yaw() const { return yaw_; }
    /// @brief 出現時の進行方向
    /// @return 方向（xz）
    const RC::Vector3 &Dir() const { return dir_; }
    /// @brief 基準の速さ（m/s）
    /// @return 速さ
    float Speed() const { return speed_; }
    /// @brief 大きさ（倍率）
    /// @return 大きさ
    float Scale() const { return scale_; }
    /// @brief 画面外判定に使う半径（m、scale 込み）
    /// @return 半径
    float Extent() const { return extent_; }
    /// @brief レーンで他と重ならないための半幅（m、scale 込み）
    /// @return 半幅
    float LaneRadius() const { return laneRadius_; }
    /// @brief 出現からの秒数
    /// @return 秒数
    float Age() const { return t_; }
    /// @brief 動きの位相ずらし
    /// @return 位相（rad）
    float Phase() const { return phase_; }

    // ---- 出現時の設定（本体・群れの先頭が書く）----
    /// @brief 大きさを設定する
    /// @param v 大きさ
    void SetScale(float v) { scale_ = v; }
    /// @brief 基準の速さを設定する
    /// @param v 速さ（m/s）
    void SetSpeed(float v) { speed_ = v; }
    /// @brief 動きの位相ずらしを設定する
    /// @param v 位相（rad）
    void SetPhase(float v) { phase_ = v; }
    /// @brief 画面外判定の半径を設定する
    /// @param v 半径（m）
    void SetExtent(float v) { extent_ = v; }
    /// @brief レーンの半幅を設定する
    /// @param v 半幅（m）
    void SetLaneRadius(float v) { laneRadius_ = v; }
    /// @brief 位置を設定する
    /// @param p 位置
    void SetPos(const RC::Vector3 &p) { pos_ = p; }
    /// @brief 高さだけ設定する
    /// @param y 高さ
    void SetPosY(float y) { pos_.y = y; }
    /// @brief 基準の進行方位を設定する
    /// @param v 方位（rad）
    void SetHeading(float v) { heading_ = v; }
    /// @brief 出現時の進行方向を設定する
    /// @param d 方向（xz）
    void SetDir(const RC::Vector3 &d) { dir_ = d; }
    /// @brief 出現位置（xz）と進行方位・方向をまとめて設定する
    /// @param x 位置 X
    /// @param z 位置 Z
    /// @param heading 進行方位（rad）
    /// @param dir 進行方向（xz）
    void SetPath(float x, float z, float heading, const RC::Vector3 &dir) {
      pos_.x = x;
      pos_.z = z;
      heading_ = heading;
      dir_ = dir;
    }
    /// @brief xz に動かす（進行・ぶつかりの押し出し）
    /// @param dx X の移動量
    /// @param dz Z の移動量
    void Translate(float dx, float dz) {
      pos_.x += dx;
      pos_.z += dz;
    }

  protected:
    /// @brief 種類ごとの 1 フレームの更新（動かして部位を置く）
    /// @param s 本体
    /// @param dt 経過秒（0 なら姿勢の書き込みだけ）
    virtual void OnTick(TitleAmbientScript &s, float dt) = 0;
    /// @brief 出現位置を選ぶ前の種類ごとの準備（画面外判定の半径・レーンの半幅・状態のリセット）
    /// @param s 本体
    virtual void OnSpawn(TitleAmbientScript &s) { (void)s; }
    /// @brief 出現位置を選んだ後の種類ごとの後処理
    /// @param s 本体
    virtual void OnPlaced(TitleAmbientScript &s) { (void)s; }
    /// @brief 出現の瞬間（姿勢を書く前）に呼ばれる
    virtual void OnActivate() {}

    /// @brief 今の向きを設定する
    /// @param v 向き（rad）
    void SetYaw(float v) { yaw_ = v; }

    /// @brief 固定部位（本体の姿勢にそのまま付いていく部位）
    /// @return 部位の並び（読み取り専用）
    const std::vector<Part> &Parts() const { return parts_; }

    /// @brief 固定部位を加える
    /// @param p 部位
    void AddRigidPart(const Part &p) { parts_.push_back(p); }

    /// @brief プリミティブの固定部位を 1 つ作って加える（作れなければ何もしない）
    /// @param s 本体（エンティティ生成の窓口）
    /// @param scene 対象のシーン
    /// @param name エンティティ名
    /// @param type 形状
    /// @param color 色
    /// @param offset 本体中心からの位置（本体ローカル、scale 1 のとき）
    /// @param scale 部位の大きさ（scale 1 のとき）
    /// @param local 本体に対する部位の向き
    void AddRigidPrimitive(TitleAmbientScript &s, Scene *scene, const std::string &name, PrimitiveType type,
                           const RC::Vector4 &color, const RC::Vector3 &offset, const RC::Vector3 &scale,
                           const Mat3 &local = TitleAmbientDetail::Identity()) {
      Part p;
      if (s.MakePrimitivePart(scene, name, type, color, offset, scale, p, local)) parts_.push_back(p);
    }

    /// @brief スキン付きモデルの固定部位を 1 つ作って加える（作れなければ何もしない）
    /// @param s 本体（エンティティ生成の窓口）
    /// @param scene 対象のシーン
    /// @param name エンティティ名
    /// @param path モデルのパス
    /// @param clips 登録するクリップ（index は .glb 内のアニメーションの並び順）
    /// @param defaultClip 最初に再生するクリップ名
    /// @param offset 本体中心からの位置（本体ローカル、scale 1 のとき）
    /// @param scale モデルの大きさ（scale 1 のとき。一様）
    void AddRigidModel(TitleAmbientScript &s, Scene *scene, const std::string &name, const std::string &path,
                       const std::vector<AnimationClip> &clips, const std::string &defaultClip,
                       const RC::Vector3 &offset, float scale) {
      Part p;
      if (s.MakeModelPart(scene, name, path, clips, defaultClip, offset, scale, p)) parts_.push_back(p);
    }

    /// @brief 固定部位をすべて本体姿勢に合わせて置く
    /// @param bodyPos 本体のワールド位置
    /// @param body 本体の回転
    void PlaceRigid(const RC::Vector3 &bodyPos, const Mat3 &body) const {
      for (const Part &p : parts_) PlaceAttached(p, bodyPos, body, p.local);
    }

    /// @brief 部位を「本体中心からの位置 offset・本体に対する向き local」で本体姿勢に付けて置く
    /// @param p 部位
    /// @param bodyPos 本体のワールド位置
    /// @param body 本体の回転
    /// @param local 本体に対する部位の向き（旗・尾びれのように動かすものはここで差し替える）
    void PlaceAttached(const Part &p, const RC::Vector3 &bodyPos, const Mat3 &body, const Mat3 &local) const {
      const RC::Vector3 off = TitleAmbientDetail::ScaleV(p.offset, scale_);
      PlacePart(p, TitleAmbientDetail::AddV(bodyPos, TitleAmbientDetail::Apply(off, body)),
                TitleAmbientDetail::MatMul(local, body));
    }

    /// @brief 部位のエンティティへワールド位置・回転・scale を書き込み、描画側へ即座に反映する
    /// @param p 部位
    /// @param worldPos ワールド位置
    /// @param rot ワールド回転
    void PlacePart(const Part &p, const RC::Vector3 &worldPos, const Mat3 &rot) const {
      auto e = p.entity.lock();
      if (!e) return;
      auto *tr = e->GetComponent<TransformComponent>();
      if (!tr) return;
      tr->position = worldPos;
      tr->rotation = TitleAmbientDetail::ToEngineEuler(rot);
      tr->scale = TitleAmbientDetail::ScaleV(p.scale, scale_);
      TitleAmbientDetail::SyncRender(*e);
    }

    /// @brief 本体がモデル 1 つで描かれていれば、そのアニメーションを返す
    /// @return AnimationComponent（プリミティブの組み合わせなら nullptr）
    AnimationComponent *PartAnimation() const {
      if (parts_.empty()) return nullptr;
      auto e = parts_[0].entity.lock();
      return e ? e->GetComponent<AnimationComponent>() : nullptr;
    }

  private:
    bool active_ = false;      ///< 出ているか
    std::vector<Part> parts_;  ///< 固定部位（本体の姿勢にそのまま付いていく部位）
    RC::Vector3 pos_{};        ///< 本体中心（XZ）。y はカモメだけ使う（飛ぶ高さ）
    float heading_ = 0.0f;     ///< 基準の進行方位（rad）
    float yaw_ = 0.0f;         ///< 今の向き（蛇行・旋回込み）
    float speed_ = 1.0f;       ///< 基準の速さ（m/s）
    float scale_ = 1.0f;       ///< 大きさ
    float extent_ = 1.0f;      ///< 画面外判定に使う半径（m、scale 込み）
    float laneRadius_ = 1.0f;  ///< レーンで他と重ならないための半幅（m、scale 込み）
    float t_ = 0.0f;           ///< 出現からの秒数
    float phase_ = 0.0f;       ///< 動きの位相ずらし
    RC::Vector3 dir_{0, 0, 1}; ///< 出現時の進行方向
  };

  /// @class FloatingActor
  /// @brief 水面に浮いていて、ぶつかり・カーソルの波に反応するもの（船・漂流物）の基底
  /// @details 以前は「kind が Ship か Drift なら」という条件で、ぶつかり・揺れの処理を分岐していた。
  ///          その 2 種類に共通の「外から受けた動き」（押された速さ・揺れのばね）をこの中間クラスに集め、
  ///          重さ・揺れやすさ・ぶつかったときの反応の違いを virtual 関数にした（01_02）。
  ///          サメ・カモメ・小魚はこれらを持たないので、AmbientActor には入れていない（is-a を守る）。
  class FloatingActor : public AmbientActor {
  public:
    /// @brief 真上から見た形を覆う当たり判定の円を求める
    /// @param out 円の書き込み先（最大 3 つ）
    /// @return 円の数
    virtual int HullCircles(Circle out[3]) const = 0;

    /// @brief カーソルの波・文字に対する舷（左右）・前後を決める向き
    /// @return 向き（rad）
    virtual float Facing() const = 0;

    /// @brief 重くて押されても動かないか（船 ＝ 無限大の重さとして扱う）
    /// @return 動かないなら true
    virtual bool IsHeavy() const { return false; }

    /// @brief カーソルの波が届く距離に足す余白（m、scale 1 のとき）
    /// @return 余白
    virtual float CursorReachPad() const = 0;

    /// @brief カーソルの波での揺れやすさ（軽い物ほど大きい）
    /// @return 倍率
    virtual float WaveSensitivity() const = 0;

    /// @brief タイトル文字にめり込んでいるときの反応
    /// @param s 本体
    /// @param letter 文字の番号
    /// @param n 文字 → 本体 の向き（xz、正規化済み）
    /// @param depth めり込みの深さ（m）
    /// @param first 触れ始めた瞬間なら true
    /// @param point 接触点（xz）
    /// @param fwd 本体の前方向（Facing）
    /// @param right 本体の右方向
    /// @param restitution 跳ね返り係数
    virtual void OnLetterContact(TitleAmbientScript &s, size_t letter, const RC::Vector3 &n, float depth,
                                 bool first, const RC::Vector3 &point, const RC::Vector3 &fwd,
                                 const RC::Vector3 &right, float restitution) = 0;

    /// @brief カーソルの波を受けたとき、揺れ以外に起きること（既定は何もしない）
    /// @param s 本体
    /// @param nx カーソル → 本体 の向き X
    /// @param nz カーソル → 本体 の向き Z
    /// @param move カーソルの動きの強さ（10 m/s で 1）
    /// @param fall 距離による減衰（0〜1）
    /// @param dt 経過秒
    /// @param clicked このフレームにクリックされたか
    virtual void OnCursorWave(TitleAmbientScript &s, float nx, float nz, float move, float fall, float dt,
                              bool clicked) {
      (void)s;
      (void)nx;
      (void)nz;
      (void)move;
      (void)fall;
      (void)dt;
      (void)clicked;
    }

    /// @brief 船・漂流物どうしでぶつかって水しぶきが出た瞬間の反応（既定は何もしない）
    /// @param s 本体
    virtual void OnBump(TitleAmbientScript &s) { (void)s; }

    /// @brief 今の速度（xz、m/s。進行 ＋ 押された分）
    /// @return 速度
    RC::Vector3 Velocity() const {
      const RC::Vector3 f = TitleAmbientDetail::Forward(Heading());
      const float sp = CurrentSpeed();
      return {f.x * sp + knock_.x, 0.0f, f.z * sp + knock_.z};
    }

    /// @brief 進行とは別の xz 速度を足す（ぶつかり・カーソルの波で押された）
    /// @param dx X 成分（m/s）
    /// @param dz Z 成分（m/s）
    void AddKnock(float dx, float dz) {
      knock_.x += dx;
      knock_.z += dz;
    }
    /// @brief ロールの角速度を足す
    /// @param dv 足す角速度（rad/s）
    void KickRoll(float dv) { rockRollVel_ += dv; }
    /// @brief ピッチの角速度を足す
    /// @param dv 足す角速度（rad/s）
    void KickPitch(float dv) { rockPitchVel_ += dv; }

    /// @brief ぶつかりの水しぶきを連発しないための待ち（秒）
    /// @return 待ち
    float ContactCooldown() const { return contactCooldown_; }
    /// @brief 前フレームに触れていた文字（ビット）
    /// @return ビット列
    uint32_t LetterContact() const { return letterContact_; }
    /// @brief 今フレームに触れていた文字を記録する（触れ始めの瞬間を知るため）
    /// @param bits ビット列
    void SetLetterContact(uint32_t bits) { letterContact_ = bits; }

  protected:
    /// @brief ぶつかり判定に使う今の進行の速さ（既定は基準の速さ）
    /// @return 速さ（m/s）
    virtual float CurrentSpeed() const { return Speed(); }

    /// @brief 種類ごとに持つ「外から受けた動き」を少しずつ戻す（StepReactions から呼ばれる）
    /// @param dt 経過秒（> 0）
    virtual void DecayReactions(float dt) { (void)dt; }

    /// @brief 外から受けた動きをすべて戻す（出現時）
    void ResetReactions() {
      knock_ = {0.0f, 0.0f, 0.0f};
      rockRoll_ = rockRollVel_ = rockPitch_ = rockPitchVel_ = 0.0f;
      contactCooldown_ = 0.0f;
      letterContact_ = 0;
    }

    /// @brief 揺れのばねを 1 ステップ進め、押された速さを少しずつ戻す
    /// @param s 本体（揺れの設定値）
    /// @param dt 経過秒
    /// @details Template Method：共通の揺れ・押され・待ちの計算はここで行い、
    ///          種類ごとの戻し（漂流物の自転・船の減速）は DecayReactions に任せる。
    void StepReactions(const TitleAmbientScript &s, float dt) {
      if (dt <= 0.0f) return;
      using namespace TitleAmbientDetail;
      // 減衰付きのばね：θ'' = -ω²θ - 2ζωθ'（半陰的オイラー。ω ≒ 3.5 rad/s なら 60fps で十分安定）
      const float w = kTwoPi * (std::max)(s.rockFrequency, 0.05f);
      const float c = 2.0f * std::clamp(s.rockDamping, 0.0f, 2.0f) * w;
      rockRollVel_ += (-w * w * rockRoll_ - c * rockRollVel_) * dt;
      rockPitchVel_ += (-w * w * rockPitch_ - c * rockPitchVel_) * dt;
      rockRoll_ = std::clamp(rockRoll_ + rockRollVel_ * dt, -kRockMax, kRockMax);
      rockPitch_ = std::clamp(rockPitch_ + rockPitchVel_ * dt, -kRockMax, kRockMax);
      const float kd = std::exp(-1.4f * dt);
      knock_ = {knock_.x * kd, 0.0f, knock_.z * kd};
      DecayReactions(dt);
      contactCooldown_ = (std::max)(0.0f, contactCooldown_ - dt);
    }

    /// @brief 進行とは別の xz 速度（m/s）
    /// @return 速度
    const RC::Vector3 &Knock() const { return knock_; }
    /// @brief 揺れのロール（rad。右が上がると正）
    /// @return ロール
    float RockRoll() const { return rockRoll_; }
    /// @brief 揺れのピッチ（rad。前が上がると正）
    /// @return ピッチ
    float RockPitch() const { return rockPitch_; }
    /// @brief 水しぶきを連発しないための待ちを設定する
    /// @param v 待ち（秒）
    void SetContactCooldown(float v) { contactCooldown_ = v; }

  private:
    RC::Vector3 knock_{};          ///< 進行とは別の xz 速度（m/s）。だんだん 0 へ戻る
    float rockRoll_ = 0.0f;        ///< 揺れのロール（rad。右が上がると正）
    float rockRollVel_ = 0.0f;     ///< その角速度（rad/s）
    float rockPitch_ = 0.0f;       ///< 揺れのピッチ（rad。前が上がると正）
    float rockPitchVel_ = 0.0f;    ///< その角速度（rad/s）
    float contactCooldown_ = 0.0f; ///< 漂流物どうしの水しぶきを連発しないための待ち（秒）
    uint32_t letterContact_ = 0;   ///< 前フレームに触れていた文字（ビット）。触れ始めの瞬間を知るため
  };

  /// @class SharkActor
  /// @brief サメ：shark モデルを水面すれすれで蛇行・尾振りさせ、頭から航跡を出す。たまに跳ねる
  /// @details 「泳ぐ ⇔ 跳ぶ」は StateMachine<SharkActor> の状態クラス（SwimState / JumpState）にした（01_03）。
  ///          以前は bool jumping の if で 1 つの関数の中を分岐していた。
  ///          元の挙動では、踏み切った・着水したそのフレームから動きが切り替わる（踏み切ったフレームは
  ///          もう跳ぶ動き、着水したフレームはもう航跡を出す）。StateMachine の遷移は OnUpdate の後に
  ///          反映されるので、踏み切りは SwimState が「その場で JumpState::Fly を呼ぶ」ことで、
  ///          着水は StepMotion の戻り値で同じフレームの結果を保っている。
  class SharkActor final : public AmbientActor {
  public:
    /// @brief 泳ぐ状態で作る
    /// @param script 持ち主（状態クラスから設定値・乱数に触れるため。持ち主がプールごと破棄するので寿命は同じ）
    explicit SharkActor(TitleAmbientScript &script) : script_(&script) {
      machine_.ChangeState<SwimState>();
      machine_.ApplyPending(*this);
    }

    /// @brief 種類
    /// @return Kind::Shark
    Kind GetKind() const override { return Kind::Shark; }

    /// @brief shark モデル 1 つを作る（アニメーションは無い）
    /// @param s 本体
    /// @param scene 対象のシーン
    void Build(TitleAmbientScript &s, Scene *scene) override {
      auto e = s.MakeEntity(scene, "AmbientShark");
      if (!e) return;
      auto &mr = e->AddComponent<ModelRendererComponent>();
      mr.modelPath = s.sharkModelPath;
      scene->InitDynamicEntityRuntime(*e);
      e->SetActive(false);
      Part p;
      p.entity = e;
      AddRigidPart(p);
    }

    /// @brief デバッグ用：すぐ跳ばせる（今跳んでいれば何もしない。どうするかは今の状態が決める）
    void ForceJump() {
      if (SharkState *st = machine_.As<SharkState>()) st->OnForceJump(*this, machine_);
      machine_.ApplyPending(*this); // 押した瞬間に跳ぶ（次のフレームを待たない）
    }

  protected:
    /// @brief 画面外判定の半径・レーンの半幅を決め、ジャンプの状態を戻す
    /// @param s 本体
    void OnSpawn(TitleAmbientScript &s) override {
      (void)s;
      SetExtent(0.55f * Scale());
      SetLaneRadius(0.5f * Scale());
      // 出現し直すたびに泳ぐ状態から始める（初期化なので外から状態を入れる）
      machine_.ChangeState<SwimState>();
      machine_.ApplyPending(*this);
      jumpT_ = 0.0f;
      jumpCooldown_ = 1.0f;
      jumps_ = 0;
    }

    /// @brief 航跡の記録を消す
    void OnActivate() override { trail_.Reset(); }

    /// @brief ジャンプの待ちを数え、今の状態に 1 フレームの動きを任せる
    /// @param s 本体
    /// @param dt 経過秒
    void OnTick(TitleAmbientScript &s, float dt) override {
      (void)s;
      if (dt > 0.0f) jumpCooldown_ -= dt;
      machine_.Update(*this, dt);
    }

  private:
    /// @brief サメの状態の基底（泳ぐ / 跳ぶ）
    class SharkState : public IState<SharkActor> {
    public:
      /// @brief デバッグの「Jump Now」が押されたときの反応（既定は何もしない）
      /// @param shark サメ
      /// @param machine ステートマシン
      virtual void OnForceJump(SharkActor &shark, StateMachine<SharkActor> &machine) {
        (void)shark;
        (void)machine;
      }
    };

    /// @brief 泳ぐ状態：蛇行しながら泳ぎ、条件がそろえば跳ぶ
    class SwimState final : public SharkState {
    public:
      /// @brief 跳ぶか決め、跳ぶならそのフレームから跳ぶ動きにする。跳ばなければ泳ぐ
      /// @param shark サメ
      /// @param machine ステートマシン
      /// @param deltaTime 経過秒
      void OnUpdate(SharkActor &shark, StateMachine<SharkActor> &machine, float deltaTime) override {
        if (shark.ShouldJump(deltaTime)) {
          shark.BeginJump();
          machine.ChangeState<JumpState>();
          JumpState::Fly(shark, machine, deltaTime); // 踏み切ったフレームから跳ぶ動き（元の挙動）
          return;
        }
        shark.StepMotion(deltaTime, false);
      }
      /// @brief デバッグ：すぐ跳ぶ
      /// @param shark サメ
      /// @param machine ステートマシン
      void OnForceJump(SharkActor &shark, StateMachine<SharkActor> &machine) override {
        shark.BeginJump();
        machine.ChangeState<JumpState>();
      }
      /// @brief 状態名
      /// @return "Swim"
      const char *Name() const override { return "Swim"; }
    };

    /// @brief 跳ぶ状態：放物線で水面から出て、着水したら泳ぐ状態へ戻る
    class JumpState final : public SharkState {
    public:
      /// @brief 跳ぶ動きを 1 フレーム進める
      /// @param shark サメ
      /// @param machine ステートマシン
      /// @param deltaTime 経過秒
      void OnUpdate(SharkActor &shark, StateMachine<SharkActor> &machine, float deltaTime) override {
        Fly(shark, machine, deltaTime);
      }
      /// @brief 跳ぶ動きを 1 フレーム進め、着水したら泳ぐ状態を予約する
      /// @param shark サメ
      /// @param machine ステートマシン
      /// @param deltaTime 経過秒
      static void Fly(SharkActor &shark, StateMachine<SharkActor> &machine, float deltaTime) {
        if (shark.StepMotion(deltaTime, true)) machine.ChangeState<SwimState>();
      }
      /// @brief 状態名
      /// @return "Jump"
      const char *Name() const override { return "Jump"; }
    };

    /// @brief 今跳ぶか（有効・回数・待ち明け・出現 1.5 秒後・確率・跳ぶ道筋が空いている）
    /// @param dt 経過秒（0 なら跳ばない）
    /// @return 跳ぶなら true
    /// @note 乱数は前の条件をすべて満たしたときだけ 1 つ引く（元の短絡評価の順番のまま）
    bool ShouldJump(float dt) {
      TitleAmbientScript &s = *script_;
      return dt > 0.0f && s.sharkJump && jumps_ < s.sharkJumpMax && jumpCooldown_ <= 0.0f && Age() > 1.5f &&
             s.Rand01() < s.sharkJumpChance * dt && CanJump();
    }

    /// @brief 今跳んでよいか（画面の内側で、跳ぶ道筋と着水点に文字・メニューが無い）
    /// @return 跳んでよければ true
    bool CanJump() const {
      using namespace TitleAmbientDetail;
      const TitleAmbientScript &s = *script_;
      if (!s.view_.valid) return false;
      const RC::Vector3 f = Forward(Yaw());
      const float len = Speed() * kSharkJumpBoost * (std::max)(s.sharkJumpDuration, 0.1f);
      const float hw = s.view_.halfW - 2.0f, hh = s.view_.halfH - 2.0f;
      const TitleBlackboard::State &bb = TitleBlackboard::Get();
      const float r = 0.35f * Scale() + 0.6f;
      const RC::Vector3 &pos = Pos();
      for (int k = 0; k <= 4; ++k) {
        const float t = static_cast<float>(k) / 4.0f;
        const float x = pos.x + f.x * len * t;
        const float z = pos.z + f.z * len * t;
        // 踏み切りから着水まで画面の内側（跳ぶところが見えるように）
        if (std::fabs(x - s.view_.cx) > hw || std::fabs(z - s.view_.cz) > hh) return false;
        RC::Vector3 n{};
        float depth = 0.0f;
        for (const auto &L : bb.Letters()) {
          if (L.visible && TitleBlackboard::CircleVsLetter(L, x, z, r, n, depth)) return false;
        }
        for (const auto &L : bb.Menu()) {
          if (L.visible && TitleBlackboard::CircleVsLetter(L, x, z, r, n, depth)) return false;
        }
      }
      return true;
    }

    /// @brief 踏み切る（跳ぶ秒数・高さを乱数で決め、踏み切りの水しぶきを出す）
    void BeginJump() {
      using namespace TitleAmbientDetail;
      TitleAmbientScript &s = *script_;
      jumpT_ = 0.0f;
      jumpDur_ = (std::max)(s.sharkJumpDuration, 0.3f) * s.Rand(0.9f, 1.1f);
      jumpH_ = s.sharkJumpHeight * (Scale() / 4.0f) * s.Rand(0.85f, 1.15f);
      ++jumps_;
      jumpCooldown_ = s.sharkJumpCooldown + jumpDur_;
      const RC::Vector3 f = Forward(Yaw());
      const RC::Vector3 &pos = Pos();
      Splash({pos.x + f.x * 0.3f * Scale(), 0.0f, pos.z + f.z * 0.3f * Scale()}, Scale(), false);
    }

    /// @brief 1 フレーム動かす：蛇行・尾振りしながら進み、水面の高さに合わせて置き、頭から航跡を出す
    /// @param dt 経過秒
    /// @param airborne 跳んでいる最中か（JumpState から true）
    /// @return 跳んでいて、このフレームに着水したら true
    bool StepMotion(float dt, bool airborne) {
      using namespace TitleAmbientDetail;
      TitleAmbientScript &s = *script_;
      bool jumping = airborne;

      // 進路：基準方位のまわりをゆっくり蛇行。跳んでいるあいだは踏み切った向きのまままっすぐ
      const float sway =
          s.sharkSwayDeg * kDeg * std::sin(Age() * kTwoPi / (std::max)(s.sharkSwayPeriod, 0.5f) + Phase());
      const float moveYaw = jumping ? Yaw() : Heading() + sway;
      const float speed = Speed() * (jumping ? kSharkJumpBoost : 1.0f);
      const RC::Vector3 f = Forward(moveYaw);
      Translate(f.x * speed * dt, f.z * speed * dt);
      SetYaw(moveYaw);

      // 尾振り：頭寄りの点を軸に体を振る（頭はあまり動かず尾が大きく振れる）。空中では小さく
      const float wagAmp = jumping ? 0.35f : 1.0f;
      const float wag =
          wagAmp * s.sharkWagDeg * kDeg * std::sin(Age() * kTwoPi * s.sharkWagFrequency + Phase() * 1.7f);
      const float visYaw = moveYaw + wag;
      const float pivot = 0.3f * Scale();
      const RC::Vector3 fv = Forward(visYaw);
      const RC::Vector3 pos = Pos();
      const RC::Vector3 head = {pos.x + f.x * pivot, 0.0f, pos.z + f.z * pivot};
      const RC::Vector3 center = {head.x - fv.x * pivot, 0.0f, head.z - fv.z * pivot};

      const float base = s.hasWater_ ? s.water_.baseHeight : s.view_.waterY;
      const float h = s.WaterHeight(center.x, center.z);
      float y = base + (h - base) * s.sharkWaveFollow - s.sharkSink;

      // ジャンプ：放物線で水面から出て戻る。上り坂は頭を上げ、下りは頭から落ちる
      float pitchUp = 0.0f, roll = 0.0f;
      if (jumping) {
        jumpT_ += dt;
        const float dur = (std::max)(jumpDur_, 0.1f);
        const float u = std::clamp(jumpT_ / dur, 0.0f, 1.0f);
        const float rise = jumpH_ + s.sharkSink;
        y += rise * 4.0f * u * (1.0f - u);
        const float vy = rise * 4.0f * (1.0f - 2.0f * u) / dur;
        pitchUp = std::atan2(vy, (std::max)(speed, 0.5f)) * 0.9f;
        roll = 0.22f * std::sin(kPi * u) * ((Phase() > kPi) ? 1.0f : -1.0f); // 少しひねる
        if (u >= 1.0f) {
          jumping = false;
          Splash({center.x + fv.x * 0.3f * Scale(), 0.0f, center.z + fv.z * 0.3f * Scale()}, Scale(), true);
        }
      }

      if (!Parts().empty()) {
        const Part &p = Parts()[0];
        const Mat3 rot = MatMul(RotY(s.sharkModelYawOffsetDeg * kDeg), BodyMatrix(visYaw, pitchUp, roll));
        auto e = p.entity.lock();
        if (e) {
          if (auto *tr = e->GetComponent<TransformComponent>()) {
            tr->position = {center.x, y, center.z};
            tr->rotation = ToEngineEuler(rot);
            tr->scale = {Scale(), Scale(), Scale()};
            SyncRender(*e);
          }
        }
      }

      // 空中では航跡を出さない（着水して泳ぎ出したらまた出す）
      if (s.sharkWake && dt > 0.0f && !jumping) {
        const float nose = 0.45f * Scale();
        // 尾は頭と反対側へ同じくらい。水面に出ているのは背中と背ビレだけなので幅は細め
        const RC::Vector3 tail = {center.x - fv.x * nose, 0.0f, center.z - fv.z * nose};
        s.Wake(trail_, Speed(), {center.x + fv.x * nose, 0.0f, center.z + fv.z * nose}, fv, s.sharkWakeLength,
               tail, 0.1f * Scale(), s.sharkFoam, dt);
      }
      return airborne && !jumping;
    }

    /// @brief 水を出る・落ちるときの水しぶき・波紋・泡
    /// @param p 位置（xz）
    /// @param scale サメの全長（m）
    /// @param landing 着水なら true（踏み切りより大きい）
    void Splash(const RC::Vector3 &p, float scale, bool landing) {
      TitleAmbientScript &s = *script_;
      const float k = std::clamp(scale / 4.0f, 0.5f, 2.0f) * (std::max)(s.sharkJumpSplash, 0.0f);
      if (k <= 0.0f) return;
      const int n = std::clamp(static_cast<int>((landing ? 22.0f : 12.0f) * k + 0.5f), 0, 64);
      for (int i = 0; i < n; ++i) {
        RC::EffectParticleSpawn sp;
        sp.kind = RC::EffectParticleKind::Splash;
        sp.position = {p.x + s.Rand(-0.3f, 0.3f), s.view_.waterY + 0.05f, p.z + s.Rand(-0.3f, 0.3f)};
        sp.scale = s.Rand(0.12f, 0.26f) * std::sqrt(k);
        sp.color = {0.88f, 0.94f, 1.0f, 0.85f};
        sp.impactFactor = landing ? s.Rand(0.35f, 0.55f) : s.Rand(0.25f, 0.4f);
        RC::SpawnEffectParticle(sp);
      }
      if (landing) {
        RC::EffectParticleSpawn col;
        col.kind = RC::EffectParticleKind::HeavySplash;
        col.position = {p.x, s.view_.waterY, p.z};
        col.scale = 0.7f * k;
        col.color = {0.9f, 0.95f, 1.0f, 1.0f};
        col.impactFactor = 0.12f; // 水柱の高さ ≒ 1.8m
        RC::SpawnEffectParticle(col);
      }
      RC::AddWaveSourceAtWorld(p.x, p.z, 0.03f, -(landing ? 0.4f : 0.25f) * k);
      RC::AddFoamSourceAtWorld(p.x, p.z, 0.02f, 1.2f * k);
      RC::AddCrestFoamSourceAtWorld(p.x, p.z, 0.015f, 1.0f * k);
    }

    TitleAmbientScript *script_;      ///< 持ち主（設定値・乱数・水面）
    StateMachine<SharkActor> machine_; ///< 泳ぐ / 跳ぶ
    WakeTrail trail_;                 ///< 頭が通った道筋（航跡）
    float jumpT_ = 0.0f;              ///< 跳んでからの秒数
    float jumpDur_ = 1.3f;            ///< 跳んでいる秒数
    float jumpH_ = 2.0f;              ///< 跳ぶ高さ（m）
    float jumpCooldown_ = 0.0f;       ///< 次に跳べるまでの秒数
    int jumps_ = 0;                   ///< この横断で跳んだ回数
  };

  /// @class DriftActor
  /// @brief 漂流物の基底：流されながら自転し、水面（波紋込み）の高さと傾きに合わせて浮く
  /// @details 以前は variant（0 木箱 / 1 樽 / 2 流木）の分岐で形・浮かせ量・当たり判定を変えていた。
  ///          形ごとの違い（部位・浮かせ量・当たり判定・大きさ）を派生クラス（CrateDrift / BarrelDrift /
  ///          LogDrift）の override に分け、流され方・ぶつかったときの反応はここで共通にした（01_02）。
  class DriftActor : public FloatingActor {
  public:
    /// @brief 種類
    /// @return Kind::Drift
    Kind GetKind() const override { return Kind::Drift; }
    /// @brief カーソルの波・文字に対する向きは自転の角度
    /// @return 自転角（rad）
    float Facing() const override { return spin_; }
    /// @brief カーソルの波が届く距離の余白（小さいので 0.5m）
    /// @return 余白
    float CursorReachPad() const override { return 0.5f; }
    /// @brief 軽いのでよく揺れる
    /// @return 倍率
    float WaveSensitivity() const override { return 2.2f; }

    /// @brief 文字に当たると、ほとんど自分が動いて跳ね返る（文字は少しだけ押す）
    void OnLetterContact(TitleAmbientScript &s, size_t letter, const RC::Vector3 &n, float depth, bool first,
                         const RC::Vector3 &point, const RC::Vector3 &fwd, const RC::Vector3 &right,
                         float restitution) override {
      (void)fwd;
      // 漂流物はほとんど自分が動く（文字は少しだけ）
      Translate(n.x * depth * 0.85f, n.z * depth * 0.85f);
      TitleBlackboard::PushLetter(letter, {-n.x * depth * 0.15f, 0.0f, -n.z * depth * 0.15f},
                                  first ? s.contactLetterShake * 0.3f : 0.0f);
      const RC::Vector3 v = Velocity();
      const float vn = v.x * n.x + v.z * n.z;
      if (vn < 0.0f) {
        AddKnock(-(n.x * vn * (1.0f + restitution)), -(n.z * vn * (1.0f + restitution)));
      }
      if (first) {
        s.ContactSplash(point.x, point.z, 0.5f);
        spinKick_ += s.Rand(-1.0f, 1.0f) * 0.9f;
        KickRoll((n.x * right.x + n.z * right.z) * 0.5f);
      }
    }

    /// @brief カーソルの波に押されて流れる（クリックなら回りだす）
    void OnCursorWave(TitleAmbientScript &s, float nx, float nz, float move, float fall, float dt,
                      bool clicked) override {
      // 漂流物は波に押されて流れる（カーソルから離れる向き）
      float push = s.cursorPush * move * fall * dt;
      if (clicked) {
        push += s.cursorPush * 1.5f * fall;
        spinKick_ += s.Rand(-1.0f, 1.0f) * fall;
      }
      AddKnock(nx * push, nz * push);
    }

    /// @brief ぶつかって水しぶきが出たら、回って揺れる
    /// @param s 本体
    void OnBump(TitleAmbientScript &s) override {
      spinKick_ += s.Rand(-1.2f, 1.2f);
      KickRoll(s.Rand(-0.6f, 0.6f));
      KickPitch(s.Rand(-0.4f, 0.4f));
      SetContactCooldown(0.4f);
    }

  protected:
    /// @brief 形ごとの浮かせ量と、水面を引く前後・左右の距離（scale 1 のとき）
    struct FloatShape {
      float floatOffset; ///< 浮かせ量（m）
      float halfLen;     ///< 前後の点までの距離（m）
      float halfWid;     ///< 左右の点までの距離（m）
    };

    /// @brief 形ごとの浮かせ量・水面を引く距離
    /// @return 値の束
    virtual FloatShape Shape() const = 0;

    /// @brief 画面外判定の半径の倍率（scale 1 のとき。自転するので長手方向ぶん）
    /// @return 倍率
    virtual float ExtentFactor() const { return 0.7f; }

    /// @brief 自転角
    /// @return 角度（rad）
    float Spin() const { return spin_; }

    /// @brief 半径・自転の速さと向きを決め、外から受けた動きを戻す
    /// @param s 本体
    void OnSpawn(TitleAmbientScript &s) override {
      SetExtent(ExtentFactor() * Scale());
      SetLaneRadius(Extent()); // 自転するので長手方向ぶん空ける
      turn_ = s.Rand(-s.driftSpinDeg, s.driftSpinDeg) * TitleAmbientDetail::kDeg;
      spin_ = s.Rand(0.0f, TitleAmbientDetail::kTwoPi);
      ResetReactions();
      spinKick_ = 0.0f;
    }

    /// @brief ぶつかって加わった自転を少しずつ戻す
    /// @param dt 経過秒
    void DecayReactions(float dt) override { spinKick_ *= std::exp(-1.0f * dt); }

    /// @brief 流しながら自転させ、水面（波紋込み）の高さと傾きに合わせて置く
    /// @param s 本体
    /// @param dt 経過秒
    void OnTick(TitleAmbientScript &s, float dt) override {
      using namespace TitleAmbientDetail;
      const RC::Vector3 f = Forward(Heading());
      // 波に揺られて速さが少し揺らぐ
      const float surge = 1.0f + 0.25f * std::sin(Age() * 0.9f + Phase());
      // 進行 ＋ ぶつかり・カーソルの波で受けた速さ（knock）
      const RC::Vector3 knock = Knock();
      Translate((f.x * Speed() * surge + knock.x) * dt, (f.z * Speed() * surge + knock.z) * dt);
      spin_ += (turn_ + spinKick_) * dt;
      SetYaw(Heading()); // 退場判定は進行方向で行う
      StepReactions(s, dt);

      // 形ごとの浮かせ量と、水面を引く前後・左右の距離
      const FloatShape shape = Shape();
      const float halfLen = shape.halfLen * Scale();
      const float halfWid = shape.halfWid * Scale();

      float y = 0.0f, pitch = 0.0f, roll = 0.0f;
      // 漂流物は航跡を出さないので、マウスや船の波紋に素直に乗せる
      s.WaterPose(Pos(), spin_, halfLen, halfWid, y, pitch, roll, /*ripple=*/true);
      // ぷかぷか：波とは別に小さく上下
      y += shape.floatOffset * Scale() + s.driftFloatBias + 0.04f * std::sin(Age() * 2.1f + Phase());
      const Mat3 body =
          BodyMatrix(spin_, pitch * s.driftTiltScale + RockPitch(), roll * s.driftTiltScale + RockRoll());
      PlaceRigid({Pos().x, y, Pos().z}, body);
    }

  private:
    float turn_ = 0.0f;     ///< 自転の速さ（rad/s）
    float spin_ = 0.0f;     ///< 自転角
    float spinKick_ = 0.0f; ///< ぶつかって加わった自転（rad/s）
  };

  /// @brief 木箱：箱 ＋ 十字の帯（上から見ると板が打ち付けてあるように見える）
  class CrateDrift final : public DriftActor {
  public:
    /// @brief 箱と帯 2 本を作る
    /// @param s 本体
    /// @param scene 対象のシーン
    void Build(TitleAmbientScript &s, Scene *scene) override {
      const RC::Vector4 wood = {0.56f, 0.39f, 0.21f, 1.0f};
      const RC::Vector4 woodDark = {0.33f, 0.22f, 0.11f, 1.0f};
      AddRigidPrimitive(s, scene, "AmbientCrate", PrimitiveType::Box, wood, {0, 0, 0}, {0.9f, 0.9f, 0.9f});
      AddRigidPrimitive(s, scene, "AmbientCrate_band", PrimitiveType::Box, woodDark, {0, 0, 0}, {0.94f, 0.94f, 0.16f});
      AddRigidPrimitive(s, scene, "AmbientCrate_band", PrimitiveType::Box, woodDark, {0, 0, 0}, {0.16f, 0.94f, 0.94f});
    }
    /// @brief 円 1 つで覆う
    /// @param out 円の書き込み先
    /// @return 1
    int HullCircles(Circle out[3]) const override {
      out[0] = {Pos().x, Pos().z, 0.55f * Scale()};
      return 1;
    }

  protected:
    /// @brief 木箱の浮かせ量・水面を引く距離
    /// @return 値の束
    FloatShape Shape() const override { return {0.1f, 0.45f, 0.45f}; }
  };

  /// @brief 樽：寝かせた円柱 ＋ たが 2 本
  class BarrelDrift final : public DriftActor {
  public:
    /// @brief 胴とたが 2 本を作る
    /// @param s 本体
    /// @param scene 対象のシーン
    void Build(TitleAmbientScript &s, Scene *scene) override {
      const RC::Vector4 wood = {0.56f, 0.39f, 0.21f, 1.0f};
      const Mat3 lying = TitleAmbientDetail::RotZ(TitleAmbientDetail::kHalfPi); // 円柱の軸（Y）を X へ寝かせる
      AddRigidPrimitive(s, scene, "AmbientBarrel", PrimitiveType::Cylinder, wood, {0, 0, 0}, {0.36f, 1.0f, 0.36f},
                        lying);
      AddRigidPrimitive(s, scene, "AmbientBarrel_hoop", PrimitiveType::Cylinder, {0.24f, 0.24f, 0.26f, 1.0f},
                        {0.3f, 0, 0}, {0.38f, 0.07f, 0.38f}, lying);
      AddRigidPrimitive(s, scene, "AmbientBarrel_hoop", PrimitiveType::Cylinder, {0.24f, 0.24f, 0.26f, 1.0f},
                        {-0.3f, 0, 0}, {0.38f, 0.07f, 0.38f}, lying);
    }
    /// @brief 円 1 つで覆う
    /// @param out 円の書き込み先
    /// @return 1
    int HullCircles(Circle out[3]) const override {
      out[0] = {Pos().x, Pos().z, 0.5f * Scale()};
      return 1;
    }

  protected:
    /// @brief 樽の浮かせ量・水面を引く距離
    /// @return 値の束
    FloatShape Shape() const override { return {0.06f, 0.5f, 0.36f}; }
  };

  /// @brief 流木：細長い円柱 ＋ 折れた枝
  class LogDrift final : public DriftActor {
  public:
    /// @brief 幹と枝を作る
    /// @param s 本体
    /// @param scene 対象のシーン
    void Build(TitleAmbientScript &s, Scene *scene) override {
      const Mat3 lying = TitleAmbientDetail::RotZ(TitleAmbientDetail::kHalfPi); // 円柱の軸（Y）を X へ寝かせる
      AddRigidPrimitive(s, scene, "AmbientLog", PrimitiveType::Cylinder, {0.40f, 0.29f, 0.18f, 1.0f}, {0, 0, 0},
                        {0.22f, 1.8f, 0.22f}, lying);
      AddRigidPrimitive(s, scene, "AmbientLog_branch", PrimitiveType::Cylinder, {0.36f, 0.26f, 0.16f, 1.0f},
                        {0.35f, 0.05f, 0.3f}, {0.07f, 0.6f, 0.07f},
                        TitleAmbientDetail::MatMul(TitleAmbientDetail::RotX(TitleAmbientDetail::kHalfPi),
                                                   TitleAmbientDetail::RotY(0.6f)));
    }
    /// @brief 細長いので軸に沿って円 2 つで覆う。円柱の軸は本体ローカル X（自転 spin で回る）
    /// @param out 円の書き込み先
    /// @return 2
    int HullCircles(Circle out[3]) const override {
      const float c = std::cos(Spin()), s = std::sin(Spin());
      const float o = 0.55f * Scale();
      out[0] = {Pos().x + c * o, Pos().z - s * o, 0.32f * Scale()};
      out[1] = {Pos().x - c * o, Pos().z + s * o, 0.32f * Scale()};
      return 2;
    }

  protected:
    /// @brief 流木の浮かせ量・水面を引く距離
    /// @return 値の束
    FloatShape Shape() const override { return {0.02f, 0.9f, 0.22f}; }
    /// @brief 長いので画面外判定の半径を広めに
    /// @return 倍率
    float ExtentFactor() const override { return 0.95f; }
  };

  /// @class GullActor
  /// @brief カモメ：旋回・バンクしながら飛び、羽ばたきと滑空を切り替える。群れ（V 字）で出る
  /// @details 羽ばたき ⇔ 滑空は 2 状態で、違いは目標の flapWeight（1 / 0）だけ、切り替えも
  ///          glideTimer が切れたときのトグル 1 行なので、State Pattern にはせず bool のままにしている
  ///          （状態クラスを作っても中身が定数 1 つになり、かえって追いにくい）。
  ///          翼（プリミティブのときだけ）は肩・肘で連鎖して曲げるので、固定部位とは別の入れ物に持つ。
  class GullActor final : public AmbientActor {
  public:
    /// @brief 種類
    /// @return Kind::Gull
    Kind GetKind() const override { return Kind::Gull; }

    /// @brief bird.glb のモデル、または胴・頭・くちばし・尾・内翼・外翼のプリミティブを作る
    /// @param s 本体
    /// @param scene 対象のシーン
    void Build(TitleAmbientScript &s, Scene *scene) override {
      if (!s.gullModelPath.empty()) {
        // bird.glb は胴の半長 = 1。プリミティブの胴（半長 0.72）に大きさを合わせる。
        // クリップの index は bird.glb の並び（0 Flap / 6 Glide）
        AddRigidModel(s, scene, "AmbientGull", s.gullModelPath,
                      {{"Flap", "", 0, true, 1.0f}, {"Glide", "", 6, true, 1.0f}}, "Flap", {0, 0, 0},
                      kGullModelScale);
        return;
      }
      const RC::Vector4 white = {0.97f, 0.97f, 0.96f, 1.0f};
      const RC::Vector4 gray = {0.74f, 0.77f, 0.80f, 1.0f};
      const RC::Vector4 tip = {0.14f, 0.15f, 0.17f, 1.0f};
      const RC::Vector4 beak = {0.98f, 0.78f, 0.18f, 1.0f};
      // 大きさは胴の長さを 1 とした値（球は半径、箱は辺の長さ）。+Z が頭
      AddRigidPrimitive(s, scene, "AmbientGull", PrimitiveType::Sphere, white, {0, 0, 0}, {0.26f, 0.24f, 0.72f});
      AddRigidPrimitive(s, scene, "AmbientGull_head", PrimitiveType::Sphere, white, {0, 0.12f, 0.72f},
                        {0.22f, 0.21f, 0.24f});
      AddRigidPrimitive(s, scene, "AmbientGull_beak", PrimitiveType::Sphere, beak, {0, 0.09f, 0.98f},
                        {0.05f, 0.045f, 0.16f});
      AddRigidPrimitive(s, scene, "AmbientGull_tail", PrimitiveType::Box, white, {0, 0.04f, -0.82f},
                        {0.42f, 0.05f, 0.42f});
      AddWing(s, scene, innerWings_, "AmbientGull_wing", gray, {0.2f, 0.12f, 0.08f}, {1.1f, 0.06f, 0.55f}, 1.0f);
      AddWing(s, scene, innerWings_, "AmbientGull_wing", gray, {-0.2f, 0.12f, 0.08f}, {1.1f, 0.06f, 0.55f}, -1.0f);
      AddWing(s, scene, outerWings_, "AmbientGull_tip", tip, {0, 0, -0.08f}, {0.95f, 0.05f, 0.36f}, 1.0f);
      AddWing(s, scene, outerWings_, "AmbientGull_tip", tip, {0, 0, -0.08f}, {0.95f, 0.05f, 0.36f}, -1.0f);
    }

    /// @brief 群れ（1〜gullFlockMax 羽）を V 字の隊形で出す。自分が先頭
    /// @param s 本体
    /// @return 先頭を置けたら true
    /// @details 1 羽ずつ出す他の種類と手順が違うので、基底の Spawn（Template Method）を丸ごと override する。
    bool Spawn(TitleAmbientScript &s) override {
      using namespace TitleAmbientDetail;
      GullActor *leader = this;
      const float height = s.Rand(s.gullHeightMin, (std::max)(s.gullHeightMin, s.gullHeightMax));
      const float scale = s.Rand(s.gull.scaleMin, s.gull.scaleMax);
      const float speed = s.Rand(s.gull.speedMin, s.gull.speedMax);
      const float turn = s.Rand(-s.gullTurnDeg, s.gullTurnDeg) * kDeg;

      leader->SetScale(scale);
      leader->SetExtent(2.6f * scale + 3.0f); // 翼端 ＋ 群れの広がり
      leader->SetPosY(s.view_.waterY + height);
      if (!s.PlacePath(*leader, s.gull, leader->Pos().y)) return false;

      // 群れの数は、カモメと全体の上限の残りを超えない範囲で乱数で決める
      const int room = (std::min)(s.gull.maxActive - s.CountActive(Kind::Gull), s.maxActive - s.CountActive());
      std::uniform_int_distribution<int> nd(1, (std::max)(1, (std::min)(s.gullFlockMax, room)));
      const int count = nd(s.rng_);

      const RC::Vector3 fwd = Forward(leader->Heading());
      const RC::Vector3 right = {fwd.z, 0.0f, -fwd.x};
      const RC::Vector3 start = leader->Pos();
      for (int i = 0; i < count; ++i) {
        // 2 羽目以降も Kind::Gull の空き（＝ GullActor）から選ぶ
        GullActor *g = (i == 0) ? leader : dynamic_cast<GullActor *>(s.FreeActor(Kind::Gull));
        if (!g) break;
        // V 字：1 羽目が先頭、以降は左右交互に後ろへ（乱数は元と同じ順に引く）
        const float rank = static_cast<float>((i + 1) / 2);
        const float sideSign = (i % 2 == 1) ? 1.0f : -1.0f;
        const float back = rank * 2.2f * scale / 0.5f * s.Rand(0.8f, 1.2f);
        const float side = (i == 0) ? 0.0f : sideSign * rank * 2.0f * scale / 0.5f * s.Rand(0.8f, 1.2f);
        const float memberScale = scale * s.Rand(0.92f, 1.08f);
        const float memberSpeed = speed * s.Rand(0.97f, 1.03f);
        const float dy = s.Rand(-0.6f, 0.6f);
        const float memberPhase = s.Rand(0.0f, kTwoPi);
        const float flapPhase = s.Rand(0.0f, kTwoPi);
        const float glideTimer = s.Rand(0.5f, 2.0f);
        g->SetScale(memberScale);
        g->SetSpeed(memberSpeed);
        g->SetHeading(leader->Heading());
        g->SetDir(leader->Dir());
        g->SetExtent(leader->Extent());
        g->SetPos({start.x - fwd.x * back + right.x * side, start.y + dy, start.z - fwd.z * back + right.z * side});
        g->SetPhase(memberPhase);
        g->BeginFlight(turn, flapPhase, glideTimer);
        g->Activate(s);
      }
      return true;
    }

    /// @brief 空を飛ぶので、画面外判定は自分の高さの平面で行う
    /// @param waterY 水面の高さ（使わない）
    /// @return 自分の高さ
    float PlaneY(float waterY) const override {
      (void)waterY;
      return Pos().y;
    }

    /// @brief 固定部位 ＋ 内翼 ＋ 外翼
    /// @param fn 部位を受け取る関数
    void ForEachPart(const PartVisitor &fn) const override {
      AmbientActor::ForEachPart(fn);
      for (const Part &p : innerWings_) fn(p);
      for (const Part &p : outerWings_) fn(p);
    }

  protected:
    /// @brief 旋回・バンクさせながら飛ばし、羽ばたきと滑空を切り替えて翼を動かす
    /// @param s 本体
    /// @param dt 経過秒
    void OnTick(TitleAmbientScript &s, float dt) override {
      using namespace TitleAmbientDetail;
      // ゆるく旋回しながら飛ぶ。旋回に合わせてバンクする
      SetHeading(WrapAngle(Heading() + turn_ * dt));
      SetYaw(Heading());
      const RC::Vector3 f = Forward(Heading());
      Translate(f.x * Speed() * dt, f.z * Speed() * dt);
      const float bob = 0.25f * std::sin(Age() * 0.8f + Phase());
      const float targetRoll = std::clamp(-turn_ * Speed() * 0.25f, -0.5f, 0.5f);
      roll_ += (targetRoll - roll_) * (std::min)(1.0f, dt * 2.0f);

      // 羽ばたき ⇔ 滑空
      if (dt > 0.0f) {
        glideTimer_ -= dt;
        if (glideTimer_ <= 0.0f) {
          gliding_ = !gliding_ && (s.Rand01() < s.gullGlideChance);
          glideTimer_ = gliding_ ? s.Rand(1.2f, 2.8f) : 2.0f;
        }
        const float target = gliding_ ? 0.0f : 1.0f;
        flapWeight_ += (target - flapWeight_) * (std::min)(1.0f, dt * 3.0f);
        flapPhase_ += kTwoPi * s.gullFlapFrequency * dt * (0.3f + 0.7f * flapWeight_);
      }
      const float amp = s.gullFlapDeg * kDeg * flapWeight_;
      const float inner = s.gullDihedralDeg * kDeg + amp * std::sin(flapPhase_);
      const float outer = 0.6f * amp * std::sin(flapPhase_ - 0.7f) - 0.05f * (1.0f - flapWeight_);
      const Mat3 body = BodyMatrix(Heading(), 0.0f, roll_);
      const RC::Vector3 pos = Pos();

      // モデル（bird.glb）：羽ばたき ⇔ 滑空はクリップの切り替え。胴の上下もクリップに入っている
      if (AnimationComponent *anim = PartAnimation()) {
        anim->PlayClip(flapWeight_ > 0.5f ? "Flap" : "Glide", 0.4f);
        // 群れがそろって羽ばたかないよう、個体ごとに速さを少しずらす
        const float jitter = 0.92f + 0.16f * std::fmod(std::fabs(Phase()), 1.0f);
        anim->speed = s.gullFlapFrequency / kGullModelFlapHz * jitter;
        PlaceRigid({pos.x, pos.y + bob, pos.z}, body);
        return;
      }

      // 羽ばたきに合わせて胴がわずかに上下する（振り下ろしで持ち上がる）
      const float lift = 0.05f * Scale() * std::cos(flapPhase_) * flapWeight_;

      const RC::Vector3 bodyPos = {pos.x, pos.y + bob + lift, pos.z};
      PlaceRigid(bodyPos, body);

      // 翼：内側は肩を軸に、外側は肘（内側の先端）を軸にさらに曲げる
      RC::Vector3 elbow[2] = {{0, 0, 0}, {0, 0, 0}};
      for (const Part &p : innerWings_) {
        const int si = (p.side > 0.0f) ? 0 : 1;
        const Mat3 local = RotZ(p.side * inner);
        const RC::Vector3 shoulder = ScaleV(p.offset, Scale());
        const RC::Vector3 halfSpan = Apply({p.side * p.scale.x * Scale() * 0.5f, 0.0f, 0.0f}, local);
        const RC::Vector3 c = AddV(shoulder, halfSpan);
        elbow[si] = AddV(c, halfSpan);
        PlacePart(p, AddV(bodyPos, Apply(c, body)), MatMul(local, body));
      }
      for (const Part &p : outerWings_) {
        const int si = (p.side > 0.0f) ? 0 : 1;
        const Mat3 local = MatMul(RotZ(p.side * outer), RotZ(p.side * inner));
        const RC::Vector3 halfSpan = Apply({p.side * p.scale.x * Scale() * 0.5f, 0.0f, 0.0f}, local);
        const RC::Vector3 c = AddV(AddV(elbow[si], halfSpan), ScaleV(p.offset, Scale()));
        PlacePart(p, AddV(bodyPos, Apply(c, body)), MatMul(local, body));
      }
    }

  private:
    /// @brief 翼の部位を 1 つ作って wings に加える（作れなければ何もしない）
    /// @param s 本体
    /// @param scene 対象のシーン
    /// @param wings 追加先（内翼 / 外翼）
    /// @param name エンティティ名
    /// @param color 色
    /// @param offset 肩（外翼は肘）からの位置（scale 1 のとき）
    /// @param scale 部位の大きさ（scale 1 のとき）
    /// @param side 左右（+1 右 / -1 左）
    static void AddWing(TitleAmbientScript &s, Scene *scene, std::vector<Part> &wings, const std::string &name,
                        const RC::Vector4 &color, const RC::Vector3 &offset, const RC::Vector3 &scale, float side) {
      Part p;
      if (s.MakePrimitivePart(scene, name, PrimitiveType::Box, color, offset, scale, p,
                              TitleAmbientDetail::Identity(), side)) {
        wings.push_back(p);
      }
    }

    /// @brief 飛び始めの状態を入れる（群れの 1 羽ずつ）
    /// @param turn 旋回速度（rad/s）
    /// @param flapPhase 羽ばたきの位相
    /// @param glideTimer 次に羽ばたき ⇔ 滑空を考えるまでの秒数
    void BeginFlight(float turn, float flapPhase, float glideTimer) {
      turn_ = turn;
      flapPhase_ = flapPhase;
      flapWeight_ = 1.0f;
      gliding_ = false;
      glideTimer_ = glideTimer;
      roll_ = 0.0f;
    }

    std::vector<Part> innerWings_; ///< 内翼（肩を軸に曲げる。プリミティブのときだけ）
    std::vector<Part> outerWings_; ///< 外翼（肘を軸にさらに曲げる。プリミティブのときだけ）
    float turn_ = 0.0f;            ///< 旋回速度（rad/s）
    float flapPhase_ = 0.0f;       ///< 羽ばたきの位相
    float flapWeight_ = 1.0f;      ///< 1: 羽ばたき / 0: 滑空
    bool gliding_ = false;         ///< 滑空中か
    float glideTimer_ = 0.0f;      ///< 次に羽ばたき ⇔ 滑空を考えるまでの秒数
    float roll_ = 0.0f;            ///< バンク（rad）
  };

  /// @class ShipActor
  /// @brief 帆船：進みながら波に揺れ、旗をなびかせ、船首から航跡を出す。文字を押しのけて進む
  class ShipActor final : public FloatingActor {
  public:
    /// @brief 種類
    /// @return Kind::Ship
    Kind GetKind() const override { return Kind::Ship; }

    /// @brief ship.glb のモデル、または船体・船首・甲板・船室・屋根・マスト・帆・旗のプリミティブを作る
    /// @param s 本体
    /// @param scene 対象のシーン
    void Build(TitleAmbientScript &s, Scene *scene) override {
      if (!s.shipModelPath.empty()) {
        // ship.glb は幅 3m・船体中心が喫水。プリミティブの船（幅 1.6m・前後 -2.1〜2.7m）に合わせて縮め、
        // 船首が前へ長いぶん少し後ろへずらす（当たり判定の円・航跡の船首位置はプリミティブのときと同じ）
        AddRigidModel(s, scene, "AmbientShip", s.shipModelPath, {{"Sail", "", 0, true, 1.0f}}, "Sail",
                      {0, 0, -0.4f}, kShipModelScale);
        return;
      }
      const RC::Vector4 deck = {0.66f, 0.49f, 0.30f, 1.0f};
      const RC::Vector4 cabin = {0.86f, 0.84f, 0.78f, 1.0f};
      const RC::Vector4 roof = {0.62f, 0.18f, 0.14f, 1.0f};
      const RC::Vector4 mast = {0.30f, 0.20f, 0.11f, 1.0f};
      // 静水面が y = 0。船体は下 0.3m が水に浸かる。+Z が船首
      AddRigidPrimitive(s, scene, "AmbientShip", PrimitiveType::Box, s.shipHullColor, {0, 0.05f, -0.1f},
                        {1.6f, 0.7f, 4.0f});
      AddRigidPrimitive(s, scene, "AmbientShip_bow", PrimitiveType::Box, s.shipHullColor, {0, 0.05f, 1.9f},
                        {1.13f, 0.7f, 1.13f},
                        TitleAmbientDetail::RotY(TitleAmbientDetail::kPi * 0.25f)); // 45°回した箱で尖った船首
      AddRigidPrimitive(s, scene, "AmbientShip_deck", PrimitiveType::Box, deck, {0, 0.42f, -0.1f},
                        {1.4f, 0.06f, 3.8f});
      AddRigidPrimitive(s, scene, "AmbientShip_cabin", PrimitiveType::Box, cabin, {0, 0.75f, -1.2f},
                        {1.0f, 0.6f, 1.0f});
      AddRigidPrimitive(s, scene, "AmbientShip_roof", PrimitiveType::Box, roof, {0, 1.08f, -1.2f},
                        {1.15f, 0.08f, 1.15f});
      AddRigidPrimitive(s, scene, "AmbientShip_mast", PrimitiveType::Cylinder, mast, {0, 1.75f, 0.5f},
                        {0.08f, 2.6f, 0.08f});
      // 帆は追い風で前へはらんだ形（上端を前へ倒す）。真上からでも白い面が見える
      AddRigidPrimitive(s, scene, "AmbientShip_sail", PrimitiveType::Box, s.shipSailColor, {0, 1.95f, 0.95f},
                        {2.0f, 1.8f, 0.08f}, TitleAmbientDetail::RotX(0.55f));
      // 旗は追い風で前へなびく（マストの前側）。なびかせるので固定部位とは別に持つ
      Part flag;
      if (s.MakePrimitivePart(scene, "AmbientShip_flag", PrimitiveType::Box, {0.90f, 0.22f, 0.20f, 1.0f},
                              {0, 2.95f, 0.78f}, {0.04f, 0.28f, 0.5f}, flag)) {
        flags_.push_back(flag);
      }
    }

    /// @brief 固定部位 ＋ 旗
    /// @param fn 部位を受け取る関数
    void ForEachPart(const PartVisitor &fn) const override {
      AmbientActor::ForEachPart(fn);
      for (const Part &p : flags_) fn(p);
    }

    /// @brief 船体 前後 ±2.0・左右 ±0.8、帆の幅 2.0（scale 1）を 3 つの円で覆う
    /// @param out 円の書き込み先
    /// @return 3
    int HullCircles(Circle out[3]) const override {
      const RC::Vector3 f = TitleAmbientDetail::Forward(Heading());
      for (int k = 0; k < 3; ++k) {
        const float o = static_cast<float>(k - 1) * 1.6f * Scale();
        out[k] = {Pos().x + f.x * o, Pos().z + f.z * o, 0.95f * Scale()};
      }
      return 3;
    }
    /// @brief カーソルの波・文字に対する向きは進行方向
    /// @return 進行方位（rad）
    float Facing() const override { return Heading(); }
    /// @brief 重いので、ぶつかっても押し戻されない
    /// @return true
    bool IsHeavy() const override { return true; }
    /// @brief 大きいので、カーソルの波が届く距離の余白も大きい
    /// @return 余白
    float CursorReachPad() const override { return 2.0f; }
    /// @brief 揺れやすさは基準
    /// @return 倍率
    float WaveSensitivity() const override { return 1.0f; }

    /// @brief 進路を変えず、文字をめり込んだぶんだけ押しのける。触れ始めは水しぶき・減速・揺れ
    void OnLetterContact(TitleAmbientScript &s, size_t letter, const RC::Vector3 &n, float depth, bool first,
                         const RC::Vector3 &point, const RC::Vector3 &fwd, const RC::Vector3 &right,
                         float restitution) override {
      (void)restitution;
      // 船は進路を変えない。文字をめり込んだぶんだけ押しのける
      TitleBlackboard::PushLetter(letter, {-n.x * depth, 0.0f, -n.z * depth}, first ? s.contactLetterShake : 0.0f);
      if (first) {
        s.ContactSplash(point.x, point.z, 1.0f);
        slow_ = (std::max)(slow_, 0.6f);
        // 当たった舷と反対へ傾き、船首なら頭が下がる
        KickRoll((n.x * right.x + n.z * right.z) * 0.25f);
        KickPitch(-(std::fabs(n.x * fwd.x + n.z * fwd.z) * 0.12f));
      }
    }

  protected:
    /// @brief 文字にぶつかって落ちた速さを含めた今の進行の速さ
    /// @return 速さ（m/s）
    float CurrentSpeed() const override {
      float sp = Speed();
      sp *= 1.0f - 0.6f * std::clamp(slow_, 0.0f, 1.0f);
      return sp;
    }
    /// @brief ぶつかって落ちた速さを少しずつ戻す
    /// @param dt 経過秒
    void DecayReactions(float dt) override { slow_ *= std::exp(-1.2f * dt); }

    /// @brief 半径・レーンの半幅を決め、外から受けた動きを戻す
    /// @param s 本体
    void OnSpawn(TitleAmbientScript &s) override {
      (void)s;
      SetExtent(3.0f * Scale());
      SetLaneRadius(1.1f * Scale());
      ResetReactions();
      slow_ = 0.0f;
    }

    /// @brief 航跡の記録を消す
    void OnActivate() override { trail_.Reset(); }

    /// @brief 船を進め、波に合わせて揺らし、旗をなびかせ、船首から航跡を出す
    /// @param s 本体
    /// @param dt 経過秒
    void OnTick(TitleAmbientScript &s, float dt) override {
      using namespace TitleAmbientDetail;
      const RC::Vector3 f = Forward(Heading());
      // 文字にぶつかると少し減速する（slow）。押された分（knock）も足す
      const float slowF = 1.0f - 0.6f * std::clamp(slow_, 0.0f, 1.0f);
      const RC::Vector3 knock = Knock();
      Translate((f.x * Speed() * slowF + knock.x) * dt, (f.z * Speed() * slowF + knock.z) * dt);
      SetYaw(Heading());
      StepReactions(s, dt);

      float y = 0.0f, pitch = 0.0f, roll = 0.0f;
      s.WaterPose(Pos(), Heading(), 2.0f * Scale(), 0.8f * Scale(), y, pitch, roll);
      const float sway = 0.03f * std::sin(Age() * 0.7f + Phase()); // 波とは別の小さな横揺れ
      // 揺れ（rock）はカーソルの波・ぶつかりで入る。上下も少し連動させる（傾くと沈む側がある）
      y -= 0.25f * Scale() * (std::fabs(RockRoll()) + std::fabs(RockPitch()));
      const Mat3 body =
          BodyMatrix(Heading(), pitch * s.shipTiltScale + RockPitch(), roll * s.shipTiltScale + sway + RockRoll());
      const float flagAngle = 0.35f * std::sin(Age() * 5.0f + Phase());
      const RC::Vector3 bodyPos = {Pos().x, y, Pos().z};
      PlaceRigid(bodyPos, body);
      // 旗は Y 軸まわりに振る
      for (const Part &p : flags_) PlaceAttached(p, bodyPos, body, MatMul(RotY(flagAngle), p.local));

      if (s.shipWake && dt > 0.0f) {
        const float bow = 2.6f * Scale();
        // 船体は WaterPose と同じく 前後 ±2.0・左右 ±0.8（scale 1）。船尾は少し後ろ
        const float sternOff = 2.1f * Scale();
        const RC::Vector3 stern = {Pos().x - f.x * sternOff, 0.0f, Pos().z - f.z * sternOff};
        s.Wake(trail_, Speed(), {Pos().x + f.x * bow, 0.0f, Pos().z + f.z * bow}, f, s.shipWakeLength, stern,
               0.8f * Scale(), s.shipFoam, dt);
      }
    }

  private:
    WakeTrail trail_;        ///< 船首が通った道筋（航跡）
    std::vector<Part> flags_; ///< 旗（なびかせる。プリミティブのときだけ）
    float slow_ = 0.0f;      ///< ぶつかって落ちた速さの割合（0〜1）
  };

  /// @class FishSchoolActor
  /// @brief 小魚の群れ：背中だけ水面に出して泳ぎ、カーソルが近づくと散って逃げる（驚くと跳ねる）
  /// @details 群れの中心がまっすぐ進み、1 匹ずつが「持ち場へ向かう・仲間を避ける・文字の下を避ける・
  ///          カーソルから逃げる」を混ぜて泳ぐ。1 匹の「跳ねている / いない」は放物線の 1 区間だけの
  ///          一時的な状態で、魚 1 匹ごとの値の束（FishState）に持つので bool のままにしている。
  class FishSchoolActor final : public AmbientActor {
  public:
    /// @brief 空の群れを作る
    /// @param target プールを作り終えたときの数（作り終えるまで出さない）
    explicit FishSchoolActor(int target) : target_(target) {}

    /// @brief 種類
    /// @return Kind::Fish
    Kind GetKind() const override { return Kind::Fish; }

    /// @brief 1 匹目を作る（2 匹目以降は AddFish で 1 フレーム 1 匹ずつ足す）
    /// @param s 本体
    /// @param scene 対象のシーン
    void Build(TitleAmbientScript &s, Scene *scene) override { AddFish(s, scene); }

    /// @brief 群れに小魚を 1 匹足す（胴の楕円球 ＋ 尾びれの板。最初は非アクティブ）
    /// @param s 本体
    /// @param scene 対象のシーン
    void AddFish(TitleAmbientScript &s, Scene *scene) {
      using namespace TitleAmbientDetail;
      const RC::Vector4 &color = s.fishColor;
      const RC::Vector4 tailColor = {color.x * 0.8f, color.y * 0.8f, color.z * 0.8f, color.w};
      // +Z が頭。scale 1 で全長 ≒ 0.4m（球は半径）
      Part body, tail;
      const bool hasBody =
          s.MakePrimitivePart(scene, "AmbientFish", PrimitiveType::Sphere, color, {0, 0, 0}, {0.06f, 0.045f, 0.2f}, body);
      const bool hasTail = s.MakePrimitivePart(scene, "AmbientFish_tail", PrimitiveType::Box, tailColor,
                                               {0, 0, -0.22f}, {0.16f, 0.02f, 0.1f}, tail);
      // 部位は「胴・尾」の 2 つ組で並べる（OnTick が i*2, i*2+1 で引く）。片方だけ作れたら捨てて 1 匹減らす
      if (!hasBody || !hasTail) {
        for (const Part *p : {&body, &tail}) {
          if (auto e = p->entity.lock()) e->Destroy();
        }
        target_ = (std::max)(0, target_ - 1);
        return;
      }
      members_.push_back(body);
      members_.push_back(tail);
      FishState fs;
      // 持ち場：半径 1 の円の中（群れの広がり fishSpread を掛けて使う）。進行方向に少し長い楕円
      const float ang = s.Rand(0.0f, kTwoPi);
      const float rad = std::sqrt(s.Rand01());
      fs.slot = {std::cos(ang) * rad * 0.8f, 0.0f, std::sin(ang) * rad * 1.2f};
      fs.wag = s.Rand(0.0f, kTwoPi);
      fish_.push_back(fs);
    }

    /// @brief プールを作り終えたか（全員そろうまで出さない）
    /// @return 出してよければ true
    bool IsReady() const override { return target_ > 0 && static_cast<int>(fish_.size()) >= target_; }

    /// @brief 全員の胴・尾
    /// @param fn 部位を受け取る関数
    void ForEachPart(const PartVisitor &fn) const override {
      for (const Part &p : members_) fn(p);
    }

  protected:
    /// @brief 画面外判定の半径・レーンの半幅を決める
    /// @param s 本体
    void OnSpawn(TitleAmbientScript &s) override {
      SetExtent(s.fishSpread + 3.0f); // 逃げて群れから離れた魚が画面内で消えないよう広めに
      SetLaneRadius(s.fishSpread);
    }

    /// @brief 群れを出現位置（群れの中心のまわり）へ並べる
    /// @param s 本体
    void OnPlaced(TitleAmbientScript &s) override {
      using namespace TitleAmbientDetail;
      const RC::Vector3 f = Forward(Heading());
      const RC::Vector3 r = {f.z, 0.0f, -f.x};
      const RC::Vector3 &c = Pos();
      for (auto &fi : fish_) {
        fi.pos = {c.x + (r.x * fi.slot.x + f.x * fi.slot.z) * s.fishSpread, 0.0f,
                  c.z + (r.z * fi.slot.x + f.z * fi.slot.z) * s.fishSpread};
        fi.vel = {f.x * Speed(), 0.0f, f.z * Speed()};
        fi.yaw = Heading();
        fi.scare = 0.0f;
        fi.jumping = false;
        fi.rippleTimer = s.Rand(0.0f, 0.3f);
      }
    }

    /// @brief 群れを進め、1 匹ずつ「持ち場へ向かう・仲間を避ける・文字の下を避ける・カーソルから逃げる」を混ぜて泳がせる
    /// @param s 本体
    /// @param dt 経過秒（0 なら姿勢の書き込みだけ）
    void OnTick(TitleAmbientScript &s, float dt) override {
      using namespace TitleAmbientDetail;
      // 群れの中心はまっすぐ進む（退場判定はこの中心で行う）
      const RC::Vector3 f = Forward(Heading());
      const RC::Vector3 r = {f.z, 0.0f, -f.x};
      Translate(f.x * Speed() * dt, f.z * Speed() * dt);
      SetYaw(Heading());
      const RC::Vector3 center = Pos();
      const float speed = Speed();
      const float scale = Scale();

      const TitleBlackboard::State &bb = TitleBlackboard::Get();
      const RC::Vector3 &cursorVel = bb.CursorVel();
      const RC::Vector3 &cursorPos = bb.CursorPos();
      const float cs = std::sqrt(cursorVel.x * cursorVel.x + cursorVel.z * cursorVel.z);
      const bool cursorStirs = bb.CursorValid() && (cs > 1.0f || bb.CursorClicked());
      const float fleeR = (std::max)(s.fishFleeRadius, 0.01f);
      const float k = (dt > 0.0f) ? 1.0f : 0.0f; // dt = 0（出現の瞬間）は動かさない

      for (size_t i = 0; i < fish_.size(); ++i) {
        FishState &fi = fish_[i];
        // 持ち場（群れの中心 ＋ 進行方向ローカルのずれ）へ向かう
        const float tx = center.x + (r.x * fi.slot.x + f.x * fi.slot.z) * s.fishSpread;
        const float tz = center.z + (r.z * fi.slot.x + f.z * fi.slot.z) * s.fishSpread;
        float dx = f.x * speed + (tx - fi.pos.x) * 1.2f;
        float dz = f.z * speed + (tz - fi.pos.z) * 1.2f;

        // カーソルから逃げる（近くで動いた・クリックした瞬間に驚き、驚きが残っているあいだ逃げる）
        if (bb.CursorValid()) {
          const float ox = fi.pos.x - cursorPos.x;
          const float oz = fi.pos.z - cursorPos.z;
          const float d = std::sqrt(ox * ox + oz * oz);
          if (cursorStirs && d < fleeR && dt > 0.0f) {
            const bool fresh = fi.scare < 0.3f;
            fi.scare = 1.0f;
            if (fresh && !fi.jumping && s.Rand01() < s.fishJumpChance) StartJump(s, fi);
          }
          if (fi.scare > 0.01f && d < fleeR * 1.6f) {
            const float inv = 1.0f / (std::max)(d, 0.1f);
            const float w = s.fishFleeSpeed * fi.scare * (1.0f - d / (fleeR * 1.6f)) * 2.0f;
            dx += ox * inv * w;
            dz += oz * inv * w;
          }
        }
        // 文字・メニューの下には入らない（よける）
        RC::Vector3 n{};
        float depth = 0.0f;
        for (const auto &L : bb.Letters()) {
          if (L.visible && TitleBlackboard::CircleVsLetter(L, fi.pos.x, fi.pos.z, 0.6f, n, depth)) {
            dx += n.x * 3.0f;
            dz += n.z * 3.0f;
          }
        }
        for (const auto &L : bb.Menu()) {
          if (L.visible && TitleBlackboard::CircleVsLetter(L, fi.pos.x, fi.pos.z, 0.6f, n, depth)) {
            dx += n.x * 3.0f;
            dz += n.z * 3.0f;
          }
        }
        // 仲間とぶつからない
        for (size_t j = 0; j < fish_.size(); ++j) {
          if (j == i) continue;
          const float ox = fi.pos.x - fish_[j].pos.x;
          const float oz = fi.pos.z - fish_[j].pos.z;
          const float d = std::sqrt(ox * ox + oz * oz);
          if (d < 0.45f && d > 1e-4f) {
            dx += ox / d * (0.45f - d) * 6.0f;
            dz += oz / d * (0.45f - d) * 6.0f;
          }
        }
        // 速さの上限（驚いているほど速く泳げる）
        const float maxSp = speed * 1.6f + (std::max)(s.fishFleeSpeed - speed * 1.6f, 0.0f) * fi.scare;
        const float dl = std::sqrt(dx * dx + dz * dz);
        if (dl > maxSp && dl > 1e-4f) {
          dx *= maxSp / dl;
          dz *= maxSp / dl;
        }
        const float steer = (std::min)(1.0f, dt * (3.0f + 5.0f * fi.scare));
        fi.vel.x += (dx - fi.vel.x) * steer;
        fi.vel.z += (dz - fi.vel.z) * steer;
        fi.pos.x += fi.vel.x * dt;
        fi.pos.z += fi.vel.z * dt;
        fi.scare = (std::max)(0.0f, fi.scare - dt * 0.5f);

        const float sp = std::sqrt(fi.vel.x * fi.vel.x + fi.vel.z * fi.vel.z);
        if (sp > 0.05f) {
          const float want = std::atan2(fi.vel.x, fi.vel.z);
          fi.yaw = WrapAngle(fi.yaw + WrapAngle(want - fi.yaw) * (std::min)(1.0f, dt * 8.0f));
        }
        fi.wag += dt * kTwoPi * (2.0f + sp * 1.5f);

        // 気まぐれに跳ねる
        if (!fi.jumping && s.Rand01() < s.fishIdleJumpChance * dt) StartJump(s, fi);

        // 高さ：背中だけ水面に出す。跳ねているあいだは放物線
        const float surface = s.WaterHeight(fi.pos.x, fi.pos.z, /*ripple=*/true);
        float y = surface - s.fishSink * scale;
        float pitch = 0.0f;
        if (fi.jumping) {
          fi.jumpT += dt;
          const float dur = (std::max)(fi.jumpDur, 0.1f);
          const float u = std::clamp(fi.jumpT / dur, 0.0f, 1.0f);
          y = surface + fi.jumpH * 4.0f * u * (1.0f - u);
          const float vy = fi.jumpH * 4.0f * (1.0f - 2.0f * u) / dur;
          pitch = std::atan2(vy, (std::max)(sp, 1.0f)) * 0.8f;
          if (u >= 1.0f) {
            fi.jumping = false;
            Splash(s, fi.pos.x, fi.pos.z);
          }
        }

        // 泳いだ跡の小さな波紋（全体の波源の枠を食わないよう間引く）
        fi.rippleTimer -= dt;
        if (k > 0.0f && fi.rippleTimer <= 0.0f && !fi.jumping && sp > 0.8f) {
          RC::AddWaveSourceAtWorld(fi.pos.x, fi.pos.z, 0.004f, -0.012f * (std::min)(sp / 3.0f, 1.5f));
          fi.rippleTimer = s.Rand(0.22f, 0.32f);
        }

        // 置く：胴 ＋ 尾びれ（尾びれは左右に振る）
        const size_t bi = i * 2, ti = i * 2 + 1;
        if (ti >= members_.size()) continue;
        const Mat3 body = BodyMatrix(fi.yaw, pitch, 0.0f);
        const RC::Vector3 bodyPos = {fi.pos.x, y, fi.pos.z};
        PlacePart(members_[bi], bodyPos, body);
        const float wagAng = (0.35f + 0.25f * fi.scare) * std::sin(fi.wag);
        PlaceAttached(members_[ti], bodyPos, body, RotY(wagAng));
      }
    }

  private:
    /// @brief 小魚 1 匹の状態（不変条件の無い値の束なので struct のまま）
    struct FishState {
      RC::Vector3 pos{};        ///< 位置（xz。y は跳ねた高さだけ使う）
      RC::Vector3 vel{};        ///< 速度（xz、m/s）
      RC::Vector3 slot{};       ///< 群れの中での持ち場（群れの中心からの xz、進行方向ローカル）
      float yaw = 0.0f;         ///< 向き（rad）
      float wag = 0.0f;         ///< 尾びれの位相
      float scare = 0.0f;       ///< 驚き（1 で全力で逃げる。だんだん 0 へ）
      bool jumping = false;     ///< 跳ねている最中か
      float jumpT = 0.0f;       ///< 跳ねてからの秒数
      float jumpDur = 0.5f;     ///< 跳ねている秒数
      float jumpH = 0.5f;       ///< 跳ねる高さ（m）
      float rippleTimer = 0.0f; ///< 次に小さな波紋を出すまでの秒数
    };

    /// @brief 小魚を跳ねさせる（水面の小さな水しぶき）
    /// @param s 本体
    /// @param fi 小魚
    static void StartJump(TitleAmbientScript &s, FishState &fi) {
      fi.jumping = true;
      fi.jumpT = 0.0f;
      fi.jumpDur = s.Rand(0.35f, 0.55f);
      fi.jumpH = s.fishJumpHeight * s.Rand(0.6f, 1.2f);
      Splash(s, fi.pos.x, fi.pos.z);
    }

    /// @brief 小魚が水を出入りするときの小さな水しぶきと波紋
    /// @param s 本体
    /// @param x ワールド X
    /// @param z ワールド Z
    static void Splash(TitleAmbientScript &s, float x, float z) {
      for (int i = 0; i < 3; ++i) {
        RC::EffectParticleSpawn sp;
        sp.kind = RC::EffectParticleKind::Wake;
        sp.position = {x, s.view_.waterY + 0.03f, z};
        sp.scale = s.Rand(0.03f, 0.06f);
        sp.color = {0.9f, 0.95f, 1.0f, 0.9f};
        RC::SpawnEffectParticle(sp);
      }
      RC::AddWaveSourceAtWorld(x, z, 0.006f, -0.04f);
    }

    std::vector<FishState> fish_; ///< 1 匹ずつの状態
    std::vector<Part> members_;   ///< 1 匹ずつの胴・尾（i*2: 胴 / i*2+1: 尾）
    int target_ = 0;              ///< プールを作り終えたときの数（作り終えるまで出さない）
  };

  // ==================================================================
  // 設定（JSON / Inspector。どれも private。読み書きは Serialize / Deserialize / OnImGui だけ）
  // ==================================================================

  // ---- 全体 ----
  bool enabled = true;
  float firstDelay = 2.0f;
  float intervalMin = 6.0f;
  float intervalMax = 12.0f;
  int maxActive = 3;
  float screenMargin = 2.0f;
  // ---- 航跡波（サメ・船共通）----
  float wakeAngleDeg = 19.5f;     ///< V の半開き角（度）。
  float wakeBowStrength = 0.008f; ///< 船首のへこみ：毎フレームの押し下げ量（1.5 m/s 時）
  float wakeBowRadius = 0.012f;   ///< 船首のへこみの半径（UV。0.01 = 1m）
  float wakeArmStrength = 0.008f; ///< V の山：根元の毎フレーム加算量（1.5 m/s 時）
  float wakeArmMin = 0.003f;      ///< V の山：末端の加算量
  float wakeArmRadius = 0.006f;   ///< V の山の半径（UV）。波紋テクスチャ 1 テクセル ≒ 0.0039
  float wakeSpacing = 0.8f;       ///< 道筋を記録する間隔（m）
  // ---- 引き波の泡（白波。サメ・船共通）----
  // 波紋テクスチャの G チャンネル（RenderInteractiveWater の泡）へ置く。泡は伝播せずその場に残り、
  // 滲みながら消えるので、船が通ったあとに白い帯が尾を引く。量は「1 秒あたり」で、速さに比例させる
  // （速さが変わってもテクセル 1 つが受け取る量 ≒ 同じ＝帯の白さが揃う）。値は 1.5 m/s のとき。
  bool foamEnabled = true;
  // 白は 2 種類：
  //   引き波（長く残る）… 船尾がかき回した白濁の帯。V の内側の中心線に細く残る
  //   波頭（すぐ消える）… 船首の砕け波と V の腕の白い線。毎フレーム今の位置に置き直すので V の形を保つ
  float foamStern = 1.0f;          ///< 船尾の引き波：中心の量 / 秒（1.5 m/s 時）
  float foamSternRadius = 0.006f;  ///< 船尾の引き波の半径（UV。0.01 = 1m）。船体の半幅が広ければそちらに合わせる
  float foamBow = 0.22f;           ///< 船首の砕け波：1 フレームの量（@60fps。定常値 ≒ 量 / (1 - crestDecay)）
  float foamBowRadius = 0.004f;    ///< 船首の砕け波の半径（UV）
  float foamArm = 0.16f;           ///< V の腕の白線：根元の 1 フレームの量（@60fps。先へ行くほど 0）
  float foamArmLength = 0.7f;      ///< V の腕のうち白線が見える範囲（V の長さに対する割合）
  float foamArmRadius = 0.0035f;   ///< V の腕の白線の半径（UV）。テクスチャ 1 テクセル ≒ 0.0039
  float foamArmStep = 0.4f;        ///< 白線を描く点の間隔（m）。道筋の記録点の間を補間して線を途切れさせない
  float foamDecay = 0.992f;        ///< 引き波の毎フレーム残存率（0.992 で約 1.4 秒で半減 @60fps）
  float foamSpread = 0.10f;        ///< 引き波の毎フレームの滲み（大きいほど後ろで帯が広がる）
  float crestDecay = 0.80f;        ///< 波頭（船首・腕）の毎フレーム残存率（小さいほど線がくっきり）

  // ---- サメ ----
  KindParams shark = MakeShark();
  /// @brief 真上から遠目に見るだけなので軽量版（shark.obj を 110k → 10k 三角形に簡略化）
  /// @details 形式は glb（Assimp の OBJ 読み込みはテキスト解析が重く、同じ中身で 10 倍以上遅い）。
  ///          テクスチャも 4096 の JPG ではなく 512 のミップ付き DDS（shark_lod_basecolor.dds）。
  ///          モデルのテクスチャは初回描画時にメインスレッドで同期ロードされるため、
  ///          4096 の JPG だとデコード＋ミップ生成でその 1 フレームが大きく止まる。
  std::string sharkModelPath = "Resources/model/shark/shark_lod.glb";
  float sharkModelYawOffsetDeg = -90.0f; ///< モデルの前（+X）をスクリプトの前（+Z）へ合わせる補正
  float sharkSink = 0.22f;               ///< モデル中心を水面からどれだけ沈めるか（m）。小さいほど背中が出る
  float sharkWaveFollow = 0.7f;          ///< 波の上下へどれだけ追従するか（0: 静水面 / 1: 完全追従）
  float sharkSwayDeg = 12.0f;            ///< 進路の蛇行（左右に振れる角度）
  float sharkSwayPeriod = 7.0f;          ///< 蛇行の周期（秒）
  float sharkWagDeg = 7.0f;              ///< 尾振り（体の首振り角）
  float sharkWagFrequency = 1.1f;        ///< 尾振りの回数（Hz）
  bool sharkWake = true;                 ///< 頭の位置から航跡波を出す
  float sharkWakeLength = 8.0f;          ///< V の長さ（m）
  float sharkFoam = 0.7f;                ///< 白波の量の倍率（背ビレと背中しか出ていないので船より控えめ）

  // ---- 漂流物 ----
  KindParams drift = MakeDrift();
  float driftTiltScale = 0.9f;  ///< 水面の傾きへの追従
  float driftSpinDeg = 8.0f;    ///< ゆっくり回る速さ（度/秒の最大値）
  float driftFloatBias = 0.0f;  ///< 浮かせ量の全体補正（m）

  // ---- カモメ ----
  KindParams gull = MakeGull();
  /// @brief カモメのモデル（空ならプリミティブの組み合わせ）。胴の半長 = 1 の単位で作ってある
  std::string gullModelPath = "Resources/model/bird/bird.glb";
  float gullHeightMin = 8.0f;   ///< 水面からの高さ（m）
  float gullHeightMax = 11.0f;
  float gullFlapFrequency = 2.4f;
  float gullFlapDeg = 32.0f;
  float gullDihedralDeg = 8.0f; ///< 滑空中に翼を持ち上げておく角度
  float gullGlideChance = 0.45f; ///< 2 秒ごとに滑空へ移る確率
  int gullFlockMax = 3;         ///< 群れの最大数（1 なら常に単独）
  float gullTurnDeg = 6.0f;     ///< 旋回の速さ（度/秒の最大値）

  // ---- 船 ----
  KindParams ship = MakeShip();
  /// @brief 船のモデル（空ならプリミティブの組み合わせ）。m 単位・全長約 10.6m で作ってあるので kShipModelScale で縮める
  std::string shipModelPath = "Resources/model/ship/ship.glb";
  float shipTiltScale = 0.8f;
  bool shipWake = true;                  ///< 船首から航跡波を出す
  float shipWakeLength = 10.0f;          ///< V の長さ（m）
  float shipFoam = 1.0f;                 ///< 泡の量の倍率
  RC::Vector4 shipHullColor = {0.36f, 0.22f, 0.12f, 1.0f};
  RC::Vector4 shipSailColor = {0.96f, 0.94f, 0.88f, 1.0f};

  // ---- サメのジャンプ ----
  bool sharkJump = true;           ///< たまに水面から跳ねる
  float sharkJumpChance = 0.35f;   ///< 画面の内側にいるあいだ、1 秒あたりに跳ぶ確率
  float sharkJumpHeight = 2.2f;    ///< 跳ぶ高さ（m。全長 4m のとき。全長に比例）
  float sharkJumpDuration = 1.3f;  ///< 水を出てから戻るまでの秒数
  float sharkJumpCooldown = 4.0f;  ///< 跳んだあと次に跳べるまでの秒数
  int sharkJumpMax = 1;            ///< 1 回の横断で跳ぶ最大回数
  float sharkJumpSplash = 1.0f;    ///< 跳ぶ・落ちるときの水しぶきの量の倍率

  // ---- 小魚の群れ ----
  KindParams fish = MakeFish();
  int fishCount = 10;              ///< 1 群れの数（プールを作るときに使う）
  float fishSpread = 1.6f;         ///< 群れの広がり（半径 m）
  float fishFleeRadius = 4.0f;     ///< カーソルがこの距離に入ると逃げる（m）
  float fishFleeSpeed = 5.5f;      ///< 逃げるときの最高速度（m/s）
  float fishJumpChance = 0.5f;     ///< 驚いたときに跳ねる確率（1 匹ごと）
  float fishIdleJumpChance = 0.03f; ///< 驚いていなくても跳ねる確率（1 秒・1 匹あたり）
  float fishJumpHeight = 0.6f;     ///< 跳ねる高さ（m）
  float fishSink = 0.03f;          ///< 体の中心を水面からどれだけ沈めるか（m）。背中だけ出る
  RC::Vector4 fishColor = {0.16f, 0.22f, 0.30f, 1.0f};

  // ---- ぶつかり（船・漂流物 ⇔ タイトル文字 / 漂流物どうし）----
  bool contactEnabled = true;       ///< ぶつかりを有効にする
  float contactLetterShake = 0.14f; ///< 船が文字に当たった瞬間の文字の揺れ（rad）
  float contactSplash = 1.0f;       ///< ぶつかったときの水しぶきの量の倍率（0 で出さない）
  float contactRipple = 0.12f;      ///< ぶつかったときの波紋の強さ（m）
  float driftRestitution = 0.45f;   ///< 漂流物の跳ね返り係数（0: くっつく / 1: 完全に弾む）

  // ---- カーソルの波で揺れる（船・漂流物）----
  bool cursorRock = true;           ///< カーソルの動きで船・漂流物を揺らす
  float cursorRockRadius = 5.0f;    ///< カーソルの波が届く距離（m）
  float cursorRockStrength = 1.4f;  ///< 揺れの強さ（角加速度 rad/s²。カーソル 10 m/s・距離 0 のとき）
  float cursorClickRock = 0.5f;     ///< クリックした瞬間の揺れ（角速度 rad/s。距離 0 のとき）
  float cursorPush = 0.6f;          ///< 漂流物をカーソルの波で押し流す強さ（m/s²）
  float rockFrequency = 0.55f;      ///< 揺れの固有振動数（Hz）
  float rockDamping = 0.18f;        ///< 揺れの減衰比（小さいほど長く揺れる）

  // ------------------------------------------------------------------
  // 既定値
  // ------------------------------------------------------------------

  /// @brief サメの既定設定
  /// @return 種類ごとの設定
  static KindParams MakeShark() {
    KindParams p;
    p.weight = 1.4f;
    p.maxActive = 1;
    p.poolSize = 1;
    p.speedMin = 2.2f;
    p.speedMax = 3.4f;
    p.scaleMin = 3.6f; // shark.obj は全長 1 なので scale ＝ 全長（m）
    p.scaleMax = 4.6f;
    return p;
  }
  /// @brief 漂流物の既定設定
  /// @return 種類ごとの設定
  static KindParams MakeDrift() {
    KindParams p;
    p.weight = 0.6f;
    p.maxActive = 2; // 2 つ同じ帯に入ると、向きによってはぶつかって弾き合う
    p.poolSize = 3; // 木箱・樽・流木を 1 つずつ
    p.speedMin = 0.6f;
    p.speedMax = 1.0f;
    p.scaleMin = 0.9f;
    p.scaleMax = 1.2f;
    // タイトル文字（z ≒ 1.3〜7.0）とメニュー（z ≒ -7.5〜-2.9）の間と、画面上端の帯
    p.lanes = {{-2.8f, 1.2f}, {7.3f, 9.4f}};
    return p;
  }
  /// @brief カモメの既定設定
  /// @return 種類ごとの設定
  static KindParams MakeGull() {
    KindParams p;
    p.weight = 0.9f;
    p.maxActive = 3;
    p.poolSize = 3;
    p.speedMin = 4.5f;
    p.speedMax = 6.5f;
    p.scaleMin = 0.45f; // 胴の長さの基準。0.45 で翼幅 ≒ 2m
    p.scaleMax = 0.55f;
    return p;
  }
  /// @brief 船の既定設定
  /// @return 種類ごとの設定
  static KindParams MakeShip() {
    KindParams p;
    p.weight = 0.5f;
    p.maxActive = 1;
    p.poolSize = 1;
    p.speedMin = 1.2f;
    p.speedMax = 1.8f;
    p.scaleMin = 0.9f;
    p.scaleMax = 1.0f;
    // 文字とメニューの間の帯。上側は文字の足元に少しかかる（たまに文字を押しのけて進む）
    p.lanes = {{-2.8f, 2.4f}};
    return p;
  }
  /// @brief 小魚の群れの既定設定
  /// @return 種類ごとの設定
  static KindParams MakeFish() {
    KindParams p;
    p.weight = 0.9f;
    p.maxActive = 1;
    p.poolSize = 1;
    p.speedMin = 1.3f;
    p.speedMax = 2.0f;
    p.scaleMin = 0.9f; // 1 で全長 ≒ 0.4m
    p.scaleMax = 1.15f;
    return p;
  }

  // ------------------------------------------------------------------
  // JSON
  // ------------------------------------------------------------------

  /// @brief j[key] が数値なら float として読み込む（無い・型違いなら out はそのまま）
  /// @param j 読み込み元の JSON
  /// @param key キー名
  /// @param out 読み込み先
  static void ReadF(const nlohmann::json &j, const char *key, float &out) {
    if (j.contains(key) && j[key].is_number()) out = j[key].get<float>();
  }
  /// @brief j[key] が数値なら int として読み込む（無い・型違いなら out はそのまま）
  /// @param j 読み込み元の JSON
  /// @param key キー名
  /// @param out 読み込み先
  static void ReadI(const nlohmann::json &j, const char *key, int &out) {
    if (j.contains(key) && j[key].is_number()) out = j[key].get<int>();
  }
  /// @brief j[key] が真偽値なら読み込む（無い・型違いなら out はそのまま）
  /// @param j 読み込み元の JSON
  /// @param key キー名
  /// @param out 読み込み先
  static void ReadB(const nlohmann::json &j, const char *key, bool &out) {
    if (j.contains(key) && j[key].is_boolean()) out = j[key].get<bool>();
  }
  /// @brief j[key] が文字列なら読み込む（無い・型違いなら out はそのまま）
  /// @param j 読み込み元の JSON
  /// @param key キー名
  /// @param out 読み込み先
  static void ReadS(const nlohmann::json &j, const char *key, std::string &out) {
    if (j.contains(key) && j[key].is_string()) out = j[key].get<std::string>();
  }
  /// @brief j[key] が要素 4 つ以上の配列なら Vector4 として読み込む（それ以外は out はそのまま）
  /// @param j 読み込み元の JSON
  /// @param key キー名
  /// @param out 読み込み先
  static void ReadVec4(const nlohmann::json &j, const char *key, RC::Vector4 &out) {
    if (!j.contains(key)) return;
    const auto &c = j[key];
    if (c.is_array() && c.size() >= 4) {
      out = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>(), c[3].get<float>()};
    }
  }
  /// @brief 種類ごとの共通設定を JSON にする
  /// @param p 書き出す設定
  /// @return JSON オブジェクト
  static nlohmann::json WriteKind(const KindParams &p) {
    nlohmann::json lanes = nlohmann::json::array();
    for (const auto &l : p.lanes) lanes.push_back({l.x, l.y});
    return {{"enabled", p.enabled},   {"weight", p.weight},     {"maxActive", p.maxActive},
            {"poolSize", p.poolSize}, {"speedMin", p.speedMin}, {"speedMax", p.speedMax},
            {"scaleMin", p.scaleMin}, {"scaleMax", p.scaleMax}, {"lanes", lanes}};
  }
  /// @brief JSON から種類ごとの共通設定を読み込む（無いキーは現在値のまま）
  /// @param j 読み込み元の JSON
  /// @param p 読み込み先
  /// @note lanes は要素 2 つ未満のものを捨て、各帯を [小, 大] の順に並べ直す
  static void ReadKind(const nlohmann::json &j, KindParams &p) {
    ReadB(j, "enabled", p.enabled);
    ReadF(j, "weight", p.weight);
    ReadI(j, "maxActive", p.maxActive);
    ReadI(j, "poolSize", p.poolSize);
    ReadF(j, "speedMin", p.speedMin);
    ReadF(j, "speedMax", p.speedMax);
    ReadF(j, "scaleMin", p.scaleMin);
    ReadF(j, "scaleMax", p.scaleMax);
    if (j.contains("lanes") && j["lanes"].is_array()) {
      p.lanes.clear();
      for (const auto &l : j["lanes"]) {
        if (!l.is_array() || l.size() < 2) continue;
        const float a = l[0].get<float>(), b = l[1].get<float>();
        p.lanes.push_back({(std::min)(a, b), (std::max)(a, b)});
      }
    }
  }

  // ------------------------------------------------------------------
  // 乱数・集計
  // ------------------------------------------------------------------

  /// @brief a〜b の一様乱数
  /// @param a 下限
  /// @param b 上限
  /// @return 乱数（b <= a なら a）
  float Rand(float a, float b) {
    if (b <= a) return a;
    std::uniform_real_distribution<float> d(a, b);
    return d(rng_);
  }
  /// @brief 0〜1 の一様乱数
  /// @return 乱数
  float Rand01() { return Rand(0.0f, 1.0f); }

  /// @brief 種類ごとの共通設定を引く
  /// @param k 種類
  /// @return 設定への参照
  /// @details 種類 → 設定 の表引き（以前は switch。振る舞いの分岐ではなく対応表なので配列にした）
  KindParams &Params(Kind k) {
    KindParams *const table[] = {&shark, &drift, &gull, &ship, &fish};
    const size_t i = static_cast<size_t>(k);
    return (i < static_cast<size_t>(Kind::Count)) ? *table[i] : ship;
  }

  /// @brief 出ている数を数える（引数なしの版は全種の合計）
  /// @param k 種類
  /// @return 出ている数
  int CountActive(Kind k) const {
    int n = 0;
    for (const auto &a : actors_) n += (a->IsActive() && a->GetKind() == k) ? 1 : 0;
    return n;
  }
  /// @brief 出ている数（全種の合計）
  /// @return 出ている数
  int CountActive() const {
    int n = 0;
    for (const auto &a : actors_) n += a->IsActive() ? 1 : 0;
    return n;
  }
  /// @brief プールにある種類 k の数（出ているものも含む）
  /// @param k 種類
  /// @return プールの数
  int CountPool(Kind k) const {
    int n = 0;
    for (const auto &a : actors_) n += (a->GetKind() == k) ? 1 : 0;
    return n;
  }

  // ------------------------------------------------------------------
  // 環境（カメラ・水面）
  // ------------------------------------------------------------------

  /// @brief 真上を向いて落ち着いたメインカメラから画面範囲を覚える
  /// @details 浮上演出の途中（水中・斜め向き）の画角で出現位置を決めると画面内に湧いてしまうので、
  ///          真下向き・水面より十分上にいるときだけ採用する。6 秒待っても条件を満たさなければ
  ///          （カメラ構成を変えた場合など）その時点の位置で妥協する。
  /// @param scene 対象のシーン
  /// @param dt 経過秒（待ち時間の計測に使う）
  void TryCacheView(Scene *scene, float dt) {
    viewWait_ += dt;
    // メインカメラを探す
    std::shared_ptr<Entity> camE;
    for (const auto &e : scene->GetEntities()) {
      if (!e || !e->IsActive()) continue;
      auto *cam = e->GetComponent<CameraComponent>();
      if (cam && cam->isMain && e->GetComponent<TransformComponent>()) {
        camE = e;
        break;
      }
    }
    if (!camE) return;
    auto *tr = camE->GetComponent<TransformComponent>();
    auto *cam = camE->GetComponent<CameraComponent>();

    // 水面の高さ（最初に見つかった水面の Transform）
    float waterY = 0.0f;
    for (const auto &e : scene->GetEntities()) {
      if (!e || !e->GetComponent<WaterComponent>()) continue;
      if (auto *wtr = e->GetComponent<TransformComponent>()) waterY = wtr->position.y;
      break;
    }

    // 真下向き（rotation.x ≒ π/2）かつ水面から 8m より上なら採用。6 秒過ぎたら条件を満たさなくても採用する
    const bool lookingDown = std::fabs(tr->rotation.x - TitleAmbientDetail::kHalfPi) < 0.05f;
    const bool highEnough = (tr->position.y - waterY) > 8.0f;
    if (!(lookingDown && highEnough) && viewWait_ < 6.0f) return;

    // 水面の高さでの画面範囲を求める
    view_.cx = tr->position.x;
    view_.cz = tr->position.z;
    view_.camY = (std::max)(tr->position.y, waterY + 8.0f);
    view_.waterY = waterY;
    view_.tanHalf = std::tan((std::max)(cam->fovY, 0.05f) * 0.5f);
    auto &rc = RC::GetRenderContext();
    if (rc.Ctx() && rc.Ctx()->app && rc.Ctx()->app->height > 0) {
      view_.aspect = static_cast<float>(rc.Ctx()->app->width) / static_cast<float>(rc.Ctx()->app->height);
    }
    view_.halfH = view_.HalfHAt(waterY);
    view_.halfW = view_.HalfWAt(waterY);
    view_.valid = true;
    Log::Print("[TitleAmbientScript] view " + std::to_string(view_.halfW * 2.0f) + " x " +
               std::to_string(view_.halfH * 2.0f) + " m");
  }

  /// @brief シーン内で最初に見つかった水面から波のパラメータを作る
  /// @param scene 対象のシーン
  /// @param out 作った波のパラメータ
  /// @return 水面があれば true
  static bool BuildWaterParams(Scene *scene, RC::WaterWaveParams &out) {
    for (const auto &e : scene->GetEntities()) {
      if (!e) continue;
      auto *w = e->GetComponent<WaterComponent>();
      if (!w) continue;
      auto *tr = e->GetComponent<TransformComponent>();
      out = w->ToWaveParams(tr ? tr->position.y : 0.0f);
      return true;
    }
    return false;
  }

  /// @brief 水面の高さ（Gerstner。ripple=true なら波紋も足す）
  /// @param x ワールド X
  /// @param z ワールド Z
  /// @param ripple 波紋（マウス・航跡）にも乗せるか。航跡を出す本人（サメ・船）は false にすること：
  ///               自分の航跡（船首直下の 30cm 級のへこみ。1〜2 フレーム遅れで届き、0.4m ごとに
  ///               パルス状に入る）を拾うと前後左右の 4 点差分が暴れてガクガク揺れる
  /// @return 水面の高さ（水面が無ければ view_.waterY）
  float WaterHeight(float x, float z, bool ripple = false) const {
    if (!hasWater_) return view_.waterY;
    // 見た目だけなので逆解きは 1 回で十分（部位ごとに何点も引くため軽くしておく）
    float h = RC::WaterSurface::SampleHeight(water_, waterTime_, x, z, nullptr, 0, 1.0f, 3.0f, 1);
    // 波紋は TitleScreenScript が読み戻しを有効にしているときだけ乗る（無効なら 0）
    if (ripple) h += RC::SampleInteractiveWaterHeight(x, z);
    return h;
  }

  /// @brief 前後・左右の 4 点の水面から、中心の高さとピッチ・ロールを求める
  /// @param c 中心位置（xz を使う）
  /// @param yaw 向き（rad）
  /// @param halfLen 前後の点までの距離（m）
  /// @param halfWid 左右の点までの距離（m）
  /// @param outY 4 点の平均の高さ
  /// @param outPitchUp ピッチ（rad。前が高いと正）
  /// @param outRoll ロール（rad。右 = (f.z, 0, -f.x) 側が高いと正）
  /// @param ripple 波紋も乗せるか（WaterHeight 参照）
  void WaterPose(const RC::Vector3 &c, float yaw, float halfLen, float halfWid, float &outY,
                 float &outPitchUp, float &outRoll, bool ripple = false) const {
    const RC::Vector3 f = TitleAmbientDetail::Forward(yaw);
    const RC::Vector3 r = {f.z, 0.0f, -f.x};
    const float hF = WaterHeight(c.x + f.x * halfLen, c.z + f.z * halfLen, ripple);
    const float hB = WaterHeight(c.x - f.x * halfLen, c.z - f.z * halfLen, ripple);
    const float hR = WaterHeight(c.x + r.x * halfWid, c.z + r.z * halfWid, ripple);
    const float hL = WaterHeight(c.x - r.x * halfWid, c.z - r.z * halfWid, ripple);
    outY = (hF + hB + hR + hL) * 0.25f;
    outPitchUp = std::atan2(hF - hB, 2.0f * halfLen);
    outRoll = std::atan2(hR - hL, 2.0f * halfWid);
  }

  // ------------------------------------------------------------------
  // プール生成
  // ------------------------------------------------------------------

  /// @brief プールの生成予定を積む（実際の生成は BuildNextPooled で 1 フレーム 1 体ずつ）
  /// @details 一度に全部作ると Debug ビルドで 1 フレーム 90ms ほど止まっていた（プリミティブ 27 個ぶん）。
  ///          最初の出現は firstDelay 秒後なので、そのあいだに分けて作れば間に合う。
  ///          サメは非同期ロードを早く始めたいので先頭に置く。
  /// @param scene 対象のシーン（まとめ用のフォルダエンティティをここに作る）
  void BuildPools(Scene *scene) {
    poolBuilt_ = true;
    auto folder = scene->CreateEntity("TitleAmbient");
    if (folder) {
      folder->SetIsFolder(true);
      folder->SetTag("transient", 1);
      folder_ = folder;
    }
    buildQueue_.clear();
    for (int i = 0; i < (std::max)(shark.poolSize, 0); ++i) buildQueue_.push_back({Kind::Shark, 0});
    for (int i = 0; i < (std::max)(drift.poolSize, 0); ++i) buildQueue_.push_back({Kind::Drift, i % 3});
    for (int i = 0; i < (std::max)(gull.poolSize, 0); ++i) buildQueue_.push_back({Kind::Gull, 0});
    for (int i = 0; i < (std::max)(ship.poolSize, 0); ++i) buildQueue_.push_back({Kind::Ship, 0});
    // 小魚は 1 フレーム 1 匹ずつ（群れ全部を 1 フレームで作ると止まる）。variant = 群れの中の番号
    const int nFish = std::clamp(fishCount, 1, 24);
    for (int i = 0; i < (std::max)(fish.poolSize, 0); ++i) {
      for (int k = 0; k < nFish; ++k) buildQueue_.push_back({Kind::Fish, k});
    }
    buildMsTotal_ = 0.0;
    buildMsMax_ = 0.0;
  }

  /// @brief 生成予定を 1 体ぶん進める
  /// @param scene 対象のシーン
  void BuildNextPooled(Scene *scene) {
    if (buildIndex_ >= buildQueue_.size()) return;
    const auto start = std::chrono::steady_clock::now();
    const PendingBuild b = buildQueue_[buildIndex_++];
    // 小魚の 2 匹目以降は、最後に作った群れへ足す（群れ全部を 1 フレームで作ると止まるため）
    FishSchoolActor *school = nullptr;
    if (b.kind == Kind::Fish && b.variant != 0 && !actors_.empty()) {
      school = dynamic_cast<FishSchoolActor *>(actors_.back().get());
    }
    if (school) {
      school->AddFish(*this, scene);
    } else {
      std::unique_ptr<AmbientActor> a = CreateActor(b.kind, b.variant);
      a->Build(*this, scene);
      actors_.push_back(std::move(a));
    }
    // 1 体ぶんの生成時間を計測し、全部作り終えたらログに出す
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    buildMsTotal_ += ms;
    buildMsMax_ = (std::max)(buildMsMax_, ms);
    if (buildIndex_ == buildQueue_.size()) {
      // モデルは非同期ロードなので、ここに出る時間はプリミティブ生成とロード要求の分だけ
      Log::Print("[TitleAmbientScript] pooled " + std::to_string(actors_.size()) + " actors over " +
                 std::to_string(buildQueue_.size()) + " frames (total " + std::to_string(buildMsTotal_) +
                 " ms, worst frame " + std::to_string(buildMsMax_) + " ms)");
    }
  }

  /// @brief 種類（と漂流物の形）から本体を作る（Factory Method）
  /// @param k 種類
  /// @param variant 漂流物の形（0 木箱 / 1 樽 / それ以外 流木）。他の種類では使わない
  /// @return 作った本体（部位はまだ無い。Build で作る）
  /// @details 「種類 → 派生クラス」の対応をこの 1 か所に集めた。呼び出し側（BuildNextPooled）は
  ///          基底のポインタを受け取るだけで、どの派生クラスかを知らずに済む。
  ///          種類を足すときはここと派生クラスを足すだけでよい。
  ///          ぶつかり・カーソルの波の対象（船・漂流物）とサメは、型付きの一覧にも登録しておく
  ///          （毎フレームのキャストや種類の判定を避けるため。一覧の順は生成順 ＝ actors_ の順）。
  std::unique_ptr<AmbientActor> CreateActor(Kind k, int variant) {
    switch (k) {
    case Kind::Shark: {
      auto a = std::make_unique<SharkActor>(*this);
      sharks_.push_back(a.get());
      return a;
    }
    case Kind::Drift: {
      std::unique_ptr<DriftActor> a;
      if (variant == 0) {
        a = std::make_unique<CrateDrift>();
      } else if (variant == 1) {
        a = std::make_unique<BarrelDrift>();
      } else {
        a = std::make_unique<LogDrift>();
      }
      floating_.push_back(a.get());
      return a;
    }
    case Kind::Gull:
      return std::make_unique<GullActor>();
    case Kind::Fish:
      return std::make_unique<FishSchoolActor>(std::clamp(fishCount, 1, 24));
    default: {
      auto a = std::make_unique<ShipActor>();
      floating_.push_back(a.get());
      return a;
    }
    }
  }

  /// @brief 部位エンティティを 1 つ作る（最初は非アクティブ）
  /// @param scene 対象のシーン
  /// @param name エンティティ名
  /// @return 作ったエンティティ（失敗したら nullptr）
  /// @note 画面に映らないよう y = -500 に極小の scale で置き、まとめ用フォルダの子にする
  std::shared_ptr<Entity> MakeEntity(Scene *scene, const std::string &name) {
    auto e = scene->CreateEntity(name);
    if (!e) return nullptr;
    if (auto f = folder_.lock()) e->SetParentGuid(f->Guid());
    e->SetTag("transient", 1); // シーン保存に含めない
    auto &tr = e->AddComponent<TransformComponent>();
    tr.position = {0.0f, -500.0f, 0.0f};
    tr.scale = {0.01f, 0.01f, 0.01f};
    return e;
  }

  /// @brief プリミティブの部位を 1 つ作る（非アクティブで作る）
  /// @param scene 対象のシーン
  /// @param name エンティティ名
  /// @param type 形状
  /// @param color 色
  /// @param offset 本体中心からの位置（本体ローカル、scale 1 のとき）
  /// @param scale 部位の大きさ（scale 1 のとき）
  /// @param out 作った部位の書き込み先
  /// @param local 本体に対する部位の向き
  /// @param side 翼の左右（+1 右 / -1 左）
  /// @return 作れたら true（false なら out はそのまま）
  bool MakePrimitivePart(Scene *scene, const std::string &name, PrimitiveType type, const RC::Vector4 &color,
                         const RC::Vector3 &offset, const RC::Vector3 &scale, Part &out,
                         const Mat3 &local = TitleAmbientDetail::Identity(), float side = 0.0f) {
    auto e = MakeEntity(scene, name);
    if (!e) return false;
    auto &pm = e->AddComponent<PrimitiveMeshComponent>();
    pm.type = type;
    pm.color = color;
    pm.shininess = 8.0f;
    // meshHandle は InitDynamicEntityRuntime が形状に合わせて作り、色もマテリアルへ反映する
    scene->InitDynamicEntityRuntime(*e);
    e->SetActive(false);
    Part p;
    p.entity = e;
    p.side = side;
    p.offset = offset;
    p.scale = scale;
    p.local = local;
    out = p;
    return true;
  }

  /// @brief スキン付きモデルの部位を 1 つ作る（非アクティブで作る）
  /// @param scene 対象のシーン
  /// @param name エンティティ名
  /// @param path モデルのパス
  /// @param clips 登録するクリップ（index は .glb 内のアニメーションの並び順）
  /// @param defaultClip 最初に再生するクリップ名
  /// @param offset 本体中心からの位置（本体ローカル、scale 1 のとき）
  /// @param scale モデルの大きさ（scale 1 のとき。一様）
  /// @param out 作った部位の書き込み先
  /// @return 作れたら true（false なら out はそのまま）
  bool MakeModelPart(Scene *scene, const std::string &name, const std::string &path,
                     const std::vector<AnimationClip> &clips, const std::string &defaultClip,
                     const RC::Vector3 &offset, float scale, Part &out) {
    auto e = MakeEntity(scene, name);
    if (!e) return false;
    auto &mr = e->AddComponent<ModelRendererComponent>();
    mr.modelPath = path;
    auto &anim = e->AddComponent<AnimationComponent>();
    anim.clips = clips;
    anim.defaultClip = defaultClip;
    scene->InitDynamicEntityRuntime(*e);
    e->SetActive(false);
    Part p;
    p.entity = e;
    p.offset = offset;
    p.scale = {scale, scale, scale};
    out = p;
    return true;
  }

  // ------------------------------------------------------------------
  // 出現
  // ------------------------------------------------------------------

  /// @brief 出せる種類の中から重み付きで抽選して出す
  /// @return 出せたら true（全体の上限に達している・どの種類も出せなければ false）
  bool TrySpawnWeighted() {
    if (CountActive() >= maxActive) return false;
    // 出せる種類だけで重み付き抽選。選んだ種類が出せなければ（レーンが埋まっている等）残りで引き直す
    std::array<bool, static_cast<size_t>(Kind::Count)> tried{};
    for (int attempt = 0; attempt < static_cast<int>(Kind::Count); ++attempt) {
      float total = 0.0f;
      for (int k = 0; k < static_cast<int>(Kind::Count); ++k) {
        if (!tried[k] && CanSpawn(static_cast<Kind>(k))) total += (std::max)(Params(static_cast<Kind>(k)).weight, 0.0f);
      }
      if (total <= 0.0f) return false;
      float r = Rand(0.0f, total);
      Kind pick = Kind::Shark;
      for (int k = 0; k < static_cast<int>(Kind::Count); ++k) {
        if (tried[k] || !CanSpawn(static_cast<Kind>(k))) continue;
        const float w = (std::max)(Params(static_cast<Kind>(k)).weight, 0.0f);
        pick = static_cast<Kind>(k);
        if (r < w) break;
        r -= w;
      }
      if (TrySpawn(pick)) return true;
      tried[static_cast<int>(pick)] = true;
    }
    return false;
  }

  /// @brief 種類 k を今出せるか（有効・上限未満・使える空きがある）
  /// @param k 種類
  /// @return 出せるなら true
  bool CanSpawn(Kind k) {
    const KindParams &p = Params(k);
    if (!p.enabled || p.weight <= 0.0f) return false;
    if (CountActive(k) >= p.maxActive) return false;
    for (const auto &a : actors_) {
      if (!a->IsActive() && a->GetKind() == k && a->IsReady()) return true;
    }
    return false;
  }

  /// @brief 種類 k の使える空きを 1 つ選ぶ
  /// @param k 種類
  /// @return 選んだ空き（無ければ nullptr）
  AmbientActor *FreeActor(Kind k) {
    // 漂流物は種類がばらけるよう、空いているものからランダムに選ぶ
    std::vector<AmbientActor *> candidates;
    for (auto &a : actors_) {
      if (!a->IsActive() && a->GetKind() == k && a->IsReady()) candidates.push_back(a.get());
    }
    if (candidates.empty()) return nullptr;
    std::uniform_int_distribution<size_t> d(0, candidates.size() - 1);
    return candidates[d(rng_)];
  }

  /// @brief 種類 k を 1 体（カモメは群れで）出す
  /// @param k 種類
  /// @return 出せたら true。失敗した理由は spawnFail_ に残す
  /// @details 出し方の違い（1 体ずつ / 群れ）は派生クラスの Spawn の override に任せる。
  bool TrySpawn(Kind k) {
    spawnFail_.clear();
    if (!CanSpawn(k)) {
      const KindParams &p = Params(k);
      if (!p.enabled || p.weight <= 0.0f) spawnFail_ = "disabled / weight 0";
      else if (CountActive(k) >= p.maxActive) spawnFail_ = "already at max active (previous one still active?)";
      else spawnFail_ = "no free actor (pool not built yet / model loading)";
      return false;
    }
    AmbientActor *a = FreeActor(k);
    if (!a) {
      spawnFail_ = "no free actor";
      return false;
    }
    return a->Spawn(*this);
  }

  /// @brief 出現位置と進行方向を決める
  /// @param a 置く本体（pos / heading / dir を書き換える）
  /// @param p 種類ごとの設定（lanes を使う）
  /// @param planeY 本体がいる平面の高さ（その高さでの画面範囲を使う）
  /// @return レーンが埋まっていて置けなければ false
  bool PlacePath(AmbientActor &a, const KindParams &p, float planeY) {
    const float hw = view_.HalfWAt(planeY);
    const float hh = view_.HalfHAt(planeY);
    const float ew = hw + screenMargin + a.Extent();
    const float eh = hh + screenMargin + a.Extent();

    if (!p.lanes.empty()) {
      // レーン：X 方向にまっすぐ横切る。水面に浮くもの同士が重ならない Z を探す
      // レーンは「物体の端まで含めて収まる帯」。幅が足りないレーンには入れない
      for (int tries = 0; tries < 8; ++tries) {
        std::uniform_int_distribution<size_t> li(0, p.lanes.size() - 1);
        const RC::Vector2 lane = p.lanes[li(rng_)];
        const float zMin = lane.x + a.LaneRadius();
        const float zMax = lane.y - a.LaneRadius();
        if (zMax < zMin) continue;
        const float z = Rand(zMin, zMax);
        if (!LaneFree(a, z)) continue;
        const bool toRight = Rand01() < 0.5f;
        const float heading = toRight ? TitleAmbientDetail::kHalfPi : -TitleAmbientDetail::kHalfPi;
        a.SetPath(view_.cx + (toRight ? -ew : ew), z, heading, TitleAmbientDetail::Forward(heading));
        return true;
      }
      return false;
    }

    // 自由：画面の内側の 1 点を通る直線。横長の画面なので横向き寄りの角度にする
    const float px = view_.cx + Rand(-0.6f, 0.6f) * hw;
    const float pz = view_.cz + Rand(-0.6f, 0.6f) * hh;
    float ang = Rand(-0.7f, 0.7f); // X 軸から ±40° 程度
    if (Rand01() < 0.5f) ang += TitleAmbientDetail::kPi;
    const float dx = std::cos(ang), dz = std::sin(ang);
    // 進行方向の逆へたどって、拡張した画面枠と交わる点を出現位置にする
    const float inf = 1e9f;
    const float tx = (dx > 1e-4f) ? (px - (view_.cx - ew)) / dx : (dx < -1e-4f) ? ((view_.cx + ew) - px) / -dx : inf;
    const float tz = (dz > 1e-4f) ? (pz - (view_.cz - eh)) / dz : (dz < -1e-4f) ? ((view_.cz + eh) - pz) / -dz : inf;
    const float t = (std::min)(tx, tz);
    a.SetPath(px - dx * t, pz - dz * t, std::atan2(dx, dz), {dx, 0.0f, dz});
    return true;
  }

  /// @brief 水面に浮くもの（漂流物・船）が同じ高さの帯を同時に通らないか
  /// @param self 置こうとしている本体（自分自身は除外する）
  /// @param z 置こうとしている Z
  /// @return 他と重ならなければ true
  bool LaneFree(const AmbientActor &self, float z) const {
    for (const FloatingActor *o : floating_) {
      if (!o->IsActive() || static_cast<const AmbientActor *>(o) == &self) continue;
      // 漂流物どうしは同じ帯に入ってよい（ぶつかって弾き合う）
      if (contactEnabled && o->GetKind() == Kind::Drift && self.GetKind() == Kind::Drift) continue;
      if (std::fabs(o->Pos().z - z) < o->LaneRadius() + self.LaneRadius()) return false;
    }
    return true;
  }

  // ------------------------------------------------------------------
  // 更新
  // ------------------------------------------------------------------

  /// @brief 1 体を更新し（中身は派生クラスの OnTick）、画面外へ出たら退場させる
  /// @param a 対象
  /// @param dt 経過秒（0 なら姿勢の書き込みだけ行い、退場判定はしない）
  void UpdateActor(AmbientActor &a, float dt) {
    a.Tick(*this, dt);
    if (dt > 0.0f && IsGone(a)) a.Deactivate();
  }

  /// @brief 画面外（拡張枠の外）へ出て、なお離れていく向きなら退場
  /// @param a 対象
  /// @return 退場させるなら true（出現から 1 秒未満は false、120 秒を超えたら必ず true）
  bool IsGone(const AmbientActor &a) const {
    if (a.Age() < 1.0f) return false;
    if (a.Age() > 120.0f) return true; // 念のため（旋回し続けて戻ってこない等）
    const float planeY = a.PlaneY(view_.waterY);
    const float ew = view_.HalfWAt(planeY) + screenMargin + a.Extent() + 0.5f;
    const float eh = view_.HalfHAt(planeY) + screenMargin + a.Extent() + 0.5f;
    const float rx = a.Pos().x - view_.cx;
    const float rz = a.Pos().z - view_.cz;
    if (std::fabs(rx) < ew && std::fabs(rz) < eh) return false;
    const RC::Vector3 f = TitleAmbientDetail::Forward(a.Yaw());
    return (rx * f.x + rz * f.z) > 0.0f;
  }

  /// @brief 航跡波を出す：船首のへこみ ＋ 通った道筋から V 字に広がる 2 本の山（＋ 白波の泡）
  /// @param trail 航跡を出す本体の道筋（ここで記録を進める）
  /// @param speed 航跡を出す本体の基準の速さ（m/s）
  /// @param bow 船首（サメは頭）のワールド位置
  /// @param fwd 進行方向（xz、正規化済み）
  /// @param length V の長さ（m）。これより古い道筋は捨てる
  /// @param stern 船尾（サメは尾）のワールド位置
  /// @param halfWidth 船体の半幅（m）。舷側と船尾の泡の広がりに使う
  /// @param foamScale 泡の量の倍率（種類ごと）
  /// @param dt 経過秒（泡の量は 1 秒あたりで指定しているため）
  /// @details 波紋シミュレーション（WaveSimulation.CS）は毎フレーム減衰が強く（速度 0.95・高さ 0.99）、
  ///          波の伝播速度（約 9 m/s）が船（1.5 m/s）よりずっと速いので、点波源を置くだけでは
  ///          物理的にケルビン波（V 字）は出ない。そこで V を毎フレーム「押し続けて」形を保つ。
  ///          V は今の向きから生やすのではなく、船首が実際に通った道筋（wakeSpacing ごとに記録）から
  ///          生やす。各点は「そこを通ったときの右方向」へ、通過後の道のり × tan(角度) だけ開く。
  ///          こうすると水に残った波として振る舞い、サメが蛇行しても V が一緒に振られない。
  ///          先端側ほど強く・末端へ向けて弱めることで、後ろへ流れて消えていくように見せる。
  ///          1 フレームに入れられる波源は全体で 64 個（マウス波紋と共用）。
  ///          1 体 = 1 + 2 × (length / wakeSpacing) 個。既定（船 10m・サメ 8m、0.8m 間隔）で 27 + 21 = 48 個。
  ///
  ///          波紋の高さだけでは真上から見てほとんど分からないので、泡（白波）も置く：
  ///            船首 … 砕け波 / 舷側 … 船腹に沿って押しのけられた水 / 船尾 … スクリューや尾が立てる白い引き波
  ///            V の腕の根元 … 船の近くだけ白く砕けて、先では普通の波に戻る
  ///          泡は伝播せずその場に残り、滲んで広がりながら消えるので、後ろへ白い帯が尾を引く。
  ///          泡の波源は高さとは別枠で 1 フレーム 64 個（1 体あたり 10〜20 個程度）。
  void Wake(WakeTrail &trail, float speed, const RC::Vector3 &bow, const RC::Vector3 &fwd, float length,
            const RC::Vector3 &stern, float halfWidth, float foamScale, float dt) {
    // 強さは 1.5 m/s を基準に速さへ比例させる（速いほどはっきり）
    const float speedScale = std::clamp(speed / 1.5f, 0.75f, 2.0f);
    const float L = (std::max)(length, 0.1f);
    const float spacing = (std::max)(wakeSpacing, 0.2f);

    // 道のりを積算し、spacing ごとに道筋を記録する（V の長さより古い道筋は捨てる）
    trail.Advance(bow, fwd, spacing, L);
    const std::vector<WakeSample> &samples = trail.Samples();
    const float wakePath = trail.Path();

    // 船首のへこみ
    if (wakeBowStrength > 0.0f) {
      if (TitleAmbientDetail::PushWaveSource(bow.x, bow.z, wakeBowRadius, -wakeBowStrength * speedScale)) {
        ++wakeSourcesThisFrame_;
      }
    }

    // V の腕（高さ）：各道筋点から、通過後の道のり d に応じて左右へ d·tan(角度) だけ開いた位置を押し上げる
    const float tanA = std::tan(std::clamp(wakeAngleDeg, 1.0f, 80.0f) * TitleAmbientDetail::kDeg);
    for (const WakeSample &w : samples) {
      const float d = wakePath - w.dist;
      if (d < spacing * 0.5f) continue; // 船首の真下はへこみと重なるので飛ばす
      const float amp = ((1.0f - d / L) * wakeArmStrength + wakeArmMin) * speedScale;
      if (amp <= 0.0f) continue;
      const float side = d * tanA;
      for (float sgn : {1.0f, -1.0f}) {
        const float px = w.pos.x + w.right.x * sgn * side;
        const float pz = w.pos.z + w.right.z * sgn * side;
        if (TitleAmbientDetail::PushWaveSource(px, pz, wakeArmRadius, amp)) ++wakeSourcesThisFrame_;
      }
    }

    // ---- 白波 ----
    if (!foamEnabled || foamScale <= 0.0f || dt <= 0.0f) return;
    const RC::Vector3 right = {fwd.z, 0.0f, -fwd.x};
    const float hw = (std::max)(halfWidth, 0.05f);

    // 波頭（船首・V の腕）：すぐ消えるので「毎フレーム今の形を描く」。量は 1 フレームあたり（60fps 基準で補正）
    const float frameScale = std::clamp(dt * 60.0f, 0.0f, 3.0f);
    const float crestScale = foamScale * frameScale * std::clamp(speed / 1.5f, 0.75f, 1.5f);
    auto pushCrest = [&](float x, float z, float radius, float amount) {
      if (amount <= 0.0f) return;
      if (RC::AddCrestFoamSourceAtWorld(x, z, radius, amount)) ++crestSourcesThisFrame_;
    };

    // V の腕の白線。道筋の記録点（wakeSpacing 間隔）のままだと点線になるので、foamArmStep 間隔で補間する。
    // 腕の上の点は「道筋上の位置 ＋ そこでの右方向 × d·tan(角度)」なので、隣り合う記録点の間を線形補間すればよい。
    // いちばん新しい区間は「最新の記録点 → 今の船首（d = 0）」。船体の横は船体に隠れるが、そのまま船首から生やす。
    const float armLen = L * std::clamp(foamArmLength, 0.0f, 1.0f);
    if (armLen > 0.0f && foamArm > 0.0f) {
      const float step = (std::max)(foamArmStep, 0.1f);
      auto armPoint = [&](const RC::Vector3 &p0, const RC::Vector3 &r0, float d0, const RC::Vector3 &p1,
                          const RC::Vector3 &r1, float d1) {
        // d0（古い側）→ d1（新しい側）。armLen より古い部分は描かない
        if (d1 >= armLen) return;
        const float segLen = std::sqrt((p1.x - p0.x) * (p1.x - p0.x) + (p1.z - p0.z) * (p1.z - p0.z));
        const int n = (std::max)(1, static_cast<int>(std::ceil(segLen / step)));
        for (int i = 0; i < n; ++i) { // 終点（新しい側）は次の区間の始点なので含めない
          const float s = static_cast<float>(i) / static_cast<float>(n);
          const float d = d0 + (d1 - d0) * s;
          if (d >= armLen || d < 0.0f) continue;
          const float t = d / armLen;
          const float amount = foamArm * crestScale * (1.0f - t * t); // 根元は濃く、先で消える
          const float bx = p0.x + (p1.x - p0.x) * s;
          const float bz = p0.z + (p1.z - p0.z) * s;
          const float rx = r0.x + (r1.x - r0.x) * s;
          const float rz = r0.z + (r1.z - r0.z) * s;
          const float side = d * tanA;
          for (float sgn : {1.0f, -1.0f}) pushCrest(bx + rx * sgn * side, bz + rz * sgn * side, foamArmRadius, amount);
        }
      };
      for (size_t i = 0; i < samples.size(); ++i) {
        const WakeSample &w0 = samples[i];
        const float d0 = wakePath - w0.dist;
        if (i + 1 < samples.size()) {
          const WakeSample &w1 = samples[i + 1];
          armPoint(w0.pos, w0.right, d0, w1.pos, w1.right, wakePath - w1.dist);
        } else {
          armPoint(w0.pos, w0.right, d0, bow, right, 0.0f);
        }
      }
    }

    // 船首の砕け波：舳先のすぐ前と、左右に割れる肩
    if (foamBow > 0.0f) {
      pushCrest(bow.x + fwd.x * 0.1f, bow.z + fwd.z * 0.1f, foamBowRadius, foamBow * crestScale);
      for (float sgn : {1.0f, -1.0f}) {
        pushCrest(bow.x + right.x * sgn * hw * 0.5f - fwd.x * 0.3f, bow.z + right.z * sgn * hw * 0.5f - fwd.z * 0.3f,
                  foamBowRadius, foamBow * 0.7f * crestScale);
      }
    }

    // 船尾の引き波（長く残る）：中心に 1 つだけ。幅は船体の幅程度に抑え、後ろで滲んで広がるに任せる。
    // 量は 1 秒あたり × 速さ比例（速いほど 1 フレームに多く置くが、そのぶん 1 テクセルの上を早く通り過ぎるので
    // 帯の濃さは速さによらずほぼ揃う）。
    if (foamStern > 0.0f) {
      const float sternR = (std::max)(foamSternRadius, hw * 0.009f);
      const float amount = foamStern * foamScale * (speed / 1.5f) * dt;
      if (amount > 0.0f &&
          RC::AddFoamSourceAtWorld(stern.x - fwd.x * 0.2f, stern.z - fwd.z * 0.2f, sternR, amount)) {
        ++foamSourcesThisFrame_;
      }
    }
  }

  // ------------------------------------------------------------------
  // 外から受けた動き（揺れ・押され・ぶつかり）
  // ------------------------------------------------------------------

  /// @brief ぶつかった場所に小さな水しぶき・波紋・白波を出す
  /// @param x ワールド X
  /// @param z ワールド Z
  /// @param power 強さ（1 で船が文字に当たったくらい）
  void ContactSplash(float x, float z, float power) {
    if (contactSplash > 0.0f) {
      const int n = std::clamp(static_cast<int>(6.0f * power * contactSplash + 0.5f), 0, 24);
      for (int i = 0; i < n; ++i) {
        RC::EffectParticleSpawn sp;
        sp.kind = RC::EffectParticleKind::Wake;
        sp.position = {x + Rand(-0.2f, 0.2f), view_.waterY + 0.05f, z + Rand(-0.2f, 0.2f)};
        sp.scale = Rand(0.05f, 0.11f) * std::sqrt((std::max)(power, 0.1f));
        sp.color = {0.88f, 0.94f, 1.0f, 0.9f};
        RC::SpawnEffectParticle(sp);
      }
    }
    if (contactRipple > 0.0f) RC::AddWaveSourceAtWorld(x, z, 0.012f, -contactRipple * power);
    RC::AddCrestFoamSourceAtWorld(x, z, 0.008f, 0.5f * power);
  }

  /// @brief カーソル（TitleBlackboard）が近くで動いた・クリックしたときに、船・漂流物を揺らす（漂流物は押し流す）
  /// @param dt 経過秒
  /// @details 船は自分の航跡を拾って暴れるので波紋の高さを読まない（WaterHeight 参照）。
  ///          代わりに「カーソルの波が来た」ことを揺れのばねへの衝撃として入れる。
  ///          カーソル側の舷（前後）が先に持ち上がる向きに入れ、あとはばねで揺り返す。
  ///          届く距離・揺れやすさ・押し流されるかは FloatingActor の virtual 関数で種類ごとに決まる。
  void ApplyCursorWaves(float dt) {
    const TitleBlackboard::State &bb = TitleBlackboard::Get();
    if (!cursorRock || !bb.CursorValid() || dt <= 0.0f) return;
    using namespace TitleAmbientDetail;
    const RC::Vector3 &cursorPos = bb.CursorPos();
    const RC::Vector3 &cursorVel = bb.CursorVel();
    const bool clicked = bb.CursorClicked();
    const float R = (std::max)(cursorRockRadius, 0.1f);
    const float cs = std::sqrt(cursorVel.x * cursorVel.x + cursorVel.z * cursorVel.z);
    const float move = std::clamp(cs / 10.0f, 0.0f, 1.5f); // 10 m/s で 1
    for (FloatingActor *fa : floating_) {
      FloatingActor &a = *fa;
      if (!a.IsActive()) continue;
      const float dx = a.Pos().x - cursorPos.x;
      const float dz = a.Pos().z - cursorPos.z;
      const float d = std::sqrt(dx * dx + dz * dz);
      const float reach = R + a.CursorReachPad() * a.Scale();
      if (d >= reach) continue;
      float fall = 1.0f - d / reach;
      fall *= fall;
      const RC::Vector3 f = Forward(a.Facing());
      const RC::Vector3 r = {f.z, 0.0f, -f.x};
      // カーソル → 本体 の向き（真上にいるときは前から来たことにする）
      const float nx = (d > 1e-3f) ? dx / d : -f.x;
      const float nz = (d > 1e-3f) ? dz / d : -f.z;
      const float side = nx * r.x + nz * r.z; // 正 = カーソルが左舷側
      const float fore = nx * f.x + nz * f.z; // 正 = カーソルが後ろ
      const float light = a.WaveSensitivity(); // 軽い物ほどよく揺れる
      const float acc = cursorRockStrength * move * fall * light;
      // カーソル側が先に持ち上がる：右舷側（side < 0）なら右が上がる（roll 正）
      a.KickRoll(-side * acc * dt);
      a.KickPitch(-fore * acc * dt);
      if (clicked) {
        const float imp = cursorClickRock * fall * light;
        a.KickRoll(-side * imp);
        a.KickPitch(-fore * imp);
      }
      // 漂流物は波に押されて流れる（船は何もしない）
      a.OnCursorWave(*this, nx, nz, move, fall, dt, clicked);
    }
  }

  /// @brief 船・漂流物 ⇔ タイトル文字、漂流物 ⇔ 漂流物・船 のぶつかりを解く
  /// @param dt 経過秒
  /// @details 文字の位置は TitleScreenScript が TitleBlackboard に書いたもの。
  ///          船は重いので文字を押しのける（文字はばねで戻る）。漂流物は軽いので自分が跳ね返り、
  ///          文字はわずかに押されるだけ。触れ始めた瞬間だけ水しぶき・揺れを出す。
  ///          文字に当たったときの反応は FloatingActor::OnLetterContact の override で種類ごとに決まる。
  void ResolveContacts(float dt) {
    if (!contactEnabled || dt <= 0.0f) return;
    using namespace TitleAmbientDetail;
    const TitleBlackboard::State &bb = TitleBlackboard::Get();
    const bool lettersOn = bb.LettersInteractive();
    const std::vector<TitleBlackboard::Letter> &letters = bb.Letters();
    const size_t letterCount = (std::min)(letters.size(), static_cast<size_t>(32));
    const float e = std::clamp(driftRestitution, 0.0f, 1.0f);

    // ---- 文字 ----
    for (FloatingActor *fa : floating_) {
      FloatingActor &a = *fa;
      if (!a.IsActive()) continue;
      Circle cs[3];
      const int nc = a.HullCircles(cs);
      uint32_t touching = 0;
      for (size_t i = 0; lettersOn && i < letterCount; ++i) {
        const TitleBlackboard::Letter &L = letters[i];
        if (!L.visible) continue;
        // いちばん深くめり込んでいる円を採る
        float best = 0.0f;
        RC::Vector3 bestN{};
        int bestC = -1;
        for (int k = 0; k < nc; ++k) {
          RC::Vector3 n{};
          float depth = 0.0f;
          if (TitleBlackboard::CircleVsLetter(L, cs[k].x, cs[k].z, cs[k].r, n, depth) && depth > best) {
            best = depth;
            bestN = n;
            bestC = k;
          }
        }
        if (bestC < 0) continue;
        const uint32_t bit = 1u << i;
        touching |= bit;
        const bool first = (a.LetterContact() & bit) == 0;
        const float cx = cs[bestC].x - bestN.x * cs[bestC].r;
        const float cz = cs[bestC].z - bestN.z * cs[bestC].r;
        const RC::Vector3 f = Forward(a.Facing());
        const RC::Vector3 r = {f.z, 0.0f, -f.x};
        a.OnLetterContact(*this, i, bestN, best, first, {cx, 0.0f, cz}, f, r, e);
      }
      a.SetLetterContact(touching);
    }

    // ---- 漂流物 ⇔ 漂流物 / 漂流物 ⇔ 船 ----
    for (size_t i = 0; i < floating_.size(); ++i) {
      FloatingActor &A = *floating_[i];
      if (!A.IsActive()) continue;
      for (size_t j = i + 1; j < floating_.size(); ++j) {
        FloatingActor &B = *floating_[j];
        if (!B.IsActive()) continue;
        if (A.IsHeavy() && B.IsHeavy()) continue; // 船どうしは同じレーンに入らない
        Circle ca[3], cb[3];
        const int na = A.HullCircles(ca);
        const int nb = B.HullCircles(cb);
        float pen = 0.0f, nx = 0.0f, nz = 0.0f, px = 0.0f, pz = 0.0f;
        for (int p = 0; p < na; ++p) {
          for (int q = 0; q < nb; ++q) {
            const float dx = ca[p].x - cb[q].x;
            const float dz = ca[p].z - cb[q].z;
            const float d = std::sqrt(dx * dx + dz * dz);
            const float o = ca[p].r + cb[q].r - d;
            if (o <= pen) continue;
            pen = o;
            nx = (d > 1e-4f) ? dx / d : 1.0f; // B → A
            nz = (d > 1e-4f) ? dz / d : 0.0f;
            px = cb[q].x + nx * cb[q].r;
            pz = cb[q].z + nz * cb[q].r;
          }
        }
        if (pen <= 0.0f) continue;

        // 押し出し：重いもの（船）は動かさない。軽いものどうしは半分ずつ
        const float wa = A.IsHeavy() ? 0.0f : (B.IsHeavy() ? 1.0f : 0.5f);
        const float wb = 1.0f - wa;
        A.Translate(nx * pen * wa, nz * pen * wa);
        B.Translate(-(nx * pen * wb), -(nz * pen * wb));

        // 速度：近づく向きの成分を跳ね返す（重さは「船 ＝ 無限大」「漂流物どうし ＝ 等しい」）
        const RC::Vector3 va = A.Velocity();
        const RC::Vector3 vb = B.Velocity();
        const float vrel = (va.x - vb.x) * nx + (va.z - vb.z) * nz;
        if (vrel < 0.0f) {
          const float jimp = -(1.0f + e) * vrel;
          A.AddKnock(nx * jimp * wa, nz * jimp * wa);
          B.AddKnock(-(nx * jimp * wb), -(nz * jimp * wb));
        }

        if (A.ContactCooldown() <= 0.0f && B.ContactCooldown() <= 0.0f) {
          ContactSplash(px, pz, 0.6f);
          // 正面衝突で押し合ったまま止まらないよう、帯の上下（z）へ少し逃がす
          const float sideSign = (A.Pos().z >= B.Pos().z) ? 1.0f : -1.0f;
          A.AddKnock(0.0f, sideSign * 0.3f * wa);
          B.AddKnock(0.0f, -(sideSign * 0.3f * wb));
          // 軽いもの（漂流物）は回って揺れる。A → B の順（乱数を引く順番を保つ）
          A.OnBump(*this);
          B.OnBump(*this);
        }
      }
    }
  }

  /// @brief デバッグ用：出ているサメをすぐ跳ばせる（いなければ出す）
  void ForceSharkJump() {
    for (SharkActor *sh : sharks_) {
      if (sh->IsActive()) {
        sh->ForceJump();
        spawnNote_ = "Shark: jump";
        return;
      }
    }
    spawnNote_ = view_.valid && TrySpawn(Kind::Shark) ? "Shark: spawned (press again to jump)" : "Shark: " + spawnFail_;
  }

  // ------------------------------------------------------------------
  // 状態
  // ------------------------------------------------------------------

  std::vector<std::unique_ptr<AmbientActor>> actors_; ///< プール（生成順）。種類は知らずに基底のポインタで持つ
  std::vector<FloatingActor *> floating_;             ///< actors_ のうち船・漂流物（生成順。持ち主は actors_）
  std::vector<SharkActor *> sharks_;                  ///< actors_ のうちサメ（生成順。持ち主は actors_）
  std::weak_ptr<Entity> folder_;
  bool poolBuilt_ = false;
  /// @brief プールの生成予定 1 件（種類と漂流物の形。値の束なので struct）
  struct PendingBuild {
    Kind kind;
    int variant;
  };
  std::vector<PendingBuild> buildQueue_; ///< プールの生成予定（1 フレーム 1 体ずつ作る）
  size_t buildIndex_ = 0;
  double buildMsTotal_ = 0.0;
  double buildMsMax_ = 0.0;
  ViewRect view_;
  float viewWait_ = 0.0f;
  float spawnTimer_ = 1.0f;
  float time_ = 0.0f;
  std::mt19937 rng_{20260929u};

  RC::WaterWaveParams water_;
  bool hasWater_ = false;
  float waterTime_ = 0.0f;
  int wakeSourcesThisFrame_ = 0; ///< このフレームに入れた航跡の波源数（ImGui 表示用）
  int foamSourcesThisFrame_ = 0;  ///< このフレームに入れた引き波の泡の波源数（ImGui 表示用）
  int crestSourcesThisFrame_ = 0; ///< このフレームに入れた波頭の白波の波源数（ImGui 表示用）
  bool foamParamsSaved_ = false;  ///< 泡の残り方を上書きする前の値を控えたか
  float savedFoamDecay_ = 0.992f;
  float savedFoamSpread_ = 0.10f;
  float savedCrestDecay_ = 0.80f;
  std::string spawnFail_;        ///< 直近の TrySpawn が失敗した理由（ImGui 表示用）
  std::string spawnNote_;        ///< ImGui の Spawn Now の結果表示
};

REGISTER_SCRIPT(TitleAmbientScript)
