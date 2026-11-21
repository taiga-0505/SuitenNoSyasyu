#pragma once

// ============================================================================
// FileWatcher
// ----------------------------------------------------------------------------
// ファイルの更新日時を一定間隔で見て、書き換えられたファイルを知らせる（ホットリロード用）。
// OS の変更通知は使わず、更新日時を見比べるだけの単純な作り。
//
//   Chaso::FileWatcher w;
//   w.AddDirectory("Resources/Shader", {".hlsl", ".hlsli"});
//   // 毎フレーム
//   for (const auto& path : w.Poll(dt)) { ...作り直す... }
//
// - 初めて見たファイル（最初の Poll・後から増えたファイル）は日時を覚えるだけで、何も知らせない。
// - 書き込み途中のファイルを読まないよう、更新日時が 1 回分の間隔だけ変わらなくなってから知らせる。
// - SetFileProvider で「今読み込んでいるファイルの一覧」を渡すと、それも監視する（テクスチャなど）。
// ============================================================================

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace Chaso {

/// @class FileWatcher
/// @brief 更新日時を見比べてファイルの書き換えを知らせる
class FileWatcher {
public:
  /// @brief フォルダを監視対象に加える（サブフォルダも含む）
  /// @param dir        フォルダ
  /// @param extensions 対象の拡張子（".hlsl" など。小文字で。空ならすべて）
  void AddDirectory(const std::string &dir, std::vector<std::string> extensions) {
    dirs_.push_back({dir, std::move(extensions)});
  }

  /// @brief フォルダ以外に監視するファイルの一覧を返す関数を設定する（毎回呼ばれる）
  void SetFileProvider(std::function<std::vector<std::string>()> provider) {
    provider_ = std::move(provider);
  }

  /// @brief 調べる間隔（秒）
  void SetInterval(float seconds) { interval_ = (std::max)(0.05f, seconds); }

  /// @brief 時間を進め、間隔が来たら調べる
  /// @param deltaTime 経過時間（秒。実時間を渡す）
  /// @return 書き換えが確定したファイル（今回は無ければ空）
  std::vector<std::string> Poll(float deltaTime) {
    timer_ += deltaTime;
    if (timer_ < interval_) return {};
    timer_ = 0.0f;
    return Scan();
  }

  /// @brief 今すぐ調べる
  std::vector<std::string> Scan() {
    std::vector<std::string> changed;
    std::unordered_map<std::string, bool> seen;
    const auto visit = [&](const std::filesystem::path &p) {
      std::error_code ec;
      const auto t = std::filesystem::last_write_time(p, ec);
      if (ec) return;
      const std::string key = p.lexically_normal().generic_string();
      if (seen.count(key)) return;
      seen[key] = true;
      const int64_t stamp = static_cast<int64_t>(t.time_since_epoch().count());

      auto it = files_.find(key);
      if (it == files_.end()) {
        // 初めて見たファイルは日時を覚えるだけで知らせない。
        // （SetFileProvider の一覧には、シーン切り替えなどで後から読み込まれたテクスチャが増えていく。
        //   それを「書き換えられた」と知らせると、読み込むたびに無駄な再読み込みと GPU 待ちが起きる）
        files_[key] = State{stamp, 0, false};
        return;
      }
      State &st = it->second;
      if (stamp != st.reported) {
        if (st.pendingValid && stamp == st.pending) {
          // 前回見た日時から変わっていない → 書き込みが終わったとみなして知らせる
          st.reported = stamp;
          st.pendingValid = false;
          changed.push_back(key);
        } else {
          st.pending = stamp;
          st.pendingValid = true;
        }
      } else {
        st.pendingValid = false;
      }
    };

    for (const auto &d : dirs_) {
      std::error_code ec;
      if (!std::filesystem::is_directory(d.path, ec)) continue;
      for (auto it = std::filesystem::recursive_directory_iterator(
               d.path, std::filesystem::directory_options::skip_permission_denied, ec);
           !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        if (!MatchesExtension_(it->path(), d.extensions)) continue;
        visit(it->path());
      }
    }
    if (provider_) {
      for (const auto &f : provider_()) visit(std::filesystem::path(f));
    }
    initialized_ = true;
    return changed;
  }

private:
  struct Dir {
    std::string path;
    std::vector<std::string> extensions;
  };
  struct State {
    int64_t reported = 0;      ///< 知らせ済み（または最初に見た）日時
    int64_t pending = 0;       ///< 変わったのを見つけた日時（次も同じなら知らせる）
    bool pendingValid = false; ///< pending が有効か
  };

  static bool MatchesExtension_(const std::filesystem::path &p, const std::vector<std::string> &exts) {
    if (exts.empty()) return true;
    std::string e = p.extension().string();
    for (char &c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return std::find(exts.begin(), exts.end(), e) != exts.end();
  }

  std::vector<Dir> dirs_;
  std::function<std::vector<std::string>()> provider_;
  std::unordered_map<std::string, State> files_;
  float interval_ = 0.5f;
  float timer_ = 0.0f;
  bool initialized_ = false;
};

} // namespace Chaso
