#include "EffectParticleSystem.h"

#include "RenderContext.h"
#include "Graphics/Mesh/MeshGenerator.h"
#include "Graphics/Mesh/PrimitiveMesh.h"
#include "Graphics/Model/ModelResource.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <numeric>

namespace RC {

namespace {

constexpr float kPi = 3.14159f; // 旧スクリプトと同じ値（角度の乱数に使う）

/// @brief 0〜(n-1) の整数乱数（旧スクリプトの rand() % n と同じ）
inline float RandMod(int n) { return static_cast<float>(std::rand() % n); }

/// @brief 行ベクトル規約の行列積（a * b）を out へ
inline void MulTo(const Matrix4x4 &a, const Matrix4x4 &b, Matrix4x4 &out) {
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      out.m[r][c] = a.m[r][0] * b.m[0][c] + a.m[r][1] * b.m[1][c] +
                    a.m[r][2] * b.m[2][c] + a.m[r][3] * b.m[3][c];
    }
  }
}

bool IsDebugShadingMode(ViewShadingMode mode) {
  return mode == ViewShadingMode::FaceOrientation || mode == ViewShadingMode::RandomColor ||
         mode == ViewShadingMode::SolidShading;
}

std::string_view DebugInstPrefix(ViewShadingMode mode) {
  switch (mode) {
  case ViewShadingMode::FaceOrientation: return "object3d_faceori_inst";
  case ViewShadingMode::RandomColor:     return "object3d_randcolor_inst";
  case ViewShadingMode::SolidShading:    return "object3d_solid_inst";
  default:                               return "object3d_inst";
  }
}

/// @brief 影パスはメインパス用に残しておく SRV 領域（ModelInstanceBatcher と同じ考え方）
constexpr uint64_t kMainPassReserveBytes = 8ull * 1024ull * 1024ull;

} // namespace

// ============================================================================
// 生成
// ============================================================================

void EffectParticleSystem::Spawn(const EffectParticleSpawn &desc) {
  if (particles_.size() >= kMaxParticles) {
    return;
  }

  Particle p;
  p.kind = desc.kind;
  p.pos = desc.position;
  p.color = desc.color;
  p.startScale = desc.scale;
  p.scale = {desc.scale, desc.scale, desc.scale};
  const float impact = desc.impactFactor;

  // 初速・寿命は旧スクリプトの OnCreate と同じ式
  switch (desc.kind) {
  case EffectParticleKind::Splash: {
    p.lifetime = 0.6f + RandMod(100) / 100.0f;
    const float angle = RandMod(360) * kPi / 180.0f;
    const float speed = (4.0f + RandMod(100) / 15.0f) * std::sqrt(impact);
    const float upSpeed = (4.0f + RandMod(100) / 15.0f) * impact;
    p.vel = {std::cos(angle) * speed, upSpeed, std::sin(angle) * speed};
    p.color.w = 0.85f;
    break;
  }
  case EffectParticleKind::Wake: {
    p.lifetime = 0.5f + RandMod(50) / 100.0f;
    const float angle = RandMod(360) * kPi / 180.0f;
    const float speed = 0.5f + RandMod(100) / 100.0f;
    p.vel = {std::cos(angle) * speed, 3.0f + RandMod(100) / 50.0f, std::sin(angle) * speed};
    p.color.w = 0.9f;
    break;
  }
  case EffectParticleKind::HeavySplash: {
    p.lifetime = 1.0f + RandMod(20) / 100.0f;
    p.maxHeight = 15.0f * impact;
    p.startY = desc.position.y;
    p.scale = {desc.scale, 0.1f, desc.scale}; // 平たく広い状態から伸びる
    break;
  }
  case EffectParticleKind::Bubble: {
    p.lifetime = 0.4f + RandMod(60) / 100.0f;
    const float angle = RandMod(360) * kPi / 180.0f;
    const float speed = 0.5f + RandMod(100) / 100.0f;
    p.vel = {std::cos(angle) * speed, 1.0f + RandMod(100) / 50.0f, std::sin(angle) * speed};
    p.color = {0.8f, 0.9f, 1.0f, 0.6f};
    break;
  }
  }

  particles_.push_back(p);
}

// ============================================================================
// 更新（旧スクリプトの OnUpdate と同じ式）
// ============================================================================

bool EffectParticleSystem::Step_(Particle &p, float dt) {
  p.elapsed += dt;
  if (p.elapsed >= p.lifetime) {
    return false;
  }
  const float t = p.elapsed / p.lifetime;

  switch (p.kind) {
  case EffectParticleKind::Splash: {
    p.vel.y -= 12.0f * dt;
    p.pos.x += p.vel.x * dt;
    p.pos.y += p.vel.y * dt;
    p.pos.z += p.vel.z * dt;
    if (p.pos.y < -0.5f) {
      return false; // 水に落ちたら消える
    }
    const float s = (std::max)((1.0f - t) * p.startScale, 0.01f);
    p.scale = {s, s, s};
    p.color.w = (1.0f - t) * 0.85f;
    return true;
  }
  case EffectParticleKind::Wake: {
    p.vel.y -= 15.0f * dt;
    p.pos.x += p.vel.x * dt;
    p.pos.y += p.vel.y * dt;
    p.pos.z += p.vel.z * dt;
    if (p.pos.y < 0.0f) { // 水面で少しバウンド
      p.pos.y = 0.0f;
      p.vel.y *= -0.3f;
      p.vel.x *= 0.8f;
      p.vel.z *= 0.8f;
    }
    const float s = (std::max)(p.startScale * (1.0f - t), 0.01f);
    p.scale = {s, s, s};
    p.color.w = (1.0f - t) * 0.9f;
    return true;
  }
  case EffectParticleKind::HeavySplash: {
    const float heightProgress = 1.0f - std::pow(1.0f - t, 4.0f);
    const float h = p.maxHeight * heightProgress;
    const float w = p.startScale * (1.0f + std::sin(t * kPi) * 0.5f);
    p.scale = {w, h, w};
    p.pos.y = p.startY + h * 0.5f; // 円柱は中心原点なので、根元を固定するため半分持ち上げる
    p.color.w = (t > 0.7f) ? (1.0f - (t - 0.7f) / 0.3f) : 1.0f;
    return true;
  }
  case EffectParticleKind::Bubble: {
    const float wobble = std::sin(p.elapsed * 10.0f) * 0.5f;
    p.pos.x += (p.vel.x + wobble * p.vel.z) * dt;
    p.pos.y += p.vel.y * dt;
    p.pos.z += (p.vel.z - wobble * p.vel.x) * dt;
    float s = p.startScale;
    if (t > 0.8f) {
      s = p.startScale * (1.0f - (t - 0.8f) * 5.0f); // 最後にはじける
    }
    s = (std::max)(s, 0.01f);
    p.scale = {s, s, s};
    p.color = {0.8f, 0.9f, 1.0f, (1.0f - t) * 0.6f};
    return true;
  }
  }
  return false;
}

void EffectParticleSystem::Update(float dt) {
  if (dt <= 0.0f) {
    return; // 停止・一時停止中は旧スクリプトと同じく止まったまま
  }
  time_ += dt;

  // 死んだ粒は末尾と入れ替えて詰める（順番は描画時に並べ直すので問題ない）
  for (size_t i = 0; i < particles_.size();) {
    if (Step_(particles_[i], dt)) {
      ++i;
    } else {
      particles_[i] = particles_.back();
      particles_.pop_back();
    }
  }
}

void EffectParticleSystem::Clear() {
  particles_.clear();
  drawRequested_ = false;
}

void EffectParticleSystem::Term(RenderContext &ctx) {
  Clear();
  if (sphereMesh_ >= 0) {
    ctx.PrimitiveMeshes().Unload(sphereMesh_);
    sphereMesh_ = -1;
  }
  if (cylinderMesh_ >= 0) {
    ctx.PrimitiveMeshes().Unload(cylinderMesh_);
    cylinderMesh_ = -1;
  }
}

void EffectParticleSystem::BeginFrame() {
  lastStats_ = stats_;
  stats_ = Stats{};
  stats_.alive = static_cast<uint32_t>(particles_.size());
}

// ============================================================================
// 描画
// ============================================================================

EffectParticleSystem::DrawGroup EffectParticleSystem::GroupOf_(EffectParticleKind k) {
  switch (k) {
  case EffectParticleKind::HeavySplash: return DrawGroup::WaterColumn;
  case EffectParticleKind::Bubble:      return DrawGroup::Bubble;
  default:                              return DrawGroup::Water;
  }
}

bool EffectParticleSystem::EnsureMeshes_(RenderContext &ctx) {
  auto &meshes = ctx.PrimitiveMeshes();
  // 旧実装では 1 粒ごとに GenerateSphere していた。ここでは種類ごとに 1 つだけ作って共有する
  if (sphereMesh_ < 0 || !meshes.IsValid(sphereMesh_)) {
    sphereMesh_ = meshes.Create(MeshGenerator::GenerateSphere(1.0f), -1, "EffectSphere");
  }
  if (cylinderMesh_ < 0 || !meshes.IsValid(cylinderMesh_)) {
    cylinderMesh_ = meshes.Create(MeshGenerator::GenerateCylinder(1.0f, 1.0f), -1, "EffectCylinder");
  }
  return sphereMesh_ >= 0 && cylinderMesh_ >= 0;
}

void EffectParticleSystem::Flush(RenderContext &ctx) {
  if (!drawRequested_) return;
  drawRequested_ = false;
  if (particles_.empty() || !EnsureMeshes_(ctx)) return;

  const bool shadowPass = ctx.IsShadowPass();
  const Frustum frustum = Frustum::FromViewProjection(ctx.CurrentPassViewProjection());

  for (auto &v : groupIndices_) v.clear();

  // 視錐台カリング（球は半径 = scale、円柱は半径 = scale.x・高さ = scale.y）
  for (uint32_t i = 0; i < particles_.size(); ++i) {
    const Particle &p = particles_[i];
    const Vector3 half = (p.kind == EffectParticleKind::HeavySplash)
                             ? Vector3{p.scale.x, p.scale.y * 0.5f, p.scale.z}
                             : p.scale;
    if (!frustum.Intersects(BoundingBox::FromCenterHalf(p.pos, half))) continue;
    groupIndices_[static_cast<size_t>(GroupOf_(p.kind))].push_back(i);
  }

  // 半透明（水）は奥から手前、不透明（泡）は手前から奥へ並べる。影パスは並べない
  if (!shadowPass) {
    const Matrix4x4 viewInv = Inverse(ctx.View());
    const Vector3 cam = {viewInv.m[3][0], viewInv.m[3][1], viewInv.m[3][2]};
    sortKeys_.resize(particles_.size());
    for (uint32_t i = 0; i < particles_.size(); ++i) {
      const Vector3 &q = particles_[i].pos;
      const float dx = q.x - cam.x, dy = q.y - cam.y, dz = q.z - cam.z;
      sortKeys_[i] = dx * dx + dy * dy + dz * dz;
    }
    for (size_t g = 0; g < static_cast<size_t>(DrawGroup::Count); ++g) {
      auto &idx = groupIndices_[g];
      if (idx.size() < 2) continue;
      const bool backToFront = (g != static_cast<size_t>(DrawGroup::Bubble));
      std::sort(idx.begin(), idx.end(), [&](uint32_t a, uint32_t b) {
        return backToFront ? (sortKeys_[a] > sortKeys_[b]) : (sortKeys_[a] < sortKeys_[b]);
      });
    }
  }

  for (size_t g = 0; g < static_cast<size_t>(DrawGroup::Count); ++g) {
    if (!groupIndices_[g].empty()) {
      EmitGroup_(ctx, static_cast<DrawGroup>(g), groupIndices_[g], shadowPass);
    }
  }
}

void EffectParticleSystem::EmitGroup_(RenderContext &ctx, DrawGroup group,
                                      const std::vector<uint32_t> &indices, bool shadowPass) {
  const uint32_t count = static_cast<uint32_t>(indices.size());
  const uint64_t bytes = static_cast<uint64_t>(count) * sizeof(ModelResource::InstanceGPU);
  const uint64_t reserve = shadowPass ? kMainPassReserveBytes : 0u;

  FrameResource &frame = ctx.CurrentFrame();
  if (bytes + reserve > UINT32_MAX || !frame.HasSRVSpace(static_cast<uint32_t>(bytes + reserve)) ||
      !frame.HasCBSpace(sizeof(Material) + sizeof(DirectionalLight) + 256u)) {
    return; // フレームリソース不足。範囲外へ書かないよう、このグループは描かない
  }

  // --- インスタンスデータ ---
  // ※ 書き込み先はアップロードヒープ（CPU から読むと非常に遅い）なので、
  //    スタック上で組み立ててから memcpy で書くだけにする（読み返さない）
  const Matrix4x4 viewProj = Multiply(ctx.View(), ctx.Proj());
  void *instMapped = nullptr;
  const D3D12_GPU_VIRTUAL_ADDRESS instAddr =
      frame.AllocSRV(static_cast<uint32_t>(bytes), &instMapped);
  auto *dst = static_cast<ModelResource::InstanceGPU *>(instMapped);

  for (uint32_t k = 0; k < count; ++k) {
    const Particle &p = particles_[indices[k]];
    ModelResource::InstanceGPU tmp{};
    // World = Scale × Translate（回転なし）
    tmp.World.m[0][0] = p.scale.x;
    tmp.World.m[1][1] = p.scale.y;
    tmp.World.m[2][2] = p.scale.z;
    tmp.World.m[3][0] = p.pos.x;
    tmp.World.m[3][1] = p.pos.y;
    tmp.World.m[3][2] = p.pos.z;
    tmp.World.m[3][3] = 1.0f;
    tmp.color = p.color;
    if (shadowPass) {
      tmp.WVP = tmp.World; // 影パスの VS は World しか読まない
      tmp.WorldInverseTranspose = tmp.World;
    } else {
      MulTo(tmp.World, viewProj, tmp.WVP);
      // 拡大＋平行移動の逆転置。法線の変換に使うのは左上 3x3 だけ
      const float sx = (std::max)(std::abs(p.scale.x), 1e-6f);
      const float sy = (std::max)(std::abs(p.scale.y), 1e-6f);
      const float sz = (std::max)(std::abs(p.scale.z), 1e-6f);
      tmp.WorldInverseTranspose.m[0][0] = 1.0f / sx;
      tmp.WorldInverseTranspose.m[1][1] = 1.0f / sy;
      tmp.WorldInverseTranspose.m[2][2] = 1.0f / sz;
      tmp.WorldInverseTranspose.m[0][3] = -p.pos.x / sx;
      tmp.WorldInverseTranspose.m[1][3] = -p.pos.y / sy;
      tmp.WorldInverseTranspose.m[2][3] = -p.pos.z / sz;
      tmp.WorldInverseTranspose.m[3][3] = 1.0f;
    }
    std::memcpy(&dst[k], &tmp, sizeof(tmp));
  }

  // --- マテリアル（PrimitiveMesh の初期値と同じ。色は instColor で渡すので白） ---
  Material mat{};
  mat.color = {1.0f, 1.0f, 1.0f, 1.0f};
  mat.lightingMode = 2; // Half Lambert（PrimitiveMesh の既定）
  if (ctx.DirLights().GetActiveCBAddress() != 0) {
    if (const auto *active = ctx.DirLights().GetActive()) {
      mat.lightingMode = active->GetLightingMode(); // DrawPrimitiveMesh と同じくライトに追従
    }
  }
  mat.shininess = 32.0f;
  mat.environmentCoefficient = 0.0f;
  mat.uvTransform = MakeIdentity4x4();
  if (group == DrawGroup::WaterColumn) {
    // 水柱は V 方向へ流れる（旧スクリプトは毎秒 3 ずつ減らしていた）。
    // WaterColumn.PS はこの値を「時間」としてノイズの座標に掛けるので、1 で折り返すと
    // 模様が 1/3 秒ごとに飛ぶ。float の精度が落ちない程度の大きな周期でだけ折り返す。
    mat.uvTransform.m[3][1] = -std::fmod(time_ * 3.0f, 1000.0f);
  }
  void *matMapped = nullptr;
  const D3D12_GPU_VIRTUAL_ADDRESS matAddr = frame.AllocCB(sizeof(Material), &matMapped);
  std::memcpy(matMapped, &mat, sizeof(Material));

  // --- 平行光源 CB（無効なら既定値の CB を用意して、未設定のまま描かないようにする） ---
  D3D12_GPU_VIRTUAL_ADDRESS lightAddr = ctx.DirLights().GetActiveCBAddress();
  if (lightAddr == 0) {
    DirectionalLight fallback{};
    fallback.color = {1.0f, 1.0f, 1.0f, 1.0f};
    fallback.direction = {0.0f, -1.0f, 0.0f};
    fallback.intensity = 1.0f;
    fallback.ambientColor = {1.0f, 1.0f, 1.0f};
    fallback.ambientIntensity = 0.0f;
    void *lightMapped = nullptr;
    lightAddr = frame.AllocCB(sizeof(DirectionalLight), &lightMapped);
    std::memcpy(lightMapped, &fallback, sizeof(DirectionalLight));
  }

  const int meshHandle = (group == DrawGroup::WaterColumn) ? cylinderMesh_ : sphereMesh_;

  ++stats_.drawCalls;
  stats_.drawn += count;

  const bool translucent = (group != DrawGroup::Bubble);
  const uint64_t sortKey =
      translucent ? SortKey::Make(SortKey::kLayerGlass, SortKey::HashPSO("object3d_water_inst"), 0)
                  : SortKey::Make(SortKey::kLayerOpaque, SortKey::HashPSO("object3d_inst"), 0);

  ctx.PushCommand3D(
      sortKey,
      [meshHandle, group, matAddr, lightAddr, instAddr, count](ID3D12GraphicsCommandList *cl) {
        auto &rc = GetRenderContext();
        PrimitiveMesh *mesh = rc.PrimitiveMeshes().Get(meshHandle);
        if (!mesh) return;
        const BlendMode prevBlend = rc.CurrentBlendMode();

        auto draw = [&](std::string_view prefix, BlendMode blend) {
          rc.SetBlendMode(blend);
          if (!rc.BindPipeline(prefix)) return;
          rc.BindCameraCB();
          rc.BindAllLightCBs();
          mesh->DrawInstancedPrepared(cl, matAddr, lightAddr, instAddr, count);
        };

        const ViewShadingMode mode = rc.GetViewShadingMode();
        if (rc.IsShadowPass()) {
          // 影は形だけ（BindPipeline が shadow_inst へ振り替える）
          draw("object3d_inst", kBlendModeNone);
        } else if (IsDebugShadingMode(mode)) {
          draw(DebugInstPrefix(mode), kBlendModeNone);
        } else {
          if (mode != ViewShadingMode::Wireframe) {
            switch (group) {
            case DrawGroup::Water:
              // 2 パス: 裏面 → 表面（DrawPrimitiveMeshWater と同じ順）
              draw("object3d_water_inst_front", kBlendModePremultiplied);
              draw("object3d_water_inst", kBlendModePremultiplied);
              break;
            case DrawGroup::WaterColumn:
              draw("object3d_watercolumn_inst_front", kBlendModePremultiplied);
              draw("object3d_watercolumn_inst", kBlendModePremultiplied);
              break;
            default:
              // 泡は従来どおり不透明の object3d（DrawPrimitiveMesh はブレンド無しで描いていた）
              draw("object3d_inst", kBlendModeNone);
              break;
            }
          }
          if (mode == ViewShadingMode::Wireframe || mode == ViewShadingMode::SolidWireframe) {
            draw("object3d_wire_inst", kBlendModeNone);
          }
        }
        rc.SetBlendMode(prevBlend);
      },
      translucent ? "EffectParticles(Water)" : "EffectParticles(Bubble)", -1);
}

} // namespace RC
