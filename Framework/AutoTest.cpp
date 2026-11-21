#include "AutoTest.h"
#include "Common/Log/Log.h"
#include "Game.h"
#include "SceneManager.h"
#include <Windows.h>
#include <shellapi.h>
#include <algorithm>
#include <ctime>
#include <filesystem>
#include <format>
#include <utility>

#pragma comment(lib, "shell32.lib")

namespace {

/// @brief 既定の順路の置き場（静的初期化の順序に左右されないよう関数内 static）
/// @return 登録された順路への参照
std::vector<std::string> &DefaultRoute() {
  static std::vector<std::string> route;
  return route;
}

/// @brief UTF-16 の文字列を UTF-8 に変換する
/// @param w 変換元
/// @return UTF-8 文字列
std::string ToUtf8(const std::wstring &w) {
  if (w.empty()) return {};
  const int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0,
                                      nullptr, nullptr);
  std::string s(static_cast<size_t>(len), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), len, nullptr,
                      nullptr);
  return s;
}

/// @brief カンマ区切りの文字列を分割する（空の要素は捨てる）
/// @param text 分割する文字列
/// @return 要素の並び
std::vector<std::string> SplitComma(const std::string &text) {
  std::vector<std::string> out;
  size_t begin = 0;
  while (begin <= text.size()) {
    const size_t end = text.find(',', begin);
    const std::string item = text.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
    if (!item.empty()) out.push_back(item);
    if (end == std::string::npos) break;
    begin = end + 1;
  }
  return out;
}

/// @brief 現在時刻から「YYYYMMDD_hhmmss」を作る
/// @return 日時文字列
std::string Timestamp() {
  const std::time_t now = std::time(nullptr);
  std::tm tm{};
  localtime_s(&tm, &now);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tm);
  return buf;
}

} // namespace

void AutoTest::SetDefaultRoute(std::vector<std::string> route) { DefaultRoute() = std::move(route); }

bool AutoTest::ParseCommandLine(const std::string &bootScene) {
  int argc = 0;
  LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv) return false;

  std::string routeArg;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = ToUtf8(argv[i]);
    if (arg == "--autotest") {
      enabled_ = true;
    } else if (arg.starts_with("--autotest-route=")) {
      routeArg = arg.substr(sizeof("--autotest-route=") - 1);
    } else if (arg.starts_with("--autotest-wait=")) {
      try {
        waitSeconds_ = std::stof(arg.substr(sizeof("--autotest-wait=") - 1));
      } catch (...) {
        Log::Print("[AutoTest] --autotest-wait の値が読めないため既定値を使います: " + arg);
      }
    } else if (arg.starts_with("--autotest-out=")) {
      outDir_ = arg.substr(sizeof("--autotest-out=") - 1);
    }
  }
  LocalFree(argv);

  if (!enabled_) return false;

  route_ = routeArg.empty() ? DefaultRoute() : SplitComma(routeArg);
  if (route_.empty()) route_.push_back(bootScene);
  if (outDir_.empty()) outDir_ = "../media/autotest/" + Timestamp();

  std::string routeText;
  for (const auto &name : route_) routeText += (routeText.empty() ? "" : " -> ") + name;
  Log::Print("[AutoTest] 開始: " + routeText);
  Log::Print(std::format("[AutoTest] 待ち時間 {:.1f} 秒 / 保存先 {}", waitSeconds_, outDir_));

  // 1 段目は起動シーン。違う場合は最初のフレームで遷移を要求する
  step_ = 0;
  phase_ = Phase::Arriving;
  changeRequested_ = (route_[0] == bootScene);
  stepStart_ = Clock::now();
  return true;
}

void AutoTest::Update(Game &game) {
  if (!enabled_ || phase_ == Phase::Finished) return;

  auto &sceneMgr = game.GetSceneManager();
  const std::string &target = route_[step_];
  const auto now = Clock::now();

  switch (phase_) {
  case Phase::Arriving: {
    // 遷移中・スクリプトのフェード中は要求が弾かれるので、受理されるまで毎フレーム出し直す
    if (!changeRequested_) {
      const auto names = sceneMgr.GetSceneNames();
      if (std::find(names.begin(), names.end(), target) == names.end()) {
        fail_("未登録のシーン名です: " + target);
        break;
      }
      changeRequested_ = sceneMgr.RequestChange(target);
      if (changeRequested_) Log::Print("[AutoTest] 遷移を要求: " + target);
    }
    const bool arrived = changeRequested_ && sceneMgr.CurrentName() == target &&
                         sceneMgr.ActiveTransition() == SceneTransition::None;
    if (arrived) {
      Log::Print("[AutoTest] 到着: " + target);
      arrivedAt_ = now;
      phase_ = Phase::Settling;
    } else if (std::chrono::duration<float>(now - stepStart_).count() > stepTimeoutSeconds_) {
      fail_(std::format("{} に {:.0f} 秒以内に到着しませんでした（現在: {}）", target,
                        stepTimeoutSeconds_, sceneMgr.CurrentName()));
    }
    break;
  }
  case Phase::Settling:
    // 待っているあいだにスクリプトが勝手に遷移した（デモ・ゲームオーバーなど）
    if (sceneMgr.CurrentName() != target || sceneMgr.ActiveTransition() != SceneTransition::None) {
      fail_(std::format("{} で待機中に別の遷移が始まりました（現在: {}）", target,
                        sceneMgr.CurrentName()));
      break;
    }
    if (std::chrono::duration<float>(now - arrivedAt_).count() >= waitSeconds_) {
      lastCapture_ = std::format("{}/{:02}_{}.png", outDir_, step_ + 1, target);
      pendingCapture_ = lastCapture_;
      phase_ = Phase::Capturing;
    }
    break;
  case Phase::Capturing:
    // 前のフレームの EndFrame で保存済みのはず
    if (std::filesystem::exists(lastCapture_)) {
      Log::Print("[AutoTest] 撮影: " + lastCapture_);
      results_.push_back(std::format("OK  {:02} {} -> {}", step_ + 1, target, lastCapture_));
      advance_(game);
    } else {
      fail_("スクリーンショットを保存できませんでした: " + lastCapture_);
    }
    break;
  case Phase::Finished:
    break;
  }
}

std::string AutoTest::TakeCaptureRequest() { return std::exchange(pendingCapture_, {}); }

void AutoTest::advance_(Game &game) {
  ++step_;
  if (step_ >= route_.size()) {
    finish_();
    return;
  }
  phase_ = Phase::Arriving;
  changeRequested_ = false;
  stepStart_ = Clock::now();
  Update(game); // 遷移の要求はこのフレームのうちに出す
}

void AutoTest::fail_(const std::string &reason) {
  Log::Print("[AutoTest] 失敗: " + reason);
  results_.push_back(std::format("NG  {:02} {} : {}", step_ + 1, route_[step_], reason));
  failed_ = true;
  finish_();
}

void AutoTest::finish_() {
  phase_ = Phase::Finished;
  Log::Print("[AutoTest] ===== 結果 =====");
  for (const auto &line : results_) Log::Print("[AutoTest] " + line);
  for (size_t i = step_ + (failed_ ? 1 : 0); i < route_.size(); ++i) {
    Log::Print(std::format("[AutoTest] --  {:02} {} : 未実施", i + 1, route_[i]));
  }
  const int exitCode = failed_ ? 1 : 0;
  Log::Print(std::format("[AutoTest] {}（終了コード {}）", failed_ ? "FAILED" : "PASSED", exitCode));
  PostQuitMessage(exitCode);
}
