#pragma once
// ============================================================================
// DeferredReleaseQueue
// ----------------------------------------------------------------------------
// GPU が参照を完了するまで D3D12 リソースの解放を遅延するキュー。
// CommandContext のフェンス値を利用して同期判定を行い、
// 完了済みのリソースのみを安全に解放する。
//
// 使用例:
//   // リソースをキューに登録（所有権を移動）
//   queue.Enqueue(std::move(myResource), currentFenceValue);
//
//   // 毎フレーム呼び出して完了済みリソースを解放
//   queue.Flush(completedFenceValue);
//
//   // 終了時に全リソースを強制解放
//   queue.FlushAll();
// ============================================================================

#include <cstdint>
#include <d3d12.h>
#include <deque>
#include <functional>
#include <memory>
#include <wrl/client.h>

/// @class DeferredReleaseQueue
/// @brief GPU が参照を完了するまでリソースの解放を遅延するキュー
/// @details フェンス値に基づいて、GPU が使い終わったリソースだけを安全に解放する。
///          シーン遷移時やリソースの動的差し替え時にクラッシュを防止する。
class DeferredReleaseQueue {
public:
  DeferredReleaseQueue() = default;
  ~DeferredReleaseQueue() { FlushAll(); }

  // コピー禁止
  DeferredReleaseQueue(const DeferredReleaseQueue &) = delete;
  DeferredReleaseQueue &operator=(const DeferredReleaseQueue &) = delete;

  /// @brief 遅延解放キューにリソースを登録する
  /// @param resource 解放予定のリソース (ComPtr で所有権を移動)
  /// @param fenceValue このフェンス値が完了した後に解放される
  void Enqueue(Microsoft::WRL::ComPtr<ID3D12Resource> resource,
               uint64_t fenceValue);

  /// @brief GPU が fenceValue を完了したら呼ぶ処理を登録する
  /// @details GPU リソースを持つオブジェクト（モデル・スプライト・テクスチャ等）の破棄や、
  ///          ディスクリプタの返却を「GPU が使い終わるまで」遅らせるのに使う。
  void EnqueueCallback(std::function<void()> fn, uint64_t fenceValue);

  /// @brief 完了済みのリソースを解放する（毎フレーム呼び出し）
  /// @param completedFenceValue 現在の完了済みフェンス値
  void Flush(uint64_t completedFenceValue);

  /// @brief 全リソースを即座に解放する（終了時用）
  void FlushAll();

  /// @brief キュー内の待機中リソース数を取得する
  /// @return 待機中のリソース数
  size_t PendingCount() const { return queue_.size() + callbacks_.size(); }

  // ── どこからでも使える入口（Dx12Core が Init で登録する） ──

  /// @brief 全体で使うキューと「今記録中のフレームが完了したときのフェンス値」を返す関数を登録する
  static void SetGlobal(DeferredReleaseQueue *queue, std::function<uint64_t()> nextFenceValue);

  /// @brief 今記録中（または直前に送った）フレームを GPU が終えたら resource を解放する
  /// @details 全体のキューが未登録（起動前・終了後）なら即座に解放する。
  static void DeferRelease(Microsoft::WRL::ComPtr<ID3D12Resource> resource);

  /// @brief 全体のキューに溜まっているものを、GPU の完了を待たずにすべて解放する
  /// @warning GPU がアイドルのとき（終了処理・WaitForGPU 直後）だけ呼ぶこと
  static void FlushAllGlobal() {
    if (s_global_) s_global_->FlushAll();
  }

  /// @brief 今記録中（または直前に送った）フレームを GPU が終えたら fn を呼ぶ
  /// @details 全体のキューが未登録なら即座に呼ぶ。
  static void DeferCall(std::function<void()> fn);

  /// @brief オブジェクトの破棄を GPU が使い終わるまで遅らせる（所有権を預かる）
  template <class T> static void DeferDelete(std::unique_ptr<T> obj) {
    if (!obj) return;
    std::shared_ptr<T> holder(std::move(obj));
    DeferCall([holder]() mutable { holder.reset(); });
  }
  template <class T> static void DeferDelete(std::shared_ptr<T> obj) {
    if (!obj) return;
    DeferCall([holder = std::move(obj)]() mutable { holder.reset(); });
  }

private:
  /// @brief 遅延解放エントリ
  struct Entry {
    Microsoft::WRL::ComPtr<ID3D12Resource> resource; ///< 解放待ちリソース
    uint64_t fenceValue;                             ///< 解放条件のフェンス値
  };

  struct CallbackEntry {
    std::function<void()> fn; ///< 完了後に呼ぶ処理
    uint64_t fenceValue;      ///< 呼び出し条件のフェンス値
  };

  std::deque<Entry> queue_;               ///< 解放待ちリソース
  std::deque<CallbackEntry> callbacks_;   ///< 完了待ちの処理

  static DeferredReleaseQueue *s_global_;           ///< 全体で使うキュー
  static std::function<uint64_t()> s_nextFence_;    ///< 今記録中のフレームのフェンス値を返す関数
};
