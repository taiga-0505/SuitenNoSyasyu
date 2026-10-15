#pragma once
#include "Log/Log.h"
#include <Windows.h>
#include <cstdint>
#include <d3d12.h>
#include <string>

/// @class Window
/// @brief Win32 のウィンドウを生成・管理するクラス
/// @details ウィンドウの生成・サイズ変更・タイトル変更と、画面クリア色の保持・クリア実行を受け持つ。
class Window {

public:
  static const int32_t kDefaultWidth = 1280; ///< 既定のクライアント幅
  static const int32_t kDefaultHeight = 720; ///< 既定のクライアント高さ

  /// @brief 背景ブラシを解放する
  ~Window();

  /// @brief ウィンドウプロシージャ
  /// @param hwnd   ウィンドウハンドル
  /// @param msg    メッセージ
  /// @param wparam メッセージの追加情報
  /// @param lparam メッセージの追加情報
  /// @return メッセージの処理結果
  /// @details Alt を押しながらのドラッグでウィンドウを移動できるようにし、ImGui にメッセージを渡す。
  ///          F10 / Alt 単独押しでメニュー操作モードに入らないよう SC_KEYMENU を握りつぶす。
  static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wparam,
                                     LPARAM lparam);

  /// @brief ウィンドウを生成する
  /// @param windowTitle ウィンドウタイトル（UTF-8。nullptr なら "No Title"）
  /// @param clientWidth [in/out] クライアント幅。fullscreen 時はモニターの実解像度で上書きされる
  /// @param clientHeight [in/out] クライアント高さ。fullscreen 時はモニターの実解像度で上書きされる
  /// @param fullscreen フルスクリーン（ボーダーレス）で生成するか
  /// @note Debug / RC_DEVELOPMENT ビルドでは fullscreen に関係なく枠なしウィンドウになる。
  void Initialize(const char *windowTitle, int &clientWidth,
                  int &clientHeight, bool fullscreen = false);

  /// @brief ウィンドウのサイズとフルスクリーン設定を動的に変更する
  /// @param outWidth [in/out] クライアント幅。fullscreen 時はモニターの実解像度で上書きされる
  /// @param outHeight [in/out] クライアント高さ。fullscreen 時はモニターの実解像度で上書きされる
  /// @param fullscreen フルスクリーン（ボーダーレス）にするか
  void Resize(int& outWidth, int& outHeight, bool fullscreen);

  /// @brief ウィンドウタイトルを変更する
  /// @param utf8 新しいタイトル（UTF-8）
  void SetTitleUTF8(const char *utf8);

  /// @brief クリア色を設定する（GDI の背景ブラシも同じ色に作り直す）
  /// @param r 赤（0〜1）
  /// @param g 緑（0〜1）
  /// @param b 青（0〜1）
  /// @param a 不透明度（0〜1）
  void SetClearColor(float r, float g, float b, float a = 1.0f);

  /// @brief 保持しているクリア色でレンダーターゲットをクリアする
  /// @param cmdList コマンドリスト
  /// @param rtv     クリアする RTV
  /// @param dsvOpt  一緒に深度をクリアする DSV（nullptr なら深度はクリアしない）
  void
  ClearCurrentRT(ID3D12GraphicsCommandList *cmdList,
                 D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                 const D3D12_CPU_DESCRIPTOR_HANDLE *dsvOpt = nullptr) const;

  /// @brief ウィンドウハンドルを取得する
  /// @return ウィンドウハンドル
  HWND GetHwnd() const { return hwnd; }
  /// @brief クライアント矩形を取得する
  /// @return 既定サイズの矩形（Initialize / Resize では更新しない）
  RECT GetClientRect() const { return wrc; }
  /// @brief インスタンスハンドルを取得する
  /// @return ウィンドウクラスに登録したインスタンスハンドル
  HINSTANCE GetInstance() const { return wc.hInstance; }

  // ==============================
  // ログの初期化
  // ==============================
  Log log; ///< ログ

private:
  WNDCLASS wc{}; ///< ウィンドウクラス

  /// @brief UTF-8 文字列をワイド文字列に変換する
  /// @param s UTF-8 文字列（nullptr なら空文字列を返す）
  /// @return 変換後のワイド文字列
  static std::wstring Utf8ToWString(const char *s);

  // Window が保持するクリア色と背景ブラシ
  float clearColor_[4] = {0.1f, 0.25f, 0.5f, 1.0f}; ///< クリア色（RGBA）

  HBRUSH hbrBackground_ = nullptr; ///< clearColor_ と同じ色の GDI 背景ブラシ

  /// @brief GDI 背景ブラシを clearColor_ に合わせて作り直す（SetClearColor から呼ぶ）
  void UpdateBackgroundBrush();

  RECT wrc{0, 0, kDefaultWidth, kDefaultHeight}; ///< クライアント矩形（既定サイズ）
  HWND hwnd{};                                   ///< ウィンドウハンドル
};
