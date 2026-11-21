#pragma once
#include <d3d12.h>
#include <wrl.h>
#include <cassert>
#include <mutex>

/// @brief アップロードヒープ上にバッファリソースを作成する
/// @param device      D3D12 デバイス
/// @param sizeInBytes バッファのサイズ（バイト）
/// @param name        デバッグ用のリソース名（nullptr なら付けない）
/// @return 作成したバッファ（状態は D3D12_RESOURCE_STATE_GENERIC_READ）
/// @note 作成に失敗すると assert で停止する。
inline Microsoft::WRL::ComPtr<ID3D12Resource>
CreateBufferResource(ID3D12Device *device, size_t sizeInBytes,
                     const wchar_t *name = nullptr) {
  D3D12_HEAP_PROPERTIES heapProperties{};
  heapProperties.Type = D3D12_HEAP_TYPE_UPLOAD;

  D3D12_RESOURCE_DESC bufferDesc{};
  bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  bufferDesc.Width = sizeInBytes;
  bufferDesc.Height = 1;
  bufferDesc.DepthOrArraySize = 1;
  bufferDesc.MipLevels = 1;
  bufferDesc.SampleDesc.Count = 1;
  bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

  Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
  HRESULT hr = device->CreateCommittedResource(
      &heapProperties, D3D12_HEAP_FLAG_NONE, &bufferDesc,
      D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&buffer));
  assert(SUCCEEDED(hr));

  if (buffer && name) {
    buffer->SetName(name);
  }

  return buffer;
}
