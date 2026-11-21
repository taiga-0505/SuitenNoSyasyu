#pragma once
#include "IComponent.h"
#include "Entity.h"
#include "TransformComponent.h"
#include "Math/MathTypes.h"
#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>

// ============================================================================
// RigidbodyComponent / PhysicsSettings
// ----------------------------------------------------------------------------
// 重力・力・速度で TransformComponent の位置を動かす簡易剛体。
// 衝突の押し出しと反発・摩擦は Scene::ResolveCollisions が行う。
//
// 時間の刻み方（ステップ幅の上限つき分割）:
//   1 フレームの deltaTime を PhysicsSettings::maxStep（既定 1/60 秒）以下に等分して積分する。
//   30fps なら 1/60 秒 × 2 回、144fps なら 1/144 秒 × 1 回。
//   完全な固定ステップ（毎回 1/60 秒）にしないのは、144Hz などで「動くフレーム／動かないフレーム」が
//   交互に出てカクつくのを避けるため（それを消すには描画用の補間が要る）。
//   フレームレートが落ちても 1 回の刻みが大きくならないので、ジャンプの高さや放物線がほぼ同じになる。
//
// スクリプトから:
//   rb->AddForce({0, 0, 10});                          // 押し続ける力（毎フレーム呼ぶ）
//   rb->AddForce({0, 5, 0}, ForceMode::Impulse);       // 一瞬の力（ジャンプ・被弾のノックバック）
//   rb->AddForce({0, 3, 0}, ForceMode::VelocityChange); // 質量を無視して速度を足す
//   rb->restitution = 0.8f;                            // よく跳ねる
//   rb->friction = 0.5f;                               // 床を滑りにくい
// ============================================================================

/// @struct PhysicsSettings
/// @brief 物理全体の設定（シングルトン）
struct PhysicsSettings {
  RC::Vector3 gravity = {0.0f, -9.81f, 0.0f}; ///< 重力加速度（m/s^2）
  float maxStep = 1.0f / 60.0f;  ///< 1 回の積分で進める最大の時間（秒）
  int maxSubSteps = 8;           ///< 1 フレームの最大分割数（これを超える分は 1 回あたりの幅を広げる）
  float bounceThreshold = 0.5f;  ///< これより遅い衝突は跳ねさせない（m/s。置いた物が細かく震えるのを防ぐ）

  /// @brief インスタンスを取得する
  static PhysicsSettings &Get() {
    static PhysicsSettings instance;
    return instance;
  }
};

/// @enum ForceMode
/// @brief AddForce の力の種類
enum class ForceMode {
  Force,          ///< 押し続ける力（N）。そのフレームの間だけ働く。質量で割る
  Acceleration,   ///< 押し続ける加速度（m/s^2）。質量を無視する
  Impulse,        ///< 一瞬の力積（N·s）。すぐに速度が変わる。質量で割る
  VelocityChange, ///< 一瞬で速度を足す（m/s）。質量を無視する
};

/// @class RigidbodyComponent
/// @brief 重力と速度で TransformComponent の位置を動かす簡易剛体コンポーネント
class RigidbodyComponent : public IComponent {
public:
  bool useGravity = true;       ///< 重力の影響を受けるか
  bool isKinematic = false;     ///< true なら物理で位置を更新しない（押し出されもしない）
  float mass = 1.0f;            ///< 質量（kg）。0 以下は 1 として扱う
  RC::Vector3 velocity = {0.0f, 0.0f, 0.0f}; ///< 現在の速度

  float gravityScale = 1.0f;    ///< 重力の倍率（0.5 でふわっと、2 で重く）
  float linearDamping = 0.0f;   ///< 空気抵抗（1 秒あたりの減速の強さ。0 で減速しない）
  float restitution = 0.0f;     ///< 反発係数（0 = 跳ねない、1 = 同じ速さで跳ね返る）
  float friction = 0.0f;        ///< 摩擦係数（0 = つるつる、1 = ほぼ滑らない）
  float maxSpeed = 0.0f;        ///< 速さの上限（m/s。0 で無制限）

  /// @brief コンポーネントの型名を返す
  /// @return "RigidbodyComponent"
  const char* TypeName() const override { return "RigidbodyComponent"; }

  /// @brief 更新順。スクリプトが書き込んだ velocity を同じフレームで反映するため、スクリプトより後
  int UpdateOrder() const override { return ComponentUpdateOrder::kPhysics; }

  /// @brief 有効な質量（0 以下の設定は 1 として扱う）
  float EffectiveMass() const { return (mass > 0.0f) ? mass : 1.0f; }

  /// @brief 力を加える
  /// @param f    力（ワールド座標）
  /// @param mode 力の種類
  void AddForce(const RC::Vector3 &f, ForceMode mode = ForceMode::Force) {
    if (isKinematic) return;
    switch (mode) {
    case ForceMode::Force: {
      const float inv = 1.0f / EffectiveMass();
      accel_ = {accel_.x + f.x * inv, accel_.y + f.y * inv, accel_.z + f.z * inv};
      break;
    }
    case ForceMode::Acceleration:
      accel_ = {accel_.x + f.x, accel_.y + f.y, accel_.z + f.z};
      break;
    case ForceMode::Impulse: {
      const float inv = 1.0f / EffectiveMass();
      velocity = {velocity.x + f.x * inv, velocity.y + f.y * inv, velocity.z + f.z * inv};
      break;
    }
    case ForceMode::VelocityChange:
      velocity = {velocity.x + f.x, velocity.y + f.y, velocity.z + f.z};
      break;
    }
  }

  /// @brief このフレームに溜まった力（Force / Acceleration）を捨てる
  void ClearForces() { accel_ = {0.0f, 0.0f, 0.0f}; }

  /// @brief 重力・力・空気抵抗で速度を更新し、速度で位置を進める
  /// @param deltaTime ゲーム時間の経過秒
  /// @note 無効時と isKinematic 時は何もしない（溜まった力は捨てる）。
  void Update(float deltaTime) override {
    if (!enabled_ || isKinematic) {
      ClearForces();
      return;
    }
    if (!(deltaTime > 0.0f)) return; // 止まっている間は力も持ち越す

    TransformComponent *tr = entity_ ? entity_->GetComponent<TransformComponent>() : nullptr;
    const PhysicsSettings &ps = PhysicsSettings::Get();

    // deltaTime を maxStep 以下に等分する（上限回数を超える分は 1 回の幅を広げる）
    const float maxStep = (ps.maxStep > 0.0f) ? ps.maxStep : deltaTime;
    int steps = static_cast<int>(std::ceil(deltaTime / maxStep - 1e-4f));
    steps = std::clamp(steps, 1, (std::max)(1, ps.maxSubSteps));
    const float h = deltaTime / static_cast<float>(steps);

    const RC::Vector3 g = useGravity
        ? RC::Vector3{ps.gravity.x * gravityScale, ps.gravity.y * gravityScale, ps.gravity.z * gravityScale}
        : RC::Vector3{0.0f, 0.0f, 0.0f};
    const RC::Vector3 a = {g.x + accel_.x, g.y + accel_.y, g.z + accel_.z};

    for (int i = 0; i < steps; ++i) {
      // 半陰的オイラー法（速度を先に更新してから位置を進める）
      velocity = {velocity.x + a.x * h, velocity.y + a.y * h, velocity.z + a.z * h};
      if (linearDamping > 0.0f) {
        const float k = 1.0f / (1.0f + linearDamping * h); // h が大きくても符号が反転しない形
        velocity = {velocity.x * k, velocity.y * k, velocity.z * k};
      }
      ClampSpeed_();
      if (tr) {
        tr->position.x += velocity.x * h;
        tr->position.y += velocity.y * h;
        tr->position.z += velocity.z * h;
      }
    }
    ClearForces();
  }

  /// @brief 剛体パラメータを JSON に書き出す
  nlohmann::json Serialize() const override {
    return {
      {"useGravity", useGravity},
      {"isKinematic", isKinematic},
      {"mass", mass},
      {"velocity", {velocity.x, velocity.y, velocity.z}},
      {"gravityScale", gravityScale},
      {"linearDamping", linearDamping},
      {"restitution", restitution},
      {"friction", friction},
      {"maxSpeed", maxSpeed}
    };
  }

  /// @brief JSON から剛体パラメータを読み込む
  /// @param j 読み込み元の JSON（含まれていないキーは現在の値のまま。古いシーンもそのまま読める）
  void Deserialize(const nlohmann::json& j) override {
    if (j.contains("useGravity")) useGravity = j["useGravity"].get<bool>();
    if (j.contains("isKinematic")) isKinematic = j["isKinematic"].get<bool>();
    if (j.contains("mass")) mass = j["mass"].get<float>();
    if (j.contains("velocity")) {
      auto& v = j["velocity"];
      velocity = {v[0].get<float>(), v[1].get<float>(), v[2].get<float>()};
    }
    if (j.contains("gravityScale")) gravityScale = j["gravityScale"].get<float>();
    if (j.contains("linearDamping")) linearDamping = j["linearDamping"].get<float>();
    if (j.contains("restitution")) restitution = j["restitution"].get<float>();
    if (j.contains("friction")) friction = j["friction"].get<float>();
    if (j.contains("maxSpeed")) maxSpeed = j["maxSpeed"].get<float>();
  }

private:
  void ClampSpeed_() {
    if (!(maxSpeed > 0.0f)) return;
    const float sq = velocity.x * velocity.x + velocity.y * velocity.y + velocity.z * velocity.z;
    if (sq <= maxSpeed * maxSpeed) return;
    const float k = maxSpeed / std::sqrt(sq);
    velocity = {velocity.x * k, velocity.y * k, velocity.z * k};
  }

  RC::Vector3 accel_ = {0.0f, 0.0f, 0.0f}; ///< このフレームに溜まった加速度（Force / Acceleration）
};
