#include "SpotLightManager.h"
#include "Dx12/Dx12Core.h" // CreateBufferResource
#include <algorithm>
#include "function/function.h"

namespace RC {

static UINT Align256_(UINT size) { return (size + 255u) & ~255u; }

void SpotLightManager::Init(ID3D12Device *device) {
  Term();
  device_ = device;
  initialized_ = (device_ != nullptr);
  slots_.clear();
  slots_.shrink_to_fit();

  activeCount_ = 0;
  active_.fill(-1);

  if (!initialized_)
    return;

  // slot[0] = デフォルト（基本OFF）
  slots_.emplace_back();
  slots_[0].inUse = true;

  auto &d = slots_[0].light.Data();
  d.intensity = 0.0f;
  d.distance = 0.0f;

  EnsureCB_();
  SyncCB_();
}

void SpotLightManager::Term() {
  slots_.clear();

  activeCount_ = 0;
  active_.fill(-1);

  hasCB_ = false;
  mapped_ = nullptr;

  device_.Reset();
  initialized_ = false;
}

int SpotLightManager::AllocSlot_() {
  for (int i = 1; i < (int)slots_.size(); ++i) {
    if (!slots_[i].inUse)
      return i;
  }
  slots_.emplace_back();
  return (int)slots_.size() - 1;
}

bool SpotLightManager::IsValid_(int handle) const {
  return (initialized_ && handle >= 0 && handle < (int)slots_.size() &&
          slots_[handle].inUse);
}

int SpotLightManager::ResolveFallbackHandle_() const { return 0; }

int SpotLightManager::Create() {
  if (!initialized_ || !device_)
    return -1;

  const int h = AllocSlot_();
  auto &slot = slots_[h];
  slot.inUse = true;
  slot.light = SpotLightSource{}; // defaults

  if (activeCount_ == 0) {
    AddActive(h);
  }

  return h;
}

void SpotLightManager::Destroy(int handle) {
  if (!IsValid_(handle))
    return;
  if (handle == 0)
    return;

  RemoveActive(handle);
  slots_[handle].inUse = false;
}

void SpotLightManager::SetActive(int handle) {
  ClearActive();
  if (handle >= 0) {
    AddActive(handle);
  }
}

void SpotLightManager::ClearActive() {
  activeCount_ = 0;
  active_.fill(-1);
}

bool SpotLightManager::AddActive(int handle) {
  if (!IsValid_(handle))
    return false;

  for (int i = 0; i < activeCount_; ++i) {
    if (active_[i] == handle)
      return true;
  }

  if (activeCount_ >= kMaxActive)
    return false;

  active_[activeCount_] = handle;
  ++activeCount_;
  return true;
}

void SpotLightManager::RemoveActive(int handle) {
  for (int i = 0; i < activeCount_; ++i) {
    if (active_[i] == handle) {
      for (int j = i; j < activeCount_ - 1; ++j) {
        active_[j] = active_[j + 1];
      }
      active_[activeCount_ - 1] = -1;
      --activeCount_;
      return;
    }
  }
}

int SpotLightManager::GetActiveHandleAt(int index) const {
  if (index < 0 || index >= activeCount_)
    return -1;
  return active_[index];
}

SpotLightSource *SpotLightManager::Get(int handle) {
  if (!IsValid_(handle))
    return nullptr;
  return &slots_[handle].light;
}

const SpotLightSource *SpotLightManager::Get(int handle) const {
  if (!IsValid_(handle))
    return nullptr;
  return &slots_[handle].light;
}

SpotLightSource *SpotLightManager::GetActive() {
  if (activeCount_ > 0)
    return Get(active_[0]);
  return Get(ResolveFallbackHandle_());
}

const SpotLightSource *SpotLightManager::GetActive() const {
  if (activeCount_ > 0)
    return Get(active_[0]);
  return Get(ResolveFallbackHandle_());
}

void SpotLightManager::EnsureCB_() {
  if (hasCB_ || !device_)
    return;

  // DynamicCB: CPU 側の値に書き、バインド時に今フレームの領域へ送る
  hasCB_ = true;
  mapped_ = dyn_.Ptr();
  if (mapped_) {
    // アップロードヒープは 0 初期化されないので、一度だけ全体をクリアしておく。
    // 以降の SyncCB_ は count と使用中エントリだけを書く（シェーダは count で break する）
    *mapped_ = ::SpotLightsCB{};
  }
}

void SpotLightManager::SyncCB_() {
  if (!mapped_)
    return;

  uint32_t outCount = 0;
  const uint32_t n = (uint32_t)(std::min)(activeCount_, kMaxActive);

  // enabled なライトだけを詰める（使用中のエントリだけ書く。全 256 灯分の再構築はしない）
  for (uint32_t i = 0; i < n; ++i) {
    const int h = active_[i];
    if (!IsValid_(h))
      continue;

    const auto &ls = slots_[h].light;
    if (!ls.IsEnabled())
      continue;

    mapped_->lights[outCount] = ls.DataForGPU();
    ++outCount;
  }
  mapped_->count = outCount;
}

void SpotLightManager::SyncCB() {
  if (!initialized_)
    return;
  EnsureCB_();
  SyncCB_();
  // 前回 GPU へ送った値と比べ、変わっていれば次の GetCBAddress で送り直す
  dyn_.Refresh();
}

D3D12_GPU_VIRTUAL_ADDRESS SpotLightManager::GetCBAddress() {
  if (!initialized_)
    return 0;
  EnsureCB_();
  // ドローごとに呼ばれるので比較はしない（SyncCB で変更を検出済み）
  return hasCB_ ? dyn_.AddressCached() : 0;
}

} // namespace RC
