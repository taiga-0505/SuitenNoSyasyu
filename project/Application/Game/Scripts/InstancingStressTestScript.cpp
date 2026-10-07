#include "ECS/ScriptableEntity.h"
#include "ECS/ScriptRegistry.h"
#include "ECS/TransformComponent.h"
#include "Common/EngineConfig.h"
#include "RenderCommon.h"

#if RC_ENABLE_IMGUI
#include "imgui/imgui.h"
#endif

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

/// @class InstancingStressTestScript
/// @brief VirtualEntity（RC::CreateModelProxy）を大量に並べて、描画負荷を確かめるための検証用スクリプト
/// @details
///   空のエンティティに付けると、そのエンティティの位置を中心に count 個のモデルを格子状に並べる。
///   Entity は 1 つも増やさず、ModelProxyPool に登録するだけなので、
///   1 万個置いても Update / 当たり判定のコストは増えない。
///   描画は DataDrivenScene が各パスで呼ぶ RC::DrawModelProxies() が
///   BVH による視錐台カリング → インスタンス描画でまとめて行う。
///
///   animatedRatio > 0 にすると、その割合のプロキシを毎フレーム上下に動かす
///   （行列が変わった物だけ BVH を更新する「差分更新」の負荷確認用）。
///   Inspector に前フレームの統計（受付数・カリング数・バッチ数・ドローコール数）を表示する。
class InstancingStressTestScript : public ScriptableEntity {
public:
  std::string modelPath = "Resources/model/cube/cube.obj"; ///< 並べるモデル
  int count = 10000;          ///< 個数
  float spacing = 3.0f;       ///< 間隔 (m)
  float animatedRatio = 0.0f; ///< 毎フレーム動かす割合 (0〜1)

  nlohmann::json Serialize() override {
    return {{"modelPath", modelPath}, {"count", count}, {"spacing", spacing},
            {"animatedRatio", animatedRatio}};
  }

  void Deserialize(const nlohmann::json &j) override {
    if (j.contains("modelPath")) modelPath = j["modelPath"].get<std::string>();
    if (j.contains("count")) count = j["count"].get<int>();
    if (j.contains("spacing")) spacing = j["spacing"].get<float>();
    if (j.contains("animatedRatio")) animatedRatio = j["animatedRatio"].get<float>();
  }

  void OnImGui() override {
#if RC_ENABLE_IMGUI
    ImGui::Text("Proxies: %u", RC::GetModelProxyCount());
    const auto st = RC::GetModelInstancingStats();
    // 前フレームの値。Drawn / Batches / Draw calls は全パス（メイン＋影）の合計
    ImGui::Text("Camera: %u visible / %u culled (of %u)", st.proxyMainVisible,
                st.proxyTotal - (std::min)(st.proxyTotal, st.proxyMainVisible), st.proxyTotal);
    ImGui::Text("Drawn %u instances in %u batches (%u draw calls), flush %.2f ms",
                st.instancesDrawn, st.batches, st.drawCalls, st.flushMs);
    if (st.overflowSkipped > 0) {
      ImGui::TextColored({1.0f, 0.3f, 0.3f, 1.0f}, "FrameResource overflow: %u skipped",
                         st.overflowSkipped);
    }
    ImGui::DragInt("Count", &count, 100.0f, 0, 200000);
    ImGui::DragFloat("Spacing", &spacing, 0.1f, 0.1f, 100.0f);
    ImGui::SliderFloat("Animated Ratio", &animatedRatio, 0.0f, 1.0f);
    if (ImGui::Button("Rebuild")) {
      Rebuild();
    }
#endif
  }

protected:
  void OnCreate() override { Rebuild(); }

  void OnUpdate(float deltaTime) override {
    time_ += deltaTime;
    if (animatedRatio <= 0.0f || proxies_.empty()) return;

    const size_t n = static_cast<size_t>(static_cast<float>(proxies_.size()) * animatedRatio);
    for (size_t i = 0; i < n && i < proxies_.size(); ++i) {
      Transform t = base_[i];
      t.translation.y += std::sin(time_ * 2.0f + static_cast<float>(i) * 0.1f) * 0.5f;
      RC::SetModelProxyTransform(proxies_[i], t);
    }
  }

  void OnDestroy() override { Release(); }

private:
  /// @brief 全プロキシを作り直す
  void Rebuild() {
    Release();
    if (count <= 0) return;

    modelHandle_ = RC::LoadModel(modelPath);
    if (modelHandle_ < 0) return;

    RC::Vector3 center = {0.0f, 0.0f, 0.0f};
    if (auto *tr = GetComponent<TransformComponent>()) center = tr->position;

    const int side = static_cast<int>(std::ceil(std::sqrt(static_cast<float>(count))));
    const float half = static_cast<float>(side - 1) * spacing * 0.5f;
    proxies_.reserve(static_cast<size_t>(count));
    base_.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
      const int x = i % side;
      const int z = i / side;
      Transform t{};
      t.scale = {1.0f, 1.0f, 1.0f};
      t.rotation = {0.0f, static_cast<float>(i) * 0.37f, 0.0f};
      t.translation = {center.x + static_cast<float>(x) * spacing - half, center.y,
                       center.z + static_cast<float>(z) * spacing - half};
      const RC::ModelProxyHandle h = RC::CreateModelProxy(modelHandle_);
      RC::SetModelProxyTransform(h, t);
      // 色違いでも同じバッチにまとまることを確かめるため、少しずつ色を変える
      RC::SetModelProxyColor(h, {0.6f + 0.4f * static_cast<float>(x % 5) / 4.0f,
                                 0.6f + 0.4f * static_cast<float>(z % 7) / 6.0f, 1.0f, 1.0f});
      proxies_.push_back(h);
      base_.push_back(t);
    }
  }

  /// @brief 作ったプロキシとテンプレートを解放する
  void Release() {
    for (const auto &h : proxies_) RC::DestroyModelProxy(h);
    proxies_.clear();
    base_.clear();
    if (modelHandle_ >= 0) {
      RC::UnloadModel(modelHandle_);
      modelHandle_ = -1;
    }
  }

  std::vector<RC::ModelProxyHandle> proxies_;
  std::vector<Transform> base_;
  int modelHandle_ = -1;
  float time_ = 0.0f;
};

REGISTER_SCRIPT(InstancingStressTestScript)
