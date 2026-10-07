#pragma once
#include "struct.h"
#include "Math/MathTypes.h"
#include "function/function.h"
#include "Render/FrameResource.h" // RC::DynamicCB
#include <d3d12.h>
#include <vector>
#include <wrl/client.h>

/// @brief 単一のプリミティブメッシュを管理・描画するクラス
/// 頂点バッファ、インデックスバッファ、定数バッファ（WVP/Material）を個別に保持します。
class PrimitiveMesh {
public:
  /// @brief デフォルトコンストラクタ
  PrimitiveMesh() = default;

  /// @brief デストラクタ
  ~PrimitiveMesh();

  /// @brief メッシュの初期化とGPUリソースの作成
  /// @param device DirectX12デバイス
  /// @param data 生成またはロード済みのモデルデータ
  void Initialize(ID3D12Device *device, const ModelData &data);

  /// @brief 描画実行
  /// 内部で保持しているトランスフォーム情報を使用して描画します。
  /// @param cmdList グラフィックスコマンドリスト
  void Draw(ID3D12GraphicsCommandList *cmdList);

  /// @brief 指定したワールド行列による描画実行
  /// @param cmdList グラフィックスコマンドリスト
  /// @param world 適用するワールド行列
  void Draw(ID3D12GraphicsCommandList *cmdList, const RC::Matrix4x4 &world);

  /// @brief 用意済みのマテリアル CB・インスタンス配列でインスタンス描画する
  /// @details Object3DInstancing ルートシグネチャ（object3d_inst 系 PSO）用。
  ///          パイプライン・カメラ・点光源などのバインドは呼び出し側で済ませておくこと。
  /// @param materialCB マテリアル CB（b0 / PS）
  /// @param lightCB 平行光源 CB（b1 / PS）。0 なら設定しない
  /// @param instanceAddr インスタンスデータ（ModelResource::InstanceGPU と同じ並び）の先頭（t1 / VS）
  /// @param count インスタンス数
  void DrawInstancedPrepared(ID3D12GraphicsCommandList *cmdList,
                             D3D12_GPU_VIRTUAL_ADDRESS materialCB,
                             D3D12_GPU_VIRTUAL_ADDRESS lightCB,
                             D3D12_GPU_VIRTUAL_ADDRESS instanceAddr, uint32_t count);

  /// @brief 使用するテクスチャのSRVを設定
  /// @param srvGPUHandle テクスチャのGPUディスクリプタハンドル
  void SetTexture(D3D12_GPU_DESCRIPTOR_HANDLE srvGPUHandle) {
    textureSrv_ = srvGPUHandle;
  }

  /// @brief 使用する法線マップのSRVを設定
  /// @param srvGPUHandle テクスチャのGPUディスクリプタハンドル
  void SetNormalMap(D3D12_GPU_DESCRIPTOR_HANDLE srvGPUHandle) {
    normalMapSrv_ = srvGPUHandle;
  }

  /// @brief 使用する粗さマップのSRVを設定
  /// @param srvGPUHandle テクスチャのGPUディスクリプタハンドル
  void SetRoughnessMap(D3D12_GPU_DESCRIPTOR_HANDLE srvGPUHandle) {
    roughnessMapSrv_ = srvGPUHandle;
  }

  /// @brief トランスフォーム情報への参照を取得
  /// @return Transform構造体への参照
  Transform &T() { return transform_; }

  /// @brief マテリアルデータへのポインタを取得（直接編集可能）
  /// @return マテリアル構造体へのポインタ
  Material *Mat() { return cbMat_.dyn.Ptr(); }

  /// @brief ライティングモードを個別に固定する
  /// @param m 設定する LightingMode
  /// @note 呼び出し以降、このメッシュはシーンの DirectionalLight の
  ///       LightingMode に追従しなくなる。追従に戻すには
  ///       ClearLightingModeOverride() を呼ぶ。
  void SetLightingMode(LightingMode m) {
    lightingModeOverride_ = static_cast<int>(m);
    if (cbMat_.dyn.Ptr()) {
      cbMat_.dyn.Ptr()->lightingMode = lightingModeOverride_;
    }
  }

  /// @brief ライティングモードの個別固定を解除し、
  ///        シーンの DirectionalLight のモードに追従させる
  void ClearLightingModeOverride() { lightingModeOverride_ = -1; }

  /// @brief ライティングモードのオーバーライド値を取得する
  /// @return -1: DirectionalLight に追従 / 0以上: 固定された LightingMode
  int GetLightingModeOverride() const { return lightingModeOverride_; }

  /// @brief 可視状態を設定
  /// @param v trueで表示
  void SetVisible(bool v) { visible_ = v; }

  /// @brief 可視状態を取得
  /// @return 表示中なら true
  bool Visible() const { return visible_; }

private:
  /// @brief 頂点バッファのアップロード
  void UploadVB_(const std::vector<VertexData> &vertices);

  /// @brief インデックスバッファのアップロード
  void UploadIB_(const std::vector<uint32_t> &indices);

  /// @brief 頂点バッファ保持用構造体
  struct VB {
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    D3D12_VERTEX_BUFFER_VIEW view{};
    uint32_t vertexCount = 0;
  };

  /// @brief インデックスバッファ保持用構造体
  struct IB {
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    D3D12_INDEX_BUFFER_VIEW view{};
    uint32_t indexCount = 0;
  };

  /// @brief WVP 用定数バッファ（値は CPU 側。バインド時に今フレームの領域へ送る）
  struct CB_WVP {
    RC::DynamicCB<TransformationMatrix> dyn;
  };

  /// @brief マテリアル用定数バッファ（値は CPU 側。バインド時に今フレームの領域へ送る）
  /// @details 以前は Map しっぱなしの固定 CB だった。CPU と GPU を並行させると、
  ///          GPU が前フレームを描いている最中に上書きしてしまうため変更した。
  struct CB_Material {
    RC::DynamicCB<Material> dyn;
  };

private:
  Microsoft::WRL::ComPtr<ID3D12Device> device_; ///< 使用中のデバイス

  VB vb_{};       ///< 頂点バッファ
  IB ib_{};       ///< インデックスバッファ
  CB_WVP cbWvp_{};   ///< 行列用定数バッファ
  CB_Material cbMat_{}; ///< マテリアル用定数バッファ
  D3D12_GPU_DESCRIPTOR_HANDLE textureSrv_{}; ///< 使用テクスチャのGPUハンドル
  D3D12_GPU_DESCRIPTOR_HANDLE normalMapSrv_{}; ///< 使用法線マップのGPUハンドル
  D3D12_GPU_DESCRIPTOR_HANDLE roughnessMapSrv_{}; ///< 使用粗さマップのGPUハンドル

  Transform transform_{{1, 1, 1}, {0, 0, 0}, {0, 0, 0}}; ///< ローカルトランスフォーム
  bool visible_ = true; ///< 可視フラグ

  int lightingModeOverride_ = -1; ///< -1: DirectionalLight に追従 / 0以上: 固定 LightingMode
};
