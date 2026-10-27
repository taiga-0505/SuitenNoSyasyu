#include "ScreenDropletSim.h"

#include <algorithm>
#include <cmath>

namespace {

// 調整値（長さは画面の高さ = 1 の単位、時間は秒）
constexpr float kCritRadius = 0.017f;      ///< これより大きい粒は自重で滑り出す（付着のムラで ±35%）
constexpr float kCritNoiseFreq = 13.0f;    ///< 付着のムラの細かさ（止まっては滑る間隔）
constexpr float kMeanderFreq = 7.0f;       ///< 進路が曲がるムラの細かさ
constexpr float kSlideRange = 0.030f;      ///< 臨界半径からこれだけ大きいと最高速
constexpr float kSlideSpeed = 0.60f;       ///< 最高速（画面の高さ / 秒）
constexpr float kMinRadius = 0.0015f;      ///< これより小さくなったら消す
constexpr float kMergeFactor = 0.78f;      ///< 中心距離が (r1 + r2) × これ未満で合体
constexpr float kOffscreenMargin = 0.12f;  ///< 画面外へこれだけ出たら消す
constexpr float kMaxStep = 1.0f / 60.0f;   ///< 1 ステップの最大秒数（長いフレームは分割）

/// @brief 2 次元の整数格子に 0〜1 の擬似乱数を割り当てる
float HashGrid(int x, int y) {
  uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u;
  h = (h ^ (h >> 13)) * 1274126177u;
  h ^= h >> 16;
  return static_cast<float>(h & 0xFFFFFFu) / static_cast<float>(0x1000000);
}

/// @brief 滑らかな 2 次元バリューノイズ（0〜1）
float ValueNoise(float x, float y) {
  const float fx = std::floor(x), fy = std::floor(y);
  const int ix = static_cast<int>(fx), iy = static_cast<int>(fy);
  float tx = x - fx, ty = y - fy;
  tx = tx * tx * (3.0f - 2.0f * tx);
  ty = ty * ty * (3.0f - 2.0f * ty);
  const float a = HashGrid(ix, iy), b = HashGrid(ix + 1, iy);
  const float c = HashGrid(ix, iy + 1), d = HashGrid(ix + 1, iy + 1);
  return (a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * ty;
}

float Clamp01(float v) { return (std::min)(1.0f, (std::max)(0.0f, v)); }

float SmoothStep(float e0, float e1, float x) {
  const float t = Clamp01((x - e0) / (e1 - e0));
  return t * t * (3.0f - 2.0f * t);
}

} // namespace

// -----------------------------------------------------------------------------
// 水膜の進み方（HLSL の ArrivalTime と同じ式。片方だけ変えると水滴と水膜がずれる）
// -----------------------------------------------------------------------------
float ScreenDropletSim::ArrivalTime(Mode mode, const Sheet &sheet, float x, float y) {
  if (mode == Mode::Dive) {
    // 水面ラインが下（y = 1.08）から上（y = -0.17）へ抜ける
    return Clamp01((1.08f - y) / 1.25f) * sheet.duration;
  }
  // 中心から外へ、かつ上ほど早く乾く。乾いた領域の半径は ease-out（1 - (1 - s)^3）で広がる
  const float dx = x - sheet.centerX, dy = y - sheet.centerY;
  const float m = std::sqrt(dx * dx + dy * dy) + sheet.gravityBias * dy;
  const float s = m / sheet.radiusMax;
  if (s <= 0.0f) return 0.0f;
  if (s >= 1.0f) return 1.0e3f;
  return sheet.duration * (1.0f - std::cbrt(1.0f - s));
}

void ScreenDropletSim::Trigger(Mode mode, const Params &params) {
  Clear();
  mode_ = mode;
  active_ = true;
  elapsed_ = 0.0f;
  rng_ = 0x9E3779B9u ^ (++triggerCount_ * 2654435761u);
  if (rng_ == 0) rng_ = 1;

  const float aspect = params.aspect;
  const float s = params.sizeScale;
  sizeScale_ = s;
  drainStart_ = 1.0e3f;
  drainDuration_ = 1.0f;
  sheet_.centerX = 0.5f * aspect;
  sheet_.centerY = 0.40f;
  sheet_.duration = (mode == Mode::Dive) ? 0.32f : 1.1f;
  sheet_.radiusMax = 1.65f;
  sheet_.gravityBias = 0.45f;

  if (mode == Mode::Surface) {
    // 水膜が引いたあとに残る水滴。小粒ほど多く、大粒はまれ（べき分布）
    for (int i = 0; i < 40; ++i) {
      const float r = s * (0.0045f + 0.046f * std::pow(Rand(), 2.6f));
      const float x = Rand() * aspect;
      const float y = Rand();
      Spawn(x, y, r, ArrivalTime(mode, sheet_, x, y) + Rand() * 0.05f);
    }
    // 最後まで水膜が残る下端・左右の端には、膜がちぎれた大粒を置く
    for (int i = 0; i < 6; ++i) {
      float x, y;
      if (Rand() < 0.5f) {
        x = Rand() * aspect;
        y = 0.8f + 0.2f * Rand();
      } else {
        const float edge = 0.15f * Rand();
        x = (Rand() < 0.5f) ? edge : aspect - edge;
        y = 0.3f + 0.7f * Rand();
      }
      const float r = s * (0.026f + 0.026f * Rand());
      Spawn(x, y, r, ArrivalTime(mode, sheet_, x, y) + Rand() * 0.05f);
    }
  } else {
    // 水面ラインが通り過ぎた場所からレンズに気泡が残る。突入の勢いで一気に付き、
    // 大きい粒は昇って画面外へ抜け、小さい粒ははじけて、沈むあいだに少しずつ減っていく
    for (int i = 0; i < 44; ++i) {
      const float r = s * (0.006f + 0.040f * std::pow(Rand(), 1.8f));
      const float x = Rand() * aspect;
      const float y = Rand();
      Spawn(x, y, r, ArrivalTime(mode, sheet_, x, y) + Rand() * 0.08f);
    }
  }

  // 生まれた瞬間は水膜から引き剥がされた勢いを少しだけ持たせる
  for (Drop &d : drops_) {
    if (!d.alive) continue;
    float fx, fy;
    FlowDir(d.x, d.y, params, fx, fy);
    d.dirX = fx;
    d.dirY = fy;
    if (mode == Mode::Dive) {
      if (d.r > kCritRadius * s * 0.45f) { // 気泡は突入の勢いで最初から昇り始める
        d.vx = fx * 0.5f;
        d.vy = fy * 0.5f;
      }
    } else if (d.r > kCritRadius * s) {
      d.vx = fx * 0.25f;
      d.vy = fy * 0.25f;
    }
  }
}

void ScreenDropletSim::Clear() {
  for (Drop &d : drops_) d.alive = false;
  active_ = false;
  elapsed_ = 0.0f;
  drainStart_ = 1.0e3f;
}

void ScreenDropletSim::Drain(float seconds) {
  if (!active_) return;
  seconds = (std::max)(seconds, 0.05f);
  // 2 回目以降は、すでに決めた締め切りより遅くしない
  if (drainStart_ < 1.0e2f && drainStart_ + drainDuration_ <= elapsed_ + seconds) return;
  drainStart_ = elapsed_;
  drainDuration_ = seconds;
  for (Drop &d : drops_) {
    if (!d.alive) continue;
    if (d.delay > 0.0f) {
      d.alive = false; // まだ水膜の中（画面に出ていない）
      continue;
    }
    // 消え始める時刻を 0〜0.7 × seconds にばらまき、残り 0.3 × seconds で痩せきる
    d.life = (std::min)(d.life, Rand() * 0.7f * seconds);
    d.vanishTime = (std::min)(d.vanishTime, 0.3f * seconds);
  }
}

void ScreenDropletSim::Update(float dt, const Params &params) {
  if (!active_ || dt <= 0.0f) return;
  dt = (std::min)(dt, 0.1f); // ヒッチで粒が画面外へワープしないように
  elapsed_ += dt;
  while (dt > 0.0f) {
    const float h = (std::min)(dt, kMaxStep);
    Step(h, params);
    dt -= h;
  }
}

ScreenDropletSim::Drop *ScreenDropletSim::Spawn(float x, float y, float r, float delay) {
  for (Drop &d : drops_) {
    if (d.alive) continue;
    d = Drop{};
    d.alive = true;
    d.x = x;
    d.y = y;
    d.r = r;
    d.delay = delay;
    d.seed = Rand();
    d.fadeSeed = Rand();
    d.wobbleAmp = 0.15f + 0.12f * Rand(); // できたての粒は形が落ち着いていない
    d.wobblePhase = Rand() * 6.2831853f;
    // 寿命：粒ごとにばらつかせ、画面から水が「少しずつ」減っていくようにする
    if (mode_ == Mode::Dive) {
      if (r > kCritRadius * sizeScale_ * 0.45f) {
        // 昇る気泡：基本は画面の上へ抜けていく。抜けきれなかった粒だけ途中ではじける
        d.life = 0.7f + 0.6f * Rand();
        d.vanishTime = 0.15f;
      } else {
        // 貼り付いた小さな気泡：時間差ではじけて消える
        d.life = 0.25f + 0.6f * Rand();
        d.vanishTime = 0.1f + 0.1f * Rand();
      }
    } else if (r > kCritRadius * sizeScale_) {
      // 滑り落ちる大粒は画面外へ出ていくのが基本。途中で止まった粒だけ寿命で痩せる
      d.life = 1.2f + 1.0f * Rand();
      d.vanishTime = 0.5f;
    } else {
      // 動かない小粒は乾いて痩せていく
      d.life = 0.4f + 1.4f * Rand();
      d.vanishTime = 0.3f + 0.3f * Rand();
    }
    return &d;
  }
  return nullptr;
}

void ScreenDropletSim::FlowDir(float x, float y, const Params &params, float &outX, float &outY) const {
  // 重力（浮上）/ 浮力（潜水）
  float fx = 0.0f;
  float fy = (mode_ == Mode::Dive) ? -1.0f : 1.0f;
  // 前進しているので、レンズ上の水は中心から外へ押し出される（中心付近ほど弱い）
  const float rx = x - 0.5f * params.aspect;
  const float ry = y - 0.5f;
  const float rl = std::sqrt(rx * rx + ry * ry);
  if (rl > 1.0e-4f) {
    const float w = params.wind * Clamp01(rl * 2.0f);
    fx += rx / rl * w;
    fy += ry / rl * w;
  }
  const float l = std::sqrt(fx * fx + fy * fy);
  outX = (l > 1.0e-5f) ? fx / l : 0.0f;
  outY = (l > 1.0e-5f) ? fy / l : 1.0f;
}

void ScreenDropletSim::Step(float dt, const Params &params) {
  const float s = params.sizeScale;
  const bool dive = (mode_ == Mode::Dive);

  for (int i = 0; i < kMaxDrops; ++i) {
    Drop &d = drops_[i];
    if (!d.alive) continue;

    // 水膜が引くまでは膜の中に埋もれている
    if (d.delay > 0.0f) {
      d.delay -= dt;
      if (d.delay > 0.0f) continue;
      d.age = -d.delay;
      d.delay = 0.0f;
    } else {
      d.age += dt;
    }

    // 寿命が来たら痩せて消える（消える時刻は粒ごとに違う）
    if (d.life > 0.0f) {
      d.life -= dt;
    } else {
      d.vanish += dt / (std::max)(d.vanishTime, 1.0e-3f);
      if (d.vanish >= 1.0f) {
        d.alive = false;
        continue;
      }
    }
    if (d.r < kMinRadius * s) {
      d.alive = false;
      continue;
    }

    // 付着の強さは場所ごとにムラがある（汚れ・コーティング）。
    // 臨界半径ぎりぎりの粒は、ムラの強い所で止まり、弱い所でまた滑り出す
    const float crit = kCritRadius * s * (dive ? 0.45f : 1.0f) *
                       (0.65f + 0.7f * ValueNoise(d.x * kCritNoiseFreq, d.y * kCritNoiseFreq));

    if (d.r > crit) {
      float fx, fy;
      FlowDir(d.x, d.y, params, fx, fy);
      // 蛇行：表面のムラで進路が少しずつ曲がる。気泡はさらに左右に揺れながら昇る
      float ang = (ValueNoise(d.x * kMeanderFreq + 31.0f, d.y * kMeanderFreq + 17.0f) - 0.5f) * (dive ? 1.8f : 1.1f);
      if (dive) ang += std::sin(d.wobblePhase * 0.5f) * 0.35f;
      const float c = std::cos(ang), sn = std::sin(ang);
      const float dx = fx * c - fy * sn;
      const float dy = fx * sn + fy * c;

      const float drive = std::clamp((d.r - crit) / (kSlideRange * s), 0.0f, 1.25f);
      const float target = kSlideSpeed * params.flowSpeed * drive * (dive ? 1.3f : 1.0f);
      const float k = 1.0f - std::exp(-dt * 5.0f); // 止まった状態からはじわっと加速する
      d.vx += (dx * target - d.vx) * k;
      d.vy += (dy * target - d.vy) * k;
      if (!d.sliding) {
        d.sliding = true;
        d.wobbleAmp += 0.18f; // 滑り出しの瞬間に形がたわむ
      }
    } else {
      const float damp = std::exp(-dt * 9.0f);
      d.vx *= damp;
      d.vy *= damp;
      if (d.vx * d.vx + d.vy * d.vy < 1.0e-4f) d.sliding = false;
    }

    d.x += d.vx * dt;
    d.y += d.vy * dt;

    const float speed = std::sqrt(d.vx * d.vx + d.vy * d.vy);
    if (speed > 1.0e-3f) {
      // 形の向きは速度へなめらかに追従させる（急に向きが変わると形が跳ねて見える）
      const float k = 1.0f - std::exp(-dt * 12.0f);
      float nx = d.dirX + (d.vx / speed - d.dirX) * k;
      float ny = d.dirY + (d.vy / speed - d.dirY) * k;
      const float nl = std::sqrt(nx * nx + ny * ny);
      if (nl > 1.0e-5f) {
        d.dirX = nx / nl;
        d.dirY = ny / nl;
      }
    }

    // 速いほど進行方向へ伸びる（気泡は表面張力が勝つのであまり伸びない）
    const float targetStretch = 1.0f + (std::min)(speed * 0.9f, 0.55f) * (dive ? 0.45f : 1.0f);
    d.stretch += (targetStretch - d.stretch) * (1.0f - std::exp(-dt * 7.0f));

    // 形の揺れ：小さい粒ほど速く震え、すぐ落ち着く
    d.wobbleAmp = (std::min)(d.wobbleAmp * std::exp(-dt * 3.2f), 0.35f);
    d.wobblePhase += dt * 11.0f * std::sqrt((0.02f * s) / (std::max)(d.r, 0.003f));

    // 滑った跡に小さな滴を取り残す（その分だけ体積が減る）
    if (!dive && speed > 0.02f && d.vanish <= 0.0f) {
      d.shedDistance += speed * dt;
      const float spacing = d.r * (1.1f + 1.2f * d.seed);
      if (d.shedDistance > spacing && d.r > crit * 0.75f) {
        d.shedDistance = 0.0f;
        d.seed = Rand();
        const float rs = d.r * (0.16f + 0.16f * Rand());
        const float back = d.r * d.stretch + rs;
        const float px = d.x - d.dirX * back;
        const float py = d.y - d.dirY * back;
        const float dirX = d.dirX, dirY = d.dirY;
        const float vol = d.r * d.r * d.r - rs * rs * rs;
        // Spawn は drops_ を書き換えるので、d の更新を先に済ませておく
        d.r = std::cbrt((std::max)(vol, 0.0f));
        if (Drop *t = Spawn(px, py, rs, 0.0f)) {
          t->dirX = dirX;
          t->dirY = dirY;
          t->wobbleAmp = 0.1f;
          t->life = 0.2f + 0.8f * Rand(); // 跡の滴はすぐ乾く
          t->vanishTime = 0.3f;
          if (drainStart_ < 1.0e2f) { // Drain 中に生まれた滴も締め切りに間に合わせる
            t->life = 0.0f;
            t->vanishTime = 0.1f;
          }
        }
      }
    }

    if (d.x < -kOffscreenMargin || d.x > params.aspect + kOffscreenMargin ||
        d.y < -kOffscreenMargin || d.y > 1.0f + kOffscreenMargin) {
      d.alive = false;
    }
  }

  Merge();
}

void ScreenDropletSim::Merge() {
  for (int i = 0; i < kMaxDrops; ++i) {
    Drop &a = drops_[i];
    if (!a.alive || a.delay > 0.0f) continue;
    for (int j = i + 1; j < kMaxDrops; ++j) {
      Drop &b = drops_[j];
      if (!b.alive || b.delay > 0.0f) continue;
      const float dx = b.x - a.x, dy = b.y - a.y;
      const float rr = (a.r + b.r) * kMergeFactor;
      if (dx * dx + dy * dy >= rr * rr) continue;

      Drop &big = (a.r >= b.r) ? a : b;
      Drop &small = (a.r >= b.r) ? b : a;
      const float vb = big.r * big.r * big.r;
      const float vs = small.r * small.r * small.r;
      const float wb = vb / (vb + vs);
      big.x = big.x * wb + small.x * (1.0f - wb);
      big.y = big.y * wb + small.y * (1.0f - wb);
      big.vx = big.vx * wb + small.vx * (1.0f - wb); // 運動量を保つ
      big.vy = big.vy * wb + small.vy * (1.0f - wb);
      big.r = std::cbrt(vb + vs);
      big.wobbleAmp += 0.05f + 0.3f * (small.r / big.r); // 吸い込んだ瞬間にぶるっと揺れる
      big.age = (std::max)(big.age, 0.1f);
      big.vanish = (std::min)(big.vanish, small.vanish); // 吸い込んだ分だけ太る
      small.alive = false;
      if (&small == &a) break; // a が消えたので次の i へ
    }
  }
}

int ScreenDropletSim::Export(GpuDrop *out, int maxCount, float fade) const {
  if (!active_ || !out) return 0;
  int n = 0;
  for (const Drop &d : drops_) {
    if (n >= maxCount) break;
    if (!d.alive || d.delay > 0.0f) continue;
    // 生まれた直後は膜から盛り上がるように半径を育て、寿命が来たら痩せさせる。
    // 全体の強度は粒ごとの閾値（fadeSeed）で効かせ、強度を下げても一斉には消えないようにする
    const float grow = SmoothStep(0.0f, 0.07f, d.age);
    const float shrink = std::sqrt(Clamp01(1.0f - d.vanish));
    const float fadeK = std::sqrt(Clamp01((fade - d.fadeSeed * 0.75f) / 0.25f));
    const float r = d.r * grow * shrink * fadeK;
    if (r <= 1.0e-5f) continue;
    const float deform = d.wobbleAmp * std::sin(d.wobblePhase);
    GpuDrop &g = out[n++];
    g.a[0] = d.x;
    g.a[1] = d.y;
    g.a[2] = r;
    // 頭（進行方向側）は丸いまま少しだけ伸び、尾（後ろ側）が長く伸びる＝涙型
    const float head = 1.0f + (d.stretch - 1.0f) * 0.35f;
    const float tail = 1.0f + (d.stretch - 1.0f) * 2.2f;
    g.a[3] = tail / head;
    g.b[0] = d.dirX;
    g.b[1] = d.dirY;
    g.b[2] = head * (1.0f + deform);
    g.b[3] = (1.0f / std::sqrt(d.stretch)) * (1.0f - deform * 0.8f);
  }
  return n;
}

float ScreenDropletSim::Rand() {
  rng_ ^= rng_ << 13;
  rng_ ^= rng_ >> 17;
  rng_ ^= rng_ << 5;
  return static_cast<float>(rng_ & 0xFFFFFFu) / static_cast<float>(0x1000000);
}
