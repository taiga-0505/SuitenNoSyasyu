#pragma once
#include <array>
#include <string>
#include "EngineConfig.h"

/// @struct AppConfig
/// @brief アプリケーションの起動設定を保持する構造体
/// @details 画面解像度、ウィンドウタイトル、V-Sync の有効無効、起動シーン、画面クリアカラーを定義する。
struct AppConfig {
#if defined(RC_DEVELOPMENT)
  int width = 1280;  ///< ウィンドウの幅
  int height = 720; ///< ウィンドウの高さ
  bool fullscreen = false; ///< フルスクリーン表示にするか
#elif defined(_DEBUG)
  int width = 1920;  ///< ウィンドウの幅
  int height = 1080; ///< ウィンドウの高さ
  bool fullscreen = false; ///< フルスクリーン表示にするか
#else
  int width = 1280; ///< ウィンドウの幅
  int height = 720; ///< ウィンドウの高さ
  bool fullscreen = false; ///< フルスクリーン表示にするか
#endif
  bool vsync = true; ///< 垂直同期（V-Sync）を有効にするか

  // ウィンドウタイトルと起動シーンはゲームごとに異なるため AppConfig.json（ゲーム側）で上書きする。
  // 文字列（全構成共通）か、構成別オブジェクトのどちらでも書ける（例）:
  //   "bootScene": "Title"
  //   "bootScene": { "Debug": "Sample", "Development": "Stage1", "Release": "Title" }
  std::string title = "ChasoEngine"; ///< ウィンドウタイトル（AppConfig.json の "title" で上書き）
  std::string bootScene = "Title";   ///< 起動時に読み込むシーン名（AppConfig.json の "bootScene" で上書き）

  std::array<float, 4> clearColor{0.1f, 0.25f, 0.5f, 1.0f}; ///< 画面のクリアカラー（RGBA）
};
