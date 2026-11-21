#pragma once
#include "Render/FrameResource.h" // RC::DynamicCB
#include "Math/MathTypes.h"
#include "function/function.h"
#include "struct.h"
#include <d3d12.h>
#include <vector>
#include <wrl/client.h>

/// @class Skydome
/// @brief 天球（ドーム状の背景）の描画を管理するクラス
/// @details 全球のメッシュを生成し、背景テクスチャを貼り付けて無限遠の背景として描画する。
///          外部のライト設定（LightManager 等）を共有する機能も備える。
class Skydome {
public:
  /// @brief コンストラクタ（GPU リソースは Initialize で作る）
  Skydome() = default;
  /// @brief デストラクタ（頂点・インデックスバッファを遅延解放キューへ渡す）
  ~Skydome();

  /// @brief 初期化（Device 依存リソースの作成と天球メッシュ生成）
  /// @param device     D3D12 デバイス
  /// @param radius     天球の半径
  /// @param sliceCount 経度分割数
  /// @param stackCount 緯度分割数
  void Initialize(ID3D12Device *device, float radius = 100.0f,
                  UINT sliceCount = 32, UINT stackCount = 32);

  /// @brief transform_ からワールド行列を作り、WVP 行列を更新する（毎フレーム呼ぶ）
  /// @param view ビュー行列
  /// @param proj プロジェクション行列
  void Update(const RC::Matrix4x4 &view, const RC::Matrix4x4 &proj);

  /// @brief 現在の行列・マテリアルで描画する
  /// @param cmdList コマンドリスト
  /// @note テクスチャ未設定（ロード待ち等）の間は描画しない。
  void Draw(ID3D12GraphicsCommandList *cmdList);

  /// @brief ワールド行列を外部から指定して描画する（コマンドキュー用）
  /// @details ビュー・プロジェクションは RenderContext の現在値を使う。
  /// @param cmdList コマンドリスト
  /// @param world   使用するワールド行列
  void Draw(ID3D12GraphicsCommandList *cmdList, const RC::Matrix4x4 &world);

  /// @brief ワールド行列とビュープロジェクション行列を外部から指定して描画する
  /// @details 天球をカメラ中心に固定したい場合など、平行移動を除いたビュー行列を
  ///          使いたいときに用いる（RC::DrawSkydome が使用）。
  /// @param cmdList  コマンドリスト
  /// @param world    使用するワールド行列
  /// @param viewProj 使用するビュー×プロジェクション行列
  void Draw(ID3D12GraphicsCommandList *cmdList, const RC::Matrix4x4 &world,
            const RC::Matrix4x4 &viewProj);

  /// @brief 天球に使用するテクスチャ（SRV）を設定する
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

  /// @brief ライト情報を取得する（読み書き可能）
  /// @return DirectionalLight へのポインタ
  DirectionalLight *Light() { return cbLight_.dyn.Ptr(); }

  /// @brief 外部で管理されているライト CB のアドレスを設定する
  /// @param addr ライト CB の GPU 仮想アドレス（0 で自前ライトに戻る）
  void SetExternalLightCBAddress(D3D12_GPU_VIRTUAL_ADDRESS addr) {
    externalLightCBAddress_ = addr;
  }

  /// @brief 外部ライト CB の GPU 仮想アドレスを取得する
  /// @return アドレス値（未設定なら 0）
  D3D12_GPU_VIRTUAL_ADDRESS GetExternalLightCBAddress() const {
    return externalLightCBAddress_;
  }

  /// @brief 可視状態を設定する
  /// @param v true で表示
  void SetVisible(bool v) { visible_ = v; }

  /// @brief 可視状態か確認する
  /// @return 可視なら true
  bool Visible() const { return visible_; }

private:
  /// @brief 天球メッシュを構築する（常に内向き）
  /// @param radius     半径
  /// @param sliceCount 経度分割数
  /// @param stackCount 緯度分割数
  void BuildGeometry(float radius, UINT sliceCount, UINT stackCount);

  /// @brief 頂点データを GPU に転送する
  void UploadVB_();

  /// @brief インデックスデータを GPU に転送する
  void UploadIB_();

  /// @struct VB
  /// @brief 頂点バッファ管理構造体
  struct VB {
    Microsoft::WRL::ComPtr<ID3D12Resource> resource; ///< 頂点バッファ
    D3D12_VERTEX_BUFFER_VIEW view{};                 ///< 頂点バッファビュー
    uint32_t vertexCount = 0;                        ///< 頂点数
  };

  /// @struct IB
  /// @brief インデックスバッファ管理構造体
  struct IB {
    Microsoft::WRL::ComPtr<ID3D12Resource> resource; ///< インデックスバッファ
    D3D12_INDEX_BUFFER_VIEW view{};                  ///< インデックスバッファビュー
    uint32_t indexCount = 0;                         ///< インデックス数
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
  Microsoft::WRL::ComPtr<ID3D12Device> device_; ///< D3D12 デバイス

  std::vector<VertexData> vertices_; ///< CPU 側の頂点データ配列
  std::vector<uint16_t> indices_;   ///< CPU 側のインデックスデータ配列

  VB vb_{};                                  ///< 頂点バッファ
  IB ib_{};                                  ///< インデックスバッファ
  CB_WVP cbWvp_{};                           ///< 行列用定数バッファ
  CB_Material cbMat_{};                      ///< マテリアル用定数バッファ
  CB_Light cbLight_{};                       ///< 自前のライト用定数バッファ（外部ライト未指定時に使う）
  D3D12_GPU_DESCRIPTOR_HANDLE textureSrv_{}; ///< 天球テクスチャの SRV（0 の間は描画しない）

  Transform transform_{{1, 1, 1}, {0, 0, 0}, {0, 0, 0}}; ///< 天球のトランスフォーム

  bool visible_ = true; ///< false なら描画しない

  D3D12_GPU_VIRTUAL_ADDRESS externalLightCBAddress_ = 0; ///< 外部ライト CB のアドレス（0 なら自前ライトを使う）
};
