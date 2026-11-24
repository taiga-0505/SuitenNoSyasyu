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
  float alpha;         ///< 波の伝播速度
  float damping;       ///< 毎フレームの速度減衰率
  int sourceCount;     ///< sources の有効数
  float padding;       ///< リセットフラグ（1 ならハイトマップを 0 クリアする）
  Vector4 sources[64]; ///< 波源（xy: UV、z: 半径、w: 強さ）
  // ---- 引き波の泡（G チャンネル）----
  float foamDecay;         ///< 引き波の泡の毎フレーム残存率
  float foamSpread;        ///< 引き波の泡の毎フレームの滲み
  int foamSourceCount;     ///< foamSources の有効数
  float crestDecay;        ///< 波頭（B チャンネル）の残存率
  Vector4 foamSources[64]; ///< 引き波の泡の波源（xy: UV、z: 半径、w: 足す量）
  // ---- 波頭の白波（B チャンネル）----
  int crestSourceCount;      ///< crestSources の有効数
  float crestPad[3];         ///< 16 バイト境界合わせ
  Vector4 crestSources[128]; ///< 波頭の白波の波源（xy: UV、z: 半径、w: 足す量）
  // ---- 範囲（窓）の移動 ----
  int shiftPrev[2];     ///< t0（1 フレーム前）を読むときのずらし量（テクセル。x, y）
  int shiftPrevPrev[2]; ///< t1（2 フレーム前）を読むときのずらし量（テクセル。x, y）
  // ---- 墨（A チャンネル）----
  float inkDecay;         ///< 墨の毎フレーム残存率
  float inkSpread;        ///< 墨の毎フレームの滲み
  int inkSourceCount;     ///< inkSources の有効数
  float inkPad;           ///< 16 バイト境界合わせ
  Vector4 inkSources[64]; ///< 墨の波源（xy: UV、z: 半径、w: 足す量）
};
static_assert(sizeof(WaveSimCB) == 16 + 16 * 64 + 16 + 16 * 64 + 16 + 16 * 128 + 16 + 16 + 16 * 64,
              "WaveSimCB layout must match WaveSimulation.CS.hlsl");

static constexpr int TEX_SIZE = 256; ///< ハイトマップの一辺（テクセル）
/// @brief 1 テクセルが覆うワールドの長さ（m）。窓はこの単位で動く
static constexpr float kTexelWorld = kInteractiveWaterWorldSize / static_cast<float>(TEX_SIZE);
/// @brief ハイトマップの形式。R = 高さ / G = 引き波の泡 / B = 波頭の白波 / A = 墨
/// @details 3 チャンネルの UAV 形式は無いので 4 チャンネルにしている。
static constexpr DXGI_FORMAT kHeightMapFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;
static constexpr int kHeightMapChannels = 4;   ///< 1 テクセルあたりの float 数
static constexpr int kMaxWaveSources = 64;     ///< 波紋の波源の上限（1 フレーム）
static constexpr int kMaxFoamSources = 64;     ///< 引き波の泡の波源の上限（1 フレーム）
static constexpr int kMaxCrestSources = 128;   ///< 波頭の白波の波源の上限（1 フレーム）
static constexpr int kMaxInkSources = 64;      ///< 墨の波源の上限（1 フレーム）
static Microsoft::WRL::ComPtr<ID3D12Resource> s_heightMaps[3]; ///< ハイトマップ（最新・1 つ前・書き込み先の 3 枚を回す）
static SRVManager::Handle s_srvs[3]; ///< 各ハイトマップの SRV
static SRVManager::Handle s_uavs[3]; ///< 各ハイトマップの UAV
static int s_currIdx = 0; ///< 最新のハイトマップのインデックス

/// @struct TexelCoord
/// @brief 窓の中心（ワールド座標をテクセル単位に丸めたもの。x がワールド X、z がワールド Z）
struct TexelCoord {
  int x = 0; ///< ワールド X / kTexelWorld
  int z = 0; ///< ワールド Z / kTexelWorld
};
/// @brief 各ハイトマップを書いたときの窓の中心
/// @details 前フレームの中身を読むとき「書いたときの中心」と「今回の中心」の差だけずらす。
///          1 つ前と 2 つ前で中心が違うことがあるので、テクスチャごとに持つ。
static TexelCoord s_texCenter[3];
static float s_desiredCenterX = 0.0f; ///< SetInteractiveWaterCenter で指定された中心（ワールド X）
static float s_desiredCenterZ = 0.0f; ///< SetInteractiveWaterCenter で指定された中心（ワールド Z）

/// @brief 波源をワールド座標で控える（窓が動いても位置がずれないよう、UV への変換は Update で行う）
struct PendingSource {
  float x;        ///< ワールド X（m）
  float z;        ///< ワールド Z（m）
  float radius;   ///< 半径（UV 空間。0.01 = 1m）
  float strength; ///< 高さの加算値・足す量
};

/// @brief 波シミュレーションの CB。値は CPU 側に持ち、バインド時に今フレームの領域へ送る
/// @details 以前は Map しっぱなしの固定 CB だった（CPU と GPU を並行させると上書き競合が起きる）。
static RC::DynamicCB<WaveSimCB> s_simCB;
static WaveSimCB* s_simCBMapped = nullptr; ///< 書き込み先（= s_simCB.Ptr()）

static std::vector<PendingSource> s_pendingSources;      ///< 次の Update で消費する波源
static std::vector<PendingSource> s_pendingFoamSources;  ///< 引き波の泡の波源（strength = 足す量）
static std::vector<PendingSource> s_pendingCrestSources; ///< 波頭の白波の波源（strength = 足す量）
static std::vector<PendingSource> s_pendingInkSources;   ///< 墨の波源（strength = 足す量）
static float s_foamDecay = 0.992f; ///< 引き波の泡の毎フレーム残存率
static float s_foamSpread = 0.10f; ///< 引き波の泡の毎フレームの滲み
static float s_crestDecay = 0.80f; ///< 波頭の白波の毎フレーム残存率
/// @brief 墨の毎フレーム残存率（0.994 で 60fps のとき約 1.9 秒で半減）
/// @details 0.997 / 滲み 0.18 では、数発撃つと数 m の滑らかな染みが重なって大きな影のようになっていた
static float s_inkDecay = 0.994f;
static float s_inkSpread = 0.07f;  ///< 墨の毎フレームの滲み（小さくして、点々や筋の形を残す）
static bool s_initialized = false; ///< InitInteractiveWater 済みか
static int s_resetFrames = 3; ///< 残りのリセットフレーム数（最初の 3 フレームはテクスチャを 0 クリアする）

/// @brief 指定された中心をテクセル単位に丸める（次の Update で使う窓の中心）
/// @return 丸めた中心
static TexelCoord SnappedDesiredCenter() {
  TexelCoord c;
  c.x = static_cast<int>(std::lround(s_desiredCenterX / kTexelWorld));
  c.z = static_cast<int>(std::lround(s_desiredCenterZ / kTexelWorld));
  return c;
}

/// @brief ワールド座標を、中心 c の窓の UV（0〜1）へ変換する
/// @param c      窓の中心
/// @param worldX ワールド X（m）
/// @param worldZ ワールド Z（m）
/// @return 窓の UV（範囲外もそのまま返す）
static Vector2 WorldToWindowUV(const TexelCoord& c, float worldX, float worldZ) {
  return {(worldX - static_cast<float>(c.x) * kTexelWorld) / kInteractiveWaterWorldSize + 0.5f,
          (worldZ - static_cast<float>(c.z) * kTexelWorld) / kInteractiveWaterWorldSize + 0.5f};
}

/// @brief 半径 radius の波源が中心 c の窓に少しでも掛かるか
/// @param c      窓の中心
/// @param s      波源（ワールド座標）
/// @return 掛かるなら true
static bool OverlapsWindow(const TexelCoord& c, const PendingSource& s) {
  const Vector2 uv = WorldToWindowUV(c, s.x, s.z);
  const float r = (std::max)(s.radius, 0.0f);
  return uv.x >= -r && uv.x <= 1.0f + r && uv.y >= -r && uv.y <= 1.0f + r;
}

// ---- CPU 読み戻し（任意） ----
// GPU が書き終えたハイトマップを READBACK ヒープへコピーし、フェンスが通ったものだけ CPU 配列へ写す。
// スロットはリングで回す。同じスロットへ再コピーするのは kReadbackCount フレーム後で、
// CommandContext::BeginFrame が (frameCount) フレーム前の完了を待つので kReadbackCount >= kMaxFrames なら
// 「GPU がまだ読み書き中のバッファ」を CPU が触ることはない。
static constexpr int kReadbackCount = 4; ///< 読み戻しスロット数（CommandContext::kMaxFrames（3）以上にする）
/// @struct ReadbackSlot
/// @brief 読み戻しリングの 1 スロット
struct ReadbackSlot {
  Microsoft::WRL::ComPtr<ID3D12Resource> buffer; ///< READBACK ヒープのバッファ
  uint64_t fenceValue = 0; ///< このスロットへのコピーを含むコマンドが完了するフェンス値（0 = 未使用）
  TexelCoord center;       ///< コピーしたハイトマップを書いたときの窓の中心
};
static ReadbackSlot s_readback[kReadbackCount]; ///< 読み戻しリング
static int s_readbackWriteIdx = 0;              ///< 次にコピーを書き込むスロット
static D3D12_PLACED_SUBRESOURCE_FOOTPRINT s_readbackFootprint{}; ///< ハイトマップをバッファへコピーするときの配置
static UINT64 s_readbackTotalBytes = 0;         ///< 読み戻しバッファ 1 つのサイズ（バイト）
static bool s_readbackEnabled = false;          ///< CPU 読み戻しが有効か
static bool s_readbackHasData = false;   ///< s_cpuHeight に有効なデータが入っているか
static uint64_t s_readbackLastFence = 0; ///< 最後に CPU へ写したスロットのフェンス値
static std::vector<float> s_cpuHeight;   ///< TEX_SIZE*TEX_SIZE、行優先（y*TEX_SIZE+x）
static TexelCoord s_cpuCenter;           ///< s_cpuHeight を書いたときの窓の中心

/// @brief UAV を許可した 2D テクスチャを DEFAULT ヒープに作る
/// @param device D3D12 デバイス
/// @param width  幅（テクセル）
/// @param height 高さ（テクセル）
/// @param format テクスチャ形式
/// @return 作成したリソース。失敗したら nullptr
/// @note 初期状態はシェーダーリソース（ピクセル／非ピクセル両方）。
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

// CreateBufferResource は function.h に定義済みなので、ここでは定義しない

// ---------------------------------------------------------------------------
// CPU 読み戻し
// ---------------------------------------------------------------------------

/// @brief READBACK バッファを（初回だけ）作る
/// @return バッファが用意できていれば true。未初期化・作成失敗なら false（失敗時は読み戻しを無効にする）
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
    // テクセルは (高さ, 引き波, 波頭, 墨) の float4。CPU 側は高さ（R）だけを使う
    for (int y = 0; y < TEX_SIZE; ++y) {
      const auto* row = reinterpret_cast<const float*>(src + static_cast<size_t>(y) * rowPitch);
      float* dst = &s_cpuHeight[static_cast<size_t>(y) * TEX_SIZE];
      for (int x = 0; x < TEX_SIZE; ++x) dst[x] = row[x * kHeightMapChannels];
    }
    const D3D12_RANGE noWrite{0, 0};
    s_readback[best].buffer->Unmap(0, &noWrite);
    s_readbackHasData = true;
    s_readbackLastFence = bestFence;
    s_cpuCenter = s_readback[best].center;
  }
  // 消費済み／古いスロットは捨てる（同じデータを二度読まない）
  for (int i = 0; i < kReadbackCount; ++i) {
    if (s_readback[i].fenceValue != 0 && s_readback[i].fenceValue <= bestFence) s_readback[i].fenceValue = 0;
  }
}

/// @brief CPU 配列を Water.VS.hlsl の gSamplerClamp（linear, clamp）と同じ規則で読む
/// @param u UV の u（0〜1。範囲外はクランプ）
/// @param v UV の v（0〜1。範囲外はクランプ）
/// @return 双線形補間した高さ（m）
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

/// @brief 窓の縁で 0 へ落とす重み（Water.hlsli の WaveWindowEdgeFade と同じ式）
/// @param u 窓の UV の u
/// @param v 窓の UV の v
/// @return 重み（0〜1。窓の外は 0）
static float WindowEdgeFade(float u, float v) {
  const float d = (std::min)((std::min)(u, 1.0f - u), (std::min)(v, 1.0f - v));
  return std::clamp(d / 0.04f, 0.0f, 1.0f);
}

float SampleInteractiveWaterHeight(float worldX, float worldZ) {
  if (!s_readbackEnabled || !s_readbackHasData) return 0.0f;
  const Vector2 uv = WorldToWindowUV(s_cpuCenter, worldX, worldZ);
  // 水面の VS と同じく、窓の縁ではぼかし、窓の外は 0（浮かべた物が見えない波に揺れないように）
  return SampleCpuHeightMap(uv.x, uv.y) * WindowEdgeFade(uv.x, uv.y);
}

bool SampleInteractiveWater(float worldX, float worldZ, float& outHeight, Vector3& outNormal) {
  outHeight = 0.0f;
  outNormal = {0.0f, 1.0f, 0.0f};
  if (!s_readbackEnabled || !s_readbackHasData) return false;
  const Vector2 uv = WorldToWindowUV(s_cpuCenter, worldX, worldZ);
  const float u = uv.x;
  const float v = uv.y;
  // 水面の VS / PS と同じく、窓の縁ではぼかし、窓の外は平らな水面として扱う
  const float fade = WindowEdgeFade(u, v);
  if (fade <= 0.0f) return true;
  outHeight = SampleCpuHeightMap(u, v) * fade;

  // Water.VS.hlsl の interactiveNormal と同じ有限差分：
  //   dX = (200*texel, hR-hL, 0), dZ = (0, hD-hU, 200*texel), N = normalize(cross(dZ, dX))
  //   = normalize(-(hR-hL), 200*texel, -(hD-hU))
  constexpr float texel = 1.0f / static_cast<float>(TEX_SIZE);
  const float hL = SampleCpuHeightMap(u - texel, v);
  const float hR = SampleCpuHeightMap(u + texel, v);
  const float hU = SampleCpuHeightMap(u, v - texel);
  const float hD = SampleCpuHeightMap(u, v + texel);
  const float nx = -(hR - hL) * fade;
  const float ny = 2.0f * kInteractiveWaterWorldSize * texel;
  const float nz = -(hD - hU) * fade;
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
  s_simCBMapped = s_simCB.Ptr();
  if (s_simCBMapped) {
    s_simCBMapped->alpha = 0.45f;
    s_simCBMapped->damping = 0.985f;
    s_simCBMapped->sourceCount = 0;
    s_simCBMapped->foamDecay = s_foamDecay;
    s_simCBMapped->foamSpread = s_foamSpread;
    s_simCBMapped->foamSourceCount = 0;
    s_simCBMapped->crestDecay = s_crestDecay;
    s_simCBMapped->crestSourceCount = 0;
    s_simCBMapped->shiftPrev[0] = s_simCBMapped->shiftPrev[1] = 0;
    s_simCBMapped->shiftPrevPrev[0] = s_simCBMapped->shiftPrevPrev[1] = 0;
    s_simCBMapped->inkDecay = s_inkDecay;
    s_simCBMapped->inkSpread = s_inkSpread;
    s_simCBMapped->inkSourceCount = 0;
    s_simCBMapped->inkPad = 0.0f;
  }

  // 3 枚とも「今の指定の中心」で書いたことにする（最初の 3 フレームは 0 クリアされる）
  const TexelCoord c = SnappedDesiredCenter();
  for (auto& tc : s_texCenter) tc = c;

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

  s_simCBMapped = nullptr;

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
  s_cpuCenter = TexelCoord{};

  s_pendingSources.clear();
  s_pendingFoamSources.clear();
  s_pendingCrestSources.clear();
  s_pendingInkSources.clear();

  s_initialized = false;
  Log::Print("[RenderInteractiveWater] Terminated");
}

void SetInteractiveWaterCenter(float worldX, float worldZ) {
  if (!std::isfinite(worldX) || !std::isfinite(worldZ)) return;
  // テクセル数へ丸めるときに int（MSVC の long は 32bit）を溢れさせない範囲に収める
  constexpr float kMaxCenter = 1.0e6f;
  s_desiredCenterX = std::clamp(worldX, -kMaxCenter, kMaxCenter);
  s_desiredCenterZ = std::clamp(worldZ, -kMaxCenter, kMaxCenter);
}

void GetInteractiveWaterWindow(float& outCenterX, float& outCenterZ, float& outSize) {
  const TexelCoord& c = s_initialized ? s_texCenter[s_currIdx] : TexelCoord{};
  outCenterX = static_cast<float>(c.x) * kTexelWorld;
  outCenterZ = static_cast<float>(c.z) * kTexelWorld;
  outSize = kInteractiveWaterWorldSize;
}

/// @brief 波源をリストへ積む（次の Update で使う窓に掛からない・上限超えは捨てる）
/// @param list     積む先のリスト
/// @param maxCount リストの上限
/// @param worldX   ワールド X（m）
/// @param worldZ   ワールド Z（m）
/// @param radius   半径（UV 空間。0.01 = 1m）
/// @param value    高さの加算値・足す量
/// @return 積めたら true
static bool PushSource(std::vector<PendingSource>& list, size_t maxCount, float worldX, float worldZ, float radius,
                       float value) {
  if (list.size() >= maxCount) return false;
  const PendingSource s{worldX, worldZ, radius, value};
  // 窓の中心は Update で決まるが、指定の中心から丸めるだけなので、ここで同じ窓を使って判定できる
  if (!OverlapsWindow(SnappedDesiredCenter(), s)) return false;
  list.push_back(s);
  return true;
}

void AddWaveSource(const WaveSource& source) {
  // uv は「原点中心の固定 UV」（以前の呼び出し側の書き方）。ワールド座標へ戻して積む
  const float worldX = (source.uv.x - 0.5f) * kInteractiveWaterWorldSize;
  const float worldZ = (source.uv.y - 0.5f) * kInteractiveWaterWorldSize;
  PushSource(s_pendingSources, kMaxWaveSources, worldX, worldZ, source.radius, source.strength);
}

bool AddWaveSourceAtWorld(float worldX, float worldZ, float radius, float strength) {
  return PushSource(s_pendingSources, kMaxWaveSources, worldX, worldZ, radius, strength);
}

/// @brief 泡・墨の波源をリストへ積む（足す量が 0 以下・範囲外・上限超えは捨てる）
/// @param list     積む先のリスト
/// @param maxCount リストの上限
/// @param worldX   ワールド X（m）
/// @param worldZ   ワールド Z（m）
/// @param radius   半径（UV 空間。0.01 = 1m）
/// @param amount   足す量（0 以下なら捨てる）
/// @return 積めたら true
static bool PushFoamLike(std::vector<PendingSource>& list, size_t maxCount, float worldX, float worldZ, float radius,
                         float amount) {
  if (amount <= 0.0f) return false;
  return PushSource(list, maxCount, worldX, worldZ, radius, amount);
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

bool AddInkSourceAtWorld(float worldX, float worldZ, float radius, float amount) {
  return PushFoamLike(s_pendingInkSources, kMaxInkSources, worldX, worldZ, radius, amount);
}

void SetInteractiveInkParams(float decayPerFrame, float spreadPerFrame) {
  s_inkDecay = std::clamp(decayPerFrame, 0.0f, 1.0f);
  s_inkSpread = std::clamp(spreadPerFrame, 0.0f, 0.25f);
}

void GetInteractiveInkParams(float& outDecayPerFrame, float& outSpreadPerFrame) {
  outDecayPerFrame = s_inkDecay;
  outSpreadPerFrame = s_inkSpread;
}

/// @brief 控えた波源（ワールド座標）を、中心 c の窓の UV に直して CB の配列へ詰める
/// @param list     控えた波源
/// @param maxCount CB の配列の大きさ
/// @param c        今回の窓の中心
/// @param out      詰める先（CB の配列）
/// @return 詰めた数
static int PackSources(const std::vector<PendingSource>& list, int maxCount, const TexelCoord& c, Vector4* out) {
  int n = 0;
  for (const PendingSource& s : list) {
    if (n >= maxCount) break;
    if (!OverlapsWindow(c, s)) continue;
    const Vector2 uv = WorldToWindowUV(c, s.x, s.z);
    out[n++] = Vector4(uv.x, uv.y, s.radius, s.strength);
  }
  return n;
}

void UpdateInteractiveWater() {
  if (!s_initialized) return;
  auto& ctx = GetRenderContext();

  // インデックスのローテーション
  // s_currIdx は前フレームでレンダリングした「最新(h1)」のテクスチャ
  int h1Idx = s_currIdx;
  // 2 フレーム前の「古い(h2)」テクスチャ
  int h2Idx = (s_currIdx + 2) % 3;
  // 今回レンダリングする「次(out)」のテクスチャ
  int nextIdx = (s_currIdx + 1) % 3;

  // 今回の窓の中心。前の 2 枚はそれぞれ書いたときの中心との差だけずらして読む
  const TexelCoord center = SnappedDesiredCenter();
  const TexelCoord prevC = s_texCenter[h1Idx];
  const TexelCoord prevPrevC = s_texCenter[h2Idx];
  s_texCenter[nextIdx] = center;

  // CB の更新
  if (s_simCBMapped) {
    // 窓の移動（テクセル。テクスチャの x がワールド X、y がワールド Z）
    s_simCBMapped->shiftPrev[0] = center.x - prevC.x;
    s_simCBMapped->shiftPrev[1] = center.z - prevC.z;
    s_simCBMapped->shiftPrevPrev[0] = center.x - prevPrevC.x;
    s_simCBMapped->shiftPrevPrev[1] = center.z - prevPrevC.z;

    s_simCBMapped->alpha = 0.15f;   // 波の伝播速度（穏やかな広がり）
    s_simCBMapped->damping = 0.95f; // 速度減衰（0.95^60≈0.05、約1秒でほぼ消滅）
    
    // リセット処理
    if (s_resetFrames > 0) {
        s_simCBMapped->padding = 1.0f;
        s_resetFrames--;
    } else {
        s_simCBMapped->padding = 0.0f;
    }

    // 波源は控えたワールド座標を、今回の窓の UV に直して詰める
    s_simCBMapped->sourceCount = PackSources(s_pendingSources, kMaxWaveSources, center, s_simCBMapped->sources);

    // 引き波の泡
    s_simCBMapped->foamDecay = s_foamDecay;
    s_simCBMapped->foamSpread = s_foamSpread;
    s_simCBMapped->foamSourceCount =
        PackSources(s_pendingFoamSources, kMaxFoamSources, center, s_simCBMapped->foamSources);
    // 波頭の白波
    s_simCBMapped->crestDecay = s_crestDecay;
    s_simCBMapped->crestSourceCount =
        PackSources(s_pendingCrestSources, kMaxCrestSources, center, s_simCBMapped->crestSources);
    // 墨
    s_simCBMapped->inkDecay = s_inkDecay;
    s_simCBMapped->inkSpread = s_inkSpread;
    s_simCBMapped->inkSourceCount =
        PackSources(s_pendingInkSources, kMaxInkSources, center, s_simCBMapped->inkSources);
  }
  s_pendingSources.clear();
  s_pendingFoamSources.clear();
  s_pendingCrestSources.clear();
  s_pendingInkSources.clear();

  // 前のフレームまでにコピーが終わっているハイトマップを CPU へ写す（読み戻しが有効なときだけ）
  if (s_readbackEnabled && !s_readback[0].buffer) EnsureReadbackBuffers();
  ConsumeReadback();
  const bool doReadback = s_readbackEnabled && s_readback[0].buffer;
  const int readbackSlot = s_readbackWriteIdx;
  if (doReadback) {
    s_readbackWriteIdx = (s_readbackWriteIdx + 1) % kReadbackCount;
    s_readback[readbackSlot].center = center; // このスロットへ写すハイトマップの窓
  }

  // リソースバリアから先の GPU コマンドをキューに積む（sortKey 0 で最初に処理させる）
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

    // Compute Shader のディスパッチ
    ID3D12PipelineState* pso = nullptr;
    ID3D12RootSignature* root = nullptr;
    if (renderCtx.Ctx() && renderCtx.Ctx()->pipelineManager) {
      pso = renderCtx.Ctx()->pipelineManager->GetComputePSO("wave_simulation");
      root = renderCtx.Ctx()->pipelineManager->GetComputeRoot("wave_simulation");
    }

    if (pso && root) {
      cl->SetPipelineState(pso);
      cl->SetComputeRootSignature(root);

      // b0: シミュレーションの定数バッファ
      cl->SetComputeRootConstantBufferView(0, s_simCB.Address());

      // t0（h1: 最新のハイトマップ）
      cl->SetComputeRootDescriptorTable(1, s_srvs[h1Idx].gpu);
      // t1（h2: 1 つ前のハイトマップ）
      cl->SetComputeRootDescriptorTable(2, s_srvs[h2Idx].gpu);
      // u0（next: 今回書き込むハイトマップ）
      cl->SetComputeRootDescriptorTable(3, s_uavs[nextIdx].gpu);

      // 256x256 のテクスチャを 16x16 のスレッドグループで覆う
      cl->Dispatch(TEX_SIZE / 16, TEX_SIZE / 16, 1);
    }

    // UAV バリアで書き込みの完了を待つ
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
      // UAV → COPY_SOURCE → READBACK バッファへコピー → SRV の順に遷移させる
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
