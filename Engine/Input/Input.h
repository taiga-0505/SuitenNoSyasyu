#pragma once
#include "EngineConfig.h"
#include <memory>
#include "InputCommon.h"

// 各デバイスクラスの前方宣言
class Keyboard;
class Mouse;
class Controller;

/// @class Input
/// @brief 全ての入力デバイス（キーボード、マウス、コントローラー）を統合管理するクラス
/// @details DirectInput と XInput をラップし、各デバイスの状態更新とアクセスを提供する。
///          従来のフラットな API との互換性維持のためのメソッドも提供している。
class Input {
public:
    /// @brief DirectInput を初期化し、各デバイスを生成する
    /// @param hwnd ウィンドウハンドル
    /// @note 生成したインスタンスを GetInstance() で取れるように登録する。
    Input(HWND hwnd);

    /// @brief カーソルロックを解除し、インスタンスの登録を外す
    ~Input();

    /// @brief 全てのデバイスの状態を更新する（毎フレーム呼び出す必要がある）
    void Update();

    /// @brief Viewport がホバーされているかを設定する
    /// @param hovered ホバーされていれば true
    void SetViewportHovered(bool hovered) { viewportHovered_ = hovered; }

    /// @brief Viewport がホバーされているかを取得する
    /// @return ホバーされていれば true
    bool IsViewportHovered() const { return viewportHovered_; }

    // デバイスへのアクセス

    /// @brief キーボード管理オブジェクトを取得する
    /// @return Keyboard ポインタ
    Keyboard* GetKeyboard() const { return keyboard_.get(); }

    /// @brief マウス管理オブジェクトを取得する
    /// @return Mouse ポインタ
    Mouse* GetMouse() const { return mouse_.get(); }

    /// @brief コントローラー管理オブジェクトを取得する
    /// @return Controller ポインタ
    Controller* GetController() const { return controller_.get(); }

    // ============================
    // キーボード入力（互換性維持用）
    // ============================

    /// @brief 指定したキーが押されているか判定する
    /// @param keyCode DIK_* 定数
    /// @return 押されていれば true
    bool IsKeyPressed(uint8_t keyCode) const;

    /// @brief 指定したキーが押されていないか判定する
    /// @param keyCode DIK_* 定数
    /// @return 押されていなければ true
    bool IsKeyUp(uint8_t keyCode) const;

    /// @brief 指定したキーが押された瞬間か判定する
    /// @param keyCode DIK_* 定数
    /// @return 押された瞬間なら true
    bool IsKeyTrigger(uint8_t keyCode) const;

    /// @brief 指定したキーが離された瞬間か判定する
    /// @param keyCode DIK_* 定数
    /// @return 離された瞬間なら true
    bool IsKeyRelease(uint8_t keyCode) const;

    // ============================
    // マウス入力（互換性維持用）
    // ============================

    /// @brief マウスの X 軸移動量を取得する
    /// @return 移動量
    LONG GetMouseX() const;

    /// @brief マウスの Y 軸移動量を取得する
    /// @return 移動量
    LONG GetMouseY() const;

    /// @brief マウスホイールの回転量を取得する
    /// @return 回転量
    LONG GetMouseZ() const;

    /// @brief マウスボタンが押されているか判定する
    /// @param button ボタン番号（0: 左, 1: 右, 2: 中）
    /// @return 押されていれば true
    bool IsMousePressed(int button) const;

    /// @brief マウスボタンが押された瞬間か判定する
    /// @param button ボタン番号
    /// @return 押された瞬間なら true
    bool IsMouseTrigger(int button) const;

    /// @brief マウスボタンが離された瞬間か判定する
    /// @param button ボタン番号
    /// @return 離された瞬間なら true
    bool IsMouseRelease(int button) const;

    // ============================
    // コントローラー入力（互換性維持用）
    // ============================

    /// @brief コントローラーが接続されているか確認する
    /// @return 接続されていれば true
    bool IsXInputConnected() const;

    /// @brief コントローラーボタンが押されているか判定する
    /// @param button XINPUT_GAMEPAD_* 定数
    /// @return 押されていれば true
    bool IsXInputButtonPressed(WORD button) const;

    /// @brief コントローラーボタンが押された瞬間か判定する
    /// @param button XINPUT_GAMEPAD_* 定数
    /// @return 押された瞬間なら true
    bool IsXInputButtonTrigger(WORD button) const;

    /// @brief コントローラーボタンが離された瞬間か判定する
    /// @param button XINPUT_GAMEPAD_* 定数
    /// @return 離された瞬間なら true
    bool IsXInputButtonRelease(WORD button) const;

    /// @brief 左スティックの X 軸入力を取得する
    /// @return -32768〜32767 の値
    SHORT GetXInputThumbLX() const;

    /// @brief 左スティックの Y 軸入力を取得する
    /// @return -32768〜32767 の値
    SHORT GetXInputThumbLY() const;

    /// @brief 右スティックの X 軸入力を取得する
    /// @return -32768〜32767 の値
    SHORT GetXInputThumbRX() const;

    /// @brief 右スティックの Y 軸入力を取得する
    /// @return -32768〜32767 の値
    SHORT GetXInputThumbRY() const;

    /// @brief 左トリガーの押し込み量を取得する
    /// @return 0〜255 の値
    BYTE GetXInputLeftTrigger() const;

    /// @brief 右トリガーの押し込み量を取得する
    /// @return 0〜255 の値
    BYTE GetXInputRightTrigger() const;

    /// @brief コントローラーの振動を設定する
    /// @param leftMotor 左モーター強度（0〜65535）
    /// @param rightMotor 右モーター強度（0〜65535）
    void SetXInputVibration(WORD leftMotor, WORD rightMotor);

    /// @brief コントローラーのデバッグ UI を表示する
    /// @param label ウィンドウラベル
    void ControllerImGui(const char* label = "コントローラー");

    /// @brief Input インスタンスを取得する（シングルトンアクセス用）
    /// @return Input ポインタ（未生成・破棄後は nullptr）
    static Input* GetInstance() { return instance_; }

    /// @brief ゲーム解像度基準のマウス座標を取得する
    /// @param outX 取得した X 座標
    /// @param outY 取得した Y 座標
    /// @note SetGameMousePosition で上書きされていなければ、カーソルのクライアント座標を返す（取得失敗時は 0）。
    void GetGameMousePosition(float& outX, float& outY) const;

    /// @brief エディタ等からゲーム解像度基準のマウス座標を強制上書きする
    /// @param x ゲーム解像度基準の X 座標
    /// @param y ゲーム解像度基準の Y 座標
    /// @note 一度呼ぶと以後は GetGameMousePosition がこの値を返し続ける。
    void SetGameMousePosition(float x, float y) {
        gameMouseX_ = x;
        gameMouseY_ = y;
        isGameMousePosSet_ = true;
    }

    // ============================
    // カーソルロック（視点操作用）
    // ============================

    /// @brief マウスカーソルを画面中央に固定して非表示にする
    /// @details FPS やレールシューターのように「マウスの移動量だけ」で視点を動かす操作で使う。
    ///          ロック中は毎フレーム（Update 内で）カーソルをロック中心へ戻し、
    ///          ウィンドウの外へ出て他のウィンドウをクリックしてしまうのを防ぐ。
    ///          移動量（GetMouseX/Y）は DirectInput から取るので、戻してもゼロにはならない。
    ///          ウィンドウが非アクティブのあいだは戻さない（他のアプリの操作を妨げない）。
    /// @param locked true でロック、false で解除（カーソルも再表示される）
    void SetCursorLocked(bool locked);

    /// @brief カーソルがロック中か
    /// @return ロック中なら true
    bool IsCursorLocked() const { return cursorLocked_; }

    /// @brief ロック中にカーソルを戻す位置をクライアント座標で指定する
    /// @details エディタでは Viewport 画像の中央を渡す。中央に戻しておけば ImGui 側で
    ///          Viewport がホバー状態のままになり、ゲームのクリック判定が通る。
    ///          指定が無ければウィンドウのクライアント領域の中央を使う。
    /// @param x ロック中心の X（クライアント座標）
    /// @param y ロック中心の Y（クライアント座標）
    void SetCursorLockCenter(float x, float y) {
        lockCenterX_ = x;
        lockCenterY_ = y;
        hasLockCenter_ = true;
    }

    /// @brief ロック中心の指定を解除する（ウィンドウ中央に戻る）
    void ClearCursorLockCenter() { hasLockCenter_ = false; }

private:
    /// @brief ロック中ならカーソルをロック中心へ戻す（Update から毎フレーム呼ぶ）
    void UpdateCursorLock();

    /// @brief ロック状態に合わせてカーソルの表示/非表示を切り替える
    /// @details ShowCursor は表示カウンタなので、状態が変わったときに一度だけ増減する。
    void ApplyCursorVisibility();

private:
    static Input* instance_;                 ///< シングルトン用インスタンスポインタ
    Microsoft::WRL::ComPtr<IDirectInput8> directInput_; ///< DirectInput インターフェース

    std::unique_ptr<Keyboard> keyboard_;     ///< キーボード
    std::unique_ptr<Mouse> mouse_;           ///< マウス
    std::unique_ptr<Controller> controller_; ///< コントローラー

    HWND hwnd_ = nullptr;                    ///< ウィンドウハンドル
    bool viewportHovered_ = false;           ///< Viewport ウィンドウがホバーされているか

    float gameMouseX_ = 0.0f;                ///< 上書きされたゲーム解像度基準のマウス X
    float gameMouseY_ = 0.0f;                ///< 上書きされたゲーム解像度基準のマウス Y
    bool isGameMousePosSet_ = false;         ///< SetGameMousePosition で上書き済みか

    bool cursorLocked_ = false;    ///< カーソルロック要求中か
    bool cursorHidden_ = false;    ///< ShowCursor(FALSE) を発行済みか（表示カウンタの二重操作防止）
    bool hasLockCenter_ = false;   ///< ロック中心が外部（エディタ）から指定されているか
    float lockCenterX_ = 0.0f;     ///< ロック中心 X（クライアント座標）
    float lockCenterY_ = 0.0f;     ///< ロック中心 Y（クライアント座標）
};
