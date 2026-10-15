#pragma once
#include "Math/Math.h"
#include "Math/MathTypes.h"
#include "Math/BoundingBox.h"
#include "function/function.h"
#include "struct.h"
#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <d3d12.h>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>
#include <wrl/client.h>

/// @class ModelMesh
/// @brief 3D モデルデータを保持し、頂点バッファやマテリアル情報を管理するクラス
/// @details Assimp を使用して .obj, .gltf, .glb 等の各種モデルファイルを読み込む。
///          全てのメッシュを 1 つの頂点バッファに統合し、ノード階層を解析して描画単位（DrawItem）に展開する。
class ModelMesh {
public:
  /// @struct DrawItem
  /// @brief ノード階層を反映した描画単位の構造体
  struct DrawItem {
    uint32_t vertexStart = 0;     ///< 頂点バッファ上の開始頂点インデックス（BaseVertex）
    uint32_t vertexCount = 0;     ///< 頂点数
    uint32_t indexStart = 0;      ///< インデックスバッファ上の開始インデックス
    uint32_t indexCount = 0;      ///< 描画するインデックス数
    uint32_t materialIndex = 0;   ///< 使用するマテリアルのインデックス
    uint32_t meshIndex = 0;       ///< 元のメッシュインデックス（デバッグ用）
    RC::Matrix4x4 nodeWorld = {}; ///< モデル空間での累積行列（ルートからの変換）
    std::string nodeName;         ///< ノード名（デバッグ用）
  };

  /// @struct SkinData
  /// @brief スキニング情報を保持する構造体
  struct SkinData {
    std::unordered_map<std::string, uint32_t> jointNameToIndex; ///< ジョイント名 → ボーンインデックス
    std::vector<RC::Matrix4x4> inverseBindPoseMatrices;         ///< 各ボーンの InverseBindPoseMatrix
  };

  /// @brief コンストラクタ（LoadModel を呼ぶまでは空）
  ModelMesh() = default;
  /// @brief デストラクタ
  ~ModelMesh();

  /// @brief モデルファイルを読み込む
  /// @param device    D3D12 デバイス
  /// @param modelPath ファイルパス、またはモデルが含まれるディレクトリパス
  /// @return 読み込みに成功すれば true
  /// @note ディレクトリを渡した場合は .gltf → .glb → .obj の順で最初に見つかったファイルを読む。
  bool LoadModel(ID3D12Device *device, const std::string &modelPath);

  /// @brief OBJ ファイルを読み込む（LoadModel の互換用）
  /// @param device D3D12 デバイス
  /// @param path   ファイルパス
  /// @return 読み込みに成功すれば true
  bool LoadObj(ID3D12Device *device, const std::string &path) {
    return LoadModel(device, path);
  }

  /// @brief UV 座標が存在しない場合に、簡易的な球面 UV を生成して適用する
  /// @note 全頂点の UV が (0, 0) のときだけ、頂点バッファを直接書き換える。
  void EnsureSphericalUVIfMissing();

  /// @brief モデルデータの準備が完了しているか（バッファが生成済みか）確認する
  /// @return 準備完了なら true
  bool Ready() const { return vb_.resource != nullptr && vb_.vertexCount > 0; }

  /// @brief 頂点バッファビューを取得する
  /// @return 頂点バッファビュー
  const D3D12_VERTEX_BUFFER_VIEW &VBV() const { return vb_.view; }

  /// @brief 総頂点数を取得する
  /// @return 総頂点数
  uint32_t VertexCount() const { return vb_.vertexCount; }

  /// @brief インデックスバッファが存在するか
  /// @return 存在すれば true
  bool HasIndexBuffer() const { return ib_.resource != nullptr && ib_.indexCount > 0; }

  /// @brief インデックスバッファビューを取得する
  /// @return インデックスバッファビュー
  const D3D12_INDEX_BUFFER_VIEW &IBV() const { return ib_.view; }

  /// @brief 総インデックス数を取得する
  /// @return 総インデックス数
  uint32_t IndexCount() const { return ib_.indexCount; }

  /// @brief 頂点バッファの GPU リソースを取得する（CS スキニング用）
  /// @return GPU リソースへのポインタ
  ID3D12Resource *GetVBResource() const { return vb_.resource.Get(); }

  /// @brief 頂点 1 つあたりのバイト数を取得する
  /// @return sizeof(VertexData)
  uint32_t GetVertexStrideBytes() const { return static_cast<uint32_t>(sizeof(VertexData)); }

  /// @brief スキンデータが存在するか
  /// @return ボーンが 1 つ以上あれば true
  bool HasSkinData() const { return !skinData_.inverseBindPoseMatrices.empty(); }

  /// @brief スキンデータを取得する
  /// @return スキンデータへの参照
  const SkinData &GetSkinData() const { return skinData_; }

  /// @brief 代表的なマテリアル情報を取得する（互換用）
  /// @return マテリアルデータ
  const MaterialData &MaterialFile() const { return materialFile_; }

  /// @brief 全てのマテリアルリストを取得する
  /// @return マテリアルデータの配列
  const std::vector<MaterialData> &Materials() const { return materials_; }

  /// @brief モデルのルートノードを取得する
  /// @return ルートノードへの参照
  const Node &RootNode() const { return rootNode_; }

  /// @brief 全ての描画項目リストを取得する
  /// @return 描画項目の配列
  const std::vector<DrawItem> &DrawItems() const { return drawItems_; }

  /// @brief モデル空間の境界箱（DrawItem の nodeWorld を反映済み）。カリング用
  /// @details スキニングモデルはバインドポーズでの値なので、アニメーション中ははみ出しうる。
  /// @return 境界箱への参照
  const RC::BoundingBox &LocalBounds() const { return localBounds_; }

  /// @brief 全 DrawItem の nodeWorld が単位行列か
  /// @details true なら全描画単位で同じインスタンスデータ（World 行列）を共有できる。
  /// @return 全て単位行列なら true
  bool AllNodeWorldIdentity() const { return allNodeWorldIdentity_; }

  /// @brief 読み込み時に指定された入力パスを取得する
  /// @return 入力パス文字列
  const std::string &SourceInputPath() const {
    return sourceInputPath_;
  }
  
  /// @brief 実際に読み込まれたファイルの絶対パスを取得する
  /// @return ファイルパス文字列
  const std::string &SourceFilePath() const {
    return sourceFilePath_;
  }

private:
  /// @struct VB
  /// @brief 頂点バッファリソースを管理する内部構造体
  struct VB {
    Microsoft::WRL::ComPtr<ID3D12Resource> resource; ///< GPU リソース
    D3D12_VERTEX_BUFFER_VIEW view{};                ///< バッファビュー
    uint32_t vertexCount = 0;                       ///< 頂点数
  };

  /// @struct IB
  /// @brief インデックスバッファリソースを管理する内部構造体
  struct IB {
    Microsoft::WRL::ComPtr<ID3D12Resource> resource; ///< GPU リソース
    D3D12_INDEX_BUFFER_VIEW view{};                 ///< バッファビュー
    uint32_t indexCount = 0;                        ///< インデックス数
  };

  /// @struct SubmeshRange
  /// @brief 各メッシュごとの頂点/インデックス範囲とマテリアルを保持する内部構造体
  struct SubmeshRange {
    uint32_t vertexStart = 0;   ///< 開始頂点（BaseVertex）
    uint32_t vertexCount = 0;   ///< 頂点数
    uint32_t indexStart = 0;    ///< 開始インデックス
    uint32_t indexCount = 0;    ///< インデックス数
    uint32_t materialIndex = 0; ///< マテリアルインデックス
  };

  Microsoft::WRL::ComPtr<ID3D12Device> device_; ///< バッファ作成に使うデバイス
  VB vb_{}; ///< 全メッシュを連結した頂点バッファ
  IB ib_{}; ///< 全メッシュを連結したインデックスバッファ

  MaterialData materialFile_{}; ///< 最初に見つかったテクスチャ情報（互換用）

  std::vector<MaterialData> materials_;  ///< 解析されたマテリアル一覧
  std::vector<SubmeshRange> submeshes_;  ///< メッシュごとの範囲
  std::vector<uint32_t> indices_;        ///< インデックスデータ
  Node rootNode_{};                      ///< 解析されたノード階層のルート
  std::vector<DrawItem> drawItems_;      ///< 最終的な描画項目のリスト
  /// @brief モデル空間の境界箱（カリング用）。未計算のうちは「消えないよう十分大きい箱」
  RC::BoundingBox localBounds_ = RC::BoundingBox::FromCenterHalf({0.0f, 0.0f, 0.0f}, {1.0e4f, 1.0e4f, 1.0e4f});
  bool allNodeWorldIdentity_ = true;     ///< 全 DrawItem の nodeWorld が単位行列か
  SkinData skinData_{};                  ///< スキンデータ

  /// @brief Assimp を使用してファイルを読み込み、GPU バッファまで作成する（内部処理）
  /// @param filePath 読み込むファイルのパス
  /// @return 成功すれば true
  bool LoadAssimp_(const std::string &filePath);

  /// @brief 読み込まれた aiScene から情報を抽出する
  /// @param scene       Assimp のシーン
  /// @param baseDir     モデルファイルのベースディレクトリ
  /// @param outVertices [out] 抽出された頂点データの出力先
  /// @return 頂点が 1 つ以上抽出できれば true
  bool ExtractScene_(const aiScene *scene, const std::string &baseDir,
                     std::vector<VertexData> &outVertices);

  /// @brief aiNode 階層を RC::Node 階層に変換する（再帰）
  /// @param node 変換元のノード（nullptr なら単位行列のノードを返す）
  /// @return 変換後のノード
  Node ReadNode_(const aiNode *node) const;

  /// @brief ノード階層を巡回し、DrawItem を構築する（再帰）
  /// @param node        処理対象のノード
  /// @param parentWorld 親ノードの累積行列
  void BuildDrawItems_(const Node &node, const RC::Matrix4x4 &parentWorld);

  /// @brief マテリアル情報からテクスチャパスを解決する
  /// @param scene   Assimp のシーン
  /// @param mat     対象のマテリアル
  /// @param baseDir モデルファイルのベースディレクトリ（相対パスの基準）
  /// @return テクスチャのパス（見つからなければ空文字列）
  /// @note BASE_COLOR → DIFFUSE → UNKNOWN の順で探す。
  std::string ResolveTexturePath_(const aiScene *scene, const aiMaterial *mat,
                                  const std::string &baseDir) const;

  /// @brief 埋め込みテクスチャを抽出してファイルとして保存する
  /// @param scene         Assimp のシーン
  /// @param assimpTexPath 埋め込みテクスチャの参照（"*0" 形式）
  /// @param baseDir       モデルファイルのベースディレクトリ（_assimp_cache をここに作る）
  /// @return 書き出したファイルのパス（失敗・非対応形式なら空文字列）
  /// @note 圧縮済み（PNG / JPG 等）のみ対応。生の RGBA は空文字列を返す。
  std::string ExtractEmbeddedTexture_(const aiScene *scene,
                                      const aiString &assimpTexPath,
                                      const std::string &baseDir) const;

  /// @brief 頂点データを GPU にアップロードする
  /// @param vertices 頂点配列（空なら何もしない）
  void UploadVB_(const std::vector<VertexData> &vertices);

  /// @brief 頂点と DrawItem から localBounds_ / allNodeWorldIdentity_ を求める
  /// @param vertices 頂点配列
  void ComputeBounds_(const std::vector<VertexData> &vertices);

  /// @brief インデックスデータ（indices_）を GPU にアップロードする
  void UploadIB_();

  /// @brief Assimp のボーンデータからスキン情報を抽出する
  /// @param scene    Assimp のシーン
  /// @param vertices ボーンインデックスとウェイトを書き込む頂点配列
  void ExtractSkinData_(const aiScene *scene, std::vector<VertexData> &vertices);

  /// @brief 実際に使用されているマテリアルだけに整理し、インデックスを詰め直す
  void CompactMaterials_();

  /// @brief 座標系変換（右手 → 左手）用の X 軸反転行列を生成する
  /// @return X 軸を反転する行列
  static RC::Matrix4x4 MakeAxisFlipX_();

  /// @brief 右手系ノード行列を左手系に変換する
  /// @param m 右手系の行列
  /// @return 左手系に変換した行列
  static RC::Matrix4x4 ConvertNodeMatrixRHtoLH_(const RC::Matrix4x4 &m);

  std::string sourceInputPath_; ///< LoadModel に渡された文字列
  std::string sourceFilePath_;  ///< 実際に読んだファイルパス
};
