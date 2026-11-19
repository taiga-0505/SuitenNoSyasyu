#pragma once
#include "DamageableScript.h"
#include "ECS/ScriptRegistry.h"
#include "ECS/TransformComponent.h"
#include "Scene.h"
#include "ECS/ModelRendererComponent.h"
#include "ECS/AnimationComponent.h"
#include "ECS/PrimitiveMeshComponent.h"
#include "ECS/GPUParticleComponent.h"
#include "ECS/NativeScriptComponent.h"
#include "Particle/GPUParticle.h"
#include "RenderCommon.h"
#include <format>
#include "Common/Log/Log.h"

#if RC_ENABLE_IMGUI
#include "imgui/imgui.h"
#endif

#include "Engine/Camera/CameraMath.h"
#include "ECS/CameraComponent.h"
#include "ECS/ColliderComponent.h"
#include "Engine/Render/RenderContext.h"
#include "Game/Framework/GameEvents.h"

/// @brief 敵の共通基底クラス
/// @details 体力・被弾フラッシュ・撃破後の消滅は DamageableScript が受け持ち、
///          このクラスは「敵として撃破されたときの共通処理」だけを足す。
///            - 撃破イベント（GameEvents::EnemyDefeated）の発行（撃破数の集計は購読側の仕事）
///            - "enemy_defeated" タグでシーン側に通知、当たり判定の無効化
///            - 泡のパーティクル（水中・水面の敵向け）
///          敵ごとの固有の撃破演出は OnEnemyDefeated() を上書きして足す。
///
///          メンバ変数はすべて private にし、派生クラスには必要な操作だけを関数で公開する。
///          以前は hp / isDead などが public で、どこからでも書き換えられた。
class EnemyBaseScript : public DamageableScript {
public:
  /// @brief 基底の HP 表示に加え、撃破演出の長さをインスペクタに出す
  void OnImGui() override {
    DamageableScript::OnImGui();
#if RC_ENABLE_IMGUI
    float duration = GetDefeatDuration();
    if (ImGui::DragFloat("Death Duration##EnemyBase", &duration, 0.05f, 0.1f, 20.0f)) {
      SetDefeatDuration(duration);
    }
#endif
  }

protected:
  /// @brief 体力と撃破演出時間を JSON へ書き出す
  /// @details 派生クラスは自分の Serialize の先頭でこれを呼び、返ってきた json に自分の項目を足していく。
  /// @return hp / maxHp / deathDuration を格納した json
  nlohmann::json Serialize() override {
    nlohmann::json j = DamageableScript::Serialize();
    j["deathDuration"] = GetDefeatDuration();
    return j;
  }

  /// @brief JSON から体力と撃破演出時間を読み込む
  /// @param j 読み込み元の json（hp / maxHp / deathDuration は任意）
  void Deserialize(const nlohmann::json& j) override {
    DamageableScript::Deserialize(j);
    if (j.contains("deathDuration")) {
      SetDefeatDuration(j["deathDuration"].get<float>());
      deathDurationFromData_ = true;
    }
  }

  /// @brief 敵タグを付け、コライダーが無ければ既定の球コライダーを追加する
  void OnCreate() override {
    if (Entity* self = GetEntity()) {
      self->SetTag("is_enemy", 1);

      // コライダーがなければフェールセーフとして追加
      if (!self->HasComponent<ColliderComponent>()) {
        auto* col = &self->AddComponent<ColliderComponent>();
        col->shape = ColliderComponent::Shape::Sphere;
        col->radius = 1.0f; // スケール依存
        col->isTrigger = false;
      }
    }
  }

  /// @brief データで指定されていなければ撃破演出の長さを既定値にする
  /// @param seconds 敵の種類ごとの既定秒数
  /// @details 沈む・崩れるといった演出の長さがタイプごとに違うため可変にした。
  ///          JSON / スポナーで指定があればそちらを尊重する。
  void SetDefaultDeathDuration(float seconds) {
    if (!deathDurationFromData_) SetDefeatDuration(seconds);
  }

  /// @brief 撃破時に泡のパーティクルを出すかを設定する
  /// @param enabled 出すなら true
  /// @details 泡は水中・水面の敵向けの演出。空を飛ぶ鳥では不自然なので false にして自前の演出に差し替える。
  void SetSpawnDeathBubbles(bool enabled) { spawnDeathBubbles_ = enabled; }

  /// @brief 敵ごとの固有の撃破演出（撃破の共通処理のあとに呼ばれる）
  virtual void OnEnemyDefeated() {}

  /// @brief モデル（ModelRendererComponent）で描かれているか
  /// @return スポナーの modelPath が指定されていて、モデルで描画しているなら true
  /// @details false ならプリミティブで代用している（modelPath が空）。
  bool IsModelVisual() const {
    Entity* self = GetEntity();
    return self && self->GetComponent<ModelRendererComponent>() != nullptr;
  }

  /// @brief モデルのアニメーションを用意する（AnimationComponent を付けてクリップを登録する）
  /// @param clips 登録するクリップ（index は .glb 内のアニメーションの並び順）
  /// @param defaultClip 最初に再生し、ワンショットのあとに戻るクリップ名
  /// @return 用意した AnimationComponent。プリミティブで代用しているときは nullptr
  /// @details 既にクリップが登録されていれば（シーンに保存されたものなど）そのまま使う。
  AnimationComponent* SetupModelClips(const std::vector<AnimationClip>& clips, const std::string& defaultClip) {
    Entity* self = GetEntity();
    if (!self || !self->GetComponent<ModelRendererComponent>()) return nullptr;
    auto* anim = self->GetComponent<AnimationComponent>();
    if (!anim) anim = &self->AddComponent<AnimationComponent>();
    if (anim->clips.empty()) {
      anim->clips = clips;
      anim->defaultClip = defaultClip;
      anim->MarkDirty();
    }
    return anim;
  }

  /// @brief モデルのアニメーション（無ければ nullptr）
  /// @return 自身の AnimationComponent
  AnimationComponent* ModelAnimation() const {
    Entity* self = GetEntity();
    return self ? self->GetComponent<AnimationComponent>() : nullptr;
  }

private:
  /// @brief 撃破時の共通処理。最後に OnEnemyDefeated() で派生クラス固有の演出を足す
  /// @details 撃破数の集計はここでは行わず、イベントを発行するだけにする（Observer）。
  void OnDefeated() final {
    Entity* self = GetEntity();
    if (!self) return;

    // 撃破を購読者（RailShooterGameMode など）へ通知する
    GameEvents::EnemyDefeated().Emit(EnemyDefeatedInfo{self->GetName(), GetMaxHp()});

    // タグでシーン側に撃破を通知
    self->SetTag("enemy_defeated", 1);

    // 撃破後は弾が当たらないようにする
    self->ClearTag("is_enemy");
    if (auto* col = self->GetComponent<ColliderComponent>()) {
      col->SetEnabled(false);
    }

    if (spawnDeathBubbles_) {
      SpawnDeathBubbles(*self);
    }

    OnEnemyDefeated();
  }

  /// @brief 撃破位置に GPU 泡パーティクルのエミッターを生成する
  /// @param self 撃破された自身のエンティティ
  void SpawnDeathBubbles(Entity& self) {
    Scene* scene = GetScene();
    if (!scene) return;
    auto emitter = scene->CreateEntity("GPUBubbleEmitter");
    auto* tr = &emitter->AddComponent<TransformComponent>();
    if (auto* myTr = self.GetComponent<TransformComponent>()) {
      tr->position = myTr->position;
    }
    auto* gpu = &emitter->AddComponent<GPUParticleComponent>();
    if (gpu->particleSystem) {
      gpu->particleSystem->SetPipelinePrefix("gpu_particle_bubble");
    }

    auto* nsc = &emitter->AddComponent<NativeScriptComponent>();
    nsc->AddScript("GPUBubbleEmitter");
    nsc->SetScene(scene);
    if (GetSceneContext()) nsc->SetSceneContext(GetSceneContext());

    scene->InitDynamicEntityRuntime(*emitter);
  }

  bool deathDurationFromData_ = false; ///< deathDuration が JSON / スポナーから与えられたか
  bool spawnDeathBubbles_ = true;      ///< 撃破時に泡のパーティクルを出すか
};
