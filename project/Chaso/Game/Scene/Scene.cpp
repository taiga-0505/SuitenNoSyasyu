#include "Scene.h"
#include "SceneFlow.h"
#include "ECS/Entity.h"
#include "ECS/TransformComponent.h"
#include "ECS/LightComponent.h"
#include "ECS/CameraComponent.h"
#include "ECS/ColliderComponent.h"
#include "ECS/RigidbodyComponent.h"
#include "ECS/NativeScriptComponent.h"
#include "ECS/ScriptableEntity.h"
#include "Common/Math/MathUtils.h"
#include "Common/Log/Log.h"
#include "RenderCommon.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <string>

void Scene::DrawLightGizmos(uint32_t selectedEntityId) {
#if RC_ENABLE_IMGUI
  if (selectedEntityId == 0) return; // 未選択時は描画しない
  for (auto& e : entities_) {
    if (e->Id() != selectedEntityId) continue;
    if (!e->IsVisible()) continue;
    auto* tr = e->GetComponent<TransformComponent>();
    if (!tr) continue;
    RC::Vector3 pos = tr->position;

    // ==========================================================
    // Directional Light — 平行矢印の束（太陽光のイメージ）
    // ==========================================================
    if (auto* dirLight = e->GetComponent<DirectionalLightComponent>()) {
      if (!dirLight->visible) continue;
      RC::Vector3 d = Normalize(dirLight->direction);
      float arrowLen = 3.0f;
      float spread  = 1.0f; // 矢印の間隔

      // direction に直交するベクトルを2本作る
      RC::Vector3 up = {0.0f, 1.0f, 0.0f};
      float dot = d.x * up.x + d.y * up.y + d.z * up.z;
      if (std::abs(dot) > 0.99f) up = {1.0f, 0.0f, 0.0f};

      RC::Vector3 right = {
        d.y * up.z - d.z * up.y,
        d.z * up.x - d.x * up.z,
        d.x * up.y - d.y * up.x
      };
      float rLen = std::sqrt(right.x * right.x + right.y * right.y + right.z * right.z);
      if (rLen > 1e-6f) { right.x /= rLen; right.y /= rLen; right.z /= rLen; }

      RC::Vector3 upLocal = {
        right.y * d.z - right.z * d.y,
        right.z * d.x - right.x * d.z,
        right.x * d.y - right.y * d.x
      };

      // 中央 + 上下左右の5本の平行矢印
      float offsets[][2] = {{0,0}, {1,0}, {-1,0}, {0,1}, {0,-1}};
      for (auto& off : offsets) {
        RC::Vector3 origin = {
          pos.x + right.x * off[0] * spread + upLocal.x * off[1] * spread,
          pos.y + right.y * off[0] * spread + upLocal.y * off[1] * spread,
          pos.z + right.z * off[0] * spread + upLocal.z * off[1] * spread
        };
        RC::Vector3 tip = {
          origin.x + d.x * arrowLen,
          origin.y + d.y * arrowLen,
          origin.z + d.z * arrowLen
        };
        RC::DrawLine3D(origin, tip, dirLight->color, true);

        // 矢じり（先端から斜め2本）
        float headLen = 0.5f;
        RC::Vector3 back = {-d.x, -d.y, -d.z};
        RC::Vector3 h1 = {
          tip.x + (back.x + right.x * 0.4f) * headLen,
          tip.y + (back.y + right.y * 0.4f) * headLen,
          tip.z + (back.z + right.z * 0.4f) * headLen
        };
        RC::Vector3 h2 = {
          tip.x + (back.x - right.x * 0.4f) * headLen,
          tip.y + (back.y - right.y * 0.4f) * headLen,
          tip.z + (back.z - right.z * 0.4f) * headLen
        };
        RC::DrawLine3D(tip, h1, dirLight->color, true);
        RC::DrawLine3D(tip, h2, dirLight->color, true);
      }
    }

    // ==========================================================
    // Point Light — 3軸リング球
    // ==========================================================
    if (auto* ptLight = e->GetComponent<PointLightComponent>()) {
      if (!ptLight->visible) continue;
      RC::DrawSphereRings3D(pos, ptLight->radius, ptLight->color, 32, true);
    }

    // ==========================================================
    // Spot Light — コーン（三角錐）
    // ==========================================================
    if (auto* spLight = e->GetComponent<SpotLightComponent>()) {
      if (!spLight->visible) continue;

      RC::Vector3 d = Normalize(spLight->direction);
      float dist = spLight->distance;

      // ライト本体と同じくオフセットを適用した位置を起点にする
      RC::Vector3 spotPos = spLight->ResolvePosition(tr->position, tr->rotation);

      // コーンの底面半径 = distance * tan(acos(cosAngle))
      float cosA = spLight->cosAngle;
      if (cosA < 0.001f) cosA = 0.001f;
      if (cosA > 0.999f) cosA = 0.999f;
      float sinA = std::sqrt(1.0f - cosA * cosA);
      float baseRadius = dist * (sinA / cosA);

      // コーン先端
      RC::Vector3 tip = {
        spotPos.x + d.x * dist,
        spotPos.y + d.y * dist,
        spotPos.z + d.z * dist
      };

      // 中心線
      RC::DrawLine3D(spotPos, tip, spLight->color, true);

      // 直交ベクトル
      RC::Vector3 up = {0.0f, 1.0f, 0.0f};
      float dot = d.x * up.x + d.y * up.y + d.z * up.z;
      if (std::abs(dot) > 0.99f) up = {1.0f, 0.0f, 0.0f};

      RC::Vector3 right = {
        d.y * up.z - d.z * up.y,
        d.z * up.x - d.x * up.z,
        d.x * up.y - d.y * up.x
      };
      float rLen = std::sqrt(right.x * right.x + right.y * right.y + right.z * right.z);
      if (rLen > 1e-6f) { right.x /= rLen; right.y /= rLen; right.z /= rLen; }

      RC::Vector3 upLocal = {
        right.y * d.z - right.z * d.y,
        right.z * d.x - right.x * d.z,
        right.x * d.y - right.y * d.x
      };

      // 底面円 + 4本の側面線
      constexpr int kSeg = 24;
      constexpr float kPi2 = 6.2831853f;
      RC::Vector4 coneColor = {spLight->color.x, spLight->color.y, spLight->color.z, 0.5f};

      RC::Vector3 prevPt = {};
      for (int i = 0; i <= kSeg; ++i) {
        float angle = kPi2 * static_cast<float>(i) / static_cast<float>(kSeg);
        float cx = std::cos(angle);
        float cy = std::sin(angle);

        RC::Vector3 pt = {
          tip.x + (right.x * cx + upLocal.x * cy) * baseRadius,
          tip.y + (right.y * cx + upLocal.y * cy) * baseRadius,
          tip.z + (right.z * cx + upLocal.z * cy) * baseRadius
        };

        if (i > 0) {
          RC::DrawLine3D(prevPt, pt, coneColor, true);
        }
        // 4本の側面線（0°, 90°, 180°, 270°）
        if (i % (kSeg / 4) == 0 && i < kSeg) {
          RC::DrawLine3D(spotPos, pt, spLight->color, true);
        }
        prevPt = pt;
      }
    }

    // ==========================================================
    // Area Light — 四角形 + 法線方向の矢印
    // ==========================================================
    if (auto* arLight = e->GetComponent<AreaLightComponent>()) {
      if (!arLight->visible) continue;

      float hw = arLight->halfWidth;
      float hh = arLight->halfHeight;
      float hr = arLight->range * 0.5f; // rangeの半分をZ方向に

      // AABB（ライトの照射ボリューム）
      RC::Vector3 mn = {pos.x - hw, pos.y - hh, pos.z - hr};
      RC::Vector3 mx = {pos.x + hw, pos.y + hh, pos.z + hr};
      RC::DrawAABB3D(mn, mx, arLight->color, true);

      // 法線方向の矢印（Z+ 方向）
      float arrowLen = 1.5f;
      RC::Vector3 nTip = {pos.x, pos.y, pos.z + arrowLen};
      RC::DrawLine3D(pos, nTip, arLight->color, true);

      // 矢じり
      float hl = 0.3f;
      RC::DrawLine3D(nTip, {nTip.x + hl, nTip.y, nTip.z - hl}, arLight->color, true);
      RC::DrawLine3D(nTip, {nTip.x - hl, nTip.y, nTip.z - hl}, arLight->color, true);
      RC::DrawLine3D(nTip, {nTip.x, nTip.y + hl, nTip.z - hl}, arLight->color, true);
      RC::DrawLine3D(nTip, {nTip.x, nTip.y - hl, nTip.z - hl}, arLight->color, true);
    }
  }
#endif
}

void Scene::DrawCameraGizmos(uint32_t selectedEntityId, float aspect) {
#if RC_ENABLE_IMGUI
  if (selectedEntityId == 0 && !showAllGizmos_) return;
  for (auto& e : entities_) {
    if (!showAllGizmos_ && e->Id() != selectedEntityId) continue;
    if (!e->IsVisible()) continue;
    auto* tr = e->GetComponent<TransformComponent>();
    auto* cam = e->GetComponent<CameraComponent>();
    if (!tr || !cam) continue;

    RC::Vector4 frustumColor = {0.5f, 0.8f, 1.0f, 1.0f}; // 水色
    RC::DrawFrustum3D(tr->position, tr->rotation,
                      cam->fovY, aspect, cam->nearZ, cam->farZ,
                      frustumColor, true);
  }
#endif
}

void Scene::DrawColliderGizmos(uint32_t selectedEntityId) {
#if RC_ENABLE_IMGUI
  if (selectedEntityId == 0 && !showColliderGizmos_ && !showAllGizmos_) return;
  for (auto& e : entities_) {
    if (!showColliderGizmos_ && !showAllGizmos_ && e->Id() != selectedEntityId) continue;
    if (!e || !e->IsActive() || e->IsPendingDestroy() || !e->IsVisible()) continue;
    auto* tr = e->GetComponent<TransformComponent>();
    auto* col = e->GetComponent<ColliderComponent>();
    if (!tr || !col || !col->IsEnabled()) continue;

    RC::Vector4 colColor = {0.2f, 1.0f, 0.2f, 1.0f}; // 黄緑色
    RC::Vector3 scaledCenter = {
      col->center.x * tr->scale.x,
      col->center.y * tr->scale.y,
      col->center.z * tr->scale.z
    };
    RC::Vector3 worldCenter = {
      tr->position.x + scaledCenter.x,
      tr->position.y + scaledCenter.y,
      tr->position.z + scaledCenter.z
    };

    if (col->shape == ColliderComponent::Shape::Sphere) {
        float maxScale = (std::max)((std::max)(std::abs(tr->scale.x), std::abs(tr->scale.y)), std::abs(tr->scale.z));
        RC::DrawSphereRings3D(worldCenter, col->radius * maxScale, colColor, 16, true);
    } else if (col->shape == ColliderComponent::Shape::AABB) {
        RC::Vector3 scaledSize = {
            std::abs(col->size.x * tr->scale.x),
            std::abs(col->size.y * tr->scale.y),
            std::abs(col->size.z * tr->scale.z)
        };
        RC::Vector3 halfSize = { scaledSize.x * 0.5f, scaledSize.y * 0.5f, scaledSize.z * 0.5f };
        RC::Vector3 minPos = { worldCenter.x - halfSize.x, worldCenter.y - halfSize.y, worldCenter.z - halfSize.z };
        RC::Vector3 maxPos = { worldCenter.x + halfSize.x, worldCenter.y + halfSize.y, worldCenter.z + halfSize.z };
        RC::DrawAABB3D(minPos, maxPos, colColor, true);
    }
  }
#endif
}

// ============================================================
// 当たり判定付き移動（キャラクターコントローラー用）
// ============================================================
namespace {

/// @brief コライダーのワールド空間での形状（AABB / 球）
struct ColliderVolume {
    bool isSphere = false;
    RC::Vector3 center{0.0f, 0.0f, 0.0f};
    RC::Vector3 half{0.0f, 0.0f, 0.0f}; ///< AABB の半サイズ
    float radius = 0.0f;                ///< 球の半径
};

/// @brief エンティティのコライダーをワールド空間の形状に変換する
/// @param overridePos TransformComponent::position の代わりに使う位置
/// @param shrink 形状を内側に縮める量（m）。0以下にはならないようクランプする
/// @return コライダーが無い / 無効なら false
bool BuildColliderVolume(Entity* e, const RC::Vector3& overridePos, float shrink,
                         ColliderVolume& out) {
    if (!e) return false;
    auto* tr = e->GetComponent<TransformComponent>();
    auto* col = e->GetComponent<ColliderComponent>();
    if (!tr || !col || !col->IsEnabled()) return false;

    const RC::Vector3 scaledCenter = {
        col->center.x * tr->scale.x,
        col->center.y * tr->scale.y,
        col->center.z * tr->scale.z
    };
    out.center = RC::Add(overridePos, scaledCenter);

    if (col->shape == ColliderComponent::Shape::Sphere) {
        out.isSphere = true;
        const float maxScale = (std::max)((std::max)(std::abs(tr->scale.x), std::abs(tr->scale.y)),
                                          std::abs(tr->scale.z));
        out.radius = (std::max)(0.001f, col->radius * maxScale - shrink);
    } else {
        // Capsule は未実装のため AABB として扱う（ResolveCollisions と同じ扱い）
        out.isSphere = false;
        out.half = {
            (std::max)(0.001f, std::abs(col->size.x * tr->scale.x * 0.5f) - shrink),
            (std::max)(0.001f, std::abs(col->size.y * tr->scale.y * 0.5f) - shrink),
            (std::max)(0.001f, std::abs(col->size.z * tr->scale.z * 0.5f) - shrink)
        };
    }
    return true;
}

/// @brief 2つの形状が重なっているか判定する
bool VolumesOverlap(const ColliderVolume& a, const ColliderVolume& b) {
    if (!a.isSphere && !b.isSphere) {
        const RC::Vector3 minA = RC::Sub(a.center, a.half), maxA = RC::Add(a.center, a.half);
        const RC::Vector3 minB = RC::Sub(b.center, b.half), maxB = RC::Add(b.center, b.half);
        return RC::CheckCollisionAabbAabb(minA, maxA, minB, maxB).hit;
    }
    if (a.isSphere && b.isSphere) {
        return RC::CheckCollisionSphereSphere(a.center, a.radius, b.center, b.radius).hit;
    }
    // 球 vs AABB
    const ColliderVolume& s = a.isSphere ? a : b;
    const ColliderVolume& box = a.isSphere ? b : a;
    const RC::Vector3 minB = RC::Sub(box.center, box.half), maxB = RC::Add(box.center, box.half);
    return RC::CheckCollisionSphereAabb(s.center, s.radius, minB, maxB).hit;
}

// 衝突判定の粗い除外（Broad-phase）に用いる安全距離マージン定数
inline constexpr float kBroadPhaseDistanceMargin = 3.0f; // 余裕を持たせた近接判定マージン（m）

} // namespace

bool Scene::TestBlockingOverlap(Entity* self, const RC::Vector3& testPos, float skin,
                                Entity** hitOut) {
    if (hitOut) *hitOut = nullptr;
    if (!self) return false;

    auto* selfCol = self->GetComponent<ColliderComponent>();
    if (!selfCol || !selfCol->IsEnabled() || selfCol->isTrigger) return false;

    ColliderVolume selfVol;
    if (!BuildColliderVolume(self, testPos, skin, selfVol)) return false;

    // BVH が一度も作られていない（UpdateEntities を通らずに呼ばれた）場合はここで作る
    if (broadphase_.IsDirty()) {
        broadphase_.Sync(entities_);
    }

    // 粗い判定（Broad-phase）: 自身の形状を包む箱と重なる候補だけを BVH から引く。
    // 以前は全エンティティを走査し「XZ 平面で中心から 3m 以内」で絞っていたが、
    // それだと中心が遠い大きな壁（長い塀など）を取りこぼしていた。
    const RC::Vector3 selfHalf = selfVol.isSphere
        ? RC::Vector3{selfVol.radius, selfVol.radius, selfVol.radius}
        : selfVol.half;
    const RC::BoundingBox queryBox = RC::BoundingBox::FromCenterHalf(selfVol.center, selfHalf);

    // 重なった相手が複数あるときは、以前と同じく entities_ の並びで最も前のものを返す
    // （MoveWithCollision が「どの障害物にめり込んでいるか」を比べるため、毎回同じ相手を返す必要がある）
    Entity* best = nullptr;
    uint32_t bestOrder = UINT32_MAX;
    broadphase_.QueryBox(queryBox, [&](const ColliderBroadphase::Entry& en) {
        if (en.order >= bestOrder) return true;
        // Sync 以降に破棄されたエンティティを触らないよう、生存を確かめてから使う
        const std::shared_ptr<Entity> other = en.weak.lock();
        if (!other || other.get() == self) return true;
        if (!other->IsActive() || other->IsPendingDestroy()) return true;

        auto* otherCol = other->GetComponent<ColliderComponent>();
        if (!otherCol || !otherCol->IsEnabled() || otherCol->isTrigger) return true;
        // レイヤーマスクが噛み合わない組み合わせは無視する
        if ((selfCol->layer & otherCol->layer) == 0) return true;

        auto* otherTr = other->GetComponent<TransformComponent>();
        if (!otherTr) return true;

        ColliderVolume otherVol;
        if (!BuildColliderVolume(other.get(), otherTr->position, 0.0f, otherVol)) return true;

        if (VolumesOverlap(selfVol, otherVol)) {
            best = other.get(); // entities_ が保持しているので、ここを抜けても生きている
            bestOrder = en.order;
        }
        return true;
    });

    if (best) {
        if (hitOut) *hitOut = best;
        return true;
    }
    return false;
}

RC::Vector3 Scene::MoveWithCollision(Entity* self, const RC::Vector3& delta, float maxStep,
                                     float skin) {
    RC::Vector3 moved{0.0f, 0.0f, 0.0f};
    if (!self) return moved;
    auto* tr = self->GetComponent<TransformComponent>();
    if (!tr) return moved;

    const RC::Vector3 startPos = tr->position;

    // コライダーが無い場合は従来通りそのまま移動する
    if (!self->GetComponent<ColliderComponent>()) {
        tr->position = RC::Add(startPos, delta);
        return delta;
    }

    // --- 移動量を maxStep 以下に分割する（トンネリング対策の本体）---
    // 分割数 = 最大成分 / maxStep。deltaTime のスパイクで delta が大きくなっても
    // 1回の判定で進む距離は必ず maxStep 以下に保たれるため、薄い壁も飛び越えない。
    const float maxComponent = (std::max)((std::max)(std::abs(delta.x), std::abs(delta.y)),
                                          std::abs(delta.z));
    if (maxComponent <= 0.0f) return moved;

    const float safeStep = (maxStep > 0.0001f) ? maxStep : 0.1f;
    int steps = static_cast<int>(std::ceil(maxComponent / safeStep));
    if (steps < 1) steps = 1;
    if (steps > 64) steps = 64; // 極端な delta でのコスト爆発を防ぐ上限

    const RC::Vector3 step = RC::Mul(delta, 1.0f / static_cast<float>(steps));

    for (int i = 0; i < steps; ++i) {
        // このステップ開始時点で既にめり込んでいるか
        Entity* stuckHit = nullptr;
        const bool stuckAtStart = TestBlockingOverlap(self, tr->position, skin, &stuckHit);

        // 軸ごとに個別に試す。ブロックされた軸だけ取り消すので
        // 斜め移動で壁に当たっても残りの成分で壁に沿ってスライドする。
        const float stepAxis[3] = {step.x, step.y, step.z};
        for (int axis = 0; axis < 3; ++axis) {
            if (stepAxis[axis] == 0.0f) continue;

            RC::Vector3 candidate = tr->position;
            if (axis == 0) candidate.x += stepAxis[axis];
            else if (axis == 1) candidate.y += stepAxis[axis];
            else candidate.z += stepAxis[axis];

            Entity* candHit = nullptr;
            const bool candOverlap = TestBlockingOverlap(self, candidate, skin, &candHit);

            if (candOverlap) {
                // 通常時：障害物と重なる移動はブロック
                if (!stuckAtStart) {
                    continue;
                }

                // スタック脱出中：別の障害物に当たる場合はブロック
                if (candHit != stuckHit) {
                    continue;
                }

                // スタック中の同一障害物に対して、中心から離れる（脱出する）方向の移動のみ許可
                if (auto* obsTr = stuckHit ? stuckHit->GetComponent<TransformComponent>() : nullptr) {
                    const float curDx = tr->position.x - obsTr->position.x;
                    const float curDz = tr->position.z - obsTr->position.z;
                    const float curDistSq = curDx * curDx + curDz * curDz;

                    const float candDx = candidate.x - obsTr->position.x;
                    const float candDz = candidate.z - obsTr->position.z;
                    const float candDistSq = candDx * candDx + candDz * candDz;

                    // 障害物中心に近づく（さらに深くめり込む）場合はブロック
                    if (candDistSq <= curDistSq) {
                        continue;
                    }
                } else {
                    continue;
                }
            }
            tr->position = candidate;
        }
    }

    moved = RC::Sub(tr->position, startPos);
    // 動いたぶんを BVH に反映し、同じフレームで後から動く他のキャラクターの判定に使わせる
    if (moved.x != 0.0f || moved.y != 0.0f || moved.z != 0.0f) {
        broadphase_.Refresh(self);
    }
    return moved;
}

// ScriptableEntity::MoveAndSlide の実体。
// 宣言は Engine/ECS/ScriptableEntity.h（Scene は前方宣言のみ）にあるため、
// Scene の完全型が見えるこの翻訳単位で定義する。
RC::Vector3 ScriptableEntity::MoveAndSlide(const RC::Vector3& delta, float maxStep) {
    Scene* scene = GetScene();
    Entity* e = GetEntity();
    if (!e) return {0.0f, 0.0f, 0.0f};

    if (!scene) {
        // シーン未設定時（単体テスト等）は従来通り直接移動
        if (auto* tr = e->GetComponent<TransformComponent>()) {
            tr->position = RC::Add(tr->position, delta);
        }
        return delta;
    }
    return scene->MoveWithCollision(e, delta, maxStep);
}

// ScriptableEntity::RequestSceneChange の実体。
// 宣言は Engine/ECS/ScriptableEntity.h（SceneContext は前方宣言のみ）にあるため、
// SceneContext の完全型が見えるこの翻訳単位で定義する。
//
// 経路は ScriptableEntity -> SceneContext::requestSceneChange -> SceneManager::RequestChange
// の一方向。エンジン層（ECS）がアプリ層（SceneManager）を型として知らずに済む。
bool ScriptableEntity::RequestSceneChange(const std::string& name) {
    return RequestSceneChange(name, SceneTransitions::kDissolve);
}

bool ScriptableEntity::RequestSceneChange(const std::string& name,
                                          const std::string& transition) {
    SceneContext* ctx = GetSceneContext();
    if (!ctx) {
        Log::Print("[Script] RequestSceneChange: SceneContext が未設定です (" + name + ")");
        return false;
    }

    // 編集モード（Stopped）や一時停止中は遷移させない。
    // DataDrivenScene::Update は停止中も dt=0 で UpdateEntities を回すため、
    // 入力トリガで遷移を書くとエディタで編集中に飛んで未保存の変更が消える。
    if (!ctx->isPlaying()) {
        return false;
    }

    if (!ctx->requestSceneChange) {
        Log::Print("[Script] RequestSceneChange: 受け口が未結線です (" + name + ")");
        return false;
    }

    // 演出名はここで型へ変換する（不明な名前は既定の Dissolve）
    return ctx->requestSceneChange(name, ParseSceneTransition(transition));
}

// ScriptableEntity::LookupTransition / RequestTransition の実体。
// 遷移元は「このスクリプトが載っているシーン」。遷移表は SceneFlow（Resources/SceneFlow.json）。
bool ScriptableEntity::LookupTransition(const std::string& trigger, std::string& outTarget,
                                        std::string& outTransition, const std::string& arg) {
    outTarget.clear();
    outTransition = SceneTransitions::kDissolve;
    Scene* scene = GetScene();
    if (!scene) {
        Log::Print("[Script] LookupTransition: シーンが未設定です (" + trigger + ")");
        return false;
    }
    SceneFlowResult result;
    if (!SceneFlow::Get().Resolve(scene->Name(), trigger, arg, result)) {
        return false;
    }
    outTarget = result.target;
    outTransition = result.transition;
    return true;
}

bool ScriptableEntity::RequestTransition(const std::string& trigger, const std::string& arg) {
    std::string target;
    std::string transition;
    if (!LookupTransition(trigger, target, transition, arg)) {
        return false;
    }
    if (target.empty()) {
        Log::Print("[Script] RequestTransition: 遷移先が空です (" + trigger + ")");
        return false;
    }
    return RequestSceneChange(target, transition);
}

void Scene::ResolveCollisions() {
    // ------------------------------------------------------------------
    // 粗い判定（Broad-phase）: DynamicBVH で「AABB が重なっている可能性のある組」だけを列挙する。
    // 以前は全エンティティの総当たり（O(N^2)）で、1 万個では毎フレーム約 5000 万ペアを調べていた。
    //
    // 処理順は以前の二重ループ（i 昇順 → j 昇順）と同じになるよう、ペアを並べてから回す。
    // 押し出しで位置が変わるのは以前と同じくペアごとに逐次反映される。
    // BVH の葉は ColliderBroadphase::kFatMargin だけ太らせてあるので、
    // このループ中の押し出しで少し動いた物同士も候補から漏れない。
    //
    // 有効／無効・位置などの「状態」は以前どおりペアごとに読む（OnCollision の中で
    // コライダーが無効化される等、同フレーム内の変化を取りこぼさないため）。
    // コンポーネントのポインタは Sync 時点のもの（以前の ColliderCache と同じ扱い）。
    // ------------------------------------------------------------------
    broadphase_.Sync(entities_);

    static std::vector<std::pair<uint32_t, uint32_t>> s_pairs; // 毎フレーム clear して使い回す
    // コライダーデバッグ表示中は全ペアを（接触点を描くため）、それ以外は
    // 「押し出し対象か OnCollision を受けるスクリプトを持つ」側を含むペアだけを調べる
    broadphase_.CollectPairs(showColliderGizmos_, s_pairs);

    for (const auto& [i, j] : s_pairs) {
        auto& e1 = entities_[i];
        if (!e1 || !e1->IsActive() || e1->IsPendingDestroy()) continue;
        const ColliderBroadphase::Entry* c1 = broadphase_.EntryAt(i);
        const ColliderBroadphase::Entry* c2 = broadphase_.EntryAt(j);
        if (!c1 || !c2) continue;

        auto* tr1 = c1->tr;
        auto* col1 = c1->col;
        if (!tr1 || !col1 || !col1->IsEnabled()) continue;

        auto* rb1 = c1->rb;
        auto* nsc1 = c1->nsc;

        // 物理押し出しを行わず、OnCollisionコールバックを受け取るスクリプトも無く、
        // コライダーデバッグ表示も無効なペア（壁 vs 壁 など）は何も起きないので最初に弾く。
        // （isKinematic は実行中に変わり得るので、下で改めて判定する。ここは粗い前振り分け）
        if (!c1->interesting && !c2->interesting && !showColliderGizmos_) {
            continue;
        }

        auto& e2 = entities_[j];
        if (!e2 || !e2->IsActive() || e2->IsPendingDestroy()) continue;
        auto* tr2 = c2->tr;
        auto* col2 = c2->col;
        if (!tr2 || !col2 || !col2->IsEnabled()) continue;

        // レイヤーマスクが噛み合わない組み合わせは即スキップ
        if ((col1->layer & col2->layer) == 0) continue;

        auto* rb2 = c2->rb;
        const bool isDynamic1 = (rb1 && !rb1->isKinematic);
        const bool isDynamic2 = (rb2 && !rb2->isKinematic);

        auto* nsc2 = c2->nsc;

        // 物理押し出しを行わず、OnCollisionコールバックを受け取るスクリプトも無く、
        // コライダーデバッグ表示も無効なペアは幾何判定を行わずに早期スキップ
        if (!isDynamic1 && !isDynamic2 && !nsc1 && !nsc2 && !showColliderGizmos_) {
            continue;
        }

        // e1 の情報計算
        RC::Vector3 scaledCenter1 = {
            col1->center.x * tr1->scale.x,
            col1->center.y * tr1->scale.y,
            col1->center.z * tr1->scale.z
        };
        RC::Vector3 center1 = RC::Add(tr1->position, scaledCenter1);

        // e2 の情報計算
        RC::Vector3 scaledCenter2 = {
            col2->center.x * tr2->scale.x,
            col2->center.y * tr2->scale.y,
            col2->center.z * tr2->scale.z
        };
        RC::Vector3 center2 = RC::Add(tr2->position, scaledCenter2);

        // 粗い距離判定（Broad-phase）：互いの最大バウンディング半径＋マージンより離れていれば交差判定をスキップ
        const float approxExtent1 = (col1->shape == ColliderComponent::Shape::Sphere)
            ? (col1->radius * (std::max)({ std::abs(tr1->scale.x), std::abs(tr1->scale.y), std::abs(tr1->scale.z) }))
            : (0.5f * (std::max)({ std::abs(col1->size.x * tr1->scale.x), std::abs(col1->size.y * tr1->scale.y), std::abs(col1->size.z * tr1->scale.z) }));
        const float approxExtent2 = (col2->shape == ColliderComponent::Shape::Sphere)
            ? (col2->radius * (std::max)({ std::abs(tr2->scale.x), std::abs(tr2->scale.y), std::abs(tr2->scale.z) }))
            : (0.5f * (std::max)({ std::abs(col2->size.x * tr2->scale.x), std::abs(col2->size.y * tr2->scale.y), std::abs(col2->size.z * tr2->scale.z) }));
        const float broadDistance = approxExtent1 + approxExtent2 + kBroadPhaseDistanceMargin;
        const float broadDistanceSq = broadDistance * broadDistance;
        const float dx = center1.x - center2.x;
        const float dy = center1.y - center2.y;
        const float dz = center1.z - center2.z;
        if (dx * dx + dy * dy + dz * dz > broadDistanceSq) continue;

        RC::CollisionResult result;
        bool reverseNormal = false;

        if (col1->shape == ColliderComponent::Shape::Sphere && col2->shape == ColliderComponent::Shape::Sphere) {
            float r1 = col1->radius * (std::max)((std::max)(std::abs(tr1->scale.x), std::abs(tr1->scale.y)), std::abs(tr1->scale.z));
            float r2 = col2->radius * (std::max)((std::max)(std::abs(tr2->scale.x), std::abs(tr2->scale.y)), std::abs(tr2->scale.z));
            result = RC::CheckCollisionSphereSphere(center1, r1, center2, r2);
        } else if (col1->shape == ColliderComponent::Shape::AABB && col2->shape == ColliderComponent::Shape::AABB) {
            RC::Vector3 h1 = { std::abs(col1->size.x * tr1->scale.x * 0.5f), std::abs(col1->size.y * tr1->scale.y * 0.5f), std::abs(col1->size.z * tr1->scale.z * 0.5f) };
            RC::Vector3 h2 = { std::abs(col2->size.x * tr2->scale.x * 0.5f), std::abs(col2->size.y * tr2->scale.y * 0.5f), std::abs(col2->size.z * tr2->scale.z * 0.5f) };
            RC::Vector3 min1 = RC::Sub(center1, h1); RC::Vector3 max1 = RC::Add(center1, h1);
            RC::Vector3 min2 = RC::Sub(center2, h2); RC::Vector3 max2 = RC::Add(center2, h2);
            result = RC::CheckCollisionAabbAabb(min1, max1, min2, max2);
        } else if (col1->shape == ColliderComponent::Shape::Sphere && col2->shape == ColliderComponent::Shape::AABB) {
            float r1 = col1->radius * (std::max)((std::max)(std::abs(tr1->scale.x), std::abs(tr1->scale.y)), std::abs(tr1->scale.z));
            RC::Vector3 h2 = { std::abs(col2->size.x * tr2->scale.x * 0.5f), std::abs(col2->size.y * tr2->scale.y * 0.5f), std::abs(col2->size.z * tr2->scale.z * 0.5f) };
            RC::Vector3 min2 = RC::Sub(center2, h2); RC::Vector3 max2 = RC::Add(center2, h2);
            result = RC::CheckCollisionSphereAabb(center1, r1, min2, max2);
        } else if (col1->shape == ColliderComponent::Shape::AABB && col2->shape == ColliderComponent::Shape::Sphere) {
            float r2 = col2->radius * (std::max)((std::max)(std::abs(tr2->scale.x), std::abs(tr2->scale.y)), std::abs(tr2->scale.z));
            RC::Vector3 h1 = { std::abs(col1->size.x * tr1->scale.x * 0.5f), std::abs(col1->size.y * tr1->scale.y * 0.5f), std::abs(col1->size.z * tr1->scale.z * 0.5f) };
            RC::Vector3 min1 = RC::Sub(center1, h1); RC::Vector3 max1 = RC::Add(center1, h1);
            result = RC::CheckCollisionSphereAabb(center2, r2, min1, max1);
            reverseNormal = true; // normal is from sphere to AABB, so from 2 to 1.
        }

        if (result.hit) {
            // コールバック呼び出し
            if (nsc1) {
                for (auto& entry : nsc1->scripts) {
                    if (entry.instance) entry.instance->OnCollision(e2.get(), result.contactPoint);
                }
            }
            if (nsc2) {
                for (auto& entry : nsc2->scripts) {
                    if (entry.instance) entry.instance->OnCollision(e1.get(), result.contactPoint);
                }
            }

            // 衝突位置のデバッグ描画
            if (showColliderGizmos_) {
                RC::DrawSphereRings3D(result.contactPoint, 0.5f, {1.0f, 0.0f, 0.0f, 1.0f}, 8, false);
            }

            // Triggerの場合は物理的な押し出しを行わない
            if (col1->isTrigger || col2->isTrigger) continue;

            // 両方とも動かない場合は物理解決をスキップ
            if (!isDynamic1 && !isDynamic2) continue;

            // result.normal direction is from 1 to 2
            RC::Vector3 normal = reverseNormal ? RC::Mul(result.normal, -1.0f) : result.normal;

            // Calculate push ratios
            float m1 = rb1 ? rb1->mass : 1.0f;
            float m2 = rb2 ? rb2->mass : 1.0f;
            float ratio1 = 0.0f;
            float ratio2 = 0.0f;

            if (isDynamic1 && isDynamic2) {
                if (m1 + m2 > 0.0f) {
                    ratio1 = m2 / (m1 + m2);
                    ratio2 = m1 / (m1 + m2);
                } else {
                    ratio1 = 0.5f; ratio2 = 0.5f;
                }
            } else if (isDynamic1) {
                ratio1 = 1.0f; ratio2 = 0.0f;
            } else if (isDynamic2) {
                ratio1 = 0.0f; ratio2 = 1.0f;
            }

            RC::Vector3 push1 = RC::Mul(normal, -result.depth * ratio1);
            RC::Vector3 push2 = RC::Mul(normal, result.depth * ratio2);

            if (isDynamic1) {
                tr1->position = RC::Add(tr1->position, push1);
                // Cancel velocity along normal
                float vDotN = RC::Dot(rb1->velocity, normal);
                if (vDotN > 0.0f) { // e1 is moving towards e2
                    RC::Vector3 vNormal = RC::Mul(normal, vDotN);
                    rb1->velocity = RC::Sub(rb1->velocity, vNormal);
                }
            }
            if (isDynamic2) {
                tr2->position = RC::Add(tr2->position, push2);
                // Cancel velocity along normal
                float vDotN = RC::Dot(rb2->velocity, normal);
                if (vDotN < 0.0f) { // e2 is moving towards e1 (normal is from 1 to 2)
                    RC::Vector3 vNormal = RC::Mul(normal, vDotN);
                    rb2->velocity = RC::Sub(rb2->velocity, vNormal);
                }
            }
        }
    }
}
