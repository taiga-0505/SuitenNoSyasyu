#pragma once
#include "Scene.h"
#include "SceneFlow.h"
#include "Common/EngineConfig.h"
#include "Common/FrameProfiler.h"
#include "Common/Math/MathUtils.h"
#include "Common/Math/Math.h"
#include "Camera/CameraController.h" // ctx.camera->GetWorldPos()（Scene.h は前方宣言のみ）
#include "RenderCommon.h"
#include <string>
#include <fstream>
#include <filesystem>
#include <nlohmann/json.hpp>
#include "Common/Log/Log.h"
#include "Input/Input.h"
#include "ECS/LevelLoader.h"

// All component headers (for factory registration)
#include "ECS/TransformComponent.h"
#include "ECS/ModelRendererComponent.h"
#include "ECS/SpriteRendererComponent.h"
#include "ECS/TextRendererComponent.h"
#include "ECS/CameraComponent.h"
#include "ECS/LightComponent.h"
#include "ECS/ColliderComponent.h"
#include "ECS/AnimationComponent.h"
#include "ECS/BoneAttachmentComponent.h"
#include "ECS/PrimitiveMeshComponent.h"
#include "ECS/TextMeshComponent.h"
#include "ECS/SkyboxComponent.h"
#include "ECS/SkydomeComponent.h"
#include "ECS/WaterComponent.h"
#include "ECS/RigidbodyComponent.h"
#include "ECS/NativeScriptComponent.h"
#include "ECS/AudioSourceComponent.h"
#include "ECS/AudioListenerComponent.h"
// Light sources for Dereferencing
#include "Graphics/Light/Directional/DirectionalLightSource.h"
#include "Graphics/Light/Point/PointLightSource.h"
#include "Graphics/Light/Spot/SpotLightSource.h"
#include "Graphics/Light/Area/AreaLightSource.h"
#include "ECS/GPUParticleComponent.h"

/// @class DataDrivenScene
/// @brief Generic Scene class that builds scenes from JSON files.
/// @details Supports creation, saving, loading, and deletion from the editor.
class DataDrivenScene : public Scene {
public:
  /// @brief Constructor
  /// @param name Scene name (display name / transition key)
  /// @param filePath JSON file path
  DataDrivenScene(const std::string& name, const std::string& filePath)
    : sceneName_(name), filePath_(filePath) {
    static bool factoryInitialized = false;
    if (!factoryInitialized) {
      RegisterAllComponents();
      factoryInitialized = true;
    }
  }

  const char* Name() const override { return sceneName_.c_str(); }

  /// @brief 静的コライダーエンティティ（マップの壁・穴など。レンダラーやライトを持たない）か判定
  static bool IsStaticMapCollider(const Entity& e) {
      return e.HasTag("is_wall") || e.HasTag("is_hole");
  }

  /// @brief Load entities from JSON on scene enter
  void OnEnter(SceneContext& ctx) override {
    Load();
    resultTriggered_ = false;
    resultChangeRequested_ = false;
    resultDelayTimer_ = 0.0f;
    waterTime_ = 0.0f;

    // GameMode / GameState をリトライ前提の初期状態へ戻す。
    // Load() が GameMode を作り直すため通常はこれで二重に初期化される形になるが、
    // JSON の読み込みに失敗した場合や、派生シーンが独自の GameMode を差し替えている
    // 場合にも、スコアと経過時間が前回のプレイのまま残らないことを保証する。
    if (gameMode_) {
        gameMode_->ResetForRestart();
        // 「1 プレイの開始」などシーン入場時のゲーム固有処理は GameMode 側に任せる
        gameMode_->OnSceneEnter(*this, ctx);
    }

    // Initialize runtime handles for all loaded components
    for (auto& e : entities_) {
        InitializeRuntimeResources(*e, ctx);
    }

    // 新シーンのクリップをロードし終えた後に、どのシーンからも参照されなくなった
    // 音声クリップを解放する。前のシーンと共有している BGM は参照が残るので消えない。
    AudioEngine::Get().PurgeUnusedClips();

    // シーンに入った時点でメインカメラを反映しておく。
    // 通常は入場直後に Update が回るので同じ結果になるが、Update を挟まずに
    // Render される経路（起動直後の ChangeImmediately など）で前のカメラ姿勢が
    // 1 フレーム見えるのを防ぐ。
    SyncMainCamera(ctx);
  }

  /// @brief Clear entities on scene exit
  void OnExit(SceneContext&) override {
    // 更新ループ中に生成されて未マージのエンティティも解放対象に含める。
    // ここで取り込まずに捨てると、生成済みのランタイムハンドルが回収されない。
    FlushPendingEntities();

    for (auto& e : entities_) {
        NotifyScriptsDestroy(*e);
    }
    for (auto& e : entities_) {
        ReleaseRuntimeResources(*e);
    }
    entities_.clear();
    pendingEntities_.clear();
    broadphase_.Clear(); // 弱参照だけになった登録を捨てる
    RC::ClearEffectParticles();
  }

  void Update(SceneManager& sm, SceneContext& ctx) override {
    (void)sm;
    // 内訳表示用。下の UpdateEntities / ResolveCollisions を除いた残りが、
    // アニメーション・Transform 同期・ライト・パーティクル更新などのエンティティ走査の時間
    CHASO_PROFILE_SCOPE("Scene: DataDrivenScene::Update total");
    currentContext_ = &ctx;

#if RC_ENABLE_IMGUI
    // デバッグキーは Debug / Development ビルドだけ（配布用 Release では効かない）
    // F3 キーでコライダーデバッグ描画をトグル
    if (ctx.input && ctx.input->IsKeyTrigger(DIK_F3)) {
        showColliderGizmos_ = !showColliderGizmos_;
    }
    // F4 キーですべてのデバッグ描画をトグル
    if (ctx.input && ctx.input->IsKeyTrigger(DIK_F4)) {
        showAllGizmos_ = !showAllGizmos_;
    }
#endif

    // NativeScriptComponent に Scene/Context 参照を設定
    for (auto& e : entities_) {
        if (auto* nsc = e->GetComponent<NativeScriptComponent>()) {
            nsc->SetScene(this);
            nsc->SetSceneContext(&ctx);
        }
    }

    // === AnimationComponent の更新（Transform同期より前に実行）===
    for (auto& e : entities_) {
      if (!e->IsVisible() || IsStaticMapCollider(*e)) continue;
      auto* ren = e->GetComponent<ModelRendererComponent>();
      auto* anim = e->GetComponent<AnimationComponent>();
      if (ren && anim && ren->HasModel() && RC::IsModelReady(ren->modelHandle)) {
        // クリップを登録しているのに何も再生していなければ、既定クリップから始める
        if (!anim->clips.empty() && anim->currentClip.empty() &&
            !anim->defaultClip.empty() && !anim->pendingRequested_) {
          anim->PlayClip(anim->defaultClip, 0.0f);
        }

        if (anim->pendingRequested_) {
          // --- 名前指定によるクリップ切り替え（スクリプトからの PlayClip）---
          ApplyPendingClip(*anim, *ren);
        } else if (anim->NeedsReattach()) {
          // --- パス / クリップ番号の直接指定（Inspector での変更）---
          if (anim->animationPath.empty() && anim->animIndex <= 0) {
            // モデル内蔵の先頭アニメーション
            RC::AttachModelAnimation(ren->modelHandle);
          } else {
            // 外部ファイル、または内蔵の 2 本目以降（glTF の複数クリップ）
            const std::string& srcPath =
                anim->animationPath.empty() ? ren->modelPath : anim->animationPath;
            if (!srcPath.empty()) {
              RC::AttachModelAnimation(ren->modelHandle, srcPath, anim->animIndex);
            }
          }
          anim->MarkAttached();
        }

        // クリップ固有の速度・ループ設定を反映する
        float clipSpeed = 1.0f;
        bool clipLoop = true;
        if (const AnimationClip* cur = anim->FindClip(anim->currentClip)) {
          clipSpeed = cur->speed;
          clipLoop = cur->loop;
        }

        const float dt = (ctx.isSimulating() && anim->playing)
            ? ctx.deltaTime * anim->speed * clipSpeed : 0.0f;
        RC::UpdateModelAnimation(ren->modelHandle, dt);

        // ワンショット（loop = false）の終了判定。
        // エンジン側の再生は常にループする（ModelObject.cpp の fmod）ため、
        // 尺を超えた時点でこちらから既定クリップへ戻す。
        if (dt > 0.0f && !clipLoop) {
          anim->clipElapsed_ += dt;
          const float duration = RC::GetModelAnimationDuration(ren->modelHandle);
          if (duration > 0.0f && anim->clipElapsed_ >= duration) {
            if (!anim->defaultClip.empty() && anim->defaultClip != anim->currentClip) {
              anim->PlayClip(anim->defaultClip);
            } else {
              anim->playing = false; // 戻り先が無い場合はその場で停止
            }
          }
        }
      }
    }

    // TransformComponent の内容を各種対象に同期
    for (auto& e : entities_) {
        if (IsStaticMapCollider(*e)) continue;
        if (auto* tr = e->GetComponent<TransformComponent>()) {
            if (auto* ren = e->GetComponent<ModelRendererComponent>()) {
                if (ren->HasModel() && RC::IsModelReady(ren->modelHandle)) {
                    if (auto* modelTr = RC::GetModelTransformPtr(ren->modelHandle)) {
                        auto* anim = e->GetComponent<AnimationComponent>();
                        if (anim && anim->attached_ && !RC::HasModelSkinData(ren->modelHandle) && !RC::HasModelSkeleton(ren->modelHandle) && !e->GetComponent<NativeScriptComponent>()) {
                            // Node animation case: animation controls the root transform.
                            // Read back from modelTr to TransformComponent to keep them in sync,
                            // and avoid overwriting the animation frame.
                            tr->SetFromTransform(*modelTr);
                        } else {
                            // Skeletal animation or no animation: TransformComponent controls root transform.
                            *modelTr = tr->ToTransform();
                        }
                    }
                    // ライティングモードの個別設定 (-1 は DirectionalLight に追従)
                    if (ren->lightingMode >= 0) {
                        RC::SetModelLightingMode(ren->modelHandle, static_cast<LightingMode>(ren->lightingMode));
                    }
                    RC::SetModelColor(ren->modelHandle, ren->color);
                    RC::SetModelEnvironmentCoefficient(ren->modelHandle, ren->environmentCoeff);
                    RC::SetModelNormalMap(ren->modelHandle, ren->normalMapOverride);
                    RC::SetModelRoughnessMap(ren->modelHandle, ren->roughnessMapOverride);
                }
            }
            if (auto* skybox = e->GetComponent<SkyboxComponent>()) {
                if (skybox->HasSkybox()) {
                    if (auto* sTr = RC::GetSkyBoxTransformPtr(skybox->skyboxHandle)) {
                        *sTr = tr->ToTransform();
                    }
                    RC::SetSkyBoxColor(skybox->skyboxHandle, skybox->color);
                }
            }
            if (auto* dirLight = e->GetComponent<DirectionalLightComponent>()) {
                if (dirLight->lightHandle == -1) {
                    dirLight->lightHandle = RC::CreateDirectionalLight(RC::LightActivateMode::Add);
                }
                if (auto* l = RC::GetDirectionalLightPtr(dirLight->lightHandle)) {
                    l->SetColor(dirLight->color);
                    l->SetDirection(dirLight->direction);
                    l->SetIntensity(dirLight->intensity);
                    l->SetAmbient(dirLight->ambientColor, dirLight->ambientIntensity);
                    RC::SetDirectionalLightEnabled(dirLight->lightHandle, e->IsVisible() && dirLight->visible && dirLight->IsEnabled());
                }
            }
            if (auto* ptLight = e->GetComponent<PointLightComponent>()) {
                if (ptLight->lightHandle == -1) {
                    ptLight->lightHandle = RC::CreatePointLight(RC::LightActivateMode::Add);
                }
                if (auto* l = RC::GetPointLightPtr(ptLight->lightHandle)) {
                    l->SetColor(ptLight->color);
                    l->SetPosition(tr->position);
                    l->SetIntensity(ptLight->intensity);
                    l->SetRadius(ptLight->radius);
                    l->SetDecay(ptLight->decay);
                    l->SetCastShadow(ptLight->castShadow);
                    l->SetShadowNear(ptLight->shadowNear);
                    l->SetShadowExcludeOwnerId(ptLight->shadowExcludeSelf ? e->GetId() : 0u);
                    l->SetShadowPriority(ptLight->shadowPriority);
                    RC::SetPointLightEnabled(ptLight->lightHandle, e->IsVisible() && ptLight->visible && ptLight->IsEnabled());
                }
            }
            if (auto* spLight = e->GetComponent<SpotLightComponent>()) {
                if (spLight->lightHandle == -1) {
                    spLight->lightHandle = RC::CreateSpotLight(RC::LightActivateMode::Add);
                }
                if (auto* l = RC::GetSpotLightPtr(spLight->lightHandle)) {
                    l->SetColor(spLight->color);
                    l->SetPosition(spLight->ResolvePosition(tr->position, tr->rotation));
                    l->SetDirection(spLight->direction);
                    l->SetIntensity(spLight->intensity);
                    l->SetDistance(spLight->distance);
                    l->SetDecay(spLight->decay);
                    l->SetCosAngle(spLight->cosAngle);
                    l->SetCastShadow(spLight->castShadow);
                    l->SetShadowNear(spLight->shadowNear);
                    l->SetShadowExcludeOwnerId(spLight->shadowExcludeSelf ? e->GetId() : 0u);
                    l->SetShadowPriority(spLight->shadowPriority);
                    RC::SetSpotLightEnabled(spLight->lightHandle, e->IsVisible() && spLight->visible && spLight->IsEnabled());
                }
            }
            if (auto* arLight = e->GetComponent<AreaLightComponent>()) {
                if (arLight->lightHandle == -1) {
                    arLight->lightHandle = RC::CreateAreaLight(RC::LightActivateMode::Add);
                }
                if (auto* l = RC::GetAreaLightPtr(arLight->lightHandle)) {
                    l->SetColor(arLight->color);
                    l->SetPosition(tr->position);
                    l->SetIntensity(arLight->intensity);
                    l->SetRange(arLight->range);
                    l->SetDecay(arLight->decay);
                    l->SetHalfSize(arLight->halfWidth, arLight->halfHeight);
                    l->SetBasis(arLight->right, arLight->up);
                    l->SetTwoSided(arLight->twoSided);
                    l->SetCastShadow(arLight->castShadow);
                    l->SetShadowNear(arLight->shadowNear);
                    l->SetShadowExcludeOwnerId(arLight->shadowExcludeSelf ? e->GetId() : 0u);
                    l->SetShadowPriority(arLight->shadowPriority);
                    RC::SetAreaLightEnabled(arLight->lightHandle, e->IsVisible() && arLight->visible && arLight->IsEnabled());
                }
            }
            if (auto* skydome = e->GetComponent<SkydomeComponent>()) {
                if (skydome->HasSkydome()) {
                    if (auto* sTr = RC::GetSkydomeTransformPtr(skydome->skydomeHandle)) {
                        *sTr = tr->ToTransform();
                    }
                    // 乗算カラーを毎フレーム同期する。
                    // Skydome 側のマテリアルは生成時に白で初期化されるため、
                    // ここで押し込まないとシーンから復元した色（特に黒）が反映されない。
                    RC::SetSkydomeColor(skydome->skydomeHandle, skydome->color);
                }
            }
            if (auto* pm = e->GetComponent<PrimitiveMeshComponent>()) {
                if (pm->HasMesh()) {
                    if (auto* pmTr = RC::GetPrimitiveMeshTransformPtr(pm->meshHandle)) {
                        *pmTr = tr->ToTransform();
                    }
                    // ライティングモードの個別設定 (-1 は DirectionalLight に追従)
                    if (pm->lightingMode >= 0) {
                        RC::SetPrimitiveMeshLightingMode(pm->meshHandle, static_cast<LightingMode>(pm->lightingMode));
                    }
                    RC::SetPrimitiveMeshEnvironmentCoefficient(pm->meshHandle, pm->environmentCoeff);
                    RC::SetPrimitiveMeshNormalMap(pm->meshHandle, pm->normalMapOverride);
                    RC::SetPrimitiveMeshRoughnessMap(pm->meshHandle, pm->roughnessMapOverride);
                }
            }
            if (auto* tm = e->GetComponent<TextMeshComponent>()) {
                // 文字列・厚さ・フォント等が変わっていればメッシュを再生成（エディタ編集にも追従）
                EnsureTextMesh(*tm);
                if (tm->HasMesh()) {
                    if (auto* tmTr = RC::GetPrimitiveMeshTransformPtr(tm->meshHandle)) {
                        *tmTr = tr->ToTransform();
                    }
                    if (tm->lightingMode >= 0) {
                        RC::SetPrimitiveMeshLightingMode(tm->meshHandle, static_cast<LightingMode>(tm->lightingMode));
                    }
                    RC::SetPrimitiveMeshEnvironmentCoefficient(tm->meshHandle, tm->environmentCoeff);
                    RC::SetPrimitiveMeshNormalMap(tm->meshHandle, tm->normalMapOverride);
                    RC::SetPrimitiveMeshRoughnessMap(tm->meshHandle, tm->roughnessMapOverride);
                }
                if (tm->HasOutlineMesh()) {
                    if (auto* olTr = RC::GetPrimitiveMeshTransformPtr(tm->outlineMeshHandle)) {
                        *olTr = tr->ToTransform();
                    }
                    ApplyTextMeshOutlineMaterial(*tm); // 色・ライティングの変更を即時反映
                }
            }
            if (auto* water = e->GetComponent<WaterComponent>()) {
                if (water->HasMesh()) {
                    if (auto* wTr = RC::GetWaterTransformPtr(water->meshHandle)) {
                        *wTr = tr->ToTransform();
                    }
                    RC::SetWaterEnvironmentCoefficient(water->meshHandle, water->environmentCoeff);
                    RC::SetWaterCrestTint(water->crestTint);
                    RC::SetWaterOceanParams(
                        water->detailStrength, water->choppiness, water->MinDisplacedWavelength(),
                        water->whitecapStrength, water->whitecapCoverage,
                        water->sssColor, water->clarity,
                        water->detailFadeDistance, water->normalTileSize);
                    RC::SetWaterRefraction(water->refraction, water->refractionStrength,
                                           water->edgeFade);
                    RC::SetWaterParams(
                        water->waveHeight, water->waveSpeed, water->waveFreq,
                        water->waveHeight2, water->waveSpeed2, water->waveFreq2,
                        water->waveSteepness,
                        water->shallowColor, water->deepColor,
                        water->fresnelPower, water->specularPower,
                        water->normalScrollSpeed, water->normalStrength);
                }
            }
            if (auto* spr = e->GetComponent<SpriteRendererComponent>()) {
                if (spr->HasSprite()) {
                    // モード切替に追従（World では Size ではなく scale が大きさになる）
                    RC::SetSpriteWorldSpace(spr->spriteHandle, spr->IsWorldSpace());
                    RC::SetSpriteTransform(spr->spriteHandle, tr->ToTransform());
                    if (!spr->IsWorldSpace()) {
                        RC::SetSpriteScreenSize(spr->spriteHandle, spr->size.x, spr->size.y);
                    }
                    RC::SetSpriteColor(spr->spriteHandle, spr->color);
                }
            }
        }
    }

    // 水面の累積時間を更新
    waterTime_ += ctx.deltaTime;
    RC::SetWaterTime(waterTime_);

    // GPUParticle の更新
    for (auto& e : entities_) {
        if (!e->IsVisible() || !e->IsActive() || IsStaticMapCollider(*e)) continue;
        if (auto* gpu = e->GetComponent<GPUParticleComponent>()) {
            if (gpu->isInitialized && gpu->particleSystem) {
                if (auto* tr = e->GetComponent<TransformComponent>()) {
                    gpu->particleSystem->emitterPosition_ = tr->position;
                }
                if (ctx.camera) {
                    // 編集モードでもパーティクルは動かして見せるが、ゲーム内ポーズ中は止める
                    const float particleDt = ctx.gamePaused ? 0.0f : ctx.deltaTime;
                    gpu->particleSystem->Update(ctx.camera->GetView(), ctx.camera->GetProjection(), particleDt);
                }
            }
        }
    }

    // ゲーム内ポーズ中は 0 で回す（停止・一時停止と同じ扱い）。
    // ポーズメニューのスクリプトだけは ctx.deltaTime を直接読んで動く。
    float updateDt = ctx.isSimulating() ? ctx.deltaTime : 0.0f;

    if (ctx.isSimulating()) {
        if (gameMode_ && !gameMode_->HasBegunPlay()) {
            gameMode_->BeginPlay(ctx);
            gameMode_->MarkBegunPlay();
        }
        if (gameMode_) {
            gameMode_->Tick(ctx);
        }
    }

    {
      CHASO_PROFILE_SCOPE("Scene: UpdateEntities (all components)");
      UpdateEntities(updateDt);
    }
    {
      // 水しぶき・泡などのエフェクト粒（Entity を使わない）。停止・ポーズ中は updateDt = 0 で止まる
      CHASO_PROFILE_SCOPE("Scene: UpdateEffectParticles");
      RC::UpdateEffectParticles(updateDt);
    }
    {
      CHASO_PROFILE_SCOPE("Scene: ResolveCollisions");
      ResolveCollisions();
    }

    // ※ AudioSource の更新（playOnAwake / BGM keep-alive）はここではなく UpdateAudio() で行う。
    //    SceneManager が演出中も含めて毎フレーム呼ぶため、Update が止まっても BGM が切れない。

    // スクリプト更新および物理衝突解決後の最新座標・回転をモデルへ同期
    for (auto& e : entities_) {
        if (IsStaticMapCollider(*e)) continue;
        if (auto* tr = e->GetComponent<TransformComponent>()) {
            if (auto* ren = e->GetComponent<ModelRendererComponent>()) {
                if (ren->HasModel()) {
                    if (auto* modelTr = RC::GetModelTransformPtr(ren->modelHandle)) {
                        auto* anim = e->GetComponent<AnimationComponent>();
                        if (anim && anim->attached_ && !RC::HasModelSkinData(ren->modelHandle) && !RC::HasModelSkeleton(ren->modelHandle) && !e->GetComponent<NativeScriptComponent>()) {
                            tr->SetFromTransform(*modelTr);
                        } else {
                            *modelTr = tr->ToTransform();
                        }
                    }
                }
            }
        }
    }

    // === BoneAttachmentComponent（ボーン追従／ソケット）===
    // アニメーション更新と Transform 同期の後に実行する必要がある。
    // ここより前だと Joint 姿勢が 1 フレーム古くなり、剣が手から遅れて付いてくる。
    UpdateBoneAttachments();

    // === ゲーム結果判定（プレイ中のみ） ===
    // 何をもって決着とするか（死亡・クリア条件、結果の記録）はゲームごとに違うので
    // GameMode::EvaluateOutcome に任せる。エンジンは決着後の余韻と遷移だけを受け持つ。
    // GameMode が返すのはきっかけ名（"cleared" など）。行き先と演出は遷移表（SceneFlow）で引く。
    if (ctx.isSimulating() && !resultTriggered_ && gameMode_) {
        std::string trigger;
        if (gameMode_->EvaluateOutcome(*this, ctx, trigger)) {
            resultTriggered_ = true;
            resultDelayTimer_ = 0.0f;
            resultTarget_.clear();
            resultTransition_ = SceneTransition::Dissolve;
            SceneFlowResult flow;
            if (!trigger.empty() && SceneFlow::Get().Resolve(sceneName_, trigger, "", flow)) {
                resultTarget_ = flow.target;
                resultTransition_ = ParseSceneTransition(flow.transition);
            }
        }
    }

    // デッドエンティティの削除は判定後に行う（タグを読み取れるようにするため）
    RemoveDeadEntities();

    // シーン遷移ディレイ処理
    if (resultTriggered_ && ctx.isSimulating()) {
        resultDelayTimer_ += ctx.deltaTime;
        if (!resultChangeRequested_ && resultDelayTimer_ >= kResultDelay_) {
            // スクリプトが自前の演出で遷移する（例: DeathSinkScript が沈みきってから "dive" で
            // Result へ送る）ときは、ここからは要求しない。目印はエンティティの
            // "result_transition_owner" タグ。演出側が失敗したときは演出側が素の遷移へ倒す。
            bool ownedByScript = false;
            for (auto& e : entities_) {
                if (e && e->GetTagInt("result_transition_owner", 0) == 1) { ownedByScript = true; break; }
            }
            if (!ownedByScript && !resultTarget_.empty()) sm.RequestChange(resultTarget_, resultTransition_);
            resultChangeRequested_ = true; // 遷移要求を一度だけ送信（決着演出はフェードアウト中も描画継続！）
        }
    }

    // === Play/Editor カメラ切り替え（F1。Debug / Development ビルドだけ） ===
#if RC_ENABLE_IMGUI
    if (ctx.isPlaying() && ctx.input && ctx.camera && ctx.input->IsKeyTrigger(DIK_F1)) {
        ctx.camera->SetUseDebug(!ctx.camera->IsUsingDebug());
    }
#endif
    SyncMainCamera(ctx);
  }

  /// @brief シーン内のメインカメラを CameraController へ反映する
  /// @details Update の末尾と OnEnter の両方から呼ぶ。OnEnter で呼ぶことで、
  ///          シーン遷移直後の 1 フレームに前のシーンのカメラ位置が残るのを防ぐ。
  void SyncMainCamera(SceneContext& ctx) {
    if (!ctx.camera || !ctx.app) return;

    // A-05: レベル JSON 側のカメラを優先する。
    // レベルのカメラは "main": true を明示したときだけ isMain が立つので、
    // 1周目でレベル側を探し、見つからなければシーン JSON 側を採用する。
    // シーン側の isMain を false に書き換えて降格させる手もあるが、
    // isMain は CameraComponent::Serialize() でシーン JSON に焼き付くため、
    // 「探す順番」だけで解決してデータは書き換えない。
    Entity* target = FindMainCamera(true);
    if (!target) target = FindMainCamera(false);
    if (!target) return;

    auto* camComp = target->GetComponent<CameraComponent>();
    auto* camTr = target->GetComponent<TransformComponent>();
    if (!camComp || !camTr) return;

    const float aspect =
        (ctx.app->height > 0) ? float(ctx.app->width) / ctx.app->height : 16.0f / 9.0f;
    if (ctx.isPlaying()) {
        ctx.camera->SetMainPosition(RC::Add(camTr->position, camComp->shakeOffset));
        ctx.camera->SetMainRotation(camTr->rotation);
        ctx.camera->SetProjection(camComp->fovY, aspect, camComp->nearZ, camComp->farZ);
    } else {
        ctx.camera->SetUseDebug(true);
    }
  }

  /// @brief isMain が立っているカメラを探す
  /// @param fromLevelOnly true ならレベル JSON 由来のエンティティだけを見る
  /// @return 見つかったエンティティ。無ければ nullptr
  Entity* FindMainCamera(bool fromLevelOnly) {
    for (auto& e : entities_) {
        if (!e) continue;
        if (fromLevelOnly != e->HasTag(kLevelEntityTag)) continue;
        auto* camComp = e->GetComponent<CameraComponent>();
        auto* camTr = e->GetComponent<TransformComponent>();
        if (camComp && camTr && camComp->isMain) return e.get();
    }
    return nullptr;
  }

  void Render(SceneContext& ctx, ID3D12GraphicsCommandList* cl) override {
    // スプライト描画（space でレイヤーを振り分ける）
    // 遅延ロードも含むので、どのパスからも同じ処理を使う
    auto drawSpriteLayer = [&](SpriteSpace layer) {
        for (auto& e : entities_) {
            if (!e->IsVisible() || !e->IsActive() || IsStaticMapCollider(*e)) continue;
            auto* spr = e->GetComponent<SpriteRendererComponent>();
            if (!spr || spr->space != layer) continue;

            // エディタで後からパスを設定/変更した場合の遅延ロード（差し替え）
            if (!spr->spritePath.empty() && spr->spritePath != spr->loadedPath) {
                if (spr->HasSprite()) { RC::UnloadSprite(spr->spriteHandle); spr->spriteHandle = -1; }
                spr->loadedPath = spr->spritePath;
                spr->spriteHandle = RC::LoadSprite(spr->spritePath, ctx);
                if (spr->HasSprite()) {
                    RC::SetSpriteWorldSpace(spr->spriteHandle, spr->IsWorldSpace());
                    if (auto* tr = e->GetComponent<TransformComponent>()) {
                        RC::SetSpriteTransform(spr->spriteHandle, tr->ToTransform());
                    }
                    if (!spr->IsWorldSpace()) {
                        RC::SetSpriteScreenSize(spr->spriteHandle, spr->size.x, spr->size.y);
                    }
                    RC::SetSpriteColor(spr->spriteHandle, spr->color);
                }
            }
            if (spr->HasSprite() && spr->visible && spr->IsEnabled()) {
                if (layer == SpriteSpace::World) {
                    RC::DrawSprite3D(spr->spriteHandle);
                } else {
                    RC::DrawSprite(spr->spriteHandle);
                }
            }
        }
    };

    // ===========================================
    // 背景2D描画（常にモデルより後ろ）
    // ===========================================
    // 3D のコマンドはキューに溜まり PreDraw2D でまとめて発行されるため、
    // ここで積んだスプライトは必ず 3D より先に実行される＝モデルの奥に見える。
    // 必ず PreDraw3D より前に行うこと。
    bool hasBehindSprite = false;
    for (auto& e : entities_) {
        if (!e->IsVisible() || !e->IsActive() || IsStaticMapCollider(*e)) continue;
        if (auto* spr = e->GetComponent<SpriteRendererComponent>()) {
            if (spr->space == SpriteSpace::ScreenBehind) { hasBehindSprite = true; break; }
        }
    }
    if (hasBehindSprite) {
        RC::PreDraw2DBackground(ctx, cl);
        drawSpriteLayer(SpriteSpace::ScreenBehind);
    }

    // ===========================================
    // シャドウパス
    // ===========================================
    // 1) 平行光源のシャドウマップ（シーン全体を暗くする従来の影）
    // 2) スポットライトごとの影アトラス（灯ごとに深度マップを持ち、壁の向こう側へ光が漏れない）
    //
    // 影を落とす物（影キャスター）は両方のパスで共通。同じ描画を「影を落とすライトの数」だけ繰り返す。
    // excludeEntityId: この識別子のエンティティだけ描かない（0 で全部描く）。
    //                  ライトを持つ本体が自分の光を遮って足元が真っ暗になるのを防ぐ用。
    //
    // 影キャスターの一覧は毎フレーム 1 回だけ組み立てる。
    // 以前はタイルごとに全エンティティを走査し直していた（タグの文字列ハッシュ 2 回＋
    // コンポーネント検索 3 回 × 全エンティティ × 最大 kMaxSpotShadows+1 パス）。
    // 判定結果はパスの途中で変わらないので、一度集めた配列を各パスで再生するだけにする。
    struct ShadowCasterEntry {
        uint32_t entityId = 0;
        ModelRendererComponent* ren = nullptr;   ///< 描くモデル（無ければ nullptr）
        PrimitiveMeshComponent* pm = nullptr;    ///< 描くプリミティブメッシュ（無ければ nullptr）
        TextMeshComponent* tm = nullptr;         ///< 描く 3D 文字メッシュ（無ければ nullptr）
        NativeScriptComponent* nsc = nullptr;    ///< OnShadowRender を持つスクリプト群（無ければ nullptr）
    };
    static std::vector<ShadowCasterEntry> s_shadowCasters; // 毎フレーム clear して使い回す（再確保しない）
    s_shadowCasters.clear();
    for (auto& e : entities_) {
        if (!e->IsVisible() || !e->IsActive() || IsStaticMapCollider(*e)) continue;
        if (e->GetTagInt("cast_shadow", 1) == 0 || e->GetTagInt("no_shadow", 0) == 1) continue;

        ShadowCasterEntry entry;
        entry.entityId = e->GetId();

        // 影を落とすオブジェクト群（モデルとプリミティブメッシュ）
        if (auto* ren = e->GetComponent<ModelRendererComponent>()) {
            if (ren->HasModel() && ren->visible && ren->IsEnabled()) {
                entry.ren = ren;
            }
        }
        if (auto* pm = e->GetComponent<PrimitiveMeshComponent>()) {
            // 自己発光（アンリット lightingMode == 0：UI板ポリやエフェクト用）は影を落とさない
            if (pm->lightingMode != 0 && pm->HasMesh() && pm->visible && pm->IsEnabled()) {
                entry.pm = pm;
            }
        }
        if (auto* tm = e->GetComponent<TextMeshComponent>()) {
            if (tm->lightingMode != 0 && tm->HasMesh() && tm->visible && tm->IsEnabled()) {
                entry.tm = tm;
            }
        }
        // スクリプトが自前で描いている 3D ジオメトリ（マップの壁・床、ドアなど）
        if (auto* nsc = e->GetComponent<NativeScriptComponent>()) {
            if (!nsc->scripts.empty()) {
                entry.nsc = nsc;
            }
        }

        if (entry.ren || entry.pm || entry.tm || entry.nsc) {
            s_shadowCasters.push_back(entry);
        }
    }

    auto drawShadowCasters = [&](uint32_t excludeEntityId) {
        for (const auto& c : s_shadowCasters) {
            if (excludeEntityId != 0u && c.entityId == excludeEntityId) continue;

            if (c.ren) {
                // 同じメッシュ・見た目の物は自動でまとめてインスタンス描画（ライト視錐台外は描かない）
                RC::DrawModelInstanced(c.ren->modelHandle, c.ren->texOverride);
            }
            if (c.pm) {
                RC::DrawPrimitiveMesh(c.pm->meshHandle, c.pm->texOverride);
            }
            if (c.tm) {
                RC::DrawPrimitiveMesh(c.tm->meshHandle, c.tm->texOverride);
                // 縁取り分だけ太ったシルエットで影を落とす（Unlit でも形状としては存在するため）
                if (c.tm->HasOutlineMesh()) {
                    RC::DrawPrimitiveMesh(c.tm->outlineMeshHandle, -1);
                }
            }
            if (c.nsc) {
                for (auto& entry : c.nsc->scripts) {
                    if (entry.instance) {
                        entry.instance->OnShadowRender();
                    }
                }
            }
        }
        // VirtualEntity（RC::CreateModelProxy で置いた見た目だけの物）。castShadow の物だけ描かれる
        RC::DrawModelProxies();
        // エフェクト粒（水しぶき・泡・水柱）。以前は 1 粒ずつ PrimitiveMesh として影を落としていた
        RC::DrawEffectParticles();
    };

    // --- 1) 平行光源 ---
    RC::Matrix4x4 lightViewProj = MakeIdentity4x4();
    RC::Vector3 lightDir = {0, -1, 0};
    bool shadowEnabled = false;

    for (auto& e : entities_) {
        if (!e->IsVisible() || !e->IsActive()) continue;
        if (auto* dl = e->GetComponent<DirectionalLightComponent>()) {
            if (dl->IsEnabled() && dl->visible) {
                // DirectionalLight用の広範囲の正射影
                float range = 40.0f;
                RC::Matrix4x4 proj = MakeOrthographicMatrix(-range, range, range, -range, 0.1f, 100.0f);
                RC::Vector3 dir = Normalize(dl->direction);
                float pitch = std::asin(-dir.y);
                float yaw = std::atan2(dir.x, dir.z);
                RC::Vector3 rot = {pitch, yaw, 0.0f};
                RC::Vector3 pos = {-dir.x * 50.0f, -dir.y * 50.0f, -dir.z * 50.0f}; // シーンの中心から引き戻す
                RC::Matrix4x4 world = MakeAffineMatrix({1.0f, 1.0f, 1.0f}, rot, pos);
                RC::Matrix4x4 view = Inverse(world);
                lightViewProj = Multiply(view, proj);
                lightDir = dl->direction;
                shadowEnabled = true;
                break;
            }
        }
    }

    ShadowParams sParams;
    sParams.lightViewProjection = lightViewProj;
    sParams.lightDirection = lightDir;
    sParams.bias = 0.01f; // シャドウアクネを防ぐためにバイアスを少し大きめに設定
    sParams.color = {0.0f, 0.0f, 0.0f, 0.5f}; // 色と濃さ
    sParams.shadowMapEnabled = shadowEnabled ? 1 : 0;
    // PCF のタップ間隔（テクセル単位）。大きくするほど影の縁が柔らかくなるが、
    // 広げすぎると接地部分の影が薄れる。0以下にすると 1 タップ（PCF無効）になる。
    sParams.pcfRadius = 1.0f;
    // shadowMapTexelSize は RenderContext 側でシャドウマップの実解像度から設定される
    RC::UpdateShadowParams(sParams);

    // --- 2) ライトごとの影：影を落とす灯を選ぶ ---
    // 点灯中かつ castShadow なライトを集め、カメラに近い順に最大 kMaxSpotShadows 灯へ
    // アトラスのタイルを割り当てる。残りは shadowIndex = -1（従来通り影なし）。
    //
    // スポットライトは照射方向の透視投影をそのまま焼く。
    // 点光源・面光源は全方位に光るが、キューブマップを持つ代わりに
    // 「ライト位置から真下を向いた広角の透視投影」を 1 枚だけ焼く。
    // 遮蔽物（壁）が垂直なので、ある水平方向が遮られているかは高さに依らない。
    // シェーダ側（SampleOmniShadowDown）が受光点を真下へずらしてからこの 1 枚を引く。
    //
    // 走査するのはエンジンが持っているアクティブなライト一覧で、コンポーネントに限らない。
    // スクリプトが RC::CreateSpotLight() 等で直接作ったライトにも同じように影が付く。
    // 位置・向き・角度は GPU へ転送される実ライトの値をそのまま使うのでライティングとずれない。
    enum class ShadowLightKind { Spot, Point, Area };
    struct ShadowCandidate {
        ShadowLightKind kind = ShadowLightKind::Spot;
        RC::SpotLightSource* spot = nullptr;
        RC::PointLightSource* point = nullptr;
        RC::AreaLightSource* area = nullptr;
        uint32_t excludeEntityId = 0; ///< このライトの影を落とさないエンティティ（0 で無し）
        int priority = 0;             ///< 小さいほど優先してタイルを割り当てる
        RC::Vector3 position{};
        RC::Vector3 direction{};     ///< スポットのみ
        float range = 0.0f;          ///< 到達距離（far クリップ）
        float halfAngle = 0.0f;      ///< 照射半角（ラジアン）
        float shadowNear = 0.05f;
        float distToCamera = 0.0f;

        void SetShadowIndex(int index) const {
            switch (kind) {
            case ShadowLightKind::Spot:  if (spot)  spot->SetShadowIndex(index);  break;
            case ShadowLightKind::Point: if (point) point->SetShadowIndex(index); break;
            case ShadowLightKind::Area:  if (area)  area->SetShadowIndex(index);  break;
            }
        }
    };
    static std::vector<ShadowCandidate> shadowCandidates; // 毎フレーム clear して使い回す（再確保しない）
    shadowCandidates.clear();
    const RC::Vector3 camPos = ctx.camera ? ctx.camera->GetWorldPos() : RC::Vector3{0.0f, 0.0f, 0.0f};

    // 点光源・面光源が真下を照らす円錐の半角。広いほど遠くの床まで 1 枚でカバーできるが、
    // タイルの解像度が中心に寄って端が粗くなる。75 度 = 全角 150 度
    constexpr float kOmniShadowHalfAngle = 75.0f * (3.14159265358979323846f / 180.0f);
    // 影を落とすライトは「カメラからこの距離＋そのライトの到達距離」までに限る。
    // 1 灯につきシーン全体の影キャスターをもう一度描くので、画面外のライトに
    // タイルを使わせない（画面に映らないので影が無くても分からない）
    constexpr float kShadowMaxCameraDistance = 25.0f;

    // --- スポットライト ---
    const int activeSpotCount = RC::GetActiveSpotLightCount();
    for (int i = 0; i < activeSpotCount; ++i) {
        const int handle = RC::GetActiveSpotLightHandleAt(i);
        if (handle < 0) continue;
        auto* src = RC::GetSpotLightPtr(handle);
        if (!src) continue;

        const auto& d = src->Data();
        if (!src->IsEnabled() || !src->IsCastShadow() || d.intensity <= 0.0f || d.distance <= 0.0f) {
            src->SetShadowIndex(-1);
            continue;
        }

        const float distToCamera = RC::Length(RC::Sub(d.position, camPos));
        if (distToCamera - d.distance > kShadowMaxCameraDistance) {
            src->SetShadowIndex(-1);
            continue;
        }

        ShadowCandidate c;
        c.kind = ShadowLightKind::Spot;
        c.priority = src->GetShadowPriority();
        c.spot = src;
        c.excludeEntityId = src->GetShadowExcludeOwnerId();
        c.position = d.position;
        c.direction = d.direction;
        c.range = d.distance;
        // 真横まで開いた円錐は透視投影で表せないので少し内側で止める
        c.halfAngle = std::acos(std::clamp(d.cosAngle, 0.0872f, 0.9999f)); // cos(85度) 〜 ほぼ 0度
        c.shadowNear = src->GetShadowNear();
        c.distToCamera = distToCamera;
        shadowCandidates.push_back(c);
    }

    // --- 点光源 ---
    const int activePointCount = RC::GetActivePointLightCount();
    for (int i = 0; i < activePointCount; ++i) {
        const int handle = RC::GetActivePointLightHandleAt(i);
        if (handle < 0) continue;
        auto* src = RC::GetPointLightPtr(handle);
        if (!src) continue;

        const auto& d = src->Data();
        if (!src->IsEnabled() || !src->IsCastShadow() || d.intensity <= 0.0f || d.radius <= 0.0f) {
            src->SetShadowIndex(-1);
            continue;
        }

        const float distToCamera = RC::Length(RC::Sub(d.position, camPos));
        if (distToCamera - d.radius > kShadowMaxCameraDistance) {
            src->SetShadowIndex(-1);
            continue;
        }

        ShadowCandidate c;
        c.kind = ShadowLightKind::Point;
        c.priority = src->GetShadowPriority();
        c.point = src;
        c.excludeEntityId = src->GetShadowExcludeOwnerId();
        c.position = d.position;
        c.direction = {0.0f, -1.0f, 0.0f}; // 真下向き固定
        c.range = d.radius;
        c.halfAngle = kOmniShadowHalfAngle;
        c.shadowNear = src->GetShadowNear();
        c.distToCamera = distToCamera;
        shadowCandidates.push_back(c);
    }

    // --- 面光源（Tube 含む） ---
    const int activeAreaCount = RC::GetActiveAreaLightCount();
    for (int i = 0; i < activeAreaCount; ++i) {
        const int handle = RC::GetActiveAreaLightHandleAt(i);
        if (handle < 0) continue;
        auto* src = RC::GetAreaLightPtr(handle);
        if (!src) continue;

        const auto& d = src->Data();
        if (!src->IsEnabled() || !src->IsCastShadow() || d.intensity <= 0.0f || d.range <= 0.0f) {
            src->SetShadowIndex(-1);
            continue;
        }

        const float distToCamera = RC::Length(RC::Sub(d.position, camPos));
        if (distToCamera - d.range > kShadowMaxCameraDistance) {
            src->SetShadowIndex(-1);
            continue;
        }

        ShadowCandidate c;
        c.kind = ShadowLightKind::Area;
        c.priority = src->GetShadowPriority();
        c.area = src;
        c.excludeEntityId = src->GetShadowExcludeOwnerId();
        c.position = d.position;
        c.direction = {0.0f, -1.0f, 0.0f}; // 真下向き固定
        // position は線分／矩形の中心なので、端まで届くぶんを到達距離に足しておく
        // （足りないと far より遠い受光点が判定不能になり、その面光源だけ影が付かなくなる）
        const float areaExtent = (d.halfHeight <= 0.0f)
            ? d.halfWidth
            : std::sqrt(d.halfWidth * d.halfWidth + d.halfHeight * d.halfHeight);
        c.range = d.range + areaExtent;
        c.halfAngle = kOmniShadowHalfAngle;
        c.shadowNear = src->GetShadowNear();
        c.distToCamera = distToCamera;
        shadowCandidates.push_back(c);
    }

    // 優先度（小さいほど優先） → カメラに近い順。
    // 敵が大量に湧いてもプレイヤーや紐のライトがタイルからあふれないようにする
    std::stable_sort(shadowCandidates.begin(), shadowCandidates.end(),
                     [](const ShadowCandidate& a, const ShadowCandidate& b) {
                         if (a.priority != b.priority) return a.priority < b.priority;
                         return a.distToCamera < b.distToCamera;
                     });

    SpotShadowCB spotShadowCB;
    spotShadowCB.count = 0;
    for (size_t i = 0; i < shadowCandidates.size(); ++i) {
        auto& c = shadowCandidates[i];
        if (i >= static_cast<size_t>(kMaxSpotShadows)) {
            c.SetShadowIndex(-1);
            continue;
        }

        // 円錐がぴったり収まるよう少しだけ余裕を持たせた正方形の視錐台
        constexpr float kMaxShadowFov = 170.0f * (3.14159265358979323846f / 180.0f);
        const float fov = (std::min)(c.halfAngle * 2.0f * 1.02f, kMaxShadowFov);
        const float farZ = c.range;
        const float nearZ = std::clamp(c.shadowNear, 0.01f, (std::max)(farZ * 0.5f, 0.01f));

        // ライト視点の View：position と direction からカメラと同じ規約（X→Y 回転）で構築
        const RC::Vector3 dir = (RC::Length(c.direction) > 1e-4f) ? Normalize(c.direction) : RC::Vector3{0.0f, -1.0f, 0.0f};
        const float pitch = std::asin(std::clamp(-dir.y, -1.0f, 1.0f));
        const float yaw = std::atan2(dir.x, dir.z);
        const RC::Vector3 lightScale = {1.0f, 1.0f, 1.0f};
        const RC::Vector3 lightRot = {pitch, yaw, 0.0f};
        RC::Matrix4x4 lightWorld = MakeAffineMatrix(lightScale, lightRot, c.position);
        RC::Matrix4x4 lightView = Inverse(lightWorld);
        RC::Matrix4x4 lightProj = MakePerspectiveFovMatrix(fov, 1.0f, nearZ, farZ);

        auto& entry = spotShadowCB.entries[i];
        entry.lightViewProjection = Multiply(lightView, lightProj);
        entry.nearZ = nearZ;
        entry.farZ = farZ;
        entry.tanHalfFov = std::tan(fov * 0.5f);
        entry.lightPosition = c.position;

        c.SetShadowIndex(static_cast<int>(i));
        ++spotShadowCB.count;
    }
    // 影の調整値（ワールド単位。マップのタイルが 2m なので数 cm 程度）。
    // シャドウパスは裏面を描く方式（second-depth）なので、光の当たっている面が
    // 自分の深度で影になること（シャドウアクネ）が起きない。そのためバイアスは
    // 「遮蔽物の裏側にどれだけ光が回り込むか」だけを決める＝小さめでよい。
    spotShadowCB.bias = 0.005f;     // 定数バイアス
    spotShadowCB.slopeBias = 0.5f;  // 斜面バイアス：光に対して斜めな面ほど強く効く
    spotShadowCB.pcfRadius = 1.0f;  // 縁の柔らかさ（テクセル単位）

    // RenderContext の準備（CommandList の設定・フレーム毎の一時 CB 確保）は PreDraw3D で行われるため先に呼ぶ
    RC::PreDraw3D(ctx, cl);

    // --- 2) スポットライト影：タイルごとに深度を描く ---
    // UpdateSpotShadowParams は BeginSpotShadowAtlas に成功したときだけ呼ぶ。
    // 失敗（＝アトラスが使えない）なら b7 は PreDraw3D が入れた count = 0 のままになり、
    // シェーダ側は「遮蔽なし」として扱う（描いていないアトラスを参照しない）
    CHASO_PROFILE_SCOPE("Scene: Render after PreDraw3D (to end of Scene::Render)");
    if (spotShadowCB.count > 0 && RC::BeginSpotShadowAtlas()) {
        RC::UpdateSpotShadowParams(spotShadowCB); // PreDraw3D の後に呼ぶこと
        for (uint32_t i = 0; i < spotShadowCB.count; ++i) {
            const auto& c = shadowCandidates[i];
            RC::BeginSpotShadowTile(static_cast<int>(i));
            drawShadowCasters(c.excludeEntityId);
            RC::Execute3DCommands(); // このタイルへ深度を書く
            RC::EndSpotShadowTile();
        }
        RC::EndSpotShadowAtlas();
    }

    // --- 1) 平行光源のシャドウパス ---
    if (shadowEnabled) {
        RC::BeginShadowPass();
        drawShadowCasters(0u);
        RC::Execute3DCommands(); // シャドウパスのコマンドを実行
        RC::EndShadowPass();
    }

    // --- 2) マスクパス（インタラクトできる物などの輪郭強調用シルエット） ---
    //
    // 強調するかどうかは各スクリプトが OnMaskRender の中で判断する（強調中だけ描く）。
    // ここでは毎フレーム必ずパスを回してマスクRTをクリアしておく。そうしないと
    // 「強調が終わったのに前フレームのシルエットが残る」ことになる。
    //
    // 置き場所はシャドウパスの直後で固定。Execute3DCommands は「その時点までに
    // 積まれた全ての3Dコマンド」を流すので、メイン3Dの Draw を積む前でなければ
    // シーン全体がマスクとして描かれてしまう。
    if (RC::BeginMaskPass()) {
        for (auto& e : entities_) {
            if (!e || e->IsPendingDestroy() || !e->IsVisible() || !e->IsActive()) continue;
            if (auto* nsc = e->GetComponent<NativeScriptComponent>()) {
                for (auto& entry : nsc->scripts) {
                    if (entry.instance) {
                        entry.instance->OnMaskRender();
                    }
                }
            }
        }
        RC::Execute3DCommands();
        RC::EndMaskPass();
    }

    // ===========================================
    // 3D描画
    // ===========================================

    // Entityコンポーネントを持つモデル・スカイボックス・天球の描画
    for (auto& e : entities_) {
        if (!e->IsVisible() || !e->IsActive() || IsStaticMapCollider(*e)) continue; // 静的壁・穴、非表示、非アクティブ時はスキップ

        if (auto* skybox = e->GetComponent<SkyboxComponent>()) {
            if (skybox->HasSkybox() && skybox->visible && skybox->IsEnabled()) {
                RC::DrawSkyBox(skybox->skyboxHandle);
            }
        }
        if (auto* skydome = e->GetComponent<SkydomeComponent>()) {
            if (skydome->HasSkydome() && skydome->visible && skydome->IsEnabled()) {
                RC::DrawSkydome(skydome->skydomeHandle, skydome->texOverride);
            }
        }
        if (auto* ren = e->GetComponent<ModelRendererComponent>()) {
            if (ren->HasModel() && ren->visible && ren->IsEnabled()) {
                if (e->Name() == "Block") {
                    RC::DrawModelGlassTwoPass(ren->modelHandle, ren->texOverride);
                } else {
                    // 同じメッシュ・見た目の物は自動でまとめてインスタンス描画＋視錐台カリング
                    // （半透明・スキニングは内部で従来の DrawModel に回る）
                    RC::DrawModelInstanced(ren->modelHandle, ren->texOverride);
                }
                // スケルトンのデバッグ表示
                if (auto* anim = e->GetComponent<AnimationComponent>()) {
                    if (anim->showSkeleton && anim->IsEnabled()) {
                        RC::DrawModelSkeleton(ren->modelHandle);
                    }
                }
            }
        }
        if (auto* pm = e->GetComponent<PrimitiveMeshComponent>()) {
            if (pm->HasMesh() && pm->visible && pm->IsEnabled()) {
                // 描画方法はエンティティ名ではなくコンポーネントの drawStyle で決める
                switch (pm->drawStyle) {
                case PrimitiveDrawStyle::Water:
                    RC::DrawPrimitiveMeshWater(pm->meshHandle, pm->texOverride);
                    break;
                case PrimitiveDrawStyle::WaterColumn:
                    RC::DrawPrimitiveMeshWaterColumn(pm->meshHandle, pm->texOverride);
                    break;
                default:
                    RC::DrawPrimitiveMesh(pm->meshHandle, pm->texOverride);
                    break;
                }
            }
        }
        if (auto* tm = e->GetComponent<TextMeshComponent>()) {
            if (tm->HasMesh() && tm->visible && tm->IsEnabled()) {
                RC::DrawPrimitiveMesh(tm->meshHandle, tm->texOverride);
                if (tm->HasOutlineMesh()) {
                    RC::DrawPrimitiveMesh(tm->outlineMeshHandle, -1);
                }
            }
        }
        if (auto* water = e->GetComponent<WaterComponent>()) {
            if (water->HasMesh() && water->visible && water->IsEnabled()) {
                RC::DrawWater(water->meshHandle, water->normalMapHandle);
            }
        }
        // ※ スクリプトの OnRender() はここでは呼ばない。
        //    2D パス（下の PreDraw2D 以降）で 1 フレームに 1 回だけ呼ぶ規約。
        //    3D パスでも呼ぶと二重描画になる。
    }

    // VirtualEntity（RC::CreateModelProxy で置いた見た目だけの物）。BVH で視錐台カリングしてまとめて描く
    RC::DrawModelProxies();
    // エフェクト粒（水しぶき・泡・水柱）。種類ごとにまとめて描く（水は奥→手前）
    RC::DrawEffectParticles();

    // ワールド空間スプライト（深度テストありで 3D キューに積む）
    // モデルと同じキューに入るので、任意のモデルとモデルの間に挟まる
    drawSpriteLayer(SpriteSpace::World);

    // GPUParticle の描画
    for (auto& e : entities_) {
        if (!e->IsVisible() || !e->IsActive()) continue;
        if (auto* gpu = e->GetComponent<GPUParticleComponent>()) {
            if (gpu->isInitialized && gpu->particleSystem) {
                gpu->particleSystem->Render(ctx, cl);
            }
        }
    }

    // === ギズモ描画（オーバーレイ: モデルの上に常に描画） ===
    RC::BeginOverlay3D();
    DrawLightGizmos(selectedEntityId_);
    DrawCameraGizmos(selectedEntityId_, float(ctx.app->width) / ctx.app->height);
    DrawColliderGizmos(selectedEntityId_);

    // スクリプト独自の当たり判定ギズモ（敵の感知範囲・視界など）。
    // ColliderComponent のギズモと同じ条件（F3 / F4 / 選択中）で表示する。
    if (showColliderGizmos_ || showAllGizmos_) {
        for (auto& e : entities_) {
            if (!e || e->IsPendingDestroy() || !e->IsActive()) continue;
            if (auto* nsc = e->GetComponent<NativeScriptComponent>()) {
                for (auto& entry : nsc->scripts) {
                    if (entry.instance) entry.instance->OnColliderDebugRender();
                }
            }
        }
    } else if (selectedEntityId_ != 0) {
        if (auto e = FindEntityById(selectedEntityId_)) {
            if (auto* nsc = e->GetComponent<NativeScriptComponent>()) {
                for (auto& entry : nsc->scripts) {
                    if (entry.instance) entry.instance->OnColliderDebugRender();
                }
            }
        }
    }

    if (showAllGizmos_) {
        for (auto& e : entities_) {
            if (!e || e->IsPendingDestroy() || !e->IsActive()) continue;
            if (auto* nsc = e->GetComponent<NativeScriptComponent>()) {
                for (auto& entry : nsc->scripts) {
                    if (entry.instance) entry.instance->OnDebugRender();
                }
            }
        }
    } else if (selectedEntityId_ != 0) {
        if (auto e = FindEntityById(selectedEntityId_)) {
            if (auto* nsc = e->GetComponent<NativeScriptComponent>()) {
                for (auto& entry : nsc->scripts) {
                    if (entry.instance) entry.instance->OnDebugRender();
                }
            }
        }
    }

    RC::EndOverlay3D();

    // ===========================================
    // 2D描画
    // ===========================================
    RC::PreDraw2D(ctx, cl);

    // Entityコンポーネントのスプライト描画（前景：モデルより手前）
    drawSpriteLayer(SpriteSpace::Screen);

    // Entityコンポーネントの文字列描画（スプライトの上に重ねる）
    for (auto& e : entities_) {
        if (!e->IsVisible() || !e->IsActive() || IsStaticMapCollider(*e)) continue;
        if (auto* txt = e->GetComponent<TextRendererComponent>()) {
            if (!txt->visible || !txt->IsEnabled()) continue;
            EnsureTextFont(*txt); // エディタでフォント/サイズを変えた場合の再ロード
            if (!txt->HasFont() || txt->text.empty()) continue;
            RC::Vector2 pos{0.0f, 0.0f};
            if (auto* tr = e->GetComponent<TransformComponent>()) {
                pos = {tr->position.x, tr->position.y};
            }
            RC::DrawString(txt->fontHandle, txt->text, pos, txt->color, txt->scale,
                           txt->align, txt->lineSpacing);
        }
    }

    // NativeScript の OnRender（HUD描画など、コマンドリストが開いた状態で実行）
    if (ctx.isPlaying()) {
        for (auto& e : entities_) {
            if (!e || e->IsPendingDestroy() || !e->IsActive()) continue;
            if (auto* nsc = e->GetComponent<NativeScriptComponent>()) {
                for (auto& entry : nsc->scripts) {
                    if (entry.instance) {
                        entry.instance->OnRender();
                    }
                }
            }
        }
    }
  }

  /// @brief ポストプロセス後のオーバーレイ（ポーズメニュー等）。スクリプトの OnOverlayRender を呼ぶ
  /// @details Render() と同じフレームの続きなので BeginFrame はせず、2D 用のステートだけ張り直す。
  void RenderOverlay(SceneContext& ctx, ID3D12GraphicsCommandList* cl) override {
    if (!ctx.isPlaying()) return;

    // 描くものが無ければステートも触らない（ポストプロセスの出力をそのまま残す）
    bool any = false;
    for (auto& e : entities_) {
        if (!e || e->IsPendingDestroy() || !e->IsActive()) continue;
        if (e->GetComponent<NativeScriptComponent>()) { any = true; break; }
    }
    if (!any) return;

    RC::ResumeDraw2D(ctx, cl);
    for (auto& e : entities_) {
        if (!e || e->IsPendingDestroy() || !e->IsActive()) continue;
        if (auto* nsc = e->GetComponent<NativeScriptComponent>()) {
            for (auto& entry : nsc->scripts) {
                if (entry.instance) {
                    entry.instance->OnOverlayRender();
                }
            }
        }
    }
  }

  ~DataDrivenScene() override = default;

  // =================================================================
  // Save / Load
  // =================================================================

  /// @brief Save current scene state to JSON file
  bool Save() override {
    FlushPendingEntities();
    nlohmann::json root;
    root["sceneName"] = sceneName_;
    nlohmann::json entitiesJson = nlohmann::json::array();
    for (auto& e : entities_) {
      if (!e) continue;
      // レベル JSON やマップローダー由来の動的エンティティはシーン JSON へ書き出さない。
      // 実体はマップ／レベル側にあるので、ここへ写すと保存のたびに複製が増えてしまう。
      if (e->HasTag(kLevelEntityTag) || e->HasTag("from_level")) continue;
      if (e->HasTag("is_wall") || e->HasTag("is_hole") || e->HasTag("is_door") || e->HasTag("is_special_door")) continue;
      if (e->HasTag("transient") || e->HasTag("no_save")) continue;
      entitiesJson.push_back(e->Serialize());
    }
    root["entities"] = entitiesJson;

    std::filesystem::path p(filePath_);
    if (p.has_parent_path()) {
      std::filesystem::create_directories(p.parent_path());
    }

    std::ofstream ofs(filePath_);
    if (!ofs.is_open()) {
      Log::Print("[DataDrivenScene] Failed to save: " + filePath_);
      return false;
    }
    ofs << root.dump(2);
    Log::Print("[DataDrivenScene] Saved: " + filePath_);
    return true;
  }

  /// @brief Load entities from JSON file
  bool Load() {
    entities_.clear();
    broadphase_.Clear();
    RC::ClearEffectParticles();
    gameMode_ = GameModeBase::Create(sceneName_); // GameModeのリセット（Application 側のファクトリで作る）

    if (!std::filesystem::exists(filePath_)) {
      Log::Print("[DataDrivenScene] File not found: " + filePath_);
      return false;
    }

    std::ifstream ifs(filePath_);
    if (!ifs.is_open()) {
      Log::Print("[DataDrivenScene] Failed to open: " + filePath_);
      return false;
    }

    nlohmann::json root;
    try {
      ifs >> root;
    } catch (const nlohmann::json::exception& e) {
      Log::Print("[DataDrivenScene] JSON parse error: " + std::string(e.what()));
      return false;
    }

    if (root.contains("entities")) {
      for (auto& ej : root["entities"]) {
        auto entity = std::make_shared<Entity>();
        entity->Deserialize(ej);
        entities_.push_back(entity);
      }
    }

    if (!levelDataPath_.empty()) {
      LevelLoader loader;
      if (loader.LoadFromFile(levelDataPath_)) {
        auto loaded = loader.TakeEntities();

        // A-05: レベル由来のエンティティに印を付ける。
        // ・Save() でシーン JSON へ二重に書き出さないため
        //   （レベル側の実体はレベル JSON なので、シーンに写すと保存ごとに増える）
        // ・SyncMainCamera がレベル側のカメラを優先するため
        for (auto& e : loaded) {
          if (e) e->SetTag(kLevelEntityTag, 1);
        }

        entities_.insert(entities_.end(), loaded.begin(), loaded.end());

        // 自キャラに座標を反映
        const auto& playerSpawns = loader.GetPlayerSpawns();
        if (!playerSpawns.empty()) {
          const auto& playerData = playerSpawns[0];
          for (auto& e : entities_) {
            if (e && e->GetName() == "player") {
              if (auto* tr = e->GetComponent<TransformComponent>()) {
                tr->position = playerData.translation;
                tr->rotation = playerData.rotation;
              }
              break;
            }
          }
        }

        // 敵キャラの発生処理
        const auto& enemySpawns = loader.GetEnemySpawns();
        for (const auto& enemyData : enemySpawns) {
          std::string entName = enemyData.fileName.empty() ? "Enemy" : enemyData.fileName;
          auto enemy = std::make_shared<Entity>(entName);

          auto& tr = enemy->AddComponent<TransformComponent>();
          tr.position = enemyData.translation;
          tr.rotation = enemyData.rotation;

          enemy->AddComponent<NativeScriptComponent>();
          // どのスクリプトを載せるかはゲームごとに違うので GameMode に任せる
          if (gameMode_) gameMode_->OnLevelEnemySpawned(*enemy, entName);

          entities_.push_back(std::move(enemy));
        }
      }
    }

    Log::Print("[DataDrivenScene] Loaded: " + filePath_ +
               " (" + std::to_string(entities_.size()) + " entities)");
    return true;
  }

  /// @brief Get JSON file path
  const std::string& FilePath() const { return filePath_; }

  /// @brief Change scene name
  void SetSceneName(const std::string& name) { sceneName_ = name; }

  /// @brief Set additional level data JSON path
  void SetLevelDataPath(const std::string& path) { levelDataPath_ = path; }

  /// @brief Backup the current state of entities to memory
  void BackupState() {
      backupJson_.clear();
      for (auto& e : entities_) {
          if (e) {
              backupJson_.push_back(e->Serialize());
          }
      }
  }

  /// @brief Restore the state of entities from memory
  void RestoreState(SceneContext& ctx) {
      if (backupJson_.empty()) return;

      // スクリプトの後始末はモデル等のハンドルを解放する前に行う。
      // 逆順だと OnDestroy から見えるハンドルが既に -1 になっていて、
      // 描画側に残した上書き設定などを片付けられない。
      for (auto& e : entities_) {
          NotifyScriptsDestroy(*e);
      }
      for (auto& e : entities_) {
          ReleaseRuntimeResources(*e);
      }
      entities_.clear();
      pendingEntities_.clear();
      broadphase_.Clear();
      RC::ClearEffectParticles();
      gameMode_ = GameModeBase::Create(sceneName_); // GameModeのリセット（Application 側のファクトリで作る）

      for (auto& ej : backupJson_) {
          auto entity = std::make_shared<Entity>();
          entity->Deserialize(ej);
          entities_.push_back(entity);
      }

      for (auto& e : entities_) {
          InitializeRuntimeResources(*e, ctx);
      }
      resultTriggered_ = false;
      resultChangeRequested_ = false;
      resultDelayTimer_ = 0.0f;
  }

  /// @brief 動的に生成したエンティティのランタイムリソースを初期化する
  void InitDynamicEntityRuntime(Entity& e) override {
      if (currentContext_) {
          InitializeRuntimeResources(e, *currentContext_);
      }
  }

  /// @brief 動的に生成したエンティティのランタイムリソースを解放する
  void ReleaseDynamicEntityRuntime(Entity& e) override {
      ReleaseRuntimeResources(e);
  }

private:
  /// @brief レベル JSON 由来のエンティティに付ける印
  /// @details Save() の除外と、SyncMainCamera の優先判定に使う
  static constexpr const char* kLevelEntityTag = "from_level";

  std::string sceneName_;
  std::string filePath_;
  std::string levelDataPath_; ///< Blenderからエクスポートした追加レベルデータのパス
  float waterTime_ = 0.0f; ///< 水面アニメーション用累積時間
  nlohmann::json backupJson_; ///< メモリ上へのバックアップ用

  // ゲーム結果判定用
  bool resultTriggered_ = false;       ///< 結果判定がトリガーされたか
  bool resultChangeRequested_ = false; ///< シーン遷移要求が送信されたか
  float resultDelayTimer_ = 0.0f;      ///< 遷移までのディレイタイマー
  std::string resultTarget_;          ///< 遷移先シーン名
  SceneTransition resultTransition_ = SceneTransition::Dissolve; ///< 遷移演出（遷移表の値）
  static constexpr float kResultDelay_ = 2.5f; ///< 結果確定からシーン遷移までの待機時間（秒）

  /// @brief TextRendererComponent のフォントを設定に合わせてロード（変更があれば差し替え）
  static void EnsureTextFont(TextRendererComponent& txt) {
      if (!txt.NeedsReload()) return;
      if (txt.fontHandle >= 0) {
          RC::UnloadFont(txt.fontHandle);
          txt.fontHandle = -1;
      }
      txt.loadedPath = txt.fontPath;
      txt.loadedSize = txt.fontSize;
      if (txt.fontPath.empty() || txt.fontSize <= 0.0f) return;
      txt.fontHandle = RC::LoadFont(txt.fontPath, txt.fontSize, txt.atlasSize);
      // 失敗しても loadedPath/Size を更新済みなので毎フレーム再試行はしない
  }

  /// @brief TextMeshComponent の生成済みメッシュへマテリアル設定（色・光沢・UV 等）を書き込む
  static void ApplyTextMeshMaterial(TextMeshComponent& tm) {
      if (!tm.HasMesh()) return;
      if (auto* mat = RC::GetPrimitiveMeshMaterialPtr(tm.meshHandle)) {
          mat->color = tm.color;
          if (tm.lightingMode >= 0) {
              RC::SetPrimitiveMeshLightingMode(tm.meshHandle, static_cast<LightingMode>(tm.lightingMode));
          } else {
              RC::ClearPrimitiveMeshLightingModeOverride(tm.meshHandle);
          }
          mat->shininess = tm.shininess;
          mat->environmentCoefficient = tm.environmentCoeff;
          mat->uvTransform = MakeIdentity4x4();
          mat->uvTransform.m[0][0] = tm.uvTiling.x;
          mat->uvTransform.m[1][1] = tm.uvTiling.y;
          mat->uvTransform.m[3][0] = tm.uvOffset.x;
          mat->uvTransform.m[3][1] = tm.uvOffset.y;
      }
      RC::SetPrimitiveMeshNormalMap(tm.meshHandle, tm.normalMapOverride);
      RC::SetPrimitiveMeshRoughnessMap(tm.meshHandle, tm.roughnessMapOverride);
      ApplyTextMeshOutlineMaterial(tm);
  }

  /// @brief 縁取りシェルのマテリアル（色・ライティング）を書き込む
  static void ApplyTextMeshOutlineMaterial(TextMeshComponent& tm) {
      if (!tm.HasOutlineMesh()) return;
      if (auto* mat = RC::GetPrimitiveMeshMaterialPtr(tm.outlineMeshHandle)) {
          mat->color = tm.outlineColor;
          mat->shininess = tm.shininess;
          mat->environmentCoefficient = 0.0f;
          mat->uvTransform = MakeIdentity4x4();
      }
      if (tm.outlineUnlit) {
          RC::SetPrimitiveMeshLightingMode(tm.outlineMeshHandle, LightingMode::None);
      } else if (tm.lightingMode >= 0) {
          RC::SetPrimitiveMeshLightingMode(tm.outlineMeshHandle, static_cast<LightingMode>(tm.lightingMode));
      } else {
          RC::ClearPrimitiveMeshLightingModeOverride(tm.outlineMeshHandle);
      }
  }

  /// @brief TextMeshComponent のメッシュを設定に合わせて（再）生成する
  /// @details 文字列・フォント・サイズ・厚さ・許容誤差・揃え・行間のいずれかが
  ///          生成時と異なる場合のみ作り直す。失敗しても builtDesc を更新するので
  ///          毎フレーム再試行はしない（設定を変えれば再度試みる）。
  static void EnsureTextMesh(TextMeshComponent& tm) {
      const bool rebuildMain = tm.NeedsRebuild();
      if (!rebuildMain && !tm.NeedsOutlineRebuild()) return;

      if (rebuildMain) {
          if (tm.meshHandle >= 0) {
              RC::UnloadPrimitiveMesh(tm.meshHandle);
              tm.meshHandle = -1;
          }
          tm.builtDesc = tm.MakeDesc();
          tm.built = true;
          tm.info = {};
          tm.meshHandle = RC::GenerateTextMesh(tm.builtDesc, tm.texOverride, &tm.info);
      }

      // 縁取りシェルは本体と同じ desc から作るので、本体を作り直したときも必ず作り直す
      if (tm.outlineMeshHandle >= 0) {
          RC::UnloadPrimitiveMesh(tm.outlineMeshHandle);
          tm.outlineMeshHandle = -1;
      }
      tm.builtOutlineEnabled = tm.outlineEnabled;
      tm.builtOutlineWidth = tm.outlineWidth;
      tm.outlineInfo = {};
      if (tm.meshHandle >= 0 && tm.outlineEnabled && tm.outlineWidth > 0.0f) {
          tm.outlineMeshHandle = RC::GenerateTextMeshOutline(
              tm.builtDesc, tm.outlineWidth, tm.outlineColor, tm.outlineUnlit, &tm.outlineInfo);
      }
      ApplyTextMeshMaterial(tm);
  }

  /// @brief PlayClip() で要求されたクリップ切り替えを適用する
  /// @param anim 対象の AnimationComponent
  /// @param ren 同じエンティティの ModelRendererComponent（モデル未ロードでないこと）
  /// @details パース結果は RC::LoadAnimationFile 側でキャッシュされるため、
  ///          同じクリップへの再切り替えでファイル I/O は発生しない。
  void ApplyPendingClip(AnimationComponent& anim, ModelRendererComponent& ren) {
    const AnimationClip* clip = anim.FindClip(anim.pendingClip_);
    if (!clip) {
      // 未登録のクリップ名。要求だけ捨てて現在の再生を続ける
      anim.pendingRequested_ = false;
      return;
    }

    const std::string& src = clip->path.empty() ? ren.modelPath : clip->path;
    if (src.empty()) {
      anim.pendingRequested_ = false;
      return;
    }

    if (anim.currentClip.empty() || anim.pendingBlend_ <= 0.0f) {
      // 初回、またはブレンド無し指定
      RC::AttachModelAnimation(ren.modelHandle, src, clip->index);
    } else {
      RC::CrossfadeModelAnimation(ren.modelHandle, src, clip->index, anim.pendingBlend_);
    }

    anim.currentClip = anim.pendingClip_;
    anim.clipElapsed_ = 0.0f;
    anim.playing = true;
    anim.pendingRequested_ = false;

    // Inspector 側の直接指定経路と食い違わないよう、適用内容を書き戻しておく。
    // これを省くと次フレームに NeedsReattach() が真になり、クリップが上書きされる。
    anim.animationPath = clip->path;
    anim.animIndex = clip->index;
    anim.MarkAttached();
  }

  /// @brief ボーン追従（ソケット）の更新
  /// @details BoneAttachmentComponent を持つエンティティのモデルを、
  ///          追従先エンティティのスケルトンの Joint に貼り付ける。
  ///          描画行列を直接上書きするので、Transform 同期より後に呼ぶこと。
  void UpdateBoneAttachments() {
    for (auto& e : entities_) {
      auto* ba = e->GetComponent<BoneAttachmentComponent>();
      if (!ba) continue;
      auto* ren = e->GetComponent<ModelRendererComponent>();
      if (!ren || !ren->HasModel()) continue;

      // 追従を止める条件。上書き中だった場合のみ解除して Transform 基準に戻す
      auto releaseOverride = [&]() {
        if (ba->overrideActive_) {
          RC::ClearModelWorldOverride(ren->modelHandle);
          ba->overrideActive_ = false;
        }
      };

      if (!ba->IsEnabled() || ba->targetGuid == 0) { releaseOverride(); continue; }

      auto target = FindEntityByGuid(ba->targetGuid);
      if (!target) { releaseOverride(); continue; }
      auto* tRen = target->GetComponent<ModelRendererComponent>();
      auto* tTr = target->GetComponent<TransformComponent>();
      if (!tRen || !tTr || !tRen->HasModel() || !RC::IsModelReady(tRen->modelHandle)) {
        releaseOverride();
        continue;
      }

      // Joint 姿勢（スケルトン空間＝追従先モデルのローカル）
      RC::Matrix4x4 joint = MakeIdentity4x4();
      if (!ba->jointName.empty()) {
        // Joint 名が見つからない場合は単位行列のまま＝追従先の原点に付く
        RC::GetModelJointMatrix(tRen->modelHandle, ba->jointName, joint);
      }

      // オフセット → Joint → 追従先ワールド の順に合成する（行ベクトル規約）
      const RC::Matrix4x4 offset =
          MakeAffineMatrix(ba->offsetScale, ba->offsetRotation, ba->offsetPosition);
      const RC::Matrix4x4 world =
          Multiply(offset, Multiply(joint, tTr->GetWorldMatrix()));

      RC::SetModelWorldOverride(ren->modelHandle, world);
      ba->overrideActive_ = true;
    }
  }

  /// @brief Register all components to ComponentFactory (called once)
  static void RegisterAllComponents() {
    Entity::ComponentFactory::Register<TransformComponent>("TransformComponent");
    Entity::ComponentFactory::Register<ModelRendererComponent>("ModelRendererComponent");
    Entity::ComponentFactory::Register<SpriteRendererComponent>("SpriteRendererComponent");
    Entity::ComponentFactory::Register<TextRendererComponent>("TextRendererComponent");
    Entity::ComponentFactory::Register<CameraComponent>("CameraComponent");
    Entity::ComponentFactory::Register<DirectionalLightComponent>("DirectionalLightComponent");
    Entity::ComponentFactory::Register<PointLightComponent>("PointLightComponent");
    Entity::ComponentFactory::Register<SpotLightComponent>("SpotLightComponent");
    Entity::ComponentFactory::Register<AreaLightComponent>("AreaLightComponent");
    Entity::ComponentFactory::Register<ColliderComponent>("ColliderComponent");
    Entity::ComponentFactory::Register<AnimationComponent>("AnimationComponent");
    Entity::ComponentFactory::Register<BoneAttachmentComponent>("BoneAttachmentComponent");
    Entity::ComponentFactory::Register<PrimitiveMeshComponent>("PrimitiveMeshComponent");
    Entity::ComponentFactory::Register<TextMeshComponent>("TextMeshComponent");
    Entity::ComponentFactory::Register<SkyboxComponent>("SkyboxComponent");
    Entity::ComponentFactory::Register<SkydomeComponent>("SkydomeComponent");
    Entity::ComponentFactory::Register<WaterComponent>("WaterComponent");
    Entity::ComponentFactory::Register<RigidbodyComponent>("RigidbodyComponent");
    Entity::ComponentFactory::Register<NativeScriptComponent>("NativeScriptComponent");
    Entity::ComponentFactory::Register<GPUParticleComponent>("GPUParticleComponent");
    Entity::ComponentFactory::Register<AudioSourceComponent>("AudioSourceComponent");
    Entity::ComponentFactory::Register<AudioListenerComponent>("AudioListenerComponent");
  }

  void InitializeRuntimeResources(Entity& e, SceneContext& ctx) {
      if (auto* ren = e.GetComponent<ModelRendererComponent>()) {
          if (!ren->modelPath.empty()) ren->modelHandle = RC::LoadModel(ren->modelPath);
          if (!ren->texturePath.empty()) ren->texOverride = RC::LoadTex(ren->texturePath);
          if (!ren->normalMapPath.empty()) ren->normalMapOverride = RC::LoadTex(ren->normalMapPath);
          if (!ren->roughnessMapPath.empty()) ren->roughnessMapOverride = RC::LoadTex(ren->roughnessMapPath);
      }
      if (auto* pm = e.GetComponent<PrimitiveMeshComponent>()) {
          if (!pm->texturePath.empty()) pm->texOverride = RC::LoadTex(pm->texturePath);
          if (!pm->normalMapPath.empty()) pm->normalMapOverride = RC::LoadTex(pm->normalMapPath);
          if (!pm->roughnessMapPath.empty()) pm->roughnessMapOverride = RC::LoadTex(pm->roughnessMapPath);

          // 呼び出し側（弾の水しぶき等のスクリプト）が先にメッシュを作ってから
          // InitDynamicEntityRuntime を呼ぶことがある。以前はここで無条件に作り直しており、
          // 先に作られたメッシュが参照を失ってシーン終了まで解放されなかった（弾 1 発ごとにリーク）。
          // テクスチャ指定が無ければ既存のメッシュをそのまま使い、あれば古い方を解放してから作り直す。
          const bool hasValidMesh =
              pm->meshHandle >= 0 && RC::GetPrimitiveMeshMaterialPtr(pm->meshHandle) != nullptr;
          if (hasValidMesh && pm->texOverride >= 0) {
              RC::UnloadPrimitiveMesh(pm->meshHandle);
              pm->meshHandle = -1;
          }
          if (pm->meshHandle < 0 || !hasValidMesh) {
              switch (pm->type) {
                  case PrimitiveType::Sphere: pm->meshHandle = RC::GenerateSphere(1.0f, pm->texOverride); break;
                  case PrimitiveType::Box: pm->meshHandle = RC::GenerateBox(1.0f, 1.0f, 1.0f, pm->texOverride); break;
                  case PrimitiveType::Plane: pm->meshHandle = RC::GeneratePlane(10.0f, 10.0f, pm->texOverride); break;
                  case PrimitiveType::Cylinder: pm->meshHandle = RC::GenerateCylinder(1.0f, 1.0f, pm->texOverride); break;
                  case PrimitiveType::Cone: pm->meshHandle = RC::GenerateCone(1.0f, 1.0f, pm->texOverride); break;
                  case PrimitiveType::Torus: pm->meshHandle = RC::GenerateTorus(1.0f, 0.3f, pm->texOverride); break;
                  case PrimitiveType::Capsule: pm->meshHandle = RC::GenerateCapsule(0.5f, 1.0f, pm->texOverride); break;
              }
          }
          if (pm->meshHandle >= 0) {
              if (auto* mat = RC::GetPrimitiveMeshMaterialPtr(pm->meshHandle)) {
                  mat->color = pm->color;
                  // -1 は DirectionalLight 追従。マテリアルへは書かず描画時に解決する
                  if (pm->lightingMode >= 0) {
                      RC::SetPrimitiveMeshLightingMode(pm->meshHandle, static_cast<LightingMode>(pm->lightingMode));
                  }
                  mat->shininess = pm->shininess;
                  mat->uvTransform = MakeIdentity4x4();
                  mat->uvTransform.m[0][0] = pm->uvTiling.x;
                  mat->uvTransform.m[1][1] = pm->uvTiling.y;
                  mat->uvTransform.m[3][0] = pm->uvOffset.x;
                  mat->uvTransform.m[3][1] = pm->uvOffset.y;
              }
          }
      }
      if (auto* tm = e.GetComponent<TextMeshComponent>()) {
          if (!tm->texturePath.empty()) tm->texOverride = RC::LoadTex(tm->texturePath);
          if (!tm->normalMapPath.empty()) tm->normalMapOverride = RC::LoadTex(tm->normalMapPath);
          if (!tm->roughnessMapPath.empty()) tm->roughnessMapOverride = RC::LoadTex(tm->roughnessMapPath);
          tm->built = false;
          EnsureTextMesh(*tm);
      }
      if (auto* dl = e.GetComponent<DirectionalLightComponent>()) {
          dl->lightHandle = RC::CreateDirectionalLight(RC::LightActivateMode::Add);
          if (dl->lightHandle >= 0) {
              if (auto* ptr = RC::GetDirectionalLightPtr(dl->lightHandle)) {
                  ptr->Data().color = dl->color;
                  ptr->Data().direction = dl->direction;
                  ptr->Data().intensity = dl->intensity;
                  ptr->Data().ambientColor = dl->ambientColor;
                  ptr->Data().ambientIntensity = dl->ambientIntensity;
              }
          }
      }
      if (auto* pl = e.GetComponent<PointLightComponent>()) {
          pl->lightHandle = RC::CreatePointLight(RC::LightActivateMode::Add);
          if (pl->lightHandle >= 0) {
              if (auto* ptr = RC::GetPointLightPtr(pl->lightHandle)) {
                  ptr->Data().color = {pl->color.x, pl->color.y, pl->color.z, pl->color.w};
                  ptr->Data().radius = pl->radius;
                  ptr->Data().intensity = pl->intensity;
                  ptr->Data().decay = pl->decay;
              }
          }
      }
      if (auto* sl = e.GetComponent<SpotLightComponent>()) {
          sl->lightHandle = RC::CreateSpotLight(RC::LightActivateMode::Add);
          if (sl->lightHandle >= 0) {
              if (auto* ptr = RC::GetSpotLightPtr(sl->lightHandle)) {
                  ptr->Data().color = {sl->color.x, sl->color.y, sl->color.z, sl->color.w};
                  ptr->Data().distance = sl->distance;
                  ptr->Data().intensity = sl->intensity;
                  ptr->Data().decay = sl->decay;
                  ptr->Data().direction = sl->direction;
                  ptr->Data().cosAngle = sl->cosAngle;
              }
          }
      }
      if (auto* al = e.GetComponent<AreaLightComponent>()) {
          al->lightHandle = RC::CreateAreaLight(RC::LightActivateMode::Add);
          if (al->lightHandle >= 0) {
              if (auto* ptr = RC::GetAreaLightPtr(al->lightHandle)) {
                  ptr->Data().color = {al->color.x, al->color.y, al->color.z, al->color.w};
                  ptr->Data().intensity = al->intensity;
                  ptr->Data().range = al->range;
                  ptr->Data().decay = al->decay;
                  ptr->Data().twoSided = al->twoSided;
                  ptr->Data().halfWidth = al->halfWidth;
                  ptr->Data().halfHeight = al->halfHeight;
                  ptr->SetBasis(al->right, al->up);
              }
          }
      }
      if (auto* sb = e.GetComponent<SkyboxComponent>()) {
          if (!sb->skyboxPath.empty()) sb->skyboxHandle = RC::CreateSkyBox(sb->skyboxPath);
      }
      if (auto* sd = e.GetComponent<SkydomeComponent>()) {
          int initTex = -1;
          if (!sd->texturePath.empty()) initTex = RC::LoadTex(sd->texturePath);
          else if (!sd->skydomePath.empty()) initTex = RC::LoadTex(sd->skydomePath);
          sd->texOverride = initTex;
          sd->skydomeHandle = RC::GenerateSkydomeEx(initTex);
          // 生成直後のマテリアルは白なので、復元した乗算カラーを反映しておく
          RC::SetSkydomeColor(sd->skydomeHandle, sd->color);
      }
      if (auto* spr = e.GetComponent<SpriteRendererComponent>()) {
          if (!spr->spritePath.empty()) spr->spriteHandle = RC::LoadSprite(spr->spritePath, ctx);
          spr->loadedPath = spr->spritePath;
      }
      if (auto* txt = e.GetComponent<TextRendererComponent>()) {
          EnsureTextFont(*txt);
      }
      if (auto* water = e.GetComponent<WaterComponent>()) {
          int normalMap = -1;
          if (!water->normalMapPath.empty()) {
              // 法線マップは色ではなく方向ベクトルなので sRGB 変換を掛けてはいけない。
              // 既定の srgb=true で読むと 0.5（=法線 0）が 0.21 に化け、水面全体の法線が
              // 一方向へ傾いてしまう。
              normalMap = RC::LoadTex(water->normalMapPath, /*srgb=*/false);
              water->normalMapHandle = normalMap;
          }
          water->meshHandle = RC::GenerateWaterPlane(
              water->planeWidth, water->planeHeight, water->segments, normalMap);
      }
      if (auto* gpu = e.GetComponent<GPUParticleComponent>()) {
          if (gpu->particleSystem) {
              gpu->particleSystem->SetTexture(gpu->texturePath);
              gpu->particleSystem->Initialize(ctx);
              gpu->isInitialized = true;
          }
      }
      if (auto* audio = e.GetComponent<AudioSourceComponent>()) {
          // クリップのロードのみ。playOnAwake の発火は UpdateAudio() が
          // Playing 状態を見て行う（エディタの停止操作中に ctx.playState がまだ
          // Playing のまま RestoreState → ここが呼ばれるため、ここで鳴らすと誤発火する）
          audio->LoadClips();
      }
  }

public:
  /// @brief AudioSourceComponent の毎フレーム更新（SceneManager::Update から毎フレーム呼ばれる）
  /// @details スクリプトから Play() された音の後始末、再生開始時の playOnAwake 発火、
  ///          BGM の keep-alive、3D 音響のリスナー／音源位置の更新をまとめて行う。
  ///          Update() の後に呼ばれるので、再生中に生成されたエンティティ（弾など）も同じフレームで拾える。
  void UpdateAudio(SceneContext& ctx) override {
      FlushPendingEntities(); // Update を挟まずに生成されたエンティティも拾う
      const bool playing = (ctx.playState == PlayState::Playing);
      const bool paused  = (ctx.playState == PlayState::Paused);
      const float dt = ctx.deltaTime;

      // リスナーは音源より先に更新する（このフレームに Play() される 3D 音の初期定位に使われる）
      UpdateAudioListener(ctx, playing, dt);

      for (auto& e : entities_) {
          if (!e || e->IsPendingDestroy()) continue;
          auto* audio = e->GetComponent<AudioSourceComponent>();
          if (!audio) continue;
          audio->LoadClips(); // Inspector でパスを差し替えた場合の遅延ロード
          if (!e->IsActive() || !audio->IsEnabled()) {
              audio->Silence();
              continue;
          }
          audio->Tick(playing, paused, dt);
      }
  }

private:
  /// @brief 3D 音響のリスナー（聞き手）を更新する
  /// @details 有効な AudioListenerComponent を持つアクティブなエンティティがあれば、最初に見つかった
  ///          1 つを使う。無ければ描画中のカメラ（編集モードならエディタカメラ）が聞き手になる。
  ///          編集モードでは速度を 0 にして、カメラを飛ばしてもドップラーがかからないようにする。
  void UpdateAudioListener(SceneContext& ctx, bool playing, float dt) {
      const RC::Matrix4x4* cameraView = ctx.camera ? &ctx.camera->GetView() : nullptr;
      for (auto& e : entities_) {
          if (!e || e->IsPendingDestroy() || !e->IsActive()) continue;
          auto* listener = e->GetComponent<AudioListenerComponent>();
          if (!listener) continue;
          if (!listener->IsEnabled()) {
              listener->ResetMotion();
              continue;
          }
          auto* tr = e->GetComponent<TransformComponent>();
          if (!tr) continue;
          listener->Tick(*tr, cameraView, playing, dt);
          return;
      }
      if (cameraView) {
          AudioEngine::Get().SetListenerFromView(*cameraView, playing ? dt : 0.0f);
      }
  }

  /// @brief エンティティに付いているスクリプトへ OnDestroy() を通知して破棄する
  /// @details ランタイムリソースの解放より先に呼ぶ。NativeScriptComponent 側で
  ///          scripts をクリアするため、後続の Entity 破棄で二重に呼ばれることはない。
  void NotifyScriptsDestroy(Entity& e) {
      if (auto* nsc = e.GetComponent<NativeScriptComponent>()) {
          nsc->DestroyAllScripts();
      }
  }

  void ReleaseRuntimeResources(Entity& e) {
      if (auto* ren = e.GetComponent<ModelRendererComponent>()) {
          if (ren->modelHandle >= 0) { RC::UnloadModel(ren->modelHandle); ren->modelHandle = -1; }
      }
      if (auto* pm = e.GetComponent<PrimitiveMeshComponent>()) {
          if (pm->meshHandle >= 0) { RC::UnloadPrimitiveMesh(pm->meshHandle); pm->meshHandle = -1; }
      }
      if (auto* tm = e.GetComponent<TextMeshComponent>()) {
          if (tm->meshHandle >= 0) { RC::UnloadPrimitiveMesh(tm->meshHandle); tm->meshHandle = -1; }
          if (tm->outlineMeshHandle >= 0) { RC::UnloadPrimitiveMesh(tm->outlineMeshHandle); tm->outlineMeshHandle = -1; }
          tm->built = false;
      }
      if (auto* dl = e.GetComponent<DirectionalLightComponent>()) {
          if (dl->lightHandle >= 0) { RC::DestroyDirectionalLight(dl->lightHandle); dl->lightHandle = -1; }
      }
      if (auto* pl = e.GetComponent<PointLightComponent>()) {
          if (pl->lightHandle >= 0) { RC::DestroyPointLight(pl->lightHandle); pl->lightHandle = -1; }
      }
      if (auto* sl = e.GetComponent<SpotLightComponent>()) {
          if (sl->lightHandle >= 0) { RC::DestroySpotLight(sl->lightHandle); sl->lightHandle = -1; }
      }
      if (auto* al = e.GetComponent<AreaLightComponent>()) {
          if (al->lightHandle >= 0) { RC::DestroyAreaLight(al->lightHandle); al->lightHandle = -1; }
      }
      if (auto* sb = e.GetComponent<SkyboxComponent>()) {
          if (sb->skyboxHandle >= 0) { RC::UnloadSkyBox(sb->skyboxHandle); sb->skyboxHandle = -1; }
      }
      if (auto* sd = e.GetComponent<SkydomeComponent>()) {
          if (sd->skydomeHandle >= 0) { RC::UnloadSkydome(sd->skydomeHandle); sd->skydomeHandle = -1; }
      }
      if (auto* spr = e.GetComponent<SpriteRendererComponent>()) {
          if (spr->spriteHandle >= 0) { RC::UnloadSprite(spr->spriteHandle); spr->spriteHandle = -1; }
          spr->loadedPath.clear();
      }
      if (auto* txt = e.GetComponent<TextRendererComponent>()) {
          if (txt->fontHandle >= 0) { RC::UnloadFont(txt->fontHandle); txt->fontHandle = -1; }
          txt->loadedPath.clear();
          txt->loadedSize = 0.0f;
      }
      if (auto* water = e.GetComponent<WaterComponent>()) {
          if (water->meshHandle >= 0) { RC::UnloadWater(water->meshHandle); water->meshHandle = -1; }
      }
      if (auto* gpu = e.GetComponent<GPUParticleComponent>()) {
          if (gpu->particleSystem) {
              gpu->particleSystem->Finalize();
          }
          gpu->isInitialized = false;
      }
      if (auto* audio = e.GetComponent<AudioSourceComponent>()) {
          // SE は止め、BGM は keep-alive を外すだけ（次のシーンが同じ曲なら継続）
          audio->ReleaseRuntime();
      }
  }
};
