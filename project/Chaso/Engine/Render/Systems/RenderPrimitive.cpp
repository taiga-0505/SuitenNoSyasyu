// ============================================================================
// RenderPrimitive.cpp
// ----------------------------------------------------------------------------
// RC::名前空間の Primitive2D / Primitive3D / Fog API 実装
// ============================================================================

#include "RenderCommon.h"
#include "RenderContext.h"
#include "SortKey.h"

#include "PipelineManager.h"
#include "Primitive/Primitive2D.h"
#include "Primitive/Primitive3D.h"
#include "Common/SceneContext.h"
#include "AppConfig.h" // SceneContext::app の画面サイズを参照する
#include "Common/Log/Log.h"
#include <format>
#include <cmath>

namespace RC {

// ============================================================================
// Primitive2D（即時描画）
// ============================================================================

void DrawLine(const Vector2 &pos1, const Vector2 &pos2, const Vector4 &color,
              float thickness, float feather) {
  auto &ctx = GetRenderContext();
  if (!ctx.IsInitialized() || !ctx.CL()) {
    return;
  }

  if (!ctx.BindPipeline("primitive2d")) {
    return;
  }

  auto *prim = ctx.EnsurePrimitive2D();
  if (!prim) {
    return;
  }

  prim->SetLine(pos1, pos2, thickness, color, feather);
  prim->Draw(ctx.CL());
}

void DrawBox(const Vector2 &pos1, const Vector2 &pos2, const Vector4 &color,
             kFillMode fillMode, float feather) {
  auto &ctx = GetRenderContext();
  if (!ctx.IsInitialized() || !ctx.CL()) {
    return;
  }

  if (!ctx.BindPipeline("primitive2d")) {
    return;
  }

  auto *prim = ctx.EnsurePrimitive2D();
  if (!prim) {
    return;
  }

  float thickness = 1.0f;
  prim->SetRect(pos1, pos2, fillMode, thickness, color, feather);
  prim->Draw(ctx.CL());
}

void DrawCircle(const Vector2 &center, float radius, const Vector4 &color,
                kFillMode fillMode, float feather) {
  auto &ctx = GetRenderContext();
  if (!ctx.IsInitialized() || !ctx.CL()) {
    return;
  }

  if (!ctx.BindPipeline("primitive2d")) {
    return;
  }

  auto *prim = ctx.EnsurePrimitive2D();
  if (!prim) {
    return;
  }

  float thickness = 1.0f;
  prim->SetCircle(center, radius, fillMode, thickness, color, feather);
  prim->Draw(ctx.CL());
}

void DrawTriangle(const Vector2 &pos1, const Vector2 &pos2,
                  const Vector2 &pos3, const Vector4 &color,
                  kFillMode fillMode, float feather) {
  auto &ctx = GetRenderContext();
  if (!ctx.IsInitialized() || !ctx.CL()) {
    return;
  }

  if (!ctx.BindPipeline("primitive2d")) {
    return;
  }

  auto *prim = ctx.EnsurePrimitive2D();
  if (!prim) {
    return;
  }

  float thickness = 1.0f;
  prim->SetTriangle(pos1, pos2, pos3, fillMode, thickness, color, feather);
  prim->Draw(ctx.CL());
}

namespace {
/// <summary>
/// 頂点が追加された後の範囲をコマンドキューに登録するヘルパー
/// </summary>
static void AddPrimitiveCommand_(RenderContext &ctx, Primitive3D *prim,
                                 bool depth, uint32_t prevCount,
                                 uint64_t sortKey = 0) {
  uint32_t currentCount = prim->GetVertexCount(depth);
  uint32_t added = currentCount - prevCount;
  if (added > 0) {
    ctx.PushPrimitive3DCommand(depth, prevCount, added, sortKey);
  }
}

/// <summary>
/// 現在のモード（BeginOverlay3D 〜 EndOverlay3D の内側か）に応じたソートキーを返す
/// </summary>
/// <remarks>
/// sortKey==0 のコマンドは安定ソートで先頭（モデル描画の前）に回るため、
/// オーバーレイ中に 0 のまま積むと地形・モデルに塗り潰されて見えなくなる。
/// 3D プリミティブ描画は必ずこれを経由して sortKey を決めること。
/// </remarks>
static uint64_t CurrentPrimitiveSortKey_(const RenderContext &ctx) {
  return ctx.IsOverlayMode() ? SortKey::Make(SortKey::kLayerOverlay, 0, 0) : 0;
}
} // namespace

// ============================================================================
// Primitive3D
// ============================================================================

void DrawLine3D(const Vector3 &a, const Vector3 &b, const Vector4 &color,
                bool depth) {
  auto &ctx = GetRenderContext();
  if (!ctx.IsInitialized()) {
    return;
  }
  auto *prim = ctx.EnsurePrimitive3D();
  if (!prim) {
    return;
  }
  // Overlay モードでも depth は引数の指定に従う
  bool useDepth = depth;
  uint64_t sortKey = ctx.IsOverlayMode()
      ? SortKey::Make(SortKey::kLayerOverlay, 0, 0) : 0;
  uint32_t prev = prim->GetVertexCount(useDepth);
  prim->AddLine(a, b, color, useDepth);
  AddPrimitiveCommand_(ctx, prim, useDepth, prev, sortKey);
}

void DrawAABB3D(const Vector3 &mn, const Vector3 &mx, const Vector4 &color,
                bool depth) {
  auto &ctx = GetRenderContext();
  if (!ctx.IsInitialized()) {
    return;
  }
  auto *prim = ctx.EnsurePrimitive3D();
  if (!prim) {
    return;
  }
  bool useDepth = ctx.IsOverlayMode() ? false : depth;
  uint64_t sortKey = ctx.IsOverlayMode()
      ? SortKey::Make(SortKey::kLayerOverlay, 0, 0) : 0;
  uint32_t prev = prim->GetVertexCount(useDepth);
  prim->AddAABB(mn, mx, color, useDepth);
  AddPrimitiveCommand_(ctx, prim, useDepth, prev, sortKey);
}

void DrawFrustum3D(const Vector3 &position, const Vector3 &rotation,
                   float fovY, float aspect, float nearZ, float farZ,
                   const Vector4 &color, bool depth) {
  auto &ctx = GetRenderContext();
  if (!ctx.IsInitialized()) return;
  auto *prim = ctx.EnsurePrimitive3D();
  if (!prim) return;

  // Near/Far面の半分のサイズを計算
  float tanHalfFov = std::tan(fovY * 0.5f);
  float nearH = nearZ * tanHalfFov;
  float nearW = nearH * aspect;
  float farH = farZ * tanHalfFov;
  float farW = farH * aspect;

  // ローカル空間での8頂点（左手座標系: カメラは +Z を向いている）
  Vector3 localVerts[8] = {
    // Near face (z = +nearZ)
    {-nearW,  nearH, nearZ},   // 0: near top-left
    { nearW,  nearH, nearZ},   // 1: near top-right
    { nearW, -nearH, nearZ},   // 2: near bottom-right
    {-nearW, -nearH, nearZ},   // 3: near bottom-left
    // Far face (z = +farZ)
    {-farW,  farH, farZ},      // 4: far top-left
    { farW,  farH, farZ},      // 5: far top-right
    { farW, -farH, farZ},      // 6: far bottom-right
    {-farW, -farH, farZ},      // 7: far bottom-left
  };

  // カメラのワールド行列を作成
  Matrix4x4 world = MakeAffineMatrix({1,1,1}, rotation, position);

  // ローカル → ワールド変換
  Vector3 w[8];
  for (int i = 0; i < 8; ++i) {
    Vector4 v4 = {localVerts[i].x, localVerts[i].y, localVerts[i].z, 1.0f};
    Vector4 r;
    r.x = v4.x * world.m[0][0] + v4.y * world.m[1][0] + v4.z * world.m[2][0] + v4.w * world.m[3][0];
    r.y = v4.x * world.m[0][1] + v4.y * world.m[1][1] + v4.z * world.m[2][1] + v4.w * world.m[3][1];
    r.z = v4.x * world.m[0][2] + v4.y * world.m[1][2] + v4.z * world.m[2][2] + v4.w * world.m[3][2];
    w[i] = {r.x, r.y, r.z};
  }

  // depth=false で描画（深度テストなし = 常にモデルの上に表示）
  const bool useDepth = false;
  uint32_t prev = prim->GetVertexCount(useDepth);

  // Near面 (4辺)
  prim->AddLine(w[0], w[1], color, useDepth);
  prim->AddLine(w[1], w[2], color, useDepth);
  prim->AddLine(w[2], w[3], color, useDepth);
  prim->AddLine(w[3], w[0], color, useDepth);

  // Far面 (4辺)
  prim->AddLine(w[4], w[5], color, useDepth);
  prim->AddLine(w[5], w[6], color, useDepth);
  prim->AddLine(w[6], w[7], color, useDepth);
  prim->AddLine(w[7], w[4], color, useDepth);

  // Near-Far接続 (4辺)
  prim->AddLine(w[0], w[4], color, useDepth);
  prim->AddLine(w[1], w[5], color, useDepth);
  prim->AddLine(w[2], w[6], color, useDepth);
  prim->AddLine(w[3], w[7], color, useDepth);

  // オーバーレイレイヤーの sortKey で描画（モデルの後に実行される）
  uint64_t overlaySortKey = SortKey::Make(SortKey::kLayerOverlay, 0, 0);
  AddPrimitiveCommand_(ctx, prim, useDepth, prev, overlaySortKey);
}

void BeginOverlay3D() {
  auto &ctx = GetRenderContext();
  ctx.SetOverlayMode(true);
}

void EndOverlay3D() {
  auto &ctx = GetRenderContext();
  ctx.SetOverlayMode(false);
}

void DrawGridXZ3D(int halfSize, float step, const Vector4 &color, bool depth) {
  auto &ctx = GetRenderContext();
  if (!ctx.IsInitialized()) {
    return;
  }
  auto *prim = ctx.EnsurePrimitive3D();
  if (!prim) {
    return;
  }
  uint32_t prev = prim->GetVertexCount(depth);
  prim->AddGridXZ(halfSize, step, color, depth);
  AddPrimitiveCommand_(ctx, prim, depth, prev, CurrentPrimitiveSortKey_(ctx));
}

void DrawGridXY3D(int halfSize, float step, const Vector4 &color, bool depth) {
  auto &ctx = GetRenderContext();
  if (!ctx.IsInitialized()) {
    return;
  }
  auto *prim = ctx.EnsurePrimitive3D();
  if (!prim) {
    return;
  }
  uint32_t prev = prim->GetVertexCount(depth);
  prim->AddGridXY(halfSize, step, color, depth);
  AddPrimitiveCommand_(ctx, prim, depth, prev, CurrentPrimitiveSortKey_(ctx));
}

void DrawGridYZ3D(int halfSize, float step, const Vector4 &color, bool depth) {
  auto &ctx = GetRenderContext();
  if (!ctx.IsInitialized()) {
    return;
  }
  auto *prim = ctx.EnsurePrimitive3D();
  if (!prim) {
    return;
  }
  uint32_t prev = prim->GetVertexCount(depth);
  prim->AddGridYZ(halfSize, step, color, depth);
  AddPrimitiveCommand_(ctx, prim, depth, prev, CurrentPrimitiveSortKey_(ctx));
}

void DrawWireSphere3D(const Vector3 &center, float radius,
                      const Vector4 &color, int slices, int stacks,
                      bool depth) {
  auto &ctx = GetRenderContext();
  if (!ctx.IsInitialized() || !ctx.CL() || !ctx.Ctx()) {
    return;
  }
  auto *prim = ctx.EnsurePrimitive3D();
  if (!prim) {
    return;
  }
  bool useDepth = depth;
  uint64_t sortKey = ctx.IsOverlayMode()
      ? SortKey::Make(SortKey::kLayerOverlay, 0, 0) : 0;
  uint32_t prev = prim->GetVertexCount(useDepth);
  prim->AddSphere(center, radius, color, slices, stacks, useDepth);
  AddPrimitiveCommand_(ctx, prim, useDepth, prev, sortKey);
}

void DrawSphereRings3D(const Vector3 &center, float radius,
                       const Vector4 &color, int segments, bool depth) {
  auto &ctx = GetRenderContext();
  if (!ctx.IsInitialized()) {
    return;
  }
  auto *prim = ctx.EnsurePrimitive3D();
  if (!prim) {
    return;
  }
  bool useDepth = ctx.IsOverlayMode() ? false : depth;
  uint64_t sortKey = ctx.IsOverlayMode()
      ? SortKey::Make(SortKey::kLayerOverlay, 0, 0) : 0;
  uint32_t prev = prim->GetVertexCount(useDepth);
  prim->AddSphereRings(center, radius, color, segments, useDepth);
  AddPrimitiveCommand_(ctx, prim, useDepth, prev, sortKey);
}

void DrawArc3D(const Vector3 &center, const Vector3 &normal,
               const Vector3 &fromDir, float radius, float startRad,
               float endRad, const Vector4 &color, int segments, bool depth,
               bool drawToCenter) {
  auto &ctx = GetRenderContext();
  if (!ctx.IsInitialized()) {
    return;
  }
  auto *prim = ctx.EnsurePrimitive3D();
  if (!prim) {
    return;
  }
  uint32_t prev = prim->GetVertexCount(depth);
  prim->AddArc(center, normal, fromDir, radius, startRad, endRad, color,
               segments, depth, drawToCenter);
  AddPrimitiveCommand_(ctx, prim, depth, prev, CurrentPrimitiveSortKey_(ctx));
}

void DrawCapsule3D(const Vector3 &p0, const Vector3 &p1, float radius,
                   const Vector4 &color, int segments, bool depth) {
  auto &ctx = GetRenderContext();
  if (!ctx.IsInitialized()) {
    return;
  }
  auto *prim = ctx.EnsurePrimitive3D();
  if (!prim) {
    return;
  }
  uint32_t prev = prim->GetVertexCount(depth);
  prim->AddCapsule(p0, p1, radius, color, segments, depth);
  AddPrimitiveCommand_(ctx, prim, depth, prev, CurrentPrimitiveSortKey_(ctx));
}

void DrawOBB3D(const Vector3 &center, const Vector3 &axisX,
               const Vector3 &axisY, const Vector3 &axisZ,
               const Vector3 &halfExtents, const Vector4 &color, bool depth) {
  auto &ctx = GetRenderContext();
  if (!ctx.IsInitialized()) {
    return;
  }
  auto *prim = ctx.EnsurePrimitive3D();
  if (!prim) {
    return;
  }
  uint32_t prev = prim->GetVertexCount(depth);
  prim->AddOBB(center, axisX, axisY, axisZ, halfExtents, color, depth);
  AddPrimitiveCommand_(ctx, prim, depth, prev, CurrentPrimitiveSortKey_(ctx));
}

void DrawFrustumCorners3D(const Vector3 corners[8], const Vector4 &color,
                          bool depth) {
  auto &ctx = GetRenderContext();
  if (!ctx.IsInitialized()) {
    return;
  }
  auto *prim = ctx.EnsurePrimitive3D();
  if (!prim) {
    return;
  }
  uint32_t prev = prim->GetVertexCount(depth);
  prim->AddFrustum(corners, color, depth);
  AddPrimitiveCommand_(ctx, prim, depth, prev, CurrentPrimitiveSortKey_(ctx));
}

void DrawFrustum3D(const Vector3 &camPos, const Vector3 &forward,
                   const Vector3 &up, float fovYRad, float aspect, float nearZ,
                   float farZ, const Vector4 &color, bool depth) {
  auto &ctx = GetRenderContext();
  if (!ctx.IsInitialized()) {
    return;
  }
  auto *prim = ctx.EnsurePrimitive3D();
  if (!prim) {
    return;
  }
  uint32_t prev = prim->GetVertexCount(depth);
  prim->AddFrustumCamera(camPos, forward, up, fovYRad, aspect, nearZ, farZ,
                         color, depth);
  AddPrimitiveCommand_(ctx, prim, depth, prev, CurrentPrimitiveSortKey_(ctx));
}

void FlushPrimitive3D() {
  auto &ctx = GetRenderContext();
  if (!ctx.IsInitialized()) {
    return;
  }
  // コマンドキューを一括実行する（後方互換用）
  ctx.Execute3DCommands();
}


// ============================================================================
// Fog (画面オーバーレイ)
// ============================================================================

void DrawFogOverlay(float timeSec, float intensity, float scale, float speed,
                    const Vector2 &wind, float feather, float bottomBias) {
  auto &ctx = GetRenderContext();
  if (!ctx.IsInitialized() || !ctx.CL() || !ctx.Ctx() ||
      !ctx.Ctx()->pipelineManager) {
    return;
  }

  ctx.UpdateFogCB(timeSec, intensity, scale, speed, wind, feather, bottomBias);

  auto *pso = ctx.Ctx()->pipelineManager->Get(
      PipelineManager::MakeKey("fog", kBlendModeNormal));
  if (!pso) {
    return;
  }

  if (ctx.Ctx()->app) {
    D3D12_VIEWPORT viewport{};
    viewport.TopLeftX = 0.0f;
    viewport.TopLeftY = 0.0f;
    viewport.Width = static_cast<float>(ctx.Ctx()->app->width);
    viewport.Height = static_cast<float>(ctx.Ctx()->app->height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    ctx.CL()->RSSetViewports(1, &viewport);

    D3D12_RECT scissor{};
    scissor.left = 0;
    scissor.top = 0;
    scissor.right = static_cast<LONG>(ctx.Ctx()->app->width);
    scissor.bottom = static_cast<LONG>(ctx.Ctx()->app->height);
    ctx.CL()->RSSetScissorRects(1, &scissor);
  }

  ctx.CL()->SetGraphicsRootSignature(pso->Root());
  ctx.CL()->SetPipelineState(pso->PSO());
  ctx.CL()->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

  if (const D3D12_GPU_VIRTUAL_ADDRESS fogAddr = ctx.FogCBAddress()) {
    ctx.CL()->SetGraphicsRootConstantBufferView(0, fogAddr);
  }

  ctx.CL()->DrawInstanced(3, 1, 0, 0);
}

void SetFogOverlayColor(const Vector4 &color) {
  auto &ctx = GetRenderContext();
  if (!ctx.IsInitialized()) {
    return;
  }
  ctx.SetFogColor(color);
}

} // namespace RC
