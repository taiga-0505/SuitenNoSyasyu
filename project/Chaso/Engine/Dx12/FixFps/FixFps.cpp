#include "FixFps.h"

void FixFps::Initialize() {
    reference_ = std::chrono::steady_clock::now(); 
}

void FixFps::Update() {
  if (targetFps_ <= 0.0f) return;

  // 目標 FPS に基づく 1 フレームの時間を計算
  const std::chrono::microseconds kMinTime(uint64_t(1000000.0f / targetFps_));

  // 現在の時間を取得
  std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
  // 前回の記録からの経過時間を取得
  std::chrono::microseconds elapsed =
      std::chrono::duration_cast<std::chrono::microseconds>(now - reference_);

  // 経過時間が目標時間未満なら待機
  if (elapsed < kMinTime) {
    // 目標時間になるまでスリープとビジーウェイトで待機
    while (std::chrono::steady_clock::now() - reference_ < kMinTime) {
      // 残り時間が 1ms 以上あればスリープし、それ未満ならビジーウェイトに切り替えて精度を出す
      if (std::chrono::steady_clock::now() - reference_ < kMinTime - std::chrono::microseconds(1000)) {
        std::this_thread::sleep_for(std::chrono::microseconds(500));
      }
    }
  }

  // 次のフレームの基準時刻を理想的な時間グリッドに従って更新
  reference_ += kMinTime;

  // 処理が重すぎて 1 フレーム分以上遅れてしまった場合、基準時刻を現在時刻に補正
  if (std::chrono::steady_clock::now() - reference_ > kMinTime) {
    reference_ = std::chrono::steady_clock::now();
  }
}
