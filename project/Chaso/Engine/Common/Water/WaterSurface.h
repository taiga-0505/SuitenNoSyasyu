#pragma once
#include "Math/MathTypes.h"
#include <cmath>
#include <algorithm>

// =====================================================================
// C-01: CPU 側で水面の高さを求めるユーティリティ
// =====================================================================
// 波の変位は Water.VS.hlsl（頂点シェーダ）の中で計算しているため、
// ゲームロジック側からは水面がどこにあるか分からない。
// 浮力・着水判定・水しぶきなど「水面と物体の関係」を扱うには
// 同じ式を CPU 側にも持つ必要がある。
//
// ここは Water.VS.hlsl と式を一致させること。
// 片方だけ直すと、見た目の水面と当たり判定の水面がズレる。
// 対応関係：
//   GetWaveDef()        -> WaterSurface::GetWave()
//   AccumulateGerstner()-> WaterSurface::Accumulate()
//   AccumulateReflection() -> WaterSurface::AccumulateReflection()
//   ComputeDetailOffset()  -> WaterSurface::Evaluate() の詳細波ループ
//   OceanWaves.hlsli の表  -> WaterSurface::kDetail* の表
//
// シェーダ側にある gInteractiveWave（弾着の波紋テクスチャ）は GPU 上にしか
// 無いためここでは無視する。振幅が小さいディテールなので浮力には影響しない。

namespace RC {

/// @brief 水面の波パラメータ（WaterComponent の波関連フィールドと 1:1 対応）
struct WaterWaveParams {
  float waveHeight    = 0.3f;
  float waveSpeed     = 1.5f;
  float waveFreq      = 0.8f;
  float waveHeight2   = 0.15f;
  float waveSpeed2    = 1.0f;
  float waveFreq2     = 1.2f;
  float waveSteepness = 0.4f;
  float baseHeight    = 0.0f;  ///< 水面エンティティの Y 座標（静水面の高さ）

  // --- 詳細波（OceanWaves.hlsli）---
  float detail        = 1.0f;  ///< 詳細波の強さ（0 で無効。シェーダの gOceanParams.x）
  float choppiness    = 0.6f;  ///< 詳細波の尖り 0..1（gOceanParams.y）
  float minWavelength = 0.0f;  ///< 頂点で変位させる最短波長 m（gOceanParams.w）。WaterComponent::MinDisplacedWavelength()
};

/// @brief 水面に波の反射を起こす障害物（円柱近似）
struct WaterObstacle {
  Vector3 pos{0.0f, 0.0f, 0.0f};
  float   radius = 1.0f;
};

/// @brief ある地点の水面サンプル結果
struct WaterSample {
  float   height{0.0f};                ///< その XZ における水面のワールド Y
  Vector3 normal{0.0f, 1.0f, 0.0f};    ///< 水面の法線（傾き合わせに使う）
};

/// @brief Gerstner 波の水面をワールド座標から引くための関数群
class WaterSurface {
public:
  static constexpr int kWaveCount    = 3; ///< シェーダと同じ重ね合わせ本数
  static constexpr int kMaxObstacles = 4; ///< シェーダの CB が持てる障害物の最大数

  // --- 詳細波の表（OceanWaves.hlsli と完全に同じ値にすること）---
  static constexpr int   kDetailWaveCount = 8;
  static constexpr float kWindAngle       = 0.620249486f; // atan2(0.5, 0.7)
  static constexpr float kDetailAngle[kDetailWaveCount] = { -0.62f, 0.47f, -0.21f, 0.83f, 0.12f, -0.97f, 0.58f, -0.41f };
  static constexpr float kDetailRatio[kDetailWaveCount] = {  1.37f, 1.79f,  2.23f, 2.71f, 3.29f,  3.97f, 4.81f,  5.83f };
  static constexpr float kDetailPhase[kDetailWaveCount] = {  0.0f,  1.7f,   4.1f,  2.3f,  5.5f,   0.9f,  3.3f,   6.0f  };
  static constexpr float kDetailAmpScale  = 0.35f;
  static constexpr float kDetailMaxSlope  = 0.10f;

  /// @brief 指定 XZ における水面の高さを求める
  /// @param p 波パラメータ
  /// @param time 経過時間（RC::SetWaterTime に渡しているものと同じ値）
  /// @param x,z ワールド座標
  /// @param obstacles 障害物配列（nullptr 可）
  /// @param obstacleCount 障害物の数
  /// @param reflectStrength 反射の強さ（シェーダの gObstacleCount.y と同じ）
  /// @param reflectRange 反射の到達範囲（半径倍率／gObstacleCount.z と同じ）
  /// @param iterations XZ 変位の逆解き回数。Gerstner は横にもずれるため、
  ///        「変位後に (x,z) へ来る元の格子点」を反復で探す。0 で無効。
  static float SampleHeight(const WaterWaveParams& p, float time, float x, float z,
                            const WaterObstacle* obstacles = nullptr, int obstacleCount = 0,
                            float reflectStrength = 1.0f, float reflectRange = 3.0f,
                            int iterations = 2) {
    return Sample(p, time, x, z, obstacles, obstacleCount,
                  reflectStrength, reflectRange, iterations).height;
  }

  /// @brief 指定 XZ における水面の高さと法線を求める
  static WaterSample Sample(const WaterWaveParams& p, float time, float x, float z,
                            const WaterObstacle* obstacles = nullptr, int obstacleCount = 0,
                            float reflectStrength = 1.0f, float reflectRange = 3.0f,
                            int iterations = 2) {
    // --- XZ 変位の逆解き ---------------------------------------------
    // Gerstner 波は頂点を横にも動かすので、格子点 s を評価すると
    // 実際の水面は s + offset.xz に現れる。ここで欲しいのは逆で、
    // 「s + offset.xz == (x,z) となる s」。不動点反復で近づける。
    // steepness が 1 に近いと収束が鈍るが、本作は 0.35 前後なので 2 回で十分。
    float sx = x, sz = z;
    Accum a{};
    for (int i = 0; i <= iterations; ++i) {
      a = Evaluate(p, time, sx, sz, obstacles, obstacleCount, reflectStrength, reflectRange);
      if (i == iterations) break;
      sx = x - a.offsetX;
      sz = z - a.offsetZ;
    }

    WaterSample out;
    out.height = p.baseHeight + a.offsetY;

    // 接線・従法線の外積で法線を作る（シェーダと同じ手順）
    // ※ シェーダでは詳細波の法線は PS で足すが、ここでは Gerstner の接線に直接入れている（結果はほぼ同じ）
    const Vector3 tangent  {a.tangentX,  a.tangentY,  a.tangentZ};
    const Vector3 binormal {a.binormalX, a.binormalY, a.binormalZ};
    Vector3 n {
      binormal.y * tangent.z - binormal.z * tangent.y,
      binormal.z * tangent.x - binormal.x * tangent.z,
      binormal.x * tangent.y - binormal.y * tangent.x
    };
    const float len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
    if (len > 1e-6f) {
      out.normal = { n.x / len, n.y / len, n.z / len };
      if (out.normal.y < 0.0f) { // 常に上向きへ揃える
        out.normal = { -out.normal.x, -out.normal.y, -out.normal.z };
      }
    }
    return out;
  }

private:
  /// @brief 1本の平面波の定義（Water.VS.hlsl の WaveDef と同じ）
  struct Wave {
    float dirX, dirZ;   ///< 波が実際に進む向き（正規化済み）
    float freq;
    float timePhase;    ///< -speed * time
    float steepness;
    float amp;
  };

  /// @brief Gerstner の累積結果（シェーダの GerstnerResult 相当）
  struct Accum {
    float offsetX = 0.0f, offsetY = 0.0f, offsetZ = 0.0f;
    float tangentX = 1.0f, tangentY = 0.0f, tangentZ = 0.0f;
    float binormalX = 0.0f, binormalY = 0.0f, binormalZ = 1.0f;
  };

  static Wave GetWave(const WaterWaveParams& p, int index, float time) {
    Wave w{};
    if (index == 0) {
      // 主波 (X方向寄り) — normalize(float2(0.7, 0.5))
      w.dirX = 0.813733471f; w.dirZ = 0.581238194f;
      w.freq = p.waveFreq;
      w.timePhase = -p.waveSpeed * time;
      w.steepness = p.waveSteepness;
      w.amp = p.waveHeight;
    } else if (index == 1) {
      // 副波 (Z方向寄り) — normalize(float2(0.3, 0.8))
      w.dirX = 0.351123409f; w.dirZ = 0.936329091f;
      w.freq = p.waveFreq2;
      w.timePhase = -p.waveSpeed2 * time;
      w.steepness = p.waveSteepness * 0.8f;
      w.amp = p.waveHeight2;
    } else {
      // 微細波 — normalize(float2(-0.4, 0.6))
      w.dirX = -0.554700196f; w.dirZ = 0.832050294f;
      w.freq = p.waveFreq * 2.5f;
      w.timePhase = -p.waveSpeed * 1.3f * time;
      w.steepness = p.waveSteepness * 0.3f;
      w.amp = p.waveHeight * 0.25f;
    }
    return w;
  }

  /// @brief 平面波1成分を Gerstner として加算する（AccumulateGerstner と同一式）
  static void Accumulate(Accum& r, float dirX, float dirZ, float freq,
                         float phase, float steepness, float amp) {
    const float s = std::sin(phase);
    const float c = std::cos(phase);

    r.offsetX += steepness * amp * dirX * c;
    r.offsetZ += steepness * amp * dirZ * c;
    r.offsetY += amp * s;

    const float wa = steepness * freq * amp * s;
    r.tangentX  -= wa * dirX * dirX;
    r.tangentY  += dirX * freq * amp * c;
    r.tangentZ  -= wa * dirX * dirZ;
    r.binormalX -= wa * dirX * dirZ;
    r.binormalZ -= wa * dirZ * dirZ;
    r.binormalY += dirZ * freq * amp * c;
  }

  /// @brief 詳細波 i を Gerstner として加算する（OceanWaves.hlsli の GetDetailWave + VS の ComputeDetailOffset）
  static void AccumulateDetail(Accum& r, const WaterWaveParams& p, int i, float time,
                               float sx, float sz) {
    const float detail = p.detail;
    const float ang = kWindAngle + kDetailAngle[i];
    const float dirX = std::cos(ang);
    const float dirZ = std::sin(ang);
    const float k = (std::max)(p.waveFreq, 1e-3f) * kDetailRatio[i];
    const float omega = p.waveSpeed * std::sqrt(kDetailRatio[i]);
    const float amp = (std::min)(p.waveHeight * kDetailAmpScale / kDetailRatio[i],
                                 kDetailMaxSlope / k) * detail;
    const float qa = p.choppiness * detail / (static_cast<float>(kDetailWaveCount) * k);

    const float minLambda = (std::max)(p.minWavelength, 1e-3f);
    const float lambda = 6.28318530718f / k;
    const float fade = SmoothStep(minLambda, minLambda * 1.5f, lambda);
    if (fade <= 0.0f) return;

    const float phase = k * (dirX * sx + dirZ * sz) - omega * time + kDetailPhase[i];
    // Accumulate() は offset.xz = steepness*amp*dir*cos なので、steepness = qa/amp として渡す
    const float a = amp * fade;
    if (a <= 1e-7f) {
      // 鉛直振幅が 0 なら水平変位だけ（detail=0 のときは qa も 0 なのでここには来ない）
      return;
    }
    Accumulate(r, dirX, dirZ, k, phase, (qa * fade) / a, a);
  }

  /// @brief 障害物による反射波（鏡像法。AccumulateReflection と同一式）
  static void AccumulateReflection(Accum& r, const Wave& w, float px, float pz,
                                   const WaterObstacle* obstacles, int obstacleCount,
                                   float reflectStrength, float reflectRange) {
    for (int i = 0; i < obstacleCount; ++i) {
      const float ox = obstacles[i].pos.x;
      const float oz = obstacles[i].pos.z;
      const float radius = (std::max)(obstacles[i].radius, 0.001f);

      const float toX = px - ox;
      const float toZ = pz - oz;
      const float dist = std::sqrt(toX * toX + toZ * toZ);
      if (dist < 1e-4f) continue; // 中心では法線が定義できない

      const float nx = toX / dist;
      const float nz = toZ / dist;

      // 波がこの面に向かって進んでいるか
      const float facing = -(w.dirX * nx + w.dirZ * nz);
      if (facing <= 0.0f) continue;

      const float hitX = ox + nx * radius;
      const float hitZ = oz + nz * radius;

      // reflect(i, n) = i - 2 * dot(i, n) * n
      const float dotIN = w.dirX * nx + w.dirZ * nz;
      const float refX = w.dirX - 2.0f * dotIN * nx;
      const float refZ = w.dirZ - 2.0f * dotIN * nz;

      const float hitPhase = (w.dirX * hitX + w.dirZ * hitZ) * w.freq + w.timePhase;
      const float refPhase = hitPhase + (refX * (px - hitX) + refZ * (pz - hitZ)) * w.freq;

      const float travelX = px - hitX;
      const float travelZ = pz - hitZ;
      const float travel = std::sqrt(travelX * travelX + travelZ * travelZ);

      float fade = 1.0f - travel / (radius * reflectRange);
      fade = std::clamp(fade, 0.0f, 1.0f);
      fade = fade * fade * (3.0f - 2.0f * fade); // smoothstep

      const float angleTerm = std::sqrt(std::clamp(facing, 0.0f, 1.0f));
      const float atten = fade * angleTerm * reflectStrength;

      Accumulate(r, refX, refZ, w.freq, refPhase, w.steepness, w.amp * atten);
    }
  }

  /// @brief 格子点 (sx, sz) を評価して変位・接線を求める（ComputeGerstnerWave 相当）
  static Accum Evaluate(const WaterWaveParams& p, float time, float sx, float sz,
                        const WaterObstacle* obstacles, int obstacleCount,
                        float reflectStrength, float reflectRange) {
    Accum r{};
    const int count = (std::min)(obstacleCount, kMaxObstacles);
    const float strength = (reflectStrength > 0.0f) ? reflectStrength : 1.0f;
    const float range    = (reflectRange > 0.0f) ? reflectRange : 3.0f;

    for (int k = 0; k < kWaveCount; ++k) {
      const Wave w = GetWave(p, k, time);
      const float phase = (w.dirX * sx + w.dirZ * sz) * w.freq + w.timePhase;
      Accumulate(r, w.dirX, w.dirZ, w.freq, phase, w.steepness, w.amp);
      if (obstacles && count > 0) {
        AccumulateReflection(r, w, sx, sz, obstacles, count, strength, range);
      }
    }

    // 詳細波（シェーダでは VS の ComputeDetailOffset）
    if (p.detail > 0.0f) {
      for (int i = 0; i < kDetailWaveCount; ++i) {
        AccumulateDetail(r, p, i, time, sx, sz);
      }
    }

    // 障害物の内側では水面を平らに寄せる（シェーダの insideMask と同じ）
    float insideMask = 1.0f;
    for (int j = 0; j < count && obstacles; ++j) {
      const float rad = (std::max)(obstacles[j].radius, 0.001f);
      const float dx = sx - obstacles[j].pos.x;
      const float dz = sz - obstacles[j].pos.z;
      const float d = std::sqrt(dx * dx + dz * dz);
      insideMask = (std::min)(insideMask, SmoothStep(rad * 0.6f, rad, d));
    }
    r.offsetX *= insideMask;
    r.offsetY *= insideMask;
    r.offsetZ *= insideMask;
    r.tangentX  = 1.0f + (r.tangentX  - 1.0f) * insideMask;
    r.tangentY  = r.tangentY  * insideMask;
    r.tangentZ  = r.tangentZ  * insideMask;
    r.binormalX = r.binormalX * insideMask;
    r.binormalZ = 1.0f + (r.binormalZ - 1.0f) * insideMask;
    r.binormalY = r.binormalY * insideMask;

    return r;
  }

  /// @brief HLSL の smoothstep(edge0, edge1, x) 相当
  static float SmoothStep(float edge0, float edge1, float x) {
    if (edge1 - edge0 <= 1e-6f) return (x < edge0) ? 0.0f : 1.0f;
    float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
  }
};

} // namespace RC
