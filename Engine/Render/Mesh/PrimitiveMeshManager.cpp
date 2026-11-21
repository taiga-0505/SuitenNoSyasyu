#include "PrimitiveMeshManager.h"
#include "Mesh/PrimitiveMesh.h"
#include "Texture/TextureManager/TextureManager.h"
#include "Common/Log/Log.h"
#include <format>

namespace RC {

PrimitiveMeshManager::~PrimitiveMeshManager() = default;

void PrimitiveMeshManager::Init(ID3D12Device *device, TextureManager *texman) {
  device_ = device;
  texman_ = texman;
  primitives_.clear();

  if (texman_) {
    whiteSrv_ = texman_->Load("Resources/white1x1.png");
  }
}

void PrimitiveMeshManager::Term() {
  primitives_.clear();
  device_ = nullptr;
  texman_ = nullptr;
}

int PrimitiveMeshManager::AllocSlot_() {
  for (int i = 0; i < static_cast<int>(primitives_.size()); ++i) {
    if (!primitives_[i].inUse) {
      return i;
    }
  }
  primitives_.emplace_back();
  return static_cast<int>(primitives_.size()) - 1;
}

int PrimitiveMeshManager::Create(const ModelData &data, int textureHandle, const std::string &typeName) {
  if (!device_) {
    return -1;
  }

  const int handle = AllocSlot_();

  auto prim = std::make_unique<PrimitiveMesh>();
  prim->Initialize(device_, data);

  if (textureHandle >= 0 && texman_) {
    prim->SetTexture(texman_->GetSrv(textureHandle));
  } else {
    prim->SetTexture(whiteSrv_);
  }

  primitives_[handle].ptr = std::move(prim);
  primitives_[handle].inUse = true;
  primitives_[handle].defaultTexHandle = textureHandle;
  primitives_[handle].typeName = typeName;

  Log::Print(std::format("[PrimitiveMesh] メッシュ生成 (Type: {}, Handle: {}, TexHandle: {})", typeName, handle, textureHandle));

  return handle;
}

void PrimitiveMeshManager::Unload(int handle) {
  if (!IsValid(handle)) {
    return;
  }

  Log::Print(std::format("[PrimitiveMesh] 破棄完了 (Type: {}, Handle: {})", primitives_[handle].typeName, handle));

  primitives_[handle].ptr.reset();
  primitives_[handle].inUse = false;
  primitives_[handle].defaultTexHandle = -1;
  primitives_[handle].typeName.clear();
}

bool PrimitiveMeshManager::IsValid(int handle) const {
  return (handle >= 0 && handle < static_cast<int>(primitives_.size()) &&
          primitives_[handle].inUse && primitives_[handle].ptr);
}

PrimitiveMesh *PrimitiveMeshManager::Get(int handle) {
  if (!IsValid(handle)) {
    return nullptr;
  }
  return primitives_[handle].ptr.get();
}

const PrimitiveMesh *PrimitiveMeshManager::Get(int handle) const {
  if (!IsValid(handle)) {
    return nullptr;
  }
  return primitives_[handle].ptr.get();
}

Transform *PrimitiveMeshManager::GetTransformPtr(int handle) {
  if (!IsValid(handle)) {
    return nullptr;
  }
  return &primitives_[handle].ptr->T();
}

void PrimitiveMeshManager::SetColor(int handle, const Vector4 &color) {
  if (!IsValid(handle)) {
    return;
  }

  if (auto *mat = primitives_[handle].ptr->Mat()) {
    mat->color = color;
  }
}

void PrimitiveMeshManager::ApplyTexture(int handle, int overrideTexHandle) {
  if (!IsValid(handle) || !texman_) {
    return;
  }

  const int useTex = (overrideTexHandle >= 0)
                         ? overrideTexHandle
                         : primitives_[handle].defaultTexHandle;
  if (useTex >= 0) {
    primitives_[handle].ptr->SetTexture(texman_->GetSrv(useTex));
  } else {
    primitives_[handle].ptr->SetTexture(whiteSrv_);
  }
}

} // namespace RC
