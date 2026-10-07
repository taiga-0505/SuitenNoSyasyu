#include "PrimitiveMesh.h"
#include "RenderContext.h"
#include "Common/SceneContext.h"
#include "../../Dx12/Dx12Core.h"
#include "../../Dx12/DeferredReleaseQueue/DeferredReleaseQueue.h"
#include "Math/Math.h"
#include "imgui/imgui.h"
#include <cassert>
#include <cstring>
#include <string>

using namespace RC;

PrimitiveMesh::~PrimitiveMesh() {
  // 実行中フレームがまだこのメッシュを参照している可能性があるため、
  // GPU リソースは即時解放せず「現在記録中のコマンドが完了するフェンス値」で遅延解放する。
  // （TextMesh の再生成のように、描画中のメッシュをフレーム途中で作り直すケースの安全策）
  SceneContext *ctx = GetRenderContext().Ctx();
  if (ctx && ctx->core) {
    auto &dq = ctx->core->DeferredRelease();
    const uint64_t fence = ctx->core->GetNextFenceValue();
    if (vb_.resource) dq.Enqueue(std::move(vb_.resource), fence);
    if (ib_.resource) dq.Enqueue(std::move(ib_.resource), fence);
  } else {
    vb_.resource.Reset();
    ib_.resource.Reset();
  }
}

void PrimitiveMesh::Initialize(ID3D12Device *device, const ModelData &data) {
  device_ = device;

  UploadVB_(data.vertices);
  UploadIB_(data.indices);

  // CB: WVP / Material は DynamicCB（CPU 側に値を持ち、バインド時に今フレームの領域へ送る）
  cbMat_.dyn.Ptr()->color = {1, 1, 1, 1};
  cbMat_.dyn.Ptr()->uvTransform = MakeIdentity4x4();
  cbMat_.dyn.Ptr()->lightingMode = 2; // Half Lambert 既定
  cbMat_.dyn.Ptr()->shininess = 32.0f;
}

void PrimitiveMesh::Draw(ID3D12GraphicsCommandList *cmdList) {
  if (!vb_.resource || !visible_)
    return;

  cmdList->IASetVertexBuffers(0, 1, &vb_.view);
  if (ib_.resource) {
    cmdList->IASetIndexBuffer(&ib_.view);
  }
  cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

  cbMat_.dyn.Ptr()->useNormalMap = (normalMapSrv_.ptr != 0) ? 1 : 0;
  cbMat_.dyn.Ptr()->useRoughnessMap = (roughnessMapSrv_.ptr != 0) ? 1 : 0;

  // RootParam: 0:Material, 1:WVP, 2:SRV, 3:Light
  cmdList->SetGraphicsRootConstantBufferView(0, cbMat_.dyn.Address());
  cmdList->SetGraphicsRootConstantBufferView(1, cbWvp_.dyn.Address());
  const D3D12_GPU_DESCRIPTOR_HANDLE mainSrv = textureSrv_.ptr != 0 ? textureSrv_ : D3D12_GPU_DESCRIPTOR_HANDLE{};
  if (mainSrv.ptr != 0) {
    cmdList->SetGraphicsRootDescriptorTable(2, mainSrv);
  }
  // Slot 9 is NormalMap
  if (normalMapSrv_.ptr != 0) {
    cmdList->SetGraphicsRootDescriptorTable(9, normalMapSrv_);
  } else if (mainSrv.ptr != 0) {
    cmdList->SetGraphicsRootDescriptorTable(9, mainSrv);
  }
  // Slot 10 is RoughnessMap
  if (roughnessMapSrv_.ptr != 0) {
    cmdList->SetGraphicsRootDescriptorTable(10, roughnessMapSrv_);
  } else if (mainSrv.ptr != 0) {
    cmdList->SetGraphicsRootDescriptorTable(10, mainSrv);
  }

  // PrimitiveMesh ではライト管理がまだ簡易。
  // 必要に応じて RenderContext から 共通ライトを取得してバインド
}

void PrimitiveMesh::DrawInstancedPrepared(ID3D12GraphicsCommandList *cmdList,
                                          D3D12_GPU_VIRTUAL_ADDRESS materialCB,
                                          D3D12_GPU_VIRTUAL_ADDRESS lightCB,
                                          D3D12_GPU_VIRTUAL_ADDRESS instanceAddr,
                                          uint32_t count) {
  if (!vb_.resource || count == 0 || instanceAddr == 0 || materialCB == 0) {
    return;
  }

  cmdList->IASetVertexBuffers(0, 1, &vb_.view);
  if (ib_.resource) {
    cmdList->IASetIndexBuffer(&ib_.view);
  }
  cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

  // RootParam (Object3DInstancing): 0:Material, 1:Instance SRV, 2:Texture, 3:DirectionalLight
  cmdList->SetGraphicsRootConstantBufferView(0, materialCB);
  cmdList->SetGraphicsRootShaderResourceView(1, instanceAddr);
  if (lightCB != 0) {
    cmdList->SetGraphicsRootConstantBufferView(3, lightCB);
  }
  if (textureSrv_.ptr != 0) {
    cmdList->SetGraphicsRootDescriptorTable(2, textureSrv_);
  }
  const D3D12_GPU_DESCRIPTOR_HANDLE normalSrv = (normalMapSrv_.ptr != 0) ? normalMapSrv_ : textureSrv_;
  const D3D12_GPU_DESCRIPTOR_HANDLE roughSrv = (roughnessMapSrv_.ptr != 0) ? roughnessMapSrv_ : textureSrv_;
  if (normalSrv.ptr != 0) {
    cmdList->SetGraphicsRootDescriptorTable(9, normalSrv);
  }
  if (roughSrv.ptr != 0) {
    cmdList->SetGraphicsRootDescriptorTable(10, roughSrv);
  }

  if (ib_.resource) {
    cmdList->DrawIndexedInstanced(ib_.indexCount, count, 0, 0, 0);
  } else {
    cmdList->DrawInstanced(vb_.vertexCount, count, 0, 0);
  }
}

void PrimitiveMesh::Draw(ID3D12GraphicsCommandList *cmdList, const RC::Matrix4x4 &world) {
  if (!vb_.resource || !visible_)
    return;

  auto &ctx = GetRenderContext();
  cbWvp_.dyn.Ptr()->World = world;
  if (!ctx.IsShadowPass()) {
    // シャドウパスの VS は World しか読まないので、WVP / 逆転置行列（4x4 逆行列）は
    // 通常パスのときだけ計算する（影タイル数ぶん毎フレーム繰り返されるため）
    Matrix4x4 vp = Multiply(ctx.View(), ctx.Proj());
    cbWvp_.dyn.Ptr()->WVP = Multiply(world, vp);
    cbWvp_.dyn.Ptr()->worldInverseTranspose = Transpose(Inverse(world));
  }

  // IA
  cmdList->IASetVertexBuffers(0, 1, &vb_.view);
  if (ib_.resource) {
    cmdList->IASetIndexBuffer(&ib_.view);
  }
  cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

  cbMat_.dyn.Ptr()->useNormalMap = (normalMapSrv_.ptr != 0) ? 1 : 0;
  cbMat_.dyn.Ptr()->useRoughnessMap = (roughnessMapSrv_.ptr != 0) ? 1 : 0;

  // RootParam: 0:Material, 1:WVP, 2:SRV, 3:Light
  cmdList->SetGraphicsRootConstantBufferView(0, cbMat_.dyn.Address());
  cmdList->SetGraphicsRootConstantBufferView(1, cbWvp_.dyn.Address());
  const D3D12_GPU_DESCRIPTOR_HANDLE mainSrv2 = textureSrv_.ptr != 0 ? textureSrv_ : D3D12_GPU_DESCRIPTOR_HANDLE{};
  if (mainSrv2.ptr != 0) {
    cmdList->SetGraphicsRootDescriptorTable(2, mainSrv2);
  }
  if (normalMapSrv_.ptr != 0) {
    cmdList->SetGraphicsRootDescriptorTable(9, normalMapSrv_);
  } else if (mainSrv2.ptr != 0) {
    cmdList->SetGraphicsRootDescriptorTable(9, mainSrv2);
  }
  if (roughnessMapSrv_.ptr != 0) {
    cmdList->SetGraphicsRootDescriptorTable(10, roughnessMapSrv_);
  } else if (mainSrv2.ptr != 0) {
    cmdList->SetGraphicsRootDescriptorTable(10, mainSrv2);
  }

  if (ib_.resource) {
    cmdList->DrawIndexedInstanced(ib_.indexCount, 1, 0, 0, 0);
  } else {
    cmdList->DrawInstanced(vb_.vertexCount, 1, 0, 0);
  }
}

void PrimitiveMesh::UploadVB_(const std::vector<VertexData> &vertices) {
  vb_.vertexCount = static_cast<uint32_t>(vertices.size());
  if (vb_.vertexCount == 0)
    return;

  const UINT sizeBytes = UINT(sizeof(VertexData) * vb_.vertexCount);
  vb_.resource = CreateBufferResource(device_.Get(), sizeBytes, L"PrimitiveMesh::vb_");

  void *mapped = nullptr;
  vb_.resource->Map(0, nullptr, &mapped);
  std::memcpy(mapped, vertices.data(), sizeBytes);
  vb_.resource->Unmap(0, nullptr);

  vb_.view.BufferLocation = vb_.resource->GetGPUVirtualAddress();
  vb_.view.SizeInBytes = sizeBytes;
  vb_.view.StrideInBytes = sizeof(VertexData);
}

void PrimitiveMesh::UploadIB_(const std::vector<uint32_t> &indices) {
  ib_.indexCount = static_cast<uint32_t>(indices.size());
  if (ib_.indexCount == 0)
    return;

  const UINT sizeBytes = UINT(sizeof(uint32_t) * ib_.indexCount);
  ib_.resource = CreateBufferResource(device_.Get(), sizeBytes, L"PrimitiveMesh::ib_");

  uint32_t *mapped = nullptr;
  ib_.resource->Map(0, nullptr, reinterpret_cast<void **>(&mapped));
  std::memcpy(mapped, indices.data(), sizeBytes);
  ib_.resource->Unmap(0, nullptr);

  ib_.view.BufferLocation = ib_.resource->GetGPUVirtualAddress();
  ib_.view.Format = DXGI_FORMAT_R32_UINT;
  ib_.view.SizeInBytes = sizeBytes;
}
