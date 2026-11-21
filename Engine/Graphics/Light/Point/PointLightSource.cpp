#include "PointLightSource.h"
#include <string>

namespace RC {

PointLightSource::PointLightSource() {
  data_.color = {1, 1, 1, 1};
  data_.position = {0.0f, 2.0f, 0.0f};
  data_.intensity = 1.0f;
  data_.radius = 10.0f;
  data_.decay = 2.0f;
}

::PointLight PointLightSource::DataForGPU() const {
  ::PointLight out = data_;
  if (!enabled_) {
    out.intensity = 0.0f;
  }
  return out;
}

} // namespace RC
