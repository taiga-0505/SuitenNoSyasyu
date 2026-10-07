#include "Sprite2D.h"
#include "Common/Log/Log.h"
#include <cassert>
#include <string>

using namespace RC;

Sprite2D::~Sprite2D() {
  Release();
}

void Sprite2D::Release() {
  mesh_.reset();

  // 定数バッファは DynamicCB（毎フレームの一時領域）なので解放するものは無い
}

void Sprite2D::Initialize(ID3D12Device *device,
                          const std::shared_ptr<SpriteMesh2D> &mesh,
                          float screenWidth, float screenHeight) {
  Release();
  device_ = device;
  mesh_ = mesh;
  screenW_ = screenWidth;
  screenH_ = screenHeight;

  // CB: WVP
  cbWVP_.dyn.Ptr()->World = MakeIdentity4x4();
  cbWVP_.dyn.Ptr()->WVP = MakeIdentity4x4();

  // CB: Material
  cbMat_.dyn.Ptr()->color = {1, 1, 1, 1};
  cbMat_.dyn.Ptr()->uvTransform = MakeIdentity4x4();

  // ビュー/プロジェクション（左上基準の直交）
  view_ = MakeIdentity4x4();
  proj_ = MakeOrthographicMatrix(0.0f, 0.0f, screenW_, screenH_, 0.0f, 100.0f);

  SetSize(transform_.scale.x, transform_.scale.y);
}

void Sprite2D::SetScreenSize(float w, float h) {
  screenW_ = w;
  screenH_ = h;
  proj_ = MakeOrthographicMatrix(0.0f, 0.0f, screenW_, screenH_, 0.0f, 100.0f);
}

void Sprite2D::SetSize(float w, float h) {
  // ワールド空間モードでは大きさは Transform.scale がそのまま意味を持つので上書きしない
  if (worldSpace_) {
    return;
  }
  transform_.scale.x = w;
  transform_.scale.y = h;
  transform_.scale.z = 1.0f;
}

void Sprite2D::Update() {
  if (worldSpace_) {
    // 共有クアッドは「左上原点・右下(1,1)・Y下向き」のスクリーン用なので、
    // 3D 用に「原点中心・1×1・Y上向き」へ補正してから Transform を掛ける。
    //   local.x = x - 0.5
    //   local.y = 0.5 - y   （Y反転）
    Matrix4x4 pre = MakeIdentity4x4();
    pre.m[1][1] = -1.0f; // Y反転
    pre.m[3][0] = -0.5f; // 中心合わせ
    pre.m[3][1] = 0.5f;

    Matrix4x4 world =
        Multiply(pre, MakeAffineMatrix(transform_.scale, transform_.rotation,
                                       transform_.translation));
    cbWVP_.dyn.Ptr()->World = world;
    cbWVP_.dyn.Ptr()->WVP = Multiply(world, Multiply(camView_, camProj_));
    return;
  }

  Matrix4x4 world = MakeAffineMatrix(transform_.scale, transform_.rotation,
                                     transform_.translation);
  cbWVP_.dyn.Ptr()->World = world;
  cbWVP_.dyn.Ptr()->WVP = Multiply(world, Multiply(view_, proj_));
}

void Sprite2D::Draw(ID3D12GraphicsCommandList *cmdList) const {
  if (!visible_ || srv_.ptr == 0)
    return;
  if (!mesh_ || !mesh_->Ready())
    return;

  assert(cmdList);

  const auto &vbv = mesh_->VBV();
  const auto &ibv = mesh_->IBV();

  cmdList->IASetVertexBuffers(0, 1, &vbv);
  cmdList->IASetIndexBuffer(&ibv);
  cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

  // RootParam: 0=Material, 1=WVP, 2=SRV
  cmdList->SetGraphicsRootConstantBufferView(
      0, cbMat_.dyn.Address());
  cmdList->SetGraphicsRootConstantBufferView(
      1, cbWVP_.dyn.Address());
  cmdList->SetGraphicsRootDescriptorTable(2, srv_);

  cmdList->DrawIndexedInstanced(mesh_->IndexCount(), 1, 0, 0, 0);
}
