#include "RenderContext.h"

#include "Common/Log/Log.h"
#include "Dx12/Dx12Core.h"
#include "PipelineManager.h"
#include "Primitive/Primitive2D.h"
#include "Primitive/Primitive3D.h"
#include "Common/SceneContext.h"
#include "AppConfig.h" // SceneContext::app の画面サイズを参照する
#include "Graphics/PostProcess/PostProcess.h"
#include "Graphics/Texture/RenderTexture/RenderTexture.h" // マスクRT（unique_ptr の実体化に必要）
#include "RenderCommon.h"
#include <algorithm>
#include <format>

namespace RC {

// 前方宣言の型を unique_ptr で持つため、コンストラクタ／デストラクタはここでだけ定義する
RenderContext::RenderContext() = default;
RenderContext::~RenderContext() = default;

// ============================================================================
// シングルトン / 取得
// ============================================================================

RenderContext &RenderContext::GetInstance() {
  static RenderContext instance;
  return instance;
}

RenderContext &GetRenderContext() { return RenderContext::GetInstance(); }

// ============================================================================
// Init / Term
// ============================================================================

void RenderContext::Init(SceneContext &ctx) {
  if (initialized_) {
    return;
  }

  ctxRef_ = &ctx;
  postProcess_ = ctx.postProcess;
  device_ = ctx.core->GetDevice();
  srvHeap_ = &ctx.core->SRV();

  texMan_.Init(&ctx.core->SRVMan());
  spriteMan_.Init(device_.Get(), &texMan_);
  fontMan_.Init(device_.Get(), &ctx.core->SRVMan());
  modelMan_.Init(device_.Get(), &texMan_);
  skydomeMan_.Init(device_.Get(), &texMan_);
  skyboxMan_.Init(device_.Get(), &texMan_);
  primitiveMeshMan_.Init(device_.Get(), &texMan_);

  dirLightMan_.Init(device_.Get());
  ptLightMan_.Init(device_.Get());
  spLightMan_.Init(device_.Get());
  arLightMan_.Init(device_.Get());

  // CameraCB
  cameraCB_ = CreateBufferResource(device_.Get(), sizeof(CameraCB),
                                   L"RenderContext::CameraCB");
  cameraCB_->Map(0, nullptr, reinterpret_cast<void **>(&cameraCBMapped_));

  // FogOverlayCB
  fogCB_ = CreateBufferResource(device_.Get(), sizeof(FogOverlayCB),
                                L"RenderContext::FogCB");
  fogCB_->Map(0, nullptr, reinterpret_cast<void **>(&fogCBMapped_));
  if (fogCBMapped_) {
    *fogCBMapped_ = FogOverlayCB{};
  }

  // ShadowCB
  shadowCB_ = CreateBufferResource(device_.Get(), sizeof(ShadowParams),
                                   L"RenderContext::ShadowCB");
  shadowCB_->Map(0, nullptr, reinterpret_cast<void **>(&shadowCBMapped_));
  if (shadowCBMapped_) {
    *shadowCBMapped_ = ShadowParams{};
  }

  // ShadowMap 初期化 (例: 2048x2048)
  shadowMap_.Create(ctx.core, 2048, 2048);
  shadowCBBoundAddr_ = shadowCB_ ? shadowCB_->GetGPUVirtualAddress() : 0;

  // スポット影アトラス初期化（kSpotShadowTileSize px のタイルを kSpotShadowTilesX × kSpotShadowTilesY 枚）
  spotShadowAtlas_.Create(ctx.core, kSpotShadowTileSize * kSpotShadowTilesX,
                          kSpotShadowTileSize * kSpotShadowTilesY);
  if (auto *res = spotShadowAtlas_.GetResource()) {
    res->SetName(L"SpotShadowAtlas Resource");
  }
  spotShadowAtlasOpen_ = false;
  currentSpotShadowTile_ = -1;
  spotShadowCBMapped_ = nullptr;
  spotShadowPassParamsMapped_ = nullptr;
  spotShadowPassParamsAddr_ = 0;

  // b7 の既定値（count = 0）。PreDraw3D が今フレーム分の一時 CB を確保するまでの間、
  // および PreDraw3D を通らないフレームで使う
  spotShadowFallbackCB_ = CreateBufferResource(device_.Get(), sizeof(SpotShadowCB),
                                               L"RenderContext::SpotShadowFallbackCB");
  spotShadowCBAddr_ = 0;
  if (spotShadowFallbackCB_) {
    SpotShadowCB *fallbackMapped = nullptr;
    if (SUCCEEDED(spotShadowFallbackCB_->Map(0, nullptr, reinterpret_cast<void **>(&fallbackMapped))) &&
        fallbackMapped) {
      *fallbackMapped = SpotShadowCB{};
      ApplySpotShadowAtlasInfo_(*fallbackMapped);
      spotShadowFallbackCB_->Unmap(0, nullptr);
    }
    spotShadowCBAddr_ = spotShadowFallbackCB_->GetGPUVirtualAddress();
  }

  view_ = MakeIdentity4x4();
  proj_ = MakeIdentity4x4();
  cl_ = nullptr;
  currentBlendMode_ = kBlendModeNone;

  // FrameResource 初期化（トリプルバッファ）
  for (uint32_t i = 0; i < FrameResource::kFrameCount; ++i) {
    frameResources_[i].Init(device_.Get(), i);
  }
  frameIndex_ = 0;

  initialized_ = true;

  // CS スキニング用パイプラインを ModelManager に注入
  if (ctxRef_ && ctxRef_->pipelineManager) {
    modelMan_.SetSkinningCS(ctxRef_->pipelineManager, &ctx.core->SRVMan());
  }

  // Dissolve ノイズテクスチャ初期化 (TextureManager が有効な状態で行う)
  if (postProcess_) {
    postProcess_->InitDissolveNoiseTextures();
  }
}

void RenderContext::Term() {
  if (!initialized_) {
    return;
  }

  // 残っている非同期タスクを全て待機
  WaitAllLoads();

  // インスタンス描画の要求と VirtualEntity を破棄（ModelObject を指しているので ModelManager より先に）
  instanceBatcher_.Clear();
  modelProxies_.Clear();

  shadowMap_.Term();
  spotShadowAtlas_.Term();
  spotShadowFallbackCB_.Reset();
  spotShadowCBMapped_ = nullptr;
  spotShadowCBAddr_ = 0;
  spotShadowPassParamsMapped_ = nullptr;
  spotShadowPassParamsAddr_ = 0;
  shadowCBBoundAddr_ = 0;

  modelMan_.Term();
  skydomeMan_.Term();
  skyboxMan_.Term();
  primitiveMeshMan_.Term();
  spriteMan_.Term();
  fontMan_.Term();
  TextMeshGenerator::ClearCache(); // 3D 文字用にキャッシュした DWrite / D2D オブジェクトを解放

  dirLightMan_.Term();
  ptLightMan_.Term();
  spLightMan_.Term();
  arLightMan_.Term();

  prim2D_.reset();
  prim3D_.reset();

  for (uint32_t i = 0; i < FrameResource::kFrameCount; ++i) {
    frameResources_[i].Term();
  }

  if (cameraCB_) {
    if (cameraCBMapped_) {
      cameraCB_->Unmap(0, nullptr);
      cameraCBMapped_ = nullptr;
    }
    cameraCB_.Reset();
  }

  if (fogCB_) {
    if (fogCBMapped_) {
      fogCB_->Unmap(0, nullptr);
      fogCBMapped_ = nullptr;
    }
    fogCB_.Reset();
  }

  if (shadowCB_) {
    if (shadowCBMapped_) {
      shadowCB_->Unmap(0, nullptr);
      shadowCBMapped_ = nullptr;
    }
    shadowCB_.Reset();
  }

  texMan_.Term();
  TermWaterResources();

  // PSO 検索キャッシュとダミー SRV のキャッシュを破棄（PipelineManager / TextureManager が消えるため）
  psoCache_.clear();
  dummyWhiteSrv_ = {};
  commandQueue3D_.clear();
  deferredCommands3D_.clear();
  currentCommandHistory_.clear();
  lastCommandHistory_.clear();

  // マスク RT を解放する。
  // RenderContext は関数ローカル static のシングルトンなので、ここで解放しないと
  // ID3D12Resource が静的デストラクタ（Dx12Core::Term よりも後）まで生き残り、
  // ReportLiveObjects で Live ID3D12Resource / Live ID3D12Device として報告される。
  maskTexture_.reset();
  maskReady_ = false;

  ctxRef_ = nullptr;
  device_.Reset();
  srvHeap_ = nullptr;
  cl_ = nullptr;

  initialized_ = false;
}

// ============================================================================
// Camera
// ============================================================================

void RenderContext::SetCamera(const Matrix4x4 &view, const Matrix4x4 &proj,
                              const Vector3 &camWorldPos) {
  view_ = view;
  proj_ = proj;

  if (cameraCBMapped_) {
    cameraCBMapped_->worldPos = camWorldPos;
    cameraCBMapped_->_pad = 0.0f;
  }

  if (postProcess_) {
    Matrix4x4 projInv = Inverse(proj);
    postProcess_->SetProjectionInverse(&projInv.m[0][0]);

    // Caustics がビュー空間 → ワールド空間の復元に使用する
    Matrix4x4 viewInv = Inverse(view);
    postProcess_->SetViewInverse(&viewInv.m[0][0]);
  }
}

// ============================================================================
// 描画パス実行
// ============================================================================

void RenderContext::PreDraw3D(SceneContext &ctx, ID3D12GraphicsCommandList *cl) {
  // 前フレームの履歴を保存して今フレーム用をクリア
  // （swap で 2 本のバッファを使い回し、毎フレームの再確保を無くす）
  std::swap(lastCommandHistory_, currentCommandHistory_);
  currentCommandHistory_.clear();

  cl_ = cl;
  ctxRef_ = &ctx;
  currentBlendMode_ = kBlendModeNone;

  // マスクは「今フレーム描かれたか」で有効になる。
  // マスクパスを回さないシーン（Title / Result 等）で前フレームのマスクを
  // 読み続けないよう、フレーム頭で必ず無効化する。
  maskReady_ = false;
  if (postProcess_) {
    postProcess_->SetMaskSRV({});
  }

  auto *pso = GetPipeline("object3d", kBlendModeNone);
  if (!pso) {
    cl_ = nullptr;
    ctxRef_ = nullptr;
    return;
  }

  cl->SetGraphicsRootSignature(pso->Root());
  cl->SetPipelineState(pso->PSO());
  cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

  BindCameraCB();
  SyncLightCBs();
  BindAllLightCBs();
  // BindShadow() は Execute3DCommands で通常パスの実行時のみバインドする

  modelMan_.ResetAllBatchCursors();
  instanceBatcher_.BeginFrame(); // 前フレームのインスタンス描画統計を確定

  // FrameResource: フレームインデックスを進めてリセット
  AdvanceFrame();
  CurrentFrame().Reset();

  // スポット影用の一時 CB を今フレーム分だけ確保する。
  // GPU が前フレームを実行中でも書き込みが競合しないよう、Map 済みの固定 CB ではなく
  // FrameResource（フレーム毎のリニアアロケータ）から取る。
  // 未使用フレームでも count = 0 の有効な CB が b7 に載るようここで必ず初期化する。
  {
    void *dst = nullptr;
    spotShadowCBAddr_ = CurrentFrame().AllocCB(sizeof(SpotShadowCB), &dst);
    spotShadowCBMapped_ = reinterpret_cast<SpotShadowCB *>(dst);
    if (spotShadowCBMapped_) {
      *spotShadowCBMapped_ = SpotShadowCB{};
      ApplySpotShadowAtlasInfo_(*spotShadowCBMapped_);
    }

    void *sliceDst = nullptr;
    spotShadowPassParamsAddr_ = CurrentFrame().AllocCB(
        kShadowParamsSliceStride * kMaxSpotShadows, &sliceDst);
    spotShadowPassParamsMapped_ = reinterpret_cast<ShadowParams *>(sliceDst);
  }
  shadowCBBoundAddr_ = shadowCB_ ? shadowCB_->GetGPUVirtualAddress() : 0;
  spotShadowAtlasOpen_ = false;
  currentSpotShadowTile_ = -1;

  // CS スキニングの一括Dispatch
  modelMan_.DispatchAllSkinning(cl, CurrentFrame());

  if (auto *prim = EnsurePrimitive3D()) {
    prim->BeginFrame(view_, proj_, kBlendModeNone);
  }
}

void RenderContext::PreDraw2D(SceneContext &ctx, ID3D12GraphicsCommandList *cl) {
  cl_ = cl;
  ctxRef_ = &ctx;
  currentBlendMode_ = kBlendModeNormal;

  // 3D コマンドキューを一括実行
  Execute3DCommands();

  // 今フレームの一時 CB（b7）はここで役目を終える。以降（および PreDraw3D を通らない次フレーム）で
  // object3d 系が描かれても count = 0 の固定 CB が載るようにする
  spotShadowCBMapped_ = nullptr;
  spotShadowCBAddr_ = spotShadowFallbackCB_ ? spotShadowFallbackCB_->GetGPUVirtualAddress() : 0;

  // 2D Viewport / Scissor
  D3D12_VIEWPORT viewport{};
  viewport.TopLeftX = 0.0f;
  viewport.TopLeftY = 0.0f;
  viewport.Width = static_cast<float>(ctx.app->width);
  viewport.Height = static_cast<float>(ctx.app->height);
  viewport.MinDepth = 0.0f;
  viewport.MaxDepth = 1.0f;

  D3D12_RECT scissor{};
  scissor.left = 0;
  scissor.top = 0;
  scissor.right = static_cast<LONG>(ctx.app->width);
  scissor.bottom = static_cast<LONG>(ctx.app->height);

  cl->RSSetViewports(1, &viewport);
  cl->RSSetScissorRects(1, &scissor);

  auto *pso = GetPipeline("sprite", kBlendModeNormal);
  if (!pso) {
    cl_ = nullptr;
    ctxRef_ = nullptr;
    return;
  }

  // Primitive2D の BeginFrame
  if (auto *prim = EnsurePrimitive2D()) {
    prim->BeginFrame();
  }

  // FontManager の BeginFrame（頂点リングの切り替え・正射影の更新）
  fontMan_.BeginFrame(static_cast<float>(ctx.app->width),
                      static_cast<float>(ctx.app->height));

  cl->SetGraphicsRootSignature(pso->Root());
  cl->SetPipelineState(pso->PSO());
  cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

void RenderContext::ResumeDraw2D(SceneContext &ctx, ID3D12GraphicsCommandList *cl) {
  cl_ = cl;
  ctxRef_ = &ctx;
  currentBlendMode_ = kBlendModeNormal;

  // ※ Execute3DCommands / BeginFrame は呼ばない（PreDraw2D 済みの続きとして描く）

  // 2D Viewport / Scissor（ポストプロセスが張り替えているので戻す）
  D3D12_VIEWPORT viewport{};
  viewport.TopLeftX = 0.0f;
  viewport.TopLeftY = 0.0f;
  viewport.Width = static_cast<float>(ctx.app->width);
  viewport.Height = static_cast<float>(ctx.app->height);
  viewport.MinDepth = 0.0f;
  viewport.MaxDepth = 1.0f;

  D3D12_RECT scissor{};
  scissor.left = 0;
  scissor.top = 0;
  scissor.right = static_cast<LONG>(ctx.app->width);
  scissor.bottom = static_cast<LONG>(ctx.app->height);

  cl->RSSetViewports(1, &viewport);
  cl->RSSetScissorRects(1, &scissor);

  auto *pso = GetPipeline("sprite", kBlendModeNormal);
  if (!pso) {
    cl_ = nullptr;
    ctxRef_ = nullptr;
    return;
  }
  cl->SetGraphicsRootSignature(pso->Root());
  cl->SetPipelineState(pso->PSO());
  cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

void RenderContext::PreDraw2DBackground(SceneContext &ctx,
                                        ID3D12GraphicsCommandList *cl) {
  cl_ = cl;
  ctxRef_ = &ctx;
  currentBlendMode_ = kBlendModeNormal;

  // ※ ここでは Execute3DCommands() を呼ばない。
  //    3D の描画コマンドは PreDraw2D の中でまとめて発行されるので、
  //    この時点で記録したスプライトは必ず 3D より前に実行される。
  //    スプライトPSOは深度書き込みOFFなので深度バッファを汚さず、
  //    後続のモデルが深度テストに通ってそのまま上書きする＝奥に見える。

  // 2D Viewport / Scissor
  D3D12_VIEWPORT viewport{};
  viewport.TopLeftX = 0.0f;
  viewport.TopLeftY = 0.0f;
  viewport.Width = static_cast<float>(ctx.app->width);
  viewport.Height = static_cast<float>(ctx.app->height);
  viewport.MinDepth = 0.0f;
  viewport.MaxDepth = 1.0f;

  D3D12_RECT scissor{};
  scissor.left = 0;
  scissor.top = 0;
  scissor.right = static_cast<LONG>(ctx.app->width);
  scissor.bottom = static_cast<LONG>(ctx.app->height);

  cl->RSSetViewports(1, &viewport);
  cl->RSSetScissorRects(1, &scissor);

  auto *pso = GetPipeline("sprite", kBlendModeNormal);
  if (!pso) {
    cl_ = nullptr;
    ctxRef_ = nullptr;
    return;
  }

  // Primitive2D / FontManager の BeginFrame はここでは行わない。
  // （1フレームに2回呼ぶと頂点リングが二重に進んでしまうため）

  cl->SetGraphicsRootSignature(pso->Root());
  cl->SetPipelineState(pso->PSO());
  cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

// ============================================================================
// テクスチャヘルパー
// ============================================================================

int RenderContext::LoadTex(const std::string &path, bool srgb) {
  if (!initialized_) {
    return -1;
  }
  return texMan_.LoadID(path, srgb);
}

D3D12_GPU_DESCRIPTOR_HANDLE RenderContext::GetSrv(int texHandle) {
  if (!initialized_ || texHandle < 0) {
    return D3D12_GPU_DESCRIPTOR_HANDLE{0};
  }
  return texMan_.GetSrv(texHandle);
}

// ============================================================================
// PSO ヘルパー
// ============================================================================

GraphicsPipeline *RenderContext::GetPipeline(std::string_view prefix,
                                             BlendMode mode) {
  if (!ctxRef_ || !ctxRef_->pipelineManager) {
    return nullptr;
  }

  // ドローごとに呼ばれるため、まず小さな線形キャッシュを引く
  // （std::string のキー生成＋unordered_map の検索を毎回行わない）。
  // 使われる prefix は数十種類なので線形探索で十分速い。
  for (const auto &e : psoCache_) {
    if (e.mode == mode && e.prefix == prefix) {
      return e.pso;
    }
  }

  GraphicsPipeline *pso =
      ctxRef_->pipelineManager->Get(PipelineManager::MakeKey(prefix, mode));

  if (!pso && mode != kBlendModeNormal) {
    pso = ctxRef_->pipelineManager->Get(
        PipelineManager::MakeKey(prefix, kBlendModeNormal));
  }

  // 見つかったものだけキャッシュする（後から登録される PSO を取りこぼさないため）。
  // PipelineManager はエントリを削除しない（Rebuild も同じオブジェクトを再利用）のでポインタは有効
  if (pso && psoCache_.size() < 256u) {
    psoCache_.push_back({std::string(prefix), mode, pso});
  }

  return pso;
}

GraphicsPipeline *RenderContext::BindPipeline(std::string_view prefix) {
  if (!cl_ || !ctxRef_) {
    return nullptr;
  }

  std::string_view actualPrefix = prefix;
  if (isShadowPass_) {
    // スキニング用・インスタンシング用などの対応
    if (prefix.find("skin") != std::string_view::npos) {
      actualPrefix = "shadow_skin";
    } else if (prefix.find("inst") != std::string_view::npos) {
      actualPrefix = "shadow_inst";
    } else {
      actualPrefix = "shadow";
    }
  } else if (isMaskPass_) {
    // マスクパス中は PS だけ「白を返す」ものへ振り替える。
    // ルートシグネチャ・入力レイアウト・VS は object3d 系と同じなので、
    // 呼び出し側は通常の DrawModel をそのまま使える。
    //
    // 振り替えられるのは object3d 系だけ。スカイボックスや天球などは
    // ルートシグネチャも入力レイアウトも違うので、振り替えると描画が壊れる。
    // マスクパスでは描かせない（nullptr を返すと呼び出し側が描画をスキップする）。
    if (prefix.find("object3d") == std::string_view::npos) {
      return nullptr;
    }
    if (prefix.find("skin") != std::string_view::npos) {
      actualPrefix = "mask_skin";
    } else if (prefix.find("inst") != std::string_view::npos) {
      actualPrefix = "mask_inst";
    } else {
      actualPrefix = "mask";
    }
  }

  auto *pso = GetPipeline(actualPrefix, currentBlendMode_);
  if (!pso) {
    return nullptr;
  }

  cl_->SetGraphicsRootSignature(pso->Root());
  cl_->SetPipelineState(pso->PSO());

  // シャドウマップ・スポット影アトラス（またはHazard回避用ダミー）のバインド
  //
  // "mask" も対象に含める。マスク用 PSO は object3d と同じルートシグネチャで、
  // VS が b6(ShadowParams) を読むため、載せずに描くとルートCBV未設定になる。
  if (actualPrefix.find("object3d") != std::string_view::npos ||
      actualPrefix.find("shadow") != std::string_view::npos ||
      actualPrefix.find("mask") != std::string_view::npos) {
    // Object3DSkin ルートシグネチャだけ 13 番が SkinMatrices(SRV) なので、後続のスロットが 1 つずれる
    const bool isSkinRoot = (actualPrefix.find("skin") != std::string_view::npos);

    D3D12_GPU_DESCRIPTOR_HANDLE shadowSrv = {};
    D3D12_GPU_DESCRIPTOR_HANDLE spotShadowSrv = {};
    if (isShadowPass_ || spotShadowAtlasOpen_) {
      // 深度書き込み中のリソースを SRV として載せないようダミーを入れる
      // （アトラスは Begin〜EndSpotShadowAtlas の間ずっと DEPTH_WRITE）
      //
      // ダミーはロード完了後は変わらないので、一度取れたらキャッシュして
      // ドローごとの TextureManager::LoadID / GetSrv（mutex＋パス正規化＋map 検索）を避ける。
      // ロード完了前は GetSrv が白テクスチャの代替（または ptr==0）を返すので、
      // 「white1x1 自身がロード済み」と判定できるまではキャッシュしない。
      if (dummyWhiteSrv_.ptr == 0) {
        int dummyTex = texMan_.LoadID("Resources/white1x1.png", false);
        if (dummyTex >= 0) {
          const D3D12_GPU_DESCRIPTOR_HANDLE srv = texMan_.GetSrv(dummyTex);
          shadowSrv = srv;
          spotShadowSrv = srv;
          if (srv.ptr != 0 && texMan_.IsLoaded(dummyTex)) {
            dummyWhiteSrv_ = srv;
          }
        }
      } else {
        shadowSrv = dummyWhiteSrv_;
        spotShadowSrv = dummyWhiteSrv_;
      }
      if (!isShadowPass_ && shadowMap_.GetResource() != nullptr && ctxRef_ && ctxRef_->core) {
        shadowSrv = ctxRef_->core->SRV().GPUAt(shadowMap_.GetSrvIndex());
      }
    } else if (ctxRef_ && ctxRef_->core) {
      if (shadowMap_.GetResource() != nullptr) {
        shadowSrv = ctxRef_->core->SRV().GPUAt(shadowMap_.GetSrvIndex());
      }
      if (spotShadowAtlas_.GetResource() != nullptr) {
        spotShadowSrv = ctxRef_->core->SRV().GPUAt(spotShadowAtlas_.GetSrvIndex());
      }
    }

    if (shadowSrv.ptr != 0) {
      // GraphicsPipeline のルートシグネチャ定義を統一したため、Skinning でも 11 番スロットに ShadowMap が来る
      UINT srvSlot = 11;
      cl_->SetGraphicsRootDescriptorTable(srvSlot, shadowSrv);
    }
    if (spotShadowSrv.ptr != 0) {
      // 13: t5 SpotShadowAtlas（Skin は 13 が SkinMatrices なので 14）
      UINT spotSrvSlot = isSkinRoot ? 14 : 13;
      cl_->SetGraphicsRootDescriptorTable(spotSrvSlot, spotShadowSrv);
    }

    // 12: b6 ShadowParams（3 種のルートシグネチャすべて 12 番。
    //     スポット影タイル描画中はそのタイルのライト行列が入ったスライスに差し替わる）
    if (shadowCBBoundAddr_ != 0) {
      cl_->SetGraphicsRootConstantBufferView(12, shadowCBBoundAddr_);
    }

    // 14: b7 SpotShadowCB（Skin は 15）
    if (spotShadowCBAddr_ != 0) {
      UINT spotCbvSlot = isSkinRoot ? 15 : 14;
      cl_->SetGraphicsRootConstantBufferView(spotCbvSlot, spotShadowCBAddr_);
    }
  } else if (actualPrefix == "water") {
    // 水面にも平行光源の影を落とす。
    // Water ルートシグネチャは b6 / t4 を WaterParams / 波紋ハイトマップに使っているので、
    // 影は 15: b7 (ShadowParams) / 16: t7 (ShadowMap) に載せる（GraphicsPipeline 参照）。
    // 水面はシャドウパス・スポット影アトラスの最中には描かれない（その間 "water" は
    // "shadow" へ振り替わる）ので、ここでのシャドウマップは常に SRV 状態。
    if (shadowCB_) {
      cl_->SetGraphicsRootConstantBufferView(15, shadowCB_->GetGPUVirtualAddress());
    }
    D3D12_GPU_DESCRIPTOR_HANDLE shadowSrv = {};
    if (shadowMap_.GetResource() != nullptr && ctxRef_ && ctxRef_->core) {
      shadowSrv = ctxRef_->core->SRV().GPUAt(shadowMap_.GetSrvIndex());
    } else {
      // シャドウマップが無い場合もテーブルを空にしない（shadowMapEnabled = 0 なので読まれない）
      shadowSrv = dummyWhiteSrv_;
    }
    if (shadowSrv.ptr != 0) {
      cl_->SetGraphicsRootDescriptorTable(16, shadowSrv);
    }
  }

  return pso;
}

void RenderContext::BindCameraCB() {
  if (!cl_ || !cameraCB_) {
    return;
  }
  cl_->SetGraphicsRootConstantBufferView(4,
                                         cameraCB_->GetGPUVirtualAddress());
}

void RenderContext::SyncLightCBs() {
  // 3 種のライト CB は各 1 本の固定バッファ。GPU が読むのはコマンドリスト実行時なので、
  // 「描画パスの先頭で 1 回」書けばドローごとに書き直すのと同じ結果になる
  ptLightMan_.SyncCB();
  spLightMan_.SyncCB();
  arLightMan_.SyncCB();
}

void RenderContext::BindAllLightCBs() {
  if (!cl_) {
    return;
  }

  // ※ ここではバインドのみ行う。CPU→GPU の転送は SyncLightCBs()（Execute3DCommands の先頭）で 1 回
  // PointLightCB → RootParam[5]
  if (const D3D12_GPU_VIRTUAL_ADDRESS addr = ptLightMan_.GetCBAddress()) {
    cl_->SetGraphicsRootConstantBufferView(5, addr);
  }
  // SpotLightCB → RootParam[6]
  if (const D3D12_GPU_VIRTUAL_ADDRESS addr = spLightMan_.GetCBAddress()) {
    cl_->SetGraphicsRootConstantBufferView(6, addr);
  }
  // AreaLightCB → RootParam[7]
  if (const D3D12_GPU_VIRTUAL_ADDRESS addr = arLightMan_.GetCBAddress()) {
    cl_->SetGraphicsRootConstantBufferView(7, addr);
  }

  // EnvironmentMap → RootParam[8]
  BindEnvironmentMap();
}

void RenderContext::BindEnvironmentMap() {
  if (!cl_ || environmentMapSrv_.ptr == 0) {
    return;
  }
  cl_->SetGraphicsRootDescriptorTable(8, environmentMapSrv_);
}

void RenderContext::UpdateShadowParams(const ShadowParams& params) {
  // カリング用に控えておく（マップ済みのアップロードヒープは読み出しが遅いので、引数側から取る）
  dirShadowViewProj_ = params.lightViewProjection;
  if (shadowCBMapped_) {
    *shadowCBMapped_ = params;

    // PCF のタップ間隔はシャドウマップの実解像度から求めるため、
    // 呼び出し側の値に関わらずここで上書きする（シーン側が解像度を知る必要をなくす）
    const uint32_t w = shadowMap_.GetWidth();
    const uint32_t h = shadowMap_.GetHeight();
    shadowCBMapped_->shadowMapTexelSize = {
        (w > 0) ? 1.0f / static_cast<float>(w) : 0.0f,
        (h > 0) ? 1.0f / static_cast<float>(h) : 0.0f};

    // 確認用の上書き（C-04）。
    // シーン側は毎フレーム pcfRadius / bias をハードコード値で作り直すので、
    // 外から効かせるにはテクセルサイズと同じくここで塗り替えるしかない。
    // enabled が false のあいだは一切触らないため、通常動作は変わらない。
    if (shadowDebug_.enabled) {
      shadowCBMapped_->pcfRadius = shadowDebug_.pcfRadius;
      shadowCBMapped_->bias = shadowDebug_.bias;
      shadowCBMapped_->color.w = shadowDebug_.darkness;
      if (shadowDebug_.forceDisable) {
        shadowCBMapped_->shadowMapEnabled = 0u;
      }
    }
  }
}

void RenderContext::BindShadow() {
  // BindPipeline 側でバインドするようになったため、ここでは何もしません。
}

void RenderContext::BeginShadowPass() {
  if (!cl_) return;

  // Viewport / Scissor をシャドウマップに合わせる
  D3D12_VIEWPORT viewport{};
  viewport.TopLeftX = 0.0f;
  viewport.TopLeftY = 0.0f;
  viewport.Width = static_cast<float>(shadowMap_.GetWidth());
  viewport.Height = static_cast<float>(shadowMap_.GetHeight());
  viewport.MinDepth = 0.0f;
  viewport.MaxDepth = 1.0f;

  D3D12_RECT scissor{};
  scissor.left = 0;
  scissor.top = 0;
  scissor.right = shadowMap_.GetWidth();
  scissor.bottom = shadowMap_.GetHeight();

  cl_->RSSetViewports(1, &viewport);
  cl_->RSSetScissorRects(1, &scissor);

  shadowMap_.BindAndClear(cl_);
  isShadowPass_ = true;

  // シャドウ用パイプラインをバインド
  auto *pso = GetPipeline("shadow", kBlendModeNone);
  if (pso) {
    cl_->SetGraphicsRootSignature(pso->Root());
    cl_->SetPipelineState(pso->PSO());
    cl_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // Dummy texture binding is now handled inside BindPipeline
  }
}

void RenderContext::EndShadowPass() {
  if (!cl_) return;
  isShadowPass_ = false;
  shadowMap_.TransitionToSRV(cl_);

  if (ctxRef_ && ctxRef_->app) {
    auto rtv = ctxRef_->currentRTV;
    auto dsv = ctxRef_->currentDSV;
    if (rtv.ptr != 0) {
      cl_->OMSetRenderTargets(1, &rtv, FALSE, (dsv.ptr != 0) ? &dsv : nullptr);
    }

    auto w = static_cast<float>(ctxRef_->app->width);
    auto h = static_cast<float>(ctxRef_->app->height);
    D3D12_VIEWPORT vp = {0.0f, 0.0f, w, h, 0.0f, 1.0f};
    D3D12_RECT scissor = {0, 0, static_cast<LONG>(w), static_cast<LONG>(h)};
    cl_->RSSetViewports(1, &vp);
    cl_->RSSetScissorRects(1, &scissor);
  }
}

// ============================================================================
// マスクパス（強調したいオブジェクトのシルエットを白く別RTへ書く）
// ============================================================================

bool RenderContext::BeginMaskPass() {
  if (!cl_ || !ctxRef_ || !ctxRef_->app || !ctxRef_->core) return false;

  const auto w = static_cast<uint32_t>(ctxRef_->app->width);
  const auto h = static_cast<uint32_t>(ctxRef_->app->height);
  if (w == 0 || h == 0) return false;

  // 遅延生成＋リサイズ対応。
  // RenderTexture::Initialize は RTV / SRV ディスクリプタを使い回すので、
  // 作り直しではなく同じインスタンスへ再 Initialize すること（ヒープが漏れる）
  if (!maskTexture_) {
    maskTexture_ = std::make_unique<RenderTexture>();
    maskWidth_ = 0;
    maskHeight_ = 0;
  }
  if (maskWidth_ != w || maskHeight_ != h) {
    // フォーマットはメインのレンダーターゲットと揃える。
    // マスク用 PSO は PipelineManager::Init に渡した rtvFormat（= Dx12Core::Desc の既定値）で
    // 作られているので、ここを変えると PSO と RTV のフォーマット不一致で描画されなくなる。
    maskTexture_->Initialize(ctxRef_->core, w, h, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
                             {0.0f, 0.0f, 0.0f, 0.0f});
    maskWidth_ = w;
    maskHeight_ = h;
  }

  maskTexture_->TransitionToRenderTarget(cl_);

  const float clearColor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  D3D12_CPU_DESCRIPTOR_HANDLE rtv = maskTexture_->GetRTV();
  cl_->ClearRenderTargetView(rtv, clearColor, 0, nullptr);

  // 深度は使わない（マスク用 PSO は enableDepth = false）ので DSV は付けない。
  // DSV ヒープが 1 枚しか無いエンジン構成でも安全で、深度リソースのハザードも起きない。
  cl_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

  D3D12_VIEWPORT viewport = {0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f};
  D3D12_RECT scissor = {0, 0, static_cast<LONG>(w), static_cast<LONG>(h)};
  cl_->RSSetViewports(1, &viewport);
  cl_->RSSetScissorRects(1, &scissor);

  isMaskPass_ = true;
  maskReady_ = false;

  // マスク用パイプラインを先にバインドしておく（シャドウパスと同じ流れ）
  auto *pso = GetPipeline("mask", kBlendModeNone);
  if (pso) {
    cl_->SetGraphicsRootSignature(pso->Root());
    cl_->SetPipelineState(pso->PSO());
    cl_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  }
  return true;
}

void RenderContext::EndMaskPass() {
  // フラグは何があっても必ず降ろす（立ったまま残ると以降の全描画がマスクPSOになる）
  isMaskPass_ = false;
  if (!cl_) return;

  if (maskTexture_) {
    maskTexture_->TransitionToShaderResource(cl_);
    maskReady_ = true;

    // ポストプロセスへ今フレームのマスクを渡す（呼び忘れが起きないようここで結線する）
    if (postProcess_) {
      postProcess_->SetMaskSRV(maskTexture_->GetSRVGPU());
    }
  }

  // メイン描画の描画先・ビューポートへ戻す（EndShadowPass と同じ後始末）
  if (ctxRef_ && ctxRef_->app) {
    auto rtv = ctxRef_->currentRTV;
    auto dsv = ctxRef_->currentDSV;
    if (rtv.ptr != 0) {
      cl_->OMSetRenderTargets(1, &rtv, FALSE, (dsv.ptr != 0) ? &dsv : nullptr);
    }

    auto w = static_cast<float>(ctxRef_->app->width);
    auto h = static_cast<float>(ctxRef_->app->height);
    D3D12_VIEWPORT vp = {0.0f, 0.0f, w, h, 0.0f, 1.0f};
    D3D12_RECT scissor = {0, 0, static_cast<LONG>(w), static_cast<LONG>(h)};
    cl_->RSSetViewports(1, &vp);
    cl_->RSSetScissorRects(1, &scissor);
  }
}

D3D12_GPU_DESCRIPTOR_HANDLE RenderContext::GetMaskSRVGPU() const {
  if (!maskReady_ || !maskTexture_) return {};
  return maskTexture_->GetSRVGPU();
}

// ============================================================================
// Spot Light Shadow（灯ごとの影アトラス）
// ============================================================================

void RenderContext::ApplySpotShadowAtlasInfo_(SpotShadowCB &cb) const {
  // タイル数・タイル解像度・テクセルサイズはアトラスの実寸から決める
  // （シーン側が解像度を知らなくてよいように、呼び出し側の値は無視して上書き）
  const uint32_t w = spotShadowAtlas_.GetWidth();
  const uint32_t h = spotShadowAtlas_.GetHeight();
  cb.tilesX = kSpotShadowTilesX;
  cb.tilesY = kSpotShadowTilesY;
  cb.tileSizePx = (w > 0) ? static_cast<float>(w / kSpotShadowTilesX)
                          : static_cast<float>(kSpotShadowTileSize);
  cb.atlasTexelSize = {(w > 0) ? 1.0f / static_cast<float>(w) : 0.0f,
                       (h > 0) ? 1.0f / static_cast<float>(h) : 0.0f};
}

void RenderContext::UpdateSpotShadowParams(const SpotShadowCB &params) {
  if (!spotShadowCBMapped_) {
    // PreDraw3D より前に呼ばれた（今フレームの一時 CB が未確保）。順序を直すこと
    return;
  }
  *spotShadowCBMapped_ = params;
  spotShadowCBMapped_->count =
      (std::min)(params.count, static_cast<uint32_t>(kMaxSpotShadows));
  ApplySpotShadowAtlasInfo_(*spotShadowCBMapped_);
}

bool RenderContext::BeginSpotShadowAtlas() {
  if (!cl_ || spotShadowAtlas_.GetResource() == nullptr || spotShadowAtlasOpen_) {
    return false;
  }
  // DEPTH_WRITE へ遷移し、全タイルをまとめてクリア（ビューポートも全面になる）
  spotShadowAtlas_.BindAndClear(cl_);
  spotShadowAtlasOpen_ = true;
  currentSpotShadowTile_ = -1;
  return true;
}

void RenderContext::BeginSpotShadowTile(int tileIndex) {
  if (!cl_ || !spotShadowAtlasOpen_) {
    return;
  }
  if (tileIndex < 0 || tileIndex >= static_cast<int>(kMaxSpotShadows) ||
      !spotShadowCBMapped_ || !spotShadowPassParamsMapped_) {
    return;
  }

  // Shadow.VS が gShadowParams.lightViewProjection（b6）で頂点を投影するため、
  // このタイルのライト行列を専用スライスへ書き、BindPipeline が載せるアドレスを差し替える
  auto *slice = reinterpret_cast<ShadowParams *>(
      reinterpret_cast<uint8_t *>(spotShadowPassParamsMapped_) +
      static_cast<size_t>(kShadowParamsSliceStride) * static_cast<size_t>(tileIndex));
  *slice = ShadowParams{};
  slice->lightViewProjection = spotShadowCBMapped_->entries[tileIndex].lightViewProjection;
  spotShadowTileViewProj_ = slice->lightViewProjection; // カリング用の控え
  slice->shadowMapEnabled = 0u;
  shadowCBBoundAddr_ = spotShadowPassParamsAddr_ +
                       static_cast<D3D12_GPU_VIRTUAL_ADDRESS>(kShadowParamsSliceStride) *
                           static_cast<D3D12_GPU_VIRTUAL_ADDRESS>(tileIndex);

  // ビューポート / シザーをこのタイルの矩形へ
  const uint32_t tileW = spotShadowAtlas_.GetWidth() / kSpotShadowTilesX;
  const uint32_t tileH = spotShadowAtlas_.GetHeight() / kSpotShadowTilesY;
  const uint32_t tx = static_cast<uint32_t>(tileIndex) % kSpotShadowTilesX;
  const uint32_t ty = static_cast<uint32_t>(tileIndex) / kSpotShadowTilesX;

  D3D12_VIEWPORT viewport{};
  viewport.TopLeftX = static_cast<float>(tx * tileW);
  viewport.TopLeftY = static_cast<float>(ty * tileH);
  viewport.Width = static_cast<float>(tileW);
  viewport.Height = static_cast<float>(tileH);
  viewport.MinDepth = 0.0f;
  viewport.MaxDepth = 1.0f;

  D3D12_RECT scissor{};
  scissor.left = static_cast<LONG>(tx * tileW);
  scissor.top = static_cast<LONG>(ty * tileH);
  scissor.right = static_cast<LONG>((tx + 1) * tileW);
  scissor.bottom = static_cast<LONG>((ty + 1) * tileH);

  cl_->RSSetViewports(1, &viewport);
  cl_->RSSetScissorRects(1, &scissor);

  isShadowPass_ = true;
  currentSpotShadowTile_ = tileIndex;

  // シャドウ用パイプラインをバインド（各 Draw が BindPipeline 経由で shadow_* に振り替える）
  if (auto *pso = GetPipeline("shadow", kBlendModeNone)) {
    cl_->SetGraphicsRootSignature(pso->Root());
    cl_->SetPipelineState(pso->PSO());
    cl_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  }
}

void RenderContext::EndSpotShadowTile() {
  isShadowPass_ = false;
  currentSpotShadowTile_ = -1;
  // b6 を通常の（平行光源用）ShadowParams に戻す
  shadowCBBoundAddr_ = shadowCB_ ? shadowCB_->GetGPUVirtualAddress() : 0;
}

void RenderContext::EndSpotShadowAtlas() {
  if (!cl_ || !spotShadowAtlasOpen_) {
    return;
  }
  if (currentSpotShadowTile_ >= 0) {
    EndSpotShadowTile();
  }
  isShadowPass_ = false;
  spotShadowAtlas_.TransitionToSRV(cl_);
  spotShadowAtlasOpen_ = false;

  // メイン描画用の RTV / DSV / ビューポートへ戻す（EndShadowPass と同じ）
  if (ctxRef_ && ctxRef_->app) {
    auto rtv = ctxRef_->currentRTV;
    auto dsv = ctxRef_->currentDSV;
    if (rtv.ptr != 0) {
      cl_->OMSetRenderTargets(1, &rtv, FALSE, (dsv.ptr != 0) ? &dsv : nullptr);
    }

    auto w = static_cast<float>(ctxRef_->app->width);
    auto h = static_cast<float>(ctxRef_->app->height);
    D3D12_VIEWPORT vp = {0.0f, 0.0f, w, h, 0.0f, 1.0f};
    D3D12_RECT scissor = {0, 0, static_cast<LONG>(w), static_cast<LONG>(h)};
    cl_->RSSetViewports(1, &vp);
    cl_->RSSetScissorRects(1, &scissor);
  }
}

// ============================================================================
// Primitive 遅延生成
// ============================================================================

Primitive2D *RenderContext::EnsurePrimitive2D() {
  if (!initialized_ || !device_ || !ctxRef_ || !ctxRef_->app) {
    return nullptr;
  }

  const float w = static_cast<float>(ctxRef_->app->width);
  const float h = static_cast<float>(ctxRef_->app->height);

  if (!prim2D_) {
    prim2D_ = std::make_unique<Primitive2D>();
    prim2D_->Initialize(device_.Get(), w, h);
  } else {
    prim2D_->SetScreenSize(w, h);
  }

  return prim2D_.get();
}

Primitive3D *RenderContext::EnsurePrimitive3D() {
  if (!initialized_ || !device_ || !ctxRef_) {
    return nullptr;
  }

  if (!prim3D_) {
    prim3D_ = std::make_unique<Primitive3D>();
    prim3D_->Initialize(device_.Get());
  }
  return prim3D_.get();
}

// ============================================================================
// Fog CB
// ============================================================================

void RenderContext::UpdateFogCB(float timeSec, float intensity, float scale,
                                float speed, const Vector2 &wind,
                                float feather, float bottomBias) {
  if (fogCBMapped_) {
    fogCBMapped_->timeSec = timeSec;
    fogCBMapped_->intensity = intensity;
    fogCBMapped_->scale = scale;
    fogCBMapped_->speed = speed;
    fogCBMapped_->wind = wind;
    fogCBMapped_->feather = feather;
    fogCBMapped_->bottomBias = bottomBias;
  }
}

void RenderContext::SetFogColor(const Vector4 &color) {
  if (fogCBMapped_) {
    fogCBMapped_->color = color;
  }
}

void RenderContext::PushPrimitive3DCommand(bool depth, uint32_t start,
                                           uint32_t count, uint64_t sortKey) {
  if (count == 0)
    return;

  if (!commandQueue3D_.empty()) {
    auto &last = commandQueue3D_.back();
    if (last.type == RenderCommand3D::Primitive && last.primDepth == depth &&
        last.sortKey == sortKey) {
      last.primCount += count;
      return;
    }
  }

  RenderCommand3D cmd;
  cmd.type = RenderCommand3D::Primitive;
  cmd.sortKey = sortKey;
  cmd.primDepth = depth;
  cmd.primStart = start;
  cmd.primCount = count;
  commandQueue3D_.push_back(std::move(cmd));
}

Matrix4x4 RenderContext::CurrentPassViewProjection() const {
  if (isShadowPass_) {
    return (currentSpotShadowTile_ >= 0) ? spotShadowTileViewProj_ : dirShadowViewProj_;
  }
  return Multiply(view_, proj_);
}

void RenderContext::Execute3DCommands() {
  if (!cl_) {
    return;
  }

  // DrawModelInstanced / DrawModelProxies で溜めた要求を、このパスの視錐台でカリングして
  // バッチ化し、コマンドキューへ積む（下のソートより前に行う）
  instanceBatcher_.Flush(*this, modelProxies_);

  // BindShadow(); // BindPipeline に移行したため不要


  // 最初の1フレームだけ自動でダンプを要求する
  static bool s_firstDump = false;
  if (!s_firstDump) {
    dumpCommandOrder_ = true;
    s_firstDump = true;
  }

  // ジオメトリだけを書くパス（シャドウ・マスク）。
  // これらは専用 PSO へ振り替えて描くため、線・ギズモ等のプリミティブは通してはいけない
  // （トポロジも入力レイアウトも違うので描画が壊れ、さらに Clear で頂点が消えてしまう）。
  const bool geometryOnlyPass = isShadowPass_ || isMaskPass_;

  // ライト CB（Point / Spot / Area）を今パスの内容で 1 回だけ転送する。
  // 以前は各ドローの BindAllLightCBs が毎回全灯分（約 49KB）を書き直していた
  // （影タイル 32 枚 × 全オブジェクトで毎フレーム数千回）。
  SyncLightCBs();

  // 0) ソートキーで安定ソート（sortKey==0 は push 順を維持）
  //    深度／マスクだけを書くパスは描く順番で結果が変わらないので並べ替えない
  //    （stable_sort は呼ぶたびに作業バッファを確保する）
  if (!geometryOnlyPass || dumpCommandOrder_) {
    std::stable_sort(commandQueue3D_.begin(), commandQueue3D_.end(),
                     [](const RenderCommand3D &a, const RenderCommand3D &b) {
                       return a.sortKey < b.sortKey;
                     });
  }

  if (dumpCommandOrder_) {
    Log::Print("[RenderContext] --- Execute3DCommands Order Dump ---");
    for (size_t i = 0; i < commandQueue3D_.size(); ++i) {
      const auto& cmd = commandQueue3D_[i];
      uint8_t layer = static_cast<uint8_t>(cmd.sortKey >> 56);
      uint16_t psoHash = static_cast<uint16_t>((cmd.sortKey >> 40) & 0xFFFF);
      uint16_t texHash = static_cast<uint16_t>((cmd.sortKey >> 24) & 0xFFFF);
      uint32_t depth24 = static_cast<uint32_t>(cmd.sortKey & 0x00FFFFFF);
      std::string layerStr = (layer == 0) ? "Opaque" : (layer == 1) ? "Translucent" : (layer == 2) ? "Glass" : (layer == 3) ? "Overlay" : "Unknown";

      if (cmd.type == RenderCommand3D::Primitive) {
        Log::Print(std::format("  [{}] Primitive (Depth:{}) - Layer: {}({}), Depth24: {}, PSO: {:04X}, Tex: {:04X}",
            i, cmd.primDepth, layer, layerStr, depth24, psoHash, texHash));
      } else {
        std::string displayName = cmd.debugName ? cmd.debugName : "Unknown";
        if (cmd.debugIndex >= 0) {
          std::string resourceName = "";
          if (displayName.find("Model") != std::string::npos) {
            if (auto* m = modelMan_.Get(cmd.debugIndex)) {
              resourceName = std::filesystem::path(m->GetFilePath()).filename().string();
            }
          } else if (displayName.find("Sprite") != std::string::npos) {
            if (auto* s = spriteMan_.Get(cmd.debugIndex)) {
              resourceName = std::filesystem::path(s->GetFilePath()).filename().string();
            }
          }
          if (!resourceName.empty()) {
            displayName += " [" + resourceName + "]";
          }

          if (cmd.sortKey == 0) {
            Log::Print(std::format("  [{}] {} [{}]", i, displayName, cmd.debugIndex));
          } else {
            Log::Print(std::format("  [{}] {} [{}] - Layer: {}({}), Depth24: {}, PSO: {:04X}, Tex: {:04X}",
              i, displayName, cmd.debugIndex, layer, layerStr, depth24, psoHash, texHash));
          }
        } else {
          if (cmd.sortKey == 0) {
            Log::Print(std::format("  [{}] {}", i, displayName));
          } else {
            Log::Print(std::format("  [{}] {} - Layer: {}({}), Depth24: {}, PSO: {:04X}, Tex: {:04X}",
              i, displayName, layer, layerStr, depth24, psoHash, texHash));
          }
        }
      }
    }
    Log::Print("[RenderContext] ------------------------------------");
    dumpCommandOrder_ = false;
  }

  // 1) プリミティブの頂点転送 (メインパスのみ)
  if (prim3D_ && prim3D_->HasAny() && !geometryOnlyPass) {
    prim3D_->TransferVertices();
  }

  // 持ち越し先はメンバを使い回す（毎パス new/delete しない）
  deferredCommands3D_.clear();

  // 2) キューに積まれたコマンドを順に実行
  for (auto &cmd : commandQueue3D_) {
    // シャドウ／マスクパス中はプリミティブ描画（デバッグ線など）をスキップし、メインパスへ持ち越す
    if (cmd.type == RenderCommand3D::Primitive && geometryOnlyPass) {
      deferredCommands3D_.push_back(std::move(cmd));
      continue;
    }

    // 履歴に追加
    if (cmd.type == RenderCommand3D::Primitive) {
      AddCommandHistory("Primitive3D", -1, cmd.sortKey);
    } else {
      AddCommandHistory(cmd.debugName ? cmd.debugName : "Unknown", cmd.debugIndex, cmd.sortKey);
    }

    if (cmd.type == RenderCommand3D::Primitive) {
      if (prim3D_) {
        const std::string_view psoName = cmd.primDepth ? "primitive3d" : "primitive3d_nodepth";
        if (BindPipeline(psoName)) {
          prim3D_->DrawRange(cl_, cmd.primDepth, cmd.primStart, cmd.primCount);
        }
      }
    } else if (cmd.func) {
      cmd.func(cl_);
    }
  }

  // 3) 実行後の後始末
  //    実行済みのコマンド（std::function）はここで破棄し、持ち越し分だけをキューへ戻す。
  //    swap で両方のバッファ容量を保持する（次パスの push_back で再確保しない）
  commandQueue3D_.swap(deferredCommands3D_);
  deferredCommands3D_.clear();
  if (prim3D_ && !geometryOnlyPass) {
    prim3D_->Clear();
  }
}

void RenderContext::ExecuteOverlay3DCommands() {
  if (!cl_ || (commandQueue3D_.empty() && !instanceBatcher_.HasPending())) {
    return;
  }

  // 2D の状態（正射影ビューポート・スプライトPSO）から一旦 3D を実行する。
  // 各コマンドは自前で BindPipeline / BindCameraCB / BindAllLightCBs を行うため、
  // ここで 3D 用のバインドを張り直す必要はない。
  const BlendMode prevBlend = currentBlendMode_;
  Execute3DCommands();

  // 2D 用のステートへ戻す。
  // 以降に DrawSprite / DrawString が呼ばれても壊れないようにするための保険。
  currentBlendMode_ = prevBlend;
  if (auto *pso = GetPipeline("sprite", currentBlendMode_)) {
    cl_->SetGraphicsRootSignature(pso->Root());
    cl_->SetPipelineState(pso->PSO());
    cl_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  }
}

void RenderContext::AddLoadingTask(std::future<void> &&task) {
  std::lock_guard<std::mutex> lock(mtxTasks_);
  ongoingTasks_.push_back(std::move(task));
}

void RenderContext::WaitAllLoads() {
  std::vector<std::future<void>> tasks;
  {
    std::lock_guard<std::mutex> lock(mtxTasks_);
    tasks = std::move(ongoingTasks_);
  }

  for (auto &f : tasks) {
    if (f.valid()) {
      f.get();
    }
  }
}

} // namespace RC
