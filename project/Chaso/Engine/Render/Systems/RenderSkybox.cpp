#include "RenderCommon.h"
#include "RenderContext.h"
#include "Skybox/Skybox.h"
#include "Skybox/SkyboxManager.h"
#include "Skybox/ProceduralSky.h"

namespace RC {

int CreateSkyBox(const std::string &ddsPath) {
  auto &ctx = GetRenderContext();
  return ctx.SkyBoxes().Create(ddsPath);
}

int CreateProceduralSkyBox() {
  auto &ctx = GetRenderContext();
  return ctx.SkyBoxes().CreateProcedural();
}

void DrawSkyBox(int skyboxHandle) {
  auto &ctx = GetRenderContext();
  auto *skybox = ctx.SkyBoxes().Get(skyboxHandle);
  if (!skybox || !skybox->Visible())
    return;

  // プロシージャル空なら、必要に応じてキューブマップを描き直す（3D キューの先頭に積まれる）
  if (ctx.SkyBoxes().IsProcedural(skyboxHandle)) {
    UpdateProceduralSky();
  }

  // ワールド行列は回転・スケールのみ使う。
  // カメラ中心への固定（View の平行移動を落とす処理）は Skybox::Draw 側で行う。
  // translation を入れると方向ベクトルがずれて背景が歪むので無視する。
  Matrix4x4 world = MakeAffineMatrix(skybox->T().scale, skybox->T().rotation,
                                     Vector3{0.0f, 0.0f, 0.0f});

  // Skybox の Cubemap SRV を環境マップとして登録
  D3D12_GPU_DESCRIPTOR_HANDLE envSrv = ctx.SkyBoxes().GetTextureSrv(skyboxHandle);
  if (envSrv.ptr != 0) {
    ctx.SetEnvironmentMap(envSrv);
  }

  // コマンドキューに「PSO 切り替え → 描画 → 元に戻す」を積む
  const uint64_t key = SortKey::Make(SortKey::kLayerOpaque, SortKey::HashPSO("skybox"), 0);
  ctx.PushCommand3D(key, [skyboxHandle, world, envSrv](ID3D12GraphicsCommandList *cl) {
    auto &r = GetRenderContext();
    auto *sb = r.SkyBoxes().Get(skyboxHandle);
    if (!sb)
      return;

    // Skybox PSO をバインド
    auto *pso = r.GetPipeline("skybox", kBlendModeNone);
    if (!pso)
      return;

    cl->SetGraphicsRootSignature(pso->Root());
    cl->SetPipelineState(pso->PSO());
    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // WVP を更新して描画
    if (auto *cbWvp = sb->Mat()) {
      // Mat() は Material を返すので、WVP は Draw 内で直接設定
    }
    sb->Draw(cl, world);

    // 元の Object3D パイプラインに戻す
    r.BindPipeline("object3d");
    r.BindCameraCB();
    r.BindAllLightCBs();
  }, "Skybox", skyboxHandle);
}

void SetEnvironmentMap(int skyboxHandle) {
  auto &ctx = GetRenderContext();
  D3D12_GPU_DESCRIPTOR_HANDLE srv = ctx.SkyBoxes().GetTextureSrv(skyboxHandle);
  ctx.SetEnvironmentMap(srv);
}

void UnloadSkyBox(int skyboxHandle) {
  auto &ctx = GetRenderContext();
  ctx.SkyBoxes().Unload(skyboxHandle);
}

Transform *GetSkyBoxTransformPtr(int skyboxHandle) {
  auto &ctx = GetRenderContext();
  return ctx.SkyBoxes().GetTransformPtr(skyboxHandle);
}

void SetSkyBoxColor(int skyboxHandle, const Vector4 &color) {
  auto &ctx = GetRenderContext();
  ctx.SkyBoxes().SetColor(skyboxHandle, color);
}

} // namespace RC
