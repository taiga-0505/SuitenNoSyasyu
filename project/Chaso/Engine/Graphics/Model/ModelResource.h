#pragma once
#include "Math/Math.h"
#include "Math/MathTypes.h"
#include "ModelMesh.h"
#include "function/function.h"
#include "struct.h"
#include <d3d12.h>
#include <memory>
#include <vector>
#include <wrl/client.h>
#include "ComputeShader/ComputeShader.h"
#include "Render/FrameResource.h" // RC::DynamicCB

class TextureManager; // 前方宣言
class SRVManager;     // 前方宣言
class PipelineManager; // 前方宣言

namespace RC {
class FrameResource; // 前方宣言
}

// ============================================================
// ModelResource
// - GPU リソース（CB, SRV, バッファ）と描画ロジックを管理する
// - ModelObject から分離された「GPU側のデータ」を担当
// - 1つの ModelMesh (共有) をバインドし、Draw/DrawBatch を実行する
//
// 毎フレームの WVP CB / InstanceData は FrameResource (リニア
// アロケータ) から確保するため、CPU/GPU 間のレースコンディション
// が発生しない。
// ============================================================
/// @class ModelResource
/// @brief GPU リソース（定数バッファ、SRV）とモデルの描画ロジックを管理するクラス
/// @details ModelObject から実体データを分離したもので、頂点バッファのバインドやシェーダーへの定数転送、
/// サブメッシュごとの描画実行などを担当します。
/// インスタンスデータやWVP行列の転送には FrameResource（リニアアロケータ）を利用します。
class ModelResource {
public:
  ModelResource() = default;
  ~ModelResource();

  /// @brief GPU リソースの初期化
  /// @param device D3D12 デバイス
  void Initialize(ID3D12Device *device);

  /// @brief 使用する共有メッシュを設定する
  /// @param mesh ModelMesh の shared_ptr
  void SetMesh(const std::shared_ptr<ModelMesh> &mesh);

  /// @brief 設定されているメッシュを取得する
  /// @return ModelMesh の shared_ptr
  const std::shared_ptr<ModelMesh> &GetMesh() const { return mesh_; }

  /// @brief 全サブメッシュ共通で適用するオーバーライドテクスチャを設定する
  /// @param srvGPUHandle テクスチャの GPU ハンドル
  void SetTexture(const D3D12_GPU_DESCRIPTOR_HANDLE &srv) { textureSrv_ = srv; }
  void ResetTextureToMtl();

  void SetNormalMap(const D3D12_GPU_DESCRIPTOR_HANDLE &srv) { normalMapSrv_ = srv; }
  void SetRoughnessMap(const D3D12_GPU_DESCRIPTOR_HANDLE &srv) { roughnessMapSrv_ = srv; }

  /// @brief テクスチャマネージャを設定する
  /// @param tm テクスチャマネージャへのポインタ
  void SetTextureManager(TextureManager *tm) { texman_ = tm; }

  /// @brief マテリアルの定数バッファ（CPUマップ済み）を取得する
  /// @return Material 構造体へのポインタ
  Material *Mat() { return cbMat_.dyn.Ptr(); }
  const Material *Mat() const { return cbMat_.dyn.Ptr(); }

  /// @brief ライトの定数バッファ（CPUマップ済み）を取得する
  /// @return DirectionalLight 構造体へのポインタ
  DirectionalLight *Light() { return cbLight_.dyn.Ptr(); }
  const DirectionalLight *Light() const { return cbLight_.dyn.Ptr(); }

  /// @brief 外部で管理されているライトCBのアドレスを設定する
  /// @param addr ライトCBの GPU 仮想アドレス
  void SetExternalLightCBAddress(D3D12_GPU_VIRTUAL_ADDRESS addr) {
    externalLightCBAddress_ = addr;
  }

  /// @brief 外部ライトCBのアドレスを取得する
  /// @return アドレス値
  D3D12_GPU_VIRTUAL_ADDRESS GetExternalLightCBAddress() const {
    return externalLightCBAddress_;
  }

  /// @brief ライティングの初期パラメータを適用する
  /// @param lightingMode ライティングモード
  /// @param color ライトカラー（RGB）
  /// @param dir ライト方向（XYZ）
  /// @param intensity ライト強度
  void ApplyLighting(int lightingMode, const float color[3],
                     const float dir[3], float intensity);

  /// @brief ワールド行列を指定してモデルを描画する
  /// @param cmdList コマンドリスト
  /// @param world ワールド行列
  /// @param view ビュー行列
  /// @param proj プロジェクション行列
  /// @param frame フレームリソース（動的CB確保用）
  /// @param worldOnly true なら World 行列だけを転送し、WVP / WorldInverseTranspose の計算を省く
  ///        （シャドウパスの VS は World しか読まないため。結果は変わらない）
  void Draw(ID3D12GraphicsCommandList *cmdList, const RC::Matrix4x4 &world,
            const RC::Matrix4x4 &view, const RC::Matrix4x4 &proj,
            RC::FrameResource &frame, bool worldOnly = false);

  /// @brief インスタンシングによる一括描画を実行する
  /// @param cmdList コマンドリスト
  /// @param view ビュー行列
  /// @param proj プロジェクション行列
  /// @param instances インスタンスごとのトランスフォームリスト
  /// @param frame フレームリソース
  /// @param worldOnly true なら World 行列だけを転送する（シャドウパス用。Draw と同じ）
  void DrawBatch(ID3D12GraphicsCommandList *cmdList, const RC::Matrix4x4 &view,
                 const RC::Matrix4x4 &proj,
                 const std::vector<Transform> &instances,
                 RC::FrameResource &frame, bool worldOnly = false);

  /// @brief インスタンシング描画（単色指定付き）
  /// @param cmdList コマンドリスト
  /// @param view ビュー行列
  /// @param proj プロジェクション行列
  /// @param instances インスタンスごとのトランスフォームリスト
  /// @param color 全インスタンスに適用するオーバーライドカラー
  /// @param frame フレームリソース
  /// @param worldOnly true なら World 行列だけを転送する（シャドウパス用。Draw と同じ）
  void DrawBatch(ID3D12GraphicsCommandList *cmdList, const RC::Matrix4x4 &view,
                 const RC::Matrix4x4 &proj,
                 const std::vector<Transform> &instances,
                 const RC::Vector4 &color,
                 RC::FrameResource &frame, bool worldOnly = false);

  /// @brief スキニング付きモデルを描画する
  /// @param cmdList コマンドリスト
  /// @param world ワールド行列
  /// @param view ビュー行列
  /// @param proj プロジェクション行列
  /// @param skinMatrices スキニング行列パレット
  /// @param frame フレームリソース
  void DrawSkinned(ID3D12GraphicsCommandList *cmdList,
                   const RC::Matrix4x4 &world,
                   const RC::Matrix4x4 &view, const RC::Matrix4x4 &proj,
                   const std::vector<RC::Matrix4x4> &skinMatrices,
                   RC::FrameResource &frame);

  /// @brief バッチ描画用のカーソルをリセットする
  /// @note FrameResource 移行後は内部で管理されるため、現在は実質的な処理を行いません。
  void ResetBatchCursor() { /* FrameResource 側で管理 */ }

  /// @brief 準備完了状態（メッシュと初期リソースが有効か）を取得する
  /// @return 準備完了なら true
  bool IsReady() const { return isReady_; }

  /// @brief 準備完了状態を設定する
  /// @param r 状態
  void SetReady(bool r) { isReady_ = r; }

  // === CS スキニング ===

  /// @brief CS スキニングをセットアップする
  /// @param device D3D12 デバイス
  /// @param pm PipelineManager
  /// @param srvMgr SRVマネージャ
  void SetSkinningCS(ID3D12Device *device, PipelineManager *pm,
                     SRVManager *srvMgr);

  /// @brief CS スキニングを実行する (Dispatch)
  /// @param cmdList コマンドリスト
  /// @param skinMatrices スキニング行列パレット
  /// @param frame フレームリソース
  void DispatchSkinning(ID3D12GraphicsCommandList *cmdList,
                        const std::vector<RC::Matrix4x4> &skinMatrices,
                        RC::FrameResource &frame);

  /// @brief CS スキニングが有効か
  bool HasCSSkinning() const { return skinningCS_.IsReady(); }

  /// @brief CS スキニング済み VBV を取得する
  const D3D12_VERTEX_BUFFER_VIEW &GetSkinnedVBV() const { return skinnedVBV_; }

  /// @brief CS スキニングが完了しているか (Dispatch済み)
  bool IsSkinningDispatched() const { return skinningDispatched_; }

  /// @brief CS スキニングの Dispatch フラグをリセットする
  void ResetSkinningDispatched() { skinningDispatched_ = false; }

  // === インスタンスバッチ描画（ModelInstanceBatcher から使う） ===

  /// @brief インスタンス 1 個ぶんの GPU データ（Object3D_Inst.VS の InstanceData と同じ並び）
  struct InstanceGPU {
    RC::Matrix4x4 WVP;                   ///< ワールド・ビュー・プロジェクション行列（影パスでは未使用）
    RC::Matrix4x4 World;                 ///< ワールド行列
    RC::Matrix4x4 WorldInverseTranspose; ///< 法線用（影パスでは未使用）
    RC::Vector4 color;                   ///< インスタンスカラー（PS で Material.color と乗算）
  };

  /// @brief 用意済みのマテリアル CB とインスタンス配列で、全 DrawItem をインスタンス描画する
  /// @param materialCB このバッチ用のマテリアル CB（b0 / PS）
  /// @param instanceAddr InstanceGPU 配列の先頭（t1 / VS）
  /// @param count インスタンス数
  /// @param perItem true なら DrawItem ごとに count 個ずつ並べたデータ（nodeWorld を掛け済み）を使う
  /// @note パイプライン（object3d_inst 系）・カメラ・ライト CB のバインドは呼び出し側で済ませておくこと
  void DrawInstancesPrepared(ID3D12GraphicsCommandList *cmdList,
                             D3D12_GPU_VIRTUAL_ADDRESS materialCB,
                             D3D12_GPU_VIRTUAL_ADDRESS instanceAddr,
                             uint32_t count, bool perItem);

  /// @brief テクスチャ上書き（0 = mtl のテクスチャ）の SRV ポインタ。バッチのまとめ判定用
  uint64_t TextureOverridePtr() const { return textureSrv_.ptr; }
  /// @brief 法線マップの SRV ポインタ（0 = 無し）
  uint64_t NormalMapPtr() const { return normalMapSrv_.ptr; }
  /// @brief ラフネスマップの SRV ポインタ（0 = 無し）
  uint64_t RoughnessMapPtr() const { return roughnessMapSrv_.ptr; }

  /// @brief 描画時に実際に使うライト CB のアドレス（外部ライト優先、無ければ自前）
  /// @note 自前ライトの場合は今フレームの領域へ送ってからアドレスを返す（コマンド記録中に呼ぶこと）
  D3D12_GPU_VIRTUAL_ADDRESS EffectiveLightCBAddress() {
    if (externalLightCBAddress_ != 0) return externalLightCBAddress_;
    return cbLight_.dyn.Address();
  }

  /// @brief ライトの識別子（バッチのまとめ判定用。外部ライトならそのアドレス、自前ならこのオブジェクト）
  uint64_t LightIdentity() const {
    return (externalLightCBAddress_ != 0) ? externalLightCBAddress_
                                          : reinterpret_cast<uint64_t>(this);
  }

  /// @brief CS スキニング済み頂点を使って描画する（通常の object3d パイプラインで）
  /// @param cmdList コマンドリスト
  /// @param world ワールド行列
  /// @param view ビュー行列
  /// @param proj プロジェクション行列
  /// @param frame フレームリソース
  void DrawSkinnedCS(ID3D12GraphicsCommandList *cmdList,
                     const RC::Matrix4x4 &world, const RC::Matrix4x4 &view,
                     const RC::Matrix4x4 &proj, RC::FrameResource &frame,
                     bool worldOnly = false);

private:
  /// @struct CB_Material
  /// @brief マテリアル定数バッファ
  /// @details 値は CPU 側に持ち、バインドするときに今フレームの領域へ送る（RC::DynamicCB）。
  ///          以前は Map しっぱなしの固定 CB だったが、CPU と GPU を並行させると
  ///          GPU が前フレームを描いている最中に上書きしてしまうため変更した。
  struct CB_Material {
    RC::DynamicCB<Material> dyn;
  };

  /// @struct CB_Light
  /// @brief モデル自前のライト定数バッファ（外部ライトが無いときに使う）
  struct CB_Light {
    RC::DynamicCB<DirectionalLight> dyn;
  };

  /// @struct InstanceDataGPU
  /// @brief シェーダーに送るインスタンスごとのデータ構造
  struct InstanceDataGPU {
    RC::Matrix4x4 WVP;                   ///< ワールド・ビュー・プロジェクション行列
    RC::Matrix4x4 World;                 ///< ワールド行列
    RC::Matrix4x4 WorldInverseTranspose; ///< ワールド逆転置行列（法線用）
    RC::Vector4 color;                   ///< インスタンスカラー
  };

  static_assert(sizeof(InstanceDataGPU) == sizeof(InstanceGPU),
                "InstanceGPU と InstanceDataGPU（シェーダーの InstanceData）の並びを揃えること");

  /// @brief メッシュに含まれる全マテリアルのテクスチャがロードされていることを保証する
  void EnsureMaterialSrvsLoaded_();

  /// @brief 指定されたマテリアルインデックスに対応する SRV ハンドルを取得する
  /// @param materialIndex マテリアルインデックス
  /// @return GPU 記述子ハンドル
  D3D12_GPU_DESCRIPTOR_HANDLE GetSrvForMaterial_(uint32_t materialIndex) const;

  Microsoft::WRL::ComPtr<ID3D12Device> device_;
  std::shared_ptr<ModelMesh> mesh_;

  CB_Material cbMat_{};
  CB_Light cbLight_{};

  D3D12_GPU_DESCRIPTOR_HANDLE textureSrv_{}; ///< オーバーライド用 SRV（共通テクスチャ）
  D3D12_GPU_DESCRIPTOR_HANDLE normalMapSrv_{};
  D3D12_GPU_DESCRIPTOR_HANDLE roughnessMapSrv_{};

  std::vector<D3D12_GPU_DESCRIPTOR_HANDLE> materialSrvs_; ///< マテリアルごとの SRV リスト

  D3D12_GPU_VIRTUAL_ADDRESS externalLightCBAddress_ = 0; ///< 外部ライトCBのアドレス

  TextureManager *texman_ = nullptr;
  bool isReady_ = false;

  // === CS スキニング用 ===
  ComputeShader skinningCS_;               ///< CS スキニング用ラッパ
  SRVManager *srvMgr_ = nullptr;               ///< SRVマネージャ

  Microsoft::WRL::ComPtr<ID3D12Resource> skinnedVertexBuffer_;  ///< スキニング済み頂点バッファ (UAV)
  D3D12_VERTEX_BUFFER_VIEW skinnedVBV_{};      ///< スキニング済み VBV
  D3D12_GPU_DESCRIPTOR_HANDLE skinnedUAVHandle_{}; ///< UAV ディスクリプタハンドル
  uint32_t skinnedVertexCount_ = 0;            ///< スキニング対象頂点数
  bool skinningResourcesReady_ = false;        ///< CS リソースが初期化済みか
  bool skinningDispatched_ = false;            ///< 今フレーム Dispatch 済みか

  Microsoft::WRL::ComPtr<ID3D12Resource> skinningInfoCB_;  ///< SkinningInfo CB
  uint32_t *skinningInfoMapped_ = nullptr;     ///< CB のマップ済みポインタ

  /// @brief CS スキニング用リソースを初期化する
  void InitSkinningResources_();
};
