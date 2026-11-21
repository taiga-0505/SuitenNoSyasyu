#pragma once

// ============================================================================
// PhysicsQuery
// ----------------------------------------------------------------------------
// Scene::Raycast / SphereCast / OverlapSphere / OverlapBox で使う結果と絞り込み条件。
// 判定するのは ColliderComponent（AABB / 球。Capsule は他の判定と同じく AABB 扱い）。
//
// 使い方（スクリプトから）:
//   RaycastHit hit;
//   if (Raycast(muzzle, forward, 100.0f, hit)) {       // 自分自身は自動で除外される
//     hit.entity->Destroy();
//   }
//
//   QueryFilter f;
//   f.layerMask = kLayerEnemy;                         // ColliderComponent::layer と AND して 0 なら無視
//   std::vector<Entity*> near;
//   GetScene()->OverlapSphere(pos, 3.0f, near, f);
// ============================================================================

#include "Math/MathTypes.h"
#include <cstdint>

class Entity;

/// @struct RaycastHit
/// @brief レイ / 球を飛ばしたときに当たった物の情報
struct RaycastHit {
  Entity *entity = nullptr;             ///< 当たったエンティティ（シーンが保持している間だけ有効）
  RC::Vector3 point{0.0f, 0.0f, 0.0f};  ///< 当たった位置（ワールド座標。相手の表面上）
  RC::Vector3 normal{0.0f, 1.0f, 0.0f}; ///< 当たった面の法線（相手の外向き、正規化済み）
  float distance = 0.0f;                ///< 起点から当たった位置までの距離（SphereCast は球の中心が進んだ距離）
};

/// @struct QueryFilter
/// @brief 判定対象の絞り込み条件
struct QueryFilter {
  uint32_t layerMask = 0xFFFFFFFFu; ///< ColliderComponent::layer と AND して 0 の物は無視する
  bool includeTriggers = false;     ///< isTrigger のコライダーも対象にするか
  const Entity *ignore = nullptr;   ///< 無視するエンティティ（撃った本人など）
};
