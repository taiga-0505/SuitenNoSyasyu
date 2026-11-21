#include "Keyboard.h"
#include <cassert>

Keyboard::Keyboard(IDirectInput8* directInput, HWND hwnd) {
    HRESULT hr = directInput->CreateDevice(GUID_SysKeyboard, device_.GetAddressOf(), NULL);
    assert(SUCCEEDED(hr));
    hr = device_->SetDataFormat(&c_dfDIKeyboard);
    assert(SUCCEEDED(hr));
    hr = device_->SetCooperativeLevel(
        hwnd, DISCL_FOREGROUND | DISCL_NONEXCLUSIVE | DISCL_NOWINKEY);
    assert(SUCCEEDED(hr));
}

Keyboard::~Keyboard() {
    if (device_) {
        device_->Unacquire();
        device_.Reset();
    }
}

void Keyboard::Update() {
    // 前フレームの状態を保存
    memcpy(preKey_, key_, sizeof(key_));

    // キーボードの状態を取得
    HRESULT hr = device_->Acquire();
    if (SUCCEEDED(hr) || hr == S_FALSE) {
        device_->GetDeviceState(sizeof(key_), &key_);
    }
}

bool Keyboard::IsPressed(uint8_t keyCode) const {
    return (key_[keyCode] & 0x80);
}

bool Keyboard::IsKeyUp(uint8_t keyCode) const {
    return !(key_[keyCode] & 0x80);
}

bool Keyboard::IsTrigger(uint8_t keyCode) const {
    return (key_[keyCode] & 0x80) && !(preKey_[keyCode] & 0x80);
}

bool Keyboard::IsRelease(uint8_t keyCode) const {
    return !(key_[keyCode] & 0x80) && (preKey_[keyCode] & 0x80);
}
