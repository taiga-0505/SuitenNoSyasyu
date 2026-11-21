#pragma once
#include <cassert>
#include <d3d12.h>
#include <wrl/client.h>
#include <vector>

/// @enum BlendMode
/// @brief ブレンドモードの定義
enum BlendMode {
  kBlendModeNone,           ///< ブレンド無し
  kBlendModeNormal,         ///< 通常のαブレンド (Src * SrcA + Dest * (1 - SrcA))
  kBlendModeAdd,            ///< 加算 (Src * SrcA + Dest * 1)
  kBlendModeSubtract,       ///< 減算 (Dest * 1 - Src * SrcA)
  kBlendModeMultiply,       ///< 乗算 (Src * 0 + Dest * Src)
  kBlendModeScreen,         ///< スクリーン (Src * (1 - Dest) + Dest * 1)
  kBlendModePremultiplied,  ///< アルファ乗算済みブレンド (Src * 1 + Dest * (1 - SrcA))
};

/// @enum RootSignatureType
/// @brief ルートシグネチャの種類
enum class RootSignatureType {
  Object3D,           ///< 3D オブジェクト用
  Object3DInstancing, ///< 3D オブジェクト（インスタンシング）用
  Object3DSkin,       ///< 3D オブジェクト（スキニング）用
  Sprite,             ///< スプライト用

  FogOverlay,         ///< フォグオーバーレイ用
  Primitive3D,        ///< 3D プリミティブ用
  PostProcess,        ///< ポストプロセス用
  SkinningCS,         ///< コンピュートシェーダーでのスキニング用
  GPUParticle,        ///< GPU パーティクル描画用
  InitParticleCS,     ///< GPU パーティクル初期化 CS 用
  UpdateParticleCS,   ///< GPU パーティクル更新 CS 用
  EmitParticleCS,     ///< GPU パーティクル射出 CS 用
  Water,              ///< 水面シェーダー用（Object3D + WaterParams b6）
  WaveSimulationCS,   ///< 波のシミュレーション CS 用
};

/// @brief Object3D 系ルートシグネチャのパラメータ番号
/// @details ルートシグネチャの定義側と描画側のバインドで同じ番号を使うための定数。
namespace Object3DRootParam {
inline constexpr UINT kMaterial = 0;      ///< CBV b0 (PS) マテリアル
inline constexpr UINT kTransform = 1;     ///< CBV b0 (VS) トランスフォーム
inline constexpr UINT kTexture = 2;       ///< SRV table t0 (PS) テクスチャ
inline constexpr UINT kLight = 3;         ///< CBV b1 (PS) 平行光源
inline constexpr UINT kNormalMap = 9;     ///< SRV table t2 (PS) 法線マップ
inline constexpr UINT kRoughnessMap = 10; ///< SRV table t3 (PS) ラフネスマップ
inline constexpr UINT kSkinMatrices = 13; ///< SRV t1 (VS) スキン行列パレット（Object3DSkin のみ）
} // namespace Object3DRootParam

/// @struct GPipelineOptions
/// @brief パイプライン構築時のオプション設定構造体
struct GPipelineOptions {
  bool enableAlphaBlend = false;      ///< アルファブレンドを有効にするか
  bool enableDepth = true;            ///< 深度テストを有効にするか
  bool enableDepthWrite = true;       ///< 深度書き込みを有効にするか
  D3D12_CULL_MODE cull = D3D12_CULL_MODE_BACK; ///< カリングモード
  D3D12_FILL_MODE fill = D3D12_FILL_MODE_SOLID; ///< フィルモード（ソリッド／ワイヤーフレーム）
  BlendMode blendMode = kBlendModeNormal;       ///< ブレンド方式
  RootSignatureType rootType = RootSignatureType::Object3D; ///< ルートシグネチャの形状
  D3D12_PRIMITIVE_TOPOLOGY_TYPE topologyType =
      D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; ///< トポロジの種類（三角形・線・点）
  bool disableRTV = false; ///< レンダーターゲット出力を無効にするか（ShadowMap 等）
  DXGI_FORMAT dsvFormatOverride = DXGI_FORMAT_UNKNOWN; ///< 既定の DSV フォーマットの上書き（UNKNOWN なら上書きしない）
};

/// @class GraphicsPipeline
/// @brief ルートシグネチャとパイプラインステートオブジェクト（PSO）を構築・管理するクラス
class GraphicsPipeline {
public:
  /// @brief 初期化する
  /// @param device DirectX12 デバイス
  void Init(ID3D12Device *device) { device_ = device; }

  /// @brief PSO・ルートシグネチャ・デバイス参照を解放する
  void Term();

  /// @brief 拡張オプション付きで PSO を構築する
  /// @param inputElems 入力レイアウト
  /// @param elemCount  入力レイアウトの要素数
  /// @param vs         VS バイナリ
  /// @param ps         PS バイナリ
  /// @param rtvFmt     RTV フォーマット
  /// @param dsvFmt     DSV フォーマット
  /// @param opt        詳細オプション
  /// @param cachedPSO  キャッシュされた PSO データ（任意）
  /// @note キャッシュ付きで生成に失敗した場合はキャッシュ無しで作り直し、IsCacheFallback が true になる。
  void BuildEx(const D3D12_INPUT_ELEMENT_DESC *inputElems, UINT elemCount,
                D3D12_SHADER_BYTECODE vs, D3D12_SHADER_BYTECODE ps,
                DXGI_FORMAT rtvFmt, DXGI_FORMAT dsvFmt,
                const GPipelineOptions &opt,
                const D3D12_CACHED_PIPELINE_STATE &cachedPSO);

  /// @brief 生成した PSO からキャッシュ用のシリアライズされたバイナリを取得する
  /// @return シリアライズされた PSO データ（PSO 未生成なら nullptr）
  Microsoft::WRL::ComPtr<ID3DBlob> GetSerializedBlob() const;

  /// @brief ルートシグネチャを取得する
  /// @return ルートシグネチャ
  ID3D12RootSignature *Root() const { return root_.Get(); }

  /// @brief パイプラインステートオブジェクト（PSO）を取得する
  /// @return PSO
  ID3D12PipelineState *PSO() const { return pso_.Get(); }

  /// @brief キャッシュの不一致などで再構築（フォールバック）が発生したか
  /// @return 直前の BuildEx でフォールバックしたなら true
  bool IsCacheFallback() const { return isCacheFallback_; }

private:
  /// @brief ルートシグネチャを構築する内部関数
  /// @param type ルートシグネチャの種類（Compute 用の種類は非対応）
  void
  buildRootSignature_(RootSignatureType type = RootSignatureType::Object3D);

private:
  Microsoft::WRL::ComPtr<ID3D12Device> device_;      ///< デバイス
  Microsoft::WRL::ComPtr<ID3D12RootSignature> root_; ///< ルートシグネチャ
  Microsoft::WRL::ComPtr<ID3D12PipelineState> pso_;  ///< パイプラインステートオブジェクト
  bool isCacheFallback_ = false;                     ///< キャッシュフォールバック発生フラグ
};
