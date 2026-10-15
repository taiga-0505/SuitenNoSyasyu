#pragma once
#include <cassert>
#include <cstdint>
#include <d3d12.h>
#include <wrl/client.h>


/// @class DescriptorHeap
/// @brief DirectX12 のディスクリプタヒープを管理するクラス
/// @details RTV, DSV, CBV/SRV/UAV などのディスクリプタの確保とハンドル管理を行う。
///          確保は先頭から順に積むだけで、個別の解放はしない。
class DescriptorHeap {
public:
  /// @brief ディスクリプタヒープを作成する
  /// @param device        DirectX12 デバイス
  /// @param type          ヒープのタイプ（RTV, DSV, CBV_SRV_UAV 等）
  /// @param capacity      最大ディスクリプタ数（1 以上）
  /// @param shaderVisible シェーダーから参照可能にするか（CBV_SRV_UAV 等で true）
  void Init(ID3D12Device *device, D3D12_DESCRIPTOR_HEAP_TYPE type,
            UINT capacity, bool shaderVisible = false);

  /// @brief ヒープを破棄する
  void Term();

  /// @brief ディスクリプタを確保し、先頭の CPU ハンドルを返す
  /// @param count 確保する連続したディスクリプタ数
  /// @return 確保された領域の先頭 CPU ハンドル（戻り値 + i * Increment() で連番分を使える）
  /// @note 容量を超えると assert で止まる。
  D3D12_CPU_DESCRIPTOR_HANDLE AllocateCPU(UINT count = 1);

  /// @brief 指定インデックスの CPU ハンドルを取得する
  /// @param index ディスクリプタのインデックス
  /// @return CPU ハンドル
  D3D12_CPU_DESCRIPTOR_HANDLE CPUAt(UINT index) const;

  /// @brief 指定インデックスの GPU ハンドルを取得する
  /// @param index ディスクリプタのインデックス
  /// @return GPU ハンドル
  /// @note シェーダー参照可能なヒープでのみ使える。
  D3D12_GPU_DESCRIPTOR_HANDLE GPUAt(UINT index) const;

  /// @brief ディスクリプタ 1 つあたりの増分サイズを取得する
  /// @return インクリメントサイズ（バイト）
  UINT Increment() const { return inc_; }

  /// @brief ID3D12DescriptorHeap のポインタを取得する
  /// @return ヒープポインタ
  ID3D12DescriptorHeap *Heap() const { return heap_.Get(); }

  /// @brief シェーダー参照可能かどうかを取得する
  /// @return 参照可能なら true
  bool ShaderVisible() const { return shaderVisible_; }

  /// @brief ヒープの最大容量を取得する
  /// @return 最大ディスクリプタ数
  UINT Capacity() const { return capacity_; }

  /// @brief 現在の使用数を取得する
  /// @return 使用済みディスクリプタ数
  UINT Used() const { return offset_; }

  /// @brief 使用状況をリセットする
  /// @note 以前確保したハンドルは無効化されるため、フレームごとの使い捨てヒープなどで使う。
  void Reset() { offset_ = 0; }

private:
  Microsoft::WRL::ComPtr<ID3D12Device> device_;     ///< デバイス
  D3D12_DESCRIPTOR_HEAP_TYPE type_{};               ///< ヒープタイプ
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap_; ///< ディスクリプタヒープ本体
  UINT capacity_ = 0;   ///< 最大容量
  UINT inc_ = 0;        ///< インクリメントサイズ
  UINT offset_ = 0;     ///< 現在のオフセット（次に確保するインデックス）
  bool shaderVisible_ = false; ///< シェーダー参照フラグ
  D3D12_CPU_DESCRIPTOR_HANDLE cpuStart_{}; ///< 先頭 CPU ハンドル
  D3D12_GPU_DESCRIPTOR_HANDLE gpuStart_{}; ///< 先頭 GPU ハンドル（シェーダー参照可能なときのみ有効）
};
