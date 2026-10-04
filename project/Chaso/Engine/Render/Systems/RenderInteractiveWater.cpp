#include "RenderInteractiveWater.h"
#include "RenderContext.h"
#include "RenderCommon.h"
#include "Dx12/PipelineManager.h"
#include "Dx12/SRVManager/SRVManager.h"
#include "Dx12/Dx12Core.h" // GetSRVManager 等が取れると仮定
#include "Common/SceneContext.h"
#include "Common/Log/Log.h"
#include "Common/function/function.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace RC {

/// @brief WaveSimulation.CS.hlsl の SimulationParams と同じ並び（16 バイト境界を崩さないこと）
struct WaveSimCB {
  float alpha;
  float damping;
  int sourceCount;
  float padding; // リセットフラグ
  Vector4 sources[64];
  // ---- 引き波の泡（G チャンネル）----
  float foamDecay;
  float foamSpread;
  int foamSourceCount;
  float crestDecay; // 波頭（B チャンネル）の残存率
  Vector4 foamSources[64];
  // ---- 波頭の白波（B チャンネル）----
  int crestSourceCount;
  float crestPad[3];
  Vector4 crestSources[128];
};
static_assert(sizeof(WaveSimCB) == 16 + 16 * 64 + 16 + 16 * 64 + 16 + 16 * 128,
              "WaveSimCB layout must match WaveSimulation.CS.hlsl");

static constexpr int TEX_SIZE = 256;
/// @brief ハイトマップの形式。R = 高さ / G = 引き波の泡 / B = 波頭の白波 / A = 未使用
/// @details 3 チャンネルの UAV 形式は無いので 4 チャンネル
static constexpr DXGI_FORMAT kHeightMapFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;
static constexpr int kHeightMapChannels = 4;
static constexpr int kMaxFoamSources = 64;
static constexpr int kMaxCrestSources = 128;
static Microsoft::WRL::ComPtr<ID3D12Resource> s_heightMaps[3];
static SRVManager::Handle s_srvs[3];
static SRVManager::Handle s_uavs[3];
static int s_currIdx = 0; // 最新のハイトマップのインデックス

static Microsoft::WRL::ComPtr<ID3D12Resource> s_simCB;
static WaveSimCB* s_simCBMapped = nullptr;

static std::vector<WaveSource> s_pendingSources;
static std::vector<WaveSource> s_pendingFoamSources;  ///< 引き波の泡の波源（strength = 足す量）
static std::vector<WaveSource> s_pendingCrestSources; ///< 波頭の白波の波源（strength = 足す量）
static float s_foamDecay = 0.992f; ///< 引き波の泡の毎フレーム残存率
static float s_foamSpread = 0.10f; ///< 引き波の泡の毎フレームの滲み
static float s_crestDecay = 0.80f; ///< 波頭の白波の毎フレーム残存率
static bool s_initialized = false;
static int s_resetFrames = 3; // 最初の3フレームはテクスチャを0クリアする

// ---- CPU 読み戻し（任意） ----
// GPU が書き終えたハイトマップを READBACK ヒープへコピーし、フェンスが通ったものだけ CPU 配列へ写す。
// スロットはリングで回す。同じスロットへ再コピーするのは kReadbackCount フレーム後で、
// CommandContext::BeginFrame が (frameCount) フレーム前の完了を待つので kReadbackCount >= kMaxFrames なら
// 「GPU がまだ読み書き中のバッファ」を CPU が触ることはない。
static constexpr int kReadbackCount = 4; // >= CommandContext::kMaxFrames (3)
struct ReadbackSlot {
  Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
  uint64_t fenceValue = 0; ///< このスロットへのコピーを含むコマンドが完了するフェンス値（0 = 未使用）
};
static ReadbackSlot s_readback[kReadbackCount];
static int s_readbackWriteIdx = 0;
static D3D12_PLACED_SUBRESOURCE_FOOTPRINT s_readbackFootprint{};
static UINT64 s_readbackTotalBytes = 0;
static bool s_readbackEnabled = false;
static bool s_readbackHasData = false;   ///< s_cpuHeight に有効なデータが入っているか
static uint64_t s_readbackLastFence = 0; ///< 最後に CPU へ写したスロットのフェンス値
static std::vector<float> s_cpuHeight;   ///< TEX_SIZE*TEX_SIZE、行優先（y*TEX_SIZE+x）

// ヘルパー: D3D12リソース作成
static Microsoft::WRL::ComPtr<ID3D12Resource> CreateUAVTexture2D(ID3D12Device* device, int width, int height, DXGI_FORMAT format) {
  Microsoft::WRL::ComPtr<ID3D12Resource> res;
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Alignment = 0;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.SampleDesc.Quality = 0;
  desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

  D3D12_HEAP_PROPERTIES heapProps = {};
  heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
  heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
  heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
  heapProps.CreationNodeMask = 1;
  heapProps.VisibleNodeMask = 1;

  HRESULT hr = device->CreateCommittedResource(
      &heapProps,
      D3D12_HEAP_FLAG_NONE,
      &desc,
      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
      nullptr,
      IID_PPV_ARGS(&res));
  if (FAILED(hr)) return nullptr;
  return res;
}

// CreateBufferResource is already defined in function.h

// ---------------------------------------------------------------------------
// CPU 読み戻し
// ---------------------------------------------------------------------------

/// @brief READBACK バッファを（初回だけ）作る
static bool EnsureReadbackBuffers() {
  if (s_readback[0].buffer) return true;
  auto& ctx = GetRenderContext();
  if (!ctx.IsInitialized() || !s_heightMaps[0]) return false;
  ID3D12Device* device = ctx.Device();

  const D3D12_RESOURCE_DESC texDesc = s_heightMaps[0]->GetDesc();
  UINT numRows = 0;
  UINT64 rowSize = 0;
  device->GetCopyableFootprints(&texDesc, 0, 1, 0, &s_readbackFootprint, &numRows, &rowSize, &s_readbackTotalBytes);

  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = D3D12_HEAP_TYPE_READBACK;
  heap.CreationNodeMask = 1;
  heap.VisibleNodeMask = 1;

  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = s_readbackTotalBytes;
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = DXGI_FORMAT_UNKNOWN;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

  for (int i = 0; i < kReadbackCount; ++i) {
    HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 IID_PPV_ARGS(&s_readback[i].buffer));
    if (FAILED(hr)) {
      for (int j = 0; j <= i; ++j) s_readback[j].buffer.Reset();
      Log::Print("[RenderInteractiveWater] failed to create readback buffer (readback disabled)");
      s_readbackEnabled = false; // 毎フレーム作り直しに行かない
      return false;
    }
    s_readback[i].buffer->SetName(L"InteractiveWater_Readback");
    s_readback[i].fenceValue = 0;
  }
  s_cpuHeight.assign(static_cast<size_t>(TEX_SIZE) * TEX_SIZE, 0.0f);
  return true;
}

/// @brief フェンスが通った中でいちばん新しいスロットを CPU 配列へ写す（GPU 待ちはしない）
static void ConsumeReadback() {
  if (!s_readbackEnabled || !s_readback[0].buffer) return;
  auto& ctx = GetRenderContext();
  if (!ctx.Ctx() || !ctx.Ctx()->core) return;
  const uint64_t completed = ctx.Ctx()->core->GetCompletedFenceValue();

  int best = -1;
  uint64_t bestFence = s_readbackLastFence;
  for (int i = 0; i < kReadbackCount; ++i) {
    const uint64_t f = s_readback[i].fenceValue;
    if (f != 0 && f <= completed && f > bestFence) {
      best = i;
      bestFence = f;
    }
  }
  if (best < 0) return;

  const D3D12_RANGE readRange{0, static_cast<SIZE_T>(s_readbackTotalBytes)};
  void* mapped = nullptr;
  if (SUCCEEDED(s_readback[best].buffer->Map(0, &readRange, &mapped)) && mapped) {
    const UINT rowPitch = s_readbackFootprint.Footprint.RowPitch;
    const auto* src = static_cast<const uint8_t*>(mapped);
    // テクセルは (高さ, 引き波, 波頭, 未使用) の float4。CPU 側は高さ（R）だけを使う
    for (int y = 0; y < TEX_SIZE; ++y) {
      const auto* row = reinterpret_cast<const float*>(src + static_cast<size_t>(y) * rowPitch);
      float* dst = &s_cpuHeight[static_cast<size_t>(y) * TEX_SIZE];
      for (int x = 0; x < TEX_SIZE; ++x) dst[x] = row[x * kHeightMapChannels];
    }
    const D3D12_RANGE noWrite{0, 0};
    s_readback[best].buffer->Unmap(0, &noWrite);
    s_readbackHasData = true;
    s_readbackLastFence = bestFence;
  }
  // 消費済み／古いスロットは捨てる（同じデータを二度読まない）
  for (int i = 0; i < kReadbackCount; ++i) {
    if (s_readback[i].fenceValue != 0 && s_readback[i].fenceValue <= bestFence) s_readback[i].fenceValue = 0;
  }
}

/// @brief CPU 配列を Water.VS.hlsl の gSamplerClamp（linear, clamp）と同じ規則で読む
/// @param u,v UV（0〜1。範囲外はクランプ）
static float SampleCpuHeightMap(float u, float v) {
  // テクセル中心は (i + 0.5) / N
  const float fx = std::clamp(u * TEX_SIZE - 0.5f, 0.0f, static_cast<float>(TEX_SIZE - 1));
  const float fy = std::clamp(v * TEX_SIZE - 0.5f, 0.0f, static_cast<float>(TEX_SIZE - 1));
  const int x0 = static_cast<int>(fx);
  const int y0 = static_cast<int>(fy);
  const int x1 = (std::min)(x0 + 1, TEX_SIZE - 1);
  const int y1 = (std::min)(y0 + 1, TEX_SIZE - 1);
  const float tx = fx - static_cast<float>(x0);
  const float ty = fy - static_cast<float>(y0);
  const float* m = s_cpuHeight.data();
  const float h00 = m[y0 * TEX_SIZE + x0];
  const float h10 = m[y0 * TEX_SIZE + x1];
  const float h01 = m[y1 * TEX_SIZE + x0];
  const float h11 = m[y1 * TEX_SIZE + x1];
  const float top = h00 + (h10 - h00) * tx;
  const float bottom = h01 + (h11 - h01) * tx;
  return top + (bottom - top) * ty;
}

void SetInteractiveWaterReadback(bool enabled) {
  if (enabled == s_readbackEnabled) return;
  if (enabled) {
    // まだ Init 前ならバッファは UpdateInteractiveWater 側で作る（要求だけ覚えておく）
    s_readbackEnabled = true;
    if (s_initialized) EnsureReadbackBuffers();
    Log::Print("[RenderInteractiveWater] readback enabled");
  } else {
    s_readbackEnabled = false;
    s_readbackHasData = false;
    s_readbackLastFence = 0;
    // GPU がまだコピー中のスロットがあっても、fenceValue を 0 にすれば以後 Map しない。
    // バッファ自体は Term まで持つ（再有効化で作り直さない）。
    for (auto& s : s_readback) s.fenceValue = 0;
    if (!s_cpuHeight.empty()) std::fill(s_cpuHeight.begin(), s_cpuHeight.end(), 0.0f);
    Log::Print("[RenderInteractiveWater] readback disabled");
  }
}

bool IsInteractiveWaterReadbackEnabled() { return s_readbackEnabled; }

float SampleInteractiveWaterHeight(float worldX, float worldZ) {
  if (!s_readbackEnabled || !s_readbackHasData) return 0.0f;
  const float u = worldX / kInteractiveWaterWorldSize + 0.5f;
  const float v = worldZ / kInteractiveWaterWorldSize + 0.5f;
  return SampleCpuHeightMap(u, v);
}

bool SampleInteractiveWater(float worldX, float worldZ, float& outHeight, Vector3& outNormal) {
  outHeight = 0.0f;
  outNormal = {0.0f, 1.0f, 0.0f};
  if (!s_readbackEnabled || !s_readbackHasData) return false;
  const float u = worldX / kInteractiveWaterWorldSize + 0.5f;
  const float v = worldZ / kInteractiveWaterWorldSize + 0.5f;
  outHeight = SampleCpuHeightMap(u, v);

  // Water.VS.hlsl の interactiveNormal と同じ有限差分：
  //   dX = (200*texel, hR-hL, 0), dZ = (0, hD-hU, 200*texel), N = normalize(cross(dZ, dX))
  //   = normalize(-(hR-hL), 200*texel, -(hD-hU))
  constexpr float texel = 1.0f / static_cast<float>(TEX_SIZE);
  const float hL = SampleCpuHeightMap(u - texel, v);
  const float hR = SampleCpuHeightMap(u + texel, v);
  const float hU = SampleCpuHeightMap(u, v - texel);
  const float hD = SampleCpuHeightMap(u, v + texel);
  const float nx = -(hR - hL);
  const float ny = 2.0f * kInteractiveWaterWorldSize * texel;
  const float nz = -(hD - hU);
  const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
  if (len > 1e-6f) outNormal = {nx / len, ny / len, nz / len};
  return true;
}

void InitInteractiveWater() {
  if (s_initialized) return;
  auto& ctx = GetRenderContext();
  if (!ctx.IsInitialized()) return;
  auto device = ctx.Device();

  // テクスチャリソースの作成
  for (int i = 0; i < 3; ++i) {
    s_heightMaps[i] = CreateUAVTexture2D(device, TEX_SIZE, TEX_SIZE, kHeightMapFormat);
    s_srvs[i] = ctx.Ctx()->core->SRVMan().CreateTexture2D(s_heightMaps[i].Get(), kHeightMapFormat, 1);
    s_uavs[i] = ctx.Ctx()->core->SRVMan().CreateTexture2DUAV(s_heightMaps[i].Get(), kHeightMapFormat);
  }

  // 定数バッファの作成
  s_simCB = CreateBufferResource(device, sizeof(WaveSimCB), L"WaveSimCB");
  s_simCB->Map(0, nullptr, reinterpret_cast<void**>(&s_simCBMapped));
  if (s_simCBMapped) {
    s_simCBMapped->alpha = 0.45f;
    s_simCBMapped->damping = 0.985f;
    s_simCBMapped->sourceCount = 0;
    s_simCBMapped->foamDecay = s_foamDecay;
    s_simCBMapped->foamSpread = s_foamSpread;
    s_simCBMapped->foamSourceCount = 0;
    s_simCBMapped->crestDecay = s_crestDecay;
    s_simCBMapped->crestSourceCount = 0;
  }

  s_currIdx = 0;
  s_resetFrames = 3; // 初期化時にリセットカウンタを設定
  s_initialized = true;
  Log::Print("[RenderInteractiveWater] Initialized");
}

void TermInteractiveWater() {
  if (!s_initialized) return;

  auto& ctx = GetRenderContext();
  for (int i = 0; i < 3; ++i) {
    if (ctx.Ctx() && ctx.Ctx()->core) {
      ctx.Ctx()->core->SRVMan().Free(s_srvs[i]);
      ctx.Ctx()->core->SRVMan().Free(s_uavs[i]);
    }
    s_heightMaps[i].Reset();
  }

  if (s_simCB) {
    s_simCB->Unmap(0, nullptr);
    s_simCBMapped = nullptr;
    s_simCB.Reset();
  }

  // 読み戻し（RC::Term は GPU 完了待ちのあとに呼ばれる前提。ハイトマップの Reset と同じ）
  s_readbackEnabled = false;
  s_readbackHasData = false;
  s_readbackLastFence = 0;
  s_readbackWriteIdx = 0;
  for (auto& s : s_readback) {
    s.buffer.Reset();
    s.fenceValue = 0;
  }
  s_cpuHeight.clear();
  s_cpuHeight.shrink_to_fit();

  s_initialized = false;
  Log::Print("[RenderInteractiveWater] Terminated");
}

void AddWaveSource(const WaveSource& source) {
  if (s_pendingSources.size() < 64) {
    s_pendingSources.push_back(source);
  }
}

bool AddWaveSourceAtWorld(float worldX, float worldZ, float radius, float strength) {
  const float u = worldX / kInteractiveWaterWorldSize + 0.5f;
  const float v = worldZ / kInteractiveWaterWorldSize + 0.5f;
  if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) return false;
  if (s_pendingSources.size() >= 64) return false;
  WaveSource s;
  s.uv = {u, v};
  s.radius = radius;
  s.strength = strength;
  s_pendingSources.push_back(s);
  return true;
}

/// @brief 泡系の波源をリストへ積む（範囲外・上限超えは捨てる）
static bool PushFoamLike(std::vector<WaveSource>& list, size_t maxCount, float worldX, float worldZ, float radius,
                         float amount) {
  if (amount <= 0.0f) return false;
  const float u = worldX / kInteractiveWaterWorldSize + 0.5f;
  const float v = worldZ / kInteractiveWaterWorldSize + 0.5f;
  if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) return false;
  if (list.size() >= maxCount) return false;
  WaveSource s;
  s.uv = {u, v};
  s.radius = radius;
  s.strength = amount;
  list.push_back(s);
  return true;
}

bool AddFoamSourceAtWorld(float worldX, float worldZ, float radius, float amount) {
  return PushFoamLike(s_pendingFoamSources, kMaxFoamSources, worldX, worldZ, radius, amount);
}

bool AddCrestFoamSourceAtWorld(float worldX, float worldZ, float radius, float amount) {
  return PushFoamLike(s_pendingCrestSources, kMaxCrestSources, worldX, worldZ, radius, amount);
}

void SetInteractiveFoamParams(float decayPerFrame, float spreadPerFrame, float crestDecayPerFrame) {
  s_foamDecay = std::clamp(decayPerFrame, 0.0f, 1.0f);
  s_foamSpread = std::clamp(spreadPerFrame, 0.0f, 0.25f);
  s_crestDecay = std::clamp(crestDecayPerFrame, 0.0f, 0.99f);
}

void GetInteractiveFoamParams(float& outDecayPerFrame, float& outSpreadPerFrame, float& outCrestDecayPerFrame) {
  outDecayPerFrame = s_foamDecay;
  outSpreadPerFrame = s_foamSpread;
  outCrestDecayPerFrame = s_crestDecay;
}

void UpdateInteractiveWater() {
  if (!s_initialized) return;
  auto& ctx = GetRenderContext();

  // インデックスのローテーション
  // s_currIdx は前フレームでレンダリングした「最新(h1)」のテクスチャ
  int h1Idx = s_currIdx;
  // 2フレーム前の「古い(h2)」テクスチャ
  int h2Idx = (s_currIdx + 2) % 3;
  // 今回レンダリングする「次(out)」のテクスチャ
  int nextIdx = (s_currIdx + 1) % 3;

  // CBの更新
  if (s_simCBMapped) {
    s_simCBMapped->alpha = 0.15f;   // 波の伝播速度（穏やかな広がり）
    s_simCBMapped->damping = 0.95f; // 速度減衰（0.95^60≈0.05、約1秒でほぼ消滅）
    
    // リセット処理
    if (s_resetFrames > 0) {
        s_simCBMapped->padding = 1.0f;
        s_resetFrames--;
    } else {
        s_simCBMapped->padding = 0.0f;
    }

    int count = std::min(64, (int)s_pendingSources.size());
    s_simCBMapped->sourceCount = count;
    for (int i = 0; i < count; ++i) {
      s_simCBMapped->sources[i] = Vector4(
          s_pendingSources[i].uv.x,
          s_pendingSources[i].uv.y,
          s_pendingSources[i].radius,
          s_pendingSources[i].strength);
    }

    // 引き波の泡
    s_simCBMapped->foamDecay = s_foamDecay;
    s_simCBMapped->foamSpread = s_foamSpread;
    const int foamCount = std::min(kMaxFoamSources, (int)s_pendingFoamSources.size());
    s_simCBMapped->foamSourceCount = foamCount;
    for (int i = 0; i < foamCount; ++i) {
      const WaveSource& f = s_pendingFoamSources[i];
      s_simCBMapped->foamSources[i] = Vector4(f.uv.x, f.uv.y, f.radius, f.strength);
    }
    // 波頭の白波
    s_simCBMapped->crestDecay = s_crestDecay;
    const int crestCount = std::min(kMaxCrestSources, (int)s_pendingCrestSources.size());
    s_simCBMapped->crestSourceCount = crestCount;
    for (int i = 0; i < crestCount; ++i) {
      const WaveSource& c = s_pendingCrestSources[i];
      s_simCBMapped->crestSources[i] = Vector4(c.uv.x, c.uv.y, c.radius, c.strength);
    }
  }
  s_pendingSources.clear();
  s_pendingFoamSources.clear();
  s_pendingCrestSources.clear();

  // 前のフレームまでにコピーが終わっているハイトマップを CPU へ写す（読み戻しが有効なときだけ）
  if (s_readbackEnabled && !s_readback[0].buffer) EnsureReadbackBuffers();
  ConsumeReadback();
  const bool doReadback = s_readbackEnabled && s_readback[0].buffer;
  const int readbackSlot = s_readbackWriteIdx;
  if (doReadback) s_readbackWriteIdx = (s_readbackWriteIdx + 1) % kReadbackCount;

  // リソースバリアから先のGPUコマンドをキューイング (SortKey=0で最初に処理させる)
  ctx.PushCommand3D(0, [h1Idx, h2Idx, nextIdx, doReadback, readbackSlot](ID3D12GraphicsCommandList* cl) {
    auto& renderCtx = GetRenderContext();
    if (!cl) return;

    // リソースバリアの設定 (nextIdx を UAV に)
    D3D12_RESOURCE_BARRIER barrierToUAV = {};
    barrierToUAV.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrierToUAV.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrierToUAV.Transition.pResource = s_heightMaps[nextIdx].Get();
    barrierToUAV.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrierToUAV.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barrierToUAV.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cl->ResourceBarrier(1, &barrierToUAV);

    // Compute Shaderのディスパッチ
    ID3D12PipelineState* pso = nullptr;
    ID3D12RootSignature* root = nullptr;
    if (renderCtx.Ctx() && renderCtx.Ctx()->pipelineManager) {
      pso = renderCtx.Ctx()->pipelineManager->GetComputePSO("wave_simulation");
      root = renderCtx.Ctx()->pipelineManager->GetComputeRoot("wave_simulation");
    }

    if (pso && root) {
      cl->SetPipelineState(pso);
      cl->SetComputeRootSignature(root);

      // b0
      cl->SetComputeRootConstantBufferView(0, s_simCB->GetGPUVirtualAddress());

      // t0 (h1: 最新のハイトマップ)
      cl->SetComputeRootDescriptorTable(1, s_srvs[h1Idx].gpu);
      // t1 (h2: 1つ前のハイトマップ)
      cl->SetComputeRootDescriptorTable(2, s_srvs[h2Idx].gpu);
      // u0 (next)
      cl->SetComputeRootDescriptorTable(3, s_uavs[nextIdx].gpu);

      // Dispatch (256x256のテクスチャで16x16のスレッドグループ)
      cl->Dispatch(TEX_SIZE / 16, TEX_SIZE / 16, 1);
    }

    // UAVバリアで完了待ち
    D3D12_RESOURCE_BARRIER uavBarrier = {};
    uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    uavBarrier.UAV.pResource = s_heightMaps[nextIdx].Get();
    cl->ResourceBarrier(1, &uavBarrier);

    // リソースバリアの復元 (nextIdx を SRV に戻す)
    D3D12_RESOURCE_BARRIER barrierToSRV = {};
    barrierToSRV.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrierToSRV.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrierToSRV.Transition.pResource = s_heightMaps[nextIdx].Get();
    barrierToSRV.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barrierToSRV.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrierToSRV.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    if (doReadback && s_readback[readbackSlot].buffer) {
      // UAV -> COPY_SOURCE -> READBACK バッファへコピー -> SRV
      D3D12_RESOURCE_BARRIER toCopy = barrierToSRV;
      toCopy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
      cl->ResourceBarrier(1, &toCopy);

      D3D12_TEXTURE_COPY_LOCATION dst = {};
      dst.pResource = s_readback[readbackSlot].buffer.Get();
      dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      dst.PlacedFootprint = s_readbackFootprint;
      D3D12_TEXTURE_COPY_LOCATION src = {};
      src.pResource = s_heightMaps[nextIdx].Get();
      src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      src.SubresourceIndex = 0;
      cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

      barrierToSRV.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
      cl->ResourceBarrier(1, &barrierToSRV);

      // このコマンドリストは EndFrame（または ExecuteAndReset）で実行され、その直後に
      // 「次のフェンス値」で Signal される。記録時点の次の値を控えておけば、
      // それが完了した時点でコピーも終わっている。
      if (renderCtx.Ctx() && renderCtx.Ctx()->core) {
        s_readback[readbackSlot].fenceValue = renderCtx.Ctx()->core->GetNextFenceValue();
      }
    } else {
      cl->ResourceBarrier(1, &barrierToSRV);
    }
  }, "InteractiveWater_WaveSim");

  // 更新されたものを次回の s_currIdx とする
  s_currIdx = nextIdx;
}

D3D12_GPU_DESCRIPTOR_HANDLE GetInteractiveWaterHeightMap() {
  if (!s_initialized) return {};
  return s_srvs[s_currIdx].gpu;
}

} // namespace RC
