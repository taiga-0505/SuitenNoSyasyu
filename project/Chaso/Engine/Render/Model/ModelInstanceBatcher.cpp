#include "ModelInstanceBatcher.h"

#include "ModelProxyPool.h"
#include "RenderContext.h"
#include "Model/ModelObject.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace RC {

namespace {

/// @brief 64bit のハッシュ合成（boost::hash_combine と同じ考え方）
inline void HashCombine(size_t &seed, uint64_t v) {
  seed ^= std::hash<uint64_t>{}(v) + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
}

inline uint32_t FloatBits(float f) {
  uint32_t u;
  std::memcpy(&u, &f, sizeof(u));
  return u;
}

/// @brief デバッグ表示用のプロシージャルシェーダーか
bool IsDebugShadingMode(ViewShadingMode mode) {
  return mode == ViewShadingMode::FaceOrientation || mode == ViewShadingMode::RandomColor ||
         mode == ViewShadingMode::SolidShading;
}

/// @brief ViewShadingMode に応じたインスタンシング用 PSO 名（RenderModel.cpp と同じ対応）
std::string_view ResolveInstPrefix(ViewShadingMode mode) {
  switch (mode) {
  case ViewShadingMode::FaceOrientation: return "object3d_faceori_inst";
  case ViewShadingMode::RandomColor:     return "object3d_randcolor_inst";
  case ViewShadingMode::SolidShading:    return "object3d_solid_inst";
  default:                               return "object3d_inst";
  }
}

/// @brief 行ベクトル規約の行列積（a * b）を out へ（Multiply の戻り値コピーを避ける）
inline void MulTo(const Matrix4x4 &a, const Matrix4x4 &b, Matrix4x4 &out) {
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      out.m[r][c] = a.m[r][0] * b.m[0][c] + a.m[r][1] * b.m[1][c] +
                    a.m[r][2] * b.m[2][c] + a.m[r][3] * b.m[3][c];
    }
  }
}

} // namespace

// ============================================================================
// キー
// ============================================================================

bool ModelInstanceBatcher::BatchKey::operator==(const BatchKey &o) const {
  return mesh == o.mesh && texture == o.texture && normalMap == o.normalMap &&
         roughnessMap == o.roughnessMap && lightCB == o.lightCB &&
         lightingMode == o.lightingMode && FloatBits(shininess) == FloatBits(o.shininess) &&
         FloatBits(environmentCoefficient) == FloatBits(o.environmentCoefficient) &&
         std::memcmp(uvTransform, o.uvTransform, sizeof(uvTransform)) == 0;
}

size_t ModelInstanceBatcher::BatchKeyHash::operator()(const BatchKey &k) const {
  size_t h = 0;
  HashCombine(h, reinterpret_cast<uint64_t>(k.mesh));
  HashCombine(h, k.texture);
  HashCombine(h, k.normalMap);
  HashCombine(h, k.roughnessMap);
  HashCombine(h, k.lightCB);
  HashCombine(h, static_cast<uint64_t>(static_cast<uint32_t>(k.lightingMode)));
  HashCombine(h, FloatBits(k.shininess));
  HashCombine(h, FloatBits(k.environmentCoefficient));
  for (float f : k.uvTransform) HashCombine(h, FloatBits(f));
  return h;
}

ModelInstanceBatcher::BatchKey
ModelInstanceBatcher::MakeKey_(RenderContext &ctx, ::ModelObject &m, Material &outMaterial) const {
  Material *src = m.Mat();

  // ライティングモード / shininess を DrawModel（RenderModel.cpp の ApplyDirLight_）と同じ規則で解決
  const D3D12_GPU_VIRTUAL_ADDRESS globalLight = ctx.DirLights().GetActiveCBAddress();
  if (globalLight != 0 && src) {
    if (const auto *active = ctx.DirLights().GetActive()) {
      const int ovr = m.GetLightingModeOverride();
      src->lightingMode = (ovr >= 0) ? ovr : active->GetLightingMode();
      src->shininess = m.GetShininess();
    }
  }

  const ModelResource &res = m.Resource();
  if (src) {
    outMaterial = *src;
  } else {
    outMaterial = Material{};
    outMaterial.uvTransform = MakeIdentity4x4();
  }
  // 色はインスタンスごとに InstanceGPU::color で渡す（PS は Material.color × instColor）
  outMaterial.color = {1.0f, 1.0f, 1.0f, 1.0f};
  outMaterial.useNormalMap = (res.NormalMapPtr() != 0) ? 1 : 0;
  outMaterial.useRoughnessMap = (res.RoughnessMapPtr() != 0) ? 1 : 0;

  BatchKey key;
  key.mesh = m.GetMesh().get();
  key.texture = res.TextureOverridePtr();
  key.normalMap = res.NormalMapPtr();
  key.roughnessMap = res.RoughnessMapPtr();
  key.lightCB = res.EffectiveLightCBAddress();
  key.lightingMode = outMaterial.lightingMode;
  key.shininess = outMaterial.shininess;
  key.environmentCoefficient = outMaterial.environmentCoefficient;
  std::memcpy(key.uvTransform, &outMaterial.uvTransform.m[0][0], sizeof(key.uvTransform));
  return key;
}

ModelInstanceBatcher::Batch &
ModelInstanceBatcher::FindOrAddBatch_(const BatchKey &key, ::ModelObject *leader,
                                      int leaderHandle, const Material &material) {
  auto it = batchLookup_.find(key);
  if (it != batchLookup_.end()) {
    return batches_[it->second];
  }
  if (batchCount_ == batches_.size()) {
    batches_.emplace_back();
  }
  const uint32_t idx = batchCount_++;
  Batch &b = batches_[idx];
  b.leader = leader;
  b.leaderHandle = leaderHandle;
  b.textureOverride = key.texture;
  b.material = material;
  b.instances.clear();
  batchLookup_.emplace(key, idx);
  return b;
}

// ============================================================================
// 受け付け
// ============================================================================

void ModelInstanceBatcher::Submit(::ModelObject *m, int modelHandle, int texHandle) {
  Submission s;
  s.model = m;
  s.modelHandle = modelHandle;
  s.texHandle = texHandle;
  s.world = m->CachedWorld();
  if (const Material *mat = m->Mat()) s.color = mat->color;
  submissions_.push_back(s);
}

void ModelInstanceBatcher::BeginFrame() {
  lastStats_ = stats_;
  stats_ = Stats{};
}

void ModelInstanceBatcher::Clear() {
  submissions_.clear();
  proxiesRequested_ = false;
  batchLookup_.clear();
  batches_.clear();
  batchCount_ = 0;
  proxyTemplateBatch_.clear();
}

// ============================================================================
// Flush
// ============================================================================

void ModelInstanceBatcher::Flush(RenderContext &ctx, ModelProxyPool &proxies) {
  if (!HasPending()) return;
  const auto flushStart = std::chrono::steady_clock::now();

  const bool shadowPass = ctx.IsShadowPass();
  // カリングはこのパスの視点（影パスならライト）で行う
  const Frustum frustum = Frustum::FromViewProjection(ctx.CurrentPassViewProjection());
  // WVP はカメラの行列（影パスでは VS が World しか読まないので使わない）
  const Matrix4x4 cameraViewProj = Multiply(ctx.View(), ctx.Proj());
  const D3D12_GPU_VIRTUAL_ADDRESS globalLight = ctx.DirLights().GetActiveCBAddress();

  batchLookup_.clear();
  batchCount_ = 0;

  // --------------------------------------------------------------------------
  // 1) エンティティのモデル（RC::DrawModelInstanced）
  // --------------------------------------------------------------------------
  for (Submission &s : submissions_) {
    ::ModelObject *m = s.model;
    if (!m || !m->IsReady() || !m->Visible()) continue;
    const auto &mesh = m->GetMesh();
    if (!mesh || !mesh->Ready()) continue;
    ++stats_.submitted;

    const Matrix4x4 &world = s.world;
    if (!frustum.Intersects(mesh->LocalBounds().Transformed(world))) {
      ++stats_.culled;
      continue;
    }

    // テクスチャ・ライト CB は DrawModel と同じ規則でモデルへ反映してからキーを作る
    if (s.texHandle >= 0) {
      m->SetTexture(ctx.Textures().GetSrv(s.texHandle));
    } else {
      m->ResetTextureToMtl();
    }
    m->SetExternalLightCBAddress(globalLight);

    Material mat{};
    const BatchKey key = MakeKey_(ctx, *m, mat);
    Batch &b = FindOrAddBatch_(key, m, s.modelHandle, mat);

    InstanceRef ref;
    ref.world = &world;
    if (!shadowPass) {
      // 逆転置はモデル側のキャッシュと同じ行列ならそれを使う（静止物は再計算しない）
      const Matrix4x4 &cached = m->CachedWorld();
      if (std::memcmp(&cached, &world, sizeof(Matrix4x4)) == 0) {
        ref.worldInvTranspose = &m->CachedWorldInverseTranspose();
      } else {
        s.worldInvTranspose = Transpose(Inverse(world));
        ref.worldInvTranspose = &s.worldInvTranspose;
      }
    }
    ref.color = s.color;
    b.instances.push_back(ref);
  }
  // ※ submissions_ は InstanceRef が指しているので、書き出しが終わるまで clear しない

  // --------------------------------------------------------------------------
  // 2) VirtualEntity（ModelProxyPool）: BVH を視錐台で辿って見えている物だけ拾う
  // --------------------------------------------------------------------------
  if (proxiesRequested_) {
    proxiesRequested_ = false;
    proxies.UpdateDirty(ctx.Models());
    stats_.proxyTotal = proxies.Count();

    struct TemplateInfo {
      int32_t batch = -1;                     ///< -1 = このテンプレートは描かない
      Vector4 color{1.0f, 1.0f, 1.0f, 1.0f};  ///< テンプレートのマテリアル色
    };
    // テンプレートごとのバッチ番号（同じテンプレートのプロキシは必ず同じバッチ）
    proxyTemplateBatch_.clear();
    std::vector<TemplateInfo> templateInfos; // proxyTemplateBatch_ の値 → この配列の添え字
    templateInfos.reserve(8);

    proxies.QueryVisible(frustum, shadowPass, [&](uint32_t slot) {
      ++stats_.submitted;
      if (!shadowPass) ++stats_.proxyMainVisible;
      const int handle = proxies.ModelHandle(slot);

      int32_t infoIdx;
      auto it = proxyTemplateBatch_.find(handle);
      if (it == proxyTemplateBatch_.end()) {
        TemplateInfo info;
        ::ModelObject *m = ctx.Models().Get(handle);
        if (m && m->IsReady() && m->GetMesh() && m->GetMesh()->Ready() && !m->HasSkinData()) {
          // テンプレートのテクスチャ上書きはそのまま使う（未設定なら mtl のテクスチャを解決）
          if (m->Resource().TextureOverridePtr() == 0) {
            m->ResetTextureToMtl();
          }
          m->SetExternalLightCBAddress(globalLight);
          Material mat{};
          const BatchKey key = MakeKey_(ctx, *m, mat);
          FindOrAddBatch_(key, m, handle, mat);
          info.batch = static_cast<int32_t>(batchLookup_.find(key)->second);
          if (m->Mat()) info.color = m->Mat()->color;
        }
        infoIdx = static_cast<int32_t>(templateInfos.size());
        templateInfos.push_back(info);
        proxyTemplateBatch_.emplace(handle, infoIdx);
      } else {
        infoIdx = it->second;
      }

      const TemplateInfo &info = templateInfos[infoIdx];
      if (info.batch < 0) return;

      const Vector4 &pc = proxies.Color(slot);
      InstanceRef ref;
      ref.world = &proxies.World(slot);
      ref.worldInvTranspose = shadowPass ? nullptr : &proxies.WorldInverseTranspose(slot);
      ref.color = {pc.x * info.color.x, pc.y * info.color.y, pc.z * info.color.z,
                   pc.w * info.color.w};
      batches_[info.batch].instances.push_back(ref);
    });
  }

  // --------------------------------------------------------------------------
  // 3) メイン系のパスでは手前から奥へ並べる
  //    深度テストで奥の物のピクセルシェーダーが早期に棄却され（Early-Z）、重ね塗りが減る。
  //    BVH を辿った順は空間的にばらばらなので、並べないと奥の物を先に塗ってしまうことがある。
  //    影パスは深度しか書かない（ピクセルシェーダーがほぼ空）ので並べない。
  // --------------------------------------------------------------------------
  if (!shadowPass) {
    const Matrix4x4 viewInv = Inverse(ctx.View());
    const float cx = viewInv.m[3][0], cy = viewInv.m[3][1], cz = viewInv.m[3][2];
    for (uint32_t i = 0; i < batchCount_; ++i) {
      auto &inst = batches_[i].instances;
      if (inst.size() < 2) continue;
      for (InstanceRef &r : inst) {
        const float dx = r.world->m[3][0] - cx;
        const float dy = r.world->m[3][1] - cy;
        const float dz = r.world->m[3][2] - cz;
        r.depthSq = dx * dx + dy * dy + dz * dz;
      }
      std::sort(inst.begin(), inst.end(),
                [](const InstanceRef &a, const InstanceRef &b) { return a.depthSq < b.depthSq; });
    }
  }

  // --------------------------------------------------------------------------
  // 4) バッチごとにインスタンスデータを書いてコマンド化
  // --------------------------------------------------------------------------
  for (uint32_t i = 0; i < batchCount_; ++i) {
    if (!batches_[i].instances.empty()) {
      EmitBatch_(ctx, batches_[i], shadowPass, cameraViewProj);
    }
    batches_[i].instances.clear();
  }
  submissions_.clear();

  stats_.flushMs += std::chrono::duration<float, std::milli>(
                        std::chrono::steady_clock::now() - flushStart).count();
}

// ============================================================================
// 1 バッチの書き出し
// ============================================================================

void ModelInstanceBatcher::EmitBatch_(RenderContext &ctx, Batch &batch, bool shadowPass,
                                      const Matrix4x4 &viewProj) {
  ::ModelObject *leader = batch.leader;
  const auto &mesh = leader->GetMesh();
  const auto &items = mesh->DrawItems();

  // nodeWorld が全て単位行列なら全 DrawItem で同じデータを共有できる。
  // そうでなければ DrawItem ごとに nodeWorld を掛けたデータを並べる。
  const bool perItem = !items.empty() && !mesh->AllNodeWorldIdentity();
  const uint32_t count = static_cast<uint32_t>(batch.instances.size());
  const uint32_t itemFactor = perItem ? static_cast<uint32_t>(items.size()) : 1u;
  const uint64_t bytes64 =
      static_cast<uint64_t>(count) * itemFactor * sizeof(ModelResource::InstanceGPU);

  FrameResource &frame = ctx.CurrentFrame();
  // 影パスはメインパス用の取り分を残しておく（影で使い切るとメイン画面の物が消えるため）
  const uint64_t reserve = shadowPass ? kMainPassReserveBytes : 0u;
  if (bytes64 + reserve > UINT32_MAX ||
      !frame.HasSRVSpace(static_cast<uint32_t>(bytes64 + reserve)) ||
      !frame.HasCBSpace(sizeof(Material))) {
    // フレームリソースが足りない。assert で落とさず、このバッチを諦めて数だけ記録する
    stats_.overflowSkipped += count;
    return;
  }

  void *instMapped = nullptr;
  const D3D12_GPU_VIRTUAL_ADDRESS instAddr =
      frame.AllocSRV(static_cast<uint32_t>(bytes64), &instMapped);
  auto *dst = static_cast<ModelResource::InstanceGPU *>(instMapped);

  // ※ 書き込み先はアップロードヒープ（CPU からは書き込み結合メモリ）。
  //    ここを「読む」とキャッシュが効かず 1 回ごとに非常に遅い（以前は WVP の計算で
  //    書き込み先の World を読み返しており、480 個で約 20ms かかっていた）。
  //    1 個分をスタック上で組み立ててから、書き込みだけを 1 回の memcpy で行う。
  for (uint32_t k = 0; k < itemFactor; ++k) {
    const Matrix4x4 *node = perItem ? &items[k].nodeWorld : nullptr;
    ModelResource::InstanceGPU *out = dst + static_cast<size_t>(k) * count;
    for (uint32_t i = 0; i < count; ++i) {
      const InstanceRef &ref = batch.instances[i];
      ModelResource::InstanceGPU tmp;
      if (node) {
        MulTo(*node, *ref.world, tmp.World);
      } else {
        tmp.World = *ref.world;
      }
      tmp.color = ref.color;
      if (shadowPass) {
        // 影パスの VS は World しか読まないので、WVP / 逆転置は計算しない（中身は使われない）
        tmp.WVP = tmp.World;
        tmp.WorldInverseTranspose = tmp.World;
      } else {
        MulTo(tmp.World, viewProj, tmp.WVP);
        tmp.WorldInverseTranspose =
            node ? Transpose(Inverse(tmp.World))
                 : (ref.worldInvTranspose ? *ref.worldInvTranspose : Transpose(Inverse(tmp.World)));
      }
      std::memcpy(&out[i], &tmp, sizeof(tmp));
    }
  }

  void *matMapped = nullptr;
  const D3D12_GPU_VIRTUAL_ADDRESS matAddr = frame.AllocCB(sizeof(Material), &matMapped);
  std::memcpy(matMapped, &batch.material, sizeof(Material));

  ++stats_.batches;
  stats_.instancesDrawn += count;
  stats_.drawCalls += static_cast<uint32_t>(items.empty() ? 1 : items.size());

  const uint64_t sortKey = SortKey::Make(SortKey::kLayerOpaque, SortKey::HashPSO("object3d_inst"), 0);
  const uint64_t textureOverride = batch.textureOverride;
  ctx.PushCommand3D(
      sortKey,
      [leader, matAddr, instAddr, count, perItem, textureOverride](ID3D12GraphicsCommandList *cl) {
        auto &rc = GetRenderContext();
        const BlendMode prevBlend = rc.CurrentBlendMode();
        rc.SetBlendMode(kBlendModeNone);

        // 同じモデルが別テクスチャで別バッチになっている場合に備え、描く直前に設定し直す
        // （DrawModel もラムダの中で ApplyTexture_ していたのと同じ）
        if (textureOverride != 0) {
          leader->SetTexture(D3D12_GPU_DESCRIPTOR_HANDLE{textureOverride});
        } else {
          leader->ResetTextureToMtl();
        }

        auto draw = [&](std::string_view prefix) {
          if (!rc.BindPipeline(prefix)) return;
          rc.BindCameraCB();
          rc.BindAllLightCBs();
          leader->Resource().DrawInstancesPrepared(cl, matAddr, instAddr, count, perItem);
        };

        const ViewShadingMode mode = rc.GetViewShadingMode();
        if (IsDebugShadingMode(mode)) {
          draw(ResolveInstPrefix(mode));
        } else {
          if (mode != ViewShadingMode::Wireframe) {
            draw("object3d_inst");
          }
          if (mode == ViewShadingMode::Wireframe || mode == ViewShadingMode::SolidWireframe) {
            draw("object3d_wire_inst");
          }
        }
        rc.SetBlendMode(prevBlend);
      },
      "Model(Instanced)", batch.leaderHandle);
}

} // namespace RC
