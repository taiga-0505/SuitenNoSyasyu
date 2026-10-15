#include "VideoRecorder.h"
#include "Common/Log/Log.h"
#include <mfapi.h>
#include <mferror.h>
#include <chrono>
#include <filesystem>
#include <cstdint>
#include <format>

#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")

VideoRecorder::VideoRecorder() {}

VideoRecorder::~VideoRecorder() {
    Stop();
}

bool VideoRecorder::InitializeMF() {
    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        Log::Print(std::format("[VideoRecorder] MFStartup failed. HR: {:08X}", (uint32_t)hr));
        return false;
    }
    return true;
}

void VideoRecorder::TerminateMF() {
    MFShutdown();
}

bool VideoRecorder::Start(ID3D12Device* device, ID3D12CommandQueue* queue, UINT width, UINT height, UINT fps, DXGI_FORMAT format) {
    if (isRecording_) return false;

    device_ = device;
    queue_ = queue;
    videoWidth_ = width;
    videoHeight_ = height;
    videoFps_ = fps;
    videoFormat_ = format;

    if (!InitializeMF()) return false;

    // 保存先フォルダを作成
    if (!std::filesystem::exists("../media")) {
        std::filesystem::create_directory("../media");
    }
    if (!std::filesystem::exists("../media/video")) {
        std::filesystem::create_directory("../media/video");
    }

    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
    localtime_s(&tm, &time_t);
    std::string filename = std::format("../media/video/{:04}{:02}{:02}_{:02}{:02}{:02}.mp4",
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
        tm.tm_hour, tm.tm_min, tm.tm_sec);

    Log logger;
    std::wstring wFilename = logger.ConvertString(filename);

    if (!SetupSinkWriter(wFilename, width, height, fps)) {
        TerminateMF();
        return false;
    }

    // 読み戻し用バッファの準備
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Alignment = 0;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    // コピー時のレイアウト（フットプリント）を計算
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = format;
    texDesc.SampleDesc.Count = 1;

    UINT numRows;
    UINT64 rowSizeInBytes;
    UINT64 totalBytes;
    device_->GetCopyableFootprints(&texDesc, 0, 1, 0, &footprint_, &numRows, &rowSizeInBytes, &totalBytes);

    desc.Width = totalBytes;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_READBACK;
    heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heapProps.CreationNodeMask = 1;
    heapProps.VisibleNodeMask = 1;

    readbackBuffers_.resize(kBufferCount);
    for (UINT i = 0; i < kBufferCount; ++i) {
        HRESULT hr = device_->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readbackBuffers_[i].resource));
        if (FAILED(hr)) {
            Log::Print("[VideoRecorder] Failed to create readback buffer.");
            return false;
        }

        hr = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&readbackBuffers_[i].fence));
        readbackBuffers_[i].fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        readbackBuffers_[i].fenceValue = 0;
        readbackBuffers_[i].inUse = false;

        // バッファごとに専用アロケータ。フェンス完了を待った後にのみ Reset するため、
        // GPU 実行中のアロケータを Reset して D3D12 ERROR になることがない
        hr = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&readbackBuffers_[i].allocator));
        if (FAILED(hr)) {
            Log::Print("[VideoRecorder] Failed to create command allocator.");
            return false;
        }
    }

    device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, readbackBuffers_[0].allocator.Get(), nullptr, IID_PPV_ARGS(&commandList_));
    commandList_->Close();

    frameCount_ = 0;
    currentBufferIndex_ = 0;
    isRecording_ = true;
    latestPath_ = filename;

    Log::Print("[VideoRecorder] Started recording: " + filename);
    return true;
}

bool VideoRecorder::SetupSinkWriter(const std::wstring& path, UINT width, UINT height, UINT fps) {
    HRESULT hr;
    Microsoft::WRL::ComPtr<IMFAttributes> attr;
    MFCreateAttributes(&attr, 1);
    attr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);

    hr = MFCreateSinkWriterFromURL(path.c_str(), nullptr, attr.Get(), &sinkWriter_);
    if (FAILED(hr)) {
        Log::Print("[VideoRecorder] MFCreateSinkWriterFromURL failed.");
        return false;
    }

    // 出力のメディアタイプ（H.264）
    Microsoft::WRL::ComPtr<IMFMediaType> mediaTypeOut;
    MFCreateMediaType(&mediaTypeOut);
    mediaTypeOut->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    mediaTypeOut->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    mediaTypeOut->SetUINT32(MF_MT_AVG_BITRATE, 8000000); // 8Mbps
    mediaTypeOut->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(mediaTypeOut.Get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(mediaTypeOut.Get(), MF_MT_FRAME_RATE, fps, 1);
    MFSetAttributeRatio(mediaTypeOut.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    hr = sinkWriter_->AddStream(mediaTypeOut.Get(), &streamIndex_);
    if (FAILED(hr)) {
        Log::Print("[VideoRecorder] Failed to add H.264 stream.");
        return false;
    }

    // 入力のメディアタイプ（ARGB32）
    Microsoft::WRL::ComPtr<IMFMediaType> mediaTypeIn;
    MFCreateMediaType(&mediaTypeIn);
    mediaTypeIn->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    mediaTypeIn->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_ARGB32); // メモリ上は BGRA の並びを期待する
    mediaTypeIn->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(mediaTypeIn.Get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(mediaTypeIn.Get(), MF_MT_FRAME_RATE, fps, 1);
    MFSetAttributeRatio(mediaTypeIn.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    hr = sinkWriter_->SetInputMediaType(streamIndex_, mediaTypeIn.Get(), nullptr);
    if (FAILED(hr)) {
        Log::Print("[VideoRecorder] Failed to set input media type.");
        return false;
    }

    hr = sinkWriter_->BeginWriting();
    if (FAILED(hr)) {
        Log::Print("[VideoRecorder] BeginWriting failed.");
        return false;
    }

    return true;
}

void VideoRecorder::Stop() {
    if (!isRecording_) return;

    FlushBuffers();

    if (sinkWriter_) {
        sinkWriter_->Finalize();
        sinkWriter_.Reset();
    }

    for (auto& buf : readbackBuffers_) {
        if (buf.fenceEvent) {
            CloseHandle(buf.fenceEvent);
            buf.fenceEvent = nullptr;
        }
    }
    // FlushBuffers で全フェンス待機済み → アロケータ含め安全に破棄できる
    commandList_.Reset();
    readbackBuffers_.clear();

    device_.Reset();
    queue_.Reset();

    TerminateMF();
    isRecording_ = false;

    // 保存完了の通知はログで行う（エディタのコンソールにも表示される）
    Log::Print(std::format("[VideoRecorder] Video saved: {}", latestPath_));
}

void VideoRecorder::FlushBuffers() {
    for (auto& buf : readbackBuffers_) {
        if (buf.inUse) {
            ProcessReadbackBuffer(buf);
        }
    }
}

void VideoRecorder::Update(ID3D12Resource* backBuffer) {
    if (!isRecording_) return;

    auto& currentBuf = readbackBuffers_[currentBufferIndex_];

    // バッファがまだ使用中なら、完了を待って書き出す
    if (currentBuf.inUse) {
        ProcessReadbackBuffer(currentBuf);
    }

    // コピーコマンドを記録
    // currentBuf のフェンスは上で待機済みなので、このアロケータの前回実行は完了している
    currentBuf.allocator->Reset();
    commandList_->Reset(currentBuf.allocator.Get(), nullptr);

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = backBuffer;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commandList_->ResourceBarrier(1, &barrier);

    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = currentBuf.resource.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprint_;

    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = backBuffer;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;

    commandList_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    commandList_->ResourceBarrier(1, &barrier);

    commandList_->Close();

    ID3D12CommandList* lists[] = { commandList_.Get() };
    queue_->ExecuteCommandLists(1, lists);

    currentBuf.fenceValue++;
    queue_->Signal(currentBuf.fence.Get(), currentBuf.fenceValue);
    currentBuf.inUse = true;

    currentBufferIndex_ = (currentBufferIndex_ + 1) % kBufferCount;
}

void VideoRecorder::ProcessReadbackBuffer(ReadbackBuffer& buf) {
    if (buf.fence->GetCompletedValue() < buf.fenceValue) {
        buf.fence->SetEventOnCompletion(buf.fenceValue, buf.fenceEvent);
        WaitForSingleObject(buf.fenceEvent, INFINITE);
    }

    void* mappedData = nullptr;
    HRESULT hr = buf.resource->Map(0, nullptr, &mappedData);
    if (SUCCEEDED(hr)) {
        WriteFrame(mappedData, footprint_.Footprint.RowPitch);
        buf.resource->Unmap(0, nullptr);
    }
    buf.inUse = false;
}

void VideoRecorder::WriteFrame(const void* data, UINT rowPitch) {
    const LONG cbWidth = 4 * videoWidth_;
    const DWORD cbBuffer = cbWidth * videoHeight_;

    Microsoft::WRL::ComPtr<IMFSample> sample;
    Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;

    HRESULT hr = MFCreateSample(&sample);
    if (FAILED(hr)) return;

    hr = MFCreateMemoryBuffer(cbBuffer, &buffer);
    if (FAILED(hr)) return;

    BYTE* pData = nullptr;
    DWORD cbMaxLength = 0;
    DWORD cbCurrentLength = 0;

    hr = buffer->Lock(&pData, &cbMaxLength, &cbCurrentLength);
    if (SUCCEEDED(hr)) {
        const BYTE* pSrc = static_cast<const BYTE*>(data);
        if (videoFormat_ == DXGI_FORMAT_R8G8B8A8_UNORM || videoFormat_ == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
            // R8G8B8A8（メモリ上は RGBA）→ ARGB32（メモリ上は BGRA）へ R と B を入れ替える
            for (UINT y = 0; y < videoHeight_; ++y) {
                const UINT32* srcRow = reinterpret_cast<const UINT32*>(pSrc + y * rowPitch);
                UINT32* dstRow = reinterpret_cast<UINT32*>(pData + y * cbWidth);
                for (UINT x = 0; x < videoWidth_; ++x) {
                    UINT32 pixel = srcRow[x];
                    dstRow[x] = (pixel & 0xFF00FF00) | ((pixel & 0x00FF0000) >> 16) | ((pixel & 0x000000FF) << 16);
                }
            }
        } else {
            for (UINT y = 0; y < videoHeight_; ++y) {
                memcpy(pData + y * cbWidth, pSrc + y * rowPitch, cbWidth);
            }
        }

        buffer->Unlock();
    }

    hr = buffer->SetCurrentLength(cbBuffer);
    hr = sample->AddBuffer(buffer.Get());

    LONGLONG rtTimestamp = (LONGLONG)frameCount_ * 10 * 1000 * 1000 / videoFps_; // 100ns 単位
    sample->SetSampleTime(rtTimestamp);
    sample->SetSampleDuration(10 * 1000 * 1000 / videoFps_);

    hr = sinkWriter_->WriteSample(streamIndex_, sample.Get());
    if (FAILED(hr)) {
        Log::Print(std::format("[VideoRecorder] WriteSample failed. HR: {:08X}", (uint32_t)hr));
    }
    frameCount_++;
}
