#include "RenderInteractiveSand.h"
#include "RenderCommon.h"
#include "RenderContext.h"
#include "Mesh/PrimitiveMesh.h"
#include "Mesh/MeshGenerator.h"
#include "Dx12/PipelineManager.h"
#include "Dx12/SRVManager/SRVManager.h"
#include "Dx12/Dx12Core.h"
#include "Common/Log/Log.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace RC {

// =============================================================================
// 跡のシミュレーション（SandSimulation.CS.hlsl）
// =============================================================================

static constexpr int kMaxDigs = 128;   ///< 1 フレームに積める「掘る」の上限
static constexpr int kMaxSmooths = 64; ///< 1 フレームに積める「均す」の上限

/// @brief SandSimulation.CS.hlsl の SandSimParams と同じ並び（16 バイト境界を崩さないこと）
struct SandSimCB {
  int digCount;          ///< digs の有効数
  int smoothCount;       ///< smooths の有効数
  float resetFlag;       ///< 1 ならハイトマップを 0 クリアする
  float heightKeep;      ///< 跡の高さの毎フレーム残存率
  int shift[2];          ///< 前フレームを読むときのずらし量（テクセル。x, y）
  float freshKeep;       ///< 掘りたての湿りの毎フレーム残存率
  float wetKeep;         ///< 波の濡れの毎フレーム残存率
  float slump;           ///< 周りの平均へ寄せる割合
  float texelWorld;      ///< 1 テクセルのワールドの長さ（m）
  float pad[2];          ///< 16 バイト境界合わせ
  Vector4 digs[kMaxDigs];      ///< 掘る（xy: UV、z: 半径（UV）、w: 深さ（m））
  Vector4 digExtra[kMaxDigs];  ///< 掘るの続き（x: 縁の盛り上がり（m）、y: 湿り）
  Vector4 smooths[kMaxSmooths]; ///< 均す（xy: UV、z: 半径（UV）、w: 割合）
};
static_assert(sizeof(SandSimCB) == 48 + 16 * kMaxDigs * 2 + 16 * kMaxSmooths,
              "SandSimCB layout must match SandSimulation.CS.hlsl");

static constexpr int kTexSize = kInteractiveSandTexSize;
static constexpr float kTexelWorld = kInteractiveSandWorldSize / static_cast<float>(kTexSize);
static constexpr DXGI_FORMAT kSandMapFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;
/// @brief 砂が描かれなくなってから、何フレームまでシミュレーションを回し続けるか
static constexpr int kKeepAliveFrames = 30;

/// @brief 跡の要求（ワールド座標で控え、UV への変換は Update で行う）
struct PendingDig {
  float x, z;     ///< ワールド XZ（m）
  float radius;   ///< 半径（m）
  float depth;    ///< 深さ（m）
  float rim;      ///< 縁の盛り上がり（m）
  float fresh;    ///< 湿り（0〜1）
};
/// @brief 均しの要求
struct PendingSmooth {
  float x, z;   ///< ワールド XZ（m）
  float radius; ///< 半径（m）
  float amount; ///< 割合（0〜1）
};

/// @struct TexelCoord
/// @brief 窓の中心（ワールド座標をテクセル単位に丸めたもの）
struct SandTexelCoord {
  int x = 0; ///< ワールド X / kTexelWorld
  int z = 0; ///< ワールド Z / kTexelWorld
};

static Microsoft::WRL::ComPtr<ID3D12Resource> s_maps[2]; ///< ハイトマップ（最新と書き込み先を交互に使う）
static SRVManager::Handle s_srvs[2];                     ///< 各ハイトマップの SRV
static SRVManager::Handle s_uavs[2];                     ///< 各ハイトマップの UAV
static SandTexelCoord s_mapCenter[2];                    ///< 各ハイトマップを書いたときの窓の中心
static int s_currIdx = 0;                                ///< 最新のハイトマップ
static float s_desiredX = 0.0f;                          ///< 指定された中心（ワールド X）
static float s_desiredZ = 0.0f;                          ///< 指定された中心（ワールド Z）
static RC::DynamicCB<SandSimCB> s_simCB;                 ///< シミュレーションの CB
static std::vector<PendingDig> s_pendingDigs;            ///< 次の Update で使う「掘る」
static std::vector<PendingSmooth> s_pendingSmooths;      ///< 次の Update で使う「均す」
static float s_heightKeep = 0.9997f;                     ///< 跡の高さの毎フレーム残存率
static float s_slump = 0.015f;                           ///< 周りの平均へ寄せる割合
static float s_freshKeep = 0.992f;                       ///< 掘りたての湿りの残存率
static float s_wetKeep = 0.996f;                         ///< 波の濡れの残存率
static int s_resetFrames = 2;                            ///< 残りの 0 クリアのフレーム数（2 枚とも消す）
static int s_aliveFrames = 0;                            ///< あと何フレームシミュレーションを回すか
static bool s_initialized = false;                       ///< InitInteractiveSand 済みか

/// @brief 指定された中心をテクセル単位に丸める
/// @return 丸めた中心
static SandTexelCoord SnappedCenter() {
  return {static_cast<int>(std::lround(s_desiredX / kTexelWorld)),
          static_cast<int>(std::lround(s_desiredZ / kTexelWorld))};
}

/// @brief ワールド座標を中心 c の窓の UV へ変換する
/// @param c 窓の中心
/// @param x ワールド X（m）
/// @param z ワールド Z（m）
/// @return 窓の UV（範囲外もそのまま返す）
static Vector2 WorldToUV(const SandTexelCoord &c, float x, float z) {
  return {(x - static_cast<float>(c.x) * kTexelWorld) / kInteractiveSandWorldSize + 0.5f,
          (z - static_cast<float>(c.z) * kTexelWorld) / kInteractiveSandWorldSize + 0.5f};
}

/// @brief 半径 radiusUV の円が窓に少しでも掛かるか
/// @param uv       中心の UV
/// @param radiusUV 半径（UV）
/// @return 掛かるなら true
static bool OverlapsWindow(const Vector2 &uv, float radiusUV) {
  return uv.x >= -radiusUV && uv.x <= 1.0f + radiusUV && uv.y >= -radiusUV && uv.y <= 1.0f + radiusUV;
}

/// @brief UAV を許可した 2D テクスチャを作る（初期状態はシェーダーリソース）
/// @param device D3D12 デバイス
/// @return 作成したリソース。失敗したら nullptr
static Microsoft::WRL::ComPtr<ID3D12Resource> CreateSandMap(ID3D12Device *device) {
  Microsoft::WRL::ComPtr<ID3D12Resource> res;
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = kTexSize;
  desc.Height = kTexSize;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = kSandMapFormat;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  heap.CreationNodeMask = 1;
  heap.VisibleNodeMask = 1;
  const HRESULT hr = device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &desc,
      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
      IID_PPV_ARGS(&res));
  if (FAILED(hr)) return nullptr;
  return res;
}

void InitInteractiveSand() {
  if (s_initialized) return;
  auto &ctx = GetRenderContext();
  if (!ctx.IsInitialized()) return;
  auto device = ctx.Device();
  for (int i = 0; i < 2; ++i) {
    s_maps[i] = CreateSandMap(device);
    if (!s_maps[i]) {
      Log::Print("[RenderInteractiveSand] failed to create sand map");
      return;
    }
    s_srvs[i] = ctx.Ctx()->core->SRVMan().CreateTexture2D(s_maps[i].Get(), kSandMapFormat, 1);
    s_uavs[i] = ctx.Ctx()->core->SRVMan().CreateTexture2DUAV(s_maps[i].Get(), kSandMapFormat);
    s_mapCenter[i] = SnappedCenter();
  }
  s_currIdx = 0;
  s_resetFrames = 2;
  s_aliveFrames = 0;
  s_initialized = true;
  Log::Print("[RenderInteractiveSand] Initialized");
}

void TermInteractiveSand() {
  if (!s_initialized) return;
  auto &ctx = GetRenderContext();
  for (int i = 0; i < 2; ++i) {
    if (ctx.Ctx() && ctx.Ctx()->core) {
      ctx.Ctx()->core->SRVMan().Free(s_srvs[i]);
      ctx.Ctx()->core->SRVMan().Free(s_uavs[i]);
    }
    s_maps[i].Reset();
  }
  s_pendingDigs.clear();
  s_pendingSmooths.clear();
  s_initialized = false;
  Log::Print("[RenderInteractiveSand] Terminated");
}

void SetInteractiveSandCenter(float worldX, float worldZ) {
  if (!std::isfinite(worldX) || !std::isfinite(worldZ)) return;
  constexpr float kMaxCenter = 1.0e6f;
  s_desiredX = std::clamp(worldX, -kMaxCenter, kMaxCenter);
  s_desiredZ = std::clamp(worldZ, -kMaxCenter, kMaxCenter);
}

void GetInteractiveSandWindow(float &outCenterX, float &outCenterZ, float &outSize) {
  const SandTexelCoord c = s_initialized ? s_mapCenter[s_currIdx] : SnappedCenter();
  outCenterX = static_cast<float>(c.x) * kTexelWorld;
  outCenterZ = static_cast<float>(c.z) * kTexelWorld;
  outSize = kInteractiveSandWorldSize;
}

bool AddSandDig(float worldX, float worldZ, float radius, float depth, float rim, float fresh) {
  if (s_pendingDigs.size() >= static_cast<size_t>(kMaxDigs)) return false;
  if (radius <= 0.0f || !std::isfinite(worldX) || !std::isfinite(worldZ)) return false;
  const Vector2 uv = WorldToUV(SnappedCenter(), worldX, worldZ);
  if (!OverlapsWindow(uv, radius * 2.0f / kInteractiveSandWorldSize)) return false;
  s_pendingDigs.push_back({worldX, worldZ, radius, (std::max)(depth, 0.0f), (std::max)(rim, 0.0f),
                           std::clamp(fresh, 0.0f, 1.0f)});
  s_aliveFrames = kKeepAliveFrames;
  return true;
}

bool AddSandSmooth(float worldX, float worldZ, float radius, float amount) {
  if (s_pendingSmooths.size() >= static_cast<size_t>(kMaxSmooths)) return false;
  if (radius <= 0.0f || amount <= 0.0f || !std::isfinite(worldX) || !std::isfinite(worldZ)) return false;
  const Vector2 uv = WorldToUV(SnappedCenter(), worldX, worldZ);
  if (!OverlapsWindow(uv, radius / kInteractiveSandWorldSize)) return false;
  s_pendingSmooths.push_back({worldX, worldZ, radius, std::clamp(amount, 0.0f, 1.0f)});
  s_aliveFrames = kKeepAliveFrames;
  return true;
}

void SetInteractiveSandParams(float heightKeep, float slump, float freshKeep, float wetKeep) {
  s_heightKeep = std::clamp(heightKeep, 0.0f, 1.0f);
  s_slump = std::clamp(slump, 0.0f, 0.25f);
  s_freshKeep = std::clamp(freshKeep, 0.0f, 1.0f);
  s_wetKeep = std::clamp(wetKeep, 0.0f, 1.0f);
}

void ClearInteractiveSand() {
  s_resetFrames = 2;
  s_aliveFrames = kKeepAliveFrames;
}

void UpdateInteractiveSand() {
  if (!s_initialized) return;
  if (s_aliveFrames <= 0) {
    // 砂を描くシーンにいない。積まれた要求も捨てる（窓の外へ出たものと同じ扱い）
    s_pendingDigs.clear();
    s_pendingSmooths.clear();
    return;
  }
  --s_aliveFrames;

  const int prevIdx = s_currIdx;
  const int nextIdx = 1 - s_currIdx;
  const SandTexelCoord center = SnappedCenter();
  const SandTexelCoord prevC = s_mapCenter[prevIdx];
  s_mapCenter[nextIdx] = center;

  SandSimCB &cb = s_simCB.Value();
  cb.shift[0] = center.x - prevC.x;
  cb.shift[1] = center.z - prevC.z;
  cb.resetFlag = (s_resetFrames > 0) ? 1.0f : 0.0f;
  if (s_resetFrames > 0) --s_resetFrames;
  cb.heightKeep = s_heightKeep;
  cb.slump = s_slump;
  cb.freshKeep = s_freshKeep;
  cb.wetKeep = s_wetKeep;
  cb.texelWorld = kTexelWorld;
  cb.pad[0] = cb.pad[1] = 0.0f;

  int n = 0;
  for (const PendingDig &d : s_pendingDigs) {
    if (n >= kMaxDigs) break;
    const Vector2 uv = WorldToUV(center, d.x, d.z);
    const float rUV = d.radius / kInteractiveSandWorldSize;
    if (!OverlapsWindow(uv, rUV * 2.0f)) continue;
    cb.digs[n] = Vector4(uv.x, uv.y, rUV, d.depth);
    cb.digExtra[n] = Vector4(d.rim, d.fresh, 0.0f, 0.0f);
    ++n;
  }
  cb.digCount = n;
  n = 0;
  for (const PendingSmooth &s : s_pendingSmooths) {
    if (n >= kMaxSmooths) break;
    const Vector2 uv = WorldToUV(center, s.x, s.z);
    const float rUV = s.radius / kInteractiveSandWorldSize;
    if (!OverlapsWindow(uv, rUV)) continue;
    cb.smooths[n] = Vector4(uv.x, uv.y, rUV, s.amount);
    ++n;
  }
  cb.smoothCount = n;
  s_pendingDigs.clear();
  s_pendingSmooths.clear();
  s_simCB.Refresh();

  auto &ctx = GetRenderContext();
  // 水面の波紋と同じく、3D 描画キューの先頭（sortKey 0）で計算する
  ctx.PushCommand3D(0, [prevIdx, nextIdx](ID3D12GraphicsCommandList *cl) {
    auto &rc = GetRenderContext();
    if (!cl || !rc.Ctx() || !rc.Ctx()->pipelineManager) return;
    ID3D12PipelineState *pso = rc.Ctx()->pipelineManager->GetComputePSO("sand_simulation");
    ID3D12RootSignature *root = rc.Ctx()->pipelineManager->GetComputeRoot("sand_simulation");
    if (!pso || !root) return;

    D3D12_RESOURCE_BARRIER toUav = {};
    toUav.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toUav.Transition.pResource = s_maps[nextIdx].Get();
    toUav.Transition.StateBefore =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    toUav.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    toUav.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cl->ResourceBarrier(1, &toUav);

    cl->SetPipelineState(pso);
    cl->SetComputeRootSignature(root);
    cl->SetComputeRootConstantBufferView(0, s_simCB.Address());
    // WaveSimulationCS のルートシグネチャ（b0 / t0 / t1 / u0）を流用する。t1 は使わないので同じものを載せる
    cl->SetComputeRootDescriptorTable(1, s_srvs[prevIdx].gpu);
    cl->SetComputeRootDescriptorTable(2, s_srvs[prevIdx].gpu);
    cl->SetComputeRootDescriptorTable(3, s_uavs[nextIdx].gpu);
    cl->Dispatch(kTexSize / 16, kTexSize / 16, 1);

    D3D12_RESOURCE_BARRIER uav = {};
    uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav.UAV.pResource = s_maps[nextIdx].Get();
    cl->ResourceBarrier(1, &uav);

    D3D12_RESOURCE_BARRIER toSrv = toUav;
    toSrv.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    toSrv.Transition.StateAfter =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    cl->ResourceBarrier(1, &toSrv);
  }, "InteractiveSand_Sim");

  s_currIdx = nextIdx;
}

D3D12_GPU_DESCRIPTOR_HANDLE GetInteractiveSandMap() {
  if (!s_initialized) return {};
  return s_srvs[s_currIdx].gpu;
}

// =============================================================================
// 砂の地形と描画（Sand.VS.hlsl / Sand.PS.hlsl）
// =============================================================================

/// @brief Sand.PS.hlsl の SandParams と同じ並び
struct SandParamsCB {
  Vector4 window;      ///< 跡のハイトマップの窓。xy: 中心のワールド XZ, z: 一辺（m）, w: 1 テクセルの長さ（m）
  Vector4 dryColor;    ///< 乾いた砂の色
  Vector4 wetColor;    ///< 濡れた砂の色
  Vector4 grainColorA; ///< 混じる粒の色 1
  Vector4 grainColorB; ///< 混じる粒の色 2
  Vector4 grain;       ///< x: 粒の細かさ, y: 色のばらつき, z: 色の付いた粒の割合, w: 時間（秒）
  Vector4 ripple;      ///< x: 風紋の間隔（m）, y: 強さ, z: 向き cos, w: 向き sin
  Vector4 wet;         ///< x: 水面の高さ, y: 濡れて見える高さ, z: 照り返し, w: きらめき
  Vector4 mark;        ///< x: 跡の陰影の強さ, y: 掘りたての暗さ
};

static RC::DynamicCB<SandParamsCB> s_sandCB; ///< 砂の見た目の CB（描くたびに今の値を送る）

int CreateSandTerrain(const float *heights, uint32_t countX, uint32_t countZ, float width, float depth) {
  if (!heights || countX < 2 || countZ < 2 || width <= 0.0f || depth <= 0.0f) return -1;
  auto &ctx = GetRenderContext();
  if (!ctx.IsInitialized()) return -1;

  const float dx = width / static_cast<float>(countX - 1);
  const float dz = depth / static_cast<float>(countZ - 1);
  auto H = [&](int ix, int iz) {
    ix = std::clamp(ix, 0, static_cast<int>(countX) - 1);
    iz = std::clamp(iz, 0, static_cast<int>(countZ) - 1);
    return heights[static_cast<size_t>(iz) * countX + static_cast<size_t>(ix)];
  };

  ModelData data;
  data.vertices.reserve(static_cast<size_t>(countX) * countZ);
  for (uint32_t iz = 0; iz < countZ; ++iz) {
    for (uint32_t ix = 0; ix < countX; ++ix) {
      VertexData v;
      const float x = -width * 0.5f + dx * static_cast<float>(ix);
      const float z = -depth * 0.5f + dz * static_cast<float>(iz);
      v.position = {x, H(ix, iz), z, 1.0f};
      v.texcoord = {static_cast<float>(ix) / static_cast<float>(countX - 1),
                    static_cast<float>(iz) / static_cast<float>(countZ - 1)};
      // 中央差分の傾きから法線を求める
      const float sx = (H(ix + 1, iz) - H(ix - 1, iz)) / (2.0f * dx);
      const float sz = (H(ix, iz + 1) - H(ix, iz - 1)) / (2.0f * dz);
      const float len = std::sqrt(sx * sx + 1.0f + sz * sz);
      v.normal = {-sx / len, 1.0f / len, -sz / len};
      data.vertices.push_back(v);
    }
  }
  data.indices.reserve(static_cast<size_t>(countX - 1) * (countZ - 1) * 6);
  for (uint32_t iz = 0; iz + 1 < countZ; ++iz) {
    for (uint32_t ix = 0; ix + 1 < countX; ++ix) {
      const uint32_t a = iz * countX + ix;
      const uint32_t b = a + 1;
      const uint32_t c = a + countX;
      const uint32_t d = c + 1;
      data.indices.push_back(a);
      data.indices.push_back(c);
      data.indices.push_back(b);
      data.indices.push_back(b);
      data.indices.push_back(c);
      data.indices.push_back(d);
    }
  }
  data.rootNode.name = "SandTerrain";
  data.rootNode.localMatrix = MakeIdentity4x4();
  return ctx.PrimitiveMeshes().Create(data, -1, "SandTerrain");
}

Transform *GetSandTransformPtr(int meshHandle) {
  return GetRenderContext().PrimitiveMeshes().GetTransformPtr(meshHandle);
}

void UnloadSandTerrain(int meshHandle) {
  if (meshHandle < 0) return;
  GetRenderContext().PrimitiveMeshes().Unload(meshHandle);
}

void DrawSand(int meshHandle, const SandLook &look) {
  auto &ctx = GetRenderContext();
  if (!ctx.IsInitialized()) return;
  auto *m = ctx.PrimitiveMeshes().Get(meshHandle);
  if (!m) return;

  // 砂を描いている間は跡のシミュレーションを回す
  s_aliveFrames = kKeepAliveFrames;

  SandParamsCB &p = s_sandCB.Value();
  {
    float cx = 0.0f, cz = 0.0f, size = kInteractiveSandWorldSize;
    GetInteractiveSandWindow(cx, cz, size);
    p.window = {cx, cz, size, kTexelWorld};
  }
  p.dryColor = look.dryColor;
  p.wetColor = look.wetColor;
  p.grainColorA = look.grainColorA;
  p.grainColorB = look.grainColorB;
  p.grain = {look.grainScale, look.grainStrength, look.speckDensity, look.time};
  const float rad = look.rippleDirDeg * 0.01745329252f;
  p.ripple = {(std::max)(look.rippleWavelength, 0.05f), look.rippleStrength, std::cos(rad), std::sin(rad)};
  p.wet = {look.waterLevel, (std::max)(look.wetBand, 0.01f), look.wetShine, look.sparkle};
  p.mark = {look.markStrength, look.freshDarken, 0.0f, 0.0f};

  const Matrix4x4 world = MakeAffineMatrix(m->T().scale, m->T().rotation, m->T().translation);
  // CB は積んだ時点の値で送る（同じフレームに見た目の違う砂を 2 つ描いても混ざらないように）
  const SandParamsCB snapshot = p;
  const uint64_t key = SortKey::Make(SortKey::kLayerOpaque, SortKey::HashPSO("sand"), 0);
  ctx.PushCommand3D(key, [m, meshHandle, world, snapshot](ID3D12GraphicsCommandList *cl) {
    auto &rc = GetRenderContext();
    // 砂は影を落とさない（影パスでは Water ルートの引数を Object3D のルートへ積んでしまうので描かない）
    if (rc.IsShadowPass()) return;
    if (!rc.BindPipeline("sand")) return;
    rc.BindCameraCB();
    if (const D3D12_GPU_VIRTUAL_ADDRESS light = rc.DirLights().GetActiveCBAddress()) {
      cl->SetGraphicsRootConstantBufferView(3, light);
    }
    rc.BindAllLightCBs();
    s_sandCB.Value() = snapshot;
    cl->SetGraphicsRootConstantBufferView(11, s_sandCB.Address());
    const D3D12_GPU_DESCRIPTOR_HANDLE sandMap = GetInteractiveSandMap();
    if (sandMap.ptr != 0) {
      // t4: 跡のハイトマップ。ほかのテーブル（t5 / t6 / t8 / t9）はシェーダーが読まないが、空にしないよう同じものを載せる
      cl->SetGraphicsRootDescriptorTable(12, sandMap);
      cl->SetGraphicsRootDescriptorTable(13, sandMap);
      cl->SetGraphicsRootDescriptorTable(14, sandMap);
      cl->SetGraphicsRootDescriptorTable(17, sandMap);
      cl->SetGraphicsRootDescriptorTable(18, sandMap);
    }
    rc.PrimitiveMeshes().ApplyTexture(meshHandle, -1);
    m->Draw(cl, world);
  }, "Sand", meshHandle);
}

} // namespace RC
