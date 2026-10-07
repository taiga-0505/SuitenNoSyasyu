#include <dxgidebug.h>
#include <format>
#include <wrl.h>

#include "Common/Log/Log.h"
#include "Dx12Core.h"
#include "RC.h"
#include "Utility/ScreenCapture.h"

using Microsoft::WRL::ComPtr;
// ---------- Debug helpers ----------
static void ReportD3D12LiveObjects(ID3D12Device *device) {
  if (!device)
    return;
  ComPtr<ID3D12DebugDevice> dbg;
  if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dbg)))) {
    // DETAIL を使うと親子関係まで見える
    dbg->ReportLiveDeviceObjects(D3D12_RLDO_DETAIL);
  }
}
static void ReportDXGILiveObjects() {
  ComPtr<IDXGIDebug1> dxgiDebug;
  if (SUCCEEDED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&dxgiDebug)))) {
    // ALL で全カテゴリ。必要なら DXGI_DEBUG_D3D12 だけに絞ってもOK
    dxgiDebug->ReportLiveObjects(DXGI_DEBUG_ALL, DXGI_DEBUG_RLO_ALL);
  }
}

void Dx12Core::Init(HWND hwnd, const Desc &d) {
  desc_ = d;

  // ====================
  // Device
  // ====================
  // デバイス初期化
  device_.Init(d.debug, d.gpuValidation);
  device_.SetupInfoQueue(true, true);
  allowTearing_ = d.allowTearingIfSupported && device_.IsTearingSupported();

  ID3D12Device *dev = device_.GetDevice();

  // ====================
  // Command
  // ====================
  // コマンド初期化
  cmd_.Init(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, d.frameCount);

  // ====================
  // Heaps
  // ====================
  // ディスクリプタヒープ初期化 (RTVはバックバッファ分だけでなく予備を確保)
  rtv_.Init(dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, d.frameCount + 16, false);
  dsv_.Init(dev, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, false);
  srv_.Init(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, d.srvHeapCapacity,
            true);

  // SRV / StructuredBuffer 管理初期化
  srvMgr_.Init(dev, &srv_);
  sbMgr_.Init(&srvMgr_);

  // ====================
  // SwapChain
  // ====================
  // スワップチェーン初期化
  swap_.SetRtvHeap(rtv_.Heap(), rtv_.Increment());
  // スワップチェーンは UNORM で作成（RTVはSRGBビューで作る）
  DXGI_FORMAT scFormat = (desc_.rtvFormat == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
                             ? DXGI_FORMAT_R8G8B8A8_UNORM
                             : desc_.rtvFormat; // それ以外はそのまま
  swap_.Init(device_.Factory(), dev, cmd_.Queue(), hwnd, d.width, d.height,
             scFormat, d.frameCount, allowTearing_);

  for (uint32_t i = 0; i < d.frameCount; ++i) {
    rtv_.AllocateCPU();
  }
  Log::Print(std::format("[Dx12Core] BackBuffer RTV Index: 0 - {}", d.frameCount - 1));

  // ====================
  // Depth
  // ====================
  // 深度バッファ初期化
  DXGI_FORMAT typelessFormat = DXGI_FORMAT_R24G8_TYPELESS;
  if (d.dsvFormat == DXGI_FORMAT_D32_FLOAT) {
      typelessFormat = DXGI_FORMAT_R32_TYPELESS;
  }
  depth_.Init(dev, d.width, d.height, dsv_, typelessFormat, d.dsvFormat);

  // ビューポート/シザー設定
  ResetViewportScissorToBackbuffer(d.width, d.height);

  // フレーム時間の計測（GPU タイムスタンプ）
  InitFrameTimer_();

  // ====================
  // FixFps
  // ====================
  // FPS固定の初期化（VSyncは切らずに追加待ちを入れる運用）
  if (fixFpsEnabled_) {
    fixFps_ = std::make_unique<FixFps>();
    fixFps_->Initialize();
    fixFps_->SetTargetFps(targetFps_);
  }

  // ====================
  // Pipeline
  // ====================
  // 頂点レイアウトの最小例（必要なら外部差し替え）
  D3D12_INPUT_ELEMENT_DESC inputElems[3] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,
       D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
       0},
      {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT,
       D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,
       D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
       0},
  };

  // ====================
  // GPU Info
  // ====================
  // アダプタ情報を取得して表示
  if (device_.Adapter()) {
    DXGI_ADAPTER_DESC3 desc;
    device_.Adapter()->GetDesc3(&desc);
    std::wstring name = desc.Description;
    float vramMB = static_cast<float>(desc.DedicatedVideoMemory) / (1024.0f * 1024.0f);
    Log logger;
    // .c_str() を使うことで、固定長配列内の NUL 終端以降のゴミを除去
    Log::Print(std::format("[Dx12Core] GPU: {} (VRAM: {:.1f} MB)", logger.ConvertString(name.c_str()), vramMB));
    Log::Print(std::format("[Dx12Core] DirectX12 デバイス生成成功 (FeatureLevel: {})", device_.FeatureLevelString()));
  }
}

void Dx12Core::InitFrameTimer_() {
  ID3D12Device *dev = device_.GetDevice();
  if (!dev || !cmd_.Queue()) return;

  D3D12_QUERY_HEAP_DESC qd{};
  qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
  qd.Count = 2;
  if (FAILED(dev->CreateQueryHeap(&qd, IID_PPV_ARGS(&timestampHeap_)))) {
    timestampHeap_.Reset();
    return;
  }

  D3D12_HEAP_PROPERTIES hp{};
  hp.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC rd{};
  rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  rd.Width = sizeof(uint64_t) * 2;
  rd.Height = 1;
  rd.DepthOrArraySize = 1;
  rd.MipLevels = 1;
  rd.SampleDesc.Count = 1;
  rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                          D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                          IID_PPV_ARGS(&timestampReadback_))) ||
      FAILED(cmd_.Queue()->GetTimestampFrequency(&timestampFrequency_)) ||
      timestampFrequency_ == 0) {
    timestampHeap_.Reset();
    timestampReadback_.Reset();
    return;
  }
  timestampReadback_->SetName(L"Dx12Core::timestampReadback_");
  cpuFrameStart_ = std::chrono::steady_clock::now();
}

void Dx12Core::BeginFrame() {
  // ====================
  // Deferred Release
  // ====================
  // 完了済みフェンス値を超えたリソースを解放
  deferredRelease_.Flush(cmd_.GetCompletedFenceValue());

  // ====================
  // Command
  // ====================
  // フレーム開始とバックバッファ取得
  backIndex_ = swap_.CurrentBackBufferIndex();
  cmd_.BeginFrame(backIndex_);

  // GPU 時間の計測開始（フレームの最初のコマンド）
  if (timestampHeap_) {
    cmd_.List()->EndQuery(timestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
  }

  // ====================
  // Resource Transition
  // ====================
  // Present → RenderTarget
  cmd_.Transition(swap_.BackBuffer(backIndex_), D3D12_RESOURCE_STATE_PRESENT,
                  D3D12_RESOURCE_STATE_RENDER_TARGET);

  // ====================
  // Render Target Bind
  // ====================
  // OM バインド
  auto *cl = cmd_.List();
  auto rtv = swap_.RtvAt(backIndex_);
  auto dsv = depth_.Dsv();
  cl->OMSetRenderTargets(1, &rtv, FALSE, &dsv);

  // ====================
  // Descriptor Heap
  // ====================
  // SRV heap セット（ImGui 等を先頭に置いている前提）
  ID3D12DescriptorHeap *heaps[] = {srv_.Heap()};
  cl->SetDescriptorHeaps(1, heaps);

  // ====================
  // Viewport / Scissor
  // ====================
  // ビューポート、シザー矩形セット
  cl->RSSetViewports(1, &viewport_);
  cl->RSSetScissorRects(1, &scissor_);

  // ====================
  // Clear
  // ====================
  // 画面クリア
  Clear();
}

void Dx12Core::Clear(float r, float g, float b, float a) {
  // ====================
  // Clear
  // ====================
  // RT/DSV クリア
  float clear[4] = {r, g, b, a};
  cmd_.List()->ClearRenderTargetView(CurrentRTV(), clear, 0, nullptr);
  cmd_.List()->ClearDepthStencilView(
      Dsv(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0,
      nullptr);
}

void Dx12Core::EndFrame() {
  // ====================
  // Resource Transition
  // ====================
  // RenderTarget → Present
  cmd_.Transition(swap_.BackBuffer(backIndex_),
                  D3D12_RESOURCE_STATE_RENDER_TARGET,
                  D3D12_RESOURCE_STATE_PRESENT);

  // GPU 時間の計測終了（フレームの最後のコマンド）→ 読み出し用バッファへ解決
  if (timestampHeap_) {
    cmd_.List()->EndQuery(timestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
    cmd_.List()->ResolveQueryData(timestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2,
                                  timestampReadback_.Get(), 0);
    timestampPending_ = true;
  }

  // CPU 時間（前フレームの EndFrame 終了〜ここ。GPU 待ち・VSync 待ちを含まない）
  const auto cpuEnd = std::chrono::steady_clock::now();
  cpuFrameMs_ = std::chrono::duration<float, std::milli>(cpuEnd - cpuFrameStart_).count();

  // ====================
  // Present
  // ====================
  // Close→Execute
  cmd_.EndFrame();
  
  // スクリーンショット撮影要求があれば実行
  if (requestScreenshot_) {
    latestScreenshotPath_ = ScreenCapture::SaveScreenshot(device_.GetDevice(), cmd_.Queue(),
                                  swap_.BackBuffer(backIndex_));
    requestScreenshot_ = false;
  }

  // ビデオ録画の更新（録画自体の負荷を切り分けられるよう時間を測る）
  const auto captureStart = std::chrono::steady_clock::now();
  if (videoRecorder_.IsRecording()) {
    videoRecorder_.Update(swap_.BackBuffer(backIndex_));
  }
  captureMs_ = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() -
                                                        captureStart).count();

  // vsync=1, tearingなら 0 でもOK（好みで）
  swap_.Present(1, 0);
  cmd_.WaitForFrame(backIndex_);

  // このフレームの GPU 処理は WaitForFrame で完了しているので、タイムスタンプを読める
  if (timestampPending_ && timestampReadback_) {
    D3D12_RANGE readRange{0, sizeof(uint64_t) * 2};
    void *mapped = nullptr;
    if (SUCCEEDED(timestampReadback_->Map(0, &readRange, &mapped)) && mapped) {
      const uint64_t *ts = static_cast<const uint64_t *>(mapped);
      if (ts[1] > ts[0]) {
        gpuFrameMs_ = static_cast<float>(static_cast<double>(ts[1] - ts[0]) * 1000.0 /
                                         static_cast<double>(timestampFrequency_));
      }
      D3D12_RANGE writeRange{0, 0};
      timestampReadback_->Unmap(0, &writeRange);
    }
    timestampPending_ = false;
  }

  // ====================
  // FixFps
  // ====================
  // VSync & フェンス待ち直後でFPS固定を実行
  if (fixFps_) {
    fixFps_->Update();
  }

  // 次フレームの CPU 時間はここから測る（Present・GPU 待ち・FPS 固定の待ちを除くため）
  cpuFrameStart_ = std::chrono::steady_clock::now();
}

void Dx12Core::WaitForGPU() {
  // ====================
  // GPU Wait
  // ====================
  // GPU 完了待ち
  cmd_.FlushGPU();
}

void Dx12Core::Term() {
  // ====================
  // GPU Wait
  // ====================
  // GPU 完全停止
  cmd_.FlushGPU();

  // ====================
  // Deferred Release
  // ====================
  // 遅延解放キューの全リソースを解放
  deferredRelease_.FlushAll();

  // ====================
  // Resource Release
  // ====================
  // フレーム依存リソースから順に解放
  depth_.Term(); // DSV リソース
  swap_.Term();  // BackBuffer リソース + SwapChain
  sbMgr_.Term();
  srvMgr_.Term();
  rtv_.Term();
  dsv_.Term();
  srv_.Term();

  // ====================
  // Command Release
  // ====================
  // コマンド系解放（Allocator/List/Queue/Fence 等）
  cmd_.Term();

  // ====================
  // Device Release
  // ====================
  // デバイス/ファクトリ解放
  device_.Term();
}

void Dx12Core::Resize(UINT width, UINT height) {
  if (desc_.width == width && desc_.height == height) {
    return;
  }

  desc_.width = width;
  desc_.height = height;

  // GPU の完了を待機
  WaitForGPU();

  // スワップチェーンのバッファリサイズ
  swap_.Resize(width, height);

  // 深度バッファの再生成
  depth_.Resize(width, height, dsv_);

  // ビューポートとシザーのリセット
  ResetViewportScissorToBackbuffer(width, height);
}

void Dx12Core::EnableFixFps(bool enable) {
  // ====================
  // FixFps
  // ====================
  // FPS固定の有効/無効切り替え
  fixFpsEnabled_ = enable;
  if (enable) {
    if (!fixFps_) {
      fixFps_ = std::make_unique<FixFps>();
      fixFps_->Initialize();
      fixFps_->SetTargetFps(targetFps_);
    }
  } else {
    fixFps_.reset();
  }
}

void Dx12Core::SetTargetFps(float fps) {
    targetFps_ = fps;
    if (fixFps_) {
        fixFps_->SetTargetFps(fps);
    }
}

void Dx12Core::StartRecording() {
  if (!videoRecorder_.IsRecording()) {
    videoRecorder_.Start(device_.GetDevice(), cmd_.Queue(), desc_.width, desc_.height, 60, desc_.rtvFormat);
  }
}

void Dx12Core::StopRecording() {
  if (videoRecorder_.IsRecording()) {
    videoRecorder_.Stop();
  }
}
