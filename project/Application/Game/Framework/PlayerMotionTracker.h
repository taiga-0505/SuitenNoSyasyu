#pragma once
#include <algorithm>
#include <cmath>
#include <memory>

#include "ECS/CameraComponent.h"
#include "ECS/Entity.h"
#include "ECS/TransformComponent.h"
#include "Scene.h"

/// @file PlayerMotionTracker.h
/// @brief 敵から見た「自機（メインカメラ）」の追跡と、進行方向・速度の推定
/// @details 以前はタコ（OctopusEnemyScript）と鳥（BirdEnemyScript）がそれぞれ
///          「キャッシュした自機の weak_ptr・前フレーム位置・進行方向・速度・推定済みフラグ」を
///          メンバ変数としてばらばらに持ち、同じ推定処理を 2 か所に書いていた
///          （鳥のコメントにも「OctopusEnemyScript と同じ取り方」とあった）。
///          関係する 6 つの変数と処理は 1 つのまとまりなので、クラスに閉じ込めて（カプセル化）
///          敵側は「自機を取る」「推定を進める」「向き・速度を読む」の 3 操作だけを使うようにした。

/// @brief 自機（メインカメラ）を追跡し、レールの進行方向と移動速度を推定する
class PlayerMotionTracker {
public:
  /// @brief シーンのメインカメラ（isMain 優先。無ければ最初のカメラ）を探す
  /// @param scene 検索するシーン
  /// @return 見つかったカメラのエンティティ。無ければ nullptr
  static std::shared_ptr<Entity> FindPlayerCamera(Scene *scene) {
    if (!scene) return nullptr;
    std::shared_ptr<Entity> fallback;
    for (const auto &e : scene->GetEntities()) {
      if (!e || !e->IsActive() || e->IsPendingDestroy()) continue;
      auto *cam = e->GetComponent<CameraComponent>();
      if (!cam || !e->GetComponent<TransformComponent>()) continue;
      if (cam->isMain) return e;
      if (!fallback) fallback = e;
    }
    return fallback;
  }

  /// @brief 自機（カメラ）の Transform を取得する
  /// @param scene 自機を探すシーン
  /// @return 自機の Transform。自機がいない／Transform が無ければ nullptr
  /// @details キャッシュが無効になったときだけ探し直す。探し直したときは前フレーム位置が
  ///          別物になるので、速度の推定をやり直す（前フレーム位置を捨てる）。
  TransformComponent *AcquirePlayer(Scene *scene) {
    std::shared_ptr<Entity> cam = target_.lock();
    if (!cam || cam->IsPendingDestroy() || !cam->IsActive()) {
      cam = FindPlayerCamera(scene);
      target_ = cam;
      hasLastPos_ = false;
    }
    if (!cam) return nullptr;
    return cam->GetComponent<TransformComponent>();
  }

  /// @brief 自機の進行方向（レールの向き）と移動速度の推定を 1 フレーム進める
  /// @param camTr 自機（カメラ）の Transform
  /// @param dt 前フレームからの経過秒数（0 より大きいこと）
  /// @details 視点操作で向きが揺れても陣取る位置がぶれないよう、カメラの回転ではなく移動方向を使う。
  ///          止まっているときだけ回転から取る。ウェーブ中はレールが止まるので、
  ///          最後に動いていた向きをそのまま使い続ける。
  void Update(const TransformComponent &camTr, float dt) {
    if (hasLastPos_) {
      const float vx = (camTr.position.x - lastPos_.x) / dt;
      const float vz = (camTr.position.z - lastPos_.z) / dt;
      const float sp = std::sqrt(vx * vx + vz * vz);
      // 偏差撃ち用の速度。フレームごとのガタつきを均す
      const float vb = (std::min)(1.0f, dt * 4.0f);
      velocity_.x += (vx - velocity_.x) * vb;
      velocity_.z += (vz - velocity_.z) * vb;
      if (sp > 0.5f) {
        const float blend = (std::min)(1.0f, dt * 3.0f);
        forward_.x += (vx / sp - forward_.x) * blend;
        forward_.z += (vz / sp - forward_.z) * blend;
      } else if (!hasForward_) {
        forward_ = {std::sin(camTr.rotation.y), 0.0f, std::cos(camTr.rotation.y)};
      }
      hasForward_ = true;
    } else if (!hasForward_) {
      forward_ = {std::sin(camTr.rotation.y), 0.0f, std::cos(camTr.rotation.y)};
    }
    const float l = std::sqrt(forward_.x * forward_.x + forward_.z * forward_.z);
    if (l > 1e-4f) {
      forward_.x /= l;
      forward_.z /= l;
    }
    lastPos_ = camTr.position;
    hasLastPos_ = true;
  }

  /// @brief 自機の進行方向（XZ 平面・正規化済み）
  /// @return 進行方向
  const RC::Vector3 &Forward() const { return forward_; }

  /// @brief 自機の進行方向に対して右向きのベクトル（XZ 平面）
  /// @return 右向きのベクトル
  RC::Vector3 Right() const { return {forward_.z, 0.0f, -forward_.x}; }

  /// @brief 自機の移動速度の推定値（XZ のみ。y は常に 0）
  /// @return 移動速度 (m/s)
  const RC::Vector3 &Velocity() const { return velocity_; }

private:
  std::weak_ptr<Entity> target_;              ///< キャッシュした自機（カメラ）
  RC::Vector3 forward_ = {0.0f, 0.0f, 1.0f};  ///< 推定した進行方向
  RC::Vector3 velocity_ = {0.0f, 0.0f, 0.0f}; ///< 推定した移動速度
  RC::Vector3 lastPos_ = {0.0f, 0.0f, 0.0f};  ///< 前フレームの自機の位置
  bool hasLastPos_ = false;                   ///< lastPos_ が有効か
  bool hasForward_ = false;                   ///< forward_ を一度でも決めたか
};
