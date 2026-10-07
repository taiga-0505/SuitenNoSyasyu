#include "Primitive3D.h"
#include "Render/FrameResource.h"
#include <cassert>
#include <cmath>
#include <format>
#include "Common/Log/Log.h"

namespace {

inline RC::Vector3 VAdd(const RC::Vector3 &a, const RC::Vector3 &b) {
  return {a.x + b.x, a.y + b.y, a.z + b.z};
}
inline RC::Vector3 VSub(const RC::Vector3 &a, const RC::Vector3 &b) {
  return {a.x - b.x, a.y - b.y, a.z - b.z};
}
inline RC::Vector3 VMul(const RC::Vector3 &v, float s) {
  return {v.x * s, v.y * s, v.z * s};
}
inline float VDot(const RC::Vector3 &a, const RC::Vector3 &b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline RC::Vector3 VCross(const RC::Vector3 &a, const RC::Vector3 &b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
          a.x * b.y - a.y * b.x};
}
inline float VLenSq(const RC::Vector3 &v) { return VDot(v, v); }
inline float VLen(const RC::Vector3 &v) { return std::sqrtf(VLenSq(v)); }
inline RC::Vector3 VNorm(const RC::Vector3 &v) {
  const float l = VLen(v);
  if (l <= 1e-8f)
    return {0, 0, 0};
  return VMul(v, 1.0f / l);
}

inline void MakeOrthoBasis(const RC::Vector3 &nNormalized, RC::Vector3 &u,
                           RC::Vector3 &v) {
  // n は正規化済み前提
  const float ay = std::fabsf(nNormalized.y);
  const RC::Vector3 tmp = (ay < 0.99f) ? RC::Vector3{0, 1, 0} : RC::Vector3{1, 0, 0};
  u = VNorm(VCross(tmp, nNormalized));
  // tmp と n がほぼ平行で u が死んだら、別軸でやり直す
  if (VLenSq(u) <= 1e-8f) {
    const RC::Vector3 tmp2 = RC::Vector3{0, 0, 1};
    u = VNorm(VCross(tmp2, nNormalized));
  }
  v = VCross(nNormalized, u);
}

inline void AddCircleLines(Primitive3D *self, const RC::Vector3 &center,
                           const RC::Vector3 &u, const RC::Vector3 &v,
                           float radius, const RC::Vector4 &color,
                           int segments, bool depth) {
  constexpr float kPi = 3.14159265358979323846f;
  const float d = (2.0f * kPi) / static_cast<float>(segments);
  RC::Vector3 prev = VAdd(center, VMul(u, radius));
  for (int i = 1; i <= segments; ++i) {
    const float t = d * static_cast<float>(i);
    const RC::Vector3 dir = VAdd(VMul(u, std::cosf(t)), VMul(v, std::sinf(t)));
    const RC::Vector3 cur = VAdd(center, VMul(dir, radius));
    self->AddLine(prev, cur, color, depth);
    prev = cur;
  }
}

inline void AddHalfCircle(Primitive3D *self, const RC::Vector3 &center,
                          const RC::Vector3 &equatorDir, // u or v
                          const RC::Vector3 &poleDir,    // +axis or -axis
                          float radius, const RC::Vector4 &color,
                          int segments, bool depth) {
  // 0..PI の半円:  +equator -> pole -> -equator
  constexpr float kPi = 3.14159265358979323846f;
  const float d = kPi / static_cast<float>(segments);
  RC::Vector3 prev = VAdd(center, VMul(equatorDir, radius));
  for (int i = 1; i <= segments; ++i) {
    const float t = d * static_cast<float>(i);
    const RC::Vector3 dir = VAdd(VMul(equatorDir, std::cosf(t)),
                                 VMul(poleDir, std::sinf(t)));
    const RC::Vector3 cur = VAdd(center, VMul(dir, radius));
    self->AddLine(prev, cur, color, depth);
    prev = cur;
  }
}

} // namespace

Primitive3D::~Primitive3D() { Term(); }

void Primitive3D::Term() {
  if (vb_) {
    if (vbMap_)
      vb_->Unmap(0, nullptr);
    vb_.Reset();
    vbMap_ = nullptr;
    vbCapacity_ = 0;
  }
  if (cbRes_) {
    if (cbMap_)
      cbRes_->Unmap(0, nullptr);
    cbRes_.Reset();
    cbMap_ = nullptr;
  }
  device_.Reset();
  Clear();
}

void Primitive3D::Initialize(ID3D12Device *device) {
  Term();
  device_ = device;

  cbStride_ = Align256((uint32_t)sizeof(PerFrameCB));
  cbRes_ = CreateBufferResource(device_.Get(), cbStride_ * kMaxDrawPerFrame, L"Primitive3D::cbRes_");
  cbRes_->Map(0, nullptr, reinterpret_cast<void **>(&cbMap_));

  // 初期VB（とりあえず 4096 頂点）
  EnsureVB_(4096);
}

void Primitive3D::BeginFrame(const RC::Matrix4x4 &view,
                             const RC::Matrix4x4 &proj, BlendMode blendAt3D) {
  perFrame_.View = view;
  perFrame_.Proj = proj;
  blendAt3D_ = blendAt3D;

  cbCursor_ = 0;
  // Note: Clear() is intentionally not called here because primitive vertices 
  // added during ImGui rendering in the previous frame must survive until Execute3DCommands.
  // Execute3DCommands() will clear them after drawing.
}

void Primitive3D::Clear() {
  vtxDepth_.clear();
  vtxNoDepth_.clear();
}

void Primitive3D::EnsureVB_(size_t vertexCount) {
  if (vb_ && vbCapacity_ >= vertexCount)
    return;

  // 既存解放
  if (vb_) {
    if (vbMap_)
      vb_->Unmap(0, nullptr);
    vb_.Reset();
    vbMap_ = nullptr;
    vbCapacity_ = 0;
  }

  // ちょい余裕持たせる（伸びた時の再確保回数を減らす）
  vbCapacity_ = (vertexCount < 4096) ? 4096 : vertexCount;
  vb_ = CreateBufferResource(device_.Get(), sizeof(Vertex) * vbCapacity_, L"Primitive3D::vb_");
  vb_->Map(0, nullptr, reinterpret_cast<void **>(&vbMap_));

  vbView_.BufferLocation = vb_->GetGPUVirtualAddress();
  vbView_.StrideInBytes = (UINT)sizeof(Vertex);
  // SizeInBytes は Draw の直前に “実際の頂点数” で上書きする
}

void Primitive3D::AddLine(const RC::Vector3 &a, const RC::Vector3 &b,
                          const RC::Vector4 &color, bool depth) {
  auto &v = depth ? vtxDepth_ : vtxNoDepth_;
  v.push_back({a, color});
  v.push_back({b, color});
}

void Primitive3D::AddAABB(const RC::Vector3 &mn, const RC::Vector3 &mx,
                          const RC::Vector4 &c, bool depth) {
  RC::Vector3 p[8] = {
      {mn.x, mn.y, mn.z}, {mx.x, mn.y, mn.z}, {mx.x, mx.y, mn.z},
      {mn.x, mx.y, mn.z}, {mn.x, mn.y, mx.z}, {mx.x, mn.y, mx.z},
      {mx.x, mx.y, mx.z}, {mn.x, mx.y, mx.z},
  };
  const int e[24] = {0, 1, 1, 2, 2, 3, 3, 0, 4, 5, 5, 6,
                     6, 7, 7, 4, 0, 4, 1, 5, 2, 6, 3, 7};
  for (int i = 0; i < 24; i += 2) {
    AddLine(p[e[i]], p[e[i + 1]], c, depth);
  }
}

void Primitive3D::AddGridXZ(int halfSize, float step, const RC::Vector4 &c,
                            bool depth) {
  const float max = halfSize * step;
  for (int i = -halfSize; i <= halfSize; ++i) {
    float x = i * step;
    AddLine({x, 0, -max}, {x, 0, +max}, c, depth);
    float z = i * step;
    AddLine({-max, 0, z}, {+max, 0, z}, c, depth);
  }
}

void Primitive3D::AddGridXY(int halfSize, float step, const RC::Vector4 &color,
                            bool depth) {
  const float max = halfSize * step;
  for (int i = -halfSize; i <= halfSize; ++i) {
    float x = i * step;
    AddLine({x, -max, 0}, {x, +max, 0}, color, depth);
    float y = i * step;
    AddLine({-max, y, 0}, {+max, y, 0}, color, depth);
  }
}

void Primitive3D::AddGridYZ(int halfSize, float step, const RC::Vector4 &color,
                            bool depth) {
  const float max = halfSize * step;
  for (int i = -halfSize; i <= halfSize; ++i) {
    float y = i * step;
    AddLine({0, y, -max}, {0, y, +max}, color, depth);
    float z = i * step;
    AddLine({0, -max, z}, {0, +max, z}, color, depth);
  }
}

// ============================================================================
// 便利形状（デバッグ用）
// ============================================================================

void Primitive3D::AddSphere(const RC::Vector3 &center, float radius,
                            const RC::Vector4 &color, int slices, int stacks,
                            bool depth) {
  if (radius <= 0.0f)
    return;
  if (slices < 3)
    slices = 3;
  if (stacks < 2)
    stacks = 2;

  // (stacks+1) x (slices+1) の点を作る（最後の列はシーム用）
  std::vector<RC::Vector3> pts;
  pts.resize(static_cast<size_t>(stacks + 1) *
             static_cast<size_t>(slices + 1));

  constexpr float kPi = 3.14159265358979323846f;
  const float dPhi = kPi / static_cast<float>(stacks);
  const float dTheta = (2.0f * kPi) / static_cast<float>(slices);

  for (int i = 0; i <= stacks; ++i) {
    const float phi = -0.5f * kPi + dPhi * static_cast<float>(i);
    const float y = radius * std::sinf(phi);
    const float r = radius * std::cosf(phi);
    for (int j = 0; j <= slices; ++j) {
      const float th = dTheta * static_cast<float>(j);
      const float x = r * std::cosf(th);
      const float z = r * std::sinf(th);
      const size_t idx = static_cast<size_t>(i) *
                             static_cast<size_t>(slices + 1) +
                         static_cast<size_t>(j);
      pts[idx] = {center.x + x, center.y + y, center.z + z};
    }
  }

  // 緯線（リング）
  for (int i = 0; i <= stacks; ++i) {
    const size_t base = static_cast<size_t>(i) *
                        static_cast<size_t>(slices + 1);
    for (int j = 0; j < slices; ++j) {
      AddLine(pts[base + static_cast<size_t>(j)],
              pts[base + static_cast<size_t>(j + 1)], color, depth);
    }
  }

  // 経線（縦）
  for (int j = 0; j <= slices; ++j) {
    for (int i = 0; i < stacks; ++i) {
      const size_t a = static_cast<size_t>(i) *
                           static_cast<size_t>(slices + 1) +
                       static_cast<size_t>(j);
      const size_t b = static_cast<size_t>(i + 1) *
                           static_cast<size_t>(slices + 1) +
                       static_cast<size_t>(j);
      AddLine(pts[a], pts[b], color, depth);
    }
  }
}

void Primitive3D::AddSphereRings(const RC::Vector3 &center, float radius,
                                 const RC::Vector4 &color, int segments,
                                 bool depth) {
  if (radius <= 0.0f)
    return;
  if (segments < 3)
    segments = 3;

  // XY
  AddCircleLines(this, center, {1, 0, 0}, {0, 1, 0}, radius, color, segments,
                 depth);
  // XZ
  AddCircleLines(this, center, {1, 0, 0}, {0, 0, 1}, radius, color, segments,
                 depth);
  // YZ
  AddCircleLines(this, center, {0, 1, 0}, {0, 0, 1}, radius, color, segments,
                 depth);
}

void Primitive3D::AddArc(const RC::Vector3 &center, const RC::Vector3 &normal,
                         const RC::Vector3 &fromDir, float radius,
                         float startRad, float endRad, const RC::Vector4 &color,
                         int segments, bool depth, bool drawToCenter) {
  if (radius <= 0.0f)
    return;
  if (segments < 1)
    segments = 1;

  RC::Vector3 n = VNorm(normal);
  if (VLenSq(n) <= 1e-8f)
    return;

  // fromDir から normal 成分を除去して面内に落とす
  RC::Vector3 u = VSub(fromDir, VMul(n, VDot(fromDir, n)));
  u = VNorm(u);
  if (VLenSq(u) <= 1e-8f) {
    RC::Vector3 v;
    MakeOrthoBasis(n, u, v);
  }
  RC::Vector3 v = VCross(n, u);

  const float range = (endRad - startRad);
  const float d = range / static_cast<float>(segments);

  RC::Vector3 prev;
  {
    const float t = startRad;
    const RC::Vector3 dir = VAdd(VMul(u, std::cosf(t)), VMul(v, std::sinf(t)));
    prev = VAdd(center, VMul(dir, radius));
  }

  for (int i = 1; i <= segments; ++i) {
    const float t = startRad + d * static_cast<float>(i);
    const RC::Vector3 dir = VAdd(VMul(u, std::cosf(t)), VMul(v, std::sinf(t)));
    const RC::Vector3 cur = VAdd(center, VMul(dir, radius));
    AddLine(prev, cur, color, depth);
    prev = cur;
  }

  if (drawToCenter) {
    const float t0 = startRad;
    const float t1 = endRad;
    const RC::Vector3 d0 = VAdd(VMul(u, std::cosf(t0)), VMul(v, std::sinf(t0)));
    const RC::Vector3 d1 = VAdd(VMul(u, std::cosf(t1)), VMul(v, std::sinf(t1)));
    AddLine(center, VAdd(center, VMul(d0, radius)), color, depth);
    AddLine(center, VAdd(center, VMul(d1, radius)), color, depth);
  }
}

void Primitive3D::AddCapsule(const RC::Vector3 &p0, const RC::Vector3 &p1,
                             float radius, const RC::Vector4 &color,
                             int segments, bool depth) {
  if (radius <= 0.0f)
    return;
  if (segments < 3)
    segments = 3;

  const RC::Vector3 axis = VSub(p1, p0);
  const float len = VLen(axis);
  if (len <= 1e-6f) {
    // ほぼ球
    AddSphereRings(p0, radius, color, segments, depth);
    return;
  }

  const RC::Vector3 n = VMul(axis, 1.0f / len); // p0->p1
  RC::Vector3 u, v;
  MakeOrthoBasis(n, u, v);

  // 端のリング
  AddCircleLines(this, p0, u, v, radius, color, segments, depth);
  AddCircleLines(this, p1, u, v, radius, color, segments, depth);

  // 側面の縦ライン（4本）
  const RC::Vector3 dirs[4] = {u, v, VMul(u, -1.0f), VMul(v, -1.0f)};
  for (int i = 0; i < 4; ++i) {
    AddLine(VAdd(p0, VMul(dirs[i], radius)), VAdd(p1, VMul(dirs[i], radius)),
            color, depth);
  }

  // 半球（u 平面 / v 平面 の 2本ずつ）
  const RC::Vector3 pole0 = VMul(n, -1.0f); // p0 側は -axis
  const RC::Vector3 pole1 = n;              // p1 側は +axis
  AddHalfCircle(this, p0, u, pole0, radius, color, segments, depth);
  AddHalfCircle(this, p0, v, pole0, radius, color, segments, depth);
  AddHalfCircle(this, p1, u, pole1, radius, color, segments, depth);
  AddHalfCircle(this, p1, v, pole1, radius, color, segments, depth);
}

void Primitive3D::AddOBB(const RC::Vector3 &center, const RC::Vector3 &axisX,
                         const RC::Vector3 &axisY, const RC::Vector3 &axisZ,
                         const RC::Vector3 &halfExtents,
                         const RC::Vector4 &color, bool depth) {
  RC::Vector3 ax = VNorm(axisX);
  RC::Vector3 ay = VNorm(axisY);
  RC::Vector3 az = VNorm(axisZ);
  if (VLenSq(ax) <= 1e-8f || VLenSq(ay) <= 1e-8f || VLenSq(az) <= 1e-8f) {
    return;
  }

  const RC::Vector3 ex = VMul(ax, halfExtents.x);
  const RC::Vector3 ey = VMul(ay, halfExtents.y);
  const RC::Vector3 ez = VMul(az, halfExtents.z);

  RC::Vector3 p[8] = {
      VSub(VSub(VSub(center, ex), ey), ez), // 0
      VSub(VSub(VAdd(center, ex), ey), ez), // 1
      VSub(VAdd(VAdd(center, ex), ey), ez), // 2
      VSub(VAdd(VSub(center, ex), ey), ez), // 3
      VAdd(VSub(VSub(center, ex), ey), ez), // 4
      VAdd(VSub(VAdd(center, ex), ey), ez), // 5
      VAdd(VAdd(VAdd(center, ex), ey), ez), // 6
      VAdd(VAdd(VSub(center, ex), ey), ez), // 7
  };

  const int e[24] = {0, 1, 1, 2, 2, 3, 3, 0, 4, 5, 5, 6,
                     6, 7, 7, 4, 0, 4, 1, 5, 2, 6, 3, 7};
  for (int i = 0; i < 24; i += 2) {
    AddLine(p[e[i]], p[e[i + 1]], color, depth);
  }
}

void Primitive3D::AddFrustum(const RC::Vector3 corners[8],
                             const RC::Vector4 &color, bool depth) {
  if (!corners)
    return;
  const int e[24] = {
      0, 1, 1, 2, 2, 3, 3, 0, // near
      4, 5, 5, 6, 6, 7, 7, 4, // far
      0, 4, 1, 5, 2, 6, 3, 7  // sides
  };
  for (int i = 0; i < 24; i += 2) {
    AddLine(corners[e[i]], corners[e[i + 1]], color, depth);
  }
}

void Primitive3D::AddFrustumCamera(const RC::Vector3 &camPos,
                                   const RC::Vector3 &forward,
                                   const RC::Vector3 &up, float fovYRad,
                                   float aspect, float nearZ, float farZ,
                                   const RC::Vector4 &color, bool depth) {
  // forward/up から right を作って、near/far の平面コーナーを算出
  RC::Vector3 f = VNorm(forward);
  RC::Vector3 u = VNorm(up);
  if (VLenSq(f) <= 1e-8f || VLenSq(u) <= 1e-8f)
    return;

  RC::Vector3 r = VNorm(VCross(f, u));
  // up が forward とほぼ平行なら作り直し
  if (VLenSq(r) <= 1e-8f) {
    RC::Vector3 uu, vv;
    MakeOrthoBasis(f, uu, vv);
    r = uu;
    u = vv;
  } else {
    // 正規直交化（u を作り直す）
    u = VCross(r, f);
  }

  const float tanHalf = std::tanf(fovYRad * 0.5f);
  const float nh = 2.0f * tanHalf * nearZ;
  const float nw = nh * aspect;
  const float fh = 2.0f * tanHalf * farZ;
  const float fw = fh * aspect;

  const RC::Vector3 nc = VAdd(camPos, VMul(f, nearZ));
  const RC::Vector3 fc = VAdd(camPos, VMul(f, farZ));

  const RC::Vector3 nu = VMul(u, nh * 0.5f);
  const RC::Vector3 nr = VMul(r, nw * 0.5f);
  const RC::Vector3 fu = VMul(u, fh * 0.5f);
  const RC::Vector3 fr = VMul(r, fw * 0.5f);

  RC::Vector3 c[8] = {
      VSub(VSub(nc, nr), nu), // 0 nearLB
      VAdd(VSub(nc, nr), nu), // 1 nearLT
      VAdd(VAdd(nc, nr), nu), // 2 nearRT
      VSub(VAdd(nc, nr), nu), // 3 nearRB
      VSub(VSub(fc, fr), fu), // 4 farLB
      VAdd(VSub(fc, fr), fu), // 5 farLT
      VAdd(VAdd(fc, fr), fu), // 6 farRT
      VSub(VAdd(fc, fr), fu), // 7 farRB
  };
  AddFrustum(c, color, depth);
}

void Primitive3D::TransferVertices() {
  size_t total = vtxDepth_.size() + vtxNoDepth_.size();
  if (total == 0 || !device_)
    return;

  // 頂点は今フレームの一時領域（FrameResource）へ毎回書く。
  // 以前は 1 本の固定 VB に上書きしていたため、CPU と GPU を並行させると
  // GPU が前フレームの線を描いている最中に次フレームの頂点で上書きしてしまう。
  // （同じフレームで 2 回転送するとき＝メイン 3D とオーバーレイでも、前の転送分を壊さない）
  const uint64_t bytes64 = sizeof(Vertex) * total;
  auto &frame = RC::CurrentFrameResource();
  if (bytes64 > UINT32_MAX || !frame.HasSRVSpace(static_cast<uint32_t>(bytes64))) {
    vbView_ = {}; // 容量不足：今回の線は描かない
    return;
  }
  void *dst = nullptr;
  const D3D12_GPU_VIRTUAL_ADDRESS addr = frame.AllocSRV(static_cast<uint32_t>(bytes64), &dst);
  auto *vb = static_cast<Vertex *>(dst);

  // vtxDepth_ を先に、vtxNoDepth_ を後にコピーする
  if (!vtxDepth_.empty()) {
    std::memcpy(vb, vtxDepth_.data(), sizeof(Vertex) * vtxDepth_.size());
  }
  if (!vtxNoDepth_.empty()) {
    std::memcpy(vb + vtxDepth_.size(), vtxNoDepth_.data(),
                sizeof(Vertex) * vtxNoDepth_.size());
  }

  vbView_.BufferLocation = addr;
  vbView_.StrideInBytes = (UINT)sizeof(Vertex);
  vbView_.SizeInBytes = (UINT)bytes64;
}

void Primitive3D::DrawRange(ID3D12GraphicsCommandList *cl, bool depth,
                            uint32_t start, uint32_t count) {
  if (count == 0 || !cl || !vbView_.BufferLocation)
    return;

  // 行列 CB は今フレームの一時領域へ送る（固定 CB の使い回しだと、GPU が前フレームを描いている最中に上書きする）
  const D3D12_GPU_VIRTUAL_ADDRESS cbAddr = RC::UploadFrameCB(perFrame_);

  cl->IASetVertexBuffers(0, 1, &vbView_);
  cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);

  // RootParam[0] = b0(VS)
  cl->SetGraphicsRootConstantBufferView(0, cbAddr);

  uint32_t offset = depth ? start : (uint32_t)vtxDepth_.size() + start;
  cl->DrawInstanced(count, 1, offset, 0);
}

uint32_t Primitive3D::GetVertexCount(bool depth) const {
  return (uint32_t)(depth ? vtxDepth_.size() : vtxNoDepth_.size());
}

void Primitive3D::Draw(ID3D12GraphicsCommandList *cl, bool depth) {
  auto &v = depth ? vtxDepth_ : vtxNoDepth_;
  if (v.empty() || !cl || !device_)
    return;

  // 旧方式との互換性用
  TransferVertices();
  uint32_t count = (uint32_t)v.size();
  DrawRange(cl, depth, 0, count);

  // 描いた分は消してOK（“そのフレームだけ”）
  v.clear();
}

