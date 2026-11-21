#pragma once

// ============================================================================
// TweenComponent
// ----------------------------------------------------------------------------
// エンティティに付けた Tween（Chaso::TweenRunner）を毎フレーム進めるコンポーネント。
// スクリプトからは ScriptableEntity::Tweens() で自動的に付く（自分で AddComponent しなくてよい）。
// エンティティと一緒に消えるので、自分のコンポーネントの値を動かす限り Tween が浮いて残らない。
// 実行時だけのものなので、シーンファイルには保存しない。
// ============================================================================

#include "IComponent.h"
#include "Common/GameTime.h"
#include "Common/Tween/Tween.h"
#include <nlohmann/json.hpp>

/// @class TweenComponent
/// @brief エンティティ単位の Tween の入れ物
class TweenComponent : public IComponent {
public:
  Chaso::TweenRunner tweens; ///< このエンティティの Tween

  const char *TypeName() const override { return "TweenComponent"; }

  /// @brief 動かした結果を、スクリプト・剛体の移動の後で書き込む
  int UpdateOrder() const override { return ComponentUpdateOrder::kLate; }

  /// @brief ゲーム時間で進める（Unscaled() の Tween は実時間で進む）
  void Update(float deltaTime) override {
    tweens.Update(deltaTime, Chaso::Time::Get().UnscaledDeltaTime());
  }

  /// @brief 実行時だけのコンポーネントなので保存しない
  bool IsSerializable() const override { return false; }

  nlohmann::json Serialize() const override { return nlohmann::json::object(); }
  void Deserialize(const nlohmann::json &) override {}
};
