#include "StructuredBufferManager.h"
#include "SRVManager/SRVManager.h"
#include <cassert>

void StructuredBufferManager::Init(SRVManager *srvMgr) {
  assert(srvMgr);
  srvMgr_ = srvMgr;
  device_ = srvMgr_->Device();
  assert(device_);
  entries_.clear();
}

void StructuredBufferManager::Term() {
  // まとめて破棄
  for (int i = 0; i < (int)entries_.size(); ++i) {
    Destroy(static_cast<BufferID>(i));
  }
  entries_.clear();
  device_.Reset();
  srvMgr_ = nullptr;
}

StructuredBufferManager::BufferID
StructuredBufferManager::Create(UINT elementCount, UINT strideBytes) {
  assert(device_);
  assert(srvMgr_);
  assert(elementCount > 0);
  assert(strideBytes > 0);

  // ---- 1) Upload ヒープ上にバッファ作成 ----
  const UINT64 totalSize = static_cast<UINT64>(elementCount) * strideBytes;

  D3D12_HEAP_PROPERTIES heapProps{};
  heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

  D3D12_RESOURCE_DESC resDesc{};
  resDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  resDesc.Width = totalSize;
  resDesc.Height = 1;
  resDesc.DepthOrArraySize = 1;
  resDesc.MipLevels = 1;
  resDesc.Format = DXGI_FORMAT_UNKNOWN;
  resDesc.SampleDesc.Count = 1;
  resDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

  Microsoft::WRL::ComPtr<ID3D12Resource> resource;
  HRESULT hr = device_->CreateCommittedResource(
      &heapProps, D3D12_HEAP_FLAG_NONE, &resDesc,
      D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&resource));
  assert(SUCCEEDED(hr) && "Failed to create structured buffer resource");
  if (resource) {
    resource->SetName(L"StructuredBuffer");
  }

  // ---- 2) SRV 作成（SRVManager 経由）----
  // ここでインデックス・CPU / GPU ハンドルまで全部揃う
  auto handle =
      srvMgr_->CreateStructuredBuffer(resource.Get(), elementCount, strideBytes);

  // ---- 3) 登録 ----
  Entry entry{};
  entry.resource = resource;
  entry.mapped = nullptr;
  entry.srvHandle = handle;
  entry.stride = strideBytes;
  entry.count = elementCount;

  entries_.push_back(entry);
  return static_cast<BufferID>(entries_.size() - 1);
}

void *StructuredBufferManager::Map(BufferID id) {
  if (id < 0 || static_cast<size_t>(id) >= entries_.size())
    return nullptr;
  Entry &e = entries_[id];
  if (!e.resource)
    return nullptr;

  if (!e.mapped) {
    HRESULT hr = e.resource->Map(0, nullptr, &e.mapped);
    assert(SUCCEEDED(hr) && "Failed to map structured buffer");
  }
  return e.mapped;
}

void StructuredBufferManager::Unmap(BufferID id) {
  if (id < 0 || static_cast<size_t>(id) >= entries_.size())
    return;
  Entry &e = entries_[id];
  if (e.resource && e.mapped) {
    e.resource->Unmap(0, nullptr);
    e.mapped = nullptr;
  }
}

D3D12_GPU_DESCRIPTOR_HANDLE
StructuredBufferManager::GetSrv(BufferID id) const {
  if (id < 0 || static_cast<size_t>(id) >= entries_.size()) {
    return D3D12_GPU_DESCRIPTOR_HANDLE{0};
  }
  return entries_[id].srvHandle.gpu;
}

ID3D12Resource *StructuredBufferManager::GetResource(BufferID id) const {
  if (id < 0 || static_cast<size_t>(id) >= entries_.size())
    return nullptr;
  return entries_[id].resource.Get();
}

void StructuredBufferManager::Destroy(BufferID id) {
  if (id < 0 || static_cast<size_t>(id) >= entries_.size())
    return;

  Entry &e = entries_[id];

  // 先にアンマップする
  if (e.resource && e.mapped) {
    e.resource->Unmap(0, nullptr);
    e.mapped = nullptr;
  }

  // SRV スロット返却（※ GPU が参照し終わってから呼ぶ前提）
  if (srvMgr_ && e.srvHandle.IsValid()) {
    srvMgr_->Free(e.srvHandle);
  }
  e.srvHandle = {};

  // リソース破棄
  if (e.resource) {
    e.resource.Reset();
  }

  e.stride = 0;
  e.count = 0;
}
