#include "SpriteMesh2D.h"

/// @brief 単位クアッドの 4 頂点を頂点バッファへ書き込む
/// @param vb 書き込み先の頂点バッファ（SpriteVertex 4 個分）
static void WriteQuadVertices_(ID3D12Resource *vb) {
  SpriteVertex *v = nullptr;
  vb->Map(0, nullptr, reinterpret_cast<void **>(&v));

  // 左下、左上、右下、右上（UV は上 0、下 1）
  v[0].position = {0.0f, 1.0f, 0.0f, 1.0f};
  v[0].texcoord = {0.0f, 1.0f};

  v[1].position = {0.0f, 0.0f, 0.0f, 1.0f};
  v[1].texcoord = {0.0f, 0.0f};

  v[2].position = {1.0f, 1.0f, 0.0f, 1.0f};
  v[2].texcoord = {1.0f, 1.0f};

  v[3].position = {1.0f, 0.0f, 0.0f, 1.0f};
  v[3].texcoord = {1.0f, 0.0f};

  vb->Unmap(0, nullptr);
}

/// @brief 単位クアッド（三角形 2 枚）の 6 インデックスをインデックスバッファへ書き込む
/// @param ib 書き込み先のインデックスバッファ（uint32_t 6 個分）
static void WriteQuadIndices_(ID3D12Resource *ib) {
  uint32_t *idx = nullptr;
  ib->Map(0, nullptr, reinterpret_cast<void **>(&idx));

  idx[0] = 0;
  idx[1] = 1;
  idx[2] = 2;
  idx[3] = 1;
  idx[4] = 3;
  idx[5] = 2;

  ib->Unmap(0, nullptr);
}

SpriteMesh2D::~SpriteMesh2D() { Release_(); }

void SpriteMesh2D::Release_() {
  if (vb_) {
    vb_.Reset();
  }
  if (ib_) {
    ib_.Reset();
  }
  device_ = nullptr;
  vbv_ = {};
  ibv_ = {};
}

bool SpriteMesh2D::Initialize(ID3D12Device *device) {
  Release_();
  device_ = device;

  // VB（4 頂点）
  vb_ = CreateBufferResource(device_.Get(), sizeof(SpriteVertex) * 4, L"SpriteMesh2D::vb_");
  WriteQuadVertices_(vb_.Get());
  vbv_.BufferLocation = vb_->GetGPUVirtualAddress();
  vbv_.SizeInBytes = sizeof(SpriteVertex) * 4;
  vbv_.StrideInBytes = sizeof(SpriteVertex);

  // IB（6 インデックス）
  ib_ = CreateBufferResource(device_.Get(), sizeof(uint32_t) * 6, L"SpriteMesh2D::ib_");
  WriteQuadIndices_(ib_.Get());
  ibv_.BufferLocation = ib_->GetGPUVirtualAddress();
  ibv_.SizeInBytes = sizeof(uint32_t) * 6;
  ibv_.Format = DXGI_FORMAT_R32_UINT;

  return Ready();
}
