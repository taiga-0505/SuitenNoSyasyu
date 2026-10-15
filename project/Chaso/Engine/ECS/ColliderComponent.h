#pragma once
#include "IComponent.h"
#include "Math/MathTypes.h"
#include "struct.h" // AABB
#include <nlohmann/json.hpp>

/// @class ColliderComponent
/// @brief 衝突判定用のコンポーネント
/// @details 形状（AABB / 球 / カプセル）とそのパラメータ、衝突レイヤーを持つ。
class ColliderComponent : public IComponent {
public:
  /// @enum Shape
  /// @brief コライダーの形状
  enum class Shape {
    AABB,    ///< 軸並行境界ボックス
    Sphere,  ///< 球
    Capsule, ///< カプセル
  };

  Shape shape = Shape::AABB; ///< 現在の形状

  AABB aabb; ///< AABB のパラメータ
  RC::Vector3 size = {1.0f, 1.0f, 1.0f}; ///< ボックスのサイズ

  RC::Vector3 center = {0.0f, 0.0f, 0.0f}; ///< 中心位置（ローカル）
  float radius = 1.0f;                      ///< 半径

  uint32_t layer = 0xFFFFFFFF; ///< 衝突レイヤー（ビットマスク）

  /// @brief トリガーモード。true なら物理的な押し戻しはせず、イベント通知だけ行う
  bool isTrigger = false;

  /// @brief 衝突ブロードフェーズ（ColliderBroadphase）内の登録番号。ランタイム専用で保存しない
  /// @details -1 なら未登録。ColliderBroadphase が書き換える（ゲーム側から触らないこと）。
  ///          コンポーネントが複製された場合でも、ブロードフェーズ側でエンティティとの対応を
  ///          照合してから使うので、番号が重複しても壊れない。
  int32_t broadphaseEntry = -1;

  /// @brief コンポーネントの型名を返す
  /// @return "ColliderComponent"
  const char* TypeName() const override { return "ColliderComponent"; }

  /// @brief 形状パラメータとレイヤーを JSON に書き出す
  /// @return shape / center / size / radius / layer / isTrigger を格納した JSON
  nlohmann::json Serialize() const override {
    return {
      {"shape", static_cast<int>(shape)},
      {"center", {center.x, center.y, center.z}},
      {"size", {size.x, size.y, size.z}},
      {"radius", radius},
      {"layer", layer},
      {"isTrigger", isTrigger}
    };
  }

  /// @brief JSON から形状パラメータとレイヤーを読み込む
  /// @param j 読み込み元の JSON（含まれていないキーは現在の値のまま）
  void Deserialize(const nlohmann::json& j) override {
    if (j.contains("shape")) shape = static_cast<Shape>(j["shape"].get<int>());
    if (j.contains("center")) {
      auto& c = j["center"];
      center = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>()};
    }
    if (j.contains("size")) {
      auto& s = j["size"];
      size = {s[0].get<float>(), s[1].get<float>(), s[2].get<float>()};
    }
    if (j.contains("radius")) radius = j["radius"].get<float>();
    if (j.contains("layer")) layer = j["layer"].get<uint32_t>();
    if (j.contains("isTrigger")) isTrigger = j["isTrigger"].get<bool>();
  }
};
