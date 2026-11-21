#include "Primitive2D.h"
#include "RenderCommon.h" // RC::LoadTex / RC::GetSrv
#include <cassert>
#include <cstring>

namespace {
int sWhiteTexHandle = -1; ///< 既定テクスチャ（白 1x1）のハンドル（未読み込みなら -1）

/// @brief テクスチャ未設定時に使う白 1x1 テクスチャの SRV を取得する（初回呼び出しで読み込む）
/// @return 白テクスチャの GPU ハンドル（読み込み失敗時は {0}）
D3D12_GPU_DESCRIPTOR_HANDLE GetWhiteSrv_() {
  if (sWhiteTexHandle < 0) {
    sWhiteTexHandle = RC::LoadTex("Resources/white1x1.png");
    // LoadTex は RC::Init 前だと -1 を返す
  }
  return RC::GetSrv(sWhiteTexHandle); // 失敗時は {0} が返る
}
} // namespace

using namespace RC;

/// @brief 画面全体を覆う 1 枚の三角形を頂点バッファへ書き込む
/// @param vb 書き込み先の頂点バッファ（Vertex 3 個分）
static void WriteFullTri(ID3D12Resource *vb) {
  Primitive2D::Vertex *v = nullptr;
  vb->Map(0, nullptr, reinterpret_cast<void **>(&v));

  // フルスクリーン三角形（clip 座標）
  v[0] = {{-1, -1, 0, 1}, {0, 1}};
  v[1] = {{-1, 3, 0, 1}, {0, -1}};
  v[2] = {{3, -1, 0, 1}, {2, 1}};

  vb->Unmap(0, nullptr);
}

Primitive2D::~Primitive2D() { Release(); }

void Primitive2D::Release() {
  if (vb_.res) {
    vb_.res.Reset();
  }
  if (cbParamsRes_) {
    cbParamsRes_.Reset();
    cbParamsMap_ = nullptr;
  }
  if (cbParams_.res) {
    cbParams_.res.Reset();
    cbParams_.map = nullptr;
  }
  if (cbDummy_.res) {
    cbDummy_.res.Reset();
    cbDummy_.map = nullptr;
  }
}

void Primitive2D::Initialize(ID3D12Device *device, float screenW,
                             float screenH) {
  Release();
  device_ = device;
  screenW_ = screenW;
  screenH_ = screenH;

  cbStride_ = Align256((uint32_t)sizeof(Params));

  // リング分まとめて確保
  // フレーム領域 × kFrameCount。前のフレームを GPU が読んでいる間に上書きしないため
  cbParamsRes_ = CreateBufferResource(device_.Get(), cbStride_ * kMaxDrawPerFrame * kFrameCount,
                                      L"Primitive2D::cbParamsRes_");
  frameSlot_ = 0;
  cbCursor_ = 0;
  cbParamsRes_->Map(0, nullptr, reinterpret_cast<void **>(&cbParamsMap_));

  // CPU 側初期化
  paramsCPU_ = {};
  paramsCPU_.C = {screenW_, screenH_, 1.0f, 1.0f};
  paramsCPU_.Color = {1, 1, 1, 1};
  paramsCPU_.UVRect = {0, 0, 1, 1};
  paramsCPU_.U = {SHAPE_LINE, 0, 0, 0};

  // ダミーの VS 用 CB は今まで通り
  cbDummy_.res = CreateBufferResource(device_.Get(), sizeof(DummyVS), L"Primitive2D::cbDummy_.res");
  cbDummy_.res->Map(0, nullptr, reinterpret_cast<void **>(&cbDummy_.map));
  cbDummy_.map->World = MakeIdentity4x4();
  cbDummy_.map->WVP = MakeIdentity4x4();

  // VB は今まで通り
  vb_.res = CreateBufferResource(device_.Get(), sizeof(Vertex) * 3, L"Primitive2D::vb_.res");
  WriteFullTri(vb_.res.Get());
  vb_.view.BufferLocation = vb_.res->GetGPUVirtualAddress();
  vb_.view.SizeInBytes = sizeof(Vertex) * 3;
  vb_.view.StrideInBytes = sizeof(Vertex);

  if (srv_.ptr == 0) {
    srv_ = GetWhiteSrv_();
  }
}

void Primitive2D::SetScreenSize(float w, float h) {
  screenW_ = w;
  screenH_ = h;
  paramsCPU_.C.x = w;
  paramsCPU_.C.y = h;
}

void Primitive2D::SetLine(const Vector2 &p0, const Vector2 &p1, float thickness,
                          const Vector4 &color, float feather) {
  paramsCPU_.U = {SHAPE_LINE, 0, 0, 0};
  paramsCPU_.A = {p0.x, p0.y, p1.x, p1.y};
  paramsCPU_.C.z = thickness;
  paramsCPU_.C.w = feather;
  paramsCPU_.Color = color;
}

void Primitive2D::SetRect(const RC::Vector2 &mn, const RC::Vector2 &mx,
                          kFillMode fillMode, float thickness,
                          const RC::Vector4 &color, float feather) {
  const bool stroke = (fillMode == kWire);
  paramsCPU_.U = {SHAPE_RECT, stroke ? FLAG_STROKE : 0u, 0, 0};
  paramsCPU_.B = {mn.x, mn.y, mx.x, mx.y};
  paramsCPU_.C.z = thickness;
  paramsCPU_.C.w = feather;
  paramsCPU_.Color = color;
}

void Primitive2D::SetCircle(const RC::Vector2 &c, float r,
                            kFillMode fillMode, float thickness,
                            const RC::Vector4 &color, float feather) {
  const bool stroke = (fillMode == kWire);
  paramsCPU_.U = {SHAPE_CIRCLE, stroke ? FLAG_STROKE : 0u, 0, 0};
  paramsCPU_.D = {c.x, c.y, r, 0.0f};
  paramsCPU_.C.z = thickness;
  paramsCPU_.C.w = feather;
  paramsCPU_.Color = color;
}

void Primitive2D::SetTriangle(const RC::Vector2 &p0, const RC::Vector2 &p1,
                              const RC::Vector2 &p2, kFillMode fillMode,
                              float thickness, const RC::Vector4 &color,
                              float feather) {
  const bool stroke = (fillMode == kWire);
  paramsCPU_.U = {SHAPE_TRIANGLE, stroke ? FLAG_STROKE : 0u, 0, 0};
  paramsCPU_.A = {p0.x, p0.y, p1.x, p1.y};
  // B.xy に p2 を入れる（B.zw は未使用）
  paramsCPU_.B = {p2.x, p2.y, 0.0f, 0.0f};
  paramsCPU_.C.z = thickness;
  paramsCPU_.C.w = feather;
  paramsCPU_.Color = color;
}

void Primitive2D::SetSpriteRect(const Vector2 &mn, const Vector2 &mx,
                                const Vector4 &color, const Vector2 &uvMin,
                                const Vector2 &uvMax) {
  paramsCPU_.U = {SHAPE_SPRITE, FLAG_TEX, 0, 0};
  paramsCPU_.B = {mn.x, mn.y, mx.x, mx.y}; // PS 側で使う想定
  paramsCPU_.UVRect = {uvMin.x, uvMin.y, uvMax.x, uvMax.y};
  paramsCPU_.Color = color;
}

void Primitive2D::BeginFrame() {
  // 以前は毎フレーム同じ領域の先頭（スロット 0）から書き直していた。
  // GPU は最大 2 フレーム遅れて前フレームのコマンドを実行するので、
  // 前フレームのレティクル等のパラメータが次フレームの別の図形で上書きされ、
  // 図形が消える・別の色や大きさで出る・ちらつく、が起きていた。
  frameSlot_ = (frameSlot_ + 1) % kFrameCount;
  cbCursor_ = 0;
}

void Primitive2D::Draw(ID3D12GraphicsCommandList *cmdList) {
  if (!visible_)
    return;

  // 上限を超えたら現フレーム領域の中で折り返す（他フレームの領域には書かない）
  const uint32_t local =
      (cbCursor_ < kMaxDrawPerFrame) ? cbCursor_++ : (cbCursor_ = 1, 0);
  const uint32_t idx = frameSlot_ * kMaxDrawPerFrame + local;

  // Draw ごとに別スロットへ書く（同じスロットを使い回すと、GPU が読む前に次の図形で上書きされる）
  std::memcpy(cbParamsMap_ + idx * cbStride_, &paramsCPU_, sizeof(Params));

  const D3D12_GPU_VIRTUAL_ADDRESS gpuAddr =
      cbParamsRes_->GetGPUVirtualAddress() + idx * cbStride_;

  cmdList->IASetVertexBuffers(0, 1, &vb_.view);
  cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

  cmdList->SetGraphicsRootConstantBufferView(0, gpuAddr);
  cmdList->SetGraphicsRootConstantBufferView(
      1, cbDummy_.res->GetGPUVirtualAddress());
  cmdList->SetGraphicsRootDescriptorTable(2, srv_);

  cmdList->DrawInstanced(3, 1, 0, 0);
}
