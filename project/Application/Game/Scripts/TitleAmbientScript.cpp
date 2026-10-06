#include "ECS/ScriptableEntity.h"
#include "ECS/ScriptRegistry.h"
#include "ECS/TransformComponent.h"
#include "ECS/CameraComponent.h"
#include "ECS/ModelRendererComponent.h"
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

#if RC_ENABLE_IMGUI
#include "imgui/imgui.h"
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
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
//       gull  : カモメ。プリミティブの組み合わせ（BirdEnemyScript と同じ作り）で羽ばたく。群れで来ることがある
//       ship  : 帆船。プリミティブの組み合わせ。波で揺れ、船首から航跡の波紋を出す
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
// ============================================================================

namespace TitleAmbientDetail {

// 行列・オイラー角の変換は共通ヘッダ（ShipVisualScript と共用）
using namespace PartPose;

/// @brief 今の Transform を描画側（PrimitiveMesh / Model）へ即座に写す
/// @details Model は DataDrivenScene がスクリプト更新後に同期するが、
///          PrimitiveMesh は次フレーム冒頭まで同期されないため 1 フレーム遅れて見える。
///          出現した瞬間に前回の位置で 1 フレーム映るのを防ぐ目的もある。
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
class TitleAmbientScript : public ScriptableEntity {
public:
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

  /// @brief 種類ごとの共通設定
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
  float shipTiltScale = 0.8f;
  bool shipWake = true;                  ///< 船首から航跡波を出す
  float shipWakeLength = 10.0f;          ///< V の長さ（m）
  float shipFoam = 1.0f;                 ///< 泡の量の倍率
  RC::Vector4 shipHullColor = {0.36f, 0.22f, 0.12f, 1.0f};
  RC::Vector4 shipSailColor = {0.96f, 0.94f, 0.88f, 1.0f};

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
    j["gull"] = jg;

    nlohmann::json jp = WriteKind(ship);
    jp["tiltScale"] = shipTiltScale;
    jp["wake"] = shipWake;
    jp["wakeLength"] = shipWakeLength;
    jp["foam"] = shipFoam;
    jp["hullColor"] = {shipHullColor.x, shipHullColor.y, shipHullColor.z, shipHullColor.w};
    jp["sailColor"] = {shipSailColor.x, shipSailColor.y, shipSailColor.z, shipSailColor.w};
    j["ship"] = jp;
    return j;
  }

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
    }
  }

#if RC_ENABLE_IMGUI
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
    kindUi("Drift", Kind::Drift, drift);
    ImGui::DragFloat("Tilt##Drift", &driftTiltScale, 0.05f, 0.0f, 2.0f);
    ImGui::DragFloat("Float Bias##Drift", &driftFloatBias, 0.01f, -1.0f, 1.0f);
    kindUi("Gull", Kind::Gull, gull);
    ImGui::DragFloatRange2("Height##Gull", &gullHeightMin, &gullHeightMax, 0.1f, 1.0f, 30.0f);
    ImGui::DragFloat("Flap Hz##Gull", &gullFlapFrequency, 0.05f, 0.1f, 8.0f);
    ImGui::DragFloat("Flap Deg##Gull", &gullFlapDeg, 0.5f, 0.0f, 70.0f);
    ImGui::DragFloat("Glide Chance##Gull", &gullGlideChance, 0.01f, 0.0f, 1.0f);
    ImGui::DragInt("Flock Max##Gull", &gullFlockMax, 1, 1, 6);
    kindUi("Ship", Kind::Ship, ship);
    ImGui::DragFloat("Tilt##Ship", &shipTiltScale, 0.05f, 0.0f, 2.0f);
    ImGui::Checkbox("Wake##Ship", &shipWake);
    ImGui::SameLine();
    ImGui::DragFloat("Len (m)##ShipWake", &shipWakeLength, 0.1f, 0.5f, 30.0f);
    ImGui::DragFloat("Foam x##Ship", &shipFoam, 0.01f, 0.0f, 5.0f);
    ImGui::TextDisabled("poolSize / modelPath / lanes are applied on scene reload");
  }
#endif

protected:
  void OnCreate() override {
    // プールは再生中の最初の Update で作る（編集モードで作ると Hierarchy が散らかるため）。
    std::random_device rd;
    rng_.seed(rd());
    spawnTimer_ = firstDelay;
  }

  void OnUpdate(float deltaTime) override {
    SceneContext *ctx = GetSceneContext();
    if (!ctx || !ctx->isPlaying() || deltaTime <= 0.0f) return;
    Scene *scene = GetScene();
    if (!scene) return;

    if (!poolBuilt_) BuildPools(scene);
    BuildNextPooled(scene);

    time_ += deltaTime;
    if (!view_.valid) TryCacheView(scene, deltaTime);

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

    for (auto &a : actors_) {
      if (!a.active) continue;
      UpdateActor(a, deltaTime);
    }

    if (!enabled || !view_.valid) return;
    spawnTimer_ -= deltaTime;
    if (spawnTimer_ > 0.0f) return;

    if (TrySpawnWeighted()) {
      spawnTimer_ = Rand(intervalMin, (std::max)(intervalMin, intervalMax));
    } else {
      spawnTimer_ = 0.5f; // 空きが無い・レーンが埋まっているときは少し待って出し直す
    }
  }

  void OnDestroy() override {
    for (auto &a : actors_) {
      for (auto &p : a.parts) {
        if (auto e = p.entity.lock()) e->Destroy();
      }
    }
    actors_.clear();
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

  enum class Kind { Shark = 0, Drift, Gull, Ship, Count };
  enum class Role { Static, WingInner, WingOuter, Flag };

  struct Part {
    std::weak_ptr<Entity> entity;
    Role role = Role::Static;
    float side = 0.0f;           ///< 翼の左右（+1 右 / -1 左）
    RC::Vector3 offset{};        ///< 本体中心からの位置（本体ローカル、scale 1 のとき）
    RC::Vector3 scale{1, 1, 1};  ///< Transform の scale（scale 1 のとき）
    Mat3 local = TitleAmbientDetail::Identity(); ///< 本体に対する部位の向き
  };

  /// @brief 航跡の道筋 1 点：船首がそこを通ったときの位置・右方向・道のり
  struct WakeSample {
    RC::Vector3 pos{};
    RC::Vector3 right{};
    float dist = 0.0f;
  };

  struct Actor {
    Kind kind = Kind::Shark;
    int variant = 0;           ///< drift: 0 木箱 / 1 樽 / 2 流木
    bool active = false;
    std::vector<Part> parts;

    RC::Vector3 pos{};         ///< 本体中心（XZ）。y はカモメだけ使う（飛ぶ高さ）
    float heading = 0.0f;      ///< 基準の進行方位（rad）
    float yaw = 0.0f;          ///< 今の向き（蛇行・旋回込み）
    float speed = 1.0f;
    float scale = 1.0f;
    float extent = 1.0f;       ///< 画面外判定に使う半径（m、scale 込み）
    float laneRadius = 1.0f;   ///< レーンで他と重ならないための半幅（m、scale 込み）
    float t = 0.0f;            ///< 出現からの秒数
    float phase = 0.0f;        ///< 動きの位相ずらし
    float turn = 0.0f;         ///< 旋回速度（rad/s）/ 漂流物の自転速度
    float spin = 0.0f;         ///< 漂流物の自転角
    RC::Vector3 dir{0, 0, 1};  ///< 出現時の進行方向（画面外へ出たかの判定用）

    // 航跡（通った道筋。Wake 参照）
    std::vector<WakeSample> wake;
    float wakePath = 0.0f;     ///< 船首が進んだ道のり（m）
    RC::Vector3 wakeLast{};    ///< 前フレームの船首位置
    bool wakeHasLast = false;

    // カモメ
    float flapPhase = 0.0f;
    float flapWeight = 1.0f;   ///< 1: 羽ばたき / 0: 滑空
    bool gliding = false;
    float glideTimer = 0.0f;
    float roll = 0.0f;
  };

  /// @brief 真上視点の画面範囲（水面の高さでの半幅・半高さ）
  struct ViewRect {
    bool valid = false;
    float cx = 0.0f, cz = 0.0f;
    float camY = 42.0f;
    float waterY = 0.0f;
    float tanHalf = 0.2288f; ///< tan(fovY / 2)
    float aspect = 16.0f / 9.0f;
    float halfW = 17.0f, halfH = 9.6f;

    /// @brief 高さ y の平面で見えている半高さ（カメラに近いほど狭い）
    float HalfHAt(float y) const { return (std::max)(camY - y, 1.0f) * tanHalf; }
    float HalfWAt(float y) const { return HalfHAt(y) * aspect; }
  };

  // ------------------------------------------------------------------
  // 既定値
  // ------------------------------------------------------------------

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
  static KindParams MakeDrift() {
    KindParams p;
    p.weight = 0.6f;
    p.maxActive = 1;
    p.poolSize = 3; // 木箱・樽・流木を 1 つずつ
    p.speedMin = 0.6f;
    p.speedMax = 1.0f;
    p.scaleMin = 0.9f;
    p.scaleMax = 1.2f;
    // タイトル文字（z ≒ 1.3〜7.0）とメニュー（z ≒ -7.5〜-2.9）の間と、画面上端の帯
    p.lanes = {{-2.8f, 1.2f}, {7.3f, 9.4f}};
    return p;
  }
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
  static KindParams MakeShip() {
    KindParams p;
    p.weight = 0.5f;
    p.maxActive = 1;
    p.poolSize = 1;
    p.speedMin = 1.2f;
    p.speedMax = 1.8f;
    p.scaleMin = 0.9f;
    p.scaleMax = 1.0f;
    p.lanes = {{-2.8f, 1.2f}}; // 文字とメニューの間の帯（帆の幅 2m を含めて収まる）
    return p;
  }

  // ------------------------------------------------------------------
  // JSON
  // ------------------------------------------------------------------

  static void ReadF(const nlohmann::json &j, const char *key, float &out) {
    if (j.contains(key) && j[key].is_number()) out = j[key].get<float>();
  }
  static void ReadI(const nlohmann::json &j, const char *key, int &out) {
    if (j.contains(key) && j[key].is_number()) out = j[key].get<int>();
  }
  static void ReadB(const nlohmann::json &j, const char *key, bool &out) {
    if (j.contains(key) && j[key].is_boolean()) out = j[key].get<bool>();
  }
  static void ReadVec4(const nlohmann::json &j, const char *key, RC::Vector4 &out) {
    if (!j.contains(key)) return;
    const auto &c = j[key];
    if (c.is_array() && c.size() >= 4) {
      out = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>(), c[3].get<float>()};
    }
  }
  static nlohmann::json WriteKind(const KindParams &p) {
    nlohmann::json lanes = nlohmann::json::array();
    for (const auto &l : p.lanes) lanes.push_back({l.x, l.y});
    return {{"enabled", p.enabled},   {"weight", p.weight},     {"maxActive", p.maxActive},
            {"poolSize", p.poolSize}, {"speedMin", p.speedMin}, {"speedMax", p.speedMax},
            {"scaleMin", p.scaleMin}, {"scaleMax", p.scaleMax}, {"lanes", lanes}};
  }
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

  float Rand(float a, float b) {
    if (b <= a) return a;
    std::uniform_real_distribution<float> d(a, b);
    return d(rng_);
  }
  float Rand01() { return Rand(0.0f, 1.0f); }

  KindParams &Params(Kind k) {
    switch (k) {
    case Kind::Shark: return shark;
    case Kind::Drift: return drift;
    case Kind::Gull: return gull;
    default: return ship;
    }
  }

  int CountActive(Kind k) const {
    int n = 0;
    for (const auto &a : actors_) n += (a.active && a.kind == k) ? 1 : 0;
    return n;
  }
  int CountActive() const {
    int n = 0;
    for (const auto &a : actors_) n += a.active ? 1 : 0;
    return n;
  }
  int CountPool(Kind k) const {
    int n = 0;
    for (const auto &a : actors_) n += (a.kind == k) ? 1 : 0;
    return n;
  }

  // ------------------------------------------------------------------
  // 環境（カメラ・水面）
  // ------------------------------------------------------------------

  /// @brief 真上を向いて落ち着いたメインカメラから画面範囲を覚える
  /// @details 浮上演出の途中（水中・斜め向き）の画角で出現位置を決めると画面内に湧いてしまうので、
  ///          真下向き・水面より十分上にいるときだけ採用する。6 秒待っても条件を満たさなければ
  ///          （カメラ構成を変えた場合など）その時点の位置で妥協する。
  void TryCacheView(Scene *scene, float dt) {
    viewWait_ += dt;
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

    float waterY = 0.0f;
    for (const auto &e : scene->GetEntities()) {
      if (!e || !e->GetComponent<WaterComponent>()) continue;
      if (auto *wtr = e->GetComponent<TransformComponent>()) waterY = wtr->position.y;
      break;
    }

    const bool lookingDown = std::fabs(tr->rotation.x - TitleAmbientDetail::kHalfPi) < 0.05f;
    const bool highEnough = (tr->position.y - waterY) > 8.0f;
    if (!(lookingDown && highEnough) && viewWait_ < 6.0f) return;

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
  /// @param ripple 波紋（マウス・航跡）にも乗せるか。航跡を出す本人（サメ・船）は false にすること：
  ///               自分の航跡（船首直下の 30cm 級のへこみ。1〜2 フレーム遅れで届き、0.4m ごとに
  ///               パルス状に入る）を拾うと前後左右の 4 点差分が暴れてガクガク揺れる
  float WaterHeight(float x, float z, bool ripple = false) const {
    if (!hasWater_) return view_.waterY;
    // 見た目だけなので逆解きは 1 回で十分（部位ごとに何点も引くため軽くしておく）
    float h = RC::WaterSurface::SampleHeight(water_, waterTime_, x, z, nullptr, 0, 1.0f, 3.0f, 1);
    // 波紋は TitleScreenScript が読み戻しを有効にしているときだけ乗る（無効なら 0）
    if (ripple) h += RC::SampleInteractiveWaterHeight(x, z);
    return h;
  }

  /// @brief 前後・左右の 4 点の水面から、中心の高さとピッチ・ロールを求める
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
    buildMsTotal_ = 0.0;
    buildMsMax_ = 0.0;
  }

  /// @brief 生成予定を 1 体ぶん進める
  void BuildNextPooled(Scene *scene) {
    if (buildIndex_ >= buildQueue_.size()) return;
    const auto start = std::chrono::steady_clock::now();
    const PendingBuild b = buildQueue_[buildIndex_++];
    switch (b.kind) {
    case Kind::Shark: actors_.push_back(BuildShark(scene)); break;
    case Kind::Drift: actors_.push_back(BuildDrift(scene, b.variant)); break;
    case Kind::Gull: actors_.push_back(BuildGull(scene)); break;
    default: actors_.push_back(BuildShip(scene)); break;
    }
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

  /// @brief 部位エンティティを 1 つ作る（最初は非アクティブ）
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

  void AddPrimitive(Scene *scene, Actor &a, const std::string &name, PrimitiveType type,
                    const RC::Vector4 &color, const RC::Vector3 &offset, const RC::Vector3 &scale,
                    const Mat3 &local = TitleAmbientDetail::Identity(), Role role = Role::Static,
                    float side = 0.0f) {
    auto e = MakeEntity(scene, name);
    if (!e) return;
    auto &pm = e->AddComponent<PrimitiveMeshComponent>();
    pm.type = type;
    pm.color = color;
    pm.shininess = 8.0f;
    // meshHandle は InitDynamicEntityRuntime が形状に合わせて作り、色もマテリアルへ反映する
    scene->InitDynamicEntityRuntime(*e);
    e->SetActive(false);
    Part p;
    p.entity = e;
    p.role = role;
    p.side = side;
    p.offset = offset;
    p.scale = scale;
    p.local = local;
    a.parts.push_back(p);
  }

  Actor BuildShark(Scene *scene) {
    Actor a;
    a.kind = Kind::Shark;
    auto e = MakeEntity(scene, "AmbientShark");
    if (!e) return a;
    auto &mr = e->AddComponent<ModelRendererComponent>();
    mr.modelPath = sharkModelPath;
    scene->InitDynamicEntityRuntime(*e);
    e->SetActive(false);
    Part p;
    p.entity = e;
    a.parts.push_back(p);
    return a;
  }

  Actor BuildDrift(Scene *scene, int variant) {
    Actor a;
    a.kind = Kind::Drift;
    a.variant = variant;
    const RC::Vector4 wood = {0.56f, 0.39f, 0.21f, 1.0f};
    const RC::Vector4 woodDark = {0.33f, 0.22f, 0.11f, 1.0f};
    const Mat3 lying = TitleAmbientDetail::RotZ(TitleAmbientDetail::kHalfPi); // 円柱の軸（Y）を X へ寝かせる
    switch (variant) {
    case 0: // 木箱：箱 ＋ 十字の帯（上から見ると板が打ち付けてあるように見える）
      AddPrimitive(scene, a, "AmbientCrate", PrimitiveType::Box, wood, {0, 0, 0}, {0.9f, 0.9f, 0.9f});
      AddPrimitive(scene, a, "AmbientCrate_band", PrimitiveType::Box, woodDark, {0, 0, 0}, {0.94f, 0.94f, 0.16f});
      AddPrimitive(scene, a, "AmbientCrate_band", PrimitiveType::Box, woodDark, {0, 0, 0}, {0.16f, 0.94f, 0.94f});
      break;
    case 1: // 樽：寝かせた円柱 ＋ たが 2 本
      AddPrimitive(scene, a, "AmbientBarrel", PrimitiveType::Cylinder, wood, {0, 0, 0}, {0.36f, 1.0f, 0.36f}, lying);
      AddPrimitive(scene, a, "AmbientBarrel_hoop", PrimitiveType::Cylinder, {0.24f, 0.24f, 0.26f, 1.0f},
                   {0.3f, 0, 0}, {0.38f, 0.07f, 0.38f}, lying);
      AddPrimitive(scene, a, "AmbientBarrel_hoop", PrimitiveType::Cylinder, {0.24f, 0.24f, 0.26f, 1.0f},
                   {-0.3f, 0, 0}, {0.38f, 0.07f, 0.38f}, lying);
      break;
    default: // 流木：細長い円柱 ＋ 折れた枝
      AddPrimitive(scene, a, "AmbientLog", PrimitiveType::Cylinder, {0.40f, 0.29f, 0.18f, 1.0f}, {0, 0, 0},
                   {0.22f, 1.8f, 0.22f}, lying);
      AddPrimitive(scene, a, "AmbientLog_branch", PrimitiveType::Cylinder, {0.36f, 0.26f, 0.16f, 1.0f},
                   {0.35f, 0.05f, 0.3f}, {0.07f, 0.6f, 0.07f},
                   TitleAmbientDetail::MatMul(TitleAmbientDetail::RotX(TitleAmbientDetail::kHalfPi),
                                           TitleAmbientDetail::RotY(0.6f)));
      break;
    }
    return a;
  }

  Actor BuildGull(Scene *scene) {
    Actor a;
    a.kind = Kind::Gull;
    const RC::Vector4 white = {0.97f, 0.97f, 0.96f, 1.0f};
    const RC::Vector4 gray = {0.74f, 0.77f, 0.80f, 1.0f};
    const RC::Vector4 tip = {0.14f, 0.15f, 0.17f, 1.0f};
    const RC::Vector4 beak = {0.98f, 0.78f, 0.18f, 1.0f};
    // 大きさは胴の長さを 1 とした値（球は半径、箱は辺の長さ）。+Z が頭
    AddPrimitive(scene, a, "AmbientGull", PrimitiveType::Sphere, white, {0, 0, 0}, {0.26f, 0.24f, 0.72f});
    AddPrimitive(scene, a, "AmbientGull_head", PrimitiveType::Sphere, white, {0, 0.12f, 0.72f}, {0.22f, 0.21f, 0.24f});
    AddPrimitive(scene, a, "AmbientGull_beak", PrimitiveType::Sphere, beak, {0, 0.09f, 0.98f}, {0.05f, 0.045f, 0.16f});
    AddPrimitive(scene, a, "AmbientGull_tail", PrimitiveType::Box, white, {0, 0.04f, -0.82f}, {0.42f, 0.05f, 0.42f});
    AddPrimitive(scene, a, "AmbientGull_wing", PrimitiveType::Box, gray, {0.2f, 0.12f, 0.08f}, {1.1f, 0.06f, 0.55f},
                 TitleAmbientDetail::Identity(), Role::WingInner, 1.0f);
    AddPrimitive(scene, a, "AmbientGull_wing", PrimitiveType::Box, gray, {-0.2f, 0.12f, 0.08f}, {1.1f, 0.06f, 0.55f},
                 TitleAmbientDetail::Identity(), Role::WingInner, -1.0f);
    AddPrimitive(scene, a, "AmbientGull_tip", PrimitiveType::Box, tip, {0, 0, -0.08f}, {0.95f, 0.05f, 0.36f},
                 TitleAmbientDetail::Identity(), Role::WingOuter, 1.0f);
    AddPrimitive(scene, a, "AmbientGull_tip", PrimitiveType::Box, tip, {0, 0, -0.08f}, {0.95f, 0.05f, 0.36f},
                 TitleAmbientDetail::Identity(), Role::WingOuter, -1.0f);
    return a;
  }

  Actor BuildShip(Scene *scene) {
    Actor a;
    a.kind = Kind::Ship;
    const RC::Vector4 deck = {0.66f, 0.49f, 0.30f, 1.0f};
    const RC::Vector4 cabin = {0.86f, 0.84f, 0.78f, 1.0f};
    const RC::Vector4 roof = {0.62f, 0.18f, 0.14f, 1.0f};
    const RC::Vector4 mast = {0.30f, 0.20f, 0.11f, 1.0f};
    // 静水面が y = 0。船体は下 0.3m が水に浸かる。+Z が船首
    AddPrimitive(scene, a, "AmbientShip", PrimitiveType::Box, shipHullColor, {0, 0.05f, -0.1f}, {1.6f, 0.7f, 4.0f});
    AddPrimitive(scene, a, "AmbientShip_bow", PrimitiveType::Box, shipHullColor, {0, 0.05f, 1.9f}, {1.13f, 0.7f, 1.13f},
                 TitleAmbientDetail::RotY(TitleAmbientDetail::kPi * 0.25f)); // 45°回した箱で尖った船首
    AddPrimitive(scene, a, "AmbientShip_deck", PrimitiveType::Box, deck, {0, 0.42f, -0.1f}, {1.4f, 0.06f, 3.8f});
    AddPrimitive(scene, a, "AmbientShip_cabin", PrimitiveType::Box, cabin, {0, 0.75f, -1.2f}, {1.0f, 0.6f, 1.0f});
    AddPrimitive(scene, a, "AmbientShip_roof", PrimitiveType::Box, roof, {0, 1.08f, -1.2f}, {1.15f, 0.08f, 1.15f});
    AddPrimitive(scene, a, "AmbientShip_mast", PrimitiveType::Cylinder, mast, {0, 1.75f, 0.5f}, {0.08f, 2.6f, 0.08f});
    // 帆は追い風で前へはらんだ形（上端を前へ倒す）。真上からでも白い面が見える
    AddPrimitive(scene, a, "AmbientShip_sail", PrimitiveType::Box, shipSailColor, {0, 1.95f, 0.95f}, {2.0f, 1.8f, 0.08f},
                 TitleAmbientDetail::RotX(0.55f));
    // 旗は追い風で前へなびく（マストの前側）
    AddPrimitive(scene, a, "AmbientShip_flag", PrimitiveType::Box, {0.90f, 0.22f, 0.20f, 1.0f}, {0, 2.95f, 0.78f},
                 {0.04f, 0.28f, 0.5f}, TitleAmbientDetail::Identity(), Role::Flag);
    return a;
  }

  // ------------------------------------------------------------------
  // 出現
  // ------------------------------------------------------------------

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

  bool CanSpawn(Kind k) {
    const KindParams &p = Params(k);
    if (!p.enabled || p.weight <= 0.0f) return false;
    if (CountActive(k) >= p.maxActive) return false;
    for (const auto &a : actors_) {
      if (!a.active && a.kind == k && IsReady(a)) return true;
    }
    return false;
  }

  /// @brief 出してよい状態か（モデルは非同期ロードなので、読み終わるまで出さない）
  static bool IsReady(const Actor &a) {
    if (a.kind != Kind::Shark) return true;
    if (a.parts.empty()) return false;
    auto e = a.parts[0].entity.lock();
    if (!e) return false;
    auto *ren = e->GetComponent<ModelRendererComponent>();
    return ren && ren->modelHandle >= 0 && RC::IsModelReady(ren->modelHandle);
  }

  Actor *FreeActor(Kind k) {
    // 漂流物は種類がばらけるよう、空いているものからランダムに選ぶ
    std::vector<Actor *> free;
    for (auto &a : actors_) {
      if (!a.active && a.kind == k && IsReady(a)) free.push_back(&a);
    }
    if (free.empty()) return nullptr;
    std::uniform_int_distribution<size_t> d(0, free.size() - 1);
    return free[d(rng_)];
  }

  bool TrySpawn(Kind k) {
    spawnFail_.clear();
    if (!CanSpawn(k)) {
      const KindParams &p = Params(k);
      if (!p.enabled || p.weight <= 0.0f) spawnFail_ = "disabled / weight 0";
      else if (CountActive(k) >= p.maxActive) spawnFail_ = "already at max active (previous one still active?)";
      else spawnFail_ = "no free actor (pool not built yet / model loading)";
      return false;
    }
    if (k == Kind::Gull) return SpawnGullFlock();
    Actor *a = FreeActor(k);
    if (!a) {
      spawnFail_ = "no free actor";
      return false;
    }
    const KindParams &p = Params(k);
    a->scale = Rand(p.scaleMin, p.scaleMax);
    a->speed = Rand(p.speedMin, p.speedMax);
    a->phase = Rand(0.0f, TitleAmbientDetail::kTwoPi);
    a->pos.y = 0.0f;

    switch (k) {
    case Kind::Shark:
      a->extent = 0.55f * a->scale;
      a->laneRadius = 0.5f * a->scale;
      break;
    case Kind::Drift:
      a->extent = (a->variant == 2 ? 0.95f : 0.7f) * a->scale;
      a->laneRadius = a->extent; // 自転するので長手方向ぶん空ける
      a->turn = Rand(-driftSpinDeg, driftSpinDeg) * TitleAmbientDetail::kDeg;
      a->spin = Rand(0.0f, TitleAmbientDetail::kTwoPi);
      break;
    case Kind::Ship:
      a->extent = 3.0f * a->scale;
      a->laneRadius = 1.1f * a->scale;
      break;
    default:
      break;
    }

    if (!PlacePath(*a, p, view_.waterY)) {
      spawnFail_ = "lane busy (drift/ship already in every lane, or lane narrower than laneRadius)";
      return false;
    }
    Activate(*a);
    return true;
  }

  /// @brief 出現位置と進行方向を決める
  /// @return レーンが埋まっていて置けなければ false
  bool PlacePath(Actor &a, const KindParams &p, float planeY) {
    const float hw = view_.HalfWAt(planeY);
    const float hh = view_.HalfHAt(planeY);
    const float ew = hw + screenMargin + a.extent;
    const float eh = hh + screenMargin + a.extent;

    if (!p.lanes.empty()) {
      // レーン：X 方向にまっすぐ横切る。水面に浮くもの同士が重ならない Z を探す
      // レーンは「物体の端まで含めて収まる帯」。幅が足りないレーンには入れない
      for (int tries = 0; tries < 8; ++tries) {
        std::uniform_int_distribution<size_t> li(0, p.lanes.size() - 1);
        const RC::Vector2 lane = p.lanes[li(rng_)];
        const float zMin = lane.x + a.laneRadius;
        const float zMax = lane.y - a.laneRadius;
        if (zMax < zMin) continue;
        const float z = Rand(zMin, zMax);
        if (!LaneFree(a, z)) continue;
        const bool toRight = Rand01() < 0.5f;
        a.pos.x = view_.cx + (toRight ? -ew : ew);
        a.pos.z = z;
        a.heading = toRight ? TitleAmbientDetail::kHalfPi : -TitleAmbientDetail::kHalfPi;
        a.dir = TitleAmbientDetail::Forward(a.heading);
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
    a.pos.x = px - dx * t;
    a.pos.z = pz - dz * t;
    a.heading = std::atan2(dx, dz);
    a.dir = {dx, 0.0f, dz};
    return true;
  }

  /// @brief 水面に浮くもの（漂流物・船）が同じ高さの帯を同時に通らないか
  bool LaneFree(const Actor &self, float z) const {
    for (const auto &o : actors_) {
      if (!o.active || &o == &self) continue;
      if (o.kind != Kind::Drift && o.kind != Kind::Ship) continue;
      if (std::fabs(o.pos.z - z) < o.laneRadius + self.laneRadius) return false;
    }
    return true;
  }

  bool SpawnGullFlock() {
    Actor *leader = FreeActor(Kind::Gull);
    if (!leader) return false;
    const float height = Rand(gullHeightMin, (std::max)(gullHeightMin, gullHeightMax));
    const float scale = Rand(gull.scaleMin, gull.scaleMax);
    const float speed = Rand(gull.speedMin, gull.speedMax);
    const float turn = Rand(-gullTurnDeg, gullTurnDeg) * TitleAmbientDetail::kDeg;

    leader->scale = scale;
    leader->extent = 2.6f * scale + 3.0f; // 翼端 ＋ 群れの広がり
    leader->pos.y = view_.waterY + height;
    if (!PlacePath(*leader, gull, leader->pos.y)) return false;

    const int room = (std::min)(gull.maxActive - CountActive(Kind::Gull), maxActive - CountActive());
    std::uniform_int_distribution<int> nd(1, (std::max)(1, (std::min)(gullFlockMax, room)));
    const int count = nd(rng_);

    const RC::Vector3 fwd = TitleAmbientDetail::Forward(leader->heading);
    const RC::Vector3 right = {fwd.z, 0.0f, -fwd.x};
    const RC::Vector3 start = leader->pos;
    for (int i = 0; i < count; ++i) {
      Actor *g = (i == 0) ? leader : FreeActor(Kind::Gull);
      if (!g) break;
      // V 字：1 羽目が先頭、以降は左右交互に後ろへ
      const float rank = static_cast<float>((i + 1) / 2);
      const float sideSign = (i % 2 == 1) ? 1.0f : -1.0f;
      const float back = rank * 2.2f * scale / 0.5f * Rand(0.8f, 1.2f);
      const float side = (i == 0) ? 0.0f : sideSign * rank * 2.0f * scale / 0.5f * Rand(0.8f, 1.2f);
      g->scale = scale * Rand(0.92f, 1.08f);
      g->speed = speed * Rand(0.97f, 1.03f);
      g->heading = leader->heading;
      g->dir = leader->dir;
      g->extent = leader->extent;
      g->turn = turn;
      g->pos = {start.x - fwd.x * back + right.x * side, start.y + Rand(-0.6f, 0.6f),
                start.z - fwd.z * back + right.z * side};
      g->phase = Rand(0.0f, TitleAmbientDetail::kTwoPi);
      g->flapPhase = Rand(0.0f, TitleAmbientDetail::kTwoPi);
      g->flapWeight = 1.0f;
      g->gliding = false;
      g->glideTimer = Rand(0.5f, 2.0f);
      g->roll = 0.0f;
      Activate(*g);
    }
    return true;
  }

  void Activate(Actor &a) {
    a.active = true;
    a.t = 0.0f;
    a.wake.clear();
    a.wakePath = 0.0f;
    a.wakeHasLast = false;
    a.yaw = a.heading;
    // 先に姿勢を書いてからアクティブにする（前回の位置で 1 フレーム映らないように）
    UpdateActor(a, 0.0f);
    for (auto &p : a.parts) {
      if (auto e = p.entity.lock()) e->SetActive(true);
    }
  }

  void Deactivate(Actor &a) {
    a.active = false;
    for (auto &p : a.parts) {
      if (auto e = p.entity.lock()) e->SetActive(false);
    }
  }

  // ------------------------------------------------------------------
  // 更新
  // ------------------------------------------------------------------

  void UpdateActor(Actor &a, float dt) {
    a.t += dt;
    switch (a.kind) {
    case Kind::Shark: UpdateShark(a, dt); break;
    case Kind::Drift: UpdateDrift(a, dt); break;
    case Kind::Gull: UpdateGull(a, dt); break;
    case Kind::Ship: UpdateShip(a, dt); break;
    default: break;
    }
    if (dt > 0.0f && IsGone(a)) Deactivate(a);
  }

  /// @brief 画面外（拡張枠の外）へ出て、なお離れていく向きなら退場
  bool IsGone(const Actor &a) const {
    if (a.t < 1.0f) return false;
    if (a.t > 120.0f) return true; // 念のため（旋回し続けて戻ってこない等）
    const float planeY = (a.kind == Kind::Gull) ? a.pos.y : view_.waterY;
    const float ew = view_.HalfWAt(planeY) + screenMargin + a.extent + 0.5f;
    const float eh = view_.HalfHAt(planeY) + screenMargin + a.extent + 0.5f;
    const float rx = a.pos.x - view_.cx;
    const float rz = a.pos.z - view_.cz;
    if (std::fabs(rx) < ew && std::fabs(rz) < eh) return false;
    const RC::Vector3 f = TitleAmbientDetail::Forward(a.yaw);
    return (rx * f.x + rz * f.z) > 0.0f;
  }

  void Place(const Actor &a, const Part &p, const RC::Vector3 &worldPos, const Mat3 &rot) {
    auto e = p.entity.lock();
    if (!e) return;
    auto *tr = e->GetComponent<TransformComponent>();
    if (!tr) return;
    tr->position = worldPos;
    tr->rotation = TitleAmbientDetail::ToEngineEuler(rot);
    tr->scale = TitleAmbientDetail::ScaleV(p.scale, a.scale);
    TitleAmbientDetail::SyncRender(*e);
  }

  /// @brief 固定の部位（翼以外）を本体姿勢に合わせて置く
  void PlaceRigid(const Actor &a, const RC::Vector3 &bodyPos, const Mat3 &body, float flagAngle = 0.0f) {
    for (const auto &p : a.parts) {
      if (p.role == Role::WingInner || p.role == Role::WingOuter) continue;
      Mat3 local = p.local;
      if (p.role == Role::Flag) local = TitleAmbientDetail::MatMul(TitleAmbientDetail::RotY(flagAngle), local);
      const RC::Vector3 off = TitleAmbientDetail::ScaleV(p.offset, a.scale);
      Place(a, p, TitleAmbientDetail::AddV(bodyPos, TitleAmbientDetail::Apply(off, body)),
            TitleAmbientDetail::MatMul(local, body));
    }
  }

  /// @brief 航跡波を出す：船首のへこみ ＋ 通った道筋から V 字に広がる 2 本の山
  /// @param bow 船首（サメは頭）のワールド位置
  /// @param fwd 進行方向（xz、正規化済み）
  /// @param length V の長さ（m）。これより古い道筋は捨てる
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
  /// @param stern 船尾（サメは尾）のワールド位置
  /// @param halfWidth 船体の半幅（m）。舷側と船尾の泡の広がりに使う
  /// @param foamScale 泡の量の倍率（種類ごと）
  /// @param dt 経過秒（泡の量は 1 秒あたりで指定しているため）
  void Wake(Actor &a, const RC::Vector3 &bow, const RC::Vector3 &fwd, float length, const RC::Vector3 &stern,
            float halfWidth, float foamScale, float dt) {
    // 強さは 1.5 m/s を基準に速さへ比例させる（速いほどはっきり）
    const float speedScale = std::clamp(a.speed / 1.5f, 0.75f, 2.0f);
    const float L = (std::max)(length, 0.1f);
    const float spacing = (std::max)(wakeSpacing, 0.2f);

    // 道のりを積算し、spacing ごとに道筋を記録する
    if (a.wakeHasLast) {
      const float dx = bow.x - a.wakeLast.x;
      const float dz = bow.z - a.wakeLast.z;
      a.wakePath += std::sqrt(dx * dx + dz * dz);
    }
    a.wakeLast = bow;
    a.wakeHasLast = true;
    if (a.wake.empty() || a.wakePath - a.wake.back().dist >= spacing) {
      a.wake.push_back({bow, {fwd.z, 0.0f, -fwd.x}, a.wakePath});
    }
    // V の長さより古い道筋は捨てる（先頭から順に古い）
    size_t drop = 0;
    while (drop < a.wake.size() && a.wakePath - a.wake[drop].dist > L) ++drop;
    if (drop > 0) a.wake.erase(a.wake.begin(), a.wake.begin() + static_cast<std::ptrdiff_t>(drop));

    // 船首のへこみ
    if (wakeBowStrength > 0.0f) {
      if (TitleAmbientDetail::PushWaveSource(bow.x, bow.z, wakeBowRadius, -wakeBowStrength * speedScale)) {
        ++wakeSourcesThisFrame_;
      }
    }

    // V の腕（高さ）：各道筋点から、通過後の道のり d に応じて左右へ d·tan(角度) だけ開いた位置を押し上げる
    const float tanA = std::tan(std::clamp(wakeAngleDeg, 1.0f, 80.0f) * TitleAmbientDetail::kDeg);
    for (const WakeSample &w : a.wake) {
      const float d = a.wakePath - w.dist;
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
    const float crestScale = foamScale * frameScale * std::clamp(a.speed / 1.5f, 0.75f, 1.5f);
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
      for (size_t i = 0; i < a.wake.size(); ++i) {
        const WakeSample &w0 = a.wake[i];
        const float d0 = a.wakePath - w0.dist;
        if (i + 1 < a.wake.size()) {
          const WakeSample &w1 = a.wake[i + 1];
          armPoint(w0.pos, w0.right, d0, w1.pos, w1.right, a.wakePath - w1.dist);
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
      const float amount = foamStern * foamScale * (a.speed / 1.5f) * dt;
      if (amount > 0.0f &&
          RC::AddFoamSourceAtWorld(stern.x - fwd.x * 0.2f, stern.z - fwd.z * 0.2f, sternR, amount)) {
        ++foamSourcesThisFrame_;
      }
    }
  }

  void UpdateShark(Actor &a, float dt) {
    using namespace TitleAmbientDetail;
    // 進路：基準方位のまわりをゆっくり蛇行
    const float sway = sharkSwayDeg * kDeg * std::sin(a.t * kTwoPi / (std::max)(sharkSwayPeriod, 0.5f) + a.phase);
    const float moveYaw = a.heading + sway;
    const RC::Vector3 f = Forward(moveYaw);
    a.pos.x += f.x * a.speed * dt;
    a.pos.z += f.z * a.speed * dt;
    a.yaw = moveYaw;

    // 尾振り：頭寄りの点を軸に体を振る（頭はあまり動かず尾が大きく振れる）
    const float wag = sharkWagDeg * kDeg * std::sin(a.t * kTwoPi * sharkWagFrequency + a.phase * 1.7f);
    const float visYaw = moveYaw + wag;
    const float pivot = 0.3f * a.scale;
    const RC::Vector3 fv = Forward(visYaw);
    const RC::Vector3 head = {a.pos.x + f.x * pivot, 0.0f, a.pos.z + f.z * pivot};
    const RC::Vector3 center = {head.x - fv.x * pivot, 0.0f, head.z - fv.z * pivot};

    const float base = hasWater_ ? water_.baseHeight : view_.waterY;
    const float h = WaterHeight(center.x, center.z);
    const float y = base + (h - base) * sharkWaveFollow - sharkSink;

    if (!a.parts.empty()) {
      const Part &p = a.parts[0];
      const Mat3 rot = MatMul(RotY(sharkModelYawOffsetDeg * kDeg), RotY(visYaw));
      auto e = p.entity.lock();
      if (e) {
        if (auto *tr = e->GetComponent<TransformComponent>()) {
          tr->position = {center.x, y, center.z};
          tr->rotation = ToEngineEuler(rot);
          tr->scale = {a.scale, a.scale, a.scale};
          SyncRender(*e);
        }
      }
    }

    if (sharkWake && dt > 0.0f) {
      const float nose = 0.45f * a.scale;
      // 尾は頭と反対側へ同じくらい。水面に出ているのは背中と背ビレだけなので幅は細め
      const RC::Vector3 tail = {center.x - fv.x * nose, 0.0f, center.z - fv.z * nose};
      Wake(a, {center.x + fv.x * nose, 0.0f, center.z + fv.z * nose}, fv, sharkWakeLength, tail, 0.1f * a.scale,
           sharkFoam, dt);
    }
  }

  void UpdateDrift(Actor &a, float dt) {
    using namespace TitleAmbientDetail;
    const RC::Vector3 f = Forward(a.heading);
    // 波に揺られて速さが少し揺らぐ
    const float surge = 1.0f + 0.25f * std::sin(a.t * 0.9f + a.phase);
    a.pos.x += f.x * a.speed * surge * dt;
    a.pos.z += f.z * a.speed * surge * dt;
    a.spin += a.turn * dt;
    a.yaw = a.heading; // 退場判定は進行方向で行う

    float floatOffset = 0.1f, halfLen = 0.45f, halfWid = 0.45f;
    if (a.variant == 1) { floatOffset = 0.06f; halfLen = 0.5f; halfWid = 0.36f; }
    if (a.variant == 2) { floatOffset = 0.02f; halfLen = 0.9f; halfWid = 0.22f; }
    halfLen *= a.scale;
    halfWid *= a.scale;

    float y = 0.0f, pitch = 0.0f, roll = 0.0f;
    // 漂流物は航跡を出さないので、マウスや船の波紋に素直に乗せる
    WaterPose(a.pos, a.spin, halfLen, halfWid, y, pitch, roll, /*ripple=*/true);
    // ぷかぷか：波とは別に小さく上下
    y += floatOffset * a.scale + driftFloatBias + 0.04f * std::sin(a.t * 2.1f + a.phase);
    const Mat3 body = BodyMatrix(a.spin, pitch * driftTiltScale, roll * driftTiltScale);
    PlaceRigid(a, {a.pos.x, y, a.pos.z}, body);
  }

  void UpdateGull(Actor &a, float dt) {
    using namespace TitleAmbientDetail;
    // ゆるく旋回しながら飛ぶ。旋回に合わせてバンクする
    a.heading = WrapAngle(a.heading + a.turn * dt);
    a.yaw = a.heading;
    const RC::Vector3 f = Forward(a.heading);
    a.pos.x += f.x * a.speed * dt;
    a.pos.z += f.z * a.speed * dt;
    const float bob = 0.25f * std::sin(a.t * 0.8f + a.phase);
    const float targetRoll = std::clamp(-a.turn * a.speed * 0.25f, -0.5f, 0.5f);
    a.roll += (targetRoll - a.roll) * (std::min)(1.0f, dt * 2.0f);

    // 羽ばたき ⇔ 滑空
    if (dt > 0.0f) {
      a.glideTimer -= dt;
      if (a.glideTimer <= 0.0f) {
        a.gliding = !a.gliding && (Rand01() < gullGlideChance);
        a.glideTimer = a.gliding ? Rand(1.2f, 2.8f) : 2.0f;
      }
      const float target = a.gliding ? 0.0f : 1.0f;
      a.flapWeight += (target - a.flapWeight) * (std::min)(1.0f, dt * 3.0f);
      a.flapPhase += kTwoPi * gullFlapFrequency * dt * (0.3f + 0.7f * a.flapWeight);
    }
    const float amp = gullFlapDeg * kDeg * a.flapWeight;
    const float inner = gullDihedralDeg * kDeg + amp * std::sin(a.flapPhase);
    const float outer = 0.6f * amp * std::sin(a.flapPhase - 0.7f) - 0.05f * (1.0f - a.flapWeight);
    // 羽ばたきに合わせて胴がわずかに上下する（振り下ろしで持ち上がる）
    const float lift = 0.05f * a.scale * std::cos(a.flapPhase) * a.flapWeight;

    const RC::Vector3 bodyPos = {a.pos.x, a.pos.y + bob + lift, a.pos.z};
    const Mat3 body = BodyMatrix(a.heading, 0.0f, a.roll);
    PlaceRigid(a, bodyPos, body);

    // 翼：内側は肩を軸に、外側は肘（内側の先端）を軸にさらに曲げる
    RC::Vector3 elbow[2] = {{0, 0, 0}, {0, 0, 0}};
    for (const auto &p : a.parts) {
      if (p.role != Role::WingInner) continue;
      const int si = (p.side > 0.0f) ? 0 : 1;
      const Mat3 local = RotZ(p.side * inner);
      const RC::Vector3 shoulder = ScaleV(p.offset, a.scale);
      const RC::Vector3 halfSpan = Apply({p.side * p.scale.x * a.scale * 0.5f, 0.0f, 0.0f}, local);
      const RC::Vector3 c = AddV(shoulder, halfSpan);
      elbow[si] = AddV(c, halfSpan);
      Place(a, p, AddV(bodyPos, Apply(c, body)), MatMul(local, body));
    }
    for (const auto &p : a.parts) {
      if (p.role != Role::WingOuter) continue;
      const int si = (p.side > 0.0f) ? 0 : 1;
      const Mat3 local = MatMul(RotZ(p.side * outer), RotZ(p.side * inner));
      const RC::Vector3 halfSpan = Apply({p.side * p.scale.x * a.scale * 0.5f, 0.0f, 0.0f}, local);
      const RC::Vector3 c = AddV(AddV(elbow[si], halfSpan), ScaleV(p.offset, a.scale));
      Place(a, p, AddV(bodyPos, Apply(c, body)), MatMul(local, body));
    }
  }

  void UpdateShip(Actor &a, float dt) {
    using namespace TitleAmbientDetail;
    const RC::Vector3 f = Forward(a.heading);
    a.pos.x += f.x * a.speed * dt;
    a.pos.z += f.z * a.speed * dt;
    a.yaw = a.heading;

    float y = 0.0f, pitch = 0.0f, roll = 0.0f;
    WaterPose(a.pos, a.heading, 2.0f * a.scale, 0.8f * a.scale, y, pitch, roll);
    const float sway = 0.03f * std::sin(a.t * 0.7f + a.phase); // 波とは別の小さな横揺れ
    const Mat3 body = BodyMatrix(a.heading, pitch * shipTiltScale, roll * shipTiltScale + sway);
    const float flag = 0.35f * std::sin(a.t * 5.0f + a.phase);
    PlaceRigid(a, {a.pos.x, y, a.pos.z}, body, flag);

    if (shipWake && dt > 0.0f) {
      const float bow = 2.6f * a.scale;
      // 船体は WaterPose と同じく 前後 ±2.0・左右 ±0.8（scale 1）。船尾は少し後ろ
      const float sternOff = 2.1f * a.scale;
      const RC::Vector3 stern = {a.pos.x - f.x * sternOff, 0.0f, a.pos.z - f.z * sternOff};
      Wake(a, {a.pos.x + f.x * bow, 0.0f, a.pos.z + f.z * bow}, f, shipWakeLength, stern, 0.8f * a.scale, shipFoam,
           dt);
    }
  }

  // ------------------------------------------------------------------
  // 状態
  // ------------------------------------------------------------------

  std::vector<Actor> actors_;
  std::weak_ptr<Entity> folder_;
  bool poolBuilt_ = false;
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
