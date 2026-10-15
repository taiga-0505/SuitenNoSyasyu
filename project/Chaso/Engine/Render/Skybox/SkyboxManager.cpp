#include "SkyboxManager.h"
#include "Skybox/Skybox.h"
#include "Texture/TextureManager/TextureManager.h"
#include "Common/Log/Log.h"
#include <format>

namespace RC {

SkyboxManager::~SkyboxManager() = default;

void SkyboxManager::Init(ID3D12Device *device, TextureManager *texman) {
  device_ = device;
  texman_ = texman;
  skyboxes_.clear();
}

void SkyboxManager::Term() {
  skyboxes_.clear();
  device_ = nullptr;
  texman_ = nullptr;
}

int SkyboxManager::AllocSlot_() {
  for (int i = 0; i < static_cast<int>(skyboxes_.size()); ++i) {
    if (!skyboxes_[i].inUse) {
      return i;
    }
  }
  skyboxes_.emplace_back();
  return static_cast<int>(skyboxes_.size()) - 1;
}

int SkyboxManager::Create(const std::string &ddsPath) {
  if (!device_ || !texman_) {
    return -1;
  }

  // テクスチャのロード（DDS のキューブマップ）
  D3D12_GPU_DESCRIPTOR_HANDLE srv = texman_->Load(ddsPath, false);

  // Skybox.PS は TextureCube でサンプリングする。
  // ファイルが見つからないと TextureManager は白 1x1 の Texture2D で代替するが、
  // Texture2D の SRV を TextureCube として読むのは D3D12 では未定義動作で、
  // GPU が無関係な VRAM（前フレームのレンダーターゲット等）を読んでしまい
  // 空の部分に残像・縞模様が出る。ここで弾いて絶対に描かせない。
  const Texture2D *tex = texman_->Get(ddsPath);
  if (!tex || !tex->IsLoaded() || !tex->Metadata().IsCubemap()) {
    Log::Print(std::format(
        "[Skybox] キューブマップとして読めないため生成を中止: {} "
        "(ファイルの有無とパス、DDS が cubemap かを確認してください)",
        ddsPath));
    return -1;
  }

  const int handle = AllocSlot_();

  auto skybox = std::make_unique<Skybox>();
  skybox->Initialize(device_);
  skybox->SetTexture(srv);

  skyboxes_[handle].ptr = std::move(skybox);
  skyboxes_[handle].inUse = true;
  skyboxes_[handle].texSrv = srv;

  return handle;
}

void SkyboxManager::Unload(int handle) {
  if (!IsValid(handle)) {
    return;
  }

  skyboxes_[handle].ptr.reset();
  skyboxes_[handle].inUse = false;
  skyboxes_[handle].texSrv = {};
}

bool SkyboxManager::IsValid(int handle) const {
  return (handle >= 0 && handle < static_cast<int>(skyboxes_.size()) &&
          skyboxes_[handle].inUse && skyboxes_[handle].ptr);
}

Skybox *SkyboxManager::Get(int handle) {
  if (!IsValid(handle)) {
    return nullptr;
  }
  return skyboxes_[handle].ptr.get();
}

const Skybox *SkyboxManager::Get(int handle) const {
  if (!IsValid(handle)) {
    return nullptr;
  }
  return skyboxes_[handle].ptr.get();
}

Transform *SkyboxManager::GetTransformPtr(int handle) {
  if (!IsValid(handle)) {
    return nullptr;
  }
  return &skyboxes_[handle].ptr->T();
}

void SkyboxManager::SetColor(int handle, const Vector4 &color) {
  if (!IsValid(handle)) {
    return;
  }

  if (auto *mat = skyboxes_[handle].ptr->Mat()) {
    mat->color = color;
  }
}

void SkyboxManager::ApplyTexture(int handle) {
  if (!IsValid(handle)) {
    return;
  }

  skyboxes_[handle].ptr->SetTexture(skyboxes_[handle].texSrv);
}

D3D12_GPU_DESCRIPTOR_HANDLE SkyboxManager::GetTextureSrv(int handle) const {
  if (!IsValid(handle)) {
    return D3D12_GPU_DESCRIPTOR_HANDLE{0};
  }
  return skyboxes_[handle].texSrv;
}

} // namespace RC
