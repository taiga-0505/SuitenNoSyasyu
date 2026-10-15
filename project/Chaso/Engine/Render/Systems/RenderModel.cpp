// ============================================================================
// RenderModel.cpp
// ----------------------------------------------------------------------------
// RC::名前空間の Model 描画 API 実装
// ============================================================================

#include "RenderCommon.h"
#include "RenderContext.h"

#include "Model/ModelObject.h"

namespace RC {

// ============================================================================
// ライト適用の共通ヘルパー（ファイルスコープ）
// ============================================================================
namespace {

/// @brief ディレクショナルライトの CB アドレスをモデルに適用し、ライティングモード／shininess をマテリアルに同期する
/// @param ctx 描画コンテキスト
/// @param m   対象モデル
/// @return 適用したライトの CB アドレス（0 = デフォルトを使用）
/// @note モデル個別のライティングモード指定があれば、ライト側のモードで上書きしない。
static D3D12_GPU_VIRTUAL_ADDRESS ApplyDirLight_(RenderContext &ctx,
                                                ModelObject *m) {
  const D3D12_GPU_VIRTUAL_ADDRESS lightAddr =
      ctx.DirLights().GetActiveCBAddress();
  m->SetExternalLightCBAddress(lightAddr);

  if (lightAddr != 0) {
    if (const auto *active = ctx.DirLights().GetActive()) {
      if (Material *mat = m->Mat()) {
        // 個別オーバーライドが設定されているモデルはライトのモードで上書きしない
        const int ovr = m->GetLightingModeOverride();
        mat->lightingMode = (ovr >= 0) ? ovr : active->GetLightingMode();
        mat->shininess = m->GetShininess();
      }
    }
  }
  return lightAddr;
}

/// @brief ガラス用のライト適用（ApplyDirLight_ と同じだが shininess は上書きしない）
/// @param ctx 描画コンテキスト
/// @param m   対象モデル
static void ApplyDirLightGlass_(RenderContext &ctx, ModelObject *m) {
  const D3D12_GPU_VIRTUAL_ADDRESS lightAddr =
      ctx.DirLights().GetActiveCBAddress();
  m->SetExternalLightCBAddress(lightAddr);

  if (lightAddr != 0) {
    if (const auto *active = ctx.DirLights().GetActive()) {
      if (Material *mat = m->Mat()) {
        // 個別オーバーライドが設定されているモデルはライトのモードで上書きしない
        const int ovr = m->GetLightingModeOverride();
        mat->lightingMode = (ovr >= 0) ? ovr : active->GetLightingMode();
      }
    }
  }
}

/// @brief 標準的な 3D バインド（パイプライン＋カメラ CB＋全ライト CB）を行う
/// @param ctx    描画コンテキスト
/// @param prefix パイプラインキーのプレフィックス
/// @return パイプラインをバインドできたら true（失敗時はカメラ・ライトもバインドしない）
static bool BindStandard3D_(RenderContext &ctx, std::string_view prefix) {
  if (!ctx.BindPipeline(prefix)) {
    return false;
  }
  ctx.BindCameraCB();
  ctx.BindAllLightCBs();
  return true;
}

/// @brief テクスチャを差し替える（負のハンドルならマテリアル本来のテクスチャへ戻す）
/// @param ctx       描画コンテキスト
/// @param m         対象モデル
/// @param texHandle 差し替えるテクスチャのハンドル（負なら MTL のテクスチャに戻す）
static void ApplyTexture_(RenderContext &ctx, ModelObject *m, int texHandle) {
  if (texHandle >= 0) {
    m->SetTexture(ctx.Textures().GetSrv(texHandle));
  } else {
    m->ResetTextureToMtl();
  }
}

// ============================================================================
// ViewShadingMode ヘルパー
// ============================================================================

/// @brief デバッグ用のプロシージャルシェーダーモードかを判定する
/// @param mode 表示モード
/// @return FaceOrientation / RandomColor / SolidShading なら true
static bool IsDebugShadingMode_(ViewShadingMode mode) {
  return mode == ViewShadingMode::FaceOrientation ||
         mode == ViewShadingMode::RandomColor ||
         mode == ViewShadingMode::SolidShading;
}

/// @brief ViewShadingMode に応じた単体描画用のパイプラインプレフィックスを返す
/// @param mode    表示モード
/// @param skinned スキニング用パイプラインを使うか
/// @return パイプラインキーのプレフィックス
static std::string_view ResolveSinglePrefix_(ViewShadingMode mode, bool skinned) {
  switch (mode) {
  case ViewShadingMode::FaceOrientation: return skinned ? "object3d_faceori_skin" : "object3d_faceori";
  case ViewShadingMode::RandomColor:     return skinned ? "object3d_randcolor_skin" : "object3d_randcolor";
  case ViewShadingMode::SolidShading:    return skinned ? "object3d_solid_skin" : "object3d_solid";
  default:                               return skinned ? "object3d_skin" : "object3d";
  }
}

/// @brief ViewShadingMode に応じたインスタンシング描画用のパイプラインプレフィックスを返す
/// @param mode 表示モード
/// @return パイプラインキーのプレフィックス
static std::string_view ResolveInstPrefix_(ViewShadingMode mode) {
  switch (mode) {
  case ViewShadingMode::FaceOrientation: return "object3d_faceori_inst";
  case ViewShadingMode::RandomColor:     return "object3d_randcolor_inst";
  case ViewShadingMode::SolidShading:    return "object3d_solid_inst";
  default:                               return "object3d_inst";
  }
}

/// @brief デバッグ表示モードでモデルを 1 体描画する
/// @details デバッグシェーダーは BlendMode = None 固定で、ワイヤーフレームは重ねない。
/// @param ctx     描画コンテキスト
/// @param m       対象モデル
/// @param cl      コマンドリスト
/// @param world   ワールド行列
/// @param mode    表示モード
/// @param skinned スキニング用パイプラインを使うか
static void DrawDebugSingle_(RenderContext &ctx, ModelObject *m,
                              ID3D12GraphicsCommandList *cl,
                              const Matrix4x4 &world,
                              ViewShadingMode mode,
                              bool skinned) {
  auto prevBlend = ctx.CurrentBlendMode();
  ctx.SetBlendMode(kBlendModeNone);

  if (BindStandard3D_(ctx, ResolveSinglePrefix_(mode, skinned))) {
    m->Draw(cl, world, ctx.CurrentFrame(), ctx.IsShadowPass());
  }

  ctx.SetBlendMode(prevBlend);
}

/// @brief デバッグ表示モードでモデルをインスタンシング描画する（色指定付き）
/// @param ctx       描画コンテキスト
/// @param m         対象モデル
/// @param cl        コマンドリスト
/// @param instances 各インスタンスの Transform
/// @param color     全インスタンス共通の色
/// @param mode      表示モード
static void DrawDebugInstColored_(RenderContext &ctx, ModelObject *m,
                                   ID3D12GraphicsCommandList *cl,
                                   const std::vector<Transform> &instances,
                                   const Vector4 &color,
                                   ViewShadingMode mode) {
  auto prevBlend = ctx.CurrentBlendMode();
  ctx.SetBlendMode(kBlendModeNone);

  if (BindStandard3D_(ctx, ResolveInstPrefix_(mode))) {
    m->DrawBatch(cl, ctx.View(), ctx.Proj(), instances, color, ctx.CurrentFrame(), ctx.IsShadowPass());
  }

  ctx.SetBlendMode(prevBlend);
}

} // namespace

// ============================================================================
// 読み込み／解放
// ============================================================================

int LoadModel(const std::string &path) {
  auto &ctx = GetRenderContext();
  if (!ctx.IsInitialized()) {
    return -1;
  }
  return ctx.Models().Load(path);
}

void UnloadModel(int modelHandle) {
  GetRenderContext().Models().Unload(modelHandle);
}

bool IsModelReady(int modelHandle) {
  auto &ctx = GetRenderContext();
  if (!ctx.IsInitialized()) {
    return false;
  }
  auto *m = ctx.Models().Get(modelHandle);
  return (m && m->IsReady());
}

// ============================================================================
// 描画（基本）
// ============================================================================

/// @brief モデルの描画に使うワールド行列を求める
/// @details SetModelWorldOverride で行列が直接指定されている場合はそれを使い、
///          そうでなければ従来通り Transform（TRS）から生成する。
///          ボーン追従（武器を手に持たせる等）はオイラー角では表現しづらいため、
///          行列をそのまま渡せる経路を用意している。
/// @param m 対象モデル
/// @return ワールド行列
static Matrix4x4 ResolveModelWorld_(ModelObject *m) {
  if (m->HasWorldOverride()) {
    return m->WorldOverride();
  }
  return MakeAffineMatrix(m->T().scale, m->T().rotation, m->T().translation);
}

void DrawModel(int modelHandle, int texHandle) {
  auto &ctx = GetRenderContext();
  if (!ctx.IsInitialized()) {
    return;
  }
  auto *m = ctx.Models().Get(modelHandle);
  if (!m || !m->IsReady()) {
    return;
  }

  Matrix4x4 world = ResolveModelWorld_(m);
  D3D12_GPU_VIRTUAL_ADDRESS lightAddr = ctx.DirLights().GetActiveCBAddress();
  BlendMode blend = ctx.CurrentBlendMode();
  const auto *mat = m->Mat();
  const bool isTranslucent = (mat && mat->color.w < 1.0f);
  if (isTranslucent && blend == kBlendModeNone) {
    blend = kBlendModeNormal;
  }

  // ソートキー構築（透明度に応じて Opaque / Translucent を切り替え）
  const auto layer = isTranslucent ? SortKey::kLayerTranslucent : SortKey::kLayerOpaque;
  const uint64_t key = SortKey::Make(layer,
                                     SortKey::HashPSO("object3d"), 0);

  ctx.PushCommand3D(key, [m, world, texHandle, lightAddr, blend](ID3D12GraphicsCommandList *cl) {
    auto &ctx = GetRenderContext();
    auto prevBlend = ctx.CurrentBlendMode();
    ctx.SetBlendMode(blend);

    ApplyTexture_(ctx, m, texHandle);
    m->SetExternalLightCBAddress(lightAddr);
    if (lightAddr != 0) {
      ApplyDirLight_(ctx, m);
    }
    m->Update(ctx.View(), ctx.Proj());

    ViewShadingMode shadingMode = ctx.GetViewShadingMode();

    // スキニングモデルかどうかでパイプラインを分岐
    const bool skinned = m->HasSkinData();
    // CS スキニングが有効なら通常パイプライン（object3d）で描画
    const bool useCSSkinning = skinned && m->Resource().HasCSSkinning();
    const std::string_view pipelinePrefix = (skinned && !useCSSkinning) ? "object3d_skin" : "object3d";

    if (IsDebugShadingMode_(shadingMode)) {
      DrawDebugSingle_(ctx, m, cl, world, shadingMode, skinned && !useCSSkinning);
    } else {
      if (shadingMode != ViewShadingMode::Wireframe) {
        if (BindStandard3D_(ctx, pipelinePrefix)) {
          m->Draw(cl, world, ctx.CurrentFrame(), ctx.IsShadowPass());
        }
      }
      if (shadingMode == ViewShadingMode::Wireframe || shadingMode == ViewShadingMode::SolidWireframe) {
        const std::string_view wirePipeline = (skinned && !useCSSkinning) ? "object3d_wire_skin" : "object3d_wire";
        if (BindStandard3D_(ctx, wirePipeline)) {
          m->Draw(cl, world, ctx.CurrentFrame(), ctx.IsShadowPass());
        }
      }
    }
    ctx.SetBlendMode(prevBlend);
  }, isTranslucent ? "Model(Translucent)" : "Model(Opaque)", modelHandle);
}

void DrawModel(int modelHandle) { DrawModel(modelHandle, -1); }

// ============================================================================
// インスタンス描画（自動バッチ＋視錐台カリング）
// ============================================================================

void DrawModelInstanced(int modelHandle, int texHandle) {
  auto &ctx = GetRenderContext();
  if (!ctx.IsInitialized()) {
    return;
  }
  auto *m = ctx.Models().Get(modelHandle);
  if (!m || !m->IsReady()) {
    return;
  }

  // まとめられない物は従来経路へ:
  //   半透明（奥から順に描く必要がある）・ブレンドモード指定中（PSO が変わる）・
  //   スキニング（ボーン行列がオブジェクトごとに違う）
  const Material *mat = m->Mat();
  const bool translucent = (mat && mat->color.w < 1.0f);
  if (translucent || ctx.CurrentBlendMode() != kBlendModeNone || m->HasSkinData()) {
    DrawModel(modelHandle, texHandle);
    return;
  }

  ctx.InstanceBatcher().Submit(m, modelHandle, texHandle);
}

ModelInstanceBatcher::Stats GetModelInstancingStats() {
  return GetRenderContext().InstanceBatcher().LastFrameStats();
}

// ============================================================================
// VirtualEntity（ModelProxyPool）の操作
// ============================================================================

ModelProxyHandle CreateModelProxy(int modelHandle) {
  return GetRenderContext().ModelProxies().Create(modelHandle);
}

void DestroyModelProxy(ModelProxyHandle h) {
  GetRenderContext().ModelProxies().Destroy(h);
}

bool IsModelProxyAlive(ModelProxyHandle h) {
  return GetRenderContext().ModelProxies().IsAlive(h);
}

void SetModelProxyTransform(ModelProxyHandle h, const Transform &t) {
  GetRenderContext().ModelProxies().SetTransform(h, t);
}

void SetModelProxyWorld(ModelProxyHandle h, const Matrix4x4 &world) {
  GetRenderContext().ModelProxies().SetWorld(h, world);
}

void SetModelProxyColor(ModelProxyHandle h, const Vector4 &color) {
  GetRenderContext().ModelProxies().SetColor(h, color);
}

void SetModelProxyVisible(ModelProxyHandle h, bool visible) {
  GetRenderContext().ModelProxies().SetVisible(h, visible);
}

void SetModelProxyCastShadow(ModelProxyHandle h, bool cast) {
  GetRenderContext().ModelProxies().SetCastShadow(h, cast);
}

uint32_t GetModelProxyCount() {
  return GetRenderContext().ModelProxies().Count();
}

void ClearModelProxies() {
  GetRenderContext().ModelProxies().Clear();
}

void DrawModelProxies() {
  auto &ctx = GetRenderContext();
  if (!ctx.IsInitialized() || ctx.ModelProxies().Count() == 0) {
    return;
  }
  ctx.InstanceBatcher().RequestProxies();
}

// ============================================================================
// バッチ描画
// ============================================================================

void DrawModelBatchColored(int modelHandle,
                           const std::vector<Transform> &instances,
                           const Vector4 &color, int texHandle,
                           bool afterWater) {
  auto &ctx = GetRenderContext();
  if (!ctx.IsInitialized()) {
    return;
  }
  auto *m = ctx.Models().Get(modelHandle);
  if (!m || !m->IsReady()) {
    return;
  }

  D3D12_GPU_VIRTUAL_ADDRESS lightAddr = ctx.DirLights().GetActiveCBAddress();
  BlendMode blend = ctx.CurrentBlendMode();
  const bool isTranslucent = (color.w < 1.0f);
  if (isTranslucent && blend == kBlendModeNone) {
    blend = kBlendModeNormal;
  }

  // afterWater: 水面（Translucent）より後の Glass レイヤーへ回す
  const auto layer = afterWater      ? SortKey::kLayerGlass
                     : isTranslucent ? SortKey::kLayerTranslucent
                                     : SortKey::kLayerOpaque;
  const uint64_t key = SortKey::Make(layer,
                                     SortKey::HashPSO("object3d_inst"), 0);

  ctx.PushCommand3D(key, [m, instances, color, texHandle,
                     lightAddr, blend](ID3D12GraphicsCommandList *cl) {
    auto &ctx = GetRenderContext();
    auto prevBlend = ctx.CurrentBlendMode();
    ctx.SetBlendMode(blend);

    ApplyTexture_(ctx, m, texHandle);
    m->SetExternalLightCBAddress(lightAddr);
    if (lightAddr != 0) {
      ApplyDirLight_(ctx, m);
    }

    ViewShadingMode shadingMode = ctx.GetViewShadingMode();

    if (IsDebugShadingMode_(shadingMode)) {
      DrawDebugInstColored_(ctx, m, cl, instances, color, shadingMode);
    } else {
      if (shadingMode != ViewShadingMode::Wireframe) {
        if (BindStandard3D_(ctx, "object3d_inst")) {
          m->DrawBatch(cl, ctx.View(), ctx.Proj(), instances, color, ctx.CurrentFrame(), ctx.IsShadowPass());
        }
      }
      if (shadingMode == ViewShadingMode::Wireframe || shadingMode == ViewShadingMode::SolidWireframe) {
        if (BindStandard3D_(ctx, "object3d_wire_inst")) {
          m->DrawBatch(cl, ctx.View(), ctx.Proj(), instances, color, ctx.CurrentFrame(), ctx.IsShadowPass());
        }
      }
    }
    ctx.SetBlendMode(prevBlend);
  }, isTranslucent ? "Model(BatchColored_Translucent)" : "Model(BatchColored)", modelHandle);
}

// ============================================================================
// ガラス（2 パス: 背面 → 表面）
// ※ 2 パスは同一パケット内で PSO を切り替える必要があるため、
//    ラムダ方式（PushCommand3D）のまま残す。
// ============================================================================

void DrawModelGlassTwoPass(int modelHandle, int texHandle) {
  auto &ctx = GetRenderContext();
  if (!ctx.IsInitialized()) {
    return;
  }
  auto *m = ctx.Models().Get(modelHandle);
  if (!m || !m->IsReady()) {
    return;
  }

  Matrix4x4 world = ResolveModelWorld_(m);
  D3D12_GPU_VIRTUAL_ADDRESS lightAddr = ctx.DirLights().GetActiveCBAddress();


  const uint64_t key = SortKey::Make(SortKey::kLayerGlass,
                                     SortKey::HashPSO("object3d_glass"), 0);

  ctx.PushCommand3D(key,
      [m, world, texHandle, lightAddr](ID3D12GraphicsCommandList *cl) {
        auto &ctx = GetRenderContext();
        const BlendMode saved = ctx.CurrentBlendMode();
        ctx.SetBlendMode(kBlendModePremultiplied);

        ApplyTexture_(ctx, m, texHandle);
        m->SetExternalLightCBAddress(lightAddr);
        if (lightAddr != 0) {
          ApplyDirLightGlass_(ctx, m);
        }
        m->Update(ctx.View(), ctx.Proj());

        ViewShadingMode shadingMode = ctx.GetViewShadingMode();
        const bool skinned = m->HasSkinData();

        if (IsDebugShadingMode_(shadingMode)) {
          DrawDebugSingle_(ctx, m, cl, world, shadingMode, skinned);
        } else {
          // 1) 背面（内側）。Solid のときだけ
          if (shadingMode != ViewShadingMode::Wireframe) {
            if (BindStandard3D_(ctx, "object3d_glass_front")) {
              m->Draw(cl, world, ctx.CurrentFrame(), ctx.IsShadowPass());
            }
          }
          // 2) 表面（外側）。SolidWireframe ならワイヤーフレームも重ねる
          if (shadingMode != ViewShadingMode::Wireframe) {
            if (BindStandard3D_(ctx, "object3d_glass")) {
              m->Draw(cl, world, ctx.CurrentFrame(), ctx.IsShadowPass());
            }
          }
          if (shadingMode == ViewShadingMode::Wireframe || shadingMode == ViewShadingMode::SolidWireframe) {
            const std::string_view wirePipeline = skinned ? "object3d_wire_skin" : "object3d_wire";
            if (BindStandard3D_(ctx, wirePipeline)) {
              m->Draw(cl, world, ctx.CurrentFrame(), ctx.IsShadowPass());
            }
          }
        }

        ctx.SetBlendMode(saved);
      }, "Model(GlassTwoPass)", modelHandle);
}

// ============================================================================
// Transform・色・ライティングモード・メッシュ・カーソル・アニメーション
// ============================================================================

Transform *GetModelTransformPtr(int modelHandle) {
  return GetRenderContext().Models().GetTransformPtr(modelHandle);
}

void SetModelColor(int modelHandle, const Vector4 &color) {
  GetRenderContext().Models().SetColor(modelHandle, color);
}

void SetModelShininess(int modelHandle, float shininess) {
  GetRenderContext().Models().SetShininess(modelHandle, shininess);
}

void SetModelLightingMode(int modelHandle, LightingMode m) {
  GetRenderContext().Models().SetLightingMode(modelHandle, m);
}

void ClearModelLightingModeOverride(int modelHandle) {
  GetRenderContext().Models().ClearLightingModeOverride(modelHandle);
}

void SetModelMesh(int modelHandle, const std::string &path) {
  GetRenderContext().Models().SetMesh(modelHandle, path);
}

void ResetCursor(int modelHandle) {
  GetRenderContext().Models().ResetCursor(modelHandle);
}

void SetModelEnvironmentCoefficient(int modelHandle, float coeff) {
  auto &ctx = GetRenderContext();
  auto *m = ctx.Models().Get(modelHandle);
  if (!m) return;
  m->SetEnvironmentCoefficient(coeff);
}

void SetModelNormalMap(int modelHandle, int texHandle) {
  auto &ctx = GetRenderContext();
  auto *m = ctx.Models().Get(modelHandle);
  if (!m) return;
  if (texHandle >= 0) {
    m->SetNormalMap(ctx.Textures().GetSrv(texHandle));
  } else {
    m->SetNormalMap(D3D12_GPU_DESCRIPTOR_HANDLE{0});
  }
}

void SetModelRoughnessMap(int modelHandle, int texHandle) {
  auto &ctx = GetRenderContext();
  auto *m = ctx.Models().Get(modelHandle);
  if (!m) return;
  if (texHandle >= 0) {
    m->SetRoughnessMap(ctx.Textures().GetSrv(texHandle));
  } else {
    m->SetRoughnessMap(D3D12_GPU_DESCRIPTOR_HANDLE{0});
  }
}

void AttachModelAnimation(int modelHandle) {
  GetRenderContext().Models().AttachAnimation(modelHandle);
}

void AttachModelAnimation(int modelHandle, const std::string& filePath) {
  GetRenderContext().Models().AttachAnimation(modelHandle, filePath);
}

void AttachModelAnimation(int modelHandle, const std::string& filePath, int animIndex) {
  GetRenderContext().Models().AttachAnimation(modelHandle, filePath, animIndex);
}

void CrossfadeModelAnimation(int modelHandle, const std::string& filePath, float blendDuration) {
  GetRenderContext().Models().CrossfadeAnimation(modelHandle, filePath, blendDuration);
}

void CrossfadeModelAnimation(int modelHandle, const std::string& filePath, int animIndex, float blendDuration) {
  GetRenderContext().Models().CrossfadeAnimation(modelHandle, filePath, animIndex, blendDuration);
}

void UpdateModelAnimation(int modelHandle, float dt) {
  if (dt < 0.0f) {
    if (auto* ctx = GetRenderContext().Ctx()) {
      dt = ctx->deltaTime;
    } else {
      dt = 1.0f / 60.0f; // SceneContext が無いときは 60fps 相当で進める
    }
  }
  GetRenderContext().Models().UpdateAnimation(modelHandle, dt);
}

float GetModelAnimationDuration(int modelHandle) {
  return GetRenderContext().Models().GetAnimationDuration(modelHandle);
}

void DrawModelSkeleton(int modelHandle) {
  auto &ctx = GetRenderContext();
  if (!ctx.IsInitialized()) return;
  auto *m = ctx.Models().Get(modelHandle);
  if (!m) return;

  // 骨格はメッシュの内側にあるので、必ずオーバーレイレイヤー
  // （sortKey = kLayerOverlay / 深度テストなし）で積む必要がある。
  // 素の 3D プリミティブは sortKey == 0 になり、安定ソートでモデルより
  // 前に実行されてしまうため、そのままではモデルに塗り潰されて何も見えない。
  const bool prevOverlay = ctx.IsOverlayMode();
  ctx.SetOverlayMode(true);
  m->DrawSkeleton();
  ctx.SetOverlayMode(prevOverlay);
}

bool HasModelSkinData(int modelHandle) {
  auto &ctx = GetRenderContext();
  auto *m = ctx.Models().Get(modelHandle);
  if (!m) return false;
  return m->HasSkinData();
}

bool HasModelSkeleton(int modelHandle) {
  auto &ctx = GetRenderContext();
  auto *m = ctx.Models().Get(modelHandle);
  if (!m) return false;
  return m->HasSkeleton();
}

// ============================================================================
// ボーン追従（ソケット）
// ============================================================================

bool GetModelJointMatrix(int modelHandle, const std::string &jointName,
                         Matrix4x4 &out) {
  auto &ctx = GetRenderContext();
  auto *m = ctx.Models().Get(modelHandle);
  if (!m) return false;
  return m->TryGetJointMatrix(jointName, out);
}

std::vector<std::string> GetModelJointNames(int modelHandle) {
  std::vector<std::string> names;
  auto &ctx = GetRenderContext();
  auto *m = ctx.Models().Get(modelHandle);
  if (!m || !m->HasSkeleton()) return names;
  const Skeleton &sk = m->GetSkeleton();
  names.reserve(sk.joints.size());
  for (const Joint &j : sk.joints) names.push_back(j.name);
  return names;
}

void SetModelWorldOverride(int modelHandle, const Matrix4x4 &world) {
  auto &ctx = GetRenderContext();
  auto *m = ctx.Models().Get(modelHandle);
  if (!m) return;
  m->SetWorldOverride(world);
}

void ClearModelWorldOverride(int modelHandle) {
  auto &ctx = GetRenderContext();
  auto *m = ctx.Models().Get(modelHandle);
  if (!m) return;
  m->ClearWorldOverride();
}

Material *GetModelMaterialPtr(int modelHandle) {
  auto &ctx = GetRenderContext();
  auto *m = ctx.Models().Get(modelHandle);
  if (!m) return nullptr;
  return m->Mat();
}

} // namespace RC
