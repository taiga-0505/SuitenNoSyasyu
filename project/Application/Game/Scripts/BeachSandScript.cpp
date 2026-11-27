#include "ECS/ScriptableEntity.h"
#include "ECS/ScriptRegistry.h"
#include "ECS/TransformComponent.h"
#include "Common/Math/MathUtils.h"
#include "Common/Log/Log.h"
#include "Input/Input.h"
#include "Render/Systems/RenderInteractiveSand.h"
#include "RenderCommon.h"
#include "Engine/Render/RenderContext.h"
#include "Framework/App.h"
#include "Game/Framework/WaterCameraFx.h"
#include "Scene.h"

#if RC_ENABLE_IMGUI
#include "imgui/imgui.h"
#endif

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

// ============================================================================
// 砂浜：触れる砂の地形（ステージセレクトの浜辺・浜辺シーン）
//
//   - 地形：波打ち際（x = shoreX 付近。z に沿って緩く波打つ）から陸側はなだらかに上がり、
//     奥で小さな砂丘になる。海側は浅瀬の坂を下って沖で急に深くなる（セレクトの海の画面には出さない）。
//     地形のメッシュは RC::CreateSandTerrain、砂の質感（粒・風紋・濡れ・きらめき）は Sand.PS.hlsl。
//   - 遊び：水面の波紋（TitleMouseRippleScript）と同じく、マウスに反応する。
//       カーソルを動かす        … 指でなぞったような細い筋が付く
//       左ボタンを押したまま動かす … 太く深い溝になり、掘った砂が両脇に寄る
//       クリック                … 足跡がぺたりと付く（左右交互・進んでいる向きに）
//     跡は RC::AddSandDig で RenderInteractiveSand のハイトマップに入り、少しずつ崩れて消える。
//   - 波：数秒ごとに波が砂浜を駆け上がり、届いた所の跡を均して濡らす（RC::AddSandSmooth）。
//   - Select.json と Shore.json に同じ設定で置く（同じ地形・同じ跡がつながって見える）。
// ============================================================================

/// @class BeachSandScript
/// @brief 砂浜の地形を作って描き、マウスで跡を付け、波で均す
/// @details JSON (scriptDataList) で設定できる項目:
///   "enabled"      : false で何もしない
///   "areaMinX" / "areaMaxX" / "areaMinZ" / "areaMaxZ" : 地形を作る範囲（m）
///   "cellSize"     : 地形の頂点の間隔（m）
///   "shoreX"       : z = 0 での波打ち際の X（m。これより +X が海）
///   "shoreWobble"  : 波打ち際の出入りの大きさ（m）
///   "bermHeight" / "bermWidth" : 波打ち際のすぐ上の段の高さ・幅（m）
///   "beachSlope"   : 段より先の上り勾配
///   "seaSlope"     : 浅瀬の下り勾配
///   "dropStart" / "dropSlope" : 波打ち際からこの距離（m）で沖へ急に深くなる・その勾配
///   "floorY"       : いちばん深い所の高さ（m）
///   "duneHeight"   : 陸の奥の砂丘の高さ（m）
///   "dryColor" / "wetColor" / "grainColorA" / "grainColorB" : 砂の色
///   "wetBand" / "grainScale" / "grainStrength" / "speckDensity" / "rippleWavelength" /
///   "rippleStrength" / "rippleDirDeg" / "markStrength" / "freshDarken" / "sparkle" / "wetShine" : 質感（RC::SandLook）
///   "trail"        : カーソルを動かしたときの筋を付けるか
///   "trailRadius" / "trailDepth"   : 筋の半径・深さ（m）
///   "dragRadius" / "dragDepth" / "dragRim" : ボタンを押したまま動かしたときの溝
///   "footprints" / "footSize"      : クリックで足跡を付けるか・足の長さ（m）
///   "swashPeriod" / "swashReach"   : 波が駆け上がる間隔（秒）・届く距離（m。毎回ばらつく）
class BeachSandScript : public ScriptableEntity {
public:
  /// @brief 設定項目を JSON に書き出す
  /// @return 設定項目を格納した JSON
  nlohmann::json Serialize() override {
    auto col = [](const RC::Vector4 &c) { return nlohmann::json::array({c.x, c.y, c.z, c.w}); };
    return {
        {"enabled", enabled_},
        {"areaMinX", areaMinX_},
        {"areaMaxX", areaMaxX_},
        {"areaMinZ", areaMinZ_},
        {"areaMaxZ", areaMaxZ_},
        {"cellSize", cellSize_},
        {"shoreX", shoreX_},
        {"shoreWobble", shoreWobble_},
        {"bermHeight", bermHeight_},
        {"bermWidth", bermWidth_},
        {"beachSlope", beachSlope_},
        {"seaSlope", seaSlope_},
        {"dropStart", dropStart_},
        {"dropSlope", dropSlope_},
        {"floorY", floorY_},
        {"duneHeight", duneHeight_},
        {"dryColor", col(look_.dryColor)},
        {"wetColor", col(look_.wetColor)},
        {"grainColorA", col(look_.grainColorA)},
        {"grainColorB", col(look_.grainColorB)},
        {"wetBand", look_.wetBand},
        {"grainScale", look_.grainScale},
        {"grainStrength", look_.grainStrength},
        {"speckDensity", look_.speckDensity},
        {"rippleWavelength", look_.rippleWavelength},
        {"rippleStrength", look_.rippleStrength},
        {"rippleDirDeg", look_.rippleDirDeg},
        {"markStrength", look_.markStrength},
        {"freshDarken", look_.freshDarken},
        {"sparkle", look_.sparkle},
        {"wetShine", look_.wetShine},
        {"trail", trail_},
        {"trailRadius", trailRadius_},
        {"trailDepth", trailDepth_},
        {"dragRadius", dragRadius_},
        {"dragDepth", dragDepth_},
        {"dragRim", dragRim_},
        {"footprints", footprints_},
        {"footSize", footSize_},
        {"swashPeriod", swashPeriod_},
        {"swashReach", swashReach_},
    };
  }

  /// @brief JSON から設定項目を読み込む
  /// @param j 読み込み元の JSON（存在し型が合うキーだけ反映する）
  void Deserialize(const nlohmann::json &j) override {
    auto readF = [&](const char *key, float &dst) {
      if (j.contains(key) && j[key].is_number()) dst = j[key].get<float>();
    };
    auto readB = [&](const char *key, bool &dst) {
      if (j.contains(key) && j[key].is_boolean()) dst = j[key].get<bool>();
    };
    auto readC = [&](const char *key, RC::Vector4 &dst) {
      if (!j.contains(key) || !j[key].is_array() || j[key].size() < 4) return;
      const auto &c = j[key];
      dst = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>(), c[3].get<float>()};
    };
    readB("enabled", enabled_);
    readF("areaMinX", areaMinX_);
    readF("areaMaxX", areaMaxX_);
    readF("areaMinZ", areaMinZ_);
    readF("areaMaxZ", areaMaxZ_);
    readF("cellSize", cellSize_);
    readF("shoreX", shoreX_);
    readF("shoreWobble", shoreWobble_);
    readF("bermHeight", bermHeight_);
    readF("bermWidth", bermWidth_);
    readF("beachSlope", beachSlope_);
    readF("seaSlope", seaSlope_);
    readF("dropStart", dropStart_);
    readF("dropSlope", dropSlope_);
    readF("floorY", floorY_);
    readF("duneHeight", duneHeight_);
    readC("dryColor", look_.dryColor);
    readC("wetColor", look_.wetColor);
    readC("grainColorA", look_.grainColorA);
    readC("grainColorB", look_.grainColorB);
    readF("wetBand", look_.wetBand);
    readF("grainScale", look_.grainScale);
    readF("grainStrength", look_.grainStrength);
    readF("speckDensity", look_.speckDensity);
    readF("rippleWavelength", look_.rippleWavelength);
    readF("rippleStrength", look_.rippleStrength);
    readF("rippleDirDeg", look_.rippleDirDeg);
    readF("markStrength", look_.markStrength);
    readF("freshDarken", look_.freshDarken);
    readF("sparkle", look_.sparkle);
    readF("wetShine", look_.wetShine);
    readB("trail", trail_);
    readF("trailRadius", trailRadius_);
    readF("trailDepth", trailDepth_);
    readF("dragRadius", dragRadius_);
    readF("dragDepth", dragDepth_);
    readF("dragRim", dragRim_);
    readB("footprints", footprints_);
    readF("footSize", footSize_);
    readF("swashPeriod", swashPeriod_);
    readF("swashReach", swashReach_);
  }

#if RC_ENABLE_IMGUI
  /// @brief インスペクタに砂の質感の調整と跡の消去ボタンを表示する
  void OnImGui() override {
    ImGui::Text("mesh=%d  cursor=%s (%.1f, %.1f)", mesh_, prevValid_ ? "on sand" : "off", prevX_, prevZ_);
    ImGui::ColorEdit3("Dry Color", &look_.dryColor.x);
    ImGui::ColorEdit3("Wet Color", &look_.wetColor.x);
    ImGui::SliderFloat("Grain Scale", &look_.grainScale, 4.0f, 80.0f);
    ImGui::SliderFloat("Grain Strength", &look_.grainStrength, 0.0f, 0.4f);
    ImGui::SliderFloat("Speck Density", &look_.speckDensity, 0.0f, 0.2f);
    ImGui::SliderFloat("Ripple Wavelength", &look_.rippleWavelength, 0.2f, 4.0f);
    ImGui::SliderFloat("Ripple Strength", &look_.rippleStrength, 0.0f, 1.5f);
    ImGui::SliderFloat("Ripple Dir", &look_.rippleDirDeg, -180.0f, 180.0f);
    ImGui::SliderFloat("Mark Strength", &look_.markStrength, 0.0f, 3.0f);
    ImGui::SliderFloat("Fresh Darken", &look_.freshDarken, 0.0f, 1.0f);
    ImGui::SliderFloat("Sparkle", &look_.sparkle, 0.0f, 3.0f);
    ImGui::SliderFloat("Wet Shine", &look_.wetShine, 0.0f, 3.0f);
    ImGui::SliderFloat("Wet Band", &look_.wetBand, 0.05f, 2.0f);
    if (ImGui::Button("Clear Marks")) RC::ClearInteractiveSand();
  }
#endif

protected:
  /// @brief 地形の高さを計算してメッシュを作る
  void OnCreate() override {
    if (!enabled_) return;
    BuildTerrain();
    swashTimer_ = swashPeriod_ * 0.5f;
  }

  /// @brief 跡の窓をカメラに合わせ、マウスの跡と波の均しを入れる
  /// @param deltaTime 前フレームからの経過時間（秒）
  void OnUpdate(float deltaTime) override {
    if (!enabled_ || mesh_ < 0) return;
    SceneContext *ctx = GetSceneContext();
    if (!ctx) return;
    const float dt = (deltaTime > 0.0f) ? deltaTime : (std::max)(ctx->deltaTime, 0.0f);
    time_ += dt;

    // 跡のハイトマップの窓はカメラの真下に置く（画面に映る所の跡だけ持てばよい）
    float camX = 0.0f, camZ = 0.0f;
    if (auto cam = WaterCameraFx::FindMainCamera(GetScene())) {
      if (auto *tr = cam->GetComponent<TransformComponent>()) {
        camX = tr->position.x;
        camZ = tr->position.z;
      }
    }
    RC::SetInteractiveSandCenter(camX, camZ);

    UpdateSwash(dt, camZ);
    UpdateCursor(ctx, dt);
  }

  /// @brief 砂の地形を描く
  void OnRender3D() override {
    if (!enabled_ || mesh_ < 0) return;
    look_.time = time_;
    RC::DrawSand(mesh_, look_);
  }

  /// @brief 地形メッシュを解放する
  void OnDestroy() override {
    if (mesh_ >= 0) RC::UnloadSandTerrain(mesh_);
    mesh_ = -1;
    prevValid_ = false;
  }

private:
  // ---- 地形 ----
  bool enabled_ = true;        ///< false で何もしない
  float areaMinX_ = -235.0f;   ///< 地形を作る範囲（X の最小）
  float areaMaxX_ = -30.0f;    ///< 地形を作る範囲（X の最大。沖で深く沈んでいる所まで）
  float areaMinZ_ = -60.0f;    ///< 地形を作る範囲（Z の最小）
  float areaMaxZ_ = 60.0f;     ///< 地形を作る範囲（Z の最大）
  float cellSize_ = 1.0f;      ///< 頂点の間隔（m）
  float shoreX_ = -58.0f;      ///< z = 0 での波打ち際の X（セレクトの浜辺の画面で左 3 分の 1 が陸になる位置）
  float shoreWobble_ = 3.0f;   ///< 波打ち際の出入り（m）
  float bermHeight_ = 1.4f;    ///< 波打ち際のすぐ上の段の高さ（m）
  float bermWidth_ = 3.0f;     ///< 段がおよそ上がりきるまでの幅（m）
  float beachSlope_ = 0.03f;   ///< 段より先の上り勾配
  float seaSlope_ = 0.12f;     ///< 浅瀬の下り勾配
  float dropStart_ = 14.0f;    ///< 波打ち際からこの距離で沖へ急に深くなる（m）
  float dropSlope_ = 0.5f;     ///< 沖の下り勾配
  float floorY_ = -16.0f;      ///< いちばん深い所
  float duneHeight_ = 1.2f;    ///< 陸の奥の砂丘の高さ（m）

  // ---- 見た目 ----
  RC::SandLook look_; ///< 砂の見た目（RC::DrawSand へ渡す）

  // ---- マウスの跡 ----
  bool trail_ = true;          ///< カーソルを動かしたときの筋を付けるか
  float trailRadius_ = 0.16f;  ///< 筋の半径（m）
  float trailDepth_ = 0.035f;  ///< 筋の深さ（m）
  float dragRadius_ = 0.30f;   ///< 押したまま動かしたときの溝の半径（m）
  float dragDepth_ = 0.08f;    ///< 溝の深さ（m）
  float dragRim_ = 0.035f;     ///< 溝の両脇の盛り上がり（m）
  bool footprints_ = true;     ///< クリックで足跡を付けるか
  float footSize_ = 1.3f;      ///< 足の長さ（m。画面で 100 ピクセルほど。少し大きめにして形が読めるように）

  // ---- 波 ----
  float swashPeriod_ = 5.5f;   ///< 波が駆け上がる間隔（秒）
  float swashReach_ = 3.5f;    ///< 波が届く距離（m。毎回 0.5〜1.2 倍にばらつく）

  // ---- 状態 ----
  int mesh_ = -1;              ///< 地形メッシュ
  float time_ = 0.0f;          ///< 経過時間（秒）
  bool prevValid_ = false;     ///< 前フレームのカーソルが砂の上にあったか
  float prevX_ = 0.0f;         ///< 前フレームのカーソル（ワールド X）
  float prevZ_ = 0.0f;         ///< 前フレームのカーソル（ワールド Z）
  float trailCarry_ = 0.0f;    ///< 筋を置く間隔の端数（m）
  float dirX_ = 0.0f;          ///< 最近の進む向き（足跡の向き）
  float dirZ_ = 1.0f;
  bool leftFoot_ = true;       ///< 次の足跡が左足か
  float swashTimer_ = 0.0f;    ///< 次の波までの時間（秒）
  float swashT_ = -1.0f;       ///< 波の進み（0〜1。負なら波は来ていない）
  float swashLen_ = 0.0f;      ///< 今回の波が届く距離（m）
  uint32_t rng_ = 0x9E3779B9u; ///< 乱数の状態

  static constexpr float kSwashTime = 1.6f; ///< 波が駆け上がって引くまで（秒）

  /// @brief 0〜1 の乱数
  float Rand01() {
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return static_cast<float>(rng_ & 0xFFFFFFu) / static_cast<float>(0x1000000u);
  }

  /// @brief なめらかな 2D ノイズ（-1〜1。地形の起伏用。ハッシュの補間）
  static float Noise(float x, float z) {
    auto hash = [](int ix, int iz) {
      uint32_t h = static_cast<uint32_t>(ix) * 374761393u + static_cast<uint32_t>(iz) * 668265263u;
      h = (h ^ (h >> 13)) * 1274126177u;
      return static_cast<float>((h ^ (h >> 16)) & 0xFFFFu) / 65535.0f * 2.0f - 1.0f;
    };
    const float fx = std::floor(x), fz = std::floor(z);
    const int ix = static_cast<int>(fx), iz = static_cast<int>(fz);
    float tx = x - fx, tz = z - fz;
    tx = tx * tx * (3.0f - 2.0f * tx);
    tz = tz * tz * (3.0f - 2.0f * tz);
    const float a = hash(ix, iz), b = hash(ix + 1, iz), c = hash(ix, iz + 1), d = hash(ix + 1, iz + 1);
    return (a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * tz;
  }

  /// @brief 波打ち際の X（z によって緩く出入りする）
  /// @param z ワールド Z
  /// @return その z での波打ち際の X
  float ShoreXAt(float z) const {
    return shoreX_ + shoreWobble_ * (std::sin(z * 0.065f) + 0.5f * std::sin(z * 0.17f + 1.3f));
  }

  /// @brief 地形の高さ
  /// @param x ワールド X
  /// @param z ワールド Z
  /// @return 高さ（m。水面は 0）
  float HeightAt(float x, float z) const {
    const float d = x - ShoreXAt(z); // 正なら海側
    float h = 0.0f;
    if (d <= 0.0f) {
      // 陸：波打ち際のすぐ上で段（バーム）になって水面より十分高くなり、
      // その先はなだらかに上がって、奥で小さな砂丘になる
      // （低いままだと水面の波が砂の上にかぶって白く見える）
      const float inland = -d;
      h = bermHeight_ * (1.0f - std::exp(-inland / (std::max)(bermWidth_, 0.1f))) + beachSlope_ * inland;
      const float duneW = std::clamp((inland - 25.0f) / 30.0f, 0.0f, 1.0f);
      h += duneHeight_ * duneW * duneW * (0.6f + 0.4f * Noise(x * 0.04f, z * 0.06f));
    } else {
      // 海：浅瀬の坂、沖で急に深くなる
      h = -seaSlope_ * (std::min)(d, dropStart_);
      if (d > dropStart_) h -= dropSlope_ * (d - dropStart_);
    }
    // 小さな起伏（波打ち際の近くは平らに）
    const float calm = std::clamp(std::fabs(d) / 6.0f, 0.2f, 1.0f);
    h += 0.12f * calm * Noise(x * 0.15f, z * 0.15f);
    return (std::max)(h, floorY_);
  }

  /// @brief 高さを並べて地形メッシュを作る
  void BuildTerrain() {
    const float cell = (std::max)(cellSize_, 0.25f);
    const float width = (std::max)(areaMaxX_ - areaMinX_, cell);
    const float depth = (std::max)(areaMaxZ_ - areaMinZ_, cell);
    const uint32_t nx = static_cast<uint32_t>(std::ceil(width / cell)) + 1;
    const uint32_t nz = static_cast<uint32_t>(std::ceil(depth / cell)) + 1;
    std::vector<float> heights(static_cast<size_t>(nx) * nz);
    for (uint32_t iz = 0; iz < nz; ++iz) {
      for (uint32_t ix = 0; ix < nx; ++ix) {
        const float x = areaMinX_ + width * static_cast<float>(ix) / static_cast<float>(nx - 1);
        const float z = areaMinZ_ + depth * static_cast<float>(iz) / static_cast<float>(nz - 1);
        heights[static_cast<size_t>(iz) * nx + ix] = HeightAt(x, z);
      }
    }
    mesh_ = RC::CreateSandTerrain(heights.data(), nx, nz, width, depth);
    if (mesh_ < 0) {
      Log::Print("[BeachSandScript] failed to create sand terrain");
      return;
    }
    if (Transform *t = RC::GetSandTransformPtr(mesh_)) {
      t->translation = {(areaMinX_ + areaMaxX_) * 0.5f, 0.0f, (areaMinZ_ + areaMaxZ_) * 0.5f};
    }
    look_.waterLevel = 0.0f;
  }

  /// @brief 数秒ごとに波を砂浜へ駆け上がらせ、届いた所の跡を均す
  /// @param dt   経過時間（秒）
  /// @param camZ カメラの Z（波を入れる範囲の中心）
  void UpdateSwash(float dt, float camZ) {
    if (swashPeriod_ <= 0.0f || swashReach_ <= 0.0f) return;
    if (swashT_ < 0.0f) {
      swashTimer_ -= dt;
      if (swashTimer_ > 0.0f) return;
      swashTimer_ = swashPeriod_ * (0.8f + 0.4f * Rand01());
      swashT_ = 0.0f;
      swashLen_ = swashReach_ * (0.5f + 0.7f * Rand01());
    }
    swashT_ += dt / kSwashTime;
    if (swashT_ >= 1.0f) {
      swashT_ = -1.0f;
      return;
    }
    // 駆け上がるのは前半だけ（後半は引いていくので均さない）
    if (swashT_ > 0.5f) return;
    const float front = swashLen_ * std::sin(swashT_ * 3.14159265f); // 波打ち際から陸側へ（m）
    constexpr float kSpacing = 2.2f;
    constexpr int kCount = 28;
    for (int i = 0; i < kCount; ++i) {
      const float z = camZ + (static_cast<float>(i) - (kCount - 1) * 0.5f) * kSpacing;
      // 波の先端は少しぎざぎざに
      const float jag = 0.6f * Noise(z * 0.3f, time_ * 0.2f);
      const float x = ShoreXAt(z) - front - jag;
      RC::AddSandSmooth(x, z, 1.8f, 0.35f);
    }
  }

  /// @brief カーソルの動き・ボタン・クリックで砂に跡を付ける
  /// @param ctx シーンの共有情報
  /// @param dt  経過時間（秒）
  void UpdateCursor(SceneContext *ctx, float dt) {
    (void)dt;
    if (!ctx->input || !ctx->isPlaying()) {
      prevValid_ = false;
      return;
    }
    float mx = 0.0f, my = 0.0f;
    ctx->input->GetGameMousePosition(mx, my);
    float screenW = 1280.0f, screenH = 720.0f;
    auto &rc = RC::GetRenderContext();
    if (rc.Ctx() && rc.Ctx()->app && rc.Ctx()->app->width > 0 && rc.Ctx()->app->height > 0) {
      screenW = static_cast<float>(rc.Ctx()->app->width);
      screenH = static_cast<float>(rc.Ctx()->app->height);
    }
    float hx = 0.0f, hz = 0.0f;
    if (mx < 0.0f || my < 0.0f || mx > screenW || my > screenH ||
        !HitSand(mx, my, screenW, screenH, rc.View(), rc.Proj(), hx, hz) ||
        HeightAt(hx, hz) < look_.waterLevel + 0.03f) {
      // 画面の外・水の上では跡を付けない（水には TitleMouseRippleScript が波紋を立てる）
      prevValid_ = false;
      return;
    }

    const bool held = ctx->input->IsMousePressed(0);
    if (prevValid_) {
      const float dx = hx - prevX_, dz = hz - prevZ_;
      const float dist = std::sqrt(dx * dx + dz * dz);
      if (dist > 0.01f) {
        // 足跡の向き：最近の進む向きをなめらかに追う
        const float k = (std::min)(1.0f, dist / 1.5f);
        dirX_ += (dx / dist - dirX_) * k;
        dirZ_ += (dz / dist - dirZ_) * k;
        const float dl = std::sqrt(dirX_ * dirX_ + dirZ_ * dirZ_);
        if (dl > 1e-4f) {
          dirX_ /= dl;
          dirZ_ /= dl;
        }
        if (held || trail_) {
          const float radius = held ? dragRadius_ : trailRadius_;
          const float depthM = held ? dragDepth_ : trailDepth_;
          const float rim = held ? dragRim_ : trailDepth_ * 0.4f;
          // 軌跡に沿って半径の 1/3 おきに置く（速く振っても途切れないように）
          const float spacing = (std::max)(radius * 0.35f, 0.05f);
          float s = spacing - trailCarry_;
          int placed = 0;
          while (s <= dist && placed < 48) {
            const float t = s / dist;
            RC::AddSandDig(prevX_ + dx * t, prevZ_ + dz * t, radius, depthM, rim, held ? 1.0f : 0.5f);
            s += spacing;
            ++placed;
          }
          trailCarry_ = dist - (s - spacing);
        }
      }
    }
    if (footprints_ && ctx->input->IsMouseTrigger(0)) StampFoot(hx, hz);

    prevX_ = hx;
    prevZ_ = hz;
    prevValid_ = true;
  }

  /// @brief 足跡を 1 つ押す（土踏まずのくびれた足の裏と 5 本の指。左右交互）
  /// @param x 足の中心のワールド X
  /// @param z 足の中心のワールド Z
  void StampFoot(float x, float z) {
    const float L = (std::max)(footSize_, 0.3f);
    // 前（つま先の向き）と右
    const float fx = dirX_, fz = dirZ_;
    const float rx = fz, rz = -fx;
    const float side = leftFoot_ ? -1.0f : 1.0f; // 左足は左へ少しずらす
    leftFoot_ = !leftFoot_;
    const float cx = x + rx * side * L * 0.18f;
    const float cz = z + rz * side * L * 0.18f;
    // 足の内側（親指側）は体の中心側 = 左足なら右
    const float inner = -side;
    auto at = [&](float forward, float right) {
      return std::pair<float, float>{cx + fx * forward * L + rx * right * L, cz + fz * forward * L + rz * right * L};
    };
    const float depth = 0.11f;
    const float rim = 0.035f;
    // かかと・土踏まず（外側寄りで細い）・母指球
    struct Pad {
      float forward, right, radius;
    };
    const Pad sole[] = {
        {-0.30f, 0.00f, 0.17f},
        {-0.10f, -0.05f * inner, 0.12f},
        {0.10f, -0.03f * inner, 0.15f},
        {0.22f, 0.02f * inner, 0.18f},
    };
    for (const Pad &p : sole) {
      const auto [px, pz] = at(p.forward, p.right);
      RC::AddSandDig(px, pz, p.radius * L, depth, rim, 1.0f);
    }
    // 指：親指が大きく、外側へいくほど小さく後ろへ下がる
    const Pad toes[] = {
        {0.44f, 0.10f, 0.075f},
        {0.43f, 0.01f, 0.055f},
        {0.41f, -0.07f, 0.050f},
        {0.38f, -0.14f, 0.045f},
        {0.34f, -0.20f, 0.040f},
    };
    for (const Pad &p : toes) {
      const auto [px, pz] = at(p.forward, p.right * inner);
      RC::AddSandDig(px, pz, p.radius * L, depth * 0.8f, rim * 0.6f, 1.0f);
    }
  }

  /// @brief 画面座標から砂の地形との交点を求める（水面の高さの平面から始めて、地形の高さで数回詰める）
  /// @return 交点が求まれば true
  bool HitSand(float mx, float my, float screenW, float screenH, const RC::Matrix4x4 &view,
               const RC::Matrix4x4 &proj, float &outX, float &outZ) const {
    const RC::Ray ray = RC::ScreenPointToRay({mx, my}, screenW, screenH, view, proj);
    const float dy = ray.direction.y;
    if (std::fabs(dy) < 1e-5f) return false;
    float planeY = 0.0f;
    float x = 0.0f, z = 0.0f;
    for (int i = 0; i < 4; ++i) {
      const float t = (planeY - ray.origin.y) / dy;
      if (t <= 0.0f || !std::isfinite(t)) return false;
      x = ray.origin.x + ray.direction.x * t;
      z = ray.origin.z + ray.direction.z * t;
      planeY = HeightAt(x, z);
    }
    if (x < areaMinX_ || x > areaMaxX_ || z < areaMinZ_ || z > areaMaxZ_) return false;
    outX = x;
    outZ = z;
    return true;
  }
};

REGISTER_SCRIPT(BeachSandScript)
