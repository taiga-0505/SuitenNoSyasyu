#include "Log.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <Windows.h>
#include <algorithm>
#include <format>
#include <string>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <nlohmann/json.hpp>
#include "Common/ResourcePath.h"

namespace {
  /// @brief 配布 exe で残すログファイルの最大数（古いものから削除）
  constexpr size_t kMaxPackagedLogFiles = 20;

  /// @brief 配布パッケージの exe を直接起動したかどうか
  /// @details 開発時: exe は generated/outputs/<構成>/ にあり、作業ディレクトリは project/（VS / build_and_run.ps1）。
  ///          配布時: exe は project/ にあり、ダブルクリックで起動すると作業ディレクトリ = exe のフォルダになる。
  ///          よって「exe のフォルダ == 作業ディレクトリ」なら配布 exe の起動とみなす。
  bool IsPackagedLaunch() {
    namespace fs = std::filesystem;
    std::wstring buf(MAX_PATH, L'\0');
    DWORD len = 0;
    while ((len = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()))) >= buf.size()) {
      buf.resize(buf.size() * 2);
    }
    if (len == 0) return false;
    buf.resize(len);
    std::error_code ec;
    const bool same = fs::equivalent(fs::path(buf).parent_path(), fs::current_path(ec), ec);
    return !ec && same;
  }

  /// @brief AppConfig.json の title（Release 用）を読む。読めなければ "ChasoApp"
  std::string ReadReleaseTitle() {
    std::string title = "ChasoApp";
    std::ifstream ifs(Chaso::kAppConfigPath);
    if (!ifs) return title;
    try {
      nlohmann::json j;
      ifs >> j;
      if (j.contains("title")) {
        const auto &v = j["title"];
        if (v.is_string()) {
          title = v.get<std::string>();
        } else if (v.is_object() && v.contains("Release") && v["Release"].is_string()) {
          title = v["Release"].get<std::string>();
        }
      }
    } catch (...) {
    }
    return title;
  }

  /// @brief 配布 exe 用のログフォルダ（%LOCALAPPDATA%\<タイトル>\logs）を返す。取得できなければ空
  std::filesystem::path GetPackagedLogDir() {
    wchar_t buf[MAX_PATH]{};
    const DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return {};

    // タイトルをフォルダ名に使えるようにする（UTF-8 → wstring、禁止文字は '_'）
    const std::string title = ReadReleaseTitle();
    const int size = MultiByteToWideChar(CP_UTF8, 0, title.c_str(), -1, nullptr, 0);
    std::wstring name(size > 0 ? size - 1 : 0, L'\0');
    if (size > 1) MultiByteToWideChar(CP_UTF8, 0, title.c_str(), -1, name.data(), size);
    for (auto &c : name) {
      if (c < 32 || wcschr(L"<>:\"/\\|?*", c)) c = L'_';
    }
    if (name.empty()) name = L"ChasoApp";

    return std::filesystem::path(buf) / name / L"logs";
  }

  /// @brief 古いログファイルを削除して最大数に収める
  void PruneOldLogs(const std::filesystem::path &dir, size_t keep) {
    namespace fs = std::filesystem;
    std::error_code ec;
    std::vector<fs::path> logs;
    for (const auto &e : fs::directory_iterator(dir, ec)) {
      if (e.is_regular_file(ec) && e.path().extension() == ".log") logs.push_back(e.path());
    }
    if (logs.size() <= keep) return;
    std::sort(logs.begin(), logs.end()); // ファイル名が日時なので名前順 = 古い順
    for (size_t i = 0; i < logs.size() - keep; ++i) fs::remove(logs[i], ec);
  }

  /// @brief 未処理例外をログに書き出してからプロセスを終了させる例外フィルター
  /// @param exceptionPointers 例外情報
  /// @return EXCEPTION_EXECUTE_HANDLER（プロセスを終了させる）
  LONG WINAPI UnhandledExceptionFilterFunc(EXCEPTION_POINTERS* exceptionPointers) {
    std::stringstream ss;
    ss << "========================================\n";
    ss << "Fatal Error: Unhandled Exception\n";
    ss << "Exception Code: 0x" << std::hex << std::uppercase << exceptionPointers->ExceptionRecord->ExceptionCode << "\n";
    ss << "Exception Address: 0x" << exceptionPointers->ExceptionRecord->ExceptionAddress << "\n";

    if (exceptionPointers->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
      ss << "Access Violation: ";
      if (exceptionPointers->ExceptionRecord->NumberParameters >= 2) {
        ss << (exceptionPointers->ExceptionRecord->ExceptionInformation[0] ? "Write" : "Read");
        ss << " at address 0x" << exceptionPointers->ExceptionRecord->ExceptionInformation[1] << "\n";
      }
    }
    ss << "========================================\n";

    Log::Print(ss.str());

    // 強制的にフラッシュ・クローズしてログを確実に書き込む
    Log::Finalize();

    return EXCEPTION_EXECUTE_HANDLER; // プロセスを終了させる
  }
}

// 静的メンバの定義
std::ofstream Log::sLogFile_;
std::vector<std::string> Log::sHistory_;
std::mutex Log::sHistoryMutex_;

void Log::AddHistory(const std::string& message) {
  std::lock_guard<std::mutex> lock(sHistoryMutex_);
  // メモリを食い続けないよう最大 1000 件に制限し、古いものから捨てる
  if (sHistory_.size() >= 1000) {
    sHistory_.erase(sHistory_.begin());
  }
  sHistory_.push_back(message);
}

const std::vector<std::string>& Log::GetHistory() {
  return sHistory_;
}

void Log::ClearHistory() {
  std::lock_guard<std::mutex> lock(sHistoryMutex_);
  sHistory_.clear();
}

void Log::Initialize() {
#ifdef _DEBUG
  if (AttachConsole(ATTACH_PARENT_PROCESS)) {
    FILE* fp;
    freopen_s(&fp, "CONOUT$", "w", stdout);
    freopen_s(&fp, "CONOUT$", "w", stderr);
    std::cout.clear();
    std::cerr.clear();
    SetConsoleOutputCP(CP_UTF8);
  }
#endif

  // 例外フィルターを登録
  SetUnhandledExceptionFilter(UnhandledExceptionFilterFunc);

  // ログのディレクトリを用意
  //   開発環境から起動（VS / build_and_run.ps1。構成問わず）: ../logs/app/
  //   配布パッケージの exe を直接起動                         : %LOCALAPPDATA%\<AppConfig の title.Release>\logs
  std::error_code ec;
  std::filesystem::path logDir = "../logs/app";
  if (IsPackagedLaunch()) {
    if (std::filesystem::path appData = GetPackagedLogDir(); !appData.empty()) {
      std::filesystem::create_directories(appData, ec);
      if (!ec) {
        logDir = appData;
        PruneOldLogs(logDir, kMaxPackagedLogFiles - 1); // これから 1 つ作るので 1 つ空ける
      }
    }
  }
  std::filesystem::create_directories(logDir, ec);
  // 現在の時間を取得（UTC）
  std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
  // ログファイルの名前にコンマ何秒はいらないので、削って秒にする
  std::chrono::time_point<std::chrono::system_clock, std::chrono::seconds>
      nowSeconds = std::chrono::time_point_cast<std::chrono::seconds>(now);
  // PC のタイムゾーン（日本なら JST）のローカル時間に変換
  std::chrono::zoned_time localTime{std::chrono::current_zone(), nowSeconds};
  // format を使って「年月日_時分秒」の文字列に変換
  std::string dateString = std::format("{:%Y-%m-%d_%H-%M-%S}", localTime);
  // 時間を使ってファイル名を決定
  const std::filesystem::path logFilePath = logDir / (dateString + ".log");
  // ファイルを開いて保持する（path で渡すので日本語フォルダ名でも開ける）
  sLogFile_.open(logFilePath, std::ios::out | std::ios::trunc);

  const std::u8string u8path = logFilePath.generic_u8string();
  Print("[Log] Log file: " + std::string(u8path.begin(), u8path.end()));
}

void Log::Finalize() {
  if (sLogFile_.is_open()) {
    sLogFile_.flush();
    sLogFile_.close();
  }
}

void Log::WriteLog(std::ostream &os, const std::string &message) {
  os << message << std::endl;
  OutputDebugStringA(message.c_str());
  
  std::string fullMsg = message + "\n";
  AddHistory(fullMsg);

  // ファイルにも書き込む
  if (sLogFile_.is_open()) {
    sLogFile_ << fullMsg;
  }
}

void Log::WriteLog(const std::string &message) {
  OutputDebugStringA(message.c_str());
  
  std::string fullMsg = message + "\n";
  AddHistory(fullMsg);

  // ファイルにも書き込む
  if (sLogFile_.is_open()) {
    sLogFile_ << fullMsg;
  }
}

void Log::Print(const std::string &message) {
  std::string msg = message;

  // 文字列内の NUL 文字（\0）を除去して表示不具合を防ぐ
  msg.erase(std::remove(msg.begin(), msg.end(), '\0'), msg.end());

  // 末尾に改行がなければ追加
  if (msg.empty() || msg.back() != '\n') {
    msg += "\n";
  }
  
  // 標準出力にも出力する（親コンソールにアタッチしている場合）
  std::cout << msg << std::flush;
  
  // UTF-8 から wstring に変換して出力（日本語対応）
  Log logger;
  std::wstring wmsg = logger.ConvertString(msg);
  OutputDebugStringW(wmsg.c_str());

  AddHistory(msg);

  // ファイルにも書き込む（UTF-8 のまま）
  if (sLogFile_.is_open()) {
    sLogFile_ << msg << std::flush;
  }
}

std::wstring Log::ConvertString(const std::string &str) {
  // 変換後に必要な文字数（終端 NUL 込み）を取得
  int size = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
  // その文字数で wstring を確保
  std::wstring wstr(size, L'\0');
  // string を wstring に変換
  MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &wstr[0], size);
  return wstr;
}

// wstring を string に変換する
std::string Log::ConvertString(const std::wstring &wstr) {
  // 変換後に必要なバイト数（終端 NUL 込み）を取得
  int size = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0,
                                 nullptr, nullptr);
  // そのバイト数で string を確保
  std::string str(size, '\0');
  // wstring を string に変換
  WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &str[0], size, nullptr,
                      nullptr);
  return str;
}

[[noreturn]] void Log::Fatal(const std::string &message) {
  const std::string full = "========================================\n"
                           "[FATAL] " + message + "\n"
                           "========================================";
  Log::Print(full);

  Log logger;
  const std::wstring wmsg = logger.ConvertString(full);

  if (::IsDebuggerPresent()) {
    // デバッガ接続時はその場でブレークし、呼び出し元のスタックを保持する
    ::DebugBreak();
  } else {
    ::MessageBoxW(nullptr, wmsg.c_str(), L"Fatal Error",
                  MB_OK | MB_ICONERROR | MB_TOPMOST);
  }

  // デストラクタを走らせず即座に落とす（半端に初期化された GPU リソースに触らない）
  ::TerminateProcess(::GetCurrentProcess(), 1);
  ::ExitProcess(1); // TerminateProcess が返った場合の保険（[[noreturn]] 担保）
}

std::string Log::NormalizePath(const std::string &path) {
  std::string ret = path;
  std::replace(ret.begin(), ret.end(), '\\', '/');
  return ret;
}
