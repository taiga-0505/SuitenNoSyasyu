#pragma once
#include "GraphicsPipeline/GraphicsPipeline.h"
#include "ShaderCompiler/ShaderCompiler.h"

#include <d3d12.h>
#include <dxcapi.h>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <filesystem>

/// @enum InputLayoutType
/// @brief 入力レイアウトの事前定義タイプ
enum class InputLayoutType {
  Object3D,      ///< 標準的な 3D オブジェクト（Pos, UV, Normal）
  Object3DSkin,  ///< スキニング付き 3D オブジェクト（Pos, UV, Normal, BlendIndices, BlendWeight）
  Sprite,        ///< 2D スプライト（Pos, UV）
  Font,          ///< 文字描画（Pos, UV, Color）※頂点カラー付き
  Particle,      ///< パーティクル（Pos, UV, Normal）
  Primitive3D,   ///< 3D プリミティブ（Pos, Color）
  None,          ///< 入力レイアウトなし（SV_VertexID 等を使用する場合）
};

/// @struct PipelineDesc
/// @brief パイプライン作成に必要な情報をまとめる構造体
struct PipelineDesc {
  // シェーダーファイル
  std::wstring vsPath; ///< 頂点シェーダーのファイルパス
  std::wstring psPath; ///< ピクセルシェーダーのファイルパス

  // コンパイル設定
  std::wstring vsEntry = L"main"; ///< 頂点シェーダーのエントリポイント
  std::wstring psEntry = L"main"; ///< ピクセルシェーダーのエントリポイント
  std::wstring vsTarget = L"vs_6_0"; ///< 頂点シェーダーのターゲットプロファイル
  std::wstring psTarget = L"ps_6_0"; ///< ピクセルシェーダーのターゲットプロファイル
  bool optimize = true;  ///< 最適化を有効にするか
  bool debugInfo = false; ///< デバッグ情報を付与するか

  // 入力レイアウト
  std::vector<D3D12_INPUT_ELEMENT_DESC> inputLayout; ///< 入力レイアウト定義（実体保持）

  // ルートシグネチャ・ブレンド・深度・ラスタライザなど
  GPipelineOptions opt{}; ///< パイプラインの詳細設定
};

/// @class PipelineManager
/// @brief GraphicsPipeline の生成・登録・管理・再構築を行うマネージャクラス
/// @details シェーダーのコンパイル結果や PSO のキャッシュ管理も担当する。
class PipelineManager {
public:
  /// @brief 初期化する（シェーダーコンパイラも初期化する）
  /// @param device DirectX12 デバイス
  /// @param rtvFmt 既定のレンダーターゲットフォーマット
  /// @param dsvFmt 既定の深度ステンシルフォーマット
  /// @note シェーダーコンパイラの初期化に失敗した場合は Log::Fatal で停止する。
  void Init(ID3D12Device *device, DXGI_FORMAT rtvFmt, DXGI_FORMAT dsvFmt);

  /// @brief 登録済みのパイプラインをすべて解放する
  void Term();

  /// @brief 指定したキー名でパイプラインを作成・登録する
  /// @param key  登録名
  /// @param desc パイプライン設定
  /// @return 生成された GraphicsPipeline へのポインタ
  /// @note PSO キャッシュが有効（シェーダーファイルの更新日時が一致）ならコンパイルを省く。
  ///       シェーダーのコンパイルに失敗した場合は Log::Fatal で停止する。
  GraphicsPipeline *Create(const std::string &key, const PipelineDesc &desc);

  /// @brief 簡易的なファイル指定によるパイプラインの作成・登録
  /// @param key        登録名
  /// @param vsPath     頂点シェーダーパス
  /// @param psPath     ピクセルシェーダーパス
  /// @param layoutType 事前定義の入力レイアウトタイプ
  /// @param opt        パイプライン設定
  /// @return 生成された GraphicsPipeline へのポインタ
  /// @note Debug ビルドでは最適化無し・デバッグ情報付きでコンパイルする。
  GraphicsPipeline *CreateFromFiles(const std::string &key,
                                    const std::wstring &vsPath,
                                    const std::wstring &psPath,
                                    InputLayoutType layoutType,
                                    const GPipelineOptions &opt);

  /// @brief 登録済みのパイプラインを取得する
  /// @param key 登録名
  /// @return 見つかった場合は GraphicsPipeline へのポインタ、なければ nullptr
  GraphicsPipeline *Get(const std::string &key);

  /// @brief 指定したパイプラインを再ビルドする（シェーダーのホットリロード等）
  /// @param key 登録名
  /// @return 成功すれば true
  /// @note コンパイルに失敗した場合はログを出して false を返し、既存のパイプラインを維持する。
  bool Rebuild(const std::string &key);

  /// @brief 登録済みのすべてのパイプラインを再ビルドする
  void RebuildAll();

  /// @brief エンジン既定のパイプライン群を一括登録する
  /// @note 登録の前に PSO キャッシュを読み込み、登録後に保存する。
  void RegisterDefaultPipelines();

  /// @brief パイプラインキャッシュファイルをロードする
  /// @param filePath キャッシュファイルのパス（Chaso::ResolvePath で解決する）
  /// @note ファイルが無い場合はログを出して何もしない（初回起動時は正常）。
  void LoadCache(const std::string &filePath);

  /// @brief パイプラインキャッシュファイルを保存する
  /// @param filePath キャッシュファイルのパス
  /// @note キャッシュが更新されていない場合は何もしない。
  void SaveCache(const std::string &filePath);

  /// @brief コンピュートパイプラインを作成・登録する
  /// @param key      登録名
  /// @param csPath   コンピュートシェーダーのファイルパス
  /// @param rootType ルートシグネチャの種類
  /// @note コンパイルに失敗した場合は Log::Fatal で停止する。
  ///       ルートシグネチャや PSO の生成に失敗した場合はログを出して登録しない。
  void CreateCompute(const std::string &key, const std::wstring &csPath,
                     RootSignatureType rootType);

  /// @brief 登録済みのコンピュート PSO を取得する
  /// @param key 登録名
  /// @return PSO へのポインタ（見つからなければ nullptr）
  ID3D12PipelineState *GetComputePSO(const std::string &key);

  /// @brief 登録済みのコンピュート用ルートシグネチャを取得する
  /// @param key 登録名
  /// @return ルートシグネチャへのポインタ（見つからなければ nullptr）
  ID3D12RootSignature *GetComputeRoot(const std::string &key);

  /// @brief 接頭辞とブレンドモードを組み合わせて一意なキーを生成するヘルパー関数
  /// @param prefix 接頭辞（例: "Object3D"）
  /// @param mode   ブレンドモード
  /// @return 合成されたキー文字列（「接頭辞.ブレンド名」の形）
  static std::string MakeKey(std::string_view prefix, BlendMode mode);

private:
  /// @brief 入力レイアウト定義を生成する内部関数
  /// @param type 事前定義の入力レイアウトタイプ
  /// @return 入力レイアウト定義（None なら空）
  static std::vector<D3D12_INPUT_ELEMENT_DESC>
  MakeInputLayout(InputLayoutType type);

  /// @brief バイナリデータからパイプラインを構築する内部関数
  /// @param key       登録名
  /// @param desc      パイプライン設定
  /// @param vs        コンパイル済み VS（nullptr 可）
  /// @param ps        コンパイル済み PS（nullptr 可）
  /// @param cachedPSO キャッシュされた PSO データ
  /// @return 生成された GraphicsPipeline へのポインタ
  /// @note キャッシュ無しで作成した場合は、生成結果を PSO キャッシュに記録する。
  GraphicsPipeline *createFromBlobs_(const std::string &key,
                                     const PipelineDesc &desc, IDxcBlob *vs,
                                     IDxcBlob *ps,
                                     const D3D12_CACHED_PIPELINE_STATE &cachedPSO);

  /// @brief PSO およびシェーダーのキャッシュ情報
  struct PsoCache {
    std::vector<uint8_t> psoData; ///< PSO キャッシュバイナリ
    std::vector<uint8_t> vsData;  ///< VS バイナリ
    std::vector<uint8_t> psData;  ///< PS バイナリ
    int64_t vsTime = 0; ///< VS ファイルの最終更新日時
    int64_t psTime = 0; ///< PS ファイルの最終更新日時
  };

  /// @brief パイプライン設定から一意なキャッシュキーを生成する
  /// @param key  登録名
  /// @param desc パイプライン設定（現状は未使用）
  /// @return キャッシュキー（現状は登録名そのまま）
  static std::string makePsoCacheKey_(const std::string &key, const PipelineDesc &desc);

private:
  /// @brief 登録済みパイプラインのエントリ
  struct Entry {
    PipelineDesc desc; ///< 作成時の設定
    std::unique_ptr<GraphicsPipeline> pipeline; ///< パイプライン本体
  };

  /// @brief コンピュートパイプラインのエントリ
  struct ComputeEntry {
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root; ///< ルートシグネチャ
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;  ///< パイプラインステート
  };

  ID3D12Device *device_ = nullptr; ///< デバイス（非所有）
  DXGI_FORMAT rtvFmt_{DXGI_FORMAT_R8G8B8A8_UNORM_SRGB}; ///< 既定の RTV フォーマット
  DXGI_FORMAT dsvFmt_{DXGI_FORMAT_D24_UNORM_S8_UINT};   ///< 既定の DSV フォーマット

  ShaderCompiler compiler_; ///< シェーダーコンパイラ
  std::unordered_map<std::string, Entry> pipelines_; ///< 登録済みパイプラインマップ
  std::unordered_map<std::string, ComputeEntry> computePipelines_; ///< コンピュートパイプライン
  std::unordered_map<std::string, PsoCache> psoCache_; ///< PSO キャッシュマップ
  bool cacheUpdated_ = false; ///< キャッシュが更新されたかどうかのフラグ
};
