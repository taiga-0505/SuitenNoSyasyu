#pragma warning(push)
#pragma warning(disable : 28251)
#include <Windows.h>
#pragma warning(pop)

#include "App.h"
#include "Log/Log.h"
#include <iostream>

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
  // COM 初期化
  HRESULT hrCom = CoInitializeEx(0, COINIT_MULTITHREADED);
  (void)hrCom;

  // ログシステム初期化（ファイル出力開始）
  Log::Initialize();

  App app;
  if (!app.Init()) {
    CoUninitialize();
    return -1;
  }

  const int code = app.Run();

  app.Term();

  // ログシステム終了（ファイルクローズ）
  Log::Finalize();

  CoUninitialize();
  return code;
}
