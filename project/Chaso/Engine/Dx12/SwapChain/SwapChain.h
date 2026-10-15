#pragma once
#include <array>
#include <cassert>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

/// @class SwapChain
/// @brief DXGI スワップチェインを管理するクラス
/// @details 画面表示のためのダブル／トリプルバッファリング、フリップ、リサイズを制御する。
class SwapChain {
public:
  /// @brief 最大フレーム数（バックバッファの最大数）
  static constexpr UINT kMaxFrames = 3;

  /// @brief スワップチェインとバックバッファの RTV を作成する
  /// @param factory      DXGI ファクトリ
  /// @param device       DirectX12 デバイス
  /// @param queue        描画に使用するコマンドキュー
  /// @param hwnd         ウィンドウハンドル
  /// @param width        画面幅
  /// @param height       画面高さ
  /// @param format       バッファのピクセルフォーマット
  /// @param frameCount   バッファ数（2 = ダブル, 3 = トリプル。範囲外なら 2）
  /// @param allowTearing テアリング（VRR 等）を許可するか（非対応環境では無効になる）
  /// @note RTV を作るには、先に SetRtvHeap でヒープを渡しておくこと。
  void Init(IDXGIFactory6 *factory, ID3D12Device *device,
            ID3D12CommandQueue *queue, HWND hwnd, UINT width, UINT height,
            DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM,
            UINT frameCount = 2, bool allowTearing = true);

  /// @brief バックバッファとスワップチェインを解放する
  void Term();

  /// @brief RTV を配置するディスクリプタヒープを設定する
  /// @param heap      RTV 用ディスクリプタヒープへのポインタ（先頭からフレーム数ぶん使う）
  /// @param increment ディスクリプタの増分サイズ
  void SetRtvHeap(ID3D12DescriptorHeap *heap, UINT increment);

  /// @brief ウィンドウサイズに合わせてバッファをリサイズする
  /// @param width  新しい幅
  /// @param height 新しい高さ
  /// @note GPU がバックバッファを使い終わってから呼ぶこと。
  void Resize(UINT width, UINT height);

  /// @brief バッファをフリップして画面を表示する
  /// @param syncInterval 垂直同期（0 = なし, 1 = あり）
  /// @param flags        プレゼントフラグ（DXGI_PRESENT_ALLOW_TEARING 等）
  void Present(UINT syncInterval = 1, UINT flags = 0);

  /// @brief 現在のバックバッファインデックスを取得する
  /// @return インデックス（0〜frameCount - 1）
  UINT CurrentBackBufferIndex() const {
    return swap_->GetCurrentBackBufferIndex();
  }

  /// @brief 指定インデックスのバックバッファリソースを取得する
  /// @param i インデックス
  /// @return ID3D12Resource へのポインタ
  ID3D12Resource *BackBuffer(UINT i) const { return backBuffers_[i].Get(); }

  /// @brief 指定インデックスの RTV ハンドルを取得する
  /// @param i インデックス
  /// @return RTV ハンドル（RTV ヒープ未設定なら空）
  D3D12_CPU_DESCRIPTOR_HANDLE RtvAt(UINT i) const { return rtv_[i]; }

  /// @brief バッファ数を取得する
  /// @return フレーム数
  UINT FrameCount() const { return frameCount_; }

  /// @brief フォーマットを取得する
  /// @return バッファのピクセルフォーマット
  DXGI_FORMAT Format() const { return format_; }

  /// @brief IDXGISwapChain4 ポインタを直接取得する
  /// @return IDXGISwapChain4 へのポインタ
  IDXGISwapChain4 *Raw() const {
    return swap_.Get();
  }

private:
  /// @brief スワップチェイン本体を生成する
  /// @param hwnd ウィンドウハンドル
  void createSwapChain(HWND hwnd);

  /// @brief バックバッファを取得し、RTV を生成する
  void createBackBufferRTVs();

private:
  Microsoft::WRL::ComPtr<IDXGIFactory6> factory_;   ///< DXGI ファクトリ
  Microsoft::WRL::ComPtr<ID3D12Device> device_;     ///< デバイス
  Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_; ///< コマンドキュー
  Microsoft::WRL::ComPtr<IDXGISwapChain4> swap_;    ///< スワップチェイン本体

  std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, kMaxFrames> backBuffers_{}; ///< バックバッファリソース配列
  std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kMaxFrames> rtv_{}; ///< RTV ハンドル配列

  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvHeap_; ///< RTV 用ディスクリプタヒープ
  UINT rtvInc_ = 0; ///< RTV ディスクリプタの増分サイズ

  UINT width_ = 0, height_ = 0; ///< 解像度
  DXGI_FORMAT format_ = DXGI_FORMAT_R8G8B8A8_UNORM; ///< フォーマット
  UINT frameCount_ = 2; ///< バッファ数
  bool allowTearing_ = true; ///< テアリング許可フラグ
};
