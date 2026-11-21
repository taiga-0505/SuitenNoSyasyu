#pragma once

#include "IComponent.h"
#include "Font/TextMeshGenerator.h"
#include "Math/MathTypes.h"
#include "struct.h" // TextAlign
#include <nlohmann/json.hpp>
#include <string>

/// @class TextMeshComponent
/// @brief 3D 空間に押し出し文字（3D 文字メッシュ）を描画するコンポーネント
/// @details
/// - TextRendererComponent（スクリーン座標の 2D 文字）とは別物で、
///   TransformComponent の位置・回転・拡縮がそのまま効く 3D オブジェクト。
/// - 生成されるメッシュは PrimitiveMesh なので、ライティング・影・テクスチャ・
///   法線マップ等は PrimitiveMeshComponent と同じものが使える。
/// - text / fontPath / size / depth などの形状パラメータを変えると、
///   次のフレームで自動的にメッシュが再生成される（NeedsRebuild 参照）。
/// - 表面（読める面）は -Z を向き、1 行目のベースラインが原点になる。
class TextMeshComponent : public IComponent {
public:
  // ---- 形状（シリアライズ対象、変更でメッシュ再生成） ----
  std::string text = "Text";                                   ///< 表示文字列（UTF-8、'\n' で改行）
  std::string fontPath = "Resources/fonts/Kiwi_Maru/KiwiMaru-Regular.ttf"; ///< フォントファイル
  float size = 1.0f;                ///< 1em の高さ（ワールド単位）
  float depth = 0.2f;               ///< 押し出し厚さ（ワールド単位、0 で板ポリ）
  float curveTolerance = 0.005f;    ///< 曲線の許容誤差（em 比。小さいほど滑らか・頂点増）
  TextAlign align = TextAlign::Center; ///< 水平揃え（原点を基準にする）
  float lineSpacing = 1.0f;         ///< 行送り倍率

  // ---- 見た目（シリアライズ対象、マテリアルへ反映） ----
  bool visible = true;                          ///< 表示フラグ
  RC::Vector4 color = {1.0f, 1.0f, 1.0f, 1.0f}; ///< 乗算色
  int lightingMode = -1;                        ///< -1: DirectionalLight に追従 / 0: なし 1: Lambert 2: Half Lambert
  float shininess = 32.0f;                      ///< 光沢
  float environmentCoeff = 0.0f;                ///< 環境マップ映り込み係数
  RC::Vector2 uvTiling = {1.0f, 1.0f};          ///< UV タイリング
  RC::Vector2 uvOffset = {0.0f, 0.0f};          ///< UV オフセット
  std::string texturePath;                      ///< テクスチャ
  std::string normalMapPath;                    ///< 法線マップ
  std::string roughnessMapPath;                 ///< 粗さマップ

  // ---- 縁取り（アウトライン）。有効/太さの変更でシェルメッシュ再生成、色は即時反映 ----
  bool outlineEnabled = false;                        ///< 縁取りを描くか
  float outlineWidth = 0.05f;                         ///< 縁取りの太さ（em 比。0.05 = 1em の 5%）
  RC::Vector4 outlineColor = {0.0f, 0.0f, 0.0f, 1.0f}; ///< 縁取り色
  bool outlineUnlit = true;                           ///< true: 単色（ライティングなし）/ false: 本体と同じライティング

  // ---- ランタイム（保存しない） ----
  int meshHandle = -1;           ///< PrimitiveMesh ハンドル
  int outlineMeshHandle = -1;    ///< 縁取りシェルの PrimitiveMesh ハンドル（無効時は -1）
  int texOverride = -1;          ///< テクスチャハンドル
  int normalMapOverride = -1;    ///< 法線マップハンドル
  int roughnessMapOverride = -1; ///< 粗さマップハンドル
  bool built = false;            ///< 一度でも生成を試みたか
  RC::TextMeshDesc builtDesc;    ///< 現在のメッシュを生成したときのパラメータ
  bool builtOutlineEnabled = false; ///< 生成時の outlineEnabled
  float builtOutlineWidth = 0.0f;   ///< 生成時の outlineWidth
  RC::TextMeshInfo info;         ///< 生成されたメッシュのローカル AABB・頂点数（Inspector 表示用）
  RC::TextMeshInfo outlineInfo;  ///< 縁取りシェルの AABB・頂点数

  /// @brief 本体メッシュが生成済みかを返す
  /// @return meshHandle が 0 以上なら true
  bool HasMesh() const { return meshHandle >= 0; }

  /// @brief 縁取りシェルのメッシュが生成済みかを返す
  /// @return outlineMeshHandle が 0 以上なら true
  bool HasOutlineMesh() const { return outlineMeshHandle >= 0; }

  /// @brief 現在の設定から生成パラメータを組み立てる
  /// @return 形状パラメータを詰めた TextMeshDesc
  RC::TextMeshDesc MakeDesc() const {
    RC::TextMeshDesc d;
    d.text = text;
    d.fontPath = fontPath;
    d.size = size;
    d.depth = depth;
    d.curveTolerance = curveTolerance;
    d.align = align;
    d.lineSpacing = lineSpacing;
    return d;
  }

  /// @brief 本体メッシュの設定と生成済みメッシュが食い違っているか（未生成なら true）
  /// @return 本体メッシュの再生成が必要なら true
  bool NeedsRebuild() const { return !built || !(MakeDesc() == builtDesc); }

  /// @brief 縁取りシェルだけ作り直せばよい状態か（有効/無効・太さの変更）
  /// @return 縁取りシェルの再生成が必要なら true
  bool NeedsOutlineRebuild() const {
    return outlineEnabled != builtOutlineEnabled || (outlineEnabled && outlineWidth != builtOutlineWidth);
  }

  /// @brief コンポーネントの型名を返す
  /// @return "TextMeshComponent"
  const char *TypeName() const override { return "TextMeshComponent"; }

  /// @brief 形状・見た目・縁取りの設定を JSON に書き出す
  /// @return シリアライズ対象のメンバをすべて格納した JSON（ランタイムの値は含まない）
  nlohmann::json Serialize() const override {
    return {
        {"text", text},
        {"fontPath", fontPath},
        {"size", size},
        {"depth", depth},
        {"curveTolerance", curveTolerance},
        {"align", static_cast<int>(align)},
        {"lineSpacing", lineSpacing},
        {"visible", visible},
        {"color", {color.x, color.y, color.z, color.w}},
        {"lightingMode", lightingMode},
        {"shininess", shininess},
        {"environmentCoeff", environmentCoeff},
        {"uvTiling", {uvTiling.x, uvTiling.y}},
        {"uvOffset", {uvOffset.x, uvOffset.y}},
        {"texturePath", texturePath},
        {"normalMapPath", normalMapPath},
        {"roughnessMapPath", roughnessMapPath},
        {"outlineEnabled", outlineEnabled},
        {"outlineWidth", outlineWidth},
        {"outlineColor", {outlineColor.x, outlineColor.y, outlineColor.z, outlineColor.w}},
        {"outlineUnlit", outlineUnlit},
    };
  }

  /// @brief JSON から形状・見た目・縁取りの設定を読み込む
  /// @param j 読み込み元の JSON（含まれていないキーは現在の値のまま）
  /// @note align が範囲外（0〜2 以外）なら Center にする。
  void Deserialize(const nlohmann::json &j) override {
    if (j.contains("text")) text = j["text"].get<std::string>();
    if (j.contains("fontPath")) fontPath = j["fontPath"].get<std::string>();
    if (j.contains("size")) size = j["size"].get<float>();
    if (j.contains("depth")) depth = j["depth"].get<float>();
    if (j.contains("curveTolerance")) curveTolerance = j["curveTolerance"].get<float>();
    if (j.contains("align")) {
      const int a = j["align"].get<int>();
      align = (a >= 0 && a <= 2) ? static_cast<TextAlign>(a) : TextAlign::Center;
    }
    if (j.contains("lineSpacing")) lineSpacing = j["lineSpacing"].get<float>();
    if (j.contains("visible")) visible = j["visible"].get<bool>();
    if (j.contains("color")) {
      auto &c = j["color"];
      color = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>(), c[3].get<float>()};
    }
    if (j.contains("lightingMode")) lightingMode = j["lightingMode"].get<int>();
    if (j.contains("shininess")) shininess = j["shininess"].get<float>();
    if (j.contains("environmentCoeff")) environmentCoeff = j["environmentCoeff"].get<float>();
    if (j.contains("uvTiling")) {
      auto &t = j["uvTiling"];
      uvTiling = {t[0].get<float>(), t[1].get<float>()};
    }
    if (j.contains("uvOffset")) {
      auto &o = j["uvOffset"];
      uvOffset = {o[0].get<float>(), o[1].get<float>()};
    }
    if (j.contains("texturePath")) texturePath = j["texturePath"].get<std::string>();
    if (j.contains("normalMapPath")) normalMapPath = j["normalMapPath"].get<std::string>();
    if (j.contains("roughnessMapPath")) roughnessMapPath = j["roughnessMapPath"].get<std::string>();
    if (j.contains("outlineEnabled")) outlineEnabled = j["outlineEnabled"].get<bool>();
    if (j.contains("outlineWidth")) outlineWidth = j["outlineWidth"].get<float>();
    if (j.contains("outlineColor")) {
      auto &c = j["outlineColor"];
      outlineColor = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>(), c[3].get<float>()};
    }
    if (j.contains("outlineUnlit")) outlineUnlit = j["outlineUnlit"].get<bool>();
  }
};
