#include "RenderCommon.h"
#include "RenderContext.h"
#include "Mesh/PrimitiveMesh.h"
#include "Mesh/MeshGenerator.h"
#include "function/function.h"
#include "RenderInteractiveWater.h"
#include "Common/SceneContext.h"
#include "../Dx12/Dx12Core.h"
#include <utility>

namespace RC {

// ============================================================================
// 水面パラメータ用 定数バッファ (GPU b6)
// ============================================================================

// HLSL の cbuffer WaterParams と完全に対応するレイアウト
struct WaterParamsCB {
  float time          = 0.0f;
  float waveHeight    = 0.3f;
  float waveSpeed     = 1.5f;
  float waveFreq      = 0.8f;

  float waveHeight2   = 0.15f;
  float waveSpeed2    = 1.0f;
  float waveFreq2     = 1.2f;
  float waveSteepness = 0.4f;

  Vector4 shallowColor = {0.1f, 0.5f, 0.6f, 0.85f};
  Vector4 deepColor    = {0.02f, 0.1f, 0.25f, 0.95f};

  float fresnelPower      = 3.0f;
  float specularPower     = 128.0f;
  float normalScrollSpeed = 0.03f;
  float normalStrength    = 0.6f;

  Vector4 invScreenSize   = {1.0f / 1280.0f, 1.0f / 720.0f, 0.0f, 0.0f};
  Vector4 cameraNearFar   = {0.1f, 1000.0f, 0.0f, 0.0f};
  Vector4 foamParams      = {2.0f, 1.0f, 0.0f, 0.0f}; // x: FoamDepth, y: FoamScale
  Vector4 foamColor       = {1.0f, 1.0f, 1.0f, 1.0f};

  /// @brief 障害物の最大数。HLSL の gObstacles[4] と
  ///        RC::WaterSurface::kMaxObstacles と必ず揃えること
  static constexpr int kMaxObstacles = 4;

  Vector4 obstacles[kMaxObstacles] = {}; // xyz: position, w: radius
  // x: 障害物の数
  // y: 反射波の強さ (1.0 で入射波と同じ振幅の完全反射)
  //    ※ 0 は「反射なし」にならない。シェーダ側が (値 > 0) ? 値 : 1.0 と
  //       フォールバックするため既定値へ化ける（技術的負債 D-14）
  // z: 反射波の到達範囲（障害物半径に対する倍率）
  Vector4 obstacleCount = {0.0f, 1.0f, 3.0f, 0.0f};

  // 海面のリアル化（OceanWaves.hlsli / Water.PS.hlsl）。WaterComponent の既定値と揃える
  // x: 詳細波の強さ, y: 尖り, z: 白波の強さ, w: 頂点で変位させる最短波長(m)
  Vector4 oceanParams  = {1.0f, 0.6f, 0.8f, 3.0f};
  // x: 透明度(m), y: 遠景フェード距離(m), z: 法線マップのタイルサイズ(m, 0=従来UV), w: 白波の量
  Vector4 oceanParams2 = {3.0f, 150.0f, 0.0f, 0.5f};
  // rgb: 波頭を透ける光の色, a: 強さ
  Vector4 sssColor     = {0.10f, 0.62f, 0.52f, 0.9f};
  // 屈折（スクリーンテクスチャ）
  // x: 有効 (1/0。シーン色のコピーが取れたフレームだけ 1 になる), y: 歪みの強さ(画面UV), z: 交差部のフェード幅(m)
  Vector4 refractParams = {0.0f, 0.04f, 0.5f, 0.0f};
};
static_assert(sizeof(WaterParamsCB) % 16 == 0, "WaterParamsCB は 16 バイト境界に揃えること（HLSL cbuffer と一致させる）");

// シングルトン的に定数バッファリソースを管理
/// @brief 水面パラメータの CB。値は CPU 側に持ち、バインド時に今フレームの領域へ送る
/// @details 以前は Map しっぱなしの固定 CB だった（CPU と GPU を並行させると上書き競合が起きる）
static RC::DynamicCB<WaterParamsCB> s_waterCB;
static WaterParamsCB* s_waterCBMapped = nullptr; ///< 書き込み先（= s_waterCB.Ptr()）
static bool s_waterCBInitialized = false;

// ============================================================================
// 屈折用：水を描く直前の画面のコピー
// ============================================================================
// 描画中のレンダーターゲットはそのまま SRV として読めない（書き込み中のため）ので、
// 水を描く直前に同じ形式のテクスチャへ CopyResource して、それを t6 として読む。
// PS はこのコピーを法線で歪めて引くことで、水中の物体がゆらいで見える「屈折」を作る。
static Microsoft::WRL::ComPtr<ID3D12Resource> s_sceneCopy;
static SRVManager::Handle s_sceneCopySrv;
static bool s_refractionEnabled = true;

/// @brief 現在の描画先をシーン色コピーへ写す。成功したら t6 に載せる SRV を返す（失敗時は ptr=0）
static D3D12_GPU_DESCRIPTOR_HANDLE CaptureSceneColor(ID3D12GraphicsCommandList *cl) {
  auto &ctx = GetRenderContext();
  if (!ctx.Ctx() || !ctx.Ctx()->core) return {};
  auto *core = ctx.Ctx()->core;
  ID3D12Resource *src = ctx.Ctx()->currentColorResource;
  if (!src) return {};

  const D3D12_RESOURCE_DESC srcDesc = src->GetDesc();
  if (srcDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || srcDesc.SampleDesc.Count != 1) {
    return {}; // MSAA などは CopyResource できないので屈折なし
  }

  // サイズ・形式が変わったら作り直す（ウィンドウのリサイズ）
  bool needCreate = !s_sceneCopy;
  if (s_sceneCopy) {
    const D3D12_RESOURCE_DESC d = s_sceneCopy->GetDesc();
    needCreate = (d.Width != srcDesc.Width || d.Height != srcDesc.Height || d.Format != srcDesc.Format);
  }
  if (needCreate) {
    if (s_sceneCopy) {
      core->DeferredRelease().Enqueue(std::move(s_sceneCopy), core->GetNextFenceValue());
    }
    if (s_sceneCopySrv.IsValid()) {
      core->SRVMan().Free(s_sceneCopySrv);
      s_sceneCopySrv = SRVManager::Handle{};
    }

    D3D12_RESOURCE_DESC desc = srcDesc;
    desc.MipLevels = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    HRESULT hr = ctx.Device()->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&s_sceneCopy));
    if (FAILED(hr) || !s_sceneCopy) {
      s_sceneCopy.Reset();
      return {};
    }
    s_sceneCopy->SetName(L"RC::WaterSceneColorCopy");
    s_sceneCopySrv = core->SRVMan().CreateTexture2D(s_sceneCopy.Get(), desc.Format, 1);
  }
  if (!s_sceneCopySrv.IsValid()) return {};

  // RT → COPY_SOURCE / コピー先 SRV → COPY_DEST
  D3D12_RESOURCE_BARRIER b[2] = {};
  b[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b[0].Transition.pResource = src;
  b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
  b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  b[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b[1].Transition.pResource = s_sceneCopy.Get();
  b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
  b[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  cl->ResourceBarrier(2, b);

  cl->CopyResource(s_sceneCopy.Get(), src);

  // 元に戻す（RT は描画を続けるので RENDER_TARGET、コピーは PS で読む）
  std::swap(b[0].Transition.StateBefore, b[0].Transition.StateAfter);
  std::swap(b[1].Transition.StateBefore, b[1].Transition.StateAfter);
  cl->ResourceBarrier(2, b);

  return s_sceneCopySrv.gpu;
}

// ============================================================================
// D-01: 障害物リストの控え
// ============================================================================
// 「どれが障害物か」はシーンの中身を知っているアプリ側にしか決められない。
// エンジンは入れ物と受け口だけを持ち、中身は SetWaterObstacles() で
// 毎フレーム外から入れてもらう（以前はここにハードコードしていた）。
//
// 定数バッファは水面を一度描くまで作られないので、直接そこへ書くと
// 「まだ描いていない段階で設定した値が捨てられる」ことになる。
// いったんこの静的領域へ控え、描画時に定数バッファへ写す。
// 取得系（GetWaterObstacles など）もこちらを読むので、
// 水面を描く前でも設定した値がそのまま返る。
static Vector4 s_obstacles[WaterParamsCB::kMaxObstacles] = {};
static int s_obstacleCount = 0;
static float s_reflectStrength = 1.0f; // 1.0 = 入射波と同じ振幅で跳ね返す
static float s_reflectRange = 3.0f;    // 反射波の到達範囲（半径倍率）

// 確認用の上書き（C-03）。
// WaterObstacleScript が毎フレーム SetWaterReflectParams を呼び直すので、
// 外から一度書いた値は次のフレームで消える。定数バッファへ写す直前で
// 横取りすることで、ゲームを動かしたまま反射の効きを見比べられるようにする。
static bool s_reflectOverride = false;
static float s_reflectOverrideStrength = 1.0f;
static float s_reflectOverrideRange = 3.0f;

static void EnsureWaterCB() {
  if (s_waterCBInitialized) return;

  auto &ctx = GetRenderContext();
  if (!ctx.IsInitialized()) return;

  s_waterCBMapped = s_waterCB.Ptr();
  if (s_waterCBMapped) {
    *s_waterCBMapped = WaterParamsCB{};
  }
  s_waterCBInitialized = true;
}

// ============================================================================
// API
// ============================================================================

int GenerateWaterPlane(float width, float height, uint32_t segments, int normalMapHandle) {
  auto &ctx = GetRenderContext();
  ModelData data = MeshGenerator::GeneratePlane(width, height, segments, segments);
  return ctx.PrimitiveMeshes().Create(data, normalMapHandle, "WaterPlane");
}

void DrawWater(int meshHandle, int normalMapHandle) {
  auto &ctx = GetRenderContext();
  if (!ctx.IsInitialized()) return;

  EnsureWaterCB();

  auto *m = ctx.PrimitiveMeshes().Get(meshHandle);
  if (!m) return;

  Matrix4x4 world = MakeAffineMatrix(m->T().scale, m->T().rotation, m->T().translation);
  D3D12_GPU_VIRTUAL_ADDRESS lightAddr = ctx.DirLights().GetActiveCBAddress();
  BlendMode blend = ctx.CurrentBlendMode();

  // 更新: スクリーンサイズとカメラパラメータを設定
  if (s_waterCBMapped && ctx.Ctx() && ctx.Ctx()->core) {
      auto* core = ctx.Ctx()->core;
      const auto& vp = core->Viewport();
      s_waterCBMapped->invScreenSize.x = 1.0f / (vp.Width > 0 ? vp.Width : 1280.0f);
      s_waterCBMapped->invScreenSize.y = 1.0f / (vp.Height > 0 ? vp.Height : 720.0f);
      // MainCamera の Near/Far を取得したいが、固定値でも大抵は0.1 / 1000.0。
      // SceneContext にカメラがあれば取得可能だが、とりあえず固定値で更新（必要なら後で修正）
      s_waterCBMapped->cameraNearFar.x = 0.1f;
      s_waterCBMapped->cameraNearFar.y = 1000.0f;

      // 障害物（岩）の座標と半径を定数バッファへ写す（D-01）。
      // 中身はアプリ側（水面に載せた WaterObstacleScript）が
      // SetWaterObstacles() で毎フレーム入れている。
      // 誰も入れていなければ s_obstacleCount = 0 で、障害物なしとして描かれる。
      for (int i = 0; i < WaterParamsCB::kMaxObstacles; ++i) {
        s_waterCBMapped->obstacles[i] = s_obstacles[i];
      }
      s_waterCBMapped->obstacleCount.x = static_cast<float>(s_obstacleCount);
      // 反射波のチューニング値（VS 側で使用）。
      // 確認用のオーバーライドが有効なら、スクリプトが入れた値より優先する。
      s_waterCBMapped->obstacleCount.y =
          s_reflectOverride ? s_reflectOverrideStrength : s_reflectStrength;
      s_waterCBMapped->obstacleCount.z =
          s_reflectOverride ? s_reflectOverrideRange : s_reflectRange;
  }

  const uint64_t key = SortKey::Make(SortKey::kLayerTranslucent, SortKey::HashPSO("water"), 0);
  ctx.PushCommand3D(key, [m, meshHandle, world, normalMapHandle, lightAddr, blend](ID3D12GraphicsCommandList *cl) {
    auto &ctx = GetRenderContext();
    auto prevBlend = ctx.CurrentBlendMode();
    ctx.SetBlendMode(blend);

    // 水面用パイプラインをバインド
    if (ctx.BindPipeline("water")) {
      ctx.BindCameraCB();
      // 平行光源は描く直前のものを使う（積んだ時点のアドレスを持ち回さない）
      if (const D3D12_GPU_VIRTUAL_ADDRESS curLight = ctx.DirLights().GetActiveCBAddress()) {
        cl->SetGraphicsRootConstantBufferView(3, curLight);
      } else if (lightAddr) {
        cl->SetGraphicsRootConstantBufferView(3, lightAddr);
      }
      ctx.BindAllLightCBs();

      // b6: WaterParams は、下で屈折の有無（refractParams.x）を書いてからバインドする

      // t4: InteractiveWave HeightMap をバインド (RootParameter 12)
      D3D12_GPU_DESCRIPTOR_HANDLE interactiveSrv = GetInteractiveWaterHeightMap();
      if (interactiveSrv.ptr != 0) {
        cl->SetGraphicsRootDescriptorTable(12, interactiveSrv);
      }

      // t5: Depth Texture for Foam (RootParameter 13)
      if (ctx.Ctx() && ctx.Ctx()->core) {
        auto* core = ctx.Ctx()->core;
        static SRVManager::Handle s_depthSrv;
        static ID3D12Resource* s_lastDepthResource = nullptr;
        
        ID3D12Resource* currentDepthResource = core->GetDepthResource();
        if (currentDepthResource != s_lastDepthResource) {
          if (s_depthSrv.IsValid()) {
            core->SRVMan().Free(s_depthSrv);
          }
          if (currentDepthResource) {
            s_depthSrv = core->SRVMan().CreateTexture2D(
                currentDepthResource, DXGI_FORMAT_R24_UNORM_X8_TYPELESS, 1);
          } else {
            s_depthSrv = SRVManager::Handle{};
          }
          s_lastDepthResource = currentDepthResource;
        }

        if (s_depthSrv.IsValid()) {
          // SRVとしてサンプリングするためにバリアで遷移
          D3D12_RESOURCE_BARRIER barrier = {};
          barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
          barrier.Transition.pResource = currentDepthResource;
          barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
          barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_DEPTH_READ;
          barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
          cl->ResourceBarrier(1, &barrier);

          cl->SetGraphicsRootDescriptorTable(13, s_depthSrv.gpu);
        }

        // t6: Scene Color（屈折用。RootParameter 14）
        // コピーが取れなかったフレームは屈折を切り、ルートパラメータが空にならないよう深度 SRV を仮に載せておく
        D3D12_GPU_DESCRIPTOR_HANDLE sceneSrv = s_refractionEnabled ? CaptureSceneColor(cl) : D3D12_GPU_DESCRIPTOR_HANDLE{};
        if (s_waterCBMapped) {
          s_waterCBMapped->refractParams.x = (sceneSrv.ptr != 0) ? 1.0f : 0.0f;
        }
        if (sceneSrv.ptr != 0) {
          cl->SetGraphicsRootDescriptorTable(14, sceneSrv);
        } else if (s_depthSrv.IsValid()) {
          cl->SetGraphicsRootDescriptorTable(14, s_depthSrv.gpu);
        }
      }

      // b6: WaterParams（Object3D と同じパラメータ 0〜10 の次、11 番）。
      // 値は CPU 側にあり、ここで今フレームの領域へ送る（屈折の有無を書いた後なので最新の値になる）
      if (s_waterCBMapped) {
        cl->SetGraphicsRootConstantBufferView(11, s_waterCB.Address());
      }

      ctx.PrimitiveMeshes().ApplyTexture(meshHandle, normalMapHandle);
      m->Draw(cl, world);

      // 描画後、元のDEPTH_WRITE状態に戻す
      if (ctx.Ctx() && ctx.Ctx()->core) {
        ID3D12Resource* currentDepthResource = ctx.Ctx()->core->GetDepthResource();
        if (currentDepthResource) {
          D3D12_RESOURCE_BARRIER barrier = {};
          barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
          barrier.Transition.pResource = currentDepthResource;
          barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_DEPTH_READ;
          barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
          barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
          cl->ResourceBarrier(1, &barrier);
        }
      }
    }
    ctx.SetBlendMode(prevBlend);
  }, "InteractiveWater", meshHandle);
}

void UnloadWater(int meshHandle) {
  GetRenderContext().PrimitiveMeshes().Unload(meshHandle);
}

Transform *GetWaterTransformPtr(int meshHandle) {
  return GetRenderContext().PrimitiveMeshes().GetTransformPtr(meshHandle);
}

void SetWaterParams(float waveHeight, float waveSpeed, float waveFreq,
                    float waveHeight2, float waveSpeed2, float waveFreq2,
                    float waveSteepness,
                    const Vector4 &shallowColor, const Vector4 &deepColor,
                    float fresnelPower, float specularPower,
                    float normalScrollSpeed, float normalStrength) {
  EnsureWaterCB();
  if (!s_waterCBMapped) return;

  // time は別途 SetWaterTime で設定するので保持
  float savedTime = s_waterCBMapped->time;
  s_waterCBMapped->time = savedTime;
  s_waterCBMapped->waveHeight = waveHeight;
  s_waterCBMapped->waveSpeed = waveSpeed;
  s_waterCBMapped->waveFreq = waveFreq;
  s_waterCBMapped->waveHeight2 = waveHeight2;
  s_waterCBMapped->waveSpeed2 = waveSpeed2;
  s_waterCBMapped->waveFreq2 = waveFreq2;
  s_waterCBMapped->waveSteepness = waveSteepness;
  s_waterCBMapped->shallowColor = shallowColor;
  s_waterCBMapped->deepColor = deepColor;
  s_waterCBMapped->fresnelPower = fresnelPower;
  s_waterCBMapped->specularPower = specularPower;
  s_waterCBMapped->normalScrollSpeed = normalScrollSpeed;
  s_waterCBMapped->normalStrength = normalStrength;
}

void SetWaterEnvironmentCoefficient(int meshHandle, float coeff) {
  auto *m = GetRenderContext().PrimitiveMeshes().Get(meshHandle);
  if (!m) return;
  if (auto* mat = m->Mat()) {
    mat->environmentCoefficient = coeff;
  }
}

void SetWaterCrestTint(float crestTint) {
  EnsureWaterCB();
  if (!s_waterCBMapped) return;
  // gFoamParams.z（未使用だった枠）を波の高さによる色付けの強さに使う。
  // x: FoamDepth / y: FoamScale は触らない。
  s_waterCBMapped->foamParams.z = (crestTint > 0.0f) ? crestTint : 0.0f;
}

void SetWaterOceanParams(float detailStrength, float choppiness, float minWavelength,
                         float whitecapStrength, float whitecapCoverage,
                         const Vector4 &sssColor, float clarity,
                         float detailFadeDistance, float normalTileSize) {
  EnsureWaterCB();
  if (!s_waterCBMapped) return;
  auto clamp0 = [](float v) { return (v > 0.0f) ? v : 0.0f; };
  s_waterCBMapped->oceanParams = {clamp0(detailStrength), clamp0(choppiness),
                                  clamp0(whitecapStrength), clamp0(minWavelength)};
  s_waterCBMapped->oceanParams2 = {clamp0(clarity), clamp0(detailFadeDistance),
                                   clamp0(normalTileSize), clamp0(whitecapCoverage)};
  s_waterCBMapped->sssColor = sssColor;
}

void SetWaterRefraction(bool enable, float strength, float edgeFade) {
  EnsureWaterCB();
  s_refractionEnabled = enable;
  if (!s_waterCBMapped) return;
  // x（実際に有効か）は描画時にコピーが取れたかで決めるのでここでは触らない
  s_waterCBMapped->refractParams.y = (strength > 0.0f) ? strength : 0.0f;
  s_waterCBMapped->refractParams.z = (edgeFade > 0.001f) ? edgeFade : 0.001f;
}

void SetWaterTime(float timeSec) {
  EnsureWaterCB();
  if (s_waterCBMapped) {
    s_waterCBMapped->time = timeSec;
  }
}

float GetWaterTime() {
  // CB がまだ無い（水面を一度も描いていない）場合は 0 を返す。
  // EnsureWaterCB() を呼ばないのは、取得だけのために GPU リソースを
  // 作らないため。
  return s_waterCBMapped ? s_waterCBMapped->time : 0.0f;
}

int GetWaterObstacles(Vector4* out, int maxCount) {
  // 定数バッファではなく控えのほうを読む。
  // 水面をまだ一度も描いていない段階でも、設定した値がそのまま返るようにするため。
  if (!out) return s_obstacleCount;

  // 実際に書き込めた数を返す。s_obstacleCount を返してしまうと、小さいバッファを
  // 渡した呼び出し側が未初期化の領域を読んでしまう。
  const int writable =
      (s_obstacleCount < maxCount) ? s_obstacleCount : ((maxCount > 0) ? maxCount : 0);
  for (int i = 0; i < writable; ++i) {
    out[i] = s_obstacles[i];
  }
  return writable;
}

void SetWaterObstacles(const Vector4* obstacles, int count) {
  if (!obstacles || count <= 0) count = 0;
  if (count > WaterParamsCB::kMaxObstacles) count = WaterParamsCB::kMaxObstacles;

  for (int i = 0; i < count; ++i) {
    s_obstacles[i] = obstacles[i];
  }
  // 使わなくなった枠は消しておく。残しておくと、障害物が減ったときに
  // 古い岩の位置で波が平らなままになる。
  for (int i = count; i < WaterParamsCB::kMaxObstacles; ++i) {
    s_obstacles[i] = {0.0f, 0.0f, 0.0f, 0.0f};
  }
  s_obstacleCount = count;
}

int GetMaxWaterObstacles() { return WaterParamsCB::kMaxObstacles; }

float GetWaterReflectStrength() { return s_reflectStrength; }

float GetWaterReflectRange() { return s_reflectRange; }

void SetWaterReflectParams(float strength, float range) {
  s_reflectStrength = strength;
  s_reflectRange = range;
}

void SetWaterReflectOverride(bool enable, float strength, float range) {
  s_reflectOverride = enable;
  // 0 は「無効」ではなく既定値へのフォールバックとして解釈されてしまうため（D-14）、
  // ここで正の最小値へ丸めておく。呼び出し側の書き間違いで
  // 「弱くしたつもりが既定値の 1.0 に戻る」事故を防ぐ。
  constexpr float kMin = 0.001f;
  s_reflectOverrideStrength = (strength > kMin) ? strength : kMin;
  s_reflectOverrideRange = (range > kMin) ? range : kMin;
}

bool GetWaterReflectOverride(float* outStrength, float* outRange) {
  if (outStrength) *outStrength = s_reflectOverrideStrength;
  if (outRange) *outRange = s_reflectOverrideRange;
  return s_reflectOverride;
}

void TermWaterResources() {
  s_waterCBMapped = nullptr;
  s_waterCBInitialized = false;
  // 屈折用のシーン色コピー（終了時にだけ呼ばれるので即解放でよい）
  s_sceneCopy.Reset();
  s_sceneCopySrv = SRVManager::Handle{};
  // 障害物の控えも空にする。残したままだと、次に読み込んだシーンで
  // 前のシーンの岩の位置に波の平らな穴が空く。
  SetWaterObstacles(nullptr, 0);
}

} // namespace RC
