#pragma once
#include "Render/FrameResource.h" // RC::DynamicCB
#include "Math/MathTypes.h"
#include "function/function.h"
#include "struct.h"
#include <d3d12.h>
#include <vector>
#include <wrl/client.h>

/// @class Skybox
/// @brief 全天を覆うスカイボックス（背景）の描画を管理するクラス
/// @details 内向きの立方体メッシュを自動生成し、カメラの移動に追従させることで無限遠の背景を表現します。
class Skybox {
public:
  Skybox() = default;
  ~Skybox();

  /// @brief 初期化（Device 依存リソースの作成とメッシュ生成）
  /// @param device D3D12 デバイス
  void Initialize(ID3D12Device *device);

  /// @brief 毎フレームの行列更新
  /// @param view ビュー行列
  /// @param proj プロジェクション行列
  void Update(const RC::Matrix4x4 &view, const RC::Matrix4x4 &proj);

  /// @brief 描画実行
  /// @param cmdList コマンドリスト
  void Draw(ID3D12GraphicsCommandList *cmdList);

  /// @brief ワールド行列を外部から指定して描画する（コマンドキュー用）
  /// @param cmdList コマンドリスト
  /// @param world 使用するワールド行列
  void Draw(ID3D12GraphicsCommandList *cmdList, const RC::Matrix4x4 &world);

  /// @brief スカイボックスに使用するテクスチャ（SRV）を設定する
  /// @param srvGPUHandle テクスチャの GPU ハンドル
  void SetTexture(D3D12_GPU_DESCRIPTOR_HANDLE srvGPUHandle) {
    textureSrv_ = srvGPUHandle;
  }

  /// @brief トランスフォーム情報を取得する（読み書き可能）
  /// @return Transform への参照
  Transform &T() { return transform_; }

  /// @brief マテリアル情報を取得する（読み書き可能）
  /// @return Material へのポインタ
  Material *Mat() { return cbMat_.dyn.Ptr(); }

  /// @brief 可視状態を設定する
  /// @param v true で表示
  void SetVisible(bool v) { visible_ = v; }

  /// @brief 可視状態か確認する
  /// @return 可視なら true
  bool Visible() const { return visible_; }

private:
  /// @brief 内向きのボックスメッシュデータを構築する
  void BuildBox_();

  /// @brief 頂点データを GPU に転送する
  void UploadVB_();

  /// @brief インデックスデータを GPU に転送する
  void UploadIB_();

  /// @struct VB
  /// @brief 頂点バッファ管理構造体
  struct VB {
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    D3D12_VERTEX_BUFFER_VIEW view{};
    uint32_t vertexCount = 0;
  };

  /// @struct IB
  /// @brief インデックスバッファ管理構造体
  struct IB {
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    D3D12_INDEX_BUFFER_VIEW view{};
    uint32_t indexCount = 0;
  };

  /// @struct CB_WVP
  /// @brief 行列用定数バッファ
  struct CB_WVP {
    RC::DynamicCB<TransformationMatrix> dyn; ///< 値は CPU 側。バインド時に今フレームの領域へ送る（CPU/GPU 並行のため）
  };

  /// @struct CB_Material
  /// @brief マテリアル用定数バッファ
  struct CB_Material {
    RC::DynamicCB<Material> dyn; ///< 値は CPU 側。バインド時に今フレームの領域へ送る（CPU/GPU 並行のため）
  };

  /// @struct CB_Light
  /// @brief ライト用定数バッファ
  struct CB_Light {
    RC::DynamicCB<DirectionalLight> dyn; ///< 値は CPU 側。バインド時に今フレームの領域へ送る（CPU/GPU 並行のため）
  };

private:
  Microsoft::WRL::ComPtr<ID3D12Device> device_;

  std::vector<VertexData> vertices_; ///< CPU側の頂点データ配列
  std::vector<uint16_t> indices_;   ///< CPU側のインデックスデータ配列

  VB vb_{};
  IB ib_{};
  CB_WVP cbWvp_{};
  CB_Material cbMat_{};
  CB_Light cbLight_{};
  D3D12_GPU_DESCRIPTOR_HANDLE textureSrv_{};

  Transform transform_{{100, 100, 100}, {0, 0, 0}, {0, 0, 0}}; ///< スカイボックスのトランスフォーム

  bool visible_ = true;
};
