#pragma once
#include <cstdint>
#include "../InputCommon.h"

/// @class Keyboard
/// @brief DirectInput を使用したキーボード入力を管理するクラス
/// @details 全てのキー（256 キー）の押下状態をフレームごとに監視し、トリガー判定やリリース判定を提供する。
class Keyboard {
public:
    /// @brief キーボードデバイスを作成して初期化する
    /// @param directInput DirectInput8 インターフェースへのポインタ
    /// @param hwnd ウィンドウハンドル
    Keyboard(IDirectInput8* directInput, HWND hwnd);

    /// @brief デバイスの入力取得を止めて解放する
    ~Keyboard();

    /// @brief 入力状態を更新する（毎フレーム呼び出す必要がある）
    void Update();

    /// @brief 指定したキーが押されているか判定する
    /// @param keyCode DIK_* 定数
    /// @return 押されていれば true
    bool IsPressed(uint8_t keyCode) const;

    /// @brief 指定したキーが押されていない（または離されている）か判定する
    /// @param keyCode DIK_* 定数
    /// @return 押されていなければ true
    bool IsKeyUp(uint8_t keyCode) const;

    /// @brief 指定したキーが押された瞬間か判定する
    /// @param keyCode DIK_* 定数
    /// @return 押された瞬間なら true
    bool IsTrigger(uint8_t keyCode) const;

    /// @brief 指定したキーが離された瞬間か判定する
    /// @param keyCode DIK_* 定数
    /// @return 離された瞬間なら true
    bool IsRelease(uint8_t keyCode) const;

private:
    Microsoft::WRL::ComPtr<IDirectInputDevice8> device_; ///< キーボードデバイス
    BYTE key_[256] = {};                                ///< 現在の全キー状態
    BYTE preKey_[256] = {};                             ///< 1 フレーム前の全キー状態
};
