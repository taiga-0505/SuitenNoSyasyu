#pragma once

#include "IComponent.h"
#include "Math/MathTypes.h"
#include <string>
#include <nlohmann/json.hpp>

/// @enum SpriteSpace
/// @brief スプライトをどの空間で描画するか
enum class SpriteSpace : int {
  Screen = 0,   ///< スクリーン座標。3D より手前（従来の挙動）
  ScreenBehind, ///< スクリーン座標。常に 3D より奥（背景用）
  World,        ///< ワールド座標。深度テストありでモデルと前後関係がつく
};

/// @class SpriteRendererComponent
/// @brief 2D スプライトを描画するコンポーネント
/// @details スプライトハンドルと表示サイズを持ち、エンティティのループで自動的に描画される。
class SpriteRendererComponent : public IComponent {
public:
  int spriteHandle = -1;  ///< スプライトハンドル（RC::LoadSprite の戻り値）
  bool visible = true;    ///< 表示するか
  RC::Vector2 size = {100.0f, 100.0f}; ///< 表示サイズ（スクリーン座標、ピクセル）
  RC::Vector4 color = {1.0f, 1.0f, 1.0f, 1.0f}; ///< 乗算色

  /// @brief 描画する空間
  /// @details Screen       … 従来通り。x/y はピクセル、常にモデルより手前。
  ///          ScreenBehind … x/y はピクセルのまま、常にモデルより奥（背景）。
  ///          World        … Transform をワールド座標として扱い、深度テストあり。
  ///                          任意のモデルとモデルの間に挟み込める。
  ///                          大きさは Size ではなく Transform.scale で決まる。
  SpriteSpace space = SpriteSpace::Screen;

  /// @brief ワールド空間モードか
  /// @return space が World なら true
  bool IsWorldSpace() const { return space == SpriteSpace::World; }

  /// @brief 有効なスプライトが設定されているかを返す
  /// @return spriteHandle が 0 以上なら true
  bool HasSprite() const { return spriteHandle >= 0; }

  std::string spritePath; ///< シリアライズ用のアセットパス
  std::string loadedPath; ///< spriteHandle に読み込み済みのパス（実行時のみ。遅延再読み込みの判定用）

  /// @brief コンポーネントの型名を返す
  /// @return "SpriteRendererComponent"
  const char* TypeName() const override { return "SpriteRendererComponent"; }

  /// @brief パスと描画設定を JSON に書き出す
  /// @return spritePath / visible / size / color / space を格納した JSON
  nlohmann::json Serialize() const override {
    return {
      {"spritePath", spritePath},
      {"visible", visible},
      {"size", {size.x, size.y}},
      {"color", {color.x, color.y, color.z, color.w}},
      {"space", static_cast<int>(space)}
    };
  }

  /// @brief JSON からパスと描画設定を読み込む
  /// @param j 読み込み元の JSON（含まれていないキーは現在の値のまま）
  /// @note space が無く旧形式の behindModel がある場合は、それを Screen / ScreenBehind に読み替える。
  void Deserialize(const nlohmann::json& j) override {
    if (j.contains("spritePath")) spritePath = j["spritePath"].get<std::string>();
    if (j.contains("visible")) visible = j["visible"].get<bool>();
    if (j.contains("size")) {
      auto& s = j["size"];
      size = {s[0].get<float>(), s[1].get<float>()};
    }
    if (j.contains("color")) {
      auto& c = j["color"];
      color = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>(), c[3].get<float>()};
    }
    if (j.contains("space")) {
      space = static_cast<SpriteSpace>(j["space"].get<int>());
    } else if (j.contains("behindModel")) {
      // 旧フォーマット互換
      space = j["behindModel"].get<bool>() ? SpriteSpace::ScreenBehind
                                           : SpriteSpace::Screen;
    }
  }
};
