#include "MainCamera.h"

using namespace RC;

void MainCamera::Initialize(const Vector3 &pos, const Vector3 &rot, float fovY,
                            float aspect, float nearZ, float farZ) {
  transform_.translation = pos;
  transform_.rotation = rot;
  transform_.scale = {1, 1, 1};
  fovY_ = fovY;
  nearZ_ = nearZ;
  farZ_ = farZ;

  // 射影行列は初期化時に一度だけ作成
  proj_ = MakePerspectiveFovMatrix(fovY, aspect, nearZ, farZ);
}

void MainCamera::Update() {
  // ----- ワールド行列を作成 -----
  // Transform からスケール・回転・並進を合成
  RC::Matrix4x4 world = MakeAffineMatrix(transform_.scale, transform_.rotation,
                                     transform_.translation);
  // ----- ビュー行列はワールド行列の逆行列 -----
  view_ = Inverse(world);
}

void MainCamera::SetProjection(float fovY, float aspect, float nearZ, float farZ) {
  fovY_ = fovY;
  nearZ_ = nearZ;
  farZ_ = farZ;
  proj_ = MakePerspectiveFovMatrix(fovY, aspect, nearZ, farZ);
}
