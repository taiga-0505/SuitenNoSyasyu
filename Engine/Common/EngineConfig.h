#pragma once

/// @file EngineConfig.h
/// @brief エンジン全体の設定（コンパイルスイッチ等）を定義するヘッダー

/// @def RC_ENABLE_IMGUI
/// @brief ImGui の有効・無効を切り替えるマクロ
/// @details デバッグビルド（_DEBUG）または開発ビルド（RC_DEVELOPMENT）の場合に 1、それ以外は 0 となる。
///          事前に定義されていればその値を優先する。
#ifndef RC_ENABLE_IMGUI
#if defined(_DEBUG) || defined(RC_DEVELOPMENT)
#define RC_ENABLE_IMGUI 1
#else
#define RC_ENABLE_IMGUI 0
#endif
#endif
