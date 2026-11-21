#include "Controller.h"
#include "EngineConfig.h"
#include <string>

#if RC_ENABLE_IMGUI
#include "imgui/imgui.h"
#endif

Controller::Controller() {
    ZeroMemory(&state_, sizeof(XINPUT_STATE));
    ZeroMemory(&preState_, sizeof(XINPUT_STATE));
}

Controller::~Controller() {
    // 終了時に振動を止める
    SetVibration(0, 0);
}

void Controller::Update() {
    preState_ = state_;
    ZeroMemory(&state_, sizeof(XINPUT_STATE));
    XInputGetState(0, &state_);
}

bool Controller::IsConnected() const {
    XINPUT_STATE tempState;
    return XInputGetState(0, &tempState) == ERROR_SUCCESS;
}

bool Controller::IsButtonPressed(WORD button) const {
    return (state_.Gamepad.wButtons & button) != 0;
}

bool Controller::IsButtonTrigger(WORD button) const {
    return (state_.Gamepad.wButtons & button) && !(preState_.Gamepad.wButtons & button);
}

bool Controller::IsButtonRelease(WORD button) const {
    return !(state_.Gamepad.wButtons & button) && (preState_.Gamepad.wButtons & button);
}

SHORT Controller::GetThumbLX() const { return state_.Gamepad.sThumbLX; }
SHORT Controller::GetThumbLY() const { return state_.Gamepad.sThumbLY; }
SHORT Controller::GetThumbRX() const { return state_.Gamepad.sThumbRX; }
SHORT Controller::GetThumbRY() const { return state_.Gamepad.sThumbRY; }
BYTE Controller::GetLeftTrigger() const { return state_.Gamepad.bLeftTrigger; }
BYTE Controller::GetRightTrigger() const { return state_.Gamepad.bRightTrigger; }

void Controller::SetVibration(WORD leftMotor, WORD rightMotor) {
    XINPUT_VIBRATION vibration = {};
    vibration.wLeftMotorSpeed = leftMotor;
    vibration.wRightMotorSpeed = rightMotor;
    XInputSetState(0, &vibration);
}

void Controller::DrawImGui(const char* label) {
#if RC_ENABLE_IMGUI
    std::string base = label ? label : "コントローラー";
    if (!ImGui::CollapsingHeader((base + "##ControllerHeader").c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }

    ImGui::Text("state : ");
    ImGui::SameLine();

    const bool connected = IsConnected();
    if (connected) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 1.0f, 0.0f, 1.0f)); // 緑
        ImGui::Text("コントローラー接続中");
        ImGui::PopStyleColor();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 0, 0, 255)); // 赤
        ImGui::Text("コントローラー未接続");
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0.0f, 5.0f));
        return;
    }

    ImGui::SeparatorText("ボタン");
    auto onoff = [&](bool v) { return v ? "入力" : "未入力"; };

    ImGui::Text("A: %s", onoff(IsButtonPressed(XINPUT_GAMEPAD_A)));
    ImGui::Text("B: %s", onoff(IsButtonPressed(XINPUT_GAMEPAD_B)));
    ImGui::Text("X: %s", onoff(IsButtonPressed(XINPUT_GAMEPAD_X)));
    ImGui::Text("Y: %s", onoff(IsButtonPressed(XINPUT_GAMEPAD_Y)));
    ImGui::Text("LB: %s", onoff(IsButtonPressed(XINPUT_GAMEPAD_LEFT_SHOULDER)));
    ImGui::Text("RB: %s", onoff(IsButtonPressed(XINPUT_GAMEPAD_RIGHT_SHOULDER)));
    ImGui::Text("START: %s", onoff(IsButtonPressed(XINPUT_GAMEPAD_START)));
    ImGui::Text("BACK : %s", onoff(IsButtonPressed(XINPUT_GAMEPAD_BACK)));
    ImGui::Text("Lスティック押し込み: %s", onoff(IsButtonPressed(XINPUT_GAMEPAD_LEFT_THUMB)));
    ImGui::Text("Rスティック押し込み: %s", onoff(IsButtonPressed(XINPUT_GAMEPAD_RIGHT_THUMB)));

    ImGui::SeparatorText("十字キー");
    ImGui::Text("十字上 : %s", onoff(IsButtonPressed(XINPUT_GAMEPAD_DPAD_UP)));
    ImGui::Text("十字下 : %s", onoff(IsButtonPressed(XINPUT_GAMEPAD_DPAD_DOWN)));
    ImGui::Text("十字左 : %s", onoff(IsButtonPressed(XINPUT_GAMEPAD_DPAD_LEFT)));
    ImGui::Text("十字右 : %s", onoff(IsButtonPressed(XINPUT_GAMEPAD_DPAD_RIGHT)));

    ImGui::SeparatorText("トリガー");
    ImGui::Text("LT: %d", GetLeftTrigger());
    ImGui::Text("RT: %d", GetRightTrigger());

    ImGui::SeparatorText("スティック（生値）");
    ImGui::Text("左スティック: X=%d, Y=%d", GetThumbLX(), GetThumbLY());
    ImGui::Text("右スティック: X=%d, Y=%d", GetThumbRX(), GetThumbRY());

    ImGui::SeparatorText("振動");
    ImGui::SliderInt(("左モーター振動##" + base).c_str(), &leftVibration_, 0, 65535);
    ImGui::SliderInt(("右モーター振動##" + base).c_str(), &rightVibration_, 0, 65535);

    if (ImGui::Button(("振動開始##" + base).c_str())) {
        SetVibration(static_cast<WORD>(leftVibration_), static_cast<WORD>(rightVibration_));
    }
    ImGui::SameLine();
    if (ImGui::Button(("振動停止##" + base).c_str())) {
        SetVibration(0, 0);
    }
    ImGui::Dummy(ImVec2(0.0f, 5.0f));
#endif
}
