#include "CaptureMode.h"

#include "Common/EngineConfig.h"

#if RC_ENABLE_IMGUI
#include "imgui/imgui.h"
#endif

#include "Common/Log/Log.h"
#include "Dx12/Dx12Core.h"
#include "Input/Input.h"

namespace {

// 撮影モードに入っているか / シーンを再生状態にしてほしいか。
// この 2 つだけは Release ビルドでも参照するのでガードの外に置く。
bool g_active = false;
bool g_wantsPlaying = false;
/// @brief マウスがゲーム画面の上にあるか（撮影モード中は常に true）
bool g_gameHovered = true;

} // namespace

// ===========================================================================
// 公開関数
// ===========================================================================

bool CaptureMode::IsActive() { return g_active; }

bool CaptureMode::WantsPlaying() { return g_wantsPlaying; }

bool CaptureMode::IsGameHovered() { return g_gameHovered; }

void CaptureMode::SetActive(bool active) {
  if (g_active == active) return;
  g_active = active;
  g_wantsPlaying = active;
  // 入った直後の 1 フレームはまだ Draw を通っていないので、
  // ゲーム画面の上にいる前提にしておく（そうしないと初回だけ撃てない）。
  g_gameHovered = true;
  if (active) {
    Log::Print("[Capture] 撮影モードを開始しました（F11 で終了 / F10 で録画の開始・停止）");
  } else {
    Log::Print("[Capture] 撮影モードを終了しました");
  }
}

bool CaptureMode::HandleHotkeys(Dx12Core *core) {
#if RC_ENABLE_IMGUI
  // F10: 録画（MP4）の開始 / 停止。撮影モードとは独立（撮影モード中なら全画面のゲーム画面が写る）
  if (core && ImGui::IsKeyPressed(ImGuiKey_F10, false)) {
    if (core->GetVideoRecorder().IsRecording()) {
      core->StopRecording();
    } else {
      core->StartRecording();
    }
  }

  // F11: 撮影モードの開始 / 終了
  if (ImGui::IsKeyPressed(ImGuiKey_F11, false)) {
    SetActive(!g_active);
    return true;
  }
#else
  (void)core;
#endif
  return false;
}

void CaptureMode::Draw(D3D12_GPU_DESCRIPTOR_HANDLE viewportSrv, Dx12Core *core,
                       Scene *currentScene, float deltaTime) {
#if RC_ENABLE_IMGUI
  if (!g_active) return;

  (void)currentScene;
  (void)deltaTime;

  const ImGuiViewport *vp = ImGui::GetMainViewport();

  // --- ゲーム画面を全画面で敷く ----------------------------------------
  // アスペクト比を保って中央に置く（引き伸ばすと録画で見栄えが悪くなる）。
  {
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    // アスペクト比を保つと上下（または左右）に帯が出る。
    // エディタの背景色（濃いグレー）のままだと録画で目立つので黒にする。
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
    // NoInputs にしてあるのは見た目のためではなく描画順のため。
    // ImGui は NoBringToFrontOnFocus 付きのウィンドウをリストの先頭へ入れる＝
    // 最初に描く＝最背面になるので、それを付けると DockSpace の不透明な背景に
    // ゲーム画面が隠れて真っ暗な動画しか撮れない。
    // かといって外すと、ゲーム画面をクリックした瞬間に前面へ来て字幕を隠す。
    // NoInputs ならホバー判定の対象外になり、順序が固定されたまま
    // マウス入力もゲーム側へ素通りする。
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

    if (ImGui::Begin("##CaptureView", nullptr, flags)) {
      if (viewportSrv.ptr != 0 && vp->Size.x > 0.0f && vp->Size.y > 0.0f) {
        float aspect = 16.0f / 9.0f;
        if (core) {
          const auto &gameVp = core->Viewport();
          if (gameVp.Width > 0.0f && gameVp.Height > 0.0f) {
            aspect = gameVp.Width / gameVp.Height;
          }
        }
        float w = vp->Size.x;
        float h = w / aspect;
        if (h > vp->Size.y) {
          h = vp->Size.y;
          w = h * aspect;
        }
        ImGui::SetCursorPos(ImVec2((vp->Size.x - w) * 0.5f, (vp->Size.y - h) * 0.5f));
        const ImVec2 imageMin = ImGui::GetCursorScreenPos();
        ImGui::Image((ImTextureID)viewportSrv.ptr, ImVec2(w, h));

        // ゲームへマウス座標を渡す。
        // 通常は Viewport パネルがこれをやっているが、撮影モードでは
        // そのパネルを描かないので、ここで同じことをしないと
        // 照準がマウスに追従しなくなる（画面に敷いた矩形が
        // レターボックスで中央寄せされているぶんも差し引く）。
        if (auto *input = Input::GetInstance()) {
          float gameW = w, gameH = h;
          if (core) {
            const auto &gameVp = core->Viewport();
            if (gameVp.Width > 0.0f && gameVp.Height > 0.0f) {
              gameW = gameVp.Width;
              gameH = gameVp.Height;
            }
          }
          const ImVec2 mouse = ImGui::GetMousePos();
          input->SetGameMousePosition(((mouse.x - imageMin.x) / w) * gameW,
                                      ((mouse.y - imageMin.y) / h) * gameH);
        }
      }
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
  }

  // 画面に出すのはゲーム画面だけ。操作できる窓が無いので、
  // マウス入力は常にゲームへ通す。
  g_gameHovered = true;
#else
  (void)viewportSrv;
  (void)core;
  (void)currentScene;
  (void)deltaTime;
#endif
}
