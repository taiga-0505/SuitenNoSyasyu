// ============================================================================
// RenderSprite.cpp
// ----------------------------------------------------------------------------
// RC::名前空間の Sprite 描画 API 実装
// ============================================================================

#include "RenderCommon.h"
#include "RenderContext.h"

#include "Common/SceneContext.h"
#include "AppConfig.h" // SceneContext::app の画面サイズを参照する

namespace RC {

int LoadSprite(const std::string &path, SceneContext &ctx, bool srgb) {
  auto &rc = GetRenderContext();
  if (!rc.IsInitialized()) {
    return -1;
  }

  const float screenW = static_cast<float>(ctx.app->width);
  const float screenH = static_cast<float>(ctx.app->height);
  return rc.Sprites().Load(path, screenW, screenH, srgb);
}

void DrawSprite(int spriteHandle) {
  auto &ctx = GetRenderContext();
  if (!ctx.IsInitialized() || !ctx.CL()) {
    return;
  }

  if (!ctx.BindPipeline("sprite")) {
    return;
  }

  ctx.AddCommandHistory("Sprite", spriteHandle);
  ctx.Sprites().Draw(spriteHandle, ctx.CL());
}

void SetSpriteWorldSpace(int spriteHandle, bool enable) {
  auto &ctx = GetRenderContext();
  if (!ctx.IsInitialized()) {
    return;
  }
  ctx.Sprites().SetWorldSpace(spriteHandle, enable);
}

void DrawSprite3D(int spriteHandle) {
  auto &ctx = GetRenderContext();
  if (!ctx.IsInitialized() || !ctx.CL()) {
    return;
  }

  auto *sp = ctx.Sprites().Get(spriteHandle);
  if (!sp) {
    return;
  }

  // このフレームのカメラ行列を焼き込む（実際の Update は実行時に走る）
  sp->SetWorldSpace(true);
  sp->SetCamera(ctx.View(), ctx.Proj());

  // アルファテストで不透明扱いなので Opaque レイヤーに積む。
  // 深度テストが効くので、モデルとの前後関係は自動で解決される。
  const uint64_t key =
      SortKey::Make(SortKey::kLayerOpaque, SortKey::HashPSO("sprite3d"), 0);

  ctx.PushCommand3D(
      key,
      [spriteHandle](ID3D12GraphicsCommandList *cl) {
        auto &c = GetRenderContext();
        auto prevBlend = c.CurrentBlendMode();
        c.SetBlendMode(kBlendModeNormal);
        if (c.BindPipeline("sprite3d")) {
          c.Sprites().Draw(spriteHandle, cl);
        }
        c.SetBlendMode(prevBlend);
      },
      "Sprite3D", spriteHandle);
}

void DrawSpriteRect(int spriteHandle, float srcX, float srcY, float srcW,
                    float srcH, float texW, float texH, float insetPx) {
  auto &ctx = GetRenderContext();
  if (!ctx.IsInitialized() || !ctx.CL()) {
    return;
  }

  if (!ctx.BindPipeline("sprite")) {
    return;
  }

  ctx.AddCommandHistory("SpriteRect", spriteHandle);
  ctx.Sprites().DrawRect(spriteHandle, srcX, srcY, srcW, srcH, texW, texH,
                         insetPx, ctx.CL());
}

void DrawSpriteRectUV(int spriteHandle, float u0, float v0, float u1,
                      float v1) {
  auto &ctx = GetRenderContext();
  if (!ctx.IsInitialized() || !ctx.CL()) {
    return;
  }

  if (!ctx.BindPipeline("sprite")) {
    return;
  }

  ctx.AddCommandHistory("SpriteRectUV", spriteHandle);
  ctx.Sprites().DrawRectUV(spriteHandle, u0, v0, u1, v1, ctx.CL());
}

void SetSpriteTransform(int spriteHandle, const Transform &t) {
  GetRenderContext().Sprites().SetTransform(spriteHandle, t);
}

void SetSpriteColor(int spriteHandle, const Vector4 &color) {
  GetRenderContext().Sprites().SetColor(spriteHandle, color);
}

void UnloadSprite(int spriteHandle) {
  GetRenderContext().Sprites().Unload(spriteHandle);
}

void SetSpriteScreenSize(int spriteHandle, float w, float h) {
  GetRenderContext().Sprites().SetSize(spriteHandle, w, h);
}

} // namespace RC
