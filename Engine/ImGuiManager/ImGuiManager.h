#pragma once
#include "AppConfig.h"
#include "Dx12Core.h"
#include <Windows.h>
#include <d3d12.h>

#if RC_ENABLE_IMGUI
#include "imgui/imgui.h"
#include "imgui/backends/imgui_impl_dx12.h"
#include "imgui/backends/imgui_impl_win32.h"
#else
struct ImFont; // Release では型だけ前方宣言
#endif

/// @class ImGuiManager
/// @brief ImGui の初期化、更新、描画を管理するクラス
/// @details DirectX12 バックエンドを使用した ImGui のライフサイクルを管理する。
///          RC_ENABLE_IMGUI マクロによって有効 / 無効が切り替わり、日本語フォントのロード機能も備えている。
class ImGuiManager {
public:
  /// @brief ImGui のコンテキスト・フォント・Win32 / DX12 バックエンドを初期化する
  /// @param hwnd ウィンドウハンドル
  /// @param core Dx12Core インスタンス
  /// @param enableDocking ドッキング機能を有効にするか
  /// @param jpFontSize 日本語フォントのサイズ
  /// @param jpFontPath 日本語フォント（TTF）のパス（ゲーム側 → エンジン側の順で探す）
  /// @note 日本語フォントを読めたらそれを既定フォントにする。ImGui が無効なビルドでも
  ///       SRV のスロット 0 は予約するので、ディスクリプタの並びはビルド間で変わらない。
  ///       初期化済みなら何もしない。
  void
  Init(HWND hwnd, Dx12Core &core, bool enableDocking = true, float jpFontSize = 15.0f,
       const char *jpFontPath = "Resources/fonts/Huninn/Huninn-Regular.ttf");

  /// @brief 新規フレームの開始を宣言する
  /// @note ドッキング有効時は、ここでメインビューポート全体に DockSpace を張る。
  void NewFrame();

  /// @brief 描画コマンドをコマンドリストに記録する
  /// @param cmdList グラフィックスコマンドリスト
  void Render(ID3D12GraphicsCommandList *cmdList);

  /// @brief バックエンドとコンテキストを破棄する
  void Shutdown();

  /// @brief ImGui が有効かつ初期化済みか
  /// @return 有効なら true
  bool Enabled() const { return RC_ENABLE_IMGUI && initialized_; }

  /// @brief デフォルトフォントを取得する
  /// @return ImFont ポインタ（日本語フォントに差し替える前の io.FontDefault。未設定なら nullptr）
  ImFont *GetDefaultFont() const { return fontDefault_; }

  /// @brief 日本語フォントを取得する
  /// @return ImFont ポインタ（読み込みに失敗した場合は nullptr）
  ImFont *GetJPFont() const { return fontJP_; }

private:
  bool initialized_ = false;       ///< 初期化フラグ
  ImFont *fontDefault_ = nullptr; ///< デフォルトフォント
  ImFont *fontJP_ = nullptr;      ///< 日本語フォント
  bool dockingEnabled_ = false;    ///< ドッキング機能が有効か

  /// @brief ImGui 用の SRV スロット（0 番）を予約する
  /// @param core Dx12Core インスタンス
  void reserveSrvSlotForImGui_(Dx12Core &core);
};
