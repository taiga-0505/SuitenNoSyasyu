#pragma once
#include "RC.h"
#include <cstdint>
#include <d3d12.h>
#include <wrl.h>

// 前方宣言
class Dx12Core;

/// @class RenderTexture
/// @brief オフスクリーン描画に使用するレンダーテクスチャを管理するクラス
/// @details RenderTargetView（RTV）として描画先になり、ShaderResourceView（SRV）としてテクスチャ入力に利用できる。
///          リソースバリアによる状態遷移（Transition）をカプセル化している。
class RenderTexture {
public:
  /// @brief コンストラクタ（リソースは Initialize で作る）
  RenderTexture() = default;
  /// @brief デストラクタ
  ~RenderTexture() = default;

  /// @brief テクスチャリソースと RTV / SRV を作成する
  /// @param dxCore     DirectX12 コアシステムへのポインタ
  /// @param width      テクスチャの幅
  /// @param height     テクスチャの高さ
  /// @param format     テクスチャフォーマット
  /// @param clearColor クリアカラー（最適化クリア値として使う）
  /// @note 再初期化時は、既に確保済みの RTV / SRV 記述子をそのまま使い回す。
  void Initialize(Dx12Core *dxCore, uint32_t width, uint32_t height,
                  DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
                  const RC::Vector4& clearColor = {0.1f, 0.25f, 0.5f, 1.0f});

  /// @brief 任意の状態にリソースを遷移させる（汎用リソースバリア）
  /// @param cmdList   コマンドリスト
  /// @param nextState 遷移先の状態
  /// @note 現在の状態と同じならバリアを張らない。
  void Transition(ID3D12GraphicsCommandList *cmdList,
                  D3D12_RESOURCE_STATES nextState);

  /// @brief 描画先として使用するための状態（RenderTarget）に遷移させる
  /// @param cmdList コマンドリスト
  void TransitionToRenderTarget(ID3D12GraphicsCommandList *cmdList);

  /// @brief テクスチャとして読み込む状態（ShaderResource）に遷移させる
  /// @param cmdList コマンドリスト
  void TransitionToShaderResource(ID3D12GraphicsCommandList *cmdList);

  /// @brief リソースを明示的に解放する
  void Release() { resource_.Reset(); }

  // 各種ゲッター

  /// @brief D3D12 リソースを取得する
  /// @return ID3D12Resource への ComPtr
  Microsoft::WRL::ComPtr<ID3D12Resource> GetResource() const {
    return resource_;
  }

  /// @brief RenderTargetView（RTV）の CPU ハンドルを取得する
  /// @return RTV ハンドル
  D3D12_CPU_DESCRIPTOR_HANDLE GetRTV() const { return rtvHandle_; }

  /// @brief ShaderResourceView（SRV）の CPU ハンドルを取得する
  /// @return SRV CPU ハンドル
  D3D12_CPU_DESCRIPTOR_HANDLE GetSRVCPU() const { return srvHandleCPU_; }

  /// @brief ShaderResourceView（SRV）の GPU ハンドルを取得する
  /// @return SRV GPU ハンドル
  D3D12_GPU_DESCRIPTOR_HANDLE GetSRVGPU() const { return srvHandleGPU_; }

private:
  /// @brief 描画用テクスチャリソースを作成する内部ヘルパー
  /// @param device     D3D12 デバイス
  /// @param width      幅
  /// @param height     高さ
  /// @param format     フォーマット
  /// @param clearColor 最適化クリア値
  /// @return 作成したリソース（初期状態は COMMON）
  Microsoft::WRL::ComPtr<ID3D12Resource>
  CreateRenderTextureResource(Microsoft::WRL::ComPtr<ID3D12Device> device,
                              UINT width, UINT height, DXGI_FORMAT format,
                              const RC::Vector4 &clearColor);

private:
  Microsoft::WRL::ComPtr<ID3D12Resource> resource_; ///< 実体となる D3D12 リソース
  D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle_{};        ///< RTV 用記述子ハンドル

  D3D12_CPU_DESCRIPTOR_HANDLE srvHandleCPU_{};     ///< SRV 用 CPU 記述子ハンドル
  D3D12_GPU_DESCRIPTOR_HANDLE srvHandleGPU_{};     ///< SRV 用 GPU 記述子ハンドル

  D3D12_RESOURCE_STATES currentState_ = D3D12_RESOURCE_STATE_COMMON; ///< 現在のリソース状態
};
