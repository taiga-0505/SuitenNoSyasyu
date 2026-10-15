#include "ComputeShader.h"

#include "Common/Log/Log.h"
#include "PipelineManager.h"
#include <cassert>
#include <format>

// ============================================================================
// 初期化
// ============================================================================

bool ComputeShader::Initialize(ID3D12Device * /*device*/, PipelineManager *pm,
                               const std::string &pipelineKey) {
  if (!pm) {
    Log::Print("[ComputeShader] PipelineManager is null");
    return false;
  }

  pso_ = pm->GetComputePSO(pipelineKey);
  root_ = pm->GetComputeRoot(pipelineKey);

  if (!pso_ || !root_) {
    Log::Print(std::format(
        "[ComputeShader] Pipeline '{}' not found in PipelineManager",
        pipelineKey));
    return false;
  }

  Log::Print(
      std::format("[ComputeShader] Initialized: {}", pipelineKey));
  return true;
}

// ============================================================================
// パイプラインバインド
// ============================================================================

void ComputeShader::Bind(ID3D12GraphicsCommandList *cl) const {
  assert(cl);
  assert(IsReady());
  cl->SetComputeRootSignature(root_);
  cl->SetPipelineState(pso_);
}

// ============================================================================
// リソースバインド
// ============================================================================

ComputeShader &ComputeShader::SetSRV(ID3D12GraphicsCommandList *cl,
                                     uint32_t rootParamIndex,
                                     D3D12_GPU_VIRTUAL_ADDRESS gpuAddr) {
  assert(cl);
  cl->SetComputeRootShaderResourceView(rootParamIndex, gpuAddr);
  return *this;
}

ComputeShader &ComputeShader::SetUAV(ID3D12GraphicsCommandList *cl,
                                     uint32_t rootParamIndex,
                                     D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle) {
  assert(cl);
  cl->SetComputeRootDescriptorTable(rootParamIndex, gpuHandle);
  return *this;
}

ComputeShader &ComputeShader::SetCBV(ID3D12GraphicsCommandList *cl,
                                     uint32_t rootParamIndex,
                                     D3D12_GPU_VIRTUAL_ADDRESS gpuAddr) {
  assert(cl);
  cl->SetComputeRootConstantBufferView(rootParamIndex, gpuAddr);
  return *this;
}

// ============================================================================
// ディスパッチ
// ============================================================================

void ComputeShader::Dispatch(ID3D12GraphicsCommandList *cl,
                             uint32_t elementCount,
                             uint32_t threadsPerGroup) const {
  assert(cl);
  assert(threadsPerGroup > 0);

  const uint32_t groupCount = (elementCount + threadsPerGroup - 1) / threadsPerGroup;
  cl->Dispatch(groupCount, 1, 1);
}

void ComputeShader::DispatchDirect(ID3D12GraphicsCommandList *cl,
                                   uint32_t groupX, uint32_t groupY,
                                   uint32_t groupZ) const {
  assert(cl);
  cl->Dispatch(groupX, groupY, groupZ);
}

// ============================================================================
// バリア
// ============================================================================

void ComputeShader::UAVBarrier(ID3D12GraphicsCommandList *cl,
                               ID3D12Resource *resource) {
  assert(cl);
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  barrier.UAV.pResource = resource;
  cl->ResourceBarrier(1, &barrier);
}
