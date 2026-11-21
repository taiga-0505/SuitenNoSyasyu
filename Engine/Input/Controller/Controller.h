#pragma once
#include "../InputCommon.h"

/// @class Controller
/// @brief XInput を使用したゲームコントローラー入力を管理するクラス
/// @details デジタルボタンの状態判定（押下・トリガー・リリース）、スティックのアナログ入力、およびモーターの振動制御を提供する。
///          扱うのは XInput のユーザー 0（1 台目のコントローラー）だけ。
class Controller {
public:
    /// @brief 入力状態をゼロで初期化する
    Controller();

    /// @brief 振動を止めて終了する
    ~Controller();

    /// @brief 入力状態を更新する（毎フレーム呼び出す必要がある）
    void Update();

    /// @brief コントローラーが接続されているか確認する
    /// @return 接続されていれば true
    bool IsConnected() const;

    /// @brief 指定したボタンが押されているか判定する
    /// @param button XINPUT_GAMEPAD_* 定数
    /// @return 押されていれば true
    bool IsButtonPressed(WORD button) const;

    /// @brief 指定したボタンが押された瞬間か判定する
    /// @param button XINPUT_GAMEPAD_* 定数
    /// @return 押された瞬間なら true
    bool IsButtonTrigger(WORD button) const;

    /// @brief 指定したボタンが離された瞬間か判定する
    /// @param button XINPUT_GAMEPAD_* 定数
    /// @return 離された瞬間なら true
    bool IsButtonRelease(WORD button) const;

    /// @brief 左スティックの X 軸入力を取得する
    /// @return -32768〜32767 の値
    SHORT GetThumbLX() const;

    /// @brief 左スティックの Y 軸入力を取得する
    /// @return -32768〜32767 の値
    SHORT GetThumbLY() const;

    /// @brief 右スティックの X 軸入力を取得する
    /// @return -32768〜32767 の値
    SHORT GetThumbRX() const;

    /// @brief 右スティックの Y 軸入力を取得する
    /// @return -32768〜32767 の値
    SHORT GetThumbRY() const;

    /// @brief 左トリガーの押し込み量を取得する
    /// @return 0〜255 の値
    BYTE GetLeftTrigger() const;

    /// @brief 右トリガーの押し込み量を取得する
    /// @return 0〜255 の値
    BYTE GetRightTrigger() const;

    /// @brief バイブレーション（振動）を設定する
    /// @param leftMotor 左モーターの強度（0〜65535）
    /// @param rightMotor 右モーターの強度（0〜65535）
    void SetVibration(WORD leftMotor, WORD rightMotor);

    /// @brief ImGui を使用したデバッグ用 UI を表示する
    /// @param label ウィンドウのラベル
    void DrawImGui(const char* label = "コントローラー");

private:
    XINPUT_STATE state_ = {};    ///< 現在の入力状態
    XINPUT_STATE preState_ = {}; ///< 1 フレーム前の入力状態

    // ImGui の振動テスト用
    int leftVibration_ = 30000;  ///< 左モーターのデフォルト振動強度
    int rightVibration_ = 30000; ///< 右モーターのデフォルト振動強度
};
