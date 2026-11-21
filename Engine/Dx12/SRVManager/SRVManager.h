#pragma once
#include <cassert>
#include <cstdint>
#include <d3d12.h>
#include <vector>

class DescriptorHeap;

/// @class SRVManager
/// @brief SRV（Shader Resource View）のディスクリプタスロットを管理するクラス
/// @details ディスクリプタヒープ内の空きスロットの確保、解放、および各種 SRV / UAV の生成をラップする。
class SRVManager {
public:
  /// @brief SRV ハンドルの管理構造体
  struct Handle {
    UINT index = UINT_MAX;              ///< ヒープ内のインデックス（UINT_MAX なら無効）
    D3D12_CPU_DESCRIPTOR_HANDLE cpu{};  ///< CPU ディスクリプタハンドル
    D3D12_GPU_DESCRIPTOR_HANDLE gpu{};  ///< GPU ディスクリプタハンドル

    /// @brief 有効なハンドルかどうか
    /// @return 有効なら true
    bool IsValid() const { return index != UINT_MAX; }
  };

  /// @brief 初期化する
  /// @param device  DirectX12 デバイス
  /// @param srvHeap 管理対象のディスクリプタヒープ（シェーダー参照可能な CBV/SRV/UAV ヒープ）
  void Init(ID3D12Device *device, DescriptorHeap *srvHeap);

  /// @brief 管理情報をクリアする（ヒープ自体は解放しない）
  void Term();

  /// @brief デバイスを取得する
  /// @return ID3D12Device へのポインタ
  ID3D12Device *Device() const { return device_; }

  /// @brief ディスクリプタヒープを取得する
  /// @return DescriptorHeap へのポインタ
  DescriptorHeap *Heap() const { return srvHeap_; }

  /// @brief 空きスロットを 1 つ確保する
  /// @return 確保されたハンドル
  /// @note 解放済みのスロットがあれば再利用し、無ければヒープの末尾から新しく確保する。
  Handle Allocate();

  /// @brief スロットを解放して再利用可能にする
  /// @note GPU が該当スロットの参照を完全に終えている必要がある。
  /// @param h 解放するハンドル（無効なハンドルなら何もしない）
  void Free(const Handle &h);

  /// @brief 2D テクスチャ用の SRV を作成する
  /// @param res             対象リソース
  /// @param fmt             フォーマット
  /// @param mipLevels       ミップマップ数
  /// @param mostDetailedMip 最も詳細なミップレベル
  /// @return 作成された SRV のハンドル
  Handle CreateTexture2D(ID3D12Resource *res, DXGI_FORMAT fmt, UINT mipLevels,
                         UINT mostDetailedMip = 0);

  /// @brief キューブマップ用の SRV を作成する
  /// @param res             対象リソース
  /// @param fmt             フォーマット
  /// @param mipLevels       ミップマップ数
  /// @param mostDetailedMip 最も詳細なミップレベル
  /// @return 作成された SRV のハンドル
  Handle CreateTextureCube(ID3D12Resource *res, DXGI_FORMAT fmt, UINT mipLevels,
                           UINT mostDetailedMip = 0);

  /// @brief 2D テクスチャ用の UAV を作成する
  /// @param res 対象リソース
  /// @param fmt フォーマット
  /// @return 作成された UAV のハンドル
  Handle CreateTexture2DUAV(ID3D12Resource *res, DXGI_FORMAT fmt);

  /// @brief ストラクチャードバッファ用の SRV を作成する
  /// @param res          対象リソース
  /// @param elementCount 要素数
  /// @param strideBytes  1 要素あたりのバイトサイズ
  /// @return 作成された SRV のハンドル
  Handle CreateStructuredBuffer(ID3D12Resource *res, UINT elementCount,
                                UINT strideBytes);

  /// @brief ストラクチャードバッファ用の UAV を作成する
  /// @param res          対象リソース（D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS 必須）
  /// @param elementCount 要素数
  /// @param strideBytes  1 要素あたりのバイトサイズ
  /// @return 作成された UAV のハンドル
  Handle CreateStructuredBufferUAV(ID3D12Resource *res, UINT elementCount,
                                   UINT strideBytes);

private:
  ID3D12Device *device_ = nullptr; ///< デバイス（非所有）
  DescriptorHeap *srvHeap_ = nullptr; ///< ディスクリプタヒープ（非所有）

  std::vector<UINT> free_;    ///< 再利用用の空きインデックスリスト
  std::vector<uint8_t> used_; ///< 使用状況管理フラグ（デバッグ・整合性チェック用）
};
