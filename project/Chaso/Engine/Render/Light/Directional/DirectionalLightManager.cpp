#include "DirectionalLightManager.h"

#include "Dx12/Dx12Core.h" // CreateBufferResource
#include "function/function.h"

namespace RC {

void DirectionalLightManager::Init(ID3D12Device *device) {
  // 二重Initにも耐える
  Term();

  device_ = device;
  initialized_ = (device_ != nullptr);

  slots_.clear();
  slots_.shrink_to_fit();

  if (!initialized_) {
    return;
  }

  // slot[0] は「デフォルトライト」予約
  slots_.emplace_back();
  slots_[0].inUse = true;
  slots_[0].light = DirectionalLightSource{}; // defaults

  EnsureCB_(slots_[0]);
  SyncCB_(slots_[0]);

  activeHandle_ = -1; // 明示的なアクティブ無し
}

void DirectionalLightManager::Term() {
  for (auto &s : slots_) {
    ReleaseSlot_(s);
    s.inUse = false;
  }
  slots_.clear();

  device_.Reset();
  activeHandle_ = -1;
  initialized_ = false;
}

int DirectionalLightManager::AllocSlot_() {
  // 0 はデフォルトなので 1 から探す
  for (int i = 1; i < static_cast<int>(slots_.size()); ++i) {
    if (!slots_[i].inUse) {
      return i;
    }
  }
  slots_.emplace_back();
  return static_cast<int>(slots_.size()) - 1;
}

bool DirectionalLightManager::IsValid_(int handle) const {
  return (initialized_ && handle >= 0 && handle < static_cast<int>(slots_.size()) &&
          slots_[handle].inUse);
}

int DirectionalLightManager::ResolveActiveHandle_() const {
  if (!initialized_) {
    return -1;
  }
  if (activeHandle_ >= 0 && IsValid_(activeHandle_)) {
    return activeHandle_;
  }
  // 未設定はデフォルト(0)
  return 0;
}

int DirectionalLightManager::Create() {
  if (!initialized_ || !device_) {
    return -1;
  }

  const int h = AllocSlot_();
  auto &slot = slots_[h];

  // 再利用スロットに古いGPUリソースが残ってたら解放
  ReleaseSlot_(slot);

  slot.inUse = true;
  slot.light = DirectionalLightSource{}; // defaults

  EnsureCB_(slot);
  SyncCB_(slot);

  // 最初のライトを作ったら自動で明示的アクティブにする（従来挙動）
  if (activeHandle_ < 0) {
    activeHandle_ = h;
  }

  return h;
}

void DirectionalLightManager::Destroy(int handle) {
  if (!IsValid_(handle)) {
    return;
  }

  // 0 はデフォルトなので破棄不可
  if (handle == 0) {
    return;
  }

  auto &slot = slots_[handle];
  ReleaseSlot_(slot);
  slot.inUse = false;

  if (activeHandle_ == handle) {
    activeHandle_ = -1;
  }
}

void DirectionalLightManager::SetActive(int handle) {
  // -1: 明示的なアクティブ無し（内部では default を使う）
  if (handle < 0) {
    activeHandle_ = -1;
    return;
  }

  if (!IsValid_(handle)) {
    return;
  }

  activeHandle_ = handle;
}

DirectionalLightSource *DirectionalLightManager::Get(int handle) {
  if (!IsValid_(handle)) {
    return nullptr;
  }
  return &slots_[handle].light;
}

const DirectionalLightSource *DirectionalLightManager::Get(int handle) const {
  if (!IsValid_(handle)) {
    return nullptr;
  }
  return &slots_[handle].light;
}

DirectionalLightSource *DirectionalLightManager::GetActive() {
  const int h = ResolveActiveHandle_();
  return Get(h);
}

const DirectionalLightSource *DirectionalLightManager::GetActive() const {
  const int h = ResolveActiveHandle_();
  return Get(h);
}

D3D12_GPU_VIRTUAL_ADDRESS DirectionalLightManager::GetCBAddress(int handle) {
  if (!IsValid_(handle)) {
    return 0;
  }

  auto &slot = slots_[handle];
  EnsureCB_(slot);
  SyncCB_(slot);

  return slot.hasCB ? slot.dyn.Address() : 0;
}

D3D12_GPU_VIRTUAL_ADDRESS DirectionalLightManager::GetActiveCBAddress() {
  const int h = ResolveActiveHandle_();
  return GetCBAddress(h);
}

void DirectionalLightManager::ReleaseSlot_(Slot &s) {
  // DynamicCB は毎フレームの一時領域を使うので、解放する GPU リソースは無い
  s.hasCB = false;
}

void DirectionalLightManager::EnsureCB_(Slot &s) {
  if (s.hasCB || !device_) {
    return;
  }
  s.hasCB = true;
  *s.dyn.Ptr() = s.light.DataForGPU();
}

void DirectionalLightManager::SyncCB_(Slot &s) {
  if (s.hasCB) {
    *s.dyn.Ptr() = s.light.DataForGPU();
  }
}

} // namespace RC
