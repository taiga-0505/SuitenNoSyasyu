#include "RenderCommon.h"
#include "RenderContext.h"
#include "Mesh/PrimitiveMesh.h"
#include "Mesh/MeshGenerator.h"

namespace RC {

namespace {

/// @brief アクティブな DirectionalLight のライティングモードをメッシュに同期する
/// @param ctx レンダーコンテキスト
/// @param m   対象のプリミティブメッシュ
/// @note 個別オーバーライド（SetLightingMode）が設定されているメッシュはその値に固定し、ライトに追従させない。
static void ApplyDirLightMode_(RenderContext &ctx, PrimitiveMesh *m) {
  Material *mat = m->Mat();
  if (!mat) {
    return;
  }
  const int ovr = m->GetLightingModeOverride();
  if (ovr >= 0) {
    mat->lightingMode = ovr; // 個別固定：ライトに追従しない
    return;
  }
  if (ctx.DirLights().GetActiveCBAddress() != 0) {
    if (const auto *active = ctx.DirLights().GetActive()) {
      mat->lightingMode = active->GetLightingMode();
    }
  }
}

} // namespace

int GeneratePlane(float width, float height, int texHandle) {
  auto &ctx = GetRenderContext();
  ModelData data = MeshGenerator::GeneratePlane(width, height);
  return ctx.PrimitiveMeshes().Create(data, texHandle, "Plane");
}

int GenerateBox(float width, float height, float depth, int texHandle) {
  auto &ctx = GetRenderContext();
  ModelData data = MeshGenerator::GenerateBox(width, height, depth);
  return ctx.PrimitiveMeshes().Create(data, texHandle, "Box");
}

int GenerateSphere(float radius, int texHandle) {
  auto &ctx = GetRenderContext();
  ModelData data = MeshGenerator::GenerateSphere(radius);
  return ctx.PrimitiveMeshes().Create(data, texHandle, "Sphere");
}

int GenerateCylinder(float radius, float height, int texHandle) {
  auto &ctx = GetRenderContext();
  ModelData data = MeshGenerator::GenerateCylinder(radius, height);
  return ctx.PrimitiveMeshes().Create(data, texHandle, "Cylinder");
}

int GenerateCone(float radius, float height, int texHandle) {
  auto &ctx = GetRenderContext();
  ModelData data = MeshGenerator::GenerateCone(radius, height);
  return ctx.PrimitiveMeshes().Create(data, texHandle, "Cone");
}

int GenerateTorus(float majorRadius, float minorRadius, int texHandle) {
  auto &ctx = GetRenderContext();
  ModelData data = MeshGenerator::GenerateTorus(majorRadius, minorRadius);
  return ctx.PrimitiveMeshes().Create(data, texHandle, "Torus");
}

int GenerateCapsule(float radius, float height, int texHandle) {
  auto &ctx = GetRenderContext();
  ModelData data = MeshGenerator::GenerateCapsule(radius, height);
  return ctx.PrimitiveMeshes().Create(data, texHandle, "Capsule");
}

int GenerateCircle(float radius, uint32_t segments, int texHandle) {
  auto &ctx = GetRenderContext();
  ModelData data = MeshGenerator::GenerateCircle(radius, segments);
  return ctx.PrimitiveMeshes().Create(data, texHandle, "Circle");
}

int GenerateRing(float innerRadius, float outerRadius, uint32_t segments,
                 int texHandle) {
  auto &ctx = GetRenderContext();
  ModelData data =
      MeshGenerator::GenerateRing(innerRadius, outerRadius, segments);
  return ctx.PrimitiveMeshes().Create(data, texHandle, "Ring");
}

int GenerateRingEx(float innerRadius, float outerRadius, uint32_t segments,
                   float startAngle, float endAngle, bool isVerticalUV,
                   bool isXY, int texHandle) {
  auto &ctx = GetRenderContext();
  // innerColor / outerColor は VertexData に頂点カラーが無いため使用しない
  // （MeshGenerator 側も UV に押し出す実装になっている）
  ModelData data = MeshGenerator::GenerateRingEx(
      innerRadius, outerRadius, segments, {1, 1, 1, 1}, {1, 1, 1, 1},
      startAngle, endAngle, isVerticalUV, isXY);
  return ctx.PrimitiveMeshes().Create(data, texHandle, "RingEx");
}

int GenerateEffectCylinder(float topRadius, float bottomRadius, float height,
                           uint32_t segments, float startAngle, float endAngle,
                           bool isVerticalUV, bool flipV, int texHandle) {
  auto &ctx = GetRenderContext();
  ModelData data = MeshGenerator::GenerateEffectCylinder(
      topRadius, bottomRadius, height, segments, startAngle, endAngle,
      isVerticalUV, flipV);
  return ctx.PrimitiveMeshes().Create(data, texHandle, "EffectCylinder");
}

int GenerateTextMesh(const TextMeshDesc &desc, int texHandle,
                     TextMeshInfo *outInfo) {
  auto &ctx = GetRenderContext();
  ModelData data;
  if (!TextMeshGenerator::Generate(desc, data, outInfo)) {
    return -1;
  }
  return ctx.PrimitiveMeshes().Create(data, texHandle, "TextMesh");
}

int GenerateTextMeshOutline(const TextMeshDesc &desc, float outlineWidth,
                            const Vector4 &color, bool unlit,
                            TextMeshInfo *outInfo) {
  auto &ctx = GetRenderContext();
  ModelData data;
  if (!TextMeshGenerator::GenerateOutline(desc, outlineWidth, data, outInfo)) {
    return -1;
  }
  const int handle = ctx.PrimitiveMeshes().Create(data, -1, "TextMeshOutline");
  if (auto *m = ctx.PrimitiveMeshes().Get(handle)) {
    if (Material *mat = m->Mat()) {
      mat->color = color;
    }
    if (unlit) {
      m->SetLightingMode(LightingMode::None);
    }
  }
  return handle;
}

void DrawPrimitiveMesh(int meshHandle, int texHandle) {
  auto &ctx = GetRenderContext();
  if (!ctx.IsInitialized()) return;

  auto *m = ctx.PrimitiveMeshes().Get(meshHandle);
  if (!m) return;

  ApplyDirLightMode_(ctx, m);

  Matrix4x4 world = MakeAffineMatrix(m->T().scale, m->T().rotation, m->T().translation);
  D3D12_GPU_VIRTUAL_ADDRESS lightAddr = ctx.DirLights().GetActiveCBAddress();
  BlendMode blend = ctx.CurrentBlendMode();

  auto layer = (blend == kBlendModeNone || blend == kBlendModeNormal) ? SortKey::kLayerOpaque : SortKey::kLayerTranslucent;
  const uint64_t key = SortKey::Make(layer, SortKey::HashPSO("object3d"), 0);
  ctx.PushCommand3D(key, [m, meshHandle, world, texHandle, lightAddr, blend](ID3D12GraphicsCommandList *cl) {
    auto &ctx = GetRenderContext();
    auto prevBlend = ctx.CurrentBlendMode();
    ctx.SetBlendMode(blend);

    ViewShadingMode shadingMode = ctx.GetViewShadingMode();

    // デバッグシェーディングモードの判定
    bool isDebug = (shadingMode == ViewShadingMode::FaceOrientation ||
                    shadingMode == ViewShadingMode::RandomColor ||
                    shadingMode == ViewShadingMode::SolidShading);

    if (isDebug) {
      // デバッグモード: 専用 PSO で描画（BlendMode = None 固定）
      auto savedBlend = ctx.CurrentBlendMode();
      ctx.SetBlendMode(kBlendModeNone);

      std::string_view prefix = "object3d";
      switch (shadingMode) {
      case ViewShadingMode::FaceOrientation: prefix = "object3d_faceori"; break;
      case ViewShadingMode::RandomColor:     prefix = "object3d_randcolor"; break;
      case ViewShadingMode::SolidShading:    prefix = "object3d_solid"; break;
      default: break;
      }

      if (ctx.BindPipeline(prefix)) {
        ctx.BindCameraCB();
        ctx.BindAllLightCBs();
        ctx.PrimitiveMeshes().ApplyTexture(meshHandle, texHandle);
        m->Draw(cl, world);
      }

      ctx.SetBlendMode(savedBlend);
    } else {
      if (shadingMode != ViewShadingMode::Wireframe) {
        if (ctx.BindPipeline("object3d")) {
          ctx.BindCameraCB();
          cl->SetGraphicsRootConstantBufferView(3, lightAddr);
          ctx.BindAllLightCBs();

          ctx.PrimitiveMeshes().ApplyTexture(meshHandle, texHandle);
          m->Draw(cl, world);
        }
      }
      if (shadingMode == ViewShadingMode::Wireframe || shadingMode == ViewShadingMode::SolidWireframe) {
        if (ctx.BindPipeline("object3d_wire")) {
          ctx.BindCameraCB();
          cl->SetGraphicsRootConstantBufferView(3, lightAddr); // object3d 用レイアウトなので必要
          ctx.BindAllLightCBs();

          ctx.PrimitiveMeshes().ApplyTexture(meshHandle, texHandle);
          m->Draw(cl, world);
        }
      }
    }
    ctx.SetBlendMode(prevBlend);
  }, "PrimitiveMesh", meshHandle);
}

void DrawPrimitiveMeshWater(int meshHandle, int texHandle) {
  auto &ctx = GetRenderContext();
  if (!ctx.IsInitialized()) return;

  auto *m = ctx.PrimitiveMeshes().Get(meshHandle);
  if (!m) return;

  ApplyDirLightMode_(ctx, m);

  Matrix4x4 world = MakeAffineMatrix(m->T().scale, m->T().rotation, m->T().translation);
  D3D12_GPU_VIRTUAL_ADDRESS lightAddr = ctx.DirLights().GetActiveCBAddress();

  // 水面はガラスと同じ半透明レイヤーに積む
  const uint64_t key = SortKey::Make(SortKey::kLayerGlass, SortKey::HashPSO("object3d_water"), 0);

  ctx.PushCommand3D(key, [m, meshHandle, world, texHandle, lightAddr](ID3D12GraphicsCommandList *cl) {
    auto &ctx = GetRenderContext();
    auto prevBlend = ctx.CurrentBlendMode();
    ctx.SetBlendMode(kBlendModePremultiplied);

    ViewShadingMode shadingMode = ctx.GetViewShadingMode();

    bool isDebug = (shadingMode == ViewShadingMode::FaceOrientation ||
                    shadingMode == ViewShadingMode::RandomColor ||
                    shadingMode == ViewShadingMode::SolidShading);

    if (isDebug) {
      auto savedBlend = ctx.CurrentBlendMode();
      ctx.SetBlendMode(kBlendModeNone);

      std::string_view prefix = "object3d";
      switch (shadingMode) {
      case ViewShadingMode::FaceOrientation: prefix = "object3d_faceori"; break;
      case ViewShadingMode::RandomColor:     prefix = "object3d_randcolor"; break;
      case ViewShadingMode::SolidShading:    prefix = "object3d_solid"; break;
      default: break;
      }

      if (ctx.BindPipeline(prefix)) {
        ctx.BindCameraCB();
        ctx.BindAllLightCBs();
        ctx.PrimitiveMeshes().ApplyTexture(meshHandle, texHandle);
        m->Draw(cl, world);
      }
      ctx.SetBlendMode(savedBlend);
    } else {
      // 水面は 2 パス: 先に裏面、次に表面
      if (shadingMode != ViewShadingMode::Wireframe) {
        if (ctx.BindPipeline("object3d_water_front")) {
          ctx.BindCameraCB();
          ctx.BindAllLightCBs();
          ctx.PrimitiveMeshes().ApplyTexture(meshHandle, texHandle);
          m->Draw(cl, world);
        }
        if (ctx.BindPipeline("object3d_water")) {
          ctx.BindCameraCB();
          ctx.BindAllLightCBs();
          ctx.PrimitiveMeshes().ApplyTexture(meshHandle, texHandle);
          m->Draw(cl, world);
        }
      }
      if (shadingMode == ViewShadingMode::Wireframe || shadingMode == ViewShadingMode::SolidWireframe) {
        if (ctx.BindPipeline("object3d_wire")) {
          ctx.BindCameraCB();
          ctx.BindAllLightCBs();
          ctx.PrimitiveMeshes().ApplyTexture(meshHandle, texHandle);
          m->Draw(cl, world);
        }
      }
    }
    ctx.SetBlendMode(prevBlend);
  }, "PrimitiveMesh(Water)", meshHandle);
}

namespace {

/// @brief エフェクト用プリミティブの共通描画
/// @details 加算合成・非ライティングの専用 PSO を直接叩くだけの軽い経路で、
///          ApplyDirLightMode_ は呼ばない（Material の流用スロットを壊さないため）。
/// @param meshHandle メッシュハンドル
/// @param texHandle  差し替えテクスチャ（-1 で生成時のもの）
/// @param prefix     使用する PSO のプレフィックス（文字列リテラルのみ）
/// @param label      デバッグ表示用ラベル（文字列リテラルのみ）
/// @note prefix / label はラムダに生ポインタのまま渡すので、寿命の短い文字列を渡さないこと。
static void DrawPrimitiveMeshEffect_(int meshHandle, int texHandle,
                                     const char *prefix, const char *label) {
  auto &ctx = GetRenderContext();
  if (!ctx.IsInitialized()) return;

  auto *m = ctx.PrimitiveMeshes().Get(meshHandle);
  if (!m) return;

  Matrix4x4 world =
      MakeAffineMatrix(m->T().scale, m->T().rotation, m->T().translation);

  // 加算合成なので最後（ガラスと同じ半透明レイヤー）に流す
  const uint64_t key =
      SortKey::Make(SortKey::kLayerGlass, SortKey::HashPSO(prefix), 0);

  ctx.PushCommand3D(
      key,
      [m, meshHandle, world, texHandle, prefix](ID3D12GraphicsCommandList *cl) {
        auto &ctx = GetRenderContext();
        auto prevBlend = ctx.CurrentBlendMode();
        ctx.SetBlendMode(kBlendModeAdd);

        if (ctx.BindPipeline(prefix)) {
          ctx.BindCameraCB();
          ctx.BindAllLightCBs();
          ctx.PrimitiveMeshes().ApplyTexture(meshHandle, texHandle);
          m->Draw(cl, world);
        }

        ctx.SetBlendMode(prevBlend);
      },
      label, meshHandle);
}

} // namespace

void DrawPrimitiveMeshScanRing(int meshHandle, int texHandle) {
  DrawPrimitiveMeshEffect_(meshHandle, texHandle, "object3d_scanring",
                           "PrimitiveMesh(ScanRing)");
}

void DrawPrimitiveMeshScanBeam(int meshHandle, int texHandle) {
  DrawPrimitiveMeshEffect_(meshHandle, texHandle, "object3d_scanbeam",
                           "PrimitiveMesh(ScanBeam)");
}

void SetPrimitiveMeshEffectParams(int meshHandle, const Vector4 &color,
                                  float progress, float time) {
  auto *m = GetRenderContext().PrimitiveMeshes().Get(meshHandle);
  if (!m) return;
  Material *mat = m->Mat();
  if (!mat) return;

  mat->color = color;
  // エフェクトシェーダー側で progress / time として読む流用スロット
  mat->shininess = progress;
  mat->environmentCoefficient = time;
}

void DrawPrimitiveMeshWaterColumn(int meshHandle, int texHandle) {
  auto &ctx = GetRenderContext();
  if (!ctx.IsInitialized()) return;

  auto *m = ctx.PrimitiveMeshes().Get(meshHandle);
  if (!m) return;

  ApplyDirLightMode_(ctx, m);

  Matrix4x4 world = MakeAffineMatrix(m->T().scale, m->T().rotation, m->T().translation);
  D3D12_GPU_VIRTUAL_ADDRESS lightAddr = ctx.DirLights().GetActiveCBAddress();

  // 水柱もガラスと同じ半透明レイヤーに積む
  const uint64_t key = SortKey::Make(SortKey::kLayerGlass, SortKey::HashPSO("object3d_watercolumn"), 0);

  ctx.PushCommand3D(key, [m, meshHandle, world, texHandle, lightAddr](ID3D12GraphicsCommandList *cl) {
    auto &ctx = GetRenderContext();
    auto prevBlend = ctx.CurrentBlendMode();
    ctx.SetBlendMode(kBlendModePremultiplied);

    ViewShadingMode shadingMode = ctx.GetViewShadingMode();

    bool isDebug = (shadingMode == ViewShadingMode::FaceOrientation ||
                    shadingMode == ViewShadingMode::RandomColor ||
                    shadingMode == ViewShadingMode::SolidShading);

    if (isDebug) {
      auto savedBlend = ctx.CurrentBlendMode();
      ctx.SetBlendMode(kBlendModeNone);

      std::string_view prefix = "object3d";
      switch (shadingMode) {
      case ViewShadingMode::FaceOrientation: prefix = "object3d_faceori"; break;
      case ViewShadingMode::RandomColor:     prefix = "object3d_randcolor"; break;
      case ViewShadingMode::SolidShading:    prefix = "object3d_solid"; break;
      default: break;
      }

      if (ctx.BindPipeline(prefix)) {
        ctx.BindCameraCB();
        ctx.BindAllLightCBs();
        ctx.PrimitiveMeshes().ApplyTexture(meshHandle, texHandle);
        m->Draw(cl, world);
      }
      ctx.SetBlendMode(savedBlend);
    } else {
      // 水柱は 2 パス: 先に裏面、次に表面
      if (shadingMode != ViewShadingMode::Wireframe) {
        if (ctx.BindPipeline("object3d_watercolumn_front")) {
          ctx.BindCameraCB();
          ctx.BindAllLightCBs();
          ctx.PrimitiveMeshes().ApplyTexture(meshHandle, texHandle);
          m->Draw(cl, world);
        }
        if (ctx.BindPipeline("object3d_watercolumn")) {
          ctx.BindCameraCB();
          ctx.BindAllLightCBs();
          ctx.PrimitiveMeshes().ApplyTexture(meshHandle, texHandle);
          m->Draw(cl, world);
        }
      }
      if (shadingMode == ViewShadingMode::Wireframe || shadingMode == ViewShadingMode::SolidWireframe) {
        if (ctx.BindPipeline("object3d_wire")) {
          ctx.BindCameraCB();
          ctx.BindAllLightCBs();
          ctx.PrimitiveMeshes().ApplyTexture(meshHandle, texHandle);
          m->Draw(cl, world);
        }
      }
    }
    ctx.SetBlendMode(prevBlend);
  }, "PrimitiveMesh(WaterColumn)", meshHandle);
}

void UnloadPrimitiveMesh(int meshHandle) {
  GetRenderContext().PrimitiveMeshes().Unload(meshHandle);
}

Transform *GetPrimitiveMeshTransformPtr(int meshHandle) {
  return GetRenderContext().PrimitiveMeshes().GetTransformPtr(meshHandle);
}

void SetPrimitiveMeshEnvironmentCoefficient(int meshHandle, float coeff) {
  auto *m = GetRenderContext().PrimitiveMeshes().Get(meshHandle);
  if (!m) return;
  if (auto* mat = m->Mat()) {
    mat->environmentCoefficient = coeff;
  }
}

Material *GetPrimitiveMeshMaterialPtr(int meshHandle) {
  auto *m = GetRenderContext().PrimitiveMeshes().Get(meshHandle);
  if (!m) return nullptr;
  return m->Mat();
}

void SetPrimitiveMeshLightingMode(int meshHandle, LightingMode mode) {
  auto *m = GetRenderContext().PrimitiveMeshes().Get(meshHandle);
  if (!m) return;
  m->SetLightingMode(mode);
}

void ClearPrimitiveMeshLightingModeOverride(int meshHandle) {
  auto *m = GetRenderContext().PrimitiveMeshes().Get(meshHandle);
  if (!m) return;
  m->ClearLightingModeOverride();
}

void SetPrimitiveMeshNormalMap(int meshHandle, int texHandle) {
  auto &ctx = GetRenderContext();
  auto *m = ctx.PrimitiveMeshes().Get(meshHandle);
  if (!m) return;
  if (texHandle >= 0) {
    m->SetNormalMap(ctx.Textures().GetSrv(texHandle));
  } else {
    m->SetNormalMap(D3D12_GPU_DESCRIPTOR_HANDLE{0});
  }
}

void SetPrimitiveMeshRoughnessMap(int meshHandle, int texHandle) {
  auto &ctx = GetRenderContext();
  auto *m = ctx.PrimitiveMeshes().Get(meshHandle);
  if (!m) return;
  if (texHandle >= 0) {
    m->SetRoughnessMap(ctx.Textures().GetSrv(texHandle));
  } else {
    m->SetRoughnessMap(D3D12_GPU_DESCRIPTOR_HANDLE{0});
  }
}

} // namespace RC
