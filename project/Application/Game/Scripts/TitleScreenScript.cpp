#include "ECS/ScriptableEntity.h"
#include "ECS/ScriptRegistry.h"
#include "ECS/TransformComponent.h"
#include "ECS/TextMeshComponent.h"
#include "ECS/WaterComponent.h"
#include "ECS/CameraComponent.h"
#include "ECS/GPUParticleComponent.h"
#include "ECS/PrimitiveMeshComponent.h"
#include "Particle/GPUParticle.h"
#include "Graphics/PostProcess/PostProcess.h"
#include "Common/Math/MathUtils.h"
#include "Common/Water/WaterSurface.h"
#include "Common/Log/Log.h"
#include "Input/Input.h"
#include "Render/Systems/RenderInteractiveWater.h"
#include "RenderCommon.h"
#include "Engine/Render/RenderContext.h"
#include "Framework/App.h"
#include "Game/Framework/UnderwaterLook.h"
#include "Game/Framework/WaterCameraFx.h"
#include "Scene.h"
#include "SceneFlow.h"

#if RC_ENABLE_IMGUI
#include "imgui/imgui.h"
#endif

#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

/// @class TitleScreenScript
/// @brief タイトル画面（真上から見た水面 ＋ 水面に浮くタイトル文字 ＋ 水面に浮くメニュー）
/// @details
///   構成:
///     - 水面と真上視点のカメラは Title.json 側に置く（WaterComponent / CameraComponent）。
///       このスクリプトは水面を「読む」だけで、生成はしない。
///     - タイトル文字は 1 文字ずつ、メニュー（スタート／ゲーム終了）は 1 項目ずつ、
///       TextMeshComponent を持つエンティティとして OnCreate で生成する。
///       どれも毎フレーム RC::WaterSurface（Water.VS.hlsl と同じ Gerstner 式）で水面の
///       高さと法線をサンプルし、浮き沈み・傾きを追従させる（Floater）。
///     - 波紋（マウス／船の航跡など、GPU の WaveSimulation）にも反応する。
///       rippleFloat が true のあいだ RenderInteractiveWater のハイトマップ読み戻しを有効にし
///       （256KB のコピーだけ。GPU 待ちはしない）、CPU 側で読んだ波紋の高さ・法線を
///       Gerstner の結果に足す。Game ではこのスクリプトが無いので読み戻しは走らない。
///     - メニューの選択は色・大きさ・浮き上がりで示す。
///       ↑↓ / W S / 十字キー / 左スティックで選択、Space / Enter / A ボタンで決定。
///       「スタート」→ 飛び込み演出のあと遷移表のきっかけ "start" の行き先へ、「ゲーム終了」→ PostQuitMessage(0)。
///     - 操作ヒントだけは小さな 2D 文字（OnRender / RC::DrawString）。showHint で消せる。
///
///   スタート時の飛び込み（Dive）:
///     真上から水面を見ているカメラをそのまま水へ飛び込ませ、深海の暗さまで沈めてから
///     "dive" 遷移（SceneManager が深海色へ短く抜ける）で Game へ渡す。
///     Game 側は DeepRiseIntroScript が同じ深海色から浮上を始めるので、画がつながる。
///       1. ため   : カメラが少し持ち上がる（diveAnticipation 秒）
///       2. 落下   : 水面へ向かって加速。画角が広がりタイトル文字が迫る（divePlunge 秒）
///       3. 着水   : 水中エフェクト一式 ＋ ラジアルブラー ＋ 上へ昇る水滴 ＋ 泡
///       4. 沈降   : 見上げる向きへ傾きながら沈み、深さに応じてフォグが深海色へ（diveSink 秒）
///       5. 完了   : RequestSceneChange(行き先, "dive")
///     水中の色・距離は UnderwaterLook（Game 側と共通の定義）で決める。
///
///   注意:
///     - DataDrivenScene 側の「導線シーンで Space → 次のシーン」判定から Title を外してあること。
///       残っていると Space で Select へ飛んでしまい、メニューと二重判定になる。
///     - TextMesh の読める面は -Z を向くので、rotation.x = +π/2 で +Y（真上）へ向ける。
///       文字の上方向は +Z になる。カメラも rotation.x = +π/2（真下向き）なので画面上＝+Z で一致する。
///     - TextMesh 本体の色は DataDrivenScene が毎フレーム同期しない（再生成時のみ）ため、
///       選択色は Material へ直接書く（ApplyMainColor）。
///
///   JSON (scriptDataList) で設定できる項目:
///     [タイトル文字]
///     "titleText"      : タイトル文字列（UTF-8、1 文字ずつ分解して浮かせる）
///     "fontPath"       : 文字メッシュのフォント
///     "letterSize"     : 1em の高さ（ワールド m）
///     "letterDepth"    : 押し出し厚さ（m）
///     "letterColor"    : 文字色 RGBA
///     "outlineEnabled" / "outlineWidth" / "outlineColor" : 縁取り
///     "letterPositions": [[x, z, yaw], ...] 文字ごとの配置（足りない分は自動配置）
///     "floatOffset"    : 文字の底面を水面（足元でいちばん高い点）からどれだけ浮かせるか（m）
///     "bobAmplitude" / "bobPeriod" : ぷかぷか上下する量（m）と周期（秒）
///     "driftRadius" / "driftPeriod" : 文字がゆっくり漂う円運動の半径（m）と周期（秒）
///     "tiltScale"      : 水面法線への傾き追従の強さ（0 で傾かない）
///     "rippleFloat"    : 波紋（マウス・航跡）にも浮き沈み・傾きを追従させるか（文字・メニュー共通）
///     "rippleTiltScale": 波紋による傾きの強さ（高さは等倍で追従。傾きだけ抑えられる）
///     "letterShininess" / "letterEnvCoeff" : タイトル文字の光沢と環境マップ映り込み
///     [ポストプロセス]  "titlePost": { enabled, bloom, bloomThreshold, bloomIntensity, bloomRadius,
///                       bloomKnee, colorGrade, gradeExposure, gradeContrast, gradeSaturation,
///                       gradeTemperature, gradeTint, gradeAmount, vignette }
///                       タイトル画面だけに掛ける。遷移時は SceneManager が ClearPostEffects する
///     [メニュー]
///     "menuFontPath"   : メニュー文字メッシュのフォント
///     "menuSize"       : メニューの 1em の高さ（m）
///     "menuDepth"      : メニューの押し出し厚さ（m）
///     "menuPositions"  : [[x, z], [x, z]] スタート／ゲーム終了のベースライン位置（画面上が +Z）
///     "menuColor" / "menuSelectedColor" / "menuOutlineColor" : 非選択色・選択色・縁取り色
///     "menuOutlineWidth"  : メニューの縁取りの太さ（タイトル文字の outlineWidth とは別）
///     "menuShininess"     : メニューの光沢（0 でハイライトなし）
///     "menuUnselectedScale" : 非選択の拡大率 / "menuPulseWhite" : 選択中が白へ明滅する量
///     "menuRing"          : { enabled, color, size, gap, spinSpeed, height, follow,
///                             rippleInterval, rippleStrength, rippleRadius, selectRipple }
///                           選択中の項目の左右で回る金のリングと、その下から出る波紋
///     [起動演出]  "intro" : { enabled, delay, interval, riseTime, depth, overshoot,
///                             menuDelay, menuInterval, menuRiseTime, menuDepth,
///                             splash, splashCount, rippleStrength, rippleRadius, skippable }
///                           文字が 1 文字ずつ水中から浮かび（飛沫・波紋）、最後にメニューが浮かぶ。
///                           編集モードでは出さない。浮上（Dive 遷移）で入ってきたときは
///                           浮上中は文字とメニューを隠し、カメラが落ち着いてから始める
///     "hintBandColor" / "hintBandPadding" : 操作ヒントの後ろに敷く半透明の帯
///     "menuSelectedScale" : 選択中の拡大率
///     "menuSelectedLift"  : 選択中に追加で浮かせる量（m）
///     "menuBobAmplitude"  : メニューの上下動（m。タイトル文字より控えめにする）
///     "menuTiltScale"     : メニューの傾き追従の強さ
///     （行き先は遷移表 Resources/SceneFlow.json のきっかけ "start"。演出が "dive" のときだけ飛び込む）
///     "quitEnabled"    : 「ゲーム終了」でアプリを終了するか（false ならログのみ）
///     [ヒント]
///     "showHint" / "hintFontPath" / "hintFontSize" : 画面下の操作ヒント（2D）
///     [飛び込み]
///     "diveEnabled"       : false なら従来どおり即 Dissolve 遷移
///     "diveAnticipation"  : ための時間（秒） / "diveLift" : そのとき持ち上がる高さ（m）
///     "divePlunge"        : 水面までの落下時間（秒）
///     "diveFovBoost"      : 落下中に画角（fovY）を何倍まで広げるか
///     "diveSink"          : 着水から diveDepth まで沈む時間（秒） / "diveDepth" : 沈む深さ（m）
///                           ※ 深さが underwater.deepDepth（画面が深海色になりきる深さ）に
///                             達した時点で Game への遷移を要求する。真っ暗な時間を延ばさないため
///     "diveLookUpPitch"   : 沈みながら向く角度（rad。0 で水平、負で見上げ、π/2 で真下のまま）
///     "diveTilt"          : 真下向きから diveLookUpPitch へ傾くのにかける時間（秒）
///     "diveSurfaceBlend"  : 着水後、水中エフェクトが完全に掛かるまでの時間（秒）
///     "diveBubbles"       : 沈降中に泡（GPU パーティクル）を流すか
///     [浮上]  Dive 遷移（Result の「タイトルへ」など）で入ってきたときだけ、深海から浮上して始まる
///     "riseEnabled"       : false なら浮上せずにいつもの真上視点から始める
///     "riseDepth"         : 浮上を始める深さ（m）
///     "riseUnderwaterTime": 水面の直下まで上がる時間（秒） / "riseEase" : その速度カーブ
///     "risePitchStart"    : 浮上開始時に見上げる角度（rad）
///     "riseClimbTime"     : 水面を抜けてから真上視点の位置まで上がる時間（秒）
///     "riseSurfaceBlend"  : 水面を抜けてから水中エフェクトが消えきるまで（秒）
///     "riseBubbles"       : 浮上中に泡を流すか
///     ※ 実装は Application/Game/Framework/WaterCameraFx.h（Result と共通）
///     "underwater"        : 水中の見た目（UnderwaterLook::Params。Game 側と同じキー）
class TitleScreenScript : public ScriptableEntity {
public:
  // ---- タイトル文字 ----
  std::string titleText = "水天の射手";
  std::string fontPath = "Resources/fonts/Kiwi_Maru/KiwiMaru-Medium.ttf";
  float letterSize = 3.0f;
  float letterDepth = 0.5f;
  RC::Vector4 letterColor = {0.97f, 0.98f, 1.0f, 1.0f};
  bool outlineEnabled = true;
  float outlineWidth = 0.04f;
  RC::Vector4 outlineColor = {0.05f, 0.12f, 0.25f, 1.0f};
  std::vector<RC::Vector3> letterPositions; ///< x, z, yaw(rad)
  float floatOffset = 0.12f;
  float bobAmplitude = 0.08f;
  float bobPeriod = 2.8f;
  float driftRadius = 0.35f;
  float driftPeriod = 9.0f;
  float tiltScale = 0.7f;
  bool rippleFloat = true;       ///< 波紋（RenderInteractiveWater）にも追従する
  float rippleTiltScale = 1.0f;  ///< 波紋による傾きの強さ
  float letterShininess = 32.0f; ///< タイトル文字の光沢（大きいほど鋭いハイライト）
  float letterEnvCoeff = 0.0f;   ///< タイトル文字の環境マップ映り込み（0〜1。金属っぽさ）

  // ---- タイトル画面だけのポストプロセス（Bloom / ColorGrade / Vignette）----
  // シーン遷移時は切り替え側が ClearPostEffects するので Game 側へは持ち越さない。
  struct TitlePost {
    bool enabled = true;
    // Bloom は 1 パス近似（2 リング 17 タップ）なので、明部が画面中に散らばる水面だと
    // にじみが二重の輪になって濁る。Vignette は強さ固定で四隅が真っ黒まで落ちる。
    // どちらも既定では切り、ColorGrade だけ軽く掛ける。
    bool bloom = false;
    float bloomThreshold = 0.85f;
    float bloomIntensity = 0.35f;
    float bloomRadius = 4.0f;
    float bloomKnee = 0.2f;
    bool colorGrade = true;
    float gradeExposure = 0.1f;
    float gradeContrast = 1.05f;
    float gradeSaturation = 1.12f;
    float gradeTemperature = 0.0f;
    float gradeTint = 0.0f;
    float gradeAmount = 1.0f;
    bool vignette = false;
  } post;

  // ---- メニュー（3D 文字）----
  std::string menuFontPath = "Resources/fonts/Kiwi_Maru/KiwiMaru-Medium.ttf";
  float menuSize = 1.5f;
  float menuDepth = 0.3f;
  std::vector<RC::Vector2> menuPositions;                    ///< x, z（ベースライン）
  RC::Vector4 menuColor = {0.46f, 0.56f, 0.64f, 1.0f};         ///< 非選択（暗めにして格を下げる）
  RC::Vector4 menuSelectedColor = {1.0f, 0.78f, 0.28f, 1.0f}; ///< 選択中（金）
  RC::Vector4 menuOutlineColor = {0.04f, 0.10f, 0.22f, 1.0f};
  float menuOutlineWidth = 0.04f; ///< メニューの縁取りの太さ（タイトル文字とは別に決める）
  float menuShininess = 0.0f;     ///< メニューの光沢（0 でハイライトなし。真上からの光で面全体が白飛びするため）
  float menuSelectedScale = 1.18f;
  float menuUnselectedScale = 0.85f; ///< 非選択の拡大率（小さくして選択中との差をつける）
  float menuPulseWhite = 0.25f;      ///< 選択中が白へ明滅する量（0 で明滅しない）
  float menuSelectedLift = 0.12f;
  float menuBobAmplitude = 0.04f;
  float menuTiltScale = 0.5f;
  bool quitEnabled = true;

  // ---- 選択中メニューの目印（左右で回る金のリング ＋ 波紋）----
  struct RingParams {
    bool enabled = true;
    RC::Vector4 color = {1.0f, 0.80f, 0.32f, 1.0f};
    float size = 0.42f;         ///< リングの半径（m。Torus は外径 1.3 なので実寸は ×1.3）
    float gap = 0.9f;           ///< 文字の端からリング中心までの距離（m）
    float spinSpeed = 3.0f;     ///< 回転の速さ（rad/s。水平軸まわりに回すので真上から見ると表裏が返る）
    float height = 0.35f;       ///< 水面からの高さ（m）
    float follow = 14.0f;       ///< 選択切り替え時に追いかける速さ
    float rippleInterval = 1.1f;   ///< リングの下から波紋を出す間隔（秒。0 で出さない）
    float rippleStrength = -0.12f; ///< その波紋の強さ（負で押し下げ）
    float rippleRadius = 0.02f;    ///< その波紋の半径（UV。0.01 = 1m）
    float selectRipple = -0.3f;    ///< 選択を切り替えた瞬間に項目の下へ出す波紋の強さ
  } ring;

  // ---- 起動演出（文字が 1 文字ずつ水中から浮かび上がる → メニュー）----
  struct IntroParams {
    bool enabled = true;
    float delay = 1.0f;          ///< 最初の文字が動き出すまで（秒。シーン遷移のフェードが明けるのを待つ）
    float interval = 0.3f;       ///< 文字ごとのずれ（秒）
    float riseTime = 0.9f;       ///< 1 文字が浮かび上がるのにかける時間（秒）
    float depth = 4.0f;          ///< 浮かび上がる前の深さ（m）
    float overshoot = 1.6f;      ///< 浮上の行き過ぎ（easeOutBack の係数。0 で行き過ぎなし）
    float menuDelay = 0.35f;     ///< 最後の文字のあと、メニューが出始めるまで（秒）
    float menuInterval = 0.15f;  ///< メニュー項目ごとのずれ（秒）
    float menuRiseTime = 0.6f;   ///< メニュー 1 項目が浮かび上がる時間（秒）
    float menuDepth = 2.0f;      ///< メニューが浮かび上がる前の深さ（m）
    bool splash = true;          ///< 水面を抜けた瞬間に飛沫（GPU パーティクル）を出す
    int splashCount = 80;        ///< 飛沫 1 回の粒の数
    float rippleStrength = -0.45f; ///< 水面を抜けた瞬間の波紋の強さ
    float rippleRadius = 0.03f;    ///< その波紋の半径（UV）
    bool skippable = true;       ///< 演出中にキーを押したら最後まで飛ばす
  } intro;

  // ---- 操作ヒント（2D）----
  bool showHint = true;
  std::string hintFontPath = "Resources/fonts/Kiwi_Maru/KiwiMaru-Regular.ttf";
  float hintFontSize = 20.0f;
  RC::Vector4 hintBandColor = {0.0f, 0.04f, 0.08f, 0.45f}; ///< ヒントの後ろに敷く帯（a = 0 で出さない）
  float hintBandPadding = 10.0f; ///< 帯の上下の余白（px）

  // ---- 飛び込み（スタート → Game）/ 浮上（Dive 遷移で入ってきたとき）----
  WaterCameraFx::DiveParams dive; ///< JSON キーは "dive*"
  WaterCameraFx::RiseParams rise; ///< JSON キーは "rise*"
  UnderwaterLook::Params look;    ///< 水中の見た目（Game 側と共通）

  nlohmann::json Serialize() override {
    nlohmann::json j = SerializeBase();
    dive.WriteJson(j);
    rise.WriteJson(j);
    return j;
  }

  nlohmann::json SerializeBase() const {
    nlohmann::json positions = nlohmann::json::array();
    for (const auto &p : letterPositions) positions.push_back({p.x, p.y, p.z});
    nlohmann::json menuPos = nlohmann::json::array();
    for (const auto &p : menuPositions) menuPos.push_back({p.x, p.y});
    auto v4 = [](const RC::Vector4 &c) { return nlohmann::json{c.x, c.y, c.z, c.w}; };
    return {
        {"titleText", titleText},
        {"fontPath", fontPath},
        {"letterSize", letterSize},
        {"letterDepth", letterDepth},
        {"letterColor", v4(letterColor)},
        {"outlineEnabled", outlineEnabled},
        {"outlineWidth", outlineWidth},
        {"outlineColor", v4(outlineColor)},
        {"letterPositions", positions},
        {"floatOffset", floatOffset},
        {"bobAmplitude", bobAmplitude},
        {"bobPeriod", bobPeriod},
        {"driftRadius", driftRadius},
        {"driftPeriod", driftPeriod},
        {"tiltScale", tiltScale},
        {"rippleFloat", rippleFloat},
        {"rippleTiltScale", rippleTiltScale},
        {"letterShininess", letterShininess},
        {"letterEnvCoeff", letterEnvCoeff},
        {"titlePost",
         {{"enabled", post.enabled},
          {"bloom", post.bloom},
          {"bloomThreshold", post.bloomThreshold},
          {"bloomIntensity", post.bloomIntensity},
          {"bloomRadius", post.bloomRadius},
          {"bloomKnee", post.bloomKnee},
          {"colorGrade", post.colorGrade},
          {"gradeExposure", post.gradeExposure},
          {"gradeContrast", post.gradeContrast},
          {"gradeSaturation", post.gradeSaturation},
          {"gradeTemperature", post.gradeTemperature},
          {"gradeTint", post.gradeTint},
          {"gradeAmount", post.gradeAmount},
          {"vignette", post.vignette}}},
        {"menuFontPath", menuFontPath},
        {"menuSize", menuSize},
        {"menuDepth", menuDepth},
        {"menuPositions", menuPos},
        {"menuColor", v4(menuColor)},
        {"menuSelectedColor", v4(menuSelectedColor)},
        {"menuOutlineColor", v4(menuOutlineColor)},
        {"menuOutlineWidth", menuOutlineWidth},
        {"menuShininess", menuShininess},
        {"menuSelectedScale", menuSelectedScale},
        {"menuUnselectedScale", menuUnselectedScale},
        {"menuPulseWhite", menuPulseWhite},
        {"menuSelectedLift", menuSelectedLift},
        {"menuBobAmplitude", menuBobAmplitude},
        {"menuTiltScale", menuTiltScale},
        {"quitEnabled", quitEnabled},
        {"menuRing",
         {{"enabled", ring.enabled},
          {"color", v4(ring.color)},
          {"size", ring.size},
          {"gap", ring.gap},
          {"spinSpeed", ring.spinSpeed},
          {"height", ring.height},
          {"follow", ring.follow},
          {"rippleInterval", ring.rippleInterval},
          {"rippleStrength", ring.rippleStrength},
          {"rippleRadius", ring.rippleRadius},
          {"selectRipple", ring.selectRipple}}},
        {"intro",
         {{"enabled", intro.enabled},
          {"delay", intro.delay},
          {"interval", intro.interval},
          {"riseTime", intro.riseTime},
          {"depth", intro.depth},
          {"overshoot", intro.overshoot},
          {"menuDelay", intro.menuDelay},
          {"menuInterval", intro.menuInterval},
          {"menuRiseTime", intro.menuRiseTime},
          {"menuDepth", intro.menuDepth},
          {"splash", intro.splash},
          {"splashCount", intro.splashCount},
          {"rippleStrength", intro.rippleStrength},
          {"rippleRadius", intro.rippleRadius},
          {"skippable", intro.skippable}}},
        {"showHint", showHint},
        {"hintFontPath", hintFontPath},
        {"hintFontSize", hintFontSize},
        {"hintBandColor", v4(hintBandColor)},
        {"hintBandPadding", hintBandPadding},
        {"underwater", look.ToJson()},
    };
  }

  void Deserialize(const nlohmann::json &j) override {
    auto readF = [&](const char *key, float &out) {
      if (j.contains(key) && j[key].is_number()) out = j[key].get<float>();
    };
    auto readB = [&](const char *key, bool &out) {
      if (j.contains(key) && j[key].is_boolean()) out = j[key].get<bool>();
    };
    auto readS = [&](const char *key, std::string &out) {
      if (j.contains(key) && j[key].is_string()) out = j[key].get<std::string>();
    };
    auto readVec4 = [&](const char *key, RC::Vector4 &out) {
      if (!j.contains(key)) return;
      const auto &c = j[key];
      if (c.is_array() && c.size() >= 4) {
        out = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>(), c[3].get<float>()};
      }
    };

    readS("titleText", titleText);
    readS("fontPath", fontPath);
    readF("letterSize", letterSize);
    readF("letterDepth", letterDepth);
    readVec4("letterColor", letterColor);
    readB("outlineEnabled", outlineEnabled);
    readF("outlineWidth", outlineWidth);
    readVec4("outlineColor", outlineColor);
    if (j.contains("letterPositions") && j["letterPositions"].is_array()) {
      letterPositions.clear();
      for (const auto &p : j["letterPositions"]) {
        if (!p.is_array() || p.size() < 2) continue;
        RC::Vector3 v{p[0].get<float>(), p[1].get<float>(), 0.0f};
        if (p.size() >= 3) v.z = p[2].get<float>();
        letterPositions.push_back(v);
      }
    }
    readF("floatOffset", floatOffset);
    readF("bobAmplitude", bobAmplitude);
    readF("bobPeriod", bobPeriod);
    readF("driftRadius", driftRadius);
    readF("driftPeriod", driftPeriod);
    readF("tiltScale", tiltScale);
    readB("rippleFloat", rippleFloat);
    readF("rippleTiltScale", rippleTiltScale);
    readF("letterShininess", letterShininess);
    readF("letterEnvCoeff", letterEnvCoeff);
    if (j.contains("titlePost") && j["titlePost"].is_object()) {
      const auto &p = j["titlePost"];
      auto pf = [&](const char *key, float &out) {
        if (p.contains(key) && p[key].is_number()) out = p[key].get<float>();
      };
      auto pb = [&](const char *key, bool &out) {
        if (p.contains(key) && p[key].is_boolean()) out = p[key].get<bool>();
      };
      pb("enabled", post.enabled);
      pb("bloom", post.bloom);
      pf("bloomThreshold", post.bloomThreshold);
      pf("bloomIntensity", post.bloomIntensity);
      pf("bloomRadius", post.bloomRadius);
      pf("bloomKnee", post.bloomKnee);
      pb("colorGrade", post.colorGrade);
      pf("gradeExposure", post.gradeExposure);
      pf("gradeContrast", post.gradeContrast);
      pf("gradeSaturation", post.gradeSaturation);
      pf("gradeTemperature", post.gradeTemperature);
      pf("gradeTint", post.gradeTint);
      pf("gradeAmount", post.gradeAmount);
      pb("vignette", post.vignette);
    }

    readS("menuFontPath", menuFontPath);
    readF("menuSize", menuSize);
    readF("menuDepth", menuDepth);
    if (j.contains("menuPositions") && j["menuPositions"].is_array()) {
      menuPositions.clear();
      for (const auto &p : j["menuPositions"]) {
        if (!p.is_array() || p.size() < 2) continue;
        menuPositions.push_back({p[0].get<float>(), p[1].get<float>()});
      }
    }
    readVec4("menuColor", menuColor);
    readVec4("menuSelectedColor", menuSelectedColor);
    readVec4("menuOutlineColor", menuOutlineColor);
    readF("menuOutlineWidth", menuOutlineWidth);
    readF("menuSelectedScale", menuSelectedScale);
    readF("menuSelectedLift", menuSelectedLift);
    readF("menuBobAmplitude", menuBobAmplitude);
    readF("menuTiltScale", menuTiltScale);
    readB("quitEnabled", quitEnabled);
    readF("menuShininess", menuShininess);
    readF("menuUnselectedScale", menuUnselectedScale);
    readF("menuPulseWhite", menuPulseWhite);

    // 入れ子のオブジェクト用（"menuRing" / "intro"）
    auto sub = [&](const char *name) -> const nlohmann::json * {
      return (j.contains(name) && j[name].is_object()) ? &j[name] : nullptr;
    };
    auto subF = [](const nlohmann::json &o, const char *key, float &out) {
      if (o.contains(key) && o[key].is_number()) out = o[key].get<float>();
    };
    auto subB = [](const nlohmann::json &o, const char *key, bool &out) {
      if (o.contains(key) && o[key].is_boolean()) out = o[key].get<bool>();
    };
    if (const nlohmann::json *r = sub("menuRing")) {
      subB(*r, "enabled", ring.enabled);
      if (r->contains("color") && (*r)["color"].is_array() && (*r)["color"].size() >= 4) {
        const auto &c = (*r)["color"];
        ring.color = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>(), c[3].get<float>()};
      }
      subF(*r, "size", ring.size);
      subF(*r, "gap", ring.gap);
      subF(*r, "spinSpeed", ring.spinSpeed);
      subF(*r, "height", ring.height);
      subF(*r, "follow", ring.follow);
      subF(*r, "rippleInterval", ring.rippleInterval);
      subF(*r, "rippleStrength", ring.rippleStrength);
      subF(*r, "rippleRadius", ring.rippleRadius);
      subF(*r, "selectRipple", ring.selectRipple);
    }
    if (const nlohmann::json *o = sub("intro")) {
      subB(*o, "enabled", intro.enabled);
      subF(*o, "delay", intro.delay);
      subF(*o, "interval", intro.interval);
      subF(*o, "riseTime", intro.riseTime);
      subF(*o, "depth", intro.depth);
      subF(*o, "overshoot", intro.overshoot);
      subF(*o, "menuDelay", intro.menuDelay);
      subF(*o, "menuInterval", intro.menuInterval);
      subF(*o, "menuRiseTime", intro.menuRiseTime);
      subF(*o, "menuDepth", intro.menuDepth);
      subB(*o, "splash", intro.splash);
      if (o->contains("splashCount") && (*o)["splashCount"].is_number()) {
        intro.splashCount = (*o)["splashCount"].get<int>();
      }
      subF(*o, "rippleStrength", intro.rippleStrength);
      subF(*o, "rippleRadius", intro.rippleRadius);
      subB(*o, "skippable", intro.skippable);
    }

    readB("showHint", showHint);
    readS("hintFontPath", hintFontPath);
    readF("hintFontSize", hintFontSize);
    readVec4("hintBandColor", hintBandColor);
    readF("hintBandPadding", hintBandPadding);

    dive.ReadJson(j);
    rise.ReadJson(j);
    if (j.contains("underwater")) look.FromJson(j["underwater"]);
  }

#if RC_ENABLE_IMGUI
  void OnImGui() override {
    ImGui::Text("Letters: %d  Menu: %d  Selected: %s", static_cast<int>(letters_.size()),
                static_cast<int>(menu_.size()), selected_ == kMenuStart ? "Start" : "Quit");
    ImGui::Text("Water time: %.2f", RC::GetWaterTime());
    ImGui::SeparatorText("Title Letters");
    ImGui::DragFloat("Float Offset", &floatOffset, 0.01f, -1.0f, 1.0f);
    ImGui::DragFloat("Bob Amplitude", &bobAmplitude, 0.01f, 0.0f, 1.0f);
    ImGui::DragFloat("Bob Period", &bobPeriod, 0.1f, 0.2f, 20.0f);
    ImGui::DragFloat("Drift Radius", &driftRadius, 0.01f, 0.0f, 5.0f);
    ImGui::DragFloat("Drift Period", &driftPeriod, 0.1f, 0.5f, 60.0f);
    ImGui::DragFloat("Tilt Scale", &tiltScale, 0.05f, 0.0f, 3.0f);
    if (ImGui::Checkbox("Ripple Float", &rippleFloat)) {
      RC::SetInteractiveWaterReadback(rippleFloat);
      readbackOwned_ = rippleFloat;
    }
    ImGui::SameLine();
    ImGui::TextDisabled(RC::IsInteractiveWaterReadbackEnabled() ? "(readback on)" : "(readback off)");
    ImGui::DragFloat("Ripple Tilt", &rippleTiltScale, 0.05f, 0.0f, 3.0f);
    if (ImGui::ColorEdit4("Letter Color", &letterColor.x)) {
      for (auto &f : letters_) ApplyMainColor(f, letterColor);
    }
    ImGui::TextDisabled("letterSize / outline / shininess are applied on scene reload");

    ImGui::SeparatorText("Title Post Effects");
    bool postChanged = false;
    postChanged |= ImGui::Checkbox("Post Enabled", &post.enabled);
    postChanged |= ImGui::Checkbox("Bloom##TitlePost", &post.bloom);
    postChanged |= ImGui::SliderFloat("Bloom Threshold##TitlePost", &post.bloomThreshold, 0.0f, 1.0f);
    postChanged |= ImGui::SliderFloat("Bloom Intensity##TitlePost", &post.bloomIntensity, 0.0f, 3.0f);
    postChanged |= ImGui::SliderFloat("Bloom Radius##TitlePost", &post.bloomRadius, 1.0f, 16.0f);
    postChanged |= ImGui::SliderFloat("Bloom Knee##TitlePost", &post.bloomKnee, 0.01f, 1.0f);
    postChanged |= ImGui::Checkbox("ColorGrade##TitlePost", &post.colorGrade);
    postChanged |= ImGui::SliderFloat("Exposure##TitlePost", &post.gradeExposure, -3.0f, 3.0f);
    postChanged |= ImGui::SliderFloat("Contrast##TitlePost", &post.gradeContrast, 0.5f, 2.0f);
    postChanged |= ImGui::SliderFloat("Saturation##TitlePost", &post.gradeSaturation, 0.0f, 2.0f);
    postChanged |= ImGui::SliderFloat("Temperature##TitlePost", &post.gradeTemperature, -1.0f, 1.0f);
    postChanged |= ImGui::SliderFloat("Tint##TitlePost", &post.gradeTint, -1.0f, 1.0f);
    postChanged |= ImGui::SliderFloat("Grade Amount##TitlePost", &post.gradeAmount, 0.0f, 1.0f);
    postChanged |= ImGui::Checkbox("Vignette##TitlePost", &post.vignette);
    if (postChanged) {
      RemoveTitlePost();
      ApplyTitlePost();
    }

    ImGui::SeparatorText("Menu");
    ImGui::ColorEdit4("Menu Color", &menuColor.x);
    ImGui::ColorEdit4("Selected Color", &menuSelectedColor.x);
    ImGui::DragFloat("Selected Scale", &menuSelectedScale, 0.01f, 1.0f, 2.0f);
    ImGui::DragFloat("Selected Lift", &menuSelectedLift, 0.01f, 0.0f, 1.0f);
    ImGui::DragFloat("Menu Bob", &menuBobAmplitude, 0.01f, 0.0f, 0.5f);
    ImGui::DragFloat("Menu Tilt", &menuTiltScale, 0.05f, 0.0f, 2.0f);
    ImGui::DragFloat("Unselected Scale", &menuUnselectedScale, 0.01f, 0.3f, 1.5f);
    ImGui::DragFloat("Pulse White", &menuPulseWhite, 0.01f, 0.0f, 1.0f);
    ImGui::Checkbox("Quit Enabled", &quitEnabled);

    ImGui::SeparatorText("Menu Ring");
    ImGui::Checkbox("Ring Enabled", &ring.enabled);
    ImGui::ColorEdit4("Ring Color", &ring.color.x);
    ImGui::DragFloat("Ring Size", &ring.size, 0.01f, 0.05f, 3.0f);
    ImGui::DragFloat("Ring Gap", &ring.gap, 0.01f, 0.0f, 5.0f);
    ImGui::DragFloat("Ring Spin", &ring.spinSpeed, 0.05f, 0.0f, 20.0f);
    ImGui::DragFloat("Ring Height", &ring.height, 0.01f, -1.0f, 2.0f);
    ImGui::DragFloat("Ring Ripple Interval", &ring.rippleInterval, 0.05f, 0.0f, 10.0f);
    ImGui::DragFloat("Ring Ripple Strength", &ring.rippleStrength, 0.01f, -1.0f, 1.0f);
    ImGui::DragFloat("Select Ripple", &ring.selectRipple, 0.01f, -1.0f, 1.0f);

    ImGui::SeparatorText("Intro");
    ImGui::Text("Intro: %s  t=%.2f", introActive_ ? "playing" : "done", introTime_);
    ImGui::Checkbox("Intro Enabled", &intro.enabled);
    ImGui::DragFloat("Intro Delay", &intro.delay, 0.01f, 0.0f, 5.0f);
    ImGui::DragFloat("Intro Interval", &intro.interval, 0.01f, 0.0f, 2.0f);
    ImGui::DragFloat("Intro Rise Time", &intro.riseTime, 0.01f, 0.05f, 5.0f);
    ImGui::DragFloat("Intro Depth", &intro.depth, 0.05f, 0.0f, 20.0f);
    ImGui::DragFloat("Intro Overshoot", &intro.overshoot, 0.05f, 0.0f, 5.0f);
    ImGui::DragFloat("Menu Delay", &intro.menuDelay, 0.01f, 0.0f, 5.0f);
    ImGui::DragFloat("Menu Rise Time", &intro.menuRiseTime, 0.01f, 0.05f, 5.0f);
    ImGui::Checkbox("Splash", &intro.splash);
    ImGui::DragInt("Splash Count", &intro.splashCount, 1, 0, 512);
    ImGui::DragFloat("Intro Ripple", &intro.rippleStrength, 0.01f, -2.0f, 2.0f);
    // 編集モードは時間が進まず文字が沈んだままになるので、再生中だけ
    SceneContext *introCtx = GetSceneContext();
    if (introCtx && introCtx->isPlaying()) {
      if (ImGui::Button("Replay Intro")) StartIntro();
    } else {
      ImGui::TextDisabled("Replay Intro: 再生中のみ");
    }

    ImGui::SeparatorText("Hint");
    ImGui::ColorEdit4("Hint Band", &hintBandColor.x);
    ImGui::DragFloat("Hint Band Padding", &hintBandPadding, 0.5f, 0.0f, 60.0f);
    ImGui::Checkbox("Show Hint", &showHint);
    if (Scene *scene = GetScene()) {
      ImGui::TextUnformatted(("Scene Flow: " + SceneFlow::Get().Describe(scene->Name(), kTriggerStart)).c_str());
    }
    ImGui::TextDisabled("menuSize / menuPositions / fonts are applied on scene reload");

    ImGui::SeparatorText("Dive (Start -> Game)");
    ImGui::Text("Dive: %s  depth=%.1f   Rise: %s",
                WaterCameraFx::DiveSequence::PhaseName(dive_.GetPhase()), dive_.Depth(),
                rise_.IsMoving() ? "moving" : (rise_.IsActive() ? "fx" : "idle"));
    dive.DrawImGui();
    if (ImGui::Button("Test Dive (no scene change)")) {
      diveTestOnly_ = true;
      BeginDive();
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset Camera")) {
      EndDive(/*restoreCamera=*/true);
    }
    ImGui::SeparatorText("Rise (entered by dive transition)");
    rise.DrawImGui();
    if (ImGui::Button("Test Rise")) {
      if (!dive_.IsActive() && !rise_.IsActive()) BeginRise();
    }
    UnderwaterLook::DrawImGui(look);
  }
#endif

protected:
  void OnCreate() override {
    // 前のシーン（Game）の障害物が水面の定数バッファに残っていると、
    // タイトルの水面に岩の反射波が出てしまう。ここには障害物が無いので空にする。
    RC::SetWaterObstacles(nullptr, 0);

    // 文字・メニューを波紋にも乗せるため、波紋ハイトマップの CPU 読み戻しを有効にする。
    // タイトルだけの機能なので OnDestroy で必ず戻す（Game では走らせない）。
    if (rippleFloat && !RC::IsInteractiveWaterReadbackEnabled()) {
      RC::SetInteractiveWaterReadback(true);
      readbackOwned_ = true;
    }

    if (showHint) {
      hintFont_ = RC::LoadFont(hintFontPath, hintFontSize);
      if (hintFont_ < 0) {
        Log::Print("[TitleScreenScript] failed to load hint font: " + hintFontPath);
      }
    }

    SpawnLetters();
    SpawnMenu();
    SpawnRings();
    ApplyTitlePost();

    // 起動演出を出すか決める。編集モード（再生していない）では出さない（配置を詰めるときに文字が沈んでいると困る）。
    // 浮上（Result → Title の Dive 遷移）で入ってきたときも出す：浮上中は文字とメニューを隠しておき
    // （上がってくるカメラが水中の文字を突き抜けないように）、カメラが真上視点に落ち着いてから始める
    bool willRise = false;
    bool playing = false;
    if (SceneContext *ctx = GetSceneContext()) {
      playing = ctx->isPlaying();
      willRise = rise.enabled && playing && ctx->lastTransition == SceneTransition::Dive;
    }
    if (intro.enabled && playing) {
      StartIntro();
      if (willRise) {
        introWaitRise_ = true;
        SetFloatersVisible(false);
      }
    }

    // 生成直後の 1 フレームだけ原点にメッシュが見えるのを防ぐため、
    // Transform をすぐ描画側へも書いておく（起動演出なら沈んだ位置で）
    UpdateFloaters(0.0f);

    // 泡のエミッタはここで作っておく（GPU パーティクルの初期化を飛び込みの瞬間に
    // 走らせるとそこで一瞬止まる）。射出数 0 で寝かせておき、着水／浮上で起こす。
    if ((dive.enabled && dive.bubbles) || (rise.enabled && rise.bubbles)) {
      bubbles_.Spawn(GetScene(), "TitleBubbles");
    }
    // 起動演出の飛沫も同じ理由で先に作っておく
    if (intro.enabled && intro.splash) SpawnSplashEmitter();

    // 飛び込み遷移（Result の「タイトルへ」など）で入ってきたときは、深海から浮上して始まる。
    // 編集モードでもスクリプトは作られるので、再生中だけ（カメラ位置が保存に焼き付かないように）
    if (willRise) BeginRise();
  }

  void OnUpdate(float deltaTime) override {
    time_ += deltaTime;
    pulseTimer_ += deltaTime;

    // 飛沫は 1 フレームだけ射出して止める（射出数は「1 フレームあたり」なので）
    if (splashFramesLeft_ > 0 && --splashFramesLeft_ == 0) SetSplashEmission(0);

    if (introActive_) UpdateIntro(deltaTime);

    UpdateFloaters(deltaTime);
    UpdateRings(deltaTime);

    if (deltaTime > 0.0f && rise_.IsActive()) {
      rise_.Update(deltaTime);
    }
    // 浮上・飛び込みの後始末（UnderwaterLook::RemoveStack）で Vignette が外れるため、
    // 水上に戻ったらタイトル用のエフェクトを積み直す（積み済みなら何もしない）
    // 決定後（遷移演出中）は積み直さない（Dissolve などの遷移エフェクトより後ろに積まれるため）
    if (!decided_ && !dive_.IsActive() && !rise_.IsActive()) ApplyTitlePost();
    // 浮上でカメラが動いているあいだはメニューを操作させない
    if (rise_.IsMoving()) return;

    if (dive_.IsActive()) {
      if (deltaTime > 0.0f) UpdateDive(deltaTime);
      return;
    }

    if (introActive_) {
      // 演出中は操作させない。キーを押したら最後まで飛ばす（その押下はメニュー操作に使わない）
      if (intro.skippable && AnyMenuKeyTriggered()) SkipIntro();
      return;
    }

    if (!decided_) {
      HandleInput();
    }
  }

  void OnDestroy() override {
    // エディタの停止 → 再生でスクリプトだけ作り直されると文字が二重に生えるため、
    // 自分が生成した文字エンティティは自分で片付ける。
    for (auto &f : letters_) {
      if (auto e = f.entity.lock()) e->Destroy();
    }
    for (auto &f : menu_) {
      if (auto e = f.entity.lock()) e->Destroy();
    }
    for (auto &r : rings_) {
      if (auto e = r.lock()) e->Destroy();
    }
    if (auto e = splash_.lock()) e->Destroy();
    if (auto e = foam_.lock()) e->Destroy();
    foam_.reset();
    letters_.clear();
    menu_.clear();
    rings_.clear();
    splash_.reset();
    // 自分が有効にした読み戻しだけ戻す（他が使っていれば触らない）
    if (readbackOwned_) {
      RC::SetInteractiveWaterReadback(false);
      readbackOwned_ = false;
    }
    // 飛び込み／浮上の途中でエディタから停止された場合は、カメラと画面を元へ戻す。
    // シーン遷移で抜ける通常の経路では、切り替え側が ClearPostEffects するので
    // ここで戻しても二重にはならない。
    if (dive_.IsActive()) EndDive(/*restoreCamera=*/true);
    if (rise_.IsActive()) rise_.End();
    bubbles_.Destroy();
    // エディタで停止したときにタイトル用のエフェクトが編集画面へ残らないようにする
    RemoveTitlePost();

    if (hintFont_ >= 0) {
      RC::UnloadFont(hintFont_);
      hintFont_ = -1;
    }
  }

  void OnRender() override {
    // 飛び込み中・浮上中は操作ヒントを出さない
    if (!showHint || hintFont_ < 0 || dive_.IsActive() || rise_.IsMoving()) return;
    // 起動演出中は、メニューが出そろうまで出さない
    if (introActive_) return;

    float screenW = 1280.0f;
    float screenH = 720.0f;
    auto &rc = RC::GetRenderContext();
    if (rc.Ctx() && rc.Ctx()->app) {
      screenW = static_cast<float>(rc.Ctx()->app->width);
      screenH = static_cast<float>(rc.Ctx()->app->height);
    }
    DrawHint(screenW, screenH);
  }

private:
  enum MenuItem { kMenuStart = 0, kMenuQuit = 1, kMenuCount = 2 };

  /// @brief 水面に浮かせる 3D 文字 1 つぶんの状態（タイトル文字・メニュー項目で共通）
  struct Floater {
    std::weak_ptr<Entity> entity;
    RC::Vector3 basePos{};      ///< 静水面上の基準位置（x, 0, z）。z はベースライン
    float yaw = 0.0f;           ///< 水面上での向き（rad）
    float phase = 0.0f;         ///< 漂い・上下動の位相オフセット
    float depth = 0.2f;         ///< 押し出し厚さ（底面の位置を出すのに使う）
    float halfW = 1.0f;         ///< 足元サンプル範囲：ローカル X の半幅（scale 1 のとき）
    float zMin = 0.0f;          ///< 足元サンプル範囲：ローカル Y（＝ワールド Z）の下端
    float zMax = 1.0f;          ///< 足元サンプル範囲：ローカル Y（＝ワールド Z）の上端
    float drift = 0.0f;         ///< 円運動の半径（0 で漂わない）
    float bob = 0.0f;           ///< 上下動の振幅
    float tilt = 1.0f;          ///< 水面法線への追従の強さ
    float lift = 0.0f;          ///< 追加で浮かせる量（選択中のメニュー）
    float scale = 1.0f;         ///< 現在の拡大率（アニメーション）
    float targetScale = 1.0f;   ///< 目標の拡大率
    float introY = 0.0f;        ///< 起動演出の上下オフセット（負で水中）
    float introScale = 1.0f;    ///< 起動演出の拡大率（メニューの出現）
    bool breached = true;       ///< 起動演出で水面を抜けたか（抜けた瞬間に飛沫・波紋を出す）
  };

  // ------------------------------------------------------------------
  // 文字の生成
  // ------------------------------------------------------------------

  /// @brief UTF-8 文字列を 1 文字ずつに分解する
  static std::vector<std::string> SplitUtf8(const std::string &s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
      const unsigned char c = static_cast<unsigned char>(s[i]);
      size_t len = 1;
      if ((c & 0xF8) == 0xF0) len = 4;
      else if ((c & 0xF0) == 0xE0) len = 3;
      else if ((c & 0xE0) == 0xC0) len = 2;
      len = (std::min)(len, s.size() - i);
      std::string ch = s.substr(i, len);
      // 空白は浮かせない（詰めてしまう）
      if (ch != " " && ch != "　") out.push_back(ch);
      i += len;
    }
    return out;
  }

  /// @brief 配置指定が無い文字の既定位置
  /// @details 左から右へ読み順に並べつつ、Z（画面の上下）をばらつかせて
  ///          「バラバラに浮いている」感じにする。画面上が +Z。
  RC::Vector3 DefaultLetterPosition(int index, int count) const {
    const float spacing = letterSize * 1.55f;
    const float x0 = -spacing * static_cast<float>(count - 1) * 0.5f;
    // 決め打ちのジグザグ（乱数だと毎回位置が変わって見た目を詰められないため）
    static const float kZOffsets[] = {0.9f, -0.5f, 1.4f, -0.9f, 0.4f, -1.2f, 0.7f, -0.3f};
    static const float kYaws[] = {-0.10f, 0.08f, -0.04f, 0.12f, -0.07f, 0.05f, -0.11f, 0.06f};
    const int n = static_cast<int>(sizeof(kZOffsets) / sizeof(kZOffsets[0]));
    RC::Vector3 p{};
    p.x = x0 + spacing * static_cast<float>(index);
    p.y = letterSize * 0.9f + kZOffsets[index % n] * letterSize * 0.35f; // ここでは y に Z を仮置き
    p.z = kYaws[index % n];                                              // ここでは z に yaw を仮置き
    return p;
  }

  /// @brief 3D 文字エンティティを 1 つ生成して水面用の Floater として登録する
  /// @return 生成した Floater（エンティティが作れなければ entity が空）
  Floater CreateTextFloater(const std::string &name, const std::string &text,
                            const std::string &font, float size, float depth,
                            const RC::Vector4 &color, const RC::Vector4 &outline,
                            float outlineW, float x, float z, float yaw,
                            float shininess = 32.0f, float envCoeff = 0.0f) {
    Floater f;
    f.basePos = {x, 0.0f, z};
    f.yaw = yaw;
    f.depth = depth;
    // AABB が取れなかったときの保険（全角 1 文字 ≒ 1em 幅、高さ 0.8em）
    f.halfW = size * 0.5f * static_cast<float>((std::max)(SplitUtf8(text).size(), size_t(1)));
    f.zMin = -size * 0.1f;
    f.zMax = size * 0.8f;

    Scene *scene = GetScene();
    if (!scene) return f;
    auto entity = scene->CreateEntity(name);
    if (!entity) return f;

    auto &tr = entity->AddComponent<TransformComponent>();
    tr.position = {x, depth * 0.5f + floatOffset, z};
    tr.rotation = {kHalfPi, yaw, 0.0f};
    tr.scale = {1.0f, 1.0f, 1.0f};

    auto &tm = entity->AddComponent<TextMeshComponent>();
    tm.text = text;
    tm.fontPath = font;
    tm.size = size;
    tm.depth = depth;
    tm.align = TextAlign::Center;
    tm.color = color;
    tm.lightingMode = -1; // DirectionalLight に追従
    tm.shininess = shininess;
    tm.environmentCoeff = envCoeff;
    tm.outlineEnabled = outlineEnabled;
    tm.outlineWidth = outlineW;
    tm.outlineColor = outline;
    tm.outlineUnlit = true;

    // メッシュ生成（EnsureTextMesh）はここで済む
    scene->InitDynamicEntityRuntime(*entity);

    // 生成されたメッシュの実寸で足元のサンプル範囲を決める。
    // ローカル X が幅、ローカル Y が文字の高さ（回転後はワールド Z）。
    if (tm.HasMesh() && tm.info.vertexCount > 0) {
      f.halfW = (std::max)(std::fabs(tm.info.min.x), std::fabs(tm.info.max.x));
      f.zMin = tm.info.min.y;
      f.zMax = tm.info.max.y;
    }

    f.entity = entity;
    return f;
  }

  void SpawnLetters() {
    const std::vector<std::string> chars = SplitUtf8(titleText);
    const int count = static_cast<int>(chars.size());
    letters_.clear();
    letters_.reserve(chars.size());

    for (int i = 0; i < count; ++i) {
      RC::Vector3 placement{}; // x, z, yaw
      if (i < static_cast<int>(letterPositions.size())) {
        placement = letterPositions[i];
      } else {
        const RC::Vector3 d = DefaultLetterPosition(i, count);
        placement = {d.x, d.y, d.z};
      }

      Floater f = CreateTextFloater("TitleLetter_" + std::to_string(i), chars[i], fontPath,
                                    letterSize, letterDepth, letterColor, outlineColor,
                                    outlineWidth, placement.x, placement.y, placement.z,
                                    letterShininess, letterEnvCoeff);
      if (f.entity.expired()) continue;
      f.phase = static_cast<float>(i) * 1.37f;
      f.drift = driftRadius;
      f.bob = bobAmplitude;
      f.tilt = tiltScale;
      letters_.push_back(f);
    }

    Log::Print("[TitleScreenScript] spawned " + std::to_string(letters_.size()) +
               " letters for \"" + titleText + "\"");
  }

  void SpawnMenu() {
    static const char *kLabels[kMenuCount] = {"スタート", "ゲーム終了"};
    static const char *kNames[kMenuCount] = {"TitleMenu_Start", "TitleMenu_Quit"};

    menu_.clear();
    menu_.reserve(kMenuCount);
    for (int i = 0; i < kMenuCount; ++i) {
      // 既定：画面中央の下寄りに 2 行。行間は 1.7em
      RC::Vector2 pos{0.0f, -menuSize * (2.6f + 1.7f * static_cast<float>(i))};
      if (i < static_cast<int>(menuPositions.size())) pos = menuPositions[i];

      const bool sel = (i == selected_);
      Floater f = CreateTextFloater(kNames[i], kLabels[i], menuFontPath, menuSize, menuDepth,
                                    sel ? menuSelectedColor : menuColor, menuOutlineColor,
                                    menuOutlineWidth, pos.x, pos.y, 0.0f, menuShininess, 0.0f);
      if (f.entity.expired()) continue;
      f.phase = 2.0f + static_cast<float>(i) * 1.9f;
      f.drift = 0.0f; // メニューは読ませたいので漂わせない
      f.bob = menuBobAmplitude;
      f.tilt = menuTiltScale;
      f.scale = f.targetScale = sel ? menuSelectedScale : menuUnselectedScale;
      f.lift = sel ? menuSelectedLift : 0.0f;
      menu_.push_back(f);
    }
  }

  // ------------------------------------------------------------------
  // 水面追従
  // ------------------------------------------------------------------

  /// @brief シーン内の WaterComponent から波パラメータを組み立てる
  bool BuildWaterParams(RC::WaterWaveParams &out) const {
    Scene *scene = GetScene();
    if (!scene) return false;
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

  void UpdateFloaters(float deltaTime) {
    RC::WaterWaveParams params;
    const bool hasWater = BuildWaterParams(params);
    const float waterTime = RC::GetWaterTime();

    for (auto &f : letters_) UpdateFloater(f, params, hasWater, waterTime, deltaTime);

    // メニューは選択状態で色・大きさ・浮き上がりを変える
    const float pulse = 0.5f + 0.5f * std::sin(pulseTimer_ * (kTwoPi / kPulsePeriod));
    for (size_t i = 0; i < menu_.size(); ++i) {
      Floater &f = menu_[i];
      const bool sel = (static_cast<int>(i) == selected_);
      f.targetScale = sel ? menuSelectedScale : menuUnselectedScale;
      f.lift = sel ? menuSelectedLift : 0.0f;

      RC::Vector4 color = menuColor;
      if (sel) {
        // 選択色 ⇔ 白 のあいだでゆっくり明滅
        const float t = pulse * menuPulseWhite;
        color = {menuSelectedColor.x + (1.0f - menuSelectedColor.x) * t,
                 menuSelectedColor.y + (1.0f - menuSelectedColor.y) * t,
                 menuSelectedColor.z + (1.0f - menuSelectedColor.z) * t,
                 menuSelectedColor.w};
      }
      ApplyMainColor(f, color);
      UpdateFloater(f, params, hasWater, waterTime, deltaTime);
    }
  }

  // ------------------------------------------------------------------
  // 選択中メニューの目印（左右で回る金のリング ＋ 波紋）
  // ------------------------------------------------------------------

  /// @brief メニュー項目の見た目の中心（x, z）と半幅を求める
  bool MenuItemExtent(int index, float &cx, float &cz, float &halfW) const {
    if (index < 0 || index >= static_cast<int>(menu_.size())) return false;
    const Floater &f = menu_[index];
    const float s = f.targetScale * f.introScale;
    cx = f.basePos.x;
    cz = f.basePos.z + (f.zMin + f.zMax) * 0.5f * s;
    halfW = f.halfW * s;
    return true;
  }

  /// @brief リング（Torus）を左右に 1 つずつ作る。最初は大きさ 0 で隠しておく
  void SpawnRings() {
    rings_.clear();
    Scene *scene = GetScene();
    if (!scene) return;
    for (int i = 0; i < 2; ++i) {
      auto e = scene->CreateEntity(i == 0 ? "TitleMenuRing_L" : "TitleMenuRing_R");
      if (!e) continue;
      e->SetTag("transient", 1); // シーン保存に含めない
      auto &tr = e->AddComponent<TransformComponent>();
      tr.position = {0.0f, -50.0f, 0.0f};
      tr.scale = {0.0f, 0.0f, 0.0f};
      auto &pm = e->AddComponent<PrimitiveMeshComponent>();
      pm.type = PrimitiveType::Torus; // XZ 平面の輪（外径 1.3）
      pm.color = ring.color;
      pm.shininess = 0.0f;            // ハイライトは付けない（面全体が白く光ってしまうため）
      scene->InitDynamicEntityRuntime(*e);
      rings_.push_back(e);
    }
    ringInit_ = false;
    ringAppear_ = 0.0f;
  }

  void UpdateRings(float deltaTime) {
    if (rings_.empty()) return;
    const float dt = (std::max)(deltaTime, 0.0f);

    // 出す／隠す（演出中・飛び込み中・浮上中は隠す）。大きさ 0↔1 でふわっと出し入れする
    const bool want = ring.enabled && !introActive_ && !dive_.IsActive() && !rise_.IsMoving();
    ringAppear_ = std::clamp(ringAppear_ + (want ? 4.0f : -6.0f) * dt, 0.0f, 1.0f);
    if (dt == 0.0f && want) ringAppear_ = 1.0f; // 編集モード（時間が止まっている）でも見えるように

    float cx = 0.0f, cz = 0.0f, hw = 1.0f;
    if (!MenuItemExtent(selected_, cx, cz, hw)) return;
    if (!ringInit_) {
      ringCx_ = cx;
      ringCz_ = cz;
      ringHalfW_ = hw;
      ringInit_ = true;
    } else {
      const float k = (dt > 0.0f) ? 1.0f - std::exp(-ring.follow * dt) : 1.0f;
      ringCx_ += (cx - ringCx_) * k;
      ringCz_ += (cz - ringCz_) * k;
      ringHalfW_ += (hw - ringHalfW_) * k;
    }
    ringSpin_ += ring.spinSpeed * dt;
    ringPop_ += (1.0f - ringPop_) * (std::min)(1.0f, dt * 8.0f);

    RC::WaterWaveParams params;
    const bool hasWater = BuildWaterParams(params);
    const float waterTime = RC::GetWaterTime();

    const float s = ring.size * ringAppear_ * ringPop_;
    const bool active = ringAppear_ > 0.001f;
    for (size_t i = 0; i < rings_.size(); ++i) {
      auto e = rings_[i].lock();
      if (!e) continue;
      e->SetActive(active);
      auto *tr = e->GetComponent<TransformComponent>();
      if (!tr) continue;
      const float side = (i == 0) ? -1.0f : 1.0f;
      const float x = ringCx_ + side * (ringHalfW_ + ring.gap);
      const float z = ringCz_;
      float y = hasWater ? RC::WaterSurface::SampleHeight(params, waterTime, x, z) : 0.0f;
      if (rippleFloat && RC::IsInteractiveWaterReadbackEnabled()) y += RC::SampleInteractiveWaterHeight(x, z);
      y += ring.height + 0.05f * std::sin(time_ * 2.2f + side);
      tr->position = {x, y, z};
      // 水平軸（X）まわりに回すと、真上から見て輪が表裏に返りながら回って見える。
      // 左右で位相を 90 度ずらして、同じ向きにそろわないようにする
      tr->rotation = {ringSpin_ + (i == 0 ? 0.0f : kHalfPi), 0.0f, 0.0f};
      tr->scale = {s, s, s};
      if (auto *pm = e->GetComponent<PrimitiveMeshComponent>()) {
        if (pm->meshHandle >= 0) {
          if (auto *p = RC::GetPrimitiveMeshTransformPtr(pm->meshHandle)) *p = tr->ToTransform();
        }
      }
    }

    // リングの下から一定間隔で波紋を出す
    if (want && dt > 0.0f && ring.rippleInterval > 0.0f) {
      ringRippleTimer_ += dt;
      if (ringRippleTimer_ >= ring.rippleInterval) {
        ringRippleTimer_ = 0.0f;
        for (float side : {-1.0f, 1.0f}) {
          RC::AddWaveSourceAtWorld(ringCx_ + side * (ringHalfW_ + ring.gap), ringCz_, ring.rippleRadius,
                                   ring.rippleStrength);
        }
      }
    }
  }

  /// @brief 選択を切り替えた瞬間：リングを弾ませ、新しい項目の下へ波紋を並べて出す
  void OnSelectionChanged() {
    ringPop_ = 1.5f;
    ringRippleTimer_ = 0.0f;
    float cx = 0.0f, cz = 0.0f, hw = 1.0f;
    if (!MenuItemExtent(selected_, cx, cz, hw)) return;
    if (ring.selectRipple == 0.0f) return;
    for (int k = -2; k <= 2; ++k) {
      RC::AddWaveSourceAtWorld(cx + static_cast<float>(k) * hw * 0.45f, cz, 0.025f, ring.selectRipple);
    }
  }

  // ------------------------------------------------------------------
  // 起動演出（文字が 1 文字ずつ水中から浮かび上がる → メニュー）
  // ------------------------------------------------------------------

  /// @brief easeOutBack（c1 = 0 なら easeOutCubic）。1 を少し行き過ぎてから戻る
  static float EaseOutBack(float p, float c1) {
    p = std::clamp(p, 0.0f, 1.0f);
    const float c3 = c1 + 1.0f;
    const float q = p - 1.0f;
    return 1.0f + c3 * q * q * q + c1 * q * q;
  }

  float IntroLetterStart(int i) const { return intro.delay + intro.interval * static_cast<float>(i); }

  /// @brief メニューが出始める時刻：最後の文字が水面を抜けたころ ＋ menuDelay
  float IntroMenuStart() const {
    const int n = (std::max)(static_cast<int>(letters_.size()), 1);
    return IntroLetterStart(n - 1) + intro.riseTime * 0.6f + intro.menuDelay;
  }

  float IntroEnd() const {
    const int m = (std::max)(static_cast<int>(menu_.size()), 1);
    return IntroMenuStart() + intro.menuInterval * static_cast<float>(m - 1) + intro.menuRiseTime;
  }

  void StartIntro() {
    introActive_ = true;
    introTime_ = 0.0f;
    for (auto &f : letters_) {
      f.introY = -intro.depth;
      f.introScale = 1.0f;
      f.breached = false;
    }
    for (auto &f : menu_) {
      f.introY = -intro.menuDepth;
      f.introScale = 0.6f;
      f.breached = false;
    }
    ringAppear_ = 0.0f;
  }

  /// @brief タイトル文字とメニューの表示を切り替える（浮上中に隠す）
  void SetFloatersVisible(bool visible) {
    for (auto &f : letters_) {
      if (auto e = f.entity.lock()) e->SetActive(visible);
    }
    for (auto &f : menu_) {
      if (auto e = f.entity.lock()) e->SetActive(visible);
    }
  }

  void UpdateIntro(float deltaTime) {
    // 浮上で入ってきたときは、カメラが上がりきるまで待つ（時間も進めない）
    if (introWaitRise_) {
      if (rise_.IsMoving()) return;
      introWaitRise_ = false;
      SetFloatersVisible(true);
      // 浮上そのものが「間」になっているので、最初の待ち（フェード明け待ち）は短くする
      introTime_ = intro.delay * 0.6f;
    }
    if (deltaTime > 0.0f) introTime_ += deltaTime;
    const float t = introTime_;

    for (size_t i = 0; i < letters_.size(); ++i) {
      Floater &f = letters_[i];
      const float p = (t - IntroLetterStart(static_cast<int>(i))) / (std::max)(intro.riseTime, 0.01f);
      const float v = (p <= 0.0f) ? 0.0f : EaseOutBack(p, intro.overshoot);
      f.introY = -intro.depth * (1.0f - v);
      // 文字の上面が水面を抜けた瞬間に飛沫と波紋
      if (!f.breached && f.introY > -(f.depth + 0.2f)) {
        f.breached = true;
        OnBreach(f, 1.0f);
      }
    }

    const float menuStart = IntroMenuStart();
    for (size_t i = 0; i < menu_.size(); ++i) {
      Floater &f = menu_[i];
      const float p = (t - menuStart - intro.menuInterval * static_cast<float>(i)) /
                      (std::max)(intro.menuRiseTime, 0.01f);
      const float v = (p <= 0.0f) ? 0.0f : EaseOutBack(p, 0.0f);
      f.introY = -intro.menuDepth * (1.0f - v);
      f.introScale = 0.6f + 0.4f * v;
      if (!f.breached && f.introY > -intro.menuDepth * 0.3f) {
        f.breached = true;
        OnBreach(f, 0.4f); // メニューは控えめに（波紋だけ弱く、飛沫は少なめ）
      }
    }

    if (t >= IntroEnd()) FinishIntro();
  }

  /// @brief 水面を抜けた瞬間の飛沫と波紋
  /// @param power 1 でタイトル文字の強さ。メニューは弱めにする
  void OnBreach(const Floater &f, float power) {
    auto e = f.entity.lock();
    if (!e) return;
    auto *tr = e->GetComponent<TransformComponent>();
    if (!tr) return;
    const float x = tr->position.x;
    const float z = tr->position.z + (f.zMin + f.zMax) * 0.5f * f.scale * f.introScale;
    if (intro.rippleStrength != 0.0f) {
      RC::AddWaveSourceAtWorld(x, z, intro.rippleRadius, intro.rippleStrength * power);
    }
    const float s = f.scale * f.introScale;
    if (intro.splash) {
      EmitSplash(x, 0.15f, z, static_cast<int>(static_cast<float>(intro.splashCount) * power), f.halfW * 2.0f * s,
                 (f.zMax - f.zMin) * s);
    }
  }

  void FinishIntro() {
    introActive_ = false;
    if (introWaitRise_) {
      introWaitRise_ = false;
      SetFloatersVisible(true);
    }
    for (auto &f : letters_) {
      f.introY = 0.0f;
      f.introScale = 1.0f;
      f.breached = true;
    }
    for (auto &f : menu_) {
      f.introY = 0.0f;
      f.introScale = 1.0f;
      f.breached = true;
    }
  }

  /// @brief キーで演出を飛ばす。まだ水面を抜けていないものにも波紋だけ出して「浮かんだ」感を残す
  void SkipIntro() {
    for (auto &f : letters_) {
      if (!f.breached && intro.rippleStrength != 0.0f) {
        if (auto e = f.entity.lock()) {
          if (auto *tr = e->GetComponent<TransformComponent>()) {
            RC::AddWaveSourceAtWorld(tr->position.x, tr->position.z, intro.rippleRadius, intro.rippleStrength * 0.5f);
          }
        }
      }
    }
    FinishIntro();
  }

  /// @brief メニュー操作に使うキー・ボタンのどれかが押されたか（演出のスキップ判定）
  bool AnyMenuKeyTriggered() const {
    SceneContext *ctx = GetSceneContext();
    if (!ctx || !ctx->input) return false;
    Input *in = ctx->input;
    if (in->IsKeyTrigger(DIK_SPACE) || in->IsKeyTrigger(DIK_RETURN) || in->IsKeyTrigger(DIK_UP) ||
        in->IsKeyTrigger(DIK_DOWN) || in->IsKeyTrigger(DIK_W) || in->IsKeyTrigger(DIK_S)) {
      return true;
    }
    if (in->IsXInputConnected()) {
      if (in->IsXInputButtonTrigger(XINPUT_GAMEPAD_A) || in->IsXInputButtonTrigger(XINPUT_GAMEPAD_START) ||
          in->IsXInputButtonTrigger(XINPUT_GAMEPAD_DPAD_UP) || in->IsXInputButtonTrigger(XINPUT_GAMEPAD_DPAD_DOWN)) {
        return true;
      }
    }
    return false;
  }

  // 水面を抜けた瞬間の泡は 2 種類の GPU パーティクルで作る（どちらも射出数 0 で寝かせておき、抜けた瞬間だけ射出）。
  // 見た目は炭酸の泡：gpu_particle_bubble パイプライン（BubbleParticle.PS。縁とハイライトだけ白く、
  // 中は透ける輪）で描く。テクスチャは使わない
  //   spray : 小さな泡。文字の足元から外へ放射状にゆっくり広がり、少しずつ浮いて消える（シュワッと弾ける）
  //   foam  : 少し大きめの泡。文字の足元に散らばって、その場でゆらゆらしながら長めに残る
  // 以前は白い丸を大きく撒いていたため、綿や煙のように見えていた。

  /// @brief 泡のエミッタを 1 つ作る（共通部分）
  std::shared_ptr<Entity> CreateSplashEmitter(Scene *scene, const std::string &name) {
    auto e = scene->CreateEntity(name);
    if (!e) return nullptr;
    e->SetTag("transient", 1);
    auto &tr = e->AddComponent<TransformComponent>();
    tr.position = {0.0f, -200.0f, 0.0f}; // 使うまでは画面外
    auto &gpu = e->AddComponent<GPUParticleComponent>();
    if (auto *ps = gpu.particleSystem.get()) {
      ps->SetParticleType(ParticleType::Default);   // EmitParticle.CS：形状・速度・寿命・色を全部使う
      ps->SetPipelinePrefix("gpu_particle_bubble");  // 中が透ける泡の輪（飛び込み・浮上の泡と同じ描き方）
      ps->SetBlendMode(kBlendModeNormal);            // BubbleParticle.PS は乗算済みでない色＋α を返す
      ps->SetMaxParticles(1024);
      ps->SetEmitCount(0);
    }
    return e;
  }

  void SpawnSplashEmitter() {
    Scene *scene = GetScene();
    if (!scene || !splash_.expired()) return;

    // ---- spray：外へ広がる小さな泡 ----
    if (auto e = CreateSplashEmitter(scene, "TitleSplashSpray")) {
      if (auto *ps = e->GetComponent<GPUParticleComponent>()->particleSystem.get()) {
        // Cone は「真上から coneAngle まで」の範囲にばらけた向きへ、|baseVelocity| の速さで飛ぶ。
        // 角度を大きく（≒ 72°）取って、ほとんどを横向き＝外へ放射状に広げる。
        // 出る位置は半径 tan(coneAngle) × shapeRadius の円内（射出のたびに文字の大きさへ合わせる）
        ps->emitterShape_ = EmitterShape::Cone;
        ps->coneAngle_ = 1.25f;
        ps->shapeRadius_ = 0.5f;
        ps->baseVelocity_ = {0.0f, 0.04f, 0.0f}; // 1 フレームあたり 0.04 ≒ 2.4 m/s（ゆっくり広がる）
        ps->velocityVariance_ = 0.02f;
        // 負の重力で少しずつ浮く（velocity.y -= gravity * dt。1 フレームあたり）
        ps->gravity_ = -0.02f;
        ps->minLifeTime_ = 0.5f;
        ps->maxLifeTime_ = 1.1f;
        ps->minScale_ = 0.06f;
        ps->maxScale_ = 0.18f;
        // ほんのり水色の泡。消えるときは α だけ落とす（通常ブレンドなので RGB は残してよい）
        ps->startColor_ = {0.85f, 0.97f, 1.0f, 0.9f};
        ps->endColor_ = {0.85f, 0.97f, 1.0f, 0.0f};
      }
      scene->InitDynamicEntityRuntime(*e);
      splash_ = e;
    }

    // ---- foam：足元に散らばって残る、少し大きめの泡 ----
    if (auto e = CreateSplashEmitter(scene, "TitleSplashFoam")) {
      if (auto *ps = e->GetComponent<GPUParticleComponent>()->particleSystem.get()) {
        ps->emitterShape_ = EmitterShape::Box;
        ps->shapeBoxSize_ = {2.0f, 0.05f, 2.0f};
        ps->baseVelocity_ = {0.0f, 0.0f, 0.0f};
        ps->velocityVariance_ = 0.012f; // その場でゆらゆら
        ps->gravity_ = 0.0f;
        ps->minLifeTime_ = 0.8f;
        ps->maxLifeTime_ = 1.8f;
        ps->minScale_ = 0.12f;
        ps->maxScale_ = 0.32f;
        ps->startColor_ = {0.85f, 0.97f, 1.0f, 0.8f};
        ps->endColor_ = {0.85f, 0.97f, 1.0f, 0.0f};
      }
      scene->InitDynamicEntityRuntime(*e);
      foam_ = e;
    }
  }


  static void SetEmission(const std::weak_ptr<Entity> &w, uint32_t count) {
    auto e = w.lock();
    if (!e) return;
    if (auto *gpu = e->GetComponent<GPUParticleComponent>()) {
      if (gpu->particleSystem) gpu->particleSystem->SetEmitCount(count);
    }
  }

  /// @brief 両方のエミッタの射出数をまとめて設定する（0 で止める）
  void SetSplashEmission(uint32_t sprayCount, uint32_t foamCount) {
    SetEmission(splash_, sprayCount);
    SetEmission(foam_, foamCount);
  }
  void SetSplashEmission(uint32_t count) { SetSplashEmission(count, count); }

  /// @brief エミッタを文字の位置へ動かし、文字の大きさに合わせて射出範囲を変える
  static void PlaceEmitter(const std::weak_ptr<Entity> &w, float x, float y, float z, float width, float length,
                           bool cone) {
    auto e = w.lock();
    if (!e) return;
    if (auto *tr = e->GetComponent<TransformComponent>()) tr->position = {x, y, z};
    auto *gpu = e->GetComponent<GPUParticleComponent>();
    auto *ps = gpu ? gpu->particleSystem.get() : nullptr;
    if (!ps) return;
    // 位置はシーン側が毎フレーム Transform から写すが、同じフレームに射出されるよう直接も書く
    ps->emitterPosition_ = {x, y, z};
    if (cone) {
      // 出る位置の半径 = tan(coneAngle) × shapeRadius。文字の半分くらいの円から出す
      const float r = 0.35f * (std::max)((std::min)(width, length), 0.5f);
      ps->shapeRadius_ = r / (std::max)(std::tan(ps->coneAngle_), 0.1f);
    } else {
      ps->shapeBoxSize_ = {(std::max)(width, 0.5f), 0.05f, (std::max)(length, 0.5f)};
    }
  }

  /// @param count  水滴（spray）の数。泡（foam）はその半分
  /// @param width / length 文字の幅と高さ（真上から見た XZ の大きさ）
  void EmitSplash(float x, float y, float z, int count, float width = 2.0f, float length = 2.0f) {
    if (count <= 0 || splash_.expired()) return;
    PlaceEmitter(splash_, x, y, z, width, length, /*cone=*/true);
    PlaceEmitter(foam_, x, 0.05f, z, width * 1.1f, length * 1.1f, /*cone=*/false);
    SetSplashEmission(static_cast<uint32_t>(count), static_cast<uint32_t>((std::max)(count / 2, 1)));
    // 2 フレーム続ける。スクリプトは DataDrivenScene の GPUParticle 更新（定数バッファへ emitCount を
    // 書く）より後に走るので、1 フレームで止めると CB 側の emitCount が 0 のまま Dispatch され、
    // 1 粒も出ない。次のフレームの更新で CB に載ってから止める
    splashFramesLeft_ = 2;
  }

  // ------------------------------------------------------------------
  // タイトル用ポストプロセス
  // ------------------------------------------------------------------

  /// @brief Bloom / ColorGrade / Vignette を積んでパラメータを書く
  /// @details 毎フレーム呼んでよい（積み済みのものは AddEffect 側が無視する）。
  ///          積む順 = 適用順：Bloom → ColorGrade → Vignette（周辺減光は最後に掛けたい）。
  void ApplyTitlePost() {
    if (!post.enabled) return;
    PostProcess *pp = RC::GetRenderContext().GetPostProcess();
    if (!pp) return;
    if (post.bloom) {
      if (!pp->HasEffect(PostEffectType::Bloom)) {
        pp->AddEffect(PostEffectType::Bloom);
        pp->SetBloomThreshold(post.bloomThreshold);
        pp->SetBloomIntensity(post.bloomIntensity);
        pp->SetBloomRadius(post.bloomRadius);
        pp->SetBloomKnee(post.bloomKnee);
      }
    }
    if (post.colorGrade) {
      if (!pp->HasEffect(PostEffectType::ColorGrade)) {
        pp->AddEffect(PostEffectType::ColorGrade);
        pp->SetGradeExposure(post.gradeExposure);
        pp->SetGradeContrast(post.gradeContrast);
        pp->SetGradeSaturation(post.gradeSaturation);
        pp->SetGradeTemperature(post.gradeTemperature);
        pp->SetGradeTint(post.gradeTint);
        pp->SetGradeLerpFactor(post.gradeAmount);
      }
    }
    if (post.vignette && !pp->HasEffect(PostEffectType::Vignette)) {
      pp->AddEffect(PostEffectType::Vignette);
    }
  }

  /// @brief ApplyTitlePost で積んだものを外す
  void RemoveTitlePost() {
    PostProcess *pp = RC::GetRenderContext().GetPostProcess();
    if (!pp) return;
    pp->RemoveEffect(PostEffectType::Bloom);
    pp->RemoveEffect(PostEffectType::ColorGrade);
    pp->RemoveEffect(PostEffectType::Vignette);
  }

  /// @brief TextMesh 本体の色を書き換える
  /// @details DataDrivenScene は本体色を再生成時にしか Material へ反映しないので、
  ///          tm.color と Material の両方へ書く（再生成が起きても同じ色に戻るように）。
  static void ApplyMainColor(Floater &f, const RC::Vector4 &color) {
    auto e = f.entity.lock();
    if (!e) return;
    auto *tm = e->GetComponent<TextMeshComponent>();
    if (!tm) return;
    tm->color = color;
    if (tm->HasMesh()) {
      if (auto *mat = RC::GetPrimitiveMeshMaterialPtr(tm->meshHandle)) mat->color = color;
    }
  }

  void UpdateFloater(Floater &f, const RC::WaterWaveParams &params, bool hasWater,
                     float waterTime, float deltaTime) {
    auto e = f.entity.lock();
    if (!e) return;
    auto *tr = e->GetComponent<TransformComponent>();
    if (!tr) return;

    // 拡大率は目標へなめらかに寄せる（選択の切り替えでポンと弾む感じ）
    if (deltaTime > 0.0f) {
      const float k = (std::min)(1.0f, deltaTime * 12.0f);
      f.scale += (f.targetScale - f.scale) * k;
    } else {
      f.scale = f.targetScale;
    }

    // ゆっくり円を描いて漂う
    RC::Vector3 pos = f.basePos;
    if (f.drift > 0.0f && driftPeriod > 0.01f) {
      const float omega = kTwoPi / driftPeriod;
      pos.x += std::cos(time_ * omega + f.phase) * f.drift;
      pos.z += std::sin(time_ * omega * 0.8f + f.phase) * f.drift;
    }

    // 文字は 1 枚の板なのに水面は文字の幅の中でも上下する。
    // 中心 1 点だけで高さを決めると、波の山が文字の端にかかったときに
    // その部分が水に沈んで見える。文字の足元（幅方向に数点 × 上下端）をサンプルして
    // いちばん高い水面を基準にすれば、どの部分も水面より下には来ない。
    RC::WaterSample sample;
    sample.height = hasWater ? params.baseHeight : 0.0f;
    float topWater = sample.height;
    if (hasWater) {
      const float zc = (f.zMin + f.zMax) * 0.5f * f.scale; // 文字の見た目の中心（ベースラインからのずれ）
      sample = RC::WaterSurface::Sample(params, waterTime, pos.x, pos.z + zc);

      // 波紋（マウス・航跡）：Water.VS.hlsl と同じく Gerstner の高さに足し、法線も同じ式で合成する。
      // 読み戻しが無効なら高さ 0・法線 (0,1,0) が返るので、そのまま足しても変わらない。
      const bool useRipple = rippleFloat && RC::IsInteractiveWaterReadbackEnabled();
      if (useRipple) {
        float rh = 0.0f;
        RC::Vector3 rn{0.0f, 1.0f, 0.0f};
        RC::SampleInteractiveWater(pos.x, pos.z + zc, rh, rn);
        sample.height += rh;
        const RC::Vector3 g = sample.normal;
        RC::Vector3 n{g.x + rn.x * rippleTiltScale, g.y * rn.y, g.z + rn.z * rippleTiltScale};
        const float len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
        if (len > 1e-6f) sample.normal = {n.x / len, n.y / len, n.z / len};
      }
      topWater = sample.height;

      const float hw = f.halfW * f.scale;
      const float cy = std::cos(f.yaw), sy = std::sin(f.yaw);
      // 幅が広い（メニューの 5 文字など）ほど横方向のサンプル数を増やす
      const int nx = std::clamp(static_cast<int>(hw / 1.2f) + 2, 2, 7);
      const float zs[2] = {f.zMin * f.scale, f.zMax * f.scale};
      for (int ix = 0; ix < nx; ++ix) {
        const float lx = -hw + 2.0f * hw * static_cast<float>(ix) / static_cast<float>(nx - 1);
        for (float lz : zs) {
          // 文字の向き（yaw）に合わせて回す
          const float wx = pos.x + lx * cy + lz * sy;
          const float wz = pos.z - lx * sy + lz * cy;
          float h = RC::WaterSurface::SampleHeight(params, waterTime, wx, wz);
          if (useRipple) h += RC::SampleInteractiveWaterHeight(wx, wz);
          topWater = (std::max)(topWater, h);
        }
      }
    }

    // ぷかぷか：水面追従に加えて、位相をずらした小さな上下動を足す
    float bob = 0.0f;
    if (f.bob > 0.0f && bobPeriod > 0.01f) {
      const float w = kTwoPi / bobPeriod;
      bob = (std::sin(time_ * w + f.phase) * 0.5f + 0.5f) * f.bob;
    }

    // 底面が「足元でいちばん高い水面」より floatOffset (+lift) だけ上に来るよう置く。
    // メッシュは厚さの中心が原点なので、底面 = position.y - depth*scale/2。
    tr->position = {pos.x, topWater + f.depth * f.scale * 0.5f + floatOffset + f.lift + bob + f.introY, pos.z};

    // 文字の上面法線を水面法線へ合わせる。
    // 回転は v·Rx·Ry·Rz の順に掛かる（MakeAffineMatrix）。Rx(π/2 + a) で上面が
    // (0, cos a, sin a) を向き、Rz(b) で (-cos a·sin b, cos a·cos b, sin a) になる。
    // これを水面法線 n に一致させると a = asin(n.z), b = asin(-n.x / cos a)。
    // 間に挟む Ry(yaw) は小角度なので誤差は無視できる。
    float a = 0.0f, b = 0.0f;
    if (f.tilt > 0.0f) {
      const RC::Vector3 n = sample.normal;
      a = std::asin(std::clamp(n.z, -1.0f, 1.0f));
      const float ca = (std::max)(std::cos(a), 1e-3f);
      b = std::asin(std::clamp(-n.x / ca, -1.0f, 1.0f));
      a *= f.tilt;
      b *= f.tilt;
    }
    tr->rotation = {kHalfPi + a, f.yaw, b};
    const float s = f.scale * f.introScale;
    tr->scale = {s, s, s};

    // 描画側の Transform も同期しておく（通常は次の Update 冒頭で同期される）
    if (auto *tm = e->GetComponent<TextMeshComponent>()) {
      if (tm->HasMesh()) {
        if (auto *p = RC::GetPrimitiveMeshTransformPtr(tm->meshHandle)) *p = tr->ToTransform();
      }
      if (tm->HasOutlineMesh()) {
        if (auto *p = RC::GetPrimitiveMeshTransformPtr(tm->outlineMeshHandle)) *p = tr->ToTransform();
      }
    }
  }

  // ------------------------------------------------------------------
  // 入力
  // ------------------------------------------------------------------

  void HandleInput() {
    SceneContext *ctx = GetSceneContext();
    if (!ctx || !ctx->input) return;
    Input *in = ctx->input;

    // 上下（キーボード / 十字キー / 左スティックのエッジ）
    int move = 0;
    if (in->IsKeyTrigger(DIK_UP) || in->IsKeyTrigger(DIK_W)) move -= 1;
    if (in->IsKeyTrigger(DIK_DOWN) || in->IsKeyTrigger(DIK_S)) move += 1;
    if (in->IsXInputConnected()) {
      if (in->IsXInputButtonTrigger(XINPUT_GAMEPAD_DPAD_UP)) move -= 1;
      if (in->IsXInputButtonTrigger(XINPUT_GAMEPAD_DPAD_DOWN)) move += 1;
      const SHORT ly = in->GetXInputThumbLY();
      int stickDir = 0;
      if (ly > kStickThreshold) stickDir = -1;       // 上
      else if (ly < -kStickThreshold) stickDir = 1;  // 下
      if (stickDir != 0 && stickDir != prevStickDir_) move += stickDir;
      prevStickDir_ = stickDir;
    }
    if (move != 0) {
      selected_ = (selected_ + move + kMenuCount) % kMenuCount;
      pulseTimer_ = 0.0f;
      // 切り替えた瞬間だけ少し大きくして「跳ねる」感じを出す
      if (selected_ >= 0 && selected_ < static_cast<int>(menu_.size())) {
        menu_[selected_].scale = menuSelectedScale * 1.12f;
        OnSelectionChanged();
      }
    }

    // 決定
    bool confirm = in->IsKeyTrigger(DIK_SPACE) || in->IsKeyTrigger(DIK_RETURN);
    if (in->IsXInputConnected() && in->IsXInputButtonTrigger(XINPUT_GAMEPAD_A)) confirm = true;
    if (!confirm) return;

    if (selected_ == kMenuStart) {
      SceneContext *sc = GetSceneContext();
      if (sc && !sc->isPlaying()) return; // 編集モードでは飛ばない（RequestSceneChange と同じ）

      // 行き先と演出は遷移表で引く。演出が dive のときだけ飛び込み演出を挟む
      std::string transition;
      if (!LookupTransition(kTriggerStart, startTarget_, transition) || startTarget_.empty()) {
        Log::Print("[TitleScreenScript] scene flow has no target for 'start'");
        return;
      }
      if (dive.enabled && transition == SceneTransitions::kDive && BeginDive()) {
        // 遷移要求は沈みきったあと UpdateDive が出す
        decided_ = true;
        Log::Print("[TitleScreenScript] start -> dive -> " + startTarget_);
      } else if (RequestSceneChange(startTarget_)) {
        decided_ = true;
        Log::Print("[TitleScreenScript] start -> " + startTarget_);
      } else {
        Log::Print("[TitleScreenScript] scene change refused: " + startTarget_);
      }
    } else {
      if (quitEnabled) {
        Log::Print("[TitleScreenScript] quit requested");
        decided_ = true;
        // App::Run のメッセージループは WM_QUIT で抜けて Term() へ進む
        PostQuitMessage(0);
      } else {
        Log::Print("[TitleScreenScript] quit is disabled (quitEnabled = false)");
      }
    }
  }

  // ------------------------------------------------------------------
  // 飛び込み（スタート → 水面 → 深海 → Game）
  // ------------------------------------------------------------------

  static constexpr float kDoneRetrySeconds = 1.0f; ///< 遷移要求が通らないとき諦めるまでの秒数

  /// @brief 飛び込みを始める
  /// @return カメラが見つからない等で始められなければ false（呼び出し側は即遷移へ倒す）
  bool BeginDive() {
    if (dive_.IsActive()) return true;
    Scene *scene = GetScene();
    auto cam = WaterCameraFx::FindMainCamera(scene);
    if (!cam) {
      Log::Print("[TitleScreenScript] dive: main camera not found");
      return false;
    }
    diveRequested_ = false;
    return dive_.Begin(cam, WaterCameraFx::FindWaterY(scene), dive, look, &bubbles_);
  }

  /// @brief 飛び込みを止めて後始末する
  /// @param restoreCamera カメラの位置・向き・画角を開始時へ戻すか（テスト／中断用）
  void EndDive(bool restoreCamera) {
    dive_.End(restoreCamera);
    diveTestOnly_ = false;
    diveRequested_ = false;
    decided_ = false;
  }

  void UpdateDive(float dt) {
    if (!dive_.Update(dt)) {
      // カメラが消えた（エディタ操作など）。演出を諦めて素直に遷移する
      const bool testOnly = diveTestOnly_;
      EndDive(false);
      if (!testOnly && !startTarget_.empty() && RequestSceneChange(startTarget_)) decided_ = true;
      return;
    }
    if (!dive_.IsDone() || diveRequested_) return;

    if (diveTestOnly_) {
      Log::Print("[TitleScreenScript] dive test finished (no scene change)");
      diveRequested_ = true;
      return;
    }
    // 画面が深海色になりきってから遷移を要求する。断られたら（遷移中など）しばらく出し直し、
    // それでも通らない（シーン名が未登録など）なら演出を戻してメニューへ返す。
    diveRequested_ = RequestSceneChange(startTarget_, SceneTransitions::kDive);
    if (!diveRequested_ && dive_.DoneTime() > kDoneRetrySeconds) {
      Log::Print("[TitleScreenScript] scene change kept failing, giving up: " + startTarget_);
      EndDive(/*restoreCamera=*/true);
    }
  }

  /// @brief 深海から浮上して始める（Dive 遷移で入ってきたとき）
  void BeginRise() {
    Scene *scene = GetScene();
    auto cam = WaterCameraFx::FindMainCamera(scene);
    if (!cam) return;
    rise_.Begin(cam, WaterCameraFx::FindWaterY(scene), rise, look, &bubbles_);
  }

  // ------------------------------------------------------------------
  // 操作ヒント（2D）
  // ------------------------------------------------------------------

  void DrawHint(float screenW, float screenH) {
    const char *text = "↑↓ 選択　　SPACE 決定";
    const float lineH = RC::GetFontLineHeight(hintFont_);
    const RC::Vector2 pos{screenW * 0.5f, screenH - lineH - 28.0f};
    // 水面の模様の上でも読めるよう、横いっぱいの半透明の帯を先に敷く
    if (hintBandColor.w > 0.0f) {
      RC::DrawBox({0.0f, pos.y - hintBandPadding}, {screenW, pos.y + lineH + hintBandPadding}, hintBandColor);
    }
    // 水面の上でも読めるように、影を 1px ずらして先に描く
    RC::DrawString(hintFont_, text, {pos.x + 1.0f, pos.y + 1.0f}, {0.0f, 0.03f, 0.08f, 0.6f},
                   1.0f, TextAlign::Center);
    RC::DrawString(hintFont_, text, pos, {0.94f, 0.97f, 1.0f, 0.95f}, 1.0f, TextAlign::Center);
  }

  static constexpr float kHalfPi = 1.57079632679f;
  static constexpr float kTwoPi = 6.28318530718f;
  static constexpr float kPulsePeriod = 1.4f; ///< 選択中メニューの明滅周期（秒）
  static constexpr SHORT kStickThreshold = 16000;

  std::vector<Floater> letters_;
  std::vector<Floater> menu_;

  // 選択中メニューの目印
  std::vector<std::weak_ptr<Entity>> rings_;
  bool ringInit_ = false;
  float ringCx_ = 0.0f, ringCz_ = 0.0f, ringHalfW_ = 1.0f; ///< 追従中のリングの基準（選択項目の中心・半幅）
  float ringAppear_ = 0.0f;      ///< 0〜1。出し入れのアニメーション
  float ringPop_ = 1.0f;         ///< 選択切り替えで弾ませる倍率（1 へ戻る）
  float ringSpin_ = 0.0f;        ///< 回転角（rad）
  float ringRippleTimer_ = 0.0f;

  // 起動演出
  bool introActive_ = false;
  bool introWaitRise_ = false;   ///< 浮上が終わるまで演出を待っている（そのあいだ文字とメニューは非表示）
  float introTime_ = 0.0f;
  std::weak_ptr<Entity> splash_; ///< 飛沫（外へ弾ける水滴）のエミッタ
  std::weak_ptr<Entity> foam_;   ///< 足元に広がる白い泡のエミッタ
  int splashFramesLeft_ = 0;     ///< 射出を止めるまでのフレーム数
  int hintFont_ = -1;
  int selected_ = kMenuStart;
  int prevStickDir_ = 0;
  bool decided_ = false;
  bool readbackOwned_ = false; ///< 波紋の読み戻しを自分が有効にしたか（OnDestroy で戻す）
  std::string startTarget_; ///< 「スタート」の行き先（決定時に遷移表から引く）
  static constexpr const char *kTriggerStart = "start"; ///< 遷移表のきっかけ名
  float time_ = 0.0f;
  float pulseTimer_ = 0.0f;

  WaterCameraFx::DiveSequence dive_; ///< スタート時の飛び込み
  WaterCameraFx::RiseSequence rise_; ///< 入場時の浮上
  WaterCameraFx::Bubbles bubbles_;   ///< 泡のエミッタ（飛び込み・浮上で共用）
  bool diveTestOnly_ = false;        ///< ImGui の Test Dive（遷移しない）
  bool diveRequested_ = false;       ///< 飛び込み後の遷移要求が受理されたか
};

REGISTER_SCRIPT(TitleScreenScript)
