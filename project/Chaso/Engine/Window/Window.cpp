#include "Window.h"
#include <cstring>
#include "imgui/imgui.h"
#include "imgui/backends/imgui_impl_dx12.h"
#include "imgui/backends/imgui_impl_win32.h"
#include "../Common/EngineConfig.h"
#include "../Common/Log/Log.h"
#include <format>

#pragma comment(lib, "winmm.lib") // timeBeginPeriod / timeGetTime を使うのに必要

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd,
                                                             UINT msg,
                                                             WPARAM wparam,
                                                             LPARAM lparam);

// 背景ブラシ解放
Window::~Window() {
  if (hbrBackground_) {
    DeleteObject(hbrBackground_);
    hbrBackground_ = nullptr;
  }
}

// ウィンドウプロシージャ
LRESULT CALLBACK Window::WindowProc(HWND hwnd, UINT msg, WPARAM wparam,
                                    LPARAM lparam) {

  // Alt + 左クリックでボーダーレスウィンドウをドラッグ移動できるようにする
  if (msg == WM_NCHITTEST) {
    if (GetAsyncKeyState(VK_MENU) & 0x8000) {
      return HTCAPTION;
    }
  }

  #if RC_ENABLE_IMGUI
  if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam)) {
    return true; // ImGui が処理した場合は true を返す
  }
#endif

  switch (msg) {
  case WM_DESTROY:
    PostQuitMessage(0);
    return 0;
  case WM_SYSCOMMAND:
    // F10 / Alt 単独押しで Windows のメニュー操作モード（SC_KEYMENU）に入らないようにする。
    // 入るとメッセージループがモーダルになり、次のキーを押すまで描画が止まる。
    // F10 は録画の開始 / 停止に使っている。
    if ((wparam & 0xFFF0) == SC_KEYMENU) {
      return 0;
    }
    break;
  }
  return DefWindowProc(hwnd, msg, wparam, lparam);
}

// 背景ブラシ更新（clearColor_ を GDI COLORREF に変換）
void Window::UpdateBackgroundBrush() {
  if (hbrBackground_) {
    DeleteObject(hbrBackground_);
    hbrBackground_ = nullptr;
  }
  auto toByte = [](float v) -> int {
    if (v < 0.0f)
      v = 0.0f;
    if (v > 1.0f)
      v = 1.0f;
    return static_cast<int>(v * 255.0f + 0.5f);
  };
  COLORREF c = RGB(toByte(clearColor_[0]), toByte(clearColor_[1]),
                   toByte(clearColor_[2]));
  hbrBackground_ = CreateSolidBrush(c);
  wc.hbrBackground = hbrBackground_;
}

namespace {
/// @brief DPI スケーリングの影響を受けずモニターの実解像度を扱えるようにする
/// @details Per-Monitor V2 が使えない環境ではシステム DPI Aware にフォールバックする。
/// @note ウィンドウ生成前に呼ぶこと。
void EnableDpiAwareness() {
  if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
    using SetCtxFn = BOOL(WINAPI *)(HANDLE);
    if (auto setCtx = reinterpret_cast<SetCtxFn>(
            GetProcAddress(user32, "SetProcessDpiAwarenessContext"))) {
      // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2（= -4）
      if (setCtx(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4)))) {
        return;
      }
    }
  }
  SetProcessDPIAware();
}
} // namespace

void Window::Initialize(const char *windowTitle, int &clientWidth,
                        int &clientHeight, bool fullscreen) {

  timeBeginPeriod(1); // タイマー精度を 1 ms に設定

  EnableDpiAwareness(); // ウィンドウ生成前に有効化すること

  // ==============================
  // ウィンドウの初期化
  // ==============================
  std::wstring wTitle = windowTitle ? Utf8ToWString(windowTitle) : L"No Title";

  // ==============================
  // ウィンドウクラスの設定
  // ==============================

  // ウィンドウプロシージャを設定
  wc.lpfnWndProc = WindowProc;
  // ウィンドウクラスの名前を設定
  wc.lpszClassName = L"SampleWindowClass";
  // インスタンスハンドル
  wc.hInstance = GetModuleHandle(nullptr);
  // カーソル
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);

  // ウィンドウクラスを登録
  RegisterClass(&wc);
  // ウィンドウスタイルを設定
#if defined(_DEBUG) || defined(RC_DEVELOPMENT)
  DWORD style = WS_POPUP | WS_VISIBLE;
#else
  DWORD style = fullscreen ? (WS_POPUP | WS_VISIBLE) : WS_OVERLAPPEDWINDOW;
#endif

  int windowW = 0;
  int windowH = 0;
  int x = 0;
  int y = 0;

  if (fullscreen) {
    // 起動したモニター（プライマリ）の実解像度に合わせる
    clientWidth = GetSystemMetrics(SM_CXSCREEN);
    clientHeight = GetSystemMetrics(SM_CYSCREEN);
    windowW = clientWidth;
    windowH = clientHeight;
  } else {
    RECT wrc = {0, 0, clientWidth, clientHeight};
    AdjustWindowRect(&wrc, style, false);
    windowW = wrc.right - wrc.left;
    windowH = wrc.bottom - wrc.top;
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    x = (screenW - windowW) / 2;
    y = (screenH - windowH) / 2;
  }

  // ウィンドウを作成
  hwnd = CreateWindow(wc.lpszClassName,     // 利用するクラス名
                      wTitle.c_str(),       // ウィンドウのタイトル
                      style,                // ウィンドウスタイル
                      x,                    // x 座標
                      y,                    // y 座標
                      windowW,              // 幅
                      windowH,              // 高さ
                      nullptr,              // 親ウィンドウハンドル
                      nullptr,              // メニューハンドル
                      wc.hInstance,         // インスタンスハンドル
                      nullptr               // オプション
  );

  // ウィンドウを表示
  ShowWindow(hwnd, SW_SHOW);

  Log::Print(std::format("[Window] Created: \"{}\" ({}x{}) Fullscreen:{}", windowTitle, clientWidth, clientHeight, fullscreen));
}

void Window::Resize(int& outWidth, int& outHeight, bool fullscreen) {
  if (!hwnd) return;

#if defined(_DEBUG) || defined(RC_DEVELOPMENT)
  DWORD style = WS_POPUP | WS_VISIBLE;
#else
  DWORD style = fullscreen ? (WS_POPUP | WS_VISIBLE) : WS_OVERLAPPEDWINDOW;
#endif
  
  SetWindowLongPtr(hwnd, GWL_STYLE, style);

  if (fullscreen) {
    // ウィンドウが現在表示されているモニターの実解像度に合わせる
    HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(MONITORINFO);
    GetMonitorInfo(monitor, &mi);
    int screenW = mi.rcMonitor.right - mi.rcMonitor.left;
    int screenH = mi.rcMonitor.bottom - mi.rcMonitor.top;
    SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, screenW,
                 screenH, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    outWidth = screenW;
    outHeight = screenH;
  } else {
    RECT wrc = {0, 0, outWidth, outHeight};
    AdjustWindowRect(&wrc, style, false);
    
    // ウィンドウをプライマリモニタの中央に配置する
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int windowW = wrc.right - wrc.left;
    int windowH = wrc.bottom - wrc.top;
    int x = (screenW - windowW) / 2;
    int y = (screenH - windowH) / 2;
    
    SetWindowPos(hwnd, HWND_TOP, x, y, windowW, windowH, SWP_NOZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
  }
  Log::Print(std::format("[Window] Resized: ({}x{}) Fullscreen:{}", outWidth, outHeight, fullscreen));
}

/// @brief UTF-8 文字列をワイド文字列に変換する
/// @param s UTF-8 文字列
/// @return 変換後のワイド文字列（nullptr や変換失敗時は空文字列）
static std::wstring Utf8ToWStringImpl(const char *s) {
  if (!s)
    return L"";
  int lenW = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
  if (lenW <= 0)
    return L"";
  std::wstring w(lenW - 1, L'\0'); // 終端 NUL のぶんは削る
  MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), lenW);
  return w;
}

// クラスの静的メンバとしての窓口（実体は Utf8ToWStringImpl）
std::wstring Window::Utf8ToWString(const char *s) {
  return Utf8ToWStringImpl(s);
}

void Window::SetTitleUTF8(const char *utf8) {
  if (!hwnd)
    return;
  std::wstring w = Utf8ToWStringImpl(utf8);
  SetWindowTextW(hwnd, w.c_str());
}


// クリア色の更新（GDI 背景も同期）
void Window::SetClearColor(float r, float g, float b, float a) {
  clearColor_[0] = r;
  clearColor_[1] = g;
  clearColor_[2] = b;
  clearColor_[3] = a;
  UpdateBackgroundBrush();
}

// RT / DS のクリアを Window 経由で実行
void Window::ClearCurrentRT(ID3D12GraphicsCommandList *cmdList,
                            D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                            const D3D12_CPU_DESCRIPTOR_HANDLE *dsvOpt) const {
  cmdList->ClearRenderTargetView(rtv, clearColor_, 0, nullptr);
  if (dsvOpt) {
    cmdList->ClearDepthStencilView(*dsvOpt, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0,
                                   nullptr);
  }
}
