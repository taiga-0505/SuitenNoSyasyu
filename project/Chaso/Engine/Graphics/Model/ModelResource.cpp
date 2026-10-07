#include "ModelResource.h"
#include "Render/FrameResource.h"
#include "DeferredReleaseQueue/DeferredReleaseQueue.h"
#include "Texture/TextureManager/TextureManager.h"
#include "SRVManager/SRVManager.h"
#include "Common/Log/Log.h"
#include "GraphicsPipeline/GraphicsPipeline.h" // Object3DRootParam
#include <algorithm>
#include <cassert>
#include <cstring>
#include <format>

using namespace RC;

ModelResource::~ModelResource() {
  // 定数バッファは DynamicCB（毎フレームの一時領域）なので解放するものは無い。
  // CS スキニング用の GPU リソースは、直前のフレームの GPU がまだ使っている可能性があるので遅延解放する
  DeferredReleaseQueue::DeferRelease(std::move(skinnedVertexBuffer_));
  DeferredReleaseQueue::DeferRelease(std::move(skinningInfoCB_));
}

void ModelResource::Initialize(ID3D12Device *device) {
  device_ = device;

  // Material CB
  cbMat_.dyn.Ptr()->color = {1, 1, 1, 1};
  cbMat_.dyn.Ptr()->lightingMode = 2; // 既定 HalfLambert
  cbMat_.dyn.Ptr()->uvTransform = MakeIdentity4x4();

  // padding 初期化（ガラスでは environmentCoefficient=IOR, padding=roughness として使う）
  cbMat_.dyn.Ptr()->environmentCoefficient = 0.0f; // 通常モデル: 映り込みなし / Glass: IOR（0ならPS側で1.5扱い）
  cbMat_.dyn.Ptr()->useNormalMap = 0;
  cbMat_.dyn.Ptr()->useRoughnessMap = 0;
  cbMat_.dyn.Ptr()->padding[0] = 0.0f;                // Glass: roughness

  // Light CB（各Objectが自前で持つ）
  cbLight_.dyn.Ptr()->color = {1, 1, 1, 1};
  cbLight_.dyn.Ptr()->direction = {0.0f, -1.0f, 0.0f};
  cbLight_.dyn.Ptr()->intensity = 1.0f;
  cbMat_.dyn.Ptr()->shininess = 32.0f;
}

void ModelResource::SetMesh(const std::shared_ptr<ModelMesh> &mesh) {
  // 差し替え前のメッシュを直前のフレームの GPU がまだ使っているかもしれないので、
  // 最後の参照がここで消えても、GPU が使い終わるまで頂点バッファを生かしておく
  if (mesh_ && mesh_ != mesh) {
    DeferredReleaseQueue::DeferDelete(mesh_);
  }
  mesh_ = mesh;
  // meshが変わったらMaterial SRVキャッシュは破棄
  materialSrvs_.clear();
  // overrideも一旦クリア
  textureSrv_ = {};
}

void ModelResource::ResetTextureToMtl() {
  // override を外すだけ。マテリアル由来の SRV キャッシュ（materialSrvs_）は mesh が同じなら
  // 変わらないので破棄しない。以前はここで clear → 全マテリアルを TextureManager::Load し直しており、
  // DrawModel はテクスチャ override が無いとき毎ドローこの関数を呼ぶため
  // （影パス × 全モデルで毎フレーム数千回）、mutex 取得とパス正規化の文字列確保が積み重なっていた。
  textureSrv_ = {};
  EnsureMaterialSrvsLoaded_(); // 未解決なら（初回・SetMesh 後）ここでロードする
}

void ModelResource::ApplyLighting(int lightingMode, const float color[3],
                                  const float dir[3], float intensity) {
  if (cbMat_.dyn.Ptr()) {
    cbMat_.dyn.Ptr()->lightingMode = lightingMode;
  }
  if (cbLight_.dyn.Ptr()) {
    cbLight_.dyn.Ptr()->color = {color[0], color[1], color[2], 1.0f};
    cbLight_.dyn.Ptr()->direction = {dir[0], dir[1], dir[2]};
    cbLight_.dyn.Ptr()->intensity = intensity;
  }
}

// ============================================================================
// 描画ヘルパー
// ============================================================================

void ModelResource::EnsureMaterialSrvsLoaded_() {
  if (!texman_ || !mesh_) {
    return;
  }

  const auto &mats = mesh_->Materials();
  if (mats.empty()) {
    // 互換：昔の1枚だけ
    const auto &mtl = mesh_->MaterialFile();
    if (mtl.textureFilePath.empty()) {
      materialSrvs_.clear();
      return;
    }
    // 既に解決済みなら何もしない。
    // 以前はここに早期 return が無く、ドローごとに TextureManager::Load
    // （mutex＋パス正規化＋キャッシュ検索）を呼び直していた
    if (materialSrvs_.size() == 1 && materialSrvs_[0].ptr != 0) {
      return;
    }
    materialSrvs_.assign(1, D3D12_GPU_DESCRIPTOR_HANDLE{});
    materialSrvs_[0] = texman_->Load(mtl.textureFilePath, /*srgb=*/true);
    return;
  }

  if (!materialSrvs_.empty() && materialSrvs_.size() == mats.size()) {
    return; // 既にロード済み
  }

  materialSrvs_.assign(mats.size(), D3D12_GPU_DESCRIPTOR_HANDLE{});
  for (uint32_t i = 0; i < mats.size(); ++i) {
    if (mats[i].textureFilePath.empty())
      continue;

    const D3D12_GPU_DESCRIPTOR_HANDLE srv =
        texman_->Load(mats[i].textureFilePath, /*srgb=*/true);
    materialSrvs_[i] = (srv.ptr == 0) ? D3D12_GPU_DESCRIPTOR_HANDLE{} : srv;
  }
}

D3D12_GPU_DESCRIPTOR_HANDLE
ModelResource::GetSrvForMaterial_(uint32_t materialIndex) const {
  if (!materialSrvs_.empty() && materialIndex < materialSrvs_.size() &&
      materialSrvs_[materialIndex].ptr != 0) {
    return materialSrvs_[materialIndex];
  }

  // fallback：先頭の有効SRV
  for (const auto &h : materialSrvs_) {
    if (h.ptr != 0)
      return h;
  }

  if (texman_) {
      return texman_->GetSrv(-1);
  }

  return D3D12_GPU_DESCRIPTOR_HANDLE{};
}

// ============================================================================
// Draw（単発描画）
// ============================================================================

void ModelResource::Draw(ID3D12GraphicsCommandList *cmdList,
                         const Matrix4x4 &world, const Matrix4x4 &view,
                         const Matrix4x4 &proj, FrameResource &frame,
                         bool worldOnly) {
  if (!isReady_ || !mesh_ || !mesh_->Ready())
    return;

  // Node階層を使う場合は DrawItem を使う
  const auto &items = mesh_->DrawItems();

  // view * proj はループ不変なので 1 回だけ計算する（シャドウパスでは使わない）
  const Matrix4x4 viewProj = worldOnly ? MakeIdentity4x4() : Multiply(view, proj);

  // テクスチャ（overrideが無い場合だけ materialIndex対応を準備）
  if (textureSrv_.ptr == 0) {
    EnsureMaterialSrvsLoaded_();
  }

  const auto &vbv = mesh_->VBV();
  cmdList->IASetVertexBuffers(0, 1, &vbv);
  cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

  cbMat_.dyn.Ptr()->useNormalMap = (normalMapSrv_.ptr != 0) ? 1 : 0;
  cbMat_.dyn.Ptr()->useRoughnessMap = (roughnessMapSrv_.ptr != 0) ? 1 : 0;

  cmdList->SetGraphicsRootConstantBufferView(
      0, cbMat_.dyn.Address());

  // Light CB（b1）: 外部ライトが指定されていればそちらを使う
  const D3D12_GPU_VIRTUAL_ADDRESS lightAddr =
      (externalLightCBAddress_ != 0)
          ? externalLightCBAddress_
          : cbLight_.dyn.Address();
  cmdList->SetGraphicsRootConstantBufferView(3, lightAddr);

  if (items.empty()) {
    // 互換：DrawItemが無い場合は全頂点を1発
    void *dst = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS addr =
        frame.AllocCB(sizeof(TransformationMatrix), &dst);

    auto *tm = reinterpret_cast<TransformationMatrix *>(dst);
    tm->World = world;
    if (!worldOnly) {
      tm->WVP = Multiply(world, viewProj);
      tm->worldInverseTranspose = Transpose(Inverse(world));
    }

    cmdList->SetGraphicsRootConstantBufferView(1, addr);

    // texture
    const D3D12_GPU_DESCRIPTOR_HANDLE mainSrv =
        (textureSrv_.ptr != 0) ? textureSrv_ : GetSrvForMaterial_(0);
    cmdList->SetGraphicsRootDescriptorTable(2, mainSrv);
    cmdList->SetGraphicsRootDescriptorTable(9, (normalMapSrv_.ptr != 0) ? normalMapSrv_ : mainSrv);
    cmdList->SetGraphicsRootDescriptorTable(10, (roughnessMapSrv_.ptr != 0) ? roughnessMapSrv_ : mainSrv);

    if (mesh_->HasIndexBuffer()) {
      cmdList->IASetIndexBuffer(&mesh_->IBV());
      cmdList->DrawIndexedInstanced(mesh_->IndexCount(), 1, 0, 0, 0);
    } else {
      cmdList->DrawInstanced(mesh_->VertexCount(), 1, 0, 0);
    }
    return;
  }

  // IndexBufferがある場合はIASetを事前に行う
  if (mesh_->HasIndexBuffer()) {
    cmdList->IASetIndexBuffer(&mesh_->IBV());
  }

  for (uint32_t i = 0; i < items.size(); ++i) {
    const auto &it = items[i];

    // Node行列（モデル空間）→ world を掛ける
    const Matrix4x4 nodeWorld = Multiply(it.nodeWorld, world);

    // FrameResource から CB 領域を確保
    void *dst = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS addr =
        frame.AllocCB(sizeof(TransformationMatrix), &dst);

    auto *tm = reinterpret_cast<TransformationMatrix *>(dst);
    tm->World = nodeWorld;
    if (!worldOnly) {
      tm->WVP = Multiply(nodeWorld, viewProj);
      tm->worldInverseTranspose = Transpose(Inverse(nodeWorld));
    }

    cmdList->SetGraphicsRootConstantBufferView(1, addr);

    // テクスチャ（override優先）
    const D3D12_GPU_DESCRIPTOR_HANDLE mainSrv =
        (textureSrv_.ptr != 0) ? textureSrv_
                               : GetSrvForMaterial_(it.materialIndex);
    cmdList->SetGraphicsRootDescriptorTable(2, mainSrv);
    cmdList->SetGraphicsRootDescriptorTable(9, (normalMapSrv_.ptr != 0) ? normalMapSrv_ : mainSrv);
    cmdList->SetGraphicsRootDescriptorTable(10, (roughnessMapSrv_.ptr != 0) ? roughnessMapSrv_ : mainSrv);

    // 描画
    if (mesh_->HasIndexBuffer() && it.indexCount > 0) {
      cmdList->DrawIndexedInstanced(it.indexCount, 1, it.indexStart, 0, 0);
    } else {
      cmdList->DrawInstanced(it.vertexCount, 1, it.vertexStart, 0);
    }
  }
}

// ============================================================================
// DrawInstancesPrepared（ModelInstanceBatcher 用。インスタンスデータは呼び出し側で作成済み）
// ============================================================================

void ModelResource::DrawInstancesPrepared(ID3D12GraphicsCommandList *cmdList,
                                          D3D12_GPU_VIRTUAL_ADDRESS materialCB,
                                          D3D12_GPU_VIRTUAL_ADDRESS instanceAddr,
                                          uint32_t count, bool perItem) {
  if (!isReady_ || !mesh_ || !mesh_->Ready() || count == 0 || instanceAddr == 0) {
    return;
  }

  if (textureSrv_.ptr == 0) {
    EnsureMaterialSrvsLoaded_();
  }

  const auto &vbv = mesh_->VBV();
  cmdList->IASetVertexBuffers(0, 1, &vbv);
  cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

  cmdList->SetGraphicsRootConstantBufferView(0, materialCB);
  cmdList->SetGraphicsRootConstantBufferView(3, EffectiveLightCBAddress());

  auto bindTextures = [&](uint32_t materialIndex) {
    const D3D12_GPU_DESCRIPTOR_HANDLE mainSrv =
        (textureSrv_.ptr != 0) ? textureSrv_ : GetSrvForMaterial_(materialIndex);
    cmdList->SetGraphicsRootDescriptorTable(2, mainSrv);
    cmdList->SetGraphicsRootDescriptorTable(9, (normalMapSrv_.ptr != 0) ? normalMapSrv_ : mainSrv);
    cmdList->SetGraphicsRootDescriptorTable(10, (roughnessMapSrv_.ptr != 0) ? roughnessMapSrv_ : mainSrv);
  };

  const auto &items = mesh_->DrawItems();
  if (items.empty()) {
    cmdList->SetGraphicsRootShaderResourceView(1, instanceAddr);
    bindTextures(0);
    if (mesh_->HasIndexBuffer()) {
      cmdList->IASetIndexBuffer(&mesh_->IBV());
      cmdList->DrawIndexedInstanced(mesh_->IndexCount(), count, 0, 0, 0);
    } else {
      cmdList->DrawInstanced(mesh_->VertexCount(), count, 0, 0);
    }
    return;
  }

  if (mesh_->HasIndexBuffer()) {
    cmdList->IASetIndexBuffer(&mesh_->IBV());
  }
  const uint64_t itemStride = static_cast<uint64_t>(count) * sizeof(InstanceGPU);
  if (!perItem) {
    cmdList->SetGraphicsRootShaderResourceView(1, instanceAddr);
  }
  for (size_t k = 0; k < items.size(); ++k) {
    const auto &it = items[k];
    if (perItem) {
      cmdList->SetGraphicsRootShaderResourceView(1, instanceAddr + itemStride * k);
    }
    bindTextures(it.materialIndex);
    if (mesh_->HasIndexBuffer() && it.indexCount > 0) {
      cmdList->DrawIndexedInstanced(it.indexCount, count, it.indexStart, 0, 0);
    } else {
      cmdList->DrawInstanced(it.vertexCount, count, it.vertexStart, 0);
    }
  }
}

// ============================================================================
// DrawBatch（インスタンシング・白色版）
// ============================================================================

void ModelResource::DrawBatch(ID3D12GraphicsCommandList *cmdList,
                              const Matrix4x4 &view, const Matrix4x4 &proj,
                              const std::vector<Transform> &instances,
                              FrameResource &frame, bool worldOnly) {
  if (!isReady_ || !mesh_ || !mesh_->Ready() || instances.empty()) {
    return;
  }

  const auto &items = mesh_->DrawItems();

  const auto &vbv = mesh_->VBV();
  cmdList->IASetVertexBuffers(0, 1, &vbv);
  cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

  cbMat_.dyn.Ptr()->useNormalMap = (normalMapSrv_.ptr != 0) ? 1 : 0;
  cbMat_.dyn.Ptr()->useRoughnessMap = (roughnessMapSrv_.ptr != 0) ? 1 : 0;

  cmdList->SetGraphicsRootConstantBufferView(
      0, cbMat_.dyn.Address());

  if (textureSrv_.ptr == 0) {
    EnsureMaterialSrvsLoaded_();
  }

  const D3D12_GPU_VIRTUAL_ADDRESS lightAddr =
      (externalLightCBAddress_ != 0)
          ? externalLightCBAddress_
          : cbLight_.dyn.Address();
  cmdList->SetGraphicsRootConstantBufferView(3, lightAddr);

  // FrameResource から SRV 領域を一括確保
  const uint32_t count = static_cast<uint32_t>(instances.size());
  const uint32_t dataSize = count * static_cast<uint32_t>(sizeof(InstanceDataGPU));

  // フレームリソースが足りないときは描かない（AllocSRV は Release ビルドで範囲外を返すため）
  if (!frame.HasSRVSpace(dataSize)) {
    return;
  }
  void *mapped = nullptr;
  D3D12_GPU_VIRTUAL_ADDRESS instAddr = frame.AllocSRV(dataSize, &mapped);
  auto *dst = reinterpret_cast<InstanceDataGPU *>(mapped);

  if (worldOnly) {
    // シャドウパス：VS は World しか読まないので WVP / 逆転置行列（4x4 の逆行列）を省く。
    // 壁・床は数百インスタンス × 影タイル数だけ毎フレーム通るため効果が大きい
    for (uint32_t i = 0; i < count; ++i) {
      const Transform &tr = instances[i];
      dst[i].World = MakeAffineMatrix(tr.scale, tr.rotation, tr.translation);
      dst[i].color = {1, 1, 1, 1};
    }
  } else {
    const Matrix4x4 viewProj = Multiply(view, proj); // ループ不変
    for (uint32_t i = 0; i < count; ++i) {
      const Transform &tr = instances[i];
      Matrix4x4 world = MakeAffineMatrix(tr.scale, tr.rotation, tr.translation);
      dst[i].World = world;
      dst[i].WVP = Multiply(world, viewProj);
      dst[i].WorldInverseTranspose = Transpose(Inverse(world));
      dst[i].color = {1, 1, 1, 1};
    }
  }

  cmdList->SetGraphicsRootShaderResourceView(1, instAddr);

  if (items.empty()) {
    const D3D12_GPU_DESCRIPTOR_HANDLE mainSrv =
        (textureSrv_.ptr != 0) ? textureSrv_ : GetSrvForMaterial_(0);
    cmdList->SetGraphicsRootDescriptorTable(2, mainSrv);
    cmdList->SetGraphicsRootDescriptorTable(9, (normalMapSrv_.ptr != 0) ? normalMapSrv_ : mainSrv);
    cmdList->SetGraphicsRootDescriptorTable(10, (roughnessMapSrv_.ptr != 0) ? roughnessMapSrv_ : mainSrv);
    if (mesh_->HasIndexBuffer()) {
      cmdList->IASetIndexBuffer(&mesh_->IBV());
      cmdList->DrawIndexedInstanced(mesh_->IndexCount(), count, 0, 0, 0);
    } else {
      cmdList->DrawInstanced(mesh_->VertexCount(), count, 0, 0);
    }
  } else {
    if (mesh_->HasIndexBuffer()) {
      cmdList->IASetIndexBuffer(&mesh_->IBV());
    }
    for (const auto &it : items) {
      const D3D12_GPU_DESCRIPTOR_HANDLE mainSrv =
          (textureSrv_.ptr != 0) ? textureSrv_
                                 : GetSrvForMaterial_(it.materialIndex);
      cmdList->SetGraphicsRootDescriptorTable(2, mainSrv);
      cmdList->SetGraphicsRootDescriptorTable(9, (normalMapSrv_.ptr != 0) ? normalMapSrv_ : mainSrv);
      cmdList->SetGraphicsRootDescriptorTable(10, (roughnessMapSrv_.ptr != 0) ? roughnessMapSrv_ : mainSrv);
      if (mesh_->HasIndexBuffer() && it.indexCount > 0) {
        cmdList->DrawIndexedInstanced(it.indexCount, count, it.indexStart, 0, 0);
      } else {
        cmdList->DrawInstanced(it.vertexCount, count, it.vertexStart, 0);
      }
    }
  }
}

// ============================================================================
// DrawBatch（インスタンシング・カラー指定版）
// ============================================================================

void ModelResource::DrawBatch(ID3D12GraphicsCommandList *cmdList,
                              const Matrix4x4 &view, const Matrix4x4 &proj,
                              const std::vector<Transform> &instances,
                              const RC::Vector4 &color,
                              FrameResource &frame, bool worldOnly) {
  if (!isReady_ || !mesh_ || !mesh_->Ready() || instances.empty()) {
    return;
  }

  const auto &items = mesh_->DrawItems();

  const auto &vbv = mesh_->VBV();
  cmdList->IASetVertexBuffers(0, 1, &vbv);
  cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

  cbMat_.dyn.Ptr()->useNormalMap = (normalMapSrv_.ptr != 0) ? 1 : 0;
  cbMat_.dyn.Ptr()->useRoughnessMap = (roughnessMapSrv_.ptr != 0) ? 1 : 0;

  cmdList->SetGraphicsRootConstantBufferView(
      0, cbMat_.dyn.Address());

  if (textureSrv_.ptr == 0) {
    EnsureMaterialSrvsLoaded_();
  }

  const D3D12_GPU_VIRTUAL_ADDRESS lightAddr =
      (externalLightCBAddress_ != 0)
          ? externalLightCBAddress_
          : cbLight_.dyn.Address();
  cmdList->SetGraphicsRootConstantBufferView(3, lightAddr);

  // FrameResource から SRV 領域を一括確保
  const uint32_t count = static_cast<uint32_t>(instances.size());
  const uint32_t dataSize = count * static_cast<uint32_t>(sizeof(InstanceDataGPU));

  // フレームリソースが足りないときは描かない（AllocSRV は Release ビルドで範囲外を返すため）
  if (!frame.HasSRVSpace(dataSize)) {
    return;
  }
  void *mapped = nullptr;
  D3D12_GPU_VIRTUAL_ADDRESS instAddr = frame.AllocSRV(dataSize, &mapped);
  auto *dst = reinterpret_cast<InstanceDataGPU *>(mapped);

  if (worldOnly) {
    // シャドウパス：VS は World しか読まないので WVP / 逆転置行列（4x4 の逆行列）を省く
    for (uint32_t i = 0; i < count; ++i) {
      const Transform &tr = instances[i];
      dst[i].World = MakeAffineMatrix(tr.scale, tr.rotation, tr.translation);
      dst[i].color = color;
    }
  } else {
    const Matrix4x4 viewProj = Multiply(view, proj); // ループ不変
    for (uint32_t i = 0; i < count; ++i) {
      const Transform &tr = instances[i];
      Matrix4x4 world = MakeAffineMatrix(tr.scale, tr.rotation, tr.translation);
      dst[i].World = world;
      dst[i].WVP = Multiply(world, viewProj);
      dst[i].WorldInverseTranspose = Transpose(Inverse(world));
      dst[i].color = color;
    }
  }

  cmdList->SetGraphicsRootShaderResourceView(1, instAddr);

  if (items.empty()) {
    const D3D12_GPU_DESCRIPTOR_HANDLE mainSrv =
        (textureSrv_.ptr != 0) ? textureSrv_ : GetSrvForMaterial_(0);
    cmdList->SetGraphicsRootDescriptorTable(2, mainSrv);
    cmdList->SetGraphicsRootDescriptorTable(9, (normalMapSrv_.ptr != 0) ? normalMapSrv_ : mainSrv);
    cmdList->SetGraphicsRootDescriptorTable(10, (roughnessMapSrv_.ptr != 0) ? roughnessMapSrv_ : mainSrv);
    if (mesh_->HasIndexBuffer()) {
      cmdList->IASetIndexBuffer(&mesh_->IBV());
      cmdList->DrawIndexedInstanced(mesh_->IndexCount(), count, 0, 0, 0);
    } else {
      cmdList->DrawInstanced(mesh_->VertexCount(), count, 0, 0);
    }
  } else {
    if (mesh_->HasIndexBuffer()) {
      cmdList->IASetIndexBuffer(&mesh_->IBV());
    }
    for (const auto &it : items) {
      const D3D12_GPU_DESCRIPTOR_HANDLE mainSrv =
          (textureSrv_.ptr != 0) ? textureSrv_
                                 : GetSrvForMaterial_(it.materialIndex);
      cmdList->SetGraphicsRootDescriptorTable(2, mainSrv);
      cmdList->SetGraphicsRootDescriptorTable(9, (normalMapSrv_.ptr != 0) ? normalMapSrv_ : mainSrv);
      cmdList->SetGraphicsRootDescriptorTable(10, (roughnessMapSrv_.ptr != 0) ? roughnessMapSrv_ : mainSrv);
      if (mesh_->HasIndexBuffer() && it.indexCount > 0) {
        cmdList->DrawIndexedInstanced(it.indexCount, count, it.indexStart, 0, 0);
      } else {
        cmdList->DrawInstanced(it.vertexCount, count, it.vertexStart, 0);
      }
    }
  }
}

// ============================================================================
// DrawSkinned（スキニング描画）
// ============================================================================

void ModelResource::DrawSkinned(ID3D12GraphicsCommandList *cmdList,
                                const Matrix4x4 &world, const Matrix4x4 &view,
                                const Matrix4x4 &proj,
                                const std::vector<Matrix4x4> &skinMatrices,
                                FrameResource &frame) {
  if (!isReady_ || !mesh_ || !mesh_->Ready() || skinMatrices.empty())
    return;

  const auto &items = mesh_->DrawItems();

  // テクスチャ
  if (textureSrv_.ptr == 0) {
    EnsureMaterialSrvsLoaded_();
  }

  const auto &vbv = mesh_->VBV();
  cmdList->IASetVertexBuffers(0, 1, &vbv);
  cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

  if (mesh_->HasIndexBuffer()) {
    cmdList->IASetIndexBuffer(&mesh_->IBV());
  }

  // Material CB (PS b0)
  cbMat_.dyn.Ptr()->useNormalMap = (normalMapSrv_.ptr != 0) ? 1 : 0;
  cbMat_.dyn.Ptr()->useRoughnessMap = (roughnessMapSrv_.ptr != 0) ? 1 : 0;
  cmdList->SetGraphicsRootConstantBufferView(
      Object3DRootParam::kMaterial, cbMat_.dyn.Address());

  // Light CB (PS b1)
  const D3D12_GPU_VIRTUAL_ADDRESS lightAddr =
      (externalLightCBAddress_ != 0)
          ? externalLightCBAddress_
          : cbLight_.dyn.Address();
  cmdList->SetGraphicsRootConstantBufferView(Object3DRootParam::kLight, lightAddr);

  // 行列パレットをSRVとして転送 (VS t1)
  const uint32_t matCount = static_cast<uint32_t>(skinMatrices.size());
  const uint32_t matSize = matCount * static_cast<uint32_t>(sizeof(Matrix4x4));
  if (!frame.HasSRVSpace(matSize)) {
    return; // フレームリソース不足（範囲外へ書かないよう描画を諦める）
  }
  void *matMapped = nullptr;
  D3D12_GPU_VIRTUAL_ADDRESS matAddr = frame.AllocSRV(matSize, &matMapped);
  std::memcpy(matMapped, skinMatrices.data(), matSize);
  cmdList->SetGraphicsRootShaderResourceView(Object3DRootParam::kSkinMatrices, matAddr);

  // スキニングモデルではnodeWorldは掛けない
  // （スキニング計算で既にスケルトン空間→ワールドはworldだけで十分）

  if (items.empty()) {
    // DrawItem無しの場合
    void *dst = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS addr =
        frame.AllocCB(sizeof(TransformationMatrix), &dst);
    auto *tm = reinterpret_cast<TransformationMatrix *>(dst);
    tm->World = world;
    tm->WVP = Multiply(world, Multiply(view, proj));
    tm->worldInverseTranspose = Transpose(Inverse(world));
    cmdList->SetGraphicsRootConstantBufferView(Object3DRootParam::kTransform, addr);

    const D3D12_GPU_DESCRIPTOR_HANDLE mainSrv =
        (textureSrv_.ptr != 0) ? textureSrv_ : GetSrvForMaterial_(0);
    cmdList->SetGraphicsRootDescriptorTable(Object3DRootParam::kTexture, mainSrv);
    cmdList->SetGraphicsRootDescriptorTable(Object3DRootParam::kNormalMap, (normalMapSrv_.ptr != 0) ? normalMapSrv_ : mainSrv);
    cmdList->SetGraphicsRootDescriptorTable(Object3DRootParam::kRoughnessMap, (roughnessMapSrv_.ptr != 0) ? roughnessMapSrv_ : mainSrv);

    if (mesh_->HasIndexBuffer()) {
      cmdList->DrawIndexedInstanced(mesh_->IndexCount(), 1, 0, 0, 0);
    } else {
      cmdList->DrawInstanced(mesh_->VertexCount(), 1, 0, 0);
    }
    return;
  }

  for (uint32_t i = 0; i < items.size(); ++i) {
    const auto &it = items[i];

    // スキニングモデルではnodeWorldは使わない（スキニング行列で処理済み）
    void *dst = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS addr =
        frame.AllocCB(sizeof(TransformationMatrix), &dst);
    auto *tm = reinterpret_cast<TransformationMatrix *>(dst);
    tm->World = world;
    tm->WVP = Multiply(world, Multiply(view, proj));
    tm->worldInverseTranspose = Transpose(Inverse(world));
    cmdList->SetGraphicsRootConstantBufferView(Object3DRootParam::kTransform, addr);

    const D3D12_GPU_DESCRIPTOR_HANDLE mainSrv =
        (textureSrv_.ptr != 0) ? textureSrv_
                               : GetSrvForMaterial_(it.materialIndex);
    cmdList->SetGraphicsRootDescriptorTable(Object3DRootParam::kTexture, mainSrv);
    cmdList->SetGraphicsRootDescriptorTable(Object3DRootParam::kNormalMap, (normalMapSrv_.ptr != 0) ? normalMapSrv_ : mainSrv);
    cmdList->SetGraphicsRootDescriptorTable(Object3DRootParam::kRoughnessMap, (roughnessMapSrv_.ptr != 0) ? roughnessMapSrv_ : mainSrv);

    if (mesh_->HasIndexBuffer() && it.indexCount > 0) {
      cmdList->DrawIndexedInstanced(it.indexCount, 1, it.indexStart, 0, 0);
    } else {
      cmdList->DrawInstanced(it.vertexCount, 1, it.vertexStart, 0);
    }
  }
}

// ============================================================================
// CS スキニング
// ============================================================================

void ModelResource::SetSkinningCS(ID3D12Device * /*device*/,
                                   PipelineManager *pm,
                                   SRVManager *srvMgr) {
  skinningCS_.Initialize(device_.Get(), pm, "skinning_cs");
  srvMgr_ = srvMgr;
}

void ModelResource::InitSkinningResources_() {
  if (skinningResourcesReady_ || !mesh_ || !mesh_->Ready() || !device_ || !srvMgr_)
    return;

  const uint32_t vtxCount = mesh_->VertexCount();
  if (vtxCount == 0)
    return;

  const uint32_t stride = mesh_->GetVertexStrideBytes();
  const size_t totalBytes = static_cast<size_t>(stride) * vtxCount;

  // UAV 用バッファ作成 (DEFAULT heap + UAV flag)
  D3D12_HEAP_PROPERTIES heapProps{};
  heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

  D3D12_RESOURCE_DESC bufDesc{};
  bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  bufDesc.Width = totalBytes;
  bufDesc.Height = 1;
  bufDesc.DepthOrArraySize = 1;
  bufDesc.MipLevels = 1;
  bufDesc.SampleDesc.Count = 1;
  bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  bufDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

  HRESULT hr = device_->CreateCommittedResource(
      &heapProps, D3D12_HEAP_FLAG_NONE, &bufDesc,
      D3D12_RESOURCE_STATE_COMMON, nullptr,
      IID_PPV_ARGS(&skinnedVertexBuffer_));
  if (FAILED(hr)) {
    Log::Print("[ModelResource] Failed to create skinned vertex buffer");
    return;
  }
  skinnedVertexBuffer_->SetName(L"ModelResource::SkinnedVertexBuffer");

  // UAV ディスクリプタ作成
  auto uavHandle = srvMgr_->CreateStructuredBufferUAV(
      skinnedVertexBuffer_.Get(), vtxCount, stride);
  skinnedUAVHandle_ = uavHandle.gpu;

  // VBV 設定
  skinnedVBV_.BufferLocation = skinnedVertexBuffer_->GetGPUVirtualAddress();
  skinnedVBV_.SizeInBytes = static_cast<UINT>(totalBytes);
  skinnedVBV_.StrideInBytes = stride;

  skinnedVertexCount_ = vtxCount;

  // SkinningInfo CB 作成 (numVertices だけの小さなCB)
  skinningInfoCB_ = CreateBufferResource(device_.Get(), 256, // 256byteアライン
                                          L"ModelResource::SkinningInfoCB");
  skinningInfoCB_->Map(0, nullptr, reinterpret_cast<void **>(&skinningInfoMapped_));
  *skinningInfoMapped_ = vtxCount;

  skinningResourcesReady_ = true;
  Log::Print(std::format("[ModelResource] CS skinning resources initialized: {} vertices", vtxCount));
}

void ModelResource::DispatchSkinning(ID3D12GraphicsCommandList *cmdList,
                                      const std::vector<Matrix4x4> &skinMatrices,
                                      FrameResource &frame) {
  if (!skinningCS_.IsReady() || !mesh_ || !mesh_->Ready())
    return;

  // 遅延初期化
  if (!skinningResourcesReady_) {
    InitSkinningResources_();
    if (!skinningResourcesReady_)
      return;
  }

  // 行列パレットを FrameResource から確保してコピー
  const uint32_t matCount = static_cast<uint32_t>(skinMatrices.size());
  const uint32_t matSize = matCount * static_cast<uint32_t>(sizeof(Matrix4x4));
  if (!frame.HasSRVSpace(matSize)) {
    return; // フレームリソース不足（範囲外へ書かないよう描画を諦める）
  }
  void *matMapped = nullptr;
  D3D12_GPU_VIRTUAL_ADDRESS matAddr = frame.AllocSRV(matSize, &matMapped);
  std::memcpy(matMapped, skinMatrices.data(), matSize);

  // 入力頂点の GPU アドレス
  D3D12_GPU_VIRTUAL_ADDRESS inputVtxAddr = mesh_->GetVBResource()->GetGPUVirtualAddress();

  // ComputeShader API でバインド & Dispatch
  skinningCS_.Bind(cmdList);
  skinningCS_.SetSRV(cmdList, 0, matAddr)         // t0: 行列パレット
             .SetSRV(cmdList, 1, inputVtxAddr)    // t1: 入力頂点
             .SetUAV(cmdList, 2, skinnedUAVHandle_) // u0: 出力頂点
             .SetCBV(cmdList, 3, skinningInfoCB_->GetGPUVirtualAddress()); // b0: 頂点数
  skinningCS_.Dispatch(cmdList, skinnedVertexCount_);
  ComputeShader::UAVBarrier(cmdList, skinnedVertexBuffer_.Get());

  skinningDispatched_ = true;
}

// ============================================================================
// DrawSkinnedCS（CS スキニング済み頂点で通常描画）
// ============================================================================

void ModelResource::DrawSkinnedCS(ID3D12GraphicsCommandList *cmdList,
                                   const Matrix4x4 &world, const Matrix4x4 &view,
                                   const Matrix4x4 &proj, FrameResource &frame,
                                   bool worldOnly) {
  if (!isReady_ || !mesh_ || !mesh_->Ready() || !skinningDispatched_)
    return;

  const auto &items = mesh_->DrawItems();

  // テクスチャ
  if (textureSrv_.ptr == 0) {
    EnsureMaterialSrvsLoaded_();
  }

  // CS スキニング済みの VBV を使用
  cmdList->IASetVertexBuffers(0, 1, &skinnedVBV_);
  cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

  if (mesh_->HasIndexBuffer()) {
    cmdList->IASetIndexBuffer(&mesh_->IBV());
  }

  // Material CB (slot 0, PS)
  cbMat_.dyn.Ptr()->useNormalMap = (normalMapSrv_.ptr != 0) ? 1 : 0;
  cbMat_.dyn.Ptr()->useRoughnessMap = (roughnessMapSrv_.ptr != 0) ? 1 : 0;
  cmdList->SetGraphicsRootConstantBufferView(
      0, cbMat_.dyn.Address());

  // Transform CB (slot 1, VS)
  void *dst = nullptr;
  D3D12_GPU_VIRTUAL_ADDRESS addr =
      frame.AllocCB(sizeof(TransformationMatrix), &dst);
  auto *tm = reinterpret_cast<TransformationMatrix *>(dst);
  tm->World = world;
  if (!worldOnly) {
    // シャドウパスの VS は World しか読まないので、その場合は WVP / 逆転置行列の計算を省く
    tm->WVP = Multiply(world, Multiply(view, proj));
    tm->worldInverseTranspose = Transpose(Inverse(world));
  }
  cmdList->SetGraphicsRootConstantBufferView(1, addr);

  // Light CB (slot 3, PS)
  const D3D12_GPU_VIRTUAL_ADDRESS lightAddr =
      (externalLightCBAddress_ != 0)
          ? externalLightCBAddress_
          : cbLight_.dyn.Address();
  cmdList->SetGraphicsRootConstantBufferView(3, lightAddr);

  if (items.empty()) {
    const D3D12_GPU_DESCRIPTOR_HANDLE mainSrv =
        (textureSrv_.ptr != 0) ? textureSrv_ : GetSrvForMaterial_(0);
    cmdList->SetGraphicsRootDescriptorTable(2, mainSrv);
    cmdList->SetGraphicsRootDescriptorTable(9, (normalMapSrv_.ptr != 0) ? normalMapSrv_ : mainSrv);
    cmdList->SetGraphicsRootDescriptorTable(10, (roughnessMapSrv_.ptr != 0) ? roughnessMapSrv_ : mainSrv);

    if (mesh_->HasIndexBuffer()) {
      cmdList->DrawIndexedInstanced(mesh_->IndexCount(), 1, 0, 0, 0);
    } else {
      cmdList->DrawInstanced(mesh_->VertexCount(), 1, 0, 0);
    }
    return;
  }

  for (uint32_t i = 0; i < items.size(); ++i) {
    const auto &it = items[i];

    const D3D12_GPU_DESCRIPTOR_HANDLE mainSrv =
        (textureSrv_.ptr != 0) ? textureSrv_
                               : GetSrvForMaterial_(it.materialIndex);
    cmdList->SetGraphicsRootDescriptorTable(2, mainSrv);
    cmdList->SetGraphicsRootDescriptorTable(9, (normalMapSrv_.ptr != 0) ? normalMapSrv_ : mainSrv);
    cmdList->SetGraphicsRootDescriptorTable(10, (roughnessMapSrv_.ptr != 0) ? roughnessMapSrv_ : mainSrv);

    if (mesh_->HasIndexBuffer() && it.indexCount > 0) {
      cmdList->DrawIndexedInstanced(it.indexCount, 1, it.indexStart, 0, 0);
    } else {
      cmdList->DrawInstanced(it.vertexCount, 1, it.vertexStart, 0);
    }
  }
}
