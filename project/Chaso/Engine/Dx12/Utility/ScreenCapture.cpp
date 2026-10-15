#include "ScreenCapture.h"
#include "Common/EngineConfig.h"
#include "DirectXTex/DirectXTex.h"
#include <filesystem>
#include <format>
#include <chrono>
#include "Common/Log/Log.h"
#if RC_ENABLE_IMGUI
#include "imgui/imgui.h"
#endif
#include "Render/RenderContext.h"
#include "Dx12/Dx12Core.h"
#include <shellapi.h>

using namespace Microsoft::WRL;

using namespace Microsoft::WRL;

ScreenCapture::NotifyData ScreenCapture::notify_;

std::string ScreenCapture::SaveScreenshot(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* backBuffer) {
    if (!device || !queue || !backBuffer) return "";

    // 1. リソース情報の取得
    D3D12_RESOURCE_DESC desc = backBuffer->GetDesc();
    
    // 2. 読み戻し（Readback）用リソースの作成に必要なサイズ計算
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    UINT numRows;
    UINT64 rowSizeInBytes;
    UINT64 totalBytes;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &numRows, &rowSizeInBytes, &totalBytes);

    ComPtr<ID3D12Resource> readbackResource;
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC bufferDesc = {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = totalBytes;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    HRESULT hr = device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &bufferDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&readbackResource));

    if (FAILED(hr)) {
        Log::Print("[ScreenCapture] Failed to create readback resource.");
        return "";
    }

    // 3. コピー用コマンドの作成と実行
    ComPtr<ID3D12CommandAllocator> allocator;
    device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    ComPtr<ID3D12GraphicsCommandList> cl;
    device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&cl));

    // バリア: PRESENT（または現在の状態）→ COPY_SOURCE
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = backBuffer;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cl->ResourceBarrier(1, &barrier);

    // コピー実行
    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = readbackResource.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprint;

    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = backBuffer;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;

    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    // バリア戻し: COPY_SOURCE → PRESENT
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    cl->ResourceBarrier(1, &barrier);

    cl->Close();
    ID3D12CommandList* lists[] = { cl.Get() };
    queue->ExecuteCommandLists(1, lists);

    // 4. GPU の完了を待機
    ComPtr<ID3D12Fence> fence;
    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    queue->Signal(fence.Get(), 1);
    HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (fence->GetCompletedValue() < 1) {
        fence->SetEventOnCompletion(1, event);
        WaitForSingleObject(event, INFINITE);
    }
    CloseHandle(event);

    // 5. Map して DirectXTex で保存
    void* mappedData = nullptr;
    hr = readbackResource->Map(0, nullptr, &mappedData);
    if (FAILED(hr)) {
        Log::Print("[ScreenCapture] Failed to map readback resource.");
        return "";
    }

    DirectX::Image image;
    image.width = static_cast<size_t>(desc.Width);
    image.height = static_cast<size_t>(desc.Height);
    image.format = desc.Format;
    image.rowPitch = static_cast<size_t>(footprint.Footprint.RowPitch);
    image.slicePitch = static_cast<size_t>(totalBytes);
    image.pixels = reinterpret_cast<uint8_t*>(mappedData);

    CreateScreenshotDirectory();
    std::string fileName = "../media/screenshots/" + GenerateTimestampFileName() + ".png";
    
    // WIC 保存用にフォーマット調整
    if (image.format == DXGI_FORMAT_R8G8B8A8_UNORM) {
        image.format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    }

    Log logger;
    std::wstring wFileName = logger.ConvertString(fileName);

    hr = DirectX::SaveToWICFile(image, DirectX::WIC_FLAGS_NONE, DirectX::GetWICCodec(DirectX::WIC_CODEC_PNG), wFileName.c_str());
    
    // クリップボードへコピー（DIB 形式）
    DirectX::Blob blob;
    HRESULT hrBlob = DirectX::SaveToWICMemory(image, DirectX::WIC_FLAGS_NONE, DirectX::GetWICCodec(DirectX::WIC_CODEC_BMP), blob);
    if (SUCCEEDED(hrBlob)) {
        if (OpenClipboard(nullptr)) {
            EmptyClipboard();
            // BMP ファイルのヘッダ（BITMAPFILEHEADER 14 バイト）をスキップして DIB 部分をコピー
            size_t dibSize = blob.GetBufferSize() - sizeof(BITMAPFILEHEADER);
            HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, dibSize);
            if (hMem) {
                void* pMem = GlobalLock(hMem);
                if (pMem) {
                    memcpy(pMem, (uint8_t*)blob.GetBufferPointer() + sizeof(BITMAPFILEHEADER), dibSize);
                    GlobalUnlock(hMem);
                    SetClipboardData(CF_DIB, hMem);
                } else {
                    GlobalFree(hMem);
                }
            }
            CloseClipboard();
        }
    }

    readbackResource->Unmap(0, nullptr);

    if (FAILED(hr)) {
        Log::Print(std::format("[ScreenCapture] Failed to save screenshot. HRESULT={:08X}", (uint32_t)hr));
        return "";
    }

    Log::Print(std::format("[ScreenCapture] Screenshot saved: {}", fileName));
    return fileName;
}

void ScreenCapture::DrawImGui(float deltaTime, Dx12Core* core) {
#if RC_ENABLE_IMGUI
    if (!core) return;

    // === 前フレームで破棄予約されたテクスチャを実際に解放 ===
    if (notify_.pendingUnloadHandle >= 0) {
        RC::GetRenderContext().Textures().Unload(notify_.pendingUnloadHandle);
        notify_.pendingUnloadHandle = -1;
    }

    // === 新しいスクリーンショットを検知 ===
    const std::string& capturePath = core->GetLatestScreenshotPath();
    if (!capturePath.empty()) {
        notify_.path = capturePath;
        // テクスチャとしてロード（RC 経由）
        notify_.texHandle = RC::GetRenderContext().Textures().LoadID(capturePath, true);
        notify_.timer = kNotifyDisplayTime;
        notify_.active = true;
        core->ClearLatestScreenshotPath();
    }

    if (notify_.active) {
        notify_.timer -= deltaTime;
        if (notify_.timer <= 0.0f) {
            notify_.active = false;
            // 表示が終わったら次フレームで破棄するように予約
            if (notify_.texHandle >= 0) {
                notify_.pendingUnloadHandle = notify_.texHandle;
                notify_.texHandle = -1;
            }
        }

        // ポップアウト表示（左下）
        float alpha = 1.0f;
        if (notify_.timer < 1.0f) {
            alpha = notify_.timer; // 残り 1 秒でフェードアウト
        }

        ImGui::SetNextWindowPos(ImVec2(10, ImGui::GetIO().DisplaySize.y - 160), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(220, 0)); // 自動高さ
        ImGui::SetNextWindowBgAlpha(alpha * 0.7f);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);

        ImGuiWindowFlags notifyFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                      ImGuiWindowFlags_NoNav;

        if (ImGui::Begin("ScreenshotNotify", nullptr, notifyFlags)) {
            ImGui::Text("Screenshot Saved!");
            if (notify_.texHandle >= 0) {
                D3D12_GPU_DESCRIPTOR_HANDLE srv = RC::GetRenderContext().Textures().GetSrv(notify_.texHandle);
                if (srv.ptr != 0) {
                    if (ImGui::ImageButton("##ScreenshotThumb", (ImTextureID)srv.ptr, ImVec2(200, 112))) {
                        // クリックでファイルを開く
                        std::filesystem::path p(notify_.path);
                        std::wstring absPath = std::filesystem::absolute(p).wstring();
                        ShellExecuteW(NULL, NULL, absPath.c_str(), NULL, NULL, SW_SHOW);

                        notify_.active = false;
                        // クリックされたら次フレームで破棄するように予約
                        if (notify_.texHandle >= 0) {
                            notify_.pendingUnloadHandle = notify_.texHandle;
                            notify_.texHandle = -1;
                        }
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Click to open file");
                    }
                } else {
                    // ロード中はプレースホルダを表示
                    ImGui::Dummy(ImVec2(200, 112));
                    ImGui::Text("Loading...");
                }
            }
        }
        ImGui::End();
        ImGui::PopStyleVar();
    }
#endif
}

void ScreenCapture::CreateScreenshotDirectory() {
    if (!std::filesystem::exists("../media")) {
        std::filesystem::create_directory("../media");
    }
    if (!std::filesystem::exists("../media/screenshots")) {
        std::filesystem::create_directory("../media/screenshots");
    }
}

std::string ScreenCapture::GenerateTimestampFileName() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
    localtime_s(&tm, &time_t);
    return std::format("{:04}{:02}{:02}_{:02}{:02}{:02}", 
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, 
        tm.tm_hour, tm.tm_min, tm.tm_sec);
}
