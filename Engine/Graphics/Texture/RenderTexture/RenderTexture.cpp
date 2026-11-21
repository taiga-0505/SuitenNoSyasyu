#include "RenderTexture.h"
#include "Dx12Core.h"

void RenderTexture::Initialize(Dx12Core *dxCore, uint32_t width,
                               uint32_t height, DXGI_FORMAT format,
                               const RC::Vector4& clearColor) {
  auto device = dxCore->GetDevice();

  // 1. リソースの作成（初期状態を COMMON にして確実にバリアが張られるようにする）
  resource_ = CreateRenderTextureResource(device, width, height, format,
                                          clearColor);
  resource_->SetName(L"RenderTexture");

  // 2. RTV の作成
  D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
  rtvDesc.Format = format;
  rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
  rtvDesc.Texture2D.MipSlice = 0;
  rtvDesc.Texture2D.PlaneSlice = 0;

  if (rtvHandle_.ptr == 0) {
      rtvHandle_ = dxCore->RTV().AllocateCPU();
  }
  device->CreateRenderTargetView(resource_.Get(), &rtvDesc, rtvHandle_);

  // 3. SRV の作成
  D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
  srvDesc.Format = format;
  srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srvDesc.Texture2D.MipLevels = 1;

  if (srvHandleCPU_.ptr == 0) {
      UINT srvIndex = dxCore->SRV().Used();
      srvHandleCPU_ = dxCore->SRV().AllocateCPU();
      srvHandleGPU_ = dxCore->SRV().GPUAt(srvIndex);
  }

  device->CreateShaderResourceView(resource_.Get(), &srvDesc, srvHandleCPU_);

  // 初期の状態を設定（作成時は COMMON）
  currentState_ = D3D12_RESOURCE_STATE_COMMON;
}

// 汎用的な状態遷移メソッド
void RenderTexture::Transition(ID3D12GraphicsCommandList *cmdList,
                               D3D12_RESOURCE_STATES nextState) {
  // 状態が同じなら無駄なバリアを張らない
  if (currentState_ == nextState) {
    return;
  }

  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
  barrier.Transition.pResource = resource_.Get();
  barrier.Transition.StateBefore = currentState_;
  barrier.Transition.StateAfter = nextState;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

  cmdList->ResourceBarrier(1, &barrier);

  // 状態を更新
  currentState_ = nextState;
}

void RenderTexture::TransitionToRenderTarget(ID3D12GraphicsCommandList *cmdList) {
  Transition(cmdList, D3D12_RESOURCE_STATE_RENDER_TARGET);
}

void RenderTexture::TransitionToShaderResource(ID3D12GraphicsCommandList *cmdList) {
  Transition(cmdList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}


Microsoft::WRL::ComPtr<ID3D12Resource>
RenderTexture::CreateRenderTextureResource(
    Microsoft::WRL::ComPtr<ID3D12Device> device, UINT width, UINT height,
    DXGI_FORMAT format, const RC::Vector4 &clearColor) {
  D3D12_RESOURCE_DESC resourceDesc{};
  resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  resourceDesc.Width = width;
  resourceDesc.Height = height;
  resourceDesc.DepthOrArraySize = 1;
  resourceDesc.MipLevels = 1;
  resourceDesc.Format = format;
  resourceDesc.SampleDesc.Count = 1;
  resourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

  D3D12_HEAP_PROPERTIES heapPropaties{};
  heapPropaties.Type = D3D12_HEAP_TYPE_DEFAULT; // VRAM 上に置く

  // クリア値の設定（既定値は青系）
  D3D12_CLEAR_VALUE clearValue{};
  clearValue.Format = format;
  clearValue.Color[0] = clearColor.x;
  clearValue.Color[1] = clearColor.y;
  clearValue.Color[2] = clearColor.z;
  clearValue.Color[3] = clearColor.w;

  Microsoft::WRL::ComPtr<ID3D12Resource> resource;
  device->CreateCommittedResource(
      &heapPropaties, D3D12_HEAP_FLAG_NONE, &resourceDesc,
      D3D12_RESOURCE_STATE_COMMON, &clearValue, IID_PPV_ARGS(&resource));

  return resource;
}
