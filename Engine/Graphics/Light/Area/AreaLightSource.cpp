#include "Light/Area/AreaLightSource.h"
#include <cmath>
#include <string>

namespace RC {

/// @brief ベクトルを正規化する（長さがほぼ 0 なら +X を返す）
/// @param v 正規化するベクトル
/// @return 正規化したベクトル
static Vector3 NormalizeSafe_(const Vector3 &v) {
  const float len2 = v.x * v.x + v.y * v.y + v.z * v.z;
  if (len2 <= 1e-12f) {
    return {1.0f, 0.0f, 0.0f};
  }
  const float invLen = 1.0f / std::sqrt(len2);
  return {v.x * invLen, v.y * invLen, v.z * invLen};
}

AreaLightSource::AreaLightSource() {
  data_.color = {1, 1, 1, 1};
  data_.position = {0.0f, 2.0f, 0.0f};
  data_.intensity = 1.0f;

  // 既定は X右 / Y上 の板
  data_.right = {1.0f, 0.0f, 0.0f};
  data_.up = {0.0f, 1.0f, 0.0f};
  data_.halfWidth = 0.5f;
  data_.halfHeight = 0.5f;

  data_.range = 10.0f;
  data_.decay = 2.0f;
  data_.twoSided = 0;
}

void AreaLightSource::SetBasis(const Vector3 &right, const Vector3 &up,
                              bool normalize) {
  if (normalize) {
    data_.right = NormalizeSafe_(right);
    data_.up = NormalizeSafe_(up);
  } else {
    data_.right = right;
    data_.up = up;
  }
}

void AreaLightSource::SetSize(float width, float height) {
  data_.halfWidth = width * 0.5f;
  data_.halfHeight = height * 0.5f;
}

::AreaLight AreaLightSource::DataForGPU() const {
  ::AreaLight out = data_;
  if (!enabled_) {
    out.intensity = 0.0f; // シェーダ側が intensity<=0 を continue する
  }
  return out;
}

} // namespace RC
