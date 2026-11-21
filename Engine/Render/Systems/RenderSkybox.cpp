#include "RenderCommon.h"
#include "RenderContext.h"
#include "Skybox/Skybox.h"
#include "Skybox/SkyboxManager.h"

namespace RC {

int CreateSkyBox(const std::string &ddsPath) {
  auto &ctx = GetRenderContext();
  return ctx.SkyBoxes().Create(ddsPath);
}

void DrawSkyBox(int skyboxHandle) {
  auto &ctx = GetRenderContext();
  auto *skybox = ctx.SkyBoxes().Get(skyboxHandle);
  if (!skybox || !skybox->Visible())
    return;

  // Skybox 用の WVP 行列を計算する
  // 平行移動を 0 にした View 行列を使う（カメラが動いてもスカイボックスとの位置関係は変わらない）
  Matrix4x4 view = ctx.View();
  // View 行列の平行移動成分をゼロにする（回転だけ適用）
  view.m[3][0] = 0.0f;
  view.m[3][1] = 0.0f;
  view.m[3][2] = 0.0f;

  Matrix4x4 proj = ctx.Proj();
  Matrix4x4 world = MakeAffineMatrix(skybox->T().scale, skybox->T().rotation,
                                     skybox->T().translation);
  Matrix4x4 vp = Multiply(view, proj);
  Matrix4x4 wvp = Multiply(world, vp);

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
