#include "CommandContext.h"
#include <format>

void CommandContext::Init(ID3D12Device *device, D3D12_COMMAND_LIST_TYPE type,
                          uint32_t frameCount) {
  assert(device);
  device_ = device;
  type_ = type;
  frameCount_ = (frameCount >= 1 && frameCount <= kMaxFrames) ? frameCount : 2;

  // コマンドキュー
  D3D12_COMMAND_QUEUE_DESC qdesc{};
  qdesc.Type = type_;
  HRESULT hr = device_->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&queue_));
  assert(SUCCEEDED(hr));
  queue_->SetName(L"CommandContext::Queue");

  // コマンドアロケータ（フレーム数ぶん）
  for (uint32_t i = 0; i < frameCount_; ++i) {
    hr = device_->CreateCommandAllocator(type_, IID_PPV_ARGS(&alloc_[i]));
    assert(SUCCEEDED(hr));
    alloc_[i]->SetName(
        std::format(L"CommandContext::Allocator_{}", i).c_str());
    frameFenceValue_[i] = 0;
  }

  // コマンドリスト（1 本を使い回す）
  hr = device_->CreateCommandList(0, type_, alloc_[0].Get(), nullptr,
                                  IID_PPV_ARGS(&list_));
  assert(SUCCEEDED(hr));
  list_->SetName(L"CommandContext::List");
  hr = list_->Close(); // 最初は閉じておく
  assert(SUCCEEDED(hr));

  // フェンスと待機用イベント
  hr = device_->CreateFence(globalFenceValue_, D3D12_FENCE_FLAG_NONE,
                            IID_PPV_ARGS(&fence_));
  assert(SUCCEEDED(hr));
  fence_->SetName(L"CommandContext::Fence");
  fenceEvent_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
  assert(fenceEvent_ != nullptr);
}

void CommandContext::Term() {
  queue_.Reset();
  list_.Reset();
  for (uint32_t i = 0; i < frameCount_; ++i) {
    alloc_[i].Reset();
  }
  fence_.Reset();
  if (fenceEvent_) {
    CloseHandle(fenceEvent_);
    fenceEvent_ = nullptr;
  }
  device_.Reset();
}

// frameIndex にはバックバッファのインデックスを渡す
void CommandContext::BeginFrame(uint32_t frameIndex) {
  currentFrameIndex_ = frameIndex;
  WaitForFrame(frameIndex); // 同じバックバッファを使った前回のフレームが完了していることを保証する
  HRESULT hr = alloc_[frameIndex]->Reset();
  assert(SUCCEEDED(hr));
  hr = list_->Reset(alloc_[frameIndex].Get(), nullptr);
  assert(SUCCEEDED(hr));
}

void CommandContext::EndFrame() {
  HRESULT hr = list_->Close();
  assert(SUCCEEDED(hr));
  ID3D12CommandList *lists[] = {list_.Get()};
  queue_->ExecuteCommandLists(1, lists);

  const uint64_t signalValue = ++globalFenceValue_;
  hr = queue_->Signal(fence_.Get(), signalValue);
  assert(SUCCEEDED(hr));
  frameFenceValue_[currentFrameIndex_] = signalValue;
}

void CommandContext::WaitForFrame(uint32_t frameIndex) {
  const uint64_t target = frameFenceValue_[frameIndex];
  if (target == 0)
    return; // まだ投げていない
  if (fence_->GetCompletedValue() < target) {
    HRESULT hr = fence_->SetEventOnCompletion(target, fenceEvent_);
    assert(SUCCEEDED(hr));
    WaitForSingleObject(fenceEvent_, INFINITE);
  }
}

void CommandContext::FlushGPU() {
  if (!queue_) {
    return;
  }
  const uint64_t v = ++globalFenceValue_;
  HRESULT hr = queue_->Signal(fence_.Get(), v);
  assert(SUCCEEDED(hr));
  if (fence_->GetCompletedValue() < v) {
    hr = fence_->SetEventOnCompletion(v, fenceEvent_);
    assert(SUCCEEDED(hr));
    WaitForSingleObject(fenceEvent_, INFINITE);
  }
}

uint64_t CommandContext::GetCompletedFenceValue() const {
  if (!fence_) return 0;
  return fence_->GetCompletedValue();
}

uint64_t CommandContext::ExecuteAndReset() {
  HRESULT hr = list_->Close();
  assert(SUCCEEDED(hr));
  ID3D12CommandList *lists[] = {list_.Get()};
  queue_->ExecuteCommandLists(1, lists);
  
  const uint64_t signalValue = ++globalFenceValue_;
  hr = queue_->Signal(fence_.Get(), signalValue);
  assert(SUCCEEDED(hr));

  // GPU の実行完了を待たないと、直後の allocator->Reset() でクラッシュする（D3D12 の規約違反）
  if (fence_->GetCompletedValue() < signalValue) {
    hr = fence_->SetEventOnCompletion(signalValue, fenceEvent_);
    assert(SUCCEEDED(hr));
    WaitForSingleObject(fenceEvent_, INFINITE);
  }

  hr = alloc_[0]->Reset();
  assert(SUCCEEDED(hr));
  hr = list_->Reset(alloc_[0].Get(), nullptr);
  assert(SUCCEEDED(hr));

  return signalValue;
}

// 単発のトランジションバリアを発行する
void CommandContext::Transition(ID3D12Resource *res,
                                D3D12_RESOURCE_STATES before,
                                D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER b{};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = res;
  b.Transition.StateBefore = before;
  b.Transition.StateAfter = after;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  list_->ResourceBarrier(1, &b);
}
