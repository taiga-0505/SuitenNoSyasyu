#include "DeferredReleaseQueue.h"

#include "Common/Log/Log.h"
#include <format>

// ============================================================================
// Enqueue
// ============================================================================

void DeferredReleaseQueue::Enqueue(
    Microsoft::WRL::ComPtr<ID3D12Resource> resource, uint64_t fenceValue) {
  if (!resource) {
    return;
  }
  queue_.push_back({std::move(resource), fenceValue});
}

// ============================================================================
// Flush
// ============================================================================

void DeferredReleaseQueue::Flush(uint64_t completedFenceValue) {
  // 完了済みのリソースを解放する。
  // ※ フェンス値は昇順とは限らない（終了時まで保持する UINT64_MAX の登録が混ざる）ので、
  //    先頭で止めずに全体を見る。以前は先頭で止めていたため、UINT64_MAX が 1 つ入ると
  //    以降の登録がすべて終了時まで解放されなかった。
  for (auto it = queue_.begin(); it != queue_.end();) {
    if (it->fenceValue <= completedFenceValue) {
      it->resource.Reset(); // 参照カウントを減らす（通常はここで解放される）
      it = queue_.erase(it);
    } else {
      ++it;
    }
  }

  // 完了待ちの処理を呼ぶ。呼んだ処理の中から新しく登録されることがあるので、先に取り出してから呼ぶ
  std::deque<CallbackEntry> ready;
  for (auto it = callbacks_.begin(); it != callbacks_.end();) {
    if (it->fenceValue <= completedFenceValue) {
      ready.push_back(std::move(*it));
      it = callbacks_.erase(it);
    } else {
      ++it;
    }
  }
  for (auto &cb : ready) {
    if (cb.fn) cb.fn();
  }
}

void DeferredReleaseQueue::EnqueueCallback(std::function<void()> fn, uint64_t fenceValue) {
  if (!fn) return;
  callbacks_.push_back({std::move(fn), fenceValue});
}

DeferredReleaseQueue *DeferredReleaseQueue::s_global_ = nullptr;
std::function<uint64_t()> DeferredReleaseQueue::s_nextFence_;

void DeferredReleaseQueue::SetGlobal(DeferredReleaseQueue *queue,
                                     std::function<uint64_t()> nextFenceValue) {
  s_global_ = queue;
  s_nextFence_ = std::move(nextFenceValue);
}

void DeferredReleaseQueue::DeferRelease(Microsoft::WRL::ComPtr<ID3D12Resource> resource) {
  if (!resource) return;
  if (s_global_ && s_nextFence_) {
    s_global_->Enqueue(std::move(resource), s_nextFence_());
  }
  // 未登録なら resource はここでスコープを抜けて解放される
}

void DeferredReleaseQueue::DeferCall(std::function<void()> fn) {
  if (!fn) return;
  if (s_global_ && s_nextFence_) {
    s_global_->EnqueueCallback(std::move(fn), s_nextFence_());
  } else {
    fn();
  }
}

// ============================================================================
// FlushAll
// ============================================================================

void DeferredReleaseQueue::FlushAll() {
  if (!queue_.empty() || !callbacks_.empty()) {
    Log::Print(std::format(
        "[DeferredReleaseQueue] FlushAll: {} resources / {} callbacks released",
        queue_.size(), callbacks_.size()));
  }
  // 処理（オブジェクトの破棄など）の中からリソースや処理が追加登録されることがあるので、
  // 両方が空になるまで繰り返す
  while (!callbacks_.empty() || !queue_.empty()) {
    std::deque<CallbackEntry> pending;
    pending.swap(callbacks_);
    for (auto &cb : pending) {
      if (cb.fn) cb.fn();
    }
    queue_.clear();
  }
}
