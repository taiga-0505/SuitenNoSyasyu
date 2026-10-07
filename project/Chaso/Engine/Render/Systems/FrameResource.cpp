#include "../FrameResource.h"
#include "Common/Log/Log.h"
#include "function/function.h"
#include <cassert>
#include <format>

namespace RC {

void FrameResource::Init(ID3D12Device *device, uint32_t frameIndex) {
  device_ = device;
  frameIndex_ = frameIndex;

  // CB ヒープ
  cbCapacity_ = kDefaultCBSize;
  cbHeap_ = CreateBufferResource(
      device, cbCapacity_,
      std::format(L"FrameResource[{}]::cbHeap_", frameIndex).c_str());
  cbHeap_->Map(0, nullptr, reinterpret_cast<void **>(&cbMapped_));
  cbOffset_ = 0;

  // SRV ヒープ
  srvCapacity_ = kDefaultSRVSize;
  srvHeap_ = CreateBufferResource(
      device, srvCapacity_,
      std::format(L"FrameResource[{}]::srvHeap_", frameIndex).c_str());
  srvHeap_->Map(0, nullptr, reinterpret_cast<void **>(&srvMapped_));
  srvOffset_ = 0;

  Log::Print(std::format("[FrameResource] Frame[{}] initialized (CB: {}KB, SRV: {}KB)",
                         frameIndex, cbCapacity_ / 1024, srvCapacity_ / 1024));
}

void FrameResource::Term() {
  if (cbMapped_) {
    cbHeap_->Unmap(0, nullptr);
    cbMapped_ = nullptr;
  }
  cbHeap_.Reset();

  if (srvMapped_) {
    srvHeap_->Unmap(0, nullptr);
    srvMapped_ = nullptr;
  }
  srvHeap_.Reset();

  device_.Reset();
}

void FrameResource::Reset() {
  cbOffset_ = 0;
  srvOffset_ = 0;
}

D3D12_GPU_VIRTUAL_ADDRESS FrameResource::AllocCB(uint32_t sizeBytes,
                                                  void **outMapped) {
  const uint32_t aligned = Align256(sizeBytes);
  // 末尾 kOverflowScratch バイトは「溢れたとき用の捨て場」として通常の確保には使わない
  const uint64_t usable = (cbCapacity_ > kOverflowScratch) ? cbCapacity_ - kOverflowScratch : cbCapacity_;
  uint64_t offset = cbOffset_;
  if (offset + aligned > usable) {
    // 容量オーバー。範囲外へ書いて落ちないよう、捨て場を返す（その描画の値は壊れうる）。
    // Debug ビルドでは気付けるよう一度だけ知らせる
    if (!overflowReported_) {
      overflowReported_ = true;
      Log::Print("[FrameResource] CB capacity exceeded. kDefaultCBSize を増やしてください");
    }
    offset = (aligned <= kOverflowScratch) ? usable : 0;
  } else {
    cbOffset_ += aligned;
  }

  if (outMapped) {
    *outMapped = cbMapped_ + offset;
  }

  return cbHeap_->GetGPUVirtualAddress() + offset;
}

D3D12_GPU_VIRTUAL_ADDRESS FrameResource::AllocSRV(uint32_t sizeBytes,
                                                   void **outMapped) {
  // 頂点バッファや StructuredBuffer として使えるよう、先頭を 16byte 境界に揃える
  const uint64_t offset = (srvOffset_ + 15u) & ~uint64_t(15u);
  assert(offset + sizeBytes <= srvCapacity_ &&
         "FrameResource SRV capacity exceeded!");
  srvOffset_ = offset + sizeBytes;

  if (outMapped) {
    *outMapped = srvMapped_ + offset;
  }

  return srvHeap_->GetGPUVirtualAddress() + offset;
}

bool FrameResource::HasCBSpace(uint32_t sizeBytes) const {
  const uint64_t usable = (cbCapacity_ > kOverflowScratch) ? cbCapacity_ - kOverflowScratch : cbCapacity_;
  return cbOffset_ + Align256(sizeBytes) <= usable;
}

bool FrameResource::HasSRVSpace(uint32_t sizeBytes) const {
  const uint64_t offset = (srvOffset_ + 15u) & ~uint64_t(15u);
  return offset + sizeBytes <= srvCapacity_;
}

} // namespace RC
