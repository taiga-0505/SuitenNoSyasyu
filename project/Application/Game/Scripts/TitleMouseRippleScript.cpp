#include "ECS/ScriptableEntity.h"
#include "ECS/ScriptRegistry.h"
#include "ECS/TransformComponent.h"
#include "ECS/WaterComponent.h"
#include "Common/Math/MathUtils.h"
#include "Common/Log/Log.h"
#include "Input/Input.h"
#include "Render/Systems/RenderInteractiveWater.h"
#include "RenderCommon.h"
#include "Engine/Render/RenderContext.h"
#include "Framework/App.h"
#include "Scene.h"

#if RC_ENABLE_IMGUI
#include "imgui/imgui.h"
#endif

#include <algorithm>
#include <cmath>

// ============================================================================
// タイトル画面：マウスカーソルの動きに合わせて水面に波紋を立てる
//
//   - Title.json の空エンティティに NativeScript として 1 つ付けるだけで動く。
//     TitleScreenScript / TitleAmbientScript とは独立していて、互いに参照しない。
//   - 仕組み：毎フレーム、マウス座標からカメラのレイを飛ばして水面（y = 水面の Transform.y）
//     との交点を求め、前フレームの交点からの軌跡に沿って RenderInteractiveWater へ波源を入れる。
//     波の伝播そのものは既存の GPU シミュレーション（WaveSimulation.CS、256x256、毎フレーム
//     もともと走っている）に任せるので、追加コストは波源の数個ぶんの CPU 計算だけ。
//   - 速く動かすほど強い波紋になる（speedRef で満額になる速さを決める）。止まっていれば何も出ない。
//   - 左クリックで大きめの波紋を 1 発。
//   - Title.json と Select.json に配置（両シーンともカメラが同じ俯瞰なので同じパラメータで揃う）。
//     ゲーム中には付けない方針。付けたければ Game.json に同じエンティティを足すだけ。
//   - 波紋の UV 写像はワールド 100m 四方固定（Water.VS.hlsl と同じ）。画面外へ出た／
//     範囲外（±50m）は AddWaveSourceAtWorld が捨てる。
// ============================================================================

/// @class TitleMouseRippleScript
/// @brief マウスカーソルの動きで水面に波紋を立てる
/// @details JSON (scriptDataList) で設定できる項目:
///   "enabled"        : false で何もしない
///   "radius"         : 波源の半径（UV 空間。0.01 = 1m）
///   "strength"       : 波源 1 つの高さ（m）。pushDown が true なら押し下げ（指で水面を撫でる感じ）
///   "pushDown"       : true で水面を押し下げる。false で持ち上げる
///   "speedRef"       : この速さ（m/s）でカーソルを動かしたとき strength 満額になる
///   "minStrengthRatio": ゆっくり動かしたときの下限（strength に掛かる割合）
///   "spacing"        : 軌跡に沿って波源を置く間隔（m）。小さいほど連続した筋になる
///   "maxPerFrame"    : 1 フレームに入れる波源の上限（シミュレーション全体で 64/フレーム）
///   "clickEnabled" / "clickRadius" / "clickStrength" : 左クリックの波紋
///   "playingOnly"    : true なら再生中だけ（エディタで編集中は出さない）
class TitleMouseRippleScript : public ScriptableEntity {
public:
  bool enabled = true;
  float radius = 0.018f;
  float strength = 0.07f;
  bool pushDown = true;
  float speedRef = 10.0f;
  float minStrengthRatio = 0.3f;
  float spacing = 0.5f;
  int maxPerFrame = 10;
  bool clickEnabled = true;
  float clickRadius = 0.035f;
  float clickStrength = 0.3f;
  bool playingOnly = false;

  nlohmann::json Serialize() override {
    return {
        {"enabled", enabled},
        {"radius", radius},
        {"strength", strength},
        {"pushDown", pushDown},
        {"speedRef", speedRef},
        {"minStrengthRatio", minStrengthRatio},
        {"spacing", spacing},
        {"maxPerFrame", maxPerFrame},
        {"clickEnabled", clickEnabled},
        {"clickRadius", clickRadius},
        {"clickStrength", clickStrength},
        {"playingOnly", playingOnly},
    };
  }

  void Deserialize(const nlohmann::json &j) override {
    auto readF = [&](const char *key, float &out) {
      if (j.contains(key) && j[key].is_number()) out = j[key].get<float>();
    };
    auto readB = [&](const char *key, bool &out) {
      if (j.contains(key) && j[key].is_boolean()) out = j[key].get<bool>();
    };
    readB("enabled", enabled);
    readF("radius", radius);
    readF("strength", strength);
    readB("pushDown", pushDown);
    readF("speedRef", speedRef);
    readF("minStrengthRatio", minStrengthRatio);
    minStrengthRatio = std::clamp(minStrengthRatio, 0.0f, 1.0f); // std::clamp(lo > hi) を避ける
    readF("spacing", spacing);
    if (j.contains("maxPerFrame") && j["maxPerFrame"].is_number()) maxPerFrame = j["maxPerFrame"].get<int>();
    readB("clickEnabled", clickEnabled);
    readF("clickRadius", clickRadius);
    readF("clickStrength", clickStrength);
    readB("playingOnly", playingOnly);
  }

#if RC_ENABLE_IMGUI
  void OnImGui() override {
    ImGui::Checkbox("Enabled", &enabled);
    ImGui::Text("Hit: %s  (%.2f, %.2f)  speed %.1f m/s  sources/frame %d",
                prevValid_ ? "yes" : "no", prevX_, prevZ_, lastSpeed_, lastCount_);
    ImGui::SeparatorText("Move");
    ImGui::DragFloat("Radius (UV)", &radius, 0.001f, 0.002f, 0.1f, "%.3f");
    ImGui::DragFloat("Strength", &strength, 0.005f, 0.0f, 0.5f);
    ImGui::Checkbox("Push Down", &pushDown);
    ImGui::DragFloat("Speed Ref (m/s)", &speedRef, 0.1f, 0.5f, 60.0f);
    ImGui::DragFloat("Min Strength Ratio", &minStrengthRatio, 0.01f, 0.0f, 1.0f);
    ImGui::DragFloat("Spacing (m)", &spacing, 0.05f, 0.1f, 5.0f);
    ImGui::DragInt("Max Per Frame", &maxPerFrame, 1, 1, 32);
    ImGui::SeparatorText("Click");
    ImGui::Checkbox("Click Enabled", &clickEnabled);
    ImGui::DragFloat("Click Radius (UV)", &clickRadius, 0.001f, 0.002f, 0.2f, "%.3f");
    ImGui::DragFloat("Click Strength", &clickStrength, 0.01f, 0.0f, 1.0f);
    ImGui::Checkbox("Playing Only", &playingOnly);
  }
#endif

protected:
  void OnUpdate(float deltaTime) override {
    SceneContext *ctx = GetSceneContext();
    if (!enabled || !ctx || !ctx->input) {
      prevValid_ = false;
      return;
    }
    if (playingOnly && !ctx->isPlaying()) {
      prevValid_ = false;
      return;
    }
    // 経過時間は速さの計算にだけ使う。ポーズ中（deltaTime = 0）は ctx 側の値で代用する
    const float dt = (deltaTime > 0.0f) ? deltaTime : (std::max)(ctx->deltaTime, 1.0f / 240.0f);

    // ---- マウス → 水面の交点 ----
    float mx = 0.0f, my = 0.0f;
    ctx->input->GetGameMousePosition(mx, my);

    float screenW = 1280.0f, screenH = 720.0f;
    auto &rc = RC::GetRenderContext();
    if (rc.Ctx() && rc.Ctx()->app && rc.Ctx()->app->width > 0 && rc.Ctx()->app->height > 0) {
      screenW = static_cast<float>(rc.Ctx()->app->width);
      screenH = static_cast<float>(rc.Ctx()->app->height);
    }
    // ウィンドウの外（フォーカスが無いときなど）は何もしない。戻ってきたら軌跡を引き直す
    if (mx < 0.0f || my < 0.0f || mx > screenW || my > screenH) {
      prevValid_ = false;
      return;
    }

    float hx = 0.0f, hz = 0.0f;
    if (!HitWater(mx, my, screenW, screenH, rc.View(), rc.Proj(), hx, hz)) {
      prevValid_ = false;
      return;
    }

    lastCount_ = 0;

    // ---- クリック：1 発大きめ ----
    if (clickEnabled && ctx->input->IsMouseTrigger(0)) {
      const float s = pushDown ? -clickStrength : clickStrength;
      if (RC::AddWaveSourceAtWorld(hx, hz, clickRadius, s)) ++lastCount_;
    }

    // ---- 動き：前フレームの交点からの軌跡に沿って波源を並べる ----
    if (prevValid_) {
      const float dx = hx - prevX_;
      const float dz = hz - prevZ_;
      const float dist = std::sqrt(dx * dx + dz * dz);
      lastSpeed_ = dist / dt;
      if (dist > kMinMove) {
        // 速いほど強く。ただし上限は strength
        const float lo = std::clamp(minStrengthRatio, 0.0f, 1.0f); // ImGui で 1 を超えて入れても安全に
        const float ratio = std::clamp(lastSpeed_ / (std::max)(speedRef, 0.01f), lo, 1.0f);
        const float s = (pushDown ? -strength : strength) * ratio;
        // 軌跡が長いときは間を埋める（速く振ったときに点線にならないように）
        const int n = std::clamp(static_cast<int>(std::ceil(dist / (std::max)(spacing, 0.01f))), 1,
                                 (std::max)(maxPerFrame, 1));
        for (int i = 1; i <= n; ++i) {
          const float t = static_cast<float>(i) / static_cast<float>(n);
          if (RC::AddWaveSourceAtWorld(prevX_ + dx * t, prevZ_ + dz * t, radius, s)) ++lastCount_;
        }
      }
    } else {
      lastSpeed_ = 0.0f;
    }

    prevX_ = hx;
    prevZ_ = hz;
    prevValid_ = true;
  }

  void OnDestroy() override { prevValid_ = false; }

private:
  static constexpr float kMinMove = 0.02f; ///< これ未満の動き（m）は「止まっている」扱い

  /// @brief 画面座標から水面（y = 水面の高さ）との交点を求める
  /// @return カメラが水面より下を向いていない等で交点が無ければ false
  bool HitWater(float mx, float my, float screenW, float screenH, const RC::Matrix4x4 &view,
                const RC::Matrix4x4 &proj, float &outX, float &outZ) const {
    const float waterY = FindWaterY();
    const RC::Ray ray = RC::ScreenPointToRay({mx, my}, screenW, screenH, view, proj);
    // レイと平面 y = waterY の交点。カメラが水面を向いていなければ無し
    const float dy = ray.direction.y;
    if (std::fabs(dy) < 1e-5f) return false;
    const float t = (waterY - ray.origin.y) / dy;
    if (t <= 0.0f || !std::isfinite(t)) return false;
    outX = ray.origin.x + ray.direction.x * t;
    outZ = ray.origin.z + ray.direction.z * t;
    return std::isfinite(outX) && std::isfinite(outZ);
  }

  /// @brief シーンの WaterComponent の高さ（無ければ 0）
  float FindWaterY() const {
    Scene *scene = GetScene();
    if (!scene) return 0.0f;
    for (const auto &e : scene->GetEntities()) {
      if (!e || !e->GetComponent<WaterComponent>()) continue;
      if (auto *tr = e->GetComponent<TransformComponent>()) return tr->position.y;
      return 0.0f;
    }
    return 0.0f;
  }

  bool prevValid_ = false;
  float prevX_ = 0.0f;
  float prevZ_ = 0.0f;
  float lastSpeed_ = 0.0f;
  int lastCount_ = 0;
};

REGISTER_SCRIPT(TitleMouseRippleScript)
