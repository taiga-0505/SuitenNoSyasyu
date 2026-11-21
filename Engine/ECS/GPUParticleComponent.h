#pragma once

#include "IComponent.h"
#include "Particle/GPUParticle.h"
#include <nlohmann/json.hpp>
#include <memory>
#include <string>

/// @class GPUParticleComponent
/// @brief Entity 上で GPU パーティクルを描画するコンポーネント
class GPUParticleComponent : public IComponent {
public:
  std::unique_ptr<GPUParticle> particleSystem; ///< パーティクルシステム本体
  bool isInitialized = false;                  ///< particleSystem の初期化が済んだか

  std::string configPath = "Resources/Particle/default.json"; ///< パーティクル設定 JSON のパス
  std::string texturePath = "Resources/Particle/circle.png";  ///< パーティクルに使うテクスチャのパス

  /// @brief パーティクルシステムを生成する（初期化はまだ行わない）
  GPUParticleComponent() {
    particleSystem = std::make_unique<GPUParticle>();
  }

  /// @brief コンポーネントの型名を返す
  /// @return "GPUParticleComponent"
  const char* TypeName() const override { return "GPUParticleComponent"; }

  /// @brief 設定・テクスチャのパスを JSON に書き出す
  /// @return configPath / texturePath を格納した JSON
  nlohmann::json Serialize() const override {
    return {
      {"configPath", configPath},
      {"texturePath", texturePath}
    };
  }

  /// @brief JSON から設定・テクスチャのパスを読み込む
  /// @param j 読み込み元の JSON（含まれていないキーは現在の値のまま）
  void Deserialize(const nlohmann::json& j) override {
    if (j.contains("configPath")) {
      configPath = j["configPath"].get<std::string>();
    }
    if (j.contains("texturePath")) {
      texturePath = j["texturePath"].get<std::string>();
    }
  }
};
