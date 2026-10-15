#include "MeshGenerator.h"
#include "Math/Math.h"
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

namespace RC {

ModelData MeshGenerator::GeneratePlane(float width, float height,
                                       uint32_t segmentsW, uint32_t segmentsH) {
  ModelData data;
  float halfW = width * 0.5f;
  float halfH = height * 0.5f;

  for (uint32_t y = 0; y <= segmentsH; ++y) {
    for (uint32_t x = 0; x <= segmentsW; ++x) {
      float u = (float)x / segmentsW;
      float v = (float)y / segmentsH;
      VertexData vtx;
      vtx.position = {u * width - halfW, 0.0f, (1.0f - v) * height - halfH, 1.0f};
      vtx.texcoord = {u, v};
      vtx.normal = {0.0f, 1.0f, 0.0f};
      data.vertices.push_back(vtx);
    }
  }

  for (uint32_t y = 0; y < segmentsH; ++y) {
    for (uint32_t x = 0; x < segmentsW; ++x) {
      uint32_t lb = y * (segmentsW + 1) + x;
      uint32_t rb = lb + 1;
      uint32_t lt = (y + 1) * (segmentsW + 1) + x;
      uint32_t rt = lt + 1;

      data.indices.push_back(lb);
      data.indices.push_back(rb);
      data.indices.push_back(lt);

      data.indices.push_back(rb);
      data.indices.push_back(rt);
      data.indices.push_back(lt);
    }
  }

  data.rootNode.name = "Plane";
  data.rootNode.localMatrix = MakeIdentity4x4();
  return data;
}

ModelData MeshGenerator::GenerateBox(float width, float height, float depth) {
  ModelData data;
  float w2 = width * 0.5f;
  float h2 = height * 0.5f;
  float d2 = depth * 0.5f;

  struct Face {
    Vector3 normal;
    Vector3 tangent;
    Vector3 bitangent;
  };
  Face faces[6] = {
      {{0, 0, 1},  {-1, 0, 0}, {0, 1, 0}}, // 前面（+Z 側から見て X- が右）
      {{0, 0, -1}, {1, 0, 0},  {0, 1, 0}}, // 背面（-Z 側から見て X+ が右）
      {{0, 1, 0},  {1, 0, 0},  {0, 0, 1}}, // 上面（+Y）
      {{0, -1, 0}, {1, 0, 0},  {0, 0, -1}},// 底面（-Y）
      {{1, 0, 0},  {0, 0, 1},  {0, 1, 0}}, // 右面（+X 側から見て Z+ が右）
      {{-1, 0, 0}, {0, 0, -1}, {0, 1, 0}}, // 左面（-X 側から見て Z- が右）
  };

  for (int i = 0; i < 6; ++i) {
    uint32_t baseIdx = (uint32_t)data.vertices.size();
    for (int y = 0; y <= 1; ++y) {
      for (int x = 0; x <= 1; ++x) {
        float fx = x * 2.0f - 1.0f;
        float fy = y * 2.0f - 1.0f;
        VertexData vtx;
        Vector3 pos = Add(Add(Multiply(faces[i].normal, 1.0f), Multiply(faces[i].tangent, fx)), Multiply(faces[i].bitangent, fy));
        vtx.position = {pos.x * w2, pos.y * h2, pos.z * d2, 1.0f};
        vtx.normal = faces[i].normal;
        vtx.texcoord = {(float)x, 1.0f - y};
        data.vertices.push_back(vtx);
      }
    }
    // 時計回り（CW）の巻き順
    data.indices.push_back(baseIdx + 0);
    data.indices.push_back(baseIdx + 2);
    data.indices.push_back(baseIdx + 3);
    data.indices.push_back(baseIdx + 0);
    data.indices.push_back(baseIdx + 3);
    data.indices.push_back(baseIdx + 1);
  }

  data.rootNode.name = "Box";
  data.rootNode.localMatrix = MakeIdentity4x4();
  return data;
}

ModelData MeshGenerator::GenerateCircle(float radius, uint32_t segments) {
  ModelData data;
  data.vertices.push_back({.position = {0, 0, 0, 1}, .texcoord = {0.5f, 0.5f}, .normal = {0, 1, 0}});

  for (uint32_t i = 0; i <= segments; ++i) {
    float rad = (float)i / segments * 2.0f * M_PI;
    float c = std::cos(rad);
    float s = std::sin(rad);
    VertexData vtx;
    vtx.position = {c * radius, 0.0f, s * radius, 1.0f};
    vtx.normal = {0, 1, 0};
    vtx.texcoord = {c * 0.5f + 0.5f, s * 0.5f + 0.5f};
    data.vertices.push_back(vtx);
  }

  for (uint32_t i = 0; i < segments; ++i) {
    data.indices.push_back(0);
    data.indices.push_back(i + 2);
    data.indices.push_back(i + 1);
  }

  data.rootNode.name = "Circle";
  return data;
}

ModelData MeshGenerator::GenerateRing(float innerRadius, float outerRadius, uint32_t segments) {
  ModelData data;
  for (uint32_t i = 0; i <= segments; ++i) {
    float rad = (float)i / segments * 2.0f * M_PI;
    float c = std::cos(rad);
    float s = std::sin(rad);
    float u = (float)i / segments;

    VertexData vInner;
    vInner.position = {c * innerRadius, 0.0f, s * innerRadius, 1.0f};
    vInner.normal = {0, 1, 0};
    vInner.texcoord = {u, 1.0f};
    data.vertices.push_back(vInner);

    VertexData vOuter;
    vOuter.position = {c * outerRadius, 0.0f, s * outerRadius, 1.0f};
    vOuter.normal = {0, 1, 0};
    vOuter.texcoord = {u, 0.0f};
    data.vertices.push_back(vOuter);
  }

  for (uint32_t i = 0; i < segments; ++i) {
    uint32_t base = i * 2;
    data.indices.push_back(base + 0);
    data.indices.push_back(base + 2);
    data.indices.push_back(base + 3);
    data.indices.push_back(base + 0);
    data.indices.push_back(base + 3);
    data.indices.push_back(base + 1);
  }
  data.rootNode.name = "Ring";
  return data;
}

ModelData MeshGenerator::GenerateRingEx(float innerRadius, float outerRadius,
                                        uint32_t segments,
                                        const Vector4 &innerColor,
                                        const Vector4 &outerColor,
                                        float startAngle, float endAngle,
                                        bool isVerticalUV, bool isXY) {
  ModelData data;
  float startRad = startAngle * M_PI / 180.0f;
  float endRad = endAngle * M_PI / 180.0f;
  float rangeRad = endRad - startRad;

  for (uint32_t i = 0; i <= segments; ++i) {
    float ratio = (float)i / segments;
    float rad = startRad + ratio * rangeRad;
    float c = std::cos(rad);
    float s = std::sin(rad);

    VertexData vInner, vOuter;
    if (isXY) {
      vInner.position = {c * innerRadius, s * innerRadius, 0.0f, 1.0f};
      vOuter.position = {c * outerRadius, s * outerRadius, 0.0f, 1.0f};
      vInner.normal = {0, 0, 1};
      vOuter.normal = {0, 0, 1};
    } else {
      vInner.position = {c * innerRadius, 0.0f, s * innerRadius, 1.0f};
      vOuter.position = {c * outerRadius, 0.0f, s * outerRadius, 1.0f};
      vInner.normal = {0, 1, 0};
      vOuter.normal = {0, 1, 0};
    }

    if (isVerticalUV) {
      // 角度 → V、半径 → U
      vInner.texcoord = {1.0f, ratio};
      vOuter.texcoord = {0.0f, ratio};
    } else {
      // 角度 → U、半径 → V
      vInner.texcoord = {ratio, 1.0f};
      vOuter.texcoord = {ratio, 0.0f};
    }

    data.vertices.push_back(vInner);
    data.vertices.push_back(vOuter);
  }

  for (uint32_t i = 0; i < segments; ++i) {
    uint32_t base = i * 2;
    // 時計回り（CW）の巻き順
    if (isXY) {
        // XY 平面（法線 +Z）。
        // base+0 が内側、base+1 が外側の頂点。
        // 角度は反時計回りに増えるので、
        // +Z 側から見て時計回りになるように並べる。
        data.indices.push_back(base + 0);
        data.indices.push_back(base + 1);
        data.indices.push_back(base + 3);
        data.indices.push_back(base + 0);
        data.indices.push_back(base + 3);
        data.indices.push_back(base + 2);
    } else {
        // XZ 平面（法線 +Y）
        data.indices.push_back(base + 0);
        data.indices.push_back(base + 2);
        data.indices.push_back(base + 3);
        data.indices.push_back(base + 0);
        data.indices.push_back(base + 3);
        data.indices.push_back(base + 1);
    }
  }

  // NOTE: VertexData に色が無いため innerColor / outerColor は保持されない。
  // 内外の色はシェーダ側で texcoord.u / v を使って扱う。
  // 現状は内側を V=1（縦 UV なら U=1）、外側を V=0（縦 UV なら U=0）にしている。

  data.rootNode.name = "RingEx";
  return data;
}

ModelData MeshGenerator::GenerateSphere(float radius, uint32_t slices, uint32_t stacks) {
  ModelData data;
  for (uint32_t y = 0; y <= stacks; ++y) {
    float phi = (float)y / stacks * M_PI;
    for (uint32_t x = 0; x <= slices; ++x) {
      float theta = (float)x / slices * 2.0f * M_PI;
      VertexData vtx;
      float sinPhi = std::sin(phi);
      float cosPhi = std::cos(phi);
      float sinTheta = std::sin(theta);
      float cosTheta = std::cos(theta);

      vtx.normal = {sinPhi * cosTheta, cosPhi, sinPhi * sinTheta};
      vtx.position = {vtx.normal.x * radius, vtx.normal.y * radius, vtx.normal.z * radius, 1.0f};
      vtx.texcoord = {(float)x / slices, (float)y / stacks};
      data.vertices.push_back(vtx);
    }
  }

  for (uint32_t y = 0; y < stacks; ++y) {
    for (uint32_t x = 0; x < slices; ++x) {
      uint32_t lb = y * (slices + 1) + x;
      uint32_t rb = lb + 1;
      uint32_t lt = (y + 1) * (slices + 1) + x;
      uint32_t rt = lt + 1;

      data.indices.push_back(lb);
      data.indices.push_back(rb);
      data.indices.push_back(lt);
      data.indices.push_back(lt);
      data.indices.push_back(rb);
      data.indices.push_back(rt);
    }
  }
  data.rootNode.name = "Sphere";
  return data;
}

ModelData MeshGenerator::GenerateCylinder(float radius, float height, uint32_t segments) {
  ModelData data;
  float h2 = height * 0.5f;
  // 側面
  for (uint32_t i = 0; i <= segments; ++i) {
    float rad = (float)i / segments * 2.0f * M_PI;
    float c = std::cos(rad);
    float s = std::sin(rad);
    float u = (float)i / segments;

    VertexData v0;
    v0.position = {c * radius, -h2, s * radius, 1.0f};
    v0.normal = {c, 0, s};
    v0.texcoord = {u, 1.0f};
    data.vertices.push_back(v0);

    VertexData v1;
    v1.position = {c * radius, h2, s * radius, 1.0f};
    v1.normal = {c, 0, s};
    v1.texcoord = {u, 0.0f};
    data.vertices.push_back(v1);
  }
  for (uint32_t i = 0; i < segments; ++i) {
    uint32_t base = i * 2;
    data.indices.push_back(base + 0);
    data.indices.push_back(base + 1);
    data.indices.push_back(base + 2);
    data.indices.push_back(base + 2);
    data.indices.push_back(base + 1);
    data.indices.push_back(base + 3);
  }
  // 上下の蓋
  uint32_t topCenterIdx = (uint32_t)data.vertices.size();
  data.vertices.push_back({.position = {0, h2, 0, 1}, .texcoord = {0.5f, 0.5f}, .normal = {0, 1, 0}});
  uint32_t bottomCenterIdx = (uint32_t)data.vertices.size();
  data.vertices.push_back({.position = {0, -h2, 0, 1}, .texcoord = {0.5f, 0.5f}, .normal = {0, -1, 0}});

  uint32_t topRingStart = (uint32_t)data.vertices.size();
  for (uint32_t i = 0; i <= segments; ++i) {
    float rad = (float)i / segments * 2.0f * M_PI;
    float c = std::cos(rad);
    float s = std::sin(rad);
    data.vertices.push_back({.position = {c * radius, h2, s * radius, 1.0f}, .texcoord = {c * 0.5f + 0.5f, s * 0.5f + 0.5f}, .normal = {0, 1, 0}});
  }
  uint32_t bottomRingStart = (uint32_t)data.vertices.size();
  for (uint32_t i = 0; i <= segments; ++i) {
    float rad = (float)i / segments * 2.0f * M_PI;
    float c = std::cos(rad);
    float s = std::sin(rad);
    data.vertices.push_back({.position = {c * radius, -h2, s * radius, 1.0f}, .texcoord = {c * 0.5f + 0.5f, s * 0.5f + 0.5f}, .normal = {0, -1, 0}});
  }

  for (uint32_t i = 0; i < segments; ++i) {
    // 上の蓋（CW）
    data.indices.push_back(topCenterIdx);
    data.indices.push_back(topRingStart + i + 1);
    data.indices.push_back(topRingStart + i);

    // 下の蓋（CW）
    data.indices.push_back(bottomCenterIdx);
    data.indices.push_back(bottomRingStart + i);
    data.indices.push_back(bottomRingStart + i + 1);
  }

  data.rootNode.name = "Cylinder";
  return data;
}

ModelData MeshGenerator::GenerateEffectCylinder(
    float topRadius, float bottomRadius, float height,
    uint32_t segments, float startAngle, float endAngle,
    bool isVerticalUV, bool flipV) {
  ModelData data;

  float startRad = startAngle * M_PI / 180.0f;
  float endRad = endAngle * M_PI / 180.0f;
  float rangeRad = endRad - startRad;

  for (uint32_t i = 0; i < segments; ++i) {
    float ratio = (float)i / segments;
    float nextRatio = (float)(i + 1) / segments;

    float rad = startRad + ratio * rangeRad;
    float nextRad = startRad + nextRatio * rangeRad;

    float s = std::sin(rad);
    float c = std::cos(rad);
    float sNext = std::sin(nextRad);
    float cNext = std::cos(nextRad);

    float utL = isVerticalUV ? 1.0f : ratio;
    float vtL = isVerticalUV ? ratio : 0.02f;
    float utR = isVerticalUV ? 1.0f : nextRatio;
    float vtR = isVerticalUV ? nextRatio : 0.02f;
    
    float ubL = isVerticalUV ? 0.0f : ratio;
    float vbL = isVerticalUV ? ratio : 0.98f;
    float ubR = isVerticalUV ? 0.0f : nextRatio;
    float vbR = isVerticalUV ? nextRatio : 0.98f;

    if (flipV) {
      vtL = 1.0f - vtL;
      vtR = 1.0f - vtR;
      vbL = 1.0f - vbL;
      vbR = 1.0f - vbR;
    }

    VertexData vTL;
    vTL.position = {-s * topRadius, height, c * topRadius, 1.0f};
    vTL.texcoord = {utL, vtL};
    vTL.normal = {-s, 0.0f, c};

    VertexData vTR;
    vTR.position = {-sNext * topRadius, height, cNext * topRadius, 1.0f};
    vTR.texcoord = {utR, vtR};
    vTR.normal = {-sNext, 0.0f, cNext};

    VertexData vBL;
    vBL.position = {-s * bottomRadius, 0.0f, c * bottomRadius, 1.0f};
    vBL.texcoord = {ubL, vbL};
    vBL.normal = {-s, 0.0f, c};

    VertexData vBR;
    vBR.position = {-sNext * bottomRadius, 0.0f, cNext * bottomRadius, 1.0f};
    vBR.texcoord = {ubR, vbR};
    vBR.normal = {-sNext, 0.0f, cNext};

    // インデックスバッファ不使用（Particle 描画用）
    data.vertices.push_back(vTL);
    data.vertices.push_back(vTR);
    data.vertices.push_back(vBL);

    data.vertices.push_back(vBL);
    data.vertices.push_back(vTR);
    data.vertices.push_back(vBR);
  }

  data.rootNode.name = "EffectCylinder";
  return data;
}

ModelData MeshGenerator::GenerateCone(float radius, float height, uint32_t segments) {
  ModelData data;
  float h2 = height * 0.5f;
  data.vertices.push_back({.position = {0, h2, 0, 1}, .texcoord = {0.5f, 0.0f}, .normal = {0, 1, 0}}); // 先端
  for (uint32_t i = 0; i <= segments; ++i) {
    float rad = (float)i / segments * 2.0f * M_PI;
    float c = std::cos(rad);
    float s = std::sin(rad);
    VertexData v;
    v.position = {c * radius, -h2, s * radius, 1.0f};
    v.normal = {c, 0.5f, s}; // 近似の法線（側面の実際の傾きとは一致しない）
    float len = std::sqrt(v.normal.x*v.normal.x + v.normal.y*v.normal.y + v.normal.z*v.normal.z);
    v.normal.x /= len; v.normal.y /= len; v.normal.z /= len;
    v.texcoord = {(float)i/segments, 1.0f};
    data.vertices.push_back(v);
  }
  for(uint32_t i=0; i<segments; ++i) {
    data.indices.push_back(0);
    data.indices.push_back(i + 2);
    data.indices.push_back(i + 1);
  }

  // 底面の蓋
  uint32_t centerIdx = (uint32_t)data.vertices.size();
  data.vertices.push_back({.position = {0, -h2, 0, 1}, .texcoord = {0.5f, 0.5f}, .normal = {0, -1, 0}});
  uint32_t ringStart = (uint32_t)data.vertices.size();
  for (uint32_t i = 0; i <= segments; ++i) {
    float rad = (float)i / segments * 2.0f * M_PI;
    float c = std::cos(rad);
    float s = std::sin(rad);
    data.vertices.push_back({.position = {c * radius, -h2, s * radius, 1.0f}, .texcoord = {c * 0.5f + 0.5f, s * 0.5f + 0.5f}, .normal = {0, -1, 0}});
  }
  for (uint32_t i = 0; i < segments; ++i) {
    data.indices.push_back(centerIdx);
    data.indices.push_back(ringStart + i);
    data.indices.push_back(ringStart + i + 1);
  }
  data.rootNode.name = "Cone";
  return data;
}

ModelData MeshGenerator::GenerateCapsule(float radius, float height, uint32_t slices, uint32_t stacks) {
  ModelData data;
  float cylinderH = std::max(0.0f, height - 2.0f * radius);
  float h2 = cylinderH * 0.5f;

  uint32_t stacks_effective = stacks;
  for (uint32_t y = 0; y <= stacks; ++y) {
    float phi = (float)y / stacks * M_PI;
    float sinPhi = std::sin(phi);
    float cosPhi = std::cos(phi);
    float yPos = cosPhi * radius;

    // 半球によるオフセット
    if (y <= stacks / 2) yPos += h2;
    else yPos -= h2;

    for (uint32_t x = 0; x <= slices; ++x) {
      float theta = (float)x / slices * 2.0f * M_PI;
      float sinTheta = std::sin(theta);
      float cosTheta = std::cos(theta);

      VertexData vtx;
      vtx.normal = {sinPhi * cosTheta, cosPhi, sinPhi * sinTheta};
      vtx.position = {vtx.normal.x * radius, yPos, vtx.normal.z * radius, 1.0f};
      vtx.texcoord = {(float)x / slices, (float)y / stacks};
      data.vertices.push_back(vtx);
    }
  }

  for (uint32_t y = 0; y < stacks; ++y) {
    for (uint32_t x = 0; x < slices; ++x) {
      uint32_t lb = y * (slices + 1) + x;
      uint32_t rb = lb + 1;
      uint32_t lt = (y + 1) * (slices + 1) + x;
      uint32_t rt = lt + 1;

      // 時計回り（CW）の巻き順（Sphere と同じ）
      data.indices.push_back(lb);
      data.indices.push_back(rb);
      data.indices.push_back(lt);
      data.indices.push_back(lt);
      data.indices.push_back(rb);
      data.indices.push_back(rt);
    }
  }
  data.rootNode.name = "Capsule";
  return data;
}

ModelData MeshGenerator::GenerateTorus(float majorRadius, float minorRadius, uint32_t majorSegments, uint32_t minorSegments) {
  ModelData data;
  for (uint32_t j = 0; j <= majorSegments; ++j) {
    float majorRad = (float)j / majorSegments * 2.0f * M_PI;
    Vector3 majorPos = {std::cos(majorRad), 0, std::sin(majorRad)};
    for (uint32_t i = 0; i <= minorSegments; ++i) {
      float minorRad = (float)i / minorSegments * 2.0f * M_PI;
      float c = std::cos(minorRad);
      float s = std::sin(minorRad);
      
      Vector3 normal = Add(Multiply(majorPos, c), Vector3{0, s, 0});
      Vector3 pos = Add(Multiply(majorPos, majorRadius), Multiply(normal, minorRadius));
      
      VertexData vtx;
      vtx.position = {pos.x, pos.y, pos.z, 1.0f};
      vtx.normal = normal;
      vtx.texcoord = {(float)j / majorSegments, (float)i / minorSegments};
      data.vertices.push_back(vtx);
    }
  }
  for (uint32_t j = 0; j < majorSegments; ++j) {
    for (uint32_t i = 0; i < minorSegments; ++i) {
      uint32_t lb = j * (minorSegments + 1) + i;
      uint32_t rb = (j + 1) * (minorSegments + 1) + i;
      uint32_t lt = lb + 1;
      uint32_t rt = rb + 1;
      // 時計回り（CW）の巻き順
      data.indices.push_back(lb);
      data.indices.push_back(lt);
      data.indices.push_back(rb);

      data.indices.push_back(lt);
      data.indices.push_back(rt);
      data.indices.push_back(rb);
    }
  }
  data.rootNode.name = "Torus";
  return data;
}

ModelData MeshGenerator::GenerateTriangle(const Vector3 &v0, const Vector3 &v1, const Vector3 &v2) {
  ModelData data;
  Vector3 normal = Cross(Subtract(v1, v0), Subtract(v2, v0));
  float len = std::sqrt(normal.x*normal.x + normal.y*normal.y + normal.z*normal.z);
  if(len > 1e-6f) { normal.x /= len; normal.y /= len; normal.z /= len; }

  data.vertices.push_back({.position = {v0.x, v0.y, v0.z, 1}, .texcoord = {0, 0}, .normal = normal});
  data.vertices.push_back({.position = {v1.x, v1.y, v1.z, 1}, .texcoord = {1, 0}, .normal = normal});
  data.vertices.push_back({.position = {v2.x, v2.y, v2.z, 1}, .texcoord = {0.5f, 1}, .normal = normal});
  data.indices = {0, 1, 2};
  data.rootNode.name = "Triangle";
  return data;
}

} // namespace RC
