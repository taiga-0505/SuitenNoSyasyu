#include "SkydomeManager.h"
#include "Skydome/Skydome.h"
#include "Texture/TextureManager/TextureManager.h"

namespace RC {

SkydomeManager::~SkydomeManager() = default;

void SkydomeManager::Init(ID3D12Device *device, TextureManager *texman) {
  device_ = device;
  texman_ = texman;
  skydomes_.clear();
}

void SkydomeManager::Term() {
  skydomes_.clear();
  device_ = nullptr;
  texman_ = nullptr;
}

int SkydomeManager::AllocSlot_() {
  for (int i = 0; i < static_cast<int>(skydomes_.size()); ++i) {
    if (!skydomes_[i].inUse) {
      return i;
    }
  }
  skydomes_.emplace_back();
  return static_cast<int>(skydomes_.size()) - 1;
}

int SkydomeManager::Create(int textureHandle, float radius, unsigned int sliceCount,
                          unsigned int stackCount) {
  if (!device_) {
    return -1;
  }

  const int handle = AllocSlot_();

  auto skydome = std::make_unique<Skydome>();
  skydome->Initialize(device_, radius, sliceCount, stackCount);

  if (textureHandle >= 0 && texman_) {
    skydome->SetTexture(texman_->GetSrv(textureHandle));
  }

  skydomes_[handle].ptr = std::move(skydome);
  skydomes_[handle].inUse = true;
  skydomes_[handle].defaultTexHandle = textureHandle;

  return handle;
}

void SkydomeManager::Unload(int handle) {
  if (!IsValid(handle)) {
    return;
  }

  skydomes_[handle].ptr.reset();
  skydomes_[handle].inUse = false;
  skydomes_[handle].defaultTexHandle = -1;
}

bool SkydomeManager::IsValid(int handle) const {
  return (handle >= 0 && handle < static_cast<int>(skydomes_.size()) &&
          skydomes_[handle].inUse && skydomes_[handle].ptr);
}

Skydome *SkydomeManager::Get(int handle) {
  if (!IsValid(handle)) {
    return nullptr;
  }
  return skydomes_[handle].ptr.get();
}

const Skydome *SkydomeManager::Get(int handle) const {
  if (!IsValid(handle)) {
    return nullptr;
  }
  return skydomes_[handle].ptr.get();
}

Transform *SkydomeManager::GetTransformPtr(int handle) {
  if (!IsValid(handle)) {
    return nullptr;
  }
  return &skydomes_[handle].ptr->T();
}

void SkydomeManager::SetColor(int handle, const Vector4 &color) {
  if (!IsValid(handle)) {
    return;
  }

  if (auto *mat = skydomes_[handle].ptr->Mat()) {
    mat->color = color;
  }
}

void SkydomeManager::ApplyTexture(int handle, int overrideTexHandle) {
  if (!IsValid(handle) || !texman_) {
    return;
  }

  const int useTex = (overrideTexHandle >= 0)
                         ? overrideTexHandle
                         : skydomes_[handle].defaultTexHandle;
                         
  skydomes_[handle].ptr->SetTexture(texman_->GetSrv(useTex));
}

} // namespace RC
