#include "ImGuiManager.h"
#include "Common/ResourcePath.h"
#include "Log/Log.h"
#include "SRVManager/SRVManager.h"
#include <cassert>
#include <string>

void ImGuiManager::reserveSrvSlotForImGui_(Dx12Core &core) {
  core.SRV().AllocateCPU(1); // スロット 0 を確保（以降は 1〜）
}

void ImGuiManager::Init(HWND hwnd, Dx12Core &core, bool enableDocking, float jpFontSize,
                        const char *jpFontPath) {
  if (initialized_)
    return;

#if RC_ENABLE_IMGUI
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::StyleColorsDark();

  dockingEnabled_ = enableDocking;
  if (dockingEnabled_) {
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
  }

  ImGuiIO &io = ImGui::GetIO();
  fontDefault_ = io.FontDefault;
  const std::string jpFontResolved = Chaso::ResolvePath(jpFontPath); // ゲーム側 → エンジン側
  fontJP_ = io.Fonts->AddFontFromFileTTF(jpFontResolved.c_str(), jpFontSize, nullptr,
                                         io.Fonts->GetGlyphRangesJapanese());
  if (fontJP_)
    io.FontDefault = fontJP_;

  ImGui_ImplWin32_Init(hwnd);

  // SRVManager を UserData に渡して、AllocFn / FreeFn で動的割り当てを行う
  SRVManager *srvMgr = &core.SRVMan();

  ImGui_ImplDX12_InitInfo initInfo = {};
  initInfo.Device = core.GetDevice();
  initInfo.CommandQueue = core.Queue();
  initInfo.NumFramesInFlight = core.FrameCount();
  initInfo.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
  initInfo.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
  initInfo.SrvDescriptorHeap = core.SRV().Heap();
  initInfo.UserData = srvMgr;

  // 動的ディスクリプタアロケータ: SRVManager を使って割り当て
  initInfo.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo *info,
                                     D3D12_CPU_DESCRIPTOR_HANDLE *out_cpu,
                                     D3D12_GPU_DESCRIPTOR_HANDLE *out_gpu) {
    SRVManager *mgr = static_cast<SRVManager *>(info->UserData);
    SRVManager::Handle h = mgr->Allocate();
    *out_cpu = h.cpu;
    *out_gpu = h.gpu;
  };
  initInfo.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo *info,
                                    D3D12_CPU_DESCRIPTOR_HANDLE cpu,
                                    D3D12_GPU_DESCRIPTOR_HANDLE gpu) {
    SRVManager *mgr = static_cast<SRVManager *>(info->UserData);
    // Handle を復元して Free
    SRVManager::Handle h{};
    h.cpu = cpu;
    h.gpu = gpu;
    // index は Free 内で使わないので UINT_MAX のままでも安全だが、
    // 正しいインデックスを復元する
    DescriptorHeap *heap = mgr->Heap();
    if (heap) {
      SIZE_T offset = cpu.ptr - heap->CPUAt(0).ptr;
      SIZE_T increment = mgr->Device()->GetDescriptorHandleIncrementSize(
          D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
      h.index = static_cast<UINT>(offset / increment);
    }
    mgr->Free(h);
  };

  ImGui_ImplDX12_Init(&initInfo);
#endif

  // ImGui の有無に関わらずスロット 0 は予約して並びを固定
  reserveSrvSlotForImGui_(core);

  initialized_ = true;
}


void ImGuiManager::NewFrame() {
#if RC_ENABLE_IMGUI
  if (!initialized_)
    return;
  ImGui_ImplDX12_NewFrame();
  ImGui_ImplWin32_NewFrame();
  ImGui::NewFrame();

  if (dockingEnabled_) {
    ImGuiID dockspace_id = ImGui::GetID("MainDockSpace");
    ImGui::DockSpaceOverViewport(dockspace_id, nullptr, ImGuiDockNodeFlags_None);
  }
#endif
}

void ImGuiManager::Render(ID3D12GraphicsCommandList *cmdList) {
#if RC_ENABLE_IMGUI
  if (!initialized_)
    return;
  // テキスト入力欄上で OS の I ビームカーソルに切り替わると暗い UI 上で見えなくなるため、
  // 通常の矢印カーソルのまま維持する（リサイズ用カーソル等はそのまま）
  if (ImGui::GetMouseCursor() == ImGuiMouseCursor_TextInput) {
    ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);
  }
  ImGui::Render();
  ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cmdList);
#endif
}

void ImGuiManager::Shutdown() {
#if RC_ENABLE_IMGUI
  if (!initialized_)
    return;
  ImGui_ImplDX12_Shutdown();
  ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext();
#endif
  initialized_ = false;
}
