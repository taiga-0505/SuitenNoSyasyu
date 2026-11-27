#include "RenderFFTOcean.h"
#include "RenderContext.h"
#include "RenderCommon.h"
#include "Dx12/PipelineManager.h"
#include "Dx12/SRVManager/SRVManager.h"
#include "Dx12/Dx12Core.h"
#include "Common/Log/Log.h"
#include <algorithm>
#include <cmath>

namespace RC {

namespace {

/// @brief FFTOcean.hlsli の cbuffer FFTOceanParams と同じ並び
struct FFTOceanCB {
  float tileSize = 128.0f; ///< タイル 1 枚のワールドサイズ（m）
  float time = 0.0f;       ///< 経過時間（秒）
  float windSpeed = 8.0f;  ///< 風速（m/s）
  float gravity = 9.81f;   ///< 重力加速度（m/s²）
  float windDirX = 1.0f;   ///< 風向き X（正規化済み）
  float windDirZ = 0.0f;   ///< 風向き Z
  float smallCutoff = 0.05f; ///< 短い波を弱める長さ（m）
  uint32_t seed = 1;       ///< 乱数の種
};
static_assert(sizeof(FFTOceanCB) == 32, "FFTOceanCB layout must match FFTOcean.hlsli");

constexpr float kGravity = 9.81f;                          ///< 重力加速度（m/s²）
constexpr float kPhillipsAlpha = 0.0081f;                  ///< Phillips 定数（FFTOceanInit.CS と同じ）
constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_R32G32B32A32_FLOAT; ///< 全テクスチャの形式

/// @brief テクスチャの番号
enum Tex : int {
  kH0 = 0,     ///< 初期スペクトル
  kSpecA,      ///< 時刻 t のスペクトル A（変位と高さ）
  kSpecB,      ///< 時刻 t のスペクトル B（傾きと変位の偏微分）
  kRowA,       ///< 行の逆 FFT の結果 A
  kRowB,       ///< 行の逆 FFT の結果 B
  kDisp,       ///< 最終: 変位（水面シェーダーが読む）
  kDeriv,      ///< 最終: 傾き（水面シェーダーが読む）
  kTexCount
};

Microsoft::WRL::ComPtr<ID3D12Resource> s_tex[kTexCount]; ///< テクスチャ
SRVManager::Handle s_uav[kTexCount];                     ///< 各テクスチャの UAV
SRVManager::Handle s_srvDisp;                            ///< 変位の SRV
SRVManager::Handle s_srvDeriv;                           ///< 傾きの SRV

RC::DynamicCB<FFTOceanCB> s_cb; ///< Compute の定数バッファ
FFTOceanSettings s_settings;    ///< 最後に渡された設定
FFTOceanSettings s_specSettings; ///< 今の初期スペクトルを作ったときの設定
bool s_initialized = false;     ///< InitFFTOcean 済みか
bool s_needInit = true;         ///< 初期スペクトルを作り直すか
bool s_requested = false;       ///< このフレームに SetFFTOcean が呼ばれたか
bool s_active = false;          ///< 直近の Update で計算したか

/// @brief 中間テクスチャの初期状態（ずっと UAV のまま使う）
constexpr D3D12_RESOURCE_STATES kUavState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
/// @brief 最終テクスチャの読み取り状態（VS と PS で読む）
constexpr D3D12_RESOURCE_STATES kReadState =
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

/// @brief UAV を許可した N×N のテクスチャを作る
/// @param device
/// @param state  初期状態
/// @param name   デバッグ名
/// @return 作ったリソース（失敗なら nullptr）
Microsoft::WRL::ComPtr<ID3D12Resource> CreateTexture(ID3D12Device *device, D3D12_RESOURCE_STATES state,
                                                     const wchar_t *name) {
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = kFFTOceanSize;
  desc.Height = kFFTOceanSize;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = kFormat;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;

  Microsoft::WRL::ComPtr<ID3D12Resource> res;
  if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                             IID_PPV_ARGS(&res)))) {
    return nullptr;
  }
  res->SetName(name);
  return res;
}

/// @brief 初期スペクトルを作り直す必要がある設定の違いか
/// @param a 設定
/// @param b 設定
/// @return 風・タイル・種・短波の打ち切りのどれかが違えば true
bool SpectrumChanged(const FFTOceanSettings &a, const FFTOceanSettings &b) {
  return a.tileSize != b.tileSize || a.windSpeed != b.windSpeed || a.windDirectionDeg != b.windDirectionDeg ||
         a.smallWaveCutoff != b.smallWaveCutoff || a.seed != b.seed;
}

/// @brief 全テクスチャに効く UAV バリア（直前のパスの書き込みを次のパスに見せる）
/// @param cl コマンドリスト
void UavBarrierAll(ID3D12GraphicsCommandList *cl) {
  D3D12_RESOURCE_BARRIER b = {};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  b.UAV.pResource = nullptr;
  cl->ResourceBarrier(1, &b);
}

/// @brief 最終テクスチャ 2 枚の状態を切り替える
/// @param cl     コマンドリスト
/// @param before 遷移前
/// @param after  遷移後
void TransitionOutputs(ID3D12GraphicsCommandList *cl, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER b[2] = {};
  for (int i = 0; i < 2; ++i) {
    b[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b[i].Transition.pResource = s_tex[i == 0 ? kDisp : kDeriv].Get();
    b[i].Transition.StateBefore = before;
    b[i].Transition.StateAfter = after;
    b[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  }
  cl->ResourceBarrier(2, b);
}

} // namespace

void InitFFTOcean() {
  if (s_initialized) return;
  auto &ctx = GetRenderContext();
  if (!ctx.IsInitialized() || !ctx.Ctx() || !ctx.Ctx()->core) return;
  ID3D12Device *device = ctx.Device();
  auto &srv = ctx.Ctx()->core->SRVMan();

  static const wchar_t *kNames[kTexCount] = {L"FFTOcean_H0",   L"FFTOcean_SpecA", L"FFTOcean_SpecB",
                                             L"FFTOcean_RowA", L"FFTOcean_RowB",  L"FFTOcean_Displacement",
                                             L"FFTOcean_Derivatives"};
  for (int i = 0; i < kTexCount; ++i) {
    const bool output = (i == kDisp || i == kDeriv);
    s_tex[i] = CreateTexture(device, output ? kReadState : kUavState, kNames[i]);
    if (!s_tex[i]) {
      Log::Print("[RenderFFTOcean] failed to create textures (FFT ocean disabled)");
      for (int j = 0; j < i; ++j) {
        srv.Free(s_uav[j]);
        s_uav[j] = SRVManager::Handle{};
        s_tex[j].Reset();
      }
      return;
    }
    s_uav[i] = srv.CreateTexture2DUAV(s_tex[i].Get(), kFormat);
  }
  s_srvDisp = srv.CreateTexture2D(s_tex[kDisp].Get(), kFormat, 1);
  s_srvDeriv = srv.CreateTexture2D(s_tex[kDeriv].Get(), kFormat, 1);

  s_needInit = true;
  s_active = false;
  s_initialized = true;
  Log::Print("[RenderFFTOcean] Initialized");
}

void TermFFTOcean() {
  if (!s_initialized) return;
  auto &ctx = GetRenderContext();
  if (ctx.Ctx() && ctx.Ctx()->core) {
    auto &srv = ctx.Ctx()->core->SRVMan();
    for (auto &h : s_uav) srv.Free(h);
    srv.Free(s_srvDisp);
    srv.Free(s_srvDeriv);
  }
  for (auto &h : s_uav) h = SRVManager::Handle{};
  s_srvDisp = SRVManager::Handle{};
  s_srvDeriv = SRVManager::Handle{};
  for (auto &t : s_tex) t.Reset();
  s_initialized = false;
  s_active = false;
  s_requested = false;
  Log::Print("[RenderFFTOcean] Terminated");
}

void SetFFTOcean(const FFTOceanSettings &settings) {
  s_settings = settings;
  s_settings.tileSize = (std::max)(settings.tileSize, 1.0f);
  s_settings.windSpeed = (std::max)(settings.windSpeed, 0.1f);
  s_settings.smallWaveCutoff = (std::max)(settings.smallWaveCutoff, 0.0f);
  s_settings.heightScale = (std::max)(settings.heightScale, 0.0f);
  s_settings.choppiness = (std::max)(settings.choppiness, 0.0f);
  if (SpectrumChanged(s_settings, s_specSettings)) s_needInit = true;
  s_requested = true;
}

void UpdateFFTOcean(float timeSec) {
  const bool run = s_initialized && s_requested && s_settings.enabled && s_settings.heightScale > 0.0f;
  s_requested = false;
  s_active = run;
  if (!run) return;

  auto &ctx = GetRenderContext();

  FFTOceanCB &cb = s_cb.Value();
  const float rad = s_settings.windDirectionDeg * 3.14159265f / 180.0f;
  cb.tileSize = s_settings.tileSize;
  cb.time = timeSec;
  cb.windSpeed = s_settings.windSpeed;
  cb.gravity = kGravity;
  cb.windDirX = std::cos(rad);
  cb.windDirZ = std::sin(rad);
  cb.smallCutoff = s_settings.smallWaveCutoff;
  cb.seed = s_settings.seed;

  const bool doInit = s_needInit;
  if (doInit) {
    s_specSettings = s_settings;
    s_needInit = false;
  }

  ctx.PushCommand3D(0, [doInit](ID3D12GraphicsCommandList *cl) {
    auto &rc = GetRenderContext();
    if (!cl || !rc.Ctx() || !rc.Ctx()->pipelineManager) return;
    auto *pm = rc.Ctx()->pipelineManager;

    // すべてのパスで同じルートシグネチャ（b0 + u0〜u3）を使い、PSO だけ替える
    auto bind = [&](const char *key, Tex u0, Tex u1, Tex u2, Tex u3) -> bool {
      ID3D12PipelineState *pso = pm->GetComputePSO(key);
      ID3D12RootSignature *root = pm->GetComputeRoot(key);
      if (!pso || !root) return false;
      cl->SetPipelineState(pso);
      cl->SetComputeRootSignature(root);
      cl->SetComputeRootConstantBufferView(0, s_cb.Address());
      cl->SetComputeRootDescriptorTable(1, s_uav[u0].gpu);
      cl->SetComputeRootDescriptorTable(2, s_uav[u1].gpu);
      cl->SetComputeRootDescriptorTable(3, s_uav[u2].gpu);
      cl->SetComputeRootDescriptorTable(4, s_uav[u3].gpu);
      return true;
    };
    constexpr UINT kGroups8 = kFFTOceanSize / 8;

    // 1. 初期スペクトル（設定が変わったときだけ）
    if (doInit && bind("fft_ocean_init", kH0, kSpecA, kSpecB, kRowA)) {
      cl->Dispatch(kGroups8, kGroups8, 1);
      UavBarrierAll(cl);
    }
    // 2. 時刻 t のスペクトル
    if (!bind("fft_ocean_spectrum", kH0, kSpecA, kSpecB, kRowA)) return;
    cl->Dispatch(kGroups8, kGroups8, 1);
    UavBarrierAll(cl);
    // 3. 行の逆 FFT
    if (!bind("fft_ocean_row", kSpecA, kSpecB, kRowA, kRowB)) return;
    cl->Dispatch(kFFTOceanSize, 1, 1);
    UavBarrierAll(cl);
    // 4. 列の逆 FFT（最終テクスチャへ書く）
    TransitionOutputs(cl, kReadState, kUavState);
    if (bind("fft_ocean_column", kRowA, kRowB, kDisp, kDeriv)) {
      cl->Dispatch(kFFTOceanSize, 1, 1);
      UavBarrierAll(cl);
    }
    TransitionOutputs(cl, kUavState, kReadState);
  }, "FFTOcean");
}

bool IsFFTOceanActive() { return s_active; }

const FFTOceanSettings &GetFFTOceanSettings() { return s_settings; }

float GetFFTOceanAmplitude() {
  if (!s_settings.enabled) return 0.0f;
  // 分散 σ² = ∫ α/(2k³) exp(-1/(k Lw)²) dk をタイルで表せる波数 [2π/L, πN/L] の範囲で積分したもの
  //         = α Lw² / 4 · (exp(-1/(kmax Lw)²) - exp(-1/(kmin Lw)²))
  const float lw = s_settings.windSpeed * s_settings.windSpeed / kGravity;
  const float kmin = 2.0f * 3.14159265f / s_settings.tileSize;
  const float kmax = 3.14159265f * static_cast<float>(kFFTOceanSize) / s_settings.tileSize;
  const float a = std::exp(-1.0f / (kmax * kmax * lw * lw));
  const float b = std::exp(-1.0f / (kmin * kmin * lw * lw));
  const float variance = kPhillipsAlpha * lw * lw * 0.25f * (std::max)(a - b, 0.0f);
  return 2.0f * std::sqrt(variance) * s_settings.heightScale;
}

D3D12_GPU_DESCRIPTOR_HANDLE GetFFTOceanDisplacementMap() {
  return s_initialized ? s_srvDisp.gpu : D3D12_GPU_DESCRIPTOR_HANDLE{};
}

D3D12_GPU_DESCRIPTOR_HANDLE GetFFTOceanDerivativeMap() {
  return s_initialized ? s_srvDeriv.gpu : D3D12_GPU_DESCRIPTOR_HANDLE{};
}

} // namespace RC
