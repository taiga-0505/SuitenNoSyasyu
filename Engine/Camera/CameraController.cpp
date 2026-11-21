#include "CameraController.h"
#include "RC.h"
#include "imGui/imGui.h"
#include "imgui/ImGuizmo.h"

namespace RC {

// 画角やクリップは共通、初期姿勢だけ可変にしておく
void CameraController::Initialize(Input *input, const Vector3 &mainPos,
                                  const Vector3 &mainRot, float fovY,
                                  float aspect, float nearZ, float farZ) {
  input_ = input;

  debug_.Initialize(input_, fovY, aspect, nearZ, farZ);
  main_.Initialize(mainPos, mainRot, fovY, aspect, nearZ, farZ);
}

// 各カメラの更新
// ゲームカメラ／デバッグカメラの切り替え（F1）は DataDrivenScene::Update が SetUseDebug で行う。
// ここでも切り替えると同じフレームで 2 回反転して打ち消し合うので、キーは見ない。
void CameraController::Update(float dt) {
  if (useDebug_) {
    #if RC_ENABLE_IMGUI
    // ギズモ操作中またはマウスがギズモに乗っている時はカメラ操作をブロックする
    bool isGuizmoActive = ImGuizmo::IsUsing() || ImGuizmo::IsOver();

    if (!isGuizmoActive) {
        // Viewport 上でマウスボタン（右 / 中）が押されたらドラッグ開始（左クリックはギズモや選択と競合するため除外）
        if (input_->IsViewportHovered() && (input_->IsMousePressed(1) || input_->IsMousePressed(2))) {
          isDraggingCamera_ = true;
        }
    }

    // 右・中のボタンが離されたらドラッグ終了
    if (!input_->IsMousePressed(1) && !input_->IsMousePressed(2)) {
      isDraggingCamera_ = false;
    }

    // マウスが Viewport 上にあるか、カメラをドラッグ中ならマウスをカメラに渡す
    bool captureMouse = ImGui::GetIO().WantCaptureMouse && !(input_->IsViewportHovered() || isDraggingCamera_);

    // Viewport 上にあるかドラッグ中なら、キーボード（WASD 等）もカメラに渡す
    bool captureKeyboard = ImGui::GetIO().WantCaptureKeyboard && !(input_->IsViewportHovered() || isDraggingCamera_);

    if (!(captureMouse || captureKeyboard) && !isGuizmoActive) {
#else
if (true) {
#endif
      debug_.Update(dt);
    }
  } else {
    main_.Update();
  }

  if (useDebug_) {
    worldPos_ = debug_.GetPosition();
  } else {
    worldPos_ = main_.GetPosition();
  }
}

void CameraController::DrawImGui() {
#if RC_ENABLE_IMGUI
  ImGui::Begin("カメラモード : F1");

  if (useDebug_) {
    ImGui::Text("デバッグカメラモード");
    ImGui::Dummy(ImVec2(0.0f, 5.0f));

    if (ImGui::Button("リセット")) {
      debug_.Reset(); // 位置・回転を初期値へ
    }
    ImGui::SameLine();
    if (ImGui::Button("操作方法")) {
      showGuide_ = !showGuide_;
    }
  } else {
    ImGui::Text("メインカメラモード");
  }

  // 現在位置表示
  ImGui::Dummy(ImVec2(0.0f, 5.0f));
  ImGui::Text("位置: (%.2f, %.2f, %.2f)", worldPos_.x, worldPos_.y,
              worldPos_.z);
  ImGui::Dummy(ImVec2(0.0f, 5.0f));

  ImGui::End();

  // 操作ガイド
  if (showGuide_) {
    ImGui::Begin("操作方法 (Blender操作)", &showGuide_, ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::Text("【マウス操作 (Blender準拠)】");
    ImGui::Text(" 中ドラッグ             : 視点回転");
    ImGui::Text(" Shift + 中ドラッグ     : 平行移動 (パン)");
    ImGui::Text(" Ctrl + 中ドラッグ      : ズーム (前後移動)");
    ImGui::Text(" マウスホイール         : ズーム (前後移動)");
    ImGui::Text(" 右ドラッグ             : 視点回転");
    ImGui::Separator();
    ImGui::Text("【キーボード操作】");
    ImGui::Text(" WASD                   : 前後左右移動");
    ImGui::Text(" E / Q                  : 上昇 / 下降");
    ImGui::Text(" Shift (長押し)         : 移動速度加速 (ブースト)");
    ImGui::Text(" Ctrl (長押し)          : 精密移動 (減速)");
    ImGui::Text(" 十字キー               : カメラ回転");
    ImGui::End();
  }
#endif
}

void CameraController::SetMainPosition(const Vector3 &pos) {
  main_.SetPosition(pos);
}

void CameraController::SetMainRotation(const Vector3 &rot) {
  main_.SetRotation(rot);
}

} // namespace RC
