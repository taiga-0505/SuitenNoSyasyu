#include "Mouse.h"
#include <cassert>

Mouse::Mouse(IDirectInput8* directInput, HWND hwnd) {
    HRESULT hr = directInput->CreateDevice(GUID_SysMouse, device_.GetAddressOf(), NULL);
    assert(SUCCEEDED(hr));
    hr = device_->SetDataFormat(&c_dfDIMouse);
    assert(SUCCEEDED(hr));
    hr = device_->SetCooperativeLevel(hwnd, DISCL_FOREGROUND | DISCL_NONEXCLUSIVE);
    assert(SUCCEEDED(hr));
}

Mouse::~Mouse() {
    if (device_) {
        device_->Unacquire();
        device_.Reset();
    }
}

void Mouse::Update() {
    preState_ = state_;

    HRESULT hr = device_->Acquire();
    if (SUCCEEDED(hr) || hr == S_FALSE) {
        device_->GetDeviceState(sizeof(DIMOUSESTATE), &state_);
    }
}

bool Mouse::IsPressed(int button) const {
    return (state_.rgbButtons[button] & 0x80);
}

bool Mouse::IsTrigger(int button) const {
    return (state_.rgbButtons[button] & 0x80) && !(preState_.rgbButtons[button] & 0x80);
}

bool Mouse::IsRelease(int button) const {
    return !(state_.rgbButtons[button] & 0x80) && (preState_.rgbButtons[button] & 0x80);
}

LONG Mouse::GetX() const { return state_.lX; }
LONG Mouse::GetY() const { return state_.lY; }
LONG Mouse::GetZ() const { return state_.lZ; }
