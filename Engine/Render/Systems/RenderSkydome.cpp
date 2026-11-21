// ============================================================================
// RenderSkydome.cpp
// ----------------------------------------------------------------------------
// RC::名前空間の Skydome 描画 API 実装
// ============================================================================

#include "RenderCommon.h"
#include "RenderContext.h"
#include "Skydome/Skydome.h"

namespace RC {

int GenerateSkydomeEx(int textureHandle, float radius, unsigned int sliceCount,
                      unsigned int stackCount) {
  auto &ctx = GetRenderContext();
  if (!ctx.IsInitialized() || !ctx.Device()) {
    return -1;
  }

  return ctx.Skydomes().Create(textureHandle, radius, sliceCount, stackCount);
}

int GenerateSkydome(int textureHandle) {
  return GenerateSkydomeEx(textureHandle);
}

void DrawSkydome(int skydomeHandle, int texHandle) {
  auto &ctx = GetRenderContext();
  if (!ctx.IsInitialized()) {
    return;
  }

  auto *s = ctx.Skydomes().Get(skydomeHandle);
  if (!s) {
    return;
  }

  // 状態キャプチャ
  Matrix4x4 world = MakeAffineMatrix(s->T().scale, s->T().rotation, s->T().translation);
  D3D12_GPU_VIRTUAL_ADDRESS lightAddr = ctx.DirLights().GetActiveCBAddress();

  // Skybox と同様、View 行列の平行移動成分を落としてカメラ中心に固定する。
  // 天球は「無限遠の背景」なのでカメラが移動しても追従し、
  // カメラが天球の外に出て背景が欠けることがない。
  // （Transform.translation はカメラからのオフセットとして残る。通常は 0）
  Matrix4x4 view = ctx.View();
  view.m[3][0] = 0.0f;
  view.m[3][1] = 0.0f;
  view.m[3][2] = 0.0f;
  const Matrix4x4 viewProj = Multiply(view, ctx.Proj());

  const uint64_t key = SortKey::Make(SortKey::kLayerOpaque,
                                     SortKey::HashPSO("object3d_skydome"), 0);
  ctx.PushCommand3D(key, [s, skydomeHandle, world, viewProj, texHandle,
                          lightAddr](ID3D12GraphicsCommandList *cl) {
    auto &ctx = GetRenderContext();

    // 天球は不透明な背景なのでブレンドは常に無し
    const BlendMode prevBlend = ctx.CurrentBlendMode();
    ctx.SetBlendMode(kBlendModeNone);

    // 天球専用 PSO（深度書き込み OFF、VS が深度を最遠に固定、カリング無し）。
    // 以前は object3d を流用していたが、深度がそのまま書かれるため
    // 半径 100 の天球がカメラの Far（既定 100）に丸ごとクリップされて描画されなかった。
    if (ctx.BindPipeline("object3d_skydome")) {
      ctx.BindCameraCB();
      // 天球は通常ライトの影響を受けないが、ルートパラメータを埋めるためバインド
      ctx.BindAllLightCBs();

      ctx.Skydomes().ApplyTexture(skydomeHandle, texHandle);
      s->SetExternalLightCBAddress(lightAddr);

      s->Draw(cl, world, viewProj);
    }
    ctx.SetBlendMode(prevBlend);
  }, "Skydome", skydomeHandle);
}


void UnloadSkydome(int skydomeHandle) {
  GetRenderContext().Skydomes().Unload(skydomeHandle);
}

Transform *GetSkydomeTransformPtr(int skydomeHandle) {
  return GetRenderContext().Skydomes().GetTransformPtr(skydomeHandle);
}

void SetSkydomeColor(int skydomeHandle, const Vector4 &color) {
  GetRenderContext().Skydomes().SetColor(skydomeHandle, color);
}

} // namespace RC
