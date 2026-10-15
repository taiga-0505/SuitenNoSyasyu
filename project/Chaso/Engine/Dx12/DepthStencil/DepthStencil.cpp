#include "DepthStencil.h"
#include "DescriptorHeap/DescriptorHeap.h"

void DepthStencil::Init(ID3D12Device *device, UINT width, UINT height,
                        DescriptorHeap &dsvHeap, DXGI_FORMAT texFormat,
                        DXGI_FORMAT dsvFormat) {
  assert(device);
  device_ = device;
  width_ = width;
  height_ = height;
  texFormat_ = texFormat;
  dsvFormat_ = dsvFormat;

  createResource();
  createView(dsvHeap);
}

void DepthStencil::Resize(UINT width, UINT height, DescriptorHeap &dsvHeap) {
  if (!device_)
    return;
  width_ = width;
  height_ = height;

  if (tex_) {
    tex_.Reset();
  }
  // DSV のスロットは再確保せず、同じ場所を上書きする
  // （別スロットにしたい場合は dsvHeap.AllocateCPU() し直してもよい）
  createResource();
  // 既存スロットに上書き
  D3D12_DEPTH_STENCIL_VIEW_DESC desc{};
  desc.Format = dsvFormat_;
  desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
  device_->CreateDepthStencilView(tex_.Get(), &desc, dsv_);
}

void DepthStencil::Term() {
  if (tex_) {
    tex_.Reset();
  }
  device_.Reset();
  dsv_ = {};
}

void DepthStencil::Clear(ID3D12GraphicsCommandList *list, float depth,
                         UINT8 stencil) const {
  list->ClearDepthStencilView(dsv_,
                              D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                              depth, stencil, 0, nullptr);
}

void DepthStencil::createResource() {
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width_;
  desc.Height = height_;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = texFormat_;
  desc.SampleDesc.Count = 1;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

  D3D12_CLEAR_VALUE clear{};
  clear.Format = dsvFormat_;
  clear.DepthStencil = {1.0f, 0};

  D3D12_HEAP_PROPERTIES heapProps{D3D12_HEAP_TYPE_DEFAULT};

  HRESULT hr = device_->CreateCommittedResource(
      &heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_DEPTH_WRITE,
      &clear, IID_PPV_ARGS(&tex_));
  assert(SUCCEEDED(hr));
}

void DepthStencil::createView(DescriptorHeap &dsvHeap) {
  // 外部ヒープから 1 スロット確保して DSV を作る
  dsv_ = dsvHeap.AllocateCPU();

  D3D12_DEPTH_STENCIL_VIEW_DESC desc{};
  desc.Format = dsvFormat_;
  desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;

  device_->CreateDepthStencilView(tex_.Get(), &desc, dsv_);
}
