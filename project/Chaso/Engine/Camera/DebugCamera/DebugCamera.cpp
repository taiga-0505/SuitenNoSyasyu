// DebugCamera.cpp
#include "DebugCamera.h"
#include "Math/Math.h" // MakePerspectiveFovMatrix, MakeAffineMatrix, Inverse
#include "Log/Log.h"       // Log::Debug
#include <cmath>           // sinf, cosf
#include <dinput.h>        // DIK_W など

using namespace RC;

#include <algorithm>       // std::clamp

void DebugCamera::Initialize(Input *input, float fovY, float aspect,
                             float nearZ, float farZ) {
  input_ = input;
  // 射影行列を一度だけ作成
  proj_ = MakePerspectiveFovMatrix(fovY, aspect, nearZ, farZ);
}

void DebugCamera::Update(float dt) {
  if (dt > 0.0f) {
    deltaTime_ = dt;
  }

  if (!input_) {
    return;
  }

  // ── 修飾キー判定（Shift, Ctrl） ──
  bool isShift = input_->IsKeyPressed(DIK_LSHIFT) || input_->IsKeyPressed(DIK_RSHIFT);
  bool isCtrl  = input_->IsKeyPressed(DIK_LCONTROL) || input_->IsKeyPressed(DIK_RCONTROL);

  // ── カメラの向きから Forward／Right／Up を計算 ──
  float yaw = rotation_.y;   // Yaw（Y 軸回転）
  float pitch = rotation_.x; // Pitch（X 軸回転）
  Vector3 forward = {std::sinf(yaw) * std::cosf(pitch), std::sinf(pitch),
                     std::cosf(yaw) * std::cosf(pitch)};
  Vector3 right = {std::cosf(yaw), 0.0f, -std::sinf(yaw)};
  // カメラのローカル上方向ベクトル（画面の上方向）
  Vector3 camUp = {-std::sinf(yaw) * std::sinf(pitch), std::cosf(pitch),
                   -std::cosf(yaw) * std::sinf(pitch)};
  Vector3 worldUp = {0.0f, 1.0f, 0.0f};

  // ── 移動速度計算（Shift で加速 / Ctrl で減速） ──
  float currentMoveSpeed = moveSpeed_;
  if (isShift) {
    currentMoveSpeed *= boostMultiplier_;
  } else if (isCtrl) {
    currentMoveSpeed *= 0.25f; // Ctrl 時は精密移動
  }

  // ── キー入力による相対移動（Blender の Walk / Fly Navigation 風） ──
  // WASD: 前後左右
  if (input_->IsKeyPressed(DIK_W)) {
    translation_ =
        Add(translation_, Multiply(forward, currentMoveSpeed * deltaTime_));
  }
  if (input_->IsKeyPressed(DIK_S)) {
    translation_ =
        Subtract(translation_, Multiply(forward, currentMoveSpeed * deltaTime_));
  }
  if (input_->IsKeyPressed(DIK_A)) {
    translation_ =
        Subtract(translation_, Multiply(right, currentMoveSpeed * deltaTime_));
  }
  if (input_->IsKeyPressed(DIK_D)) {
    translation_ =
        Add(translation_, Multiply(right, currentMoveSpeed * deltaTime_));
  }
  // E / Q: 上下移動（Blender の Walk Navigation 準拠: E で上昇、Q で下降）
  if (input_->IsKeyPressed(DIK_E)) {
    translation_.y += currentMoveSpeed * deltaTime_;
  }
  if (input_->IsKeyPressed(DIK_Q)) {
    translation_.y -= currentMoveSpeed * deltaTime_;
  }

  // ── Blender マウス操作 ──
  LONG mouseX = input_->GetMouseX();
  LONG mouseY = input_->GetMouseY();
  LONG wheel  = input_->GetMouseZ();

  // マウス中ボタン（MMB: 2）または右クリック（1）の操作
  if (input_->IsMousePressed(2)) {
    if (isShift) {
      // Shift + 中クリックドラッグ : 平行移動（パン）
      float panSpeed = mousePanSpeed_ * (isShift ? (boostMultiplier_ * 0.5f) : 1.0f);
      // X ドラッグ: カメラを横方向に移動（画面を掴んでドラッグ）
      translation_ = Add(translation_, Multiply(right, -mouseX * panSpeed));
      // Y ドラッグ: カメラを上下方向に移動（画面を掴んでドラッグ）
      translation_ = Add(translation_, Multiply(camUp, mouseY * panSpeed));
    } else if (isCtrl) {
      // Ctrl + 中クリックドラッグ : ズーム（前後移動）
      float zoomSpeed = mouseZoomSpeed_ * (isShift ? boostMultiplier_ : 1.0f);
      translation_ = Add(translation_, Multiply(forward, -mouseY * zoomSpeed));
    } else {
      // 中クリックドラッグ単独 : 視点回転（オービット / 回転）
      rotation_.y += mouseX * mouseRotateSpeed_;
      rotation_.x += mouseY * mouseRotateSpeed_;
    }
  } else if (input_->IsMousePressed(1)) {
    // 右クリックドラッグ : 視点回転（従来操作との互換性と FPS 風の視点操作のため）
    rotation_.y += mouseX * mouseRotateSpeed_;
    rotation_.x += mouseY * mouseRotateSpeed_;
  }

  // ── マウスホイールによる前後移動（ズーム） ──
  if (wheel != 0) {
    float wheelMultiplier = isShift ? boostMultiplier_ : 1.0f;
    translation_ = Add(translation_, Multiply(forward, wheel * mouseWheelSpeed_ * wheelMultiplier));
  }

  // ── 十字キーによる回転 ──
  if (input_->IsKeyPressed(DIK_UP))
    rotation_.x -= rotateSpeed_ * deltaTime_;
  if (input_->IsKeyPressed(DIK_DOWN))
    rotation_.x += rotateSpeed_ * deltaTime_;
  if (input_->IsKeyPressed(DIK_LEFT))
    rotation_.y -= rotateSpeed_ * deltaTime_;
  if (input_->IsKeyPressed(DIK_RIGHT))
    rotation_.y += rotateSpeed_ * deltaTime_;

  // ── Pitch 角の制限（真上・真下を越えて反転・宙返りしないように） ──
  const float maxPitch = 89.0f * (3.14159265f / 180.0f); // 約 1.55 ラジアン（89 度）
  rotation_.x = std::clamp(rotation_.x, -maxPitch, maxPitch);

  // ── ビュー行列の再計算 ──
  RebuildView_();
}

void DebugCamera::Reset() {
  translation_ = {0.0f, 0.0f, -8.0f}; // 初期位置（メンバの初期値と同じ）にリセット
  rotation_ = {0.0f, 0.0f, 0.0f};     // 回転なしにリセット
}

void DebugCamera::SetPosition(const Vector3 &pos) {
  translation_ = pos;
  RebuildView_();
}

void DebugCamera::SetRotation(const Vector3 &rot) {
  rotation_ = rot;
  RebuildView_();
}

void DebugCamera::SetTransform(const Vector3 &pos, const Vector3 &rot) {
  translation_ = pos;
  rotation_ = rot;
  RebuildView_();
}

// ==================
// ビュー行列の再計算
// ==================
void DebugCamera::RebuildView_() {
  Matrix4x4 world = MakeAffineMatrix({1, 1, 1}, rotation_, translation_);
  view_ = Inverse(world);
}
