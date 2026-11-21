#include "SRVManager.h"
#include "DescriptorHeap/DescriptorHeap.h"

void SRVManager::Init(ID3D12Device *device, DescriptorHeap *srvHeap) {
  assert(device);
  assert(srvHeap);
  device_ = device;
  srvHeap_ = srvHeap;

  free_.clear();
  used_.assign(srvHeap_->Capacity(), 0);
}

void SRVManager::Term() {
  device_ = nullptr;
  srvHeap_ = nullptr;
  free_.clear();
  used_.clear();
}

SRVManager::Handle SRVManager::Allocate() {
  assert(device_);
  assert(srvHeap_);

  UINT idx = UINT_MAX;

  if (!free_.empty()) {
    idx = free_.back();
    free_.pop_back();
  } else {
    // 新規確保：現在の使用数（Used）がそのまま確保されるインデックスになる
    idx = srvHeap_->Used();
    srvHeap_->AllocateCPU(1); // offset_ を進める（DescriptorHeap の基本動作）
  }

  assert(idx < srvHeap_->Capacity());
  if (!used_.empty()) {
    assert(used_[idx] == 0 && "SRV slot double-allocate?");
    used_[idx] = 1;
  }

  Handle h{};
  h.index = idx;
  h.cpu = srvHeap_->CPUAt(idx);
  h.gpu = srvHeap_->GPUAt(idx);
  return h;
}

void SRVManager::Free(const Handle &h) {
  if (!h.IsValid())
    return;
  assert(srvHeap_);
  assert(h.index < srvHeap_->Capacity());

  if (!used_.empty()) {
    assert(used_[h.index] == 1 && "SRV slot double-free?");
    used_[h.index] = 0;
  }
  free_.push_back(h.index);
}

SRVManager::Handle SRVManager::CreateTexture2D(ID3D12Resource *res,
                                               DXGI_FORMAT fmt, UINT mipLevels,
                                               UINT mostDetailedMip) {
  assert(res);
  auto h = Allocate();

  D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
  desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  desc.Format = fmt;
  desc.Texture2D.MostDetailedMip = mostDetailedMip;
  desc.Texture2D.MipLevels = mipLevels;
  desc.Texture2D.ResourceMinLODClamp = 0.0f;

  device_->CreateShaderResourceView(res, &desc, h.cpu);
  return h;
}

SRVManager::Handle SRVManager::CreateTextureCube(ID3D12Resource *res,
                                                 DXGI_FORMAT fmt,
                                                 UINT mipLevels,
                                                 UINT mostDetailedMip) {
  assert(res);
  auto h = Allocate();

  D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
  desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
  desc.Format = fmt;
  desc.TextureCube.MostDetailedMip = mostDetailedMip;
  desc.TextureCube.MipLevels = mipLevels;
  desc.TextureCube.ResourceMinLODClamp = 0.0f;

  device_->CreateShaderResourceView(res, &desc, h.cpu);
  return h;
}

SRVManager::Handle SRVManager::CreateTexture2DUAV(ID3D12Resource *res, DXGI_FORMAT fmt) {
  assert(res);
  auto h = Allocate();

  D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
  uavDesc.Format = fmt;
  uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  uavDesc.Texture2D.MipSlice = 0;

  device_->CreateUnorderedAccessView(res, nullptr, &uavDesc, h.cpu);
  return h;
}

SRVManager::Handle SRVManager::CreateStructuredBuffer(ID3D12Resource *res,
                                                      UINT elementCount,
                                                      UINT strideBytes) {
  assert(res);
  assert(elementCount > 0);
  assert(strideBytes > 0);

  auto h = Allocate();

  D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
  srvDesc.Format = DXGI_FORMAT_UNKNOWN;
  srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
  srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srvDesc.Buffer.FirstElement = 0;
  srvDesc.Buffer.NumElements = elementCount;
  srvDesc.Buffer.StructureByteStride = strideBytes;
  srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

  device_->CreateShaderResourceView(res, &srvDesc, h.cpu);
  return h;
}

SRVManager::Handle SRVManager::CreateStructuredBufferUAV(ID3D12Resource *res,
                                                          UINT elementCount,
                                                          UINT strideBytes) {
  assert(res);
  assert(elementCount > 0);
  assert(strideBytes > 0);

  auto h = Allocate();

  D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
  uavDesc.Format = DXGI_FORMAT_UNKNOWN;
  uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
  uavDesc.Buffer.FirstElement = 0;
  uavDesc.Buffer.NumElements = elementCount;
  uavDesc.Buffer.StructureByteStride = strideBytes;
  uavDesc.Buffer.CounterOffsetInBytes = 0;
  uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;

  device_->CreateUnorderedAccessView(res, nullptr, &uavDesc, h.cpu);
  return h;
}
