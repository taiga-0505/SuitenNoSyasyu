#include "Skydome.h"
#include "../../Render/RenderContext.h"
#include "Common/SceneContext.h"
#include "../../Dx12/Dx12Core.h"
#include "../../Dx12/DeferredReleaseQueue/DeferredReleaseQueue.h"
#include "Math/Math.h"
#include "imgui/imgui.h"
#include <cassert>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

using namespace RC;

Skydome::~Skydome() {
  if (auto ctx = GetRenderContext().Ctx()) {
    auto& dq = ctx->core->DeferredRelease();
    (void)dq;
    // 今記録中のフレームを GPU が終えたら解放する（以前は UINT64_MAX で終了時まで保持していた）
    DeferredReleaseQueue::DeferRelease(std::move(vb_.resource));
    DeferredReleaseQueue::DeferRelease(std::move(ib_.resource));
  } else {
    vb_.resource.Reset();
    ib_.resource.Reset();
  }
}

void Skydome::Initialize(ID3D12Device *device, float radius, UINT sliceCount,
                        UINT stackCount) {
  device_ = device;

  // メッシュ生成
  BuildGeometry(radius, sliceCount, stackCount);
  UploadVB_();
  UploadIB_();

  // CB: WVP
  cbWvp_.dyn.Ptr()->WVP = MakeIdentity4x4();
  cbWvp_.dyn.Ptr()->World = MakeIdentity4x4();
  cbWvp_.dyn.Ptr()->worldInverseTranspose = MakeIdentity4x4();

  // CB: Material
  cbMat_.dyn.Ptr()->color = {1, 1, 1, 1};
  cbMat_.dyn.Ptr()->uvTransform = MakeIdentity4x4();
  cbMat_.dyn.Ptr()->lightingMode = 0; // 天球なので初期はライティング無効

  // CB: Light（天球ごとに持つが一応残す）
  cbLight_.dyn.Ptr()->color = {1, 1, 1, 1};
  cbLight_.dyn.Ptr()->direction = {0.0f, -1.0f, 0.0f};
  cbLight_.dyn.Ptr()->intensity = 1.0f;
  cbMat_.dyn.Ptr()->shininess = 32.0f;
}

void Skydome::Update(const Matrix4x4 &view, const Matrix4x4 &proj) {
  Matrix4x4 world = MakeAffineMatrix(transform_.scale, transform_.rotation,
                                     transform_.translation);
  cbWvp_.dyn.Ptr()->World = world;
  cbWvp_.dyn.Ptr()->WVP = Multiply(world, Multiply(view, proj));
  cbWvp_.dyn.Ptr()->worldInverseTranspose = Transpose(Inverse(world));
}

void Skydome::Draw(ID3D12GraphicsCommandList *cmdList) {
  if (!vb_.resource || !ib_.resource || !visible_)
    return;

  // テクスチャ未解決（ロード待ち等）のまま描くと t0 のディスクリプタテーブルが
  // 未設定になり動作未定義なので、その間は描画をスキップする
  // （SkydomeManager::ApplyTexture が毎フレーム解決を試みる。テクスチャ無しなら white1x1）
  if (textureSrv_.ptr == 0)
    return;

  cmdList->IASetVertexBuffers(0, 1, &vb_.view);
  cmdList->IASetIndexBuffer(&ib_.view);
  cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

  // RootParam: 0:Material, 1:WVP, 2:SRV, 3:Light
  cmdList->SetGraphicsRootConstantBufferView(
      0, cbMat_.dyn.Address());
  cmdList->SetGraphicsRootConstantBufferView(
      1, cbWvp_.dyn.Address());
  cmdList->SetGraphicsRootDescriptorTable(2, textureSrv_);

  // Light CB（b1）: 外部ライトが指定されていればそちらを使う
  const D3D12_GPU_VIRTUAL_ADDRESS lightAddr =
      (externalLightCBAddress_ != 0)
          ? externalLightCBAddress_
          : cbLight_.dyn.Address();
  cmdList->SetGraphicsRootConstantBufferView(3, lightAddr);

  cmdList->DrawIndexedInstanced(ib_.indexCount, 1, 0, 0, 0);
}

void Skydome::Draw(ID3D12GraphicsCommandList *cmdList, const Matrix4x4 &world) {
  auto &ctx = GetRenderContext();
  Draw(cmdList, world, Multiply(ctx.View(), ctx.Proj()));
}

void Skydome::Draw(ID3D12GraphicsCommandList *cmdList, const Matrix4x4 &world,
                   const Matrix4x4 &viewProj) {
  if (!vb_.resource || !ib_.resource || !visible_ || !cbWvp_.dyn.Ptr())
    return;

  cbWvp_.dyn.Ptr()->World = world;
  cbWvp_.dyn.Ptr()->WVP = Multiply(world, viewProj);
  cbWvp_.dyn.Ptr()->worldInverseTranspose = Transpose(Inverse(world));

  Draw(cmdList);
}


void Skydome::BuildGeometry(float radius, UINT sliceCount, UINT stackCount) {
  vertices_.clear();
  indices_.clear();

  // 上極点
  vertices_.push_back({Vector4(0, +radius, 0, 1), Vector2(0.5f, 0.0f)});

  // 中間リング（緯度）
  for (UINT lat = 1; lat < stackCount; ++lat) {
    float phi = lat * (float(M_PI) / float(stackCount));
    float v = float(lat) / float(stackCount);

    for (UINT lon = 0; lon <= sliceCount; ++lon) {
      float theta = lon * (2.0f * float(M_PI) / float(sliceCount));
      float u = float(lon) / float(sliceCount);

      float x = radius * sinf(phi) * cosf(theta);
      float y = radius * cosf(phi);
      float z = radius * sinf(phi) * sinf(theta);

      vertices_.push_back({Vector4(x, y, z, 1), Vector2(u, v)});
    }
  }

  // 下極点
  vertices_.push_back({Vector4(0, -radius, 0, 1), Vector2(0.5f, 1.0f)});

  // 法線（位置の正規化 + 天球なので内側を向くように反転）
  for (auto &v : vertices_) {
    float len =
        std::sqrt(v.position.x * v.position.x + v.position.y * v.position.y +
                  v.position.z * v.position.z);
    if (len > 0.0f) {
      // 天球なので内向き
      v.normal.x = -v.position.x / len;
      v.normal.y = -v.position.y / len;
      v.normal.z = -v.position.z / len;
    } else {
      v.normal = {0.0f, -1.0f, 0.0f};
    }
  }

  // インデックス
  UINT ringVerts = sliceCount + 1;

  // 上極ファン（内向きなので順序を反転させる）
  for (UINT i = 1; i <= sliceCount; ++i) {
    indices_.push_back(0);
    indices_.push_back(i);
    indices_.push_back(i + 1);
  }

  // 中間帯のクアッド
  for (UINT i = 0; i < stackCount - 2; ++i) {
    for (UINT j = 0; j < sliceCount; ++j) {
      UINT a = 1 + i * ringVerts + j;
      UINT b = 1 + i * ringVerts + j + 1;
      UINT c = 1 + (i + 1) * ringVerts + j;
      UINT d = 1 + (i + 1) * ringVerts + j + 1;
      // 内向き
      indices_.push_back(a);
      indices_.push_back(d);
      indices_.push_back(b);
      indices_.push_back(a);
      indices_.push_back(c);
      indices_.push_back(d);
    }
  }

  // 下極ファン
  UINT southPoleIndex = UINT(vertices_.size() - 1);
  UINT baseIndex = southPoleIndex - ringVerts;
  for (UINT i = 0; i < sliceCount; ++i) {
    indices_.push_back(southPoleIndex);
    indices_.push_back(baseIndex + i + 1);
    indices_.push_back(baseIndex + i);
  }
}

void Skydome::UploadVB_() {
  vb_.vertexCount = static_cast<uint32_t>(vertices_.size());
  if (vb_.vertexCount == 0)
    return;

  const UINT sizeBytes = UINT(sizeof(VertexData) * vb_.vertexCount);
  vb_.resource = CreateBufferResource(device_.Get(), sizeBytes, L"Skydome::vb_");

  void *mapped = nullptr;
  vb_.resource->Map(0, nullptr, &mapped);
  std::memcpy(mapped, vertices_.data(), sizeBytes);
  vb_.resource->Unmap(0, nullptr);

  vb_.view.BufferLocation = vb_.resource->GetGPUVirtualAddress();
  vb_.view.SizeInBytes = sizeBytes;
  vb_.view.StrideInBytes = sizeof(VertexData);
}

void Skydome::UploadIB_() {
  ib_.indexCount = static_cast<uint32_t>(indices_.size());
  if (ib_.indexCount == 0)
    return;

  const UINT sizeBytes = UINT(sizeof(uint16_t) * ib_.indexCount);
  ib_.resource = CreateBufferResource(device_.Get(), sizeBytes, L"Skydome::ib_");
  assert(ib_.resource);

  uint16_t *mapped = nullptr;
  ib_.resource->Map(0, nullptr, reinterpret_cast<void **>(&mapped));
  std::memcpy(mapped, indices_.data(), sizeBytes);
  ib_.resource->Unmap(0, nullptr);

  ib_.view.BufferLocation = ib_.resource->GetGPUVirtualAddress();
  ib_.view.Format = DXGI_FORMAT_R16_UINT;
  ib_.view.SizeInBytes = sizeBytes;
}
