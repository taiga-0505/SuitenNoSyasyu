#pragma once

#include "IComponent.h"
#include <string>
#include <nlohmann/json.hpp>
#include "Math/MathTypes.h"

/// @enum PrimitiveType
/// @brief プリミティブメッシュの形状
enum class PrimitiveType {
  Sphere,   ///< 球
  Box,      ///< 箱
  Plane,    ///< 平面
  Cylinder, ///< 円柱
  Cone,     ///< 円錐
  Torus,    ///< トーラス
  Capsule,  ///< カプセル
};

/// @enum PrimitiveDrawStyle
/// @brief プリミティブの描画方法
/// @details 以前は DataDrivenScene がエンティティ名（"PlayerBullet" など）で
///          描画関数を切り替えていた。名前はゲーム側の都合なので、コンポーネントの値で持つ。
enum class PrimitiveDrawStyle {
  Default = 0,     ///< 通常の DrawPrimitiveMesh
  Water = 1,       ///< 水の質感（DrawPrimitiveMeshWater）。水弾・しぶきなど
  WaterColumn = 2, ///< 水柱（DrawPrimitiveMeshWaterColumn）
};

/// @class PrimitiveMeshComponent
/// @brief プリミティブメッシュを描画するコンポーネント
/// @details 形状の種類とサイズのパラメータを持ち、エンティティのループで自動的に描画される。
class PrimitiveMeshComponent : public IComponent {
public:
  int meshHandle = -1;    ///< メッシュハンドル（RC::Generate* の戻り値）
  int texOverride = -1;   ///< 差し替えるテクスチャのハンドル（-1 なら既定のテクスチャ）
  int normalMapOverride = -1; ///< 差し替える法線マップのハンドル
  int roughnessMapOverride = -1; ///< 差し替えるラフネスマップのハンドル
  bool visible = true;    ///< 表示するか
  PrimitiveType type = PrimitiveType::Sphere; ///< 形状の種類（Inspector 用）
  RC::Vector4 color = {1.0f, 1.0f, 1.0f, 1.0f}; ///< 乗算色
  float environmentCoeff = 0.0f; ///< 環境マップの映り込み係数
  PrimitiveDrawStyle drawStyle = PrimitiveDrawStyle::Default; ///< 描画方法

  // --- マテリアル設定（初期化時に GPU 側の Material へ反映する） ---
  // 既定値は PrimitiveMesh::Initialize() の GPU 側 Material 初期値と一致させる
  int lightingMode = -1;         ///< -1: DirectionalLight の設定に従う / 0: なし 1: Lambert 2: Half Lambert
  float shininess = 32.0f;       ///< 鏡面反射の鋭さ
  RC::Vector2 uvTiling = {1.0f, 1.0f};  ///< UV の繰り返し数（uvTransform のスケール）
  RC::Vector2 uvOffset = {0.0f, 0.0f};  ///< UV のずらし量（uvTransform の平行移動）

  /// @brief 有効なメッシュが設定されているかを返す
  /// @return meshHandle が 0 以上なら true
  bool HasMesh() const { return meshHandle >= 0; }

  std::string texturePath; ///< シリアライズ用のテクスチャのアセットパス
  std::string normalMapPath; ///< シリアライズ用の法線マップのアセットパス
  std::string roughnessMapPath; ///< シリアライズ用のラフネスマップのアセットパス

  /// @brief コンポーネントの型名を返す
  /// @return "PrimitiveMeshComponent"
  const char* TypeName() const override { return "PrimitiveMeshComponent"; }

  /// @brief 形状・アセットパス・マテリアル設定を JSON に書き出す
  /// @return type / 各アセットパス / 描画・マテリアル設定 / drawStyle を格納した JSON
  nlohmann::json Serialize() const override {
    return {
      {"type", static_cast<int>(type)},
      {"texturePath", texturePath},
      {"normalMapPath", normalMapPath},
      {"roughnessMapPath", roughnessMapPath},
      {"visible", visible},
      {"color", {color.x, color.y, color.z, color.w}},
      {"environmentCoeff", environmentCoeff},
      {"lightingMode", lightingMode},
      {"shininess", shininess},
      {"uvTiling", {uvTiling.x, uvTiling.y}},
      {"uvOffset", {uvOffset.x, uvOffset.y}},
      {"drawStyle", static_cast<int>(drawStyle)}
    };
  }

  /// @brief JSON から形状・アセットパス・マテリアル設定を読み込む
  /// @param j 読み込み元の JSON（含まれていないキーは現在の値のまま）
  void Deserialize(const nlohmann::json& j) override {
    if (j.contains("type")) type = static_cast<PrimitiveType>(j["type"].get<int>());
    if (j.contains("texturePath")) texturePath = j["texturePath"].get<std::string>();
    if (j.contains("normalMapPath")) normalMapPath = j["normalMapPath"].get<std::string>();
    if (j.contains("roughnessMapPath")) roughnessMapPath = j["roughnessMapPath"].get<std::string>();
    if (j.contains("visible")) visible = j["visible"].get<bool>();
    if (j.contains("color")) {
      auto& c = j["color"];
      color = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>(), c[3].get<float>()};
    }
    if (j.contains("environmentCoeff")) environmentCoeff = j["environmentCoeff"].get<float>();
    if (j.contains("lightingMode")) lightingMode = j["lightingMode"].get<int>();
    if (j.contains("shininess")) shininess = j["shininess"].get<float>();
    if (j.contains("uvTiling")) {
      auto& t = j["uvTiling"];
      uvTiling = {t[0].get<float>(), t[1].get<float>()};
    }
    if (j.contains("uvOffset")) {
      auto& o = j["uvOffset"];
      uvOffset = {o[0].get<float>(), o[1].get<float>()};
    }
    if (j.contains("drawStyle")) drawStyle = static_cast<PrimitiveDrawStyle>(j["drawStyle"].get<int>());
  }
};
