#include "Input.h"
#include "Keyboard/Keyboard.h"
#include "Mouse/Mouse.h"
#include "Controller/Controller.h"
#include <cassert>

Input* Input::instance_ = nullptr;

Input::Input(HWND hwnd) {
    instance_ = this;
    hwnd_ = hwnd;
    HRESULT hr = DirectInput8Create(
        GetModuleHandle(nullptr), DIRECTINPUT_HEADER_VERSION,
        IID_IDirectInput8, (void**)directInput_.GetAddressOf(), nullptr);
    assert(SUCCEEDED(hr));

    // 各デバイスのインスタンスを生成
    keyboard_ = std::make_unique<Keyboard>(directInput_.Get(), hwnd);
    mouse_ = std::make_unique<Mouse>(directInput_.Get(), hwnd);
    controller_ = std::make_unique<Controller>();
}

Input::~Input() {
    // 隠したままにするとエンジン終了後もカーソルが見えなくなるので必ず戻す
    SetCursorLocked(false);
    // 各デバイスのデストラクタが呼ばれる
    instance_ = nullptr;
}

void Input::Update() {
    if (keyboard_) keyboard_->Update();
    if (mouse_) mouse_->Update();
    if (controller_) controller_->Update();
    UpdateCursorLock();
}

// ============================
// カーソルロック
// ============================
void Input::SetCursorLocked(bool locked) {
    if (cursorLocked_ == locked) return;
    cursorLocked_ = locked;
    ApplyCursorVisibility();
    // ロックした瞬間にも中央へ寄せておく（次の Update まで待つと 1 フレーム端に残る）
    if (cursorLocked_) UpdateCursorLock();
}

void Input::ApplyCursorVisibility() {
    const bool hide = cursorLocked_;
    if (hide == cursorHidden_) return;
    cursorHidden_ = hide;
    // ShowCursor は呼び出し回数を数えるカウンタ。0 未満で非表示、0 以上で表示。
    // 他の場所で増減されていても確実に目的の状態へ持っていくためループで合わせる。
    if (hide) {
        while (ShowCursor(FALSE) >= 0) {}
    } else {
        while (ShowCursor(TRUE) < 0) {}
    }
}

void Input::UpdateCursorLock() {
    if (!cursorLocked_ || !hwnd_) return;
    // 非アクティブ時に戻すと、他のウィンドウを操作しているユーザーのカーソルを奪ってしまう
    if (GetForegroundWindow() != hwnd_) return;

    POINT center{};
    if (hasLockCenter_) {
        center.x = static_cast<LONG>(lockCenterX_ + 0.5f);
        center.y = static_cast<LONG>(lockCenterY_ + 0.5f);
    } else {
        RECT rc{};
        if (!GetClientRect(hwnd_, &rc)) return;
        center.x = (rc.right - rc.left) / 2;
        center.y = (rc.bottom - rc.top) / 2;
    }
    ClientToScreen(hwnd_, &center);

    // すでに中央にあるなら何もしない（無駄な WM_MOUSEMOVE を出さない）
    POINT cur{};
    if (GetCursorPos(&cur) && cur.x == center.x && cur.y == center.y) return;
    SetCursorPos(center.x, center.y);
}

// ============================
// キーボード入力 (ラッパー)
// ============================
bool Input::IsKeyPressed(uint8_t keyCode) const { return keyboard_->IsPressed(keyCode); }
bool Input::IsKeyUp(uint8_t keyCode) const { return keyboard_->IsKeyUp(keyCode); }
bool Input::IsKeyTrigger(uint8_t keyCode) const { return keyboard_->IsTrigger(keyCode); }
bool Input::IsKeyRelease(uint8_t keyCode) const { return keyboard_->IsRelease(keyCode); }

// ============================
// マウス入力 (ラッパー)
// ============================
LONG Input::GetMouseX() const { return mouse_->GetX(); }
LONG Input::GetMouseY() const { return mouse_->GetY(); }
LONG Input::GetMouseZ() const { return mouse_->GetZ(); }
bool Input::IsMousePressed(int button) const { return mouse_->IsPressed(button); }
bool Input::IsMouseTrigger(int button) const { return mouse_->IsTrigger(button); }
bool Input::IsMouseRelease(int button) const { return mouse_->IsRelease(button); }

// ============================
// コントローラー入力 (ラッパー)
// ============================
bool Input::IsXInputConnected() const { return controller_->IsConnected(); }
bool Input::IsXInputButtonPressed(WORD button) const { return controller_->IsButtonPressed(button); }
bool Input::IsXInputButtonTrigger(WORD button) const { return controller_->IsButtonTrigger(button); }
bool Input::IsXInputButtonRelease(WORD button) const { return controller_->IsButtonRelease(button); }
SHORT Input::GetXInputThumbLX() const { return controller_->GetThumbLX(); }
SHORT Input::GetXInputThumbLY() const { return controller_->GetThumbLY(); }
SHORT Input::GetXInputThumbRX() const { return controller_->GetThumbRX(); }
SHORT Input::GetXInputThumbRY() const { return controller_->GetThumbRY(); }
BYTE Input::GetXInputLeftTrigger() const { return controller_->GetLeftTrigger(); }
BYTE Input::GetXInputRightTrigger() const { return controller_->GetRightTrigger(); }
void Input::SetXInputVibration(WORD leftMotor, WORD rightMotor) { controller_->SetVibration(leftMotor, rightMotor); }
void Input::ControllerImGui(const char* label) { controller_->DrawImGui(label); }

void Input::GetGameMousePosition(float& outX, float& outY) const {
    if (isGameMousePosSet_) {
        outX = gameMouseX_;
        outY = gameMouseY_;
    } else {
        POINT pt;
        if (GetCursorPos(&pt)) {
            ScreenToClient(hwnd_, &pt);
            outX = static_cast<float>(pt.x);
            outY = static_cast<float>(pt.y);
        } else {
            outX = 0.0f;
            outY = 0.0f;
        }
    }
}
