#include "ProceduralSky.h"
#include "Render/RenderContext.h"
#include "Render/RenderCommon.h"
#include "Graphics/Light/Directional/DirectionalLightSource.h"
#include "Dx12/PipelineManager.h"
#include "Dx12/SRVManager/SRVManager.h"
#include "Dx12/Dx12Core.h"
#include "Common/Log/Log.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace RC {

namespace {

/// @brief ProceduralSky.CS.hlsl の cbuffer ProceduralSkyParams と同じ並び
struct ProceduralSkyCB {
  Vector3 zenith{};           ///< 天頂の色
  float gradientExponent = 0; ///< グラデーションの曲がり
  Vector3 horizon{};          ///< 地平線の色
  float horizonHaze = 0;      ///< 地平線のかすみ
  Vector3 ground{};           ///< 地平線より下の色
  float sunCosOuter = 0;      ///< 太陽の縁の外側の cos
  Vector3 sunDir{};           ///< 太陽へ向かう向き（正規化済み）
  float sunCosInner = 0;      ///< 太陽の縁の内側の cos
  Vector3 sunColor{};         ///< 太陽の色
  float sunIntensity = 0;     ///< 太陽本体の明るさ
  Vector3 cloudColor{};       ///< 雲の日なた側の色
  float cloudCoverage = 0;    ///< 雲の量
  Vector3 cloudShadow{};      ///< 雲の影側の色
  float cloudScale = 0;       ///< 雲の模様の細かさ
  float cloudOffsetX = 0;     ///< 雲の流れた量 X
  float cloudOffsetZ = 0;     ///< 雲の流れた量 Z
  float cloudOpacity = 0;     ///< 雲の不透明度
  uint32_t faceSize = 0;      ///< 1 面の一辺（テクセル）
  float sunGlow = 0;          ///< 太陽のまわりの光の強さ
  float sunGlowPower = 0;     ///< 太陽のまわりの光の鋭さ
  float cloudSoftness = 0;    ///< 雲の縁のぼかし幅
  float pad = 0;              ///< 16 バイト境界合わせ
};
static_assert(sizeof(ProceduralSkyCB) == 144, "ProceduralSkyCB layout must match ProceduralSky.CS.hlsl");

constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_R16G16B16A16_FLOAT; ///< キューブマップの形式（太陽を 1 より明るく持てるよう float）
constexpr float kPi = 3.14159265f;

/// @brief 描画で読むときの状態（水面の VS でも読む可能性があるので両方）
constexpr D3D12_RESOURCE_STATES kReadState =
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

Microsoft::WRL::ComPtr<ID3D12Resource> s_tex; ///< キューブマップ（6 枚の 2D 配列）
SRVManager::Handle s_uav;                     ///< 書き込み用 UAV（Texture2DArray）
SRVManager::Handle s_srv;                     ///< 読み取り用 SRV（TextureCube）
RC::DynamicCB<ProceduralSkyCB> s_cb;          ///< Compute の定数バッファ
ProceduralSkySettings s_settings;             ///< 最後に渡された設定
ProceduralSkyCB s_lastCB{};                   ///< 最後にキューブマップを描いたときの定数
bool s_hasContent = false;                    ///< 一度でも描いたか
bool s_initialized = false;                   ///< InitProceduralSky 済みか

/// @brief 色の各成分を 0 以上にする
/// @param v 色
/// @return 負の成分を 0 にした色
Vector3 NonNegative(const Vector3 &v) {
  return {(std::max)(v.x, 0.0f), (std::max)(v.y, 0.0f), (std::max)(v.z, 0.0f)};
}

/// @brief 正規化する（長さが 0 に近ければ真上を返す）
/// @param v ベクトル
/// @return 正規化したベクトル
Vector3 SafeNormalize(const Vector3 &v) {
  const float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
  if (len < 1e-5f) return {0.0f, 1.0f, 0.0f};
  return {v.x / len, v.y / len, v.z / len};
}

/// @brief 今の設定・光源・時刻から定数バッファの値を作る
/// @return 定数バッファの値
ProceduralSkyCB BuildCB() {
  const ProceduralSkySettings &s = s_settings;
  ProceduralSkyCB cb{};
  cb.zenith = NonNegative(s.zenithColor);
  cb.gradientExponent = (std::max)(s.gradientExponent, 0.01f);
  cb.horizon = NonNegative(s.horizonColor);
  cb.horizonHaze = std::clamp(s.horizonHaze, 0.0f, 1.0f);
  cb.ground = NonNegative(s.groundColor);

  // 太陽の向き。平行光源の direction は「光が進む向き」なので、太陽はその逆側にある
  Vector3 sunDir = s.sunDirection;
  if (s.sunFollowsLight) {
    if (const DirectionalLightSource *light = GetRenderContext().DirLights().GetActive()) {
      const Vector3 &d = light->Data().direction;
      sunDir = {-d.x, -d.y, -d.z};
    }
  }
  cb.sunDir = SafeNormalize(sunDir);

  const float sunRad = std::clamp(s.sunSizeDeg, 0.05f, 30.0f) * kPi / 180.0f;
  cb.sunCosOuter = std::cos(sunRad);
  cb.sunCosInner = std::cos(sunRad * 0.75f); // 縁の 25% をぼかす
  cb.sunColor = NonNegative(s.sunColor);
  cb.sunIntensity = (std::max)(s.sunIntensity, 0.0f);
  cb.sunGlow = (std::max)(s.sunGlow, 0.0f);
  cb.sunGlowPower = (std::max)(s.sunGlowPower, 1.0f);

  cb.cloudColor = NonNegative(s.cloudColor);
  cb.cloudShadow = NonNegative(s.cloudShadowColor);
  cb.cloudCoverage = std::clamp(s.cloudCoverage, 0.0f, 1.0f);
  cb.cloudSoftness = (std::max)(s.cloudSoftness, 0.01f);
  cb.cloudScale = (std::max)(s.cloudScale, 0.01f);
  cb.cloudOpacity = std::clamp(s.cloudOpacity, 0.0f, 1.0f);

  // 雲の流れ。速さ 0 なら位置は 0 のまま（時刻が進んでも再計算しない）
  if (s.cloudSpeed != 0.0f) {
    const float rad = s.cloudDirectionDeg * kPi / 180.0f;
    const float dist = s.cloudSpeed * GetWaterTime();
    cb.cloudOffsetX = std::cos(rad) * dist;
    cb.cloudOffsetZ = std::sin(rad) * dist;
  }
  cb.faceSize = static_cast<uint32_t>(kProceduralSkySize);
  return cb;
}

/// @brief キューブマップの状態を切り替える
/// @param cl     コマンドリスト
/// @param before 遷移前
/// @param after  遷移後
void Transition(ID3D12GraphicsCommandList *cl, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER b = {};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = s_tex.Get();
  b.Transition.StateBefore = before;
  b.Transition.StateAfter = after;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  cl->ResourceBarrier(1, &b);
}

} // namespace

void InitProceduralSky() {
  if (s_initialized) return;
  auto &ctx = GetRenderContext();
  if (!ctx.IsInitialized() || !ctx.Ctx() || !ctx.Ctx()->core) return;
  ID3D12Device *device = ctx.Device();
  auto &srv = ctx.Ctx()->core->SRVMan();

  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = kProceduralSkySize;
  desc.Height = kProceduralSkySize;
  desc.DepthOrArraySize = 6;
  desc.MipLevels = 1;
  desc.Format = kFormat;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;

  if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, kReadState, nullptr,
                                             IID_PPV_ARGS(&s_tex)))) {
    Log::Print("[ProceduralSky] failed to create cubemap (procedural sky disabled)");
    s_tex.Reset();
    return;
  }
  s_tex->SetName(L"ProceduralSky_Cubemap");

  s_uav = srv.CreateTexture2DArrayUAV(s_tex.Get(), kFormat, 6);
  s_srv = srv.CreateTextureCube(s_tex.Get(), kFormat, 1);

  s_hasContent = false;
  s_initialized = true;
  Log::Print("[ProceduralSky] Initialized");
}

void TermProceduralSky() {
  if (!s_initialized) return;
  auto &ctx = GetRenderContext();
  if (ctx.Ctx() && ctx.Ctx()->core) {
    auto &srv = ctx.Ctx()->core->SRVMan();
    srv.Free(s_uav);
    srv.Free(s_srv);
  }
  s_uav = SRVManager::Handle{};
  s_srv = SRVManager::Handle{};
  s_tex.Reset();
  s_hasContent = false;
  s_initialized = false;
  Log::Print("[ProceduralSky] Terminated");
}

bool IsProceduralSkyReady() { return s_initialized; }

void SetProceduralSky(const ProceduralSkySettings &settings) { s_settings = settings; }

const ProceduralSkySettings &GetProceduralSkySettings() { return s_settings; }

void UpdateProceduralSky() {
  if (!s_initialized) return;

  const ProceduralSkyCB cb = BuildCB();
  // 前回描いたときと全く同じなら描き直さない（雲が止まっていれば最初の 1 回だけになる）
  if (s_hasContent && std::memcmp(&cb, &s_lastCB, sizeof(cb)) == 0) return;
  s_lastCB = cb;
  s_hasContent = true;
  s_cb.Value() = cb;

  auto &ctx = GetRenderContext();
  ctx.PushCommand3D(0, [](ID3D12GraphicsCommandList *cl) {
    auto &rc = GetRenderContext();
    if (!cl || !rc.Ctx() || !rc.Ctx()->pipelineManager) return;
    auto *pm = rc.Ctx()->pipelineManager;
    ID3D12PipelineState *pso = pm->GetComputePSO("procedural_sky_cs");
    ID3D12RootSignature *root = pm->GetComputeRoot("procedural_sky_cs");
    if (!pso || !root) return;

    Transition(cl, kReadState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cl->SetPipelineState(pso);
    cl->SetComputeRootSignature(root);
    cl->SetComputeRootConstantBufferView(0, s_cb.Address());
    cl->SetComputeRootDescriptorTable(1, s_uav.gpu);
    constexpr UINT kGroups = kProceduralSkySize / 8; // numthreads(8, 8, 1)
    cl->Dispatch(kGroups, kGroups, 6);
    Transition(cl, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kReadState);
  }, "ProceduralSky");
}

D3D12_GPU_DESCRIPTOR_HANDLE GetProceduralSkySrv() {
  return s_initialized ? s_srv.gpu : D3D12_GPU_DESCRIPTOR_HANDLE{};
}

} // namespace RC
