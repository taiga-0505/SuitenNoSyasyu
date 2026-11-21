#pragma once
#include <cassert>
#include <d3d12.h>
#include <wrl/client.h>

class DescriptorHeap; // 前方宣言（ヘッダ循環防止）

/// @class DepthStencil
/// @brief 深度ステンシルバッファ（テクスチャと DSV）を管理するクラス
class DepthStencil {
public:
  /// @brief 初期化。深度ステンシルリソースの作成と DSV の配置を行う
  /// @param device    DirectX12 デバイス
  /// @param width     幅
  /// @param height    高さ
  /// @param dsvHeap   DSV を配置するディスクリプタヒープ
  /// @param texFormat テクスチャリソースのフォーマット
  /// @param dsvFormat DSV のフォーマット
  void Init(ID3D12Device *device, UINT width, UINT height,
            DescriptorHeap &dsvHeap,
            DXGI_FORMAT texFormat = DXGI_FORMAT_D24_UNORM_S8_UINT,
            DXGI_FORMAT dsvFormat = DXGI_FORMAT_D24_UNORM_S8_UINT);

  /// @brief バッファをリサイズする
  /// @note GPU のアイドル待機後に呼び出す必要がある。DSV は新しく確保せず、既存のスロットに作り直す。
  /// @param width   新しい幅
  /// @param height  新しい高さ
  /// @param dsvHeap DSV を再配置するディスクリプタヒープ（現在は未使用）
  void Resize(UINT width, UINT height, DescriptorHeap &dsvHeap);

  /// @brief リソースを破棄する
  void Term();

  /// @brief 深度ステンシルバッファをクリアする
  /// @param list    コマンドリスト
  /// @param depth   クリアする深度値（0.0〜1.0）
  /// @param stencil クリアするステンシル値
  void Clear(ID3D12GraphicsCommandList *list, float depth = 1.0f,
             UINT8 stencil = 0) const;

  /// @brief DSV の CPU ハンドルを取得する
  /// @return DSV ハンドル
  D3D12_CPU_DESCRIPTOR_HANDLE Dsv() const { return dsv_; }

  /// @brief 深度ステンシルリソースを取得する
  /// @return ID3D12Resource へのポインタ
  ID3D12Resource *Resource() const { return tex_.Get(); }

  /// @brief テクスチャフォーマットを取得する
  /// @return テクスチャリソースのフォーマット
  DXGI_FORMAT Format() const { return texFormat_; }

  /// @brief 幅を取得
  /// @return 幅
  UINT Width() const { return width_; }

  /// @brief 高さを取得
  /// @return 高さ
  UINT Height() const { return height_; }

private:
  /// @brief 深度ステンシルリソースを生成する
  void createResource();
  /// @brief 深度ステンシルビュー（DSV）を生成する
  /// @param dsvHeap DSV のスロットを確保するディスクリプタヒープ
  void createView(DescriptorHeap &dsvHeap);

private:
  Microsoft::WRL::ComPtr<ID3D12Device> device_; ///< デバイス
  Microsoft::WRL::ComPtr<ID3D12Resource> tex_;  ///< 深度ステンシルテクスチャリソース
  D3D12_CPU_DESCRIPTOR_HANDLE dsv_{};           ///< ディスクリプタヒープ内の DSV ハンドル
  UINT width_ = 0, height_ = 0;                 ///< サイズ
  DXGI_FORMAT texFormat_ = DXGI_FORMAT_D24_UNORM_S8_UINT; ///< テクスチャフォーマット
  DXGI_FORMAT dsvFormat_ = DXGI_FORMAT_D24_UNORM_S8_UINT; ///< DSV フォーマット
};
