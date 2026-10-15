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

namespace {
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
  std::filesystem::create_directory("../logs");
  std::filesystem::create_directory("../logs/app");
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
  std::string logFilePath = std::string("../logs/app/") + dateString + ".log";
  // ファイルを開いて保持する
  sLogFile_.open(logFilePath, std::ios::out | std::ios::trunc);
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
