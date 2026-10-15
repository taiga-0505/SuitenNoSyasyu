#pragma once
#include "../InputCommon.h"

/// @class Mouse
/// @brief DirectInput を使用したマウス入力を管理するクラス
/// @details 各マウスボタンの押下・トリガー・リリース判定に加え、移動量（X, Y）やスクロールホイール（Z）の差分値を取得できる。
class Mouse {
public:
    /// @brief マウスデバイスを作成して初期化する
    /// @param directInput DirectInput8 インターフェースへのポインタ
    /// @param hwnd ウィンドウハンドル
    Mouse(IDirectInput8* directInput, HWND hwnd);

    /// @brief デバイスの入力取得を止めて解放する
    ~Mouse();

    /// @brief 入力状態を更新する（毎フレーム呼び出す必要がある）
    void Update();

    /// @brief 指定したボタンが押されているか判定する
    /// @param button ボタン番号（0: 左, 1: 右, 2: 中）
    /// @return 押されていれば true
    bool IsPressed(int button) const;

    /// @brief 指定したボタンが押された瞬間か判定する
    /// @param button ボタン番号
    /// @return 押された瞬間なら true
    bool IsTrigger(int button) const;

    /// @brief 指定したボタンが離された瞬間か判定する
    /// @param button ボタン番号
    /// @return 離された瞬間なら true
    bool IsRelease(int button) const;

    /// @brief X 軸の移動量（前回更新時からの差分）を取得する
    /// @return X 軸移動量
    LONG GetX() const;

    /// @brief Y 軸の移動量（前回更新時からの差分）を取得する
    /// @return Y 軸移動量
    LONG GetY() const;

    /// @brief ホイールの回転量を取得する
    /// @return スクロール量（奥に回すと正、手前に回すと負）
    LONG GetZ() const;

private:
    Microsoft::WRL::ComPtr<IDirectInputDevice8> device_; ///< マウスデバイス
    DIMOUSESTATE state_ = {};    ///< 現在のマウス状態
    DIMOUSESTATE preState_ = {}; ///< 1 フレーム前のマウス状態
};
