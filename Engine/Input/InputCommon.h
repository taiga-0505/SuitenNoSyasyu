/// @file InputCommon.h
/// @brief 入力システム全体で使用する共通のヘッダー、マクロ、ライブラリリンクを定義するヘッダー

#pragma once

#ifndef DIRECTINPUT_VERSION
#define DIRECTINPUT_VERSION 0x0800
#endif

#include <dinput.h>
#include <Xinput.h>
#include <wrl/client.h>
#include <windows.h>

#pragma comment(lib, "xinput.lib")
#pragma comment(lib, "dinput8.lib")
#pragma comment(lib, "dxguid.lib")
