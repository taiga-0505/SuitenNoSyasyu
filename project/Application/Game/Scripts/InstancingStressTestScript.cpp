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

  /// @brief 設定値（モデル・個数・間隔・動かす割合）を JSON に書き出す
  /// @return 設定値を持つ JSON オブジェクト
  nlohmann::json Serialize() override {
    return {{"modelPath", modelPath}, {"count", count}, {"spacing", spacing},
            {"animatedRatio", animatedRatio}};
  }

  /// @brief JSON から設定値を読み込む
  /// @param j 読み込む JSON（無いキーは現在の値のまま）
  void Deserialize(const nlohmann::json &j) override {
    if (j.contains("modelPath")) modelPath = j["modelPath"].get<std::string>();
    if (j.contains("count")) count = j["count"].get<int>();
    if (j.contains("spacing")) spacing = j["spacing"].get<float>();
    if (j.contains("animatedRatio")) animatedRatio = j["animatedRatio"].get<float>();
  }

  /// @brief Inspector に前フレームの描画統計と設定値の編集 UI・再構築ボタンを表示する
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
  /// @brief 設定値に従ってプロキシを並べる
  void OnCreate() override { Rebuild(); }

  /// @brief 先頭から animatedRatio の割合のプロキシを sin 波で上下に動かす（BVH 差分更新の負荷確認用）
  /// @param deltaTime 前フレームからの経過時間（秒）
  void OnUpdate(float deltaTime) override {
    time_ += deltaTime;
    if (animatedRatio <= 0.0f || proxies_.empty()) return;

    // 位相を index ごとに 0.1 ずつずらし、全部が同時に動かないようにする（振幅 0.5m）
    const size_t n = static_cast<size_t>(static_cast<float>(proxies_.size()) * animatedRatio);
    for (size_t i = 0; i < n && i < proxies_.size(); ++i) {
      Transform t = base_[i];
      t.translation.y += std::sin(time_ * 2.0f + static_cast<float>(i) * 0.1f) * 0.5f;
      RC::SetModelProxyTransform(proxies_[i], t);
    }
  }

  /// @brief 作ったプロキシとモデルを解放する
  void OnDestroy() override { Release(); }

private:
  /// @brief 全プロキシを作り直す
  /// @note count が 0 以下、またはモデルの読み込みに失敗したときは何も並べない。
  void Rebuild() {
    Release();
    if (count <= 0) return;

    modelHandle_ = RC::LoadModel(modelPath);
    if (modelHandle_ < 0) return;

    RC::Vector3 center = {0.0f, 0.0f, 0.0f};
    if (auto *tr = GetComponent<TransformComponent>()) center = tr->position;

    // ceil(√count) 列の正方形の格子に、エンティティの位置を中心として並べる。
    // 向きは index ごとに 0.37rad ずつ回して、全部が同じ向きにならないようにする
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

  /// @brief 作ったプロキシと基準 Transform・読み込んだモデルを解放する
  void Release() {
    for (const auto &h : proxies_) RC::DestroyModelProxy(h);
    proxies_.clear();
    base_.clear();
    if (modelHandle_ >= 0) {
      RC::UnloadModel(modelHandle_);
      modelHandle_ = -1;
    }
  }

  std::vector<RC::ModelProxyHandle> proxies_; ///< 作ったプロキシ
  std::vector<Transform> base_;               ///< 各プロキシの基準 Transform（上下動の基準）
  int modelHandle_ = -1;                      ///< 読み込んだモデル（未読込なら -1）
  float time_ = 0.0f;                         ///< 経過時間（秒。上下動の位相に使う）
};

REGISTER_SCRIPT(InstancingStressTestScript)
