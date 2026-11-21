#include "ShadowMap.h"
#include "../Dx12Core.h"
#include <stdexcept>

ShadowMap::ShadowMap() {}

ShadowMap::~ShadowMap() {
  Term();
}

void ShadowMap::Term() {
  resource_.Reset();
  dsvHeap_.Term();
  if (core_ && srvIndex_ > 0) {
    SRVManager::Handle h;
    h.index = srvIndex_;
    core_->SRVMan().Free(h);
    srvIndex_ = 0;
  }
  core_ = nullptr;
}

void ShadowMap::Create(Dx12Core* core, uint32_t width, uint32_t height) {
  core_ = core;
  width_ = width;
  height_ = height;
  auto device = core->GetDevice();

  // 1. テクスチャリソースの作成（DSV と SRV で別フォーマットを使うため R32_TYPELESS）
  D3D12_RESOURCE_DESC texDesc = {};
  texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  texDesc.Alignment = 0;
  texDesc.Width = width;
  texDesc.Height = height;
  texDesc.DepthOrArraySize = 1;
  texDesc.MipLevels = 1;
  texDesc.Format = DXGI_FORMAT_R32_TYPELESS;
  texDesc.SampleDesc.Count = 1;
  texDesc.SampleDesc.Quality = 0;
  texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

  D3D12_CLEAR_VALUE clearValue = {};
  clearValue.Format = DXGI_FORMAT_D32_FLOAT;
  clearValue.DepthStencil.Depth = 1.0f;
  clearValue.DepthStencil.Stencil = 0;

  D3D12_HEAP_PROPERTIES heapProps = {};
  heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
  heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
  heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

  HRESULT hr = device->CreateCommittedResource(
      &heapProps,
      D3D12_HEAP_FLAG_NONE,
      &texDesc,
      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
      &clearValue,
      IID_PPV_ARGS(&resource_));
  
  if (FAILED(hr)) {
      throw std::runtime_error("Failed to create ShadowMap resource.");
  }
  resource_->SetName(L"ShadowMap Resource");

  // 2. DSV ヒープの作成と DSV 生成
  dsvHeap_.Init(device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, false);
  D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
  dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
  dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
  dsvDesc.Flags = D3D12_DSV_FLAG_NONE;
  device->CreateDepthStencilView(resource_.Get(), &dsvDesc, dsvHeap_.CPUAt(0));

  // 3. SRV の生成
  auto srvHandle = core->SRVMan().Allocate();
  srvIndex_ = srvHandle.index;
  D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
  srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
  srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srvDesc.Texture2D.MipLevels = 1;
  
  device->CreateShaderResourceView(resource_.Get(), &srvDesc, srvHandle.cpu);
}

void ShadowMap::BindAndClear(ID3D12GraphicsCommandList* cmd) {
  // リソースの状態を DEPTH_WRITE に変更
  D3D12_RESOURCE_BARRIER barrier = {};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = resource_.Get();
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  cmd->ResourceBarrier(1, &barrier);

  auto dsvHandle = dsvHeap_.CPUAt(0);
  cmd->OMSetRenderTargets(0, nullptr, FALSE, &dsvHandle);
  cmd->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

  // ビューポートとシザー
  D3D12_VIEWPORT vp = {0.0f, 0.0f, static_cast<float>(width_), static_cast<float>(height_), 0.0f, 1.0f};
  D3D12_RECT scissor = {0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_)};
  cmd->RSSetViewports(1, &vp);
  cmd->RSSetScissorRects(1, &scissor);
}

void ShadowMap::TransitionToSRV(ID3D12GraphicsCommandList* cmd) {
  D3D12_RESOURCE_BARRIER barrier = {};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = resource_.Get();
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  cmd->ResourceBarrier(1, &barrier);
}
