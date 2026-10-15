#pragma once
#include "struct.h"
#include "Math/MathTypes.h"
#include <cstdint>
#include <string>

namespace RC {

/// @struct TextMeshDesc
/// @brief 3D 文字メッシュ（押し出し文字）の生成パラメータ
/// @details
/// - 文字は XY 平面に配置され、Z 方向へ押し出される。
/// - 表面（読める面）の法線は -Z（カメラが -Z 側から +Z を見る既定構成で正しく読める向き）。
/// - 厚さは Z=0 を中心に前後 depth/2 ずつ振り分ける。
/// - 1 行目のベースラインが Y=0、align で指定した基準点が X=0 になる。
struct TextMeshDesc {
  std::string text;                  ///< 表示文字列（UTF-8、'\n' で改行）
  std::string fontPath;              ///< フォントファイル（.ttf / .otf / .ttc）
  uint32_t faceIndex = 0;            ///< TTC 等でフェイスが複数ある場合のインデックス
  float size = 1.0f;                 ///< 1em の高さ（ワールド単位）
  float depth = 0.2f;                ///< 押し出し厚さ（ワールド単位、0 なら板ポリ）
  float curveTolerance = 0.005f;     ///< 曲線を折れ線化する際の許容誤差（em 比、小さいほど滑らか・重い）
  TextAlign align = TextAlign::Left; ///< 水平揃え（X=0 を基準点とする）
  float lineSpacing = 1.0f;          ///< 行送り倍率

  /// @brief 全パラメータの一致比較（再生成の要否判定に使う）
  /// @return 全メンバが等しければ true
  bool operator==(const TextMeshDesc &) const = default;
};

/// @struct TextMeshInfo
/// @brief 生成された文字メッシュの情報（ローカル AABB と規模）
struct TextMeshInfo {
  Vector3 min{0.0f, 0.0f, 0.0f}; ///< AABB 最小
  Vector3 max{0.0f, 0.0f, 0.0f}; ///< AABB 最大
  uint32_t vertexCount = 0;      ///< 頂点数
  uint32_t triangleCount = 0;    ///< 三角形数
};

/// @class TextMeshGenerator
/// @brief DirectWrite のグリフアウトラインから押し出し 3D 文字メッシュ（ModelData）を生成する
/// @details
/// - アウトライン取得は IDWriteFontFace::GetGlyphRunOutline、
///   曲線の平坦化・自己交差除去・三角形分割は Direct2D のジオメトリ演算
///   （ID2D1Geometry::Outline / Tessellate / Simplify）で行う。
///   デバイスやレンダーターゲットは作らないため d2d1.dll のファクトリのみに依存する。
/// - 日本語フォントの重なり合うストロークも Outline で 1 つの輪郭に統合されるため、
///   側面が内部に生じることはない。
/// - UV: 表面・裏面は文字列全体の AABB に 0〜1 で平面投影（上が v=0）、
///   側面は輪郭に沿った周長を u（1em = 1.0）、厚さ方向を v（表面側 0 → 裏面側 1）とする。
/// - 法線は面ごとにフラット（表・裏・各側面クアッドで独立頂点）。
/// - 生成結果はそのまま PrimitiveMesh::Initialize() に渡せる（RC::GenerateTextMesh 参照）。
/// - フォントフェイスと DirectWrite / Direct2D ファクトリは内部でキャッシュされる。
class TextMeshGenerator {
public:
  /// @brief 文字メッシュを生成する
  /// @param desc    生成パラメータ
  /// @param out     出力先モデルデータ（クリアしてから書き込む）
  /// @param outInfo バウンディングボックス（不要なら nullptr）
  /// @return 1 つ以上の三角形が生成できたら true（フォントが開けない・空文字・空白のみ等は false）
  static bool Generate(const TextMeshDesc &desc, ModelData &out,
                       TextMeshInfo *outInfo = nullptr);

  /// @brief 縁取り（アウトライン）用のシェルメッシュを生成する
  /// @param desc         本体と同じ生成パラメータ
  /// @param outlineWidth 縁取りの太さ（em 比。例: 0.05 で 1em の 5%）
  /// @param out          出力先モデルデータ
  /// @param outInfo      AABB・頂点数（不要なら nullptr）
  /// @return 生成できたら true
  /// @details 本体の形状を outlineWidth だけ外側へ太らせた（丸角）形を、本体より僅かに
  ///          奥まった表裏面で押し出したもの。本体の後ろに重ねて描くと、正面からは文字の
  ///          周囲に幅 outlineWidth の縁が、横からは縁取り色の側面が見える。
  ///          単色・非ライティングで描く想定（RC::GenerateTextMeshOutline 参照）。
  static bool GenerateOutline(const TextMeshDesc &desc, float outlineWidth, ModelData &out,
                              TextMeshInfo *outInfo = nullptr);

  /// @brief キャッシュしているフォントフェイスを全て解放する（終了時用）
  static void ClearCache();
};

} // namespace RC
