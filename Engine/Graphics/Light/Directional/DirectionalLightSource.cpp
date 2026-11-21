#include "DirectionalLightSource.h"
#include <cmath>
#include <string>

namespace RC {

DirectionalLightSource::DirectionalLightSource() {
  // デフォルトは上からの白いライト
  data_.color = {1.0f, 1.0f, 1.0f, 1.0f};
  data_.direction = {0.0f, -1.0f, 0.0f};
  data_.intensity = 0.0f; // デフォルトで照らさないようにする
  data_.ambientColor = {1.0f, 1.0f, 1.0f};
  data_.ambientIntensity = 0.0f; // デフォルトは環境光なし（ライトの当たった所だけ見える）
}

void DirectionalLightSource::SetDirection(const Vector3 &dir, bool normalize) {
  Vector3 d = dir;
  if (normalize) {
    float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    if (len > 1e-6f) {
      d.x /= len;
      d.y /= len;
      d.z /= len;
    }
  }
  data_.direction = d;
}

void DirectionalLightSource::SetColor(const Vector3 &rgb, float alpha) {
  data_.color = {rgb.x, rgb.y, rgb.z, alpha};
}

void DirectionalLightSource::SetColor(const Vector4 &rgba) {
  data_.color = rgba;
}

void DirectionalLightSource::SetIntensity(float intensity) {
  data_.intensity = intensity;
}

void DirectionalLightSource::SetAmbient(const Vector3 &rgb, float intensity) {
  data_.ambientColor = rgb;
  data_.ambientIntensity = intensity;
}

DirectionalLight DirectionalLightSource::DataForGPU() const {
  DirectionalLight out = data_;
  if (!enabled_) {
    out.intensity = 0.0f;
  }
  return out;
}

} // namespace RC
