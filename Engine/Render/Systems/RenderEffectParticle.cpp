// ============================================================================
// RenderEffectParticle.cpp
// ----------------------------------------------------------------------------
// RC::名前空間のエフェクト粒（水しぶき・泡・水柱）API 実装
// ============================================================================

#include "RenderCommon.h"
#include "RenderContext.h"

namespace RC {

void SpawnEffectParticle(const EffectParticleSpawn &desc) {
  GetRenderContext().EffectParticles().Spawn(desc);
}

void UpdateEffectParticles(float dt) {
  GetRenderContext().EffectParticles().Update(dt);
}

void DrawEffectParticles() {
  auto &ctx = GetRenderContext();
  if (!ctx.IsInitialized() || ctx.EffectParticles().Count() == 0) {
    return;
  }
  ctx.EffectParticles().RequestDraw();
}

void ClearEffectParticles() {
  GetRenderContext().EffectParticles().Clear();
}

uint32_t GetEffectParticleCount() {
  return GetRenderContext().EffectParticles().Count();
}

EffectParticleSystem::Stats GetEffectParticleStats() {
  return GetRenderContext().EffectParticles().LastFrameStats();
}

} // namespace RC
