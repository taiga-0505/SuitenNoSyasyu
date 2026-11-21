#include "Device.h"
#include "Log/Log.h"
#include <wrl/client.h>
#include <dxgi1_6.h>
#include <format>

Log logger;

using Microsoft::WRL::ComPtr;

void Device::enableDebug_(bool gpuValidation) {
#ifdef _DEBUG
  ComPtr<ID3D12Debug1> debug;
  if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
    debug->EnableDebugLayer();
    if (gpuValidation) {
      debug->SetEnableGPUBasedValidation(TRUE);
    }
  }
#endif
}

IDXGIAdapter4 *Device::pickHardwareAdapter_() {
  IDXGIAdapter4 *result = nullptr;

  ComPtr<IDXGIFactory6> f6;
  factory_->QueryInterface(IID_PPV_ARGS(&f6));

  for (UINT i = 0;; ++i) {
    ComPtr<IDXGIAdapter1> a1;
    if (FAILED(f6->EnumAdapterByGpuPreference(
            i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&a1)))) {
      break;
    }
    DXGI_ADAPTER_DESC1 d{};
    a1->GetDesc1(&d);
    if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
      continue; // ソフトウェアは除外

    // まず 12.1 を試し、ダメなら 12.0 でもよい
    ComPtr<ID3D12Device> probe;
    if (SUCCEEDED(D3D12CreateDevice(a1.Get(), D3D_FEATURE_LEVEL_12_1,
                                    IID_PPV_ARGS(&probe))) ||
        SUCCEEDED(D3D12CreateDevice(a1.Get(), D3D_FEATURE_LEVEL_12_0,
                                    IID_PPV_ARGS(&probe)))) {
      a1->QueryInterface(IID_PPV_ARGS(&result)); // IDXGIAdapter4 に昇格
      break;
    }
  }
  return result;
}

IDXGIAdapter4 *Device::getWarpAdapter_() {
  IDXGIAdapter4 *out = nullptr;
  ComPtr<IDXGIAdapter> warp;
  if (SUCCEEDED(factory_->EnumWarpAdapter(IID_PPV_ARGS(&warp)))) {
    warp->QueryInterface(IID_PPV_ARGS(&out));
  }
  return out;
}

void Device::Init(bool enableDebug, bool gpuValidation) {
  if (enableDebug)
    enableDebug_(gpuValidation);

  UINT factoryFlags = 0;
#ifdef _DEBUG
  if (enableDebug)
    factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
#endif
  HRESULT hr = CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&factory_));
  assert(SUCCEEDED(hr));

  adapter_.Attach(pickHardwareAdapter_());
  if (!adapter_) {
    adapter_.Attach(getWarpAdapter_());
    assert(adapter_ && "No suitable DXGI adapter found");
  }


  // ===== 機能レベルを高い順に試す =====
  const D3D_FEATURE_LEVEL tryLevels[] = {
      D3D_FEATURE_LEVEL_12_2,
      D3D_FEATURE_LEVEL_12_1,
      D3D_FEATURE_LEVEL_12_0,
  };
  const char *tryLevelStr[] = {"12.2", "12.1", "12.0"};

  ID3D12Device *dev = nullptr;
  for (size_t i = 0; i < _countof(tryLevels); ++i) {
    hr = D3D12CreateDevice(adapter_.Get(), tryLevels[i], IID_PPV_ARGS(&dev));
    if (SUCCEEDED(hr)) {
      featureLevel_ = tryLevels[i];
      device_.Attach(dev); // 所有権を移譲（AddRef しない）
      break;
    }
  }
  assert(device_ && "D3D12 device creation failed");
  device_->SetName(L"MainDevice");
}


// ============================================================================
// D3D12 デバッグログの表示設定
// ここで true / false を切り替えるだけで、各レベルのログ出力を制御できる
// ============================================================================
namespace {
  constexpr bool kShowD3D12Info    = false; // INFO: PSO キャッシュなどの情報
  constexpr bool kShowD3D12Warning = true;  // WARNING: リソース解放忘れなどの警告
  constexpr bool kShowD3D12Error   = true;  // ERROR/CORRUPTION: 重大なエラー
}

void Device::SetupInfoQueue(bool breakOnError, bool muteInfo) {
#ifdef _DEBUG
  if (!device_)
    return;
  ComPtr<ID3D12InfoQueue> iq;
  if (SUCCEEDED(device_->QueryInterface(IID_PPV_ARGS(&iq)))) {
    if (muteInfo) {
      D3D12_MESSAGE_SEVERITY sev[] = {D3D12_MESSAGE_SEVERITY_INFO};
      D3D12_INFO_QUEUE_FILTER f{};
      f.DenyList.NumSeverities = _countof(sev);
      f.DenyList.pSeverityList = sev;
      iq->PushStorageFilter(&f);
    }
    if (breakOnError) {
      iq->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
      iq->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
      iq->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, FALSE);
    }
  }

  ComPtr<ID3D12InfoQueue1> iq1;
  if (SUCCEEDED(device_->QueryInterface(IID_PPV_ARGS(&iq1)))) {
    DWORD callbackCookie = 0;
    auto callback = [](D3D12_MESSAGE_CATEGORY Category, D3D12_MESSAGE_SEVERITY Severity,
                       D3D12_MESSAGE_ID ID, LPCSTR pDescription, void* pContext) {
      
      // 設定によるフィルタリング
      if (Severity == D3D12_MESSAGE_SEVERITY_INFO && !kShowD3D12Info) return;
      if (Severity == D3D12_MESSAGE_SEVERITY_WARNING && !kShowD3D12Warning) return;
      if ((Severity == D3D12_MESSAGE_SEVERITY_ERROR || Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION) && !kShowD3D12Error) return;

      std::string sevStr;
      switch (Severity) {
      case D3D12_MESSAGE_SEVERITY_CORRUPTION: sevStr = "CORRUPTION"; break;
      case D3D12_MESSAGE_SEVERITY_ERROR:      sevStr = "ERROR"; break;
      case D3D12_MESSAGE_SEVERITY_WARNING:    sevStr = "WARNING"; break;
      case D3D12_MESSAGE_SEVERITY_INFO:       sevStr = "INFO"; break;
      case D3D12_MESSAGE_SEVERITY_MESSAGE:    sevStr = "MESSAGE"; break;
      default:                                sevStr = "UNKNOWN"; break;
      }
      Log::Print(std::format("D3D12 {}: {}", sevStr, pDescription));
    };
    iq1->RegisterMessageCallback(callback, D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &callbackCookie);
  }
#endif
}

// 表示用の文字列
const char *Device::FeatureLevelString() const {
  switch (featureLevel_) {
  case D3D_FEATURE_LEVEL_12_2:
    return "12.2";
  case D3D_FEATURE_LEVEL_12_1:
    return "12.1";
  case D3D_FEATURE_LEVEL_12_0:
    return "12.0";
  default:
    return "Unknown";
  }
}

bool Device::IsTearingSupported() const {
  BOOL supported = FALSE;
  if (factory_) {
    factory_->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                                  &supported, sizeof(supported));
  }
  return supported == TRUE;
}

void Device::Term() {
  device_.Reset();
  adapter_.Reset();
  factory_.Reset();
}
