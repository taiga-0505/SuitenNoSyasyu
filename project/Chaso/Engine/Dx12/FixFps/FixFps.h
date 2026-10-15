#pragma once
#include <chrono>
#include <thread>

/// @class FixFps
/// @brief フレームレートを目標 FPS（既定 60）に固定するためのクラス
class FixFps {
public:
  /// @brief 初期化する（基準時間をリセット）
  void Initialize();

  /// @brief フレームレート調整。前フレームからの経過時間を計測し、必要に応じて待機する
  /// @note Present の直後に毎フレーム呼び出すこと。目標 FPS が 0 以下なら何もしない。
  void Update();

  /// @brief 目標 FPS を設定する
  /// @param fps 目標 FPS
  void SetTargetFps(float fps) { targetFps_ = fps; }
  /// @brief 目標 FPS を取得する
  /// @return 目標 FPS
  float GetTargetFps() const { return targetFps_; }

private:
  std::chrono::steady_clock::time_point reference_; ///< フレーム開始時の基準時間
  float targetFps_ = 60.0f;                         ///< 目標フレームレート
};
