#pragma once

// ============================================================================
// FrameProfiler
// ----------------------------------------------------------------------------
// 1 フレームの CPU 時間を「区間ごと」に測る簡易プロファイラ。
// Performance パネル（EditorManager）に、前フレームの内訳を表示する。
//
// 使い方:
//   {
//     CHASO_PROFILE_SCOPE("Scene/Update");   // このスコープを抜けるまでの時間を加算
//     ...
//   }
//   FrameProfiler::Get().EndFrame();          // 1 フレームに 1 回（App のループ末尾）
//
// - 同じ名前の区間を 1 フレームに何度通っても合計される（影パスの回数ぶん等）。
// - 区間の名前は文字列リテラル（静的寿命）を渡すこと（ポインタで比較・保持する）。
// - 区間は入れ子にしてよい（親の時間には子の時間も含まれる）。
// - Release ビルド（RC_ENABLE_IMGUI = 0）では何もしない。
// ============================================================================

#include "EngineConfig.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>

/// @brief フレーム内の区間ごとの CPU 時間を集計する
class FrameProfiler {
public:
  /// @brief 1 区間の集計値
  struct Entry {
    const char *name = nullptr; ///< 区間名（文字列リテラル）
    float ms = 0.0f;            ///< そのフレームの合計時間
    uint32_t calls = 0;         ///< そのフレームに通った回数
  };

  static constexpr size_t kMaxEntries = 48;

  static FrameProfiler &Get() {
    static FrameProfiler s;
    return s;
  }

  /// @brief 実行時に決まる名前（スクリプト名など）を、プログラム終了まで有効な文字列にする
  /// @details 同じ名前は同じポインタを返す。区間名に std::string の c_str() を直接渡すと、
  ///          持ち主が破棄されたあとに表示側が触って落ちるため、必ずこれを通すこと。
  const char *Intern(const std::string &name) {
    auto it = interned_.find(name);
    if (it != interned_.end()) return it->second->c_str();
    auto owned = std::make_unique<std::string>(name);
    const char *p = owned->c_str();
    interned_.emplace(name, std::move(owned));
    return p;
  }

  /// @brief 区間の時間を加算する
  void Add(const char *name, float ms) {
    for (size_t i = 0; i < count_; ++i) {
      if (current_[i].name == name || std::strcmp(current_[i].name, name) == 0) {
        current_[i].ms += ms;
        ++current_[i].calls;
        return;
      }
    }
    if (count_ < kMaxEntries) {
      current_[count_].name = name;
      current_[count_].ms = ms;
      current_[count_].calls = 1;
      ++count_;
    }
  }

  /// @brief フレームの区切り。集計を前フレームの値として確定し、次フレーム用にリセットする
  void EndFrame() {
    // このフレームに通った区間だけを前フレームの値として残す。
    // 通らなかった区間（別シーンのスクリプトなど）は表から外し、枠を空ける。
    size_t kept = 0;
    for (size_t i = 0; i < count_; ++i) {
      if (current_[i].calls == 0) continue;
      last_[kept] = current_[i];
      current_[kept] = {current_[i].name, 0.0f, 0};
      ++kept;
    }
    count_ = kept;
    lastCount_ = kept;
  }

  /// @brief 前フレームの集計（[0, LastCount()) が有効）
  const std::array<Entry, kMaxEntries> &Last() const { return last_; }
  size_t LastCount() const { return lastCount_; }

private:
  std::array<Entry, kMaxEntries> current_{};
  std::array<Entry, kMaxEntries> last_{};
  size_t count_ = 0;
  size_t lastCount_ = 0;
  std::unordered_map<std::string, std::unique_ptr<std::string>> interned_;
};

/// @brief スコープを抜けるまでの時間を FrameProfiler に加算する
class FrameProfileScope {
public:
  explicit FrameProfileScope(const char *name)
      : name_(name), start_(std::chrono::steady_clock::now()) {}
  ~FrameProfileScope() {
    const auto end = std::chrono::steady_clock::now();
    FrameProfiler::Get().Add(name_, std::chrono::duration<float, std::milli>(end - start_).count());
  }
  FrameProfileScope(const FrameProfileScope &) = delete;
  FrameProfileScope &operator=(const FrameProfileScope &) = delete;

private:
  const char *name_;
  std::chrono::steady_clock::time_point start_;
};

#define CHASO_PROFILE_CONCAT_INNER_(a, b) a##b
#define CHASO_PROFILE_CONCAT_(a, b) CHASO_PROFILE_CONCAT_INNER_(a, b)

#if RC_ENABLE_IMGUI
/// @brief このスコープの CPU 時間を name の区間として計測する
#define CHASO_PROFILE_SCOPE(name) \
  FrameProfileScope CHASO_PROFILE_CONCAT_(chasoProfileScope_, __LINE__)(name)
#else
#define CHASO_PROFILE_SCOPE(name) ((void)0)
#endif
