#pragma once
#include "Common/EngineConfig.h"
#include "Common/Log/Log.h"
#include "ECS/CameraComponent.h"
#include "ECS/Entity.h"
#include "ECS/GPUParticleComponent.h"
#include "ECS/TransformComponent.h"
#include "ECS/WaterComponent.h"
#include "Engine/Render/RenderContext.h"
#include "Graphics/PostProcess/PostProcess.h"
#include "Particle/GPUParticle.h"
#include "RenderCommon.h"
#include "Scene.h"
#include "Game/Framework/UnderwaterLook.h"
#include "Game/Framework/ExposureFlash.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <memory>

#if RC_ENABLE_IMGUI
#include "imgui/imgui.h"
#endif

/// @brief 真上視点のカメラを「水へ飛び込ませる」「深海から浮上させる」共通処理
/// @details Title（スタート時の飛び込み / 入場時の浮上）と Result（もう一度・タイトルへ の
///          飛び込み）が同じ動きを使う。シーン遷移の要求はスクリプトの持ち物
///          （ScriptableEntity::RequestSceneChange）なので、ここではカメラと画面効果だけを扱い、
///          「暗くなりきった（IsDone）」ことを呼び出し側へ知らせる。
///
///          水中の色は UnderwaterLook（Game と共通）で決めるので、飛び込みで暗くなりきった画と
///          次のシーンの浮上開始の画がつながる。
namespace WaterCameraFx {

// ------------------------------------------------------------------
// 小物
// ------------------------------------------------------------------

/// @brief smoothstep（3t^2 - 2t^3）。両端でなめらかに止まる S 字カーブ
/// @param t 進行度（0〜1 に丸める）
/// @return 0〜1
inline float Smooth01(float t) {
  t = std::clamp(t, 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}
/// @brief 1 - (1-u)^3（最初は速く、終わりに向かって減速）
/// @param u 進行度（0〜1 に丸める）
/// @return 0〜1
inline float EaseOutCubic(float u) {
  u = std::clamp(u, 0.0f, 1.0f);
  const float v = 1.0f - u;
  return 1.0f - v * v * v;
}
/// @brief 1 - (1-u)^2（最初は速く、終わりに向かって減速）
/// @param u 進行度（0〜1 に丸める）
/// @return 0〜1
inline float EaseOutQuad(float u) {
  u = std::clamp(u, 0.0f, 1.0f);
  return 1.0f - (1.0f - u) * (1.0f - u);
}
/// @brief u^2（最初はゆっくり、終わりに向かって加速）
/// @param u 進行度（0〜1 に丸める）
/// @return 0〜1
inline float EaseInQuad(float u) {
  u = std::clamp(u, 0.0f, 1.0f);
  return u * u;
}
/// @brief 1 - (1-u)^k（最初は速く、終わりに向かって減速）
/// @param u 進行度（0〜1 に丸める）
/// @param k 指数（0.1 未満は 0.1 に丸める。大きいほど序盤が速い）
/// @return 0〜1
inline float EaseOutPow(float u, float k) {
  u = std::clamp(u, 0.0f, 1.0f);
  return 1.0f - std::pow(1.0f - u, (std::max)(k, 0.1f));
}

/// @brief シーンのメインカメラ（CameraComponent::isMain）を探す
/// @param scene 探すシーン（nullptr なら nullptr を返す）
/// @return TransformComponent も持つメインカメラのエンティティ。見つからなければ nullptr
inline std::shared_ptr<Entity> FindMainCamera(Scene *scene) {
  if (!scene) return nullptr;
  for (const auto &e : scene->GetEntities()) {
    if (!e) continue;
    auto *cam = e->GetComponent<CameraComponent>();
    if (cam && cam->isMain && e->GetComponent<TransformComponent>()) return e;
  }
  return nullptr;
}

/// @brief シーンの水面の高さを探す
/// @param scene 探すシーン（nullptr なら 0 を返す）
/// @return 最初に見つかった WaterComponent の Transform.y。無ければ 0
inline float FindWaterY(Scene *scene) {
  if (!scene) return 0.0f;
  for (const auto &e : scene->GetEntities()) {
    if (!e || !e->GetComponent<WaterComponent>()) continue;
    if (auto *tr = e->GetComponent<TransformComponent>()) return tr->position.y;
    return 0.0f;
  }
  return 0.0f;
}

// 水面を出入りするときのレンズ演出（RailShooterController と同じ値）
inline constexpr float kRadialBlurDuration = 0.45f; ///< ラジアルブラーの長さ（秒。radialScale 1 のとき）
inline constexpr float kRadialBlurMaxWidth = 0.035f; ///< ラジアルブラーの最大幅（radialScale 1 のとき）
inline constexpr float kDropletDuration = 2.2f;     ///< レンズ水滴が消えきるまで（秒）
inline constexpr float kDropletDistortion = 0.06f;  ///< レンズ水滴の歪み量
inline constexpr float kDropletScale = 1.5f;        ///< レンズ水滴の大きさ

/// @brief ラジアルブラー・レンズ水滴・露出フラッシュの減衰を管理する
struct LensFx {
  float radialTimer = 0.0f;              ///< ラジアルブラーの残り時間（秒）
  float radialMax = kRadialBlurMaxWidth; ///< 今回のラジアルブラーの最大幅
  float dropletTimer = 0.0f;             ///< レンズ水滴の残り時間（秒）
  ExposureFlash flash;                   ///< 出入水の露出フラッシュ

  /// @brief 水面を出入りした瞬間のレンズ演出を一式開始する
  /// @param pp           対象の PostProcess（nullptr なら何もしない）
  /// @param radialScale  ラジアルブラーの強さと長さの倍率
  /// @param dropletSpeed 負で上へ昇る（着水）、正で下へ垂れる（出水）
  /// @param flashEv      露出フラッシュのピーク（EV。負で暗く、正で白く。0 で無し）
  /// @param flashDuration 露出フラッシュの長さ（秒）
  void Trigger(PostProcess *pp, float radialScale, float dropletSpeed,
               float flashEv = 0.0f, float flashDuration = 0.0f) {
    if (!pp) return;
    flash.Trigger(pp, flashEv, flashDuration);
    radialMax = kRadialBlurMaxWidth * radialScale;
    radialTimer = kRadialBlurDuration * radialScale;
    pp->AddEffect(PostEffectType::RadialBlur);
    pp->SetRadialBlurCenter(0.5f, 0.5f);
    pp->SetRadialBlurWidth(radialMax);

    dropletTimer = kDropletDuration;
    pp->AddEffect(PostEffectType::ScreenDroplets);
    pp->SetScreenDropletsIntensity(1.0f);
    pp->SetScreenDropletsSpeed(dropletSpeed);
    pp->SetScreenDropletsDistortion(kDropletDistortion);
    pp->SetScreenDropletsScale(kDropletScale);
  }

  /// @brief 各効果の残り時間を進めて減衰させ、終わったものはスタックから外す（毎フレーム呼ぶ）
  /// @param pp 対象の PostProcess（nullptr なら何もしない）
  /// @param dt 経過時間（秒）
  void Update(PostProcess *pp, float dt) {
    if (!pp) return;
    flash.Update(pp, dt);
    // ラジアルブラー：残り時間の割合の 2 乗で幅を絞る。
    // 全体の長さは Trigger の radialScale 倍なので、radialMax / kRadialBlurMaxWidth からその倍率を復元して割る
    if (radialTimer > 0.0f) {
      radialTimer -= dt;
      if (radialTimer <= 0.0f) {
        radialTimer = 0.0f;
        pp->RemoveEffect(PostEffectType::RadialBlur);
      } else {
        const float p = radialTimer / (kRadialBlurDuration * (radialMax / kRadialBlurMaxWidth));
        pp->SetRadialBlurWidth(radialMax * p * p);
      }
    }
    // レンズ水滴：残り時間の割合の 2 乗で薄くする
    if (dropletTimer > 0.0f) {
      dropletTimer -= dt;
      if (dropletTimer <= 0.0f) {
        dropletTimer = 0.0f;
        pp->RemoveEffect(PostEffectType::ScreenDroplets);
      } else {
        const float p = dropletTimer / kDropletDuration;
        pp->SetScreenDropletsIntensity(p * p);
      }
    }
  }

  /// @brief いずれかの効果がまだ残っているか
  /// @return ラジアルブラー・水滴・露出フラッシュのどれかが動作中なら true
  bool Active() const { return radialTimer > 0.0f || dropletTimer > 0.0f || flash.Active(); }

  /// @brief すべての効果を即座に止めてスタックから外す
  /// @param pp 対象の PostProcess（nullptr でもタイマーはリセットする）
  void Clear(PostProcess *pp) {
    flash.Clear(pp);
    if (pp) {
      pp->RemoveEffect(PostEffectType::RadialBlur);
      pp->RemoveEffect(PostEffectType::ScreenDroplets);
    }
    radialTimer = dropletTimer = 0.0f;
  }
};

// ------------------------------------------------------------------
// 泡（GPU パーティクル）
// ------------------------------------------------------------------

/// @brief カメラの前方から昇る泡。GPU パーティクルは水面で消えないので、
///        射出点は常に水面から kMinDepth より下に抑え、寿命も昇りきらない長さにしてある。
class Bubbles {
public:
  static constexpr uint32_t kEmitPerFrame = 5; ///< 演出中の 1 フレームあたりの射出数
  static constexpr float kMinDepth = 7.0f;  ///< 射出ボックス上端を水面からこの深さより下に保つ
  static constexpr float kBoxHalfH = 2.0f;  ///< 射出ボックスの高さの半分（m）

  /// @brief エミッタを作る（初期化で一瞬止まるので、演出の直前ではなく OnCreate で呼ぶ）
  /// @param scene エミッタを作るシーン（nullptr なら何もしない）
  /// @param name  作るエンティティの名前
  /// @note 既にエミッタを持っている場合は何もしない。作った直後は射出数 0。
  void Spawn(Scene *scene, const std::string &name) {
    if (!scene || !entity_.expired()) return;
    auto e = scene->CreateEntity(name);
    auto &tr = e->AddComponent<TransformComponent>();
    tr.position = {0.0f, -200.0f, 0.0f}; // 使うまでは画面外
    auto &gpu = e->AddComponent<GPUParticleComponent>();
    if (auto *ps = gpu.particleSystem.get()) {
      ps->SetPipelinePrefix("gpu_particle_bubble");
      ps->SetParticleType(ParticleType::Default); // EmitParticle.CS：形状・速度・寿命・色を全部使う
      ps->SetBlendMode(kBlendModeNormal);
      ps->SetMaxParticles(1024);
      ps->SetEmitCount(0);
      ps->emitterShape_ = EmitterShape::Box;
      // 奥行きを薄くしてカメラの至近に湧かないようにする（至近の泡は画面いっぱいの玉になる）
      ps->shapeBoxSize_ = {12.0f, kBoxHalfH * 2.0f, 6.0f};
      // 速度は 1 フレームあたりの移動量（UpdateParticle.CS は translate += velocity）
      ps->baseVelocity_ = {0.0f, 0.045f, 0.0f}; // ≒ 2.7 m/s で上昇
      ps->velocityVariance_ = 0.015f;
      ps->gravity_ = 0.0f;
      // 最長 2.2s × 2.7m/s ≒ 6m < kMinDepth。水面から飛び出さない
      ps->minLifeTime_ = 1.2f;
      ps->maxLifeTime_ = 2.2f;
      ps->minScale_ = 0.06f;
      ps->maxScale_ = 0.2f;
      ps->startColor_ = {0.85f, 0.95f, 1.0f, 0.75f};
      ps->endColor_ = {0.85f, 0.95f, 1.0f, 0.0f};
    }
    scene->InitDynamicEntityRuntime(*e);
    entity_ = e;
  }

  /// @brief 1 フレームあたりの射出数を設定する（0 で止める）
  /// @param perFrame 射出数
  void SetEmission(uint32_t perFrame) {
    auto e = entity_.lock();
    if (!e) return;
    if (auto *gpu = e->GetComponent<GPUParticleComponent>()) {
      if (gpu->particleSystem) gpu->particleSystem->SetEmitCount(perFrame);
    }
  }

  /// @brief カメラの見ている方向の少し先・少し下へエミッタを置く
  /// @param cam    追従するカメラの Transform
  /// @param waterY 水面のワールド Y（エミッタをこれより kMinDepth 以上下に保つ）
  void Follow(const TransformComponent &cam, float waterY) {
    auto e = entity_.lock();
    if (!e) return;
    auto *tr = e->GetComponent<TransformComponent>();
    if (!tr) return;
    // rotation.x = +π/2 で真下、0 で +Z、負で見上げ
    const float rx = cam.rotation.x;
    const RC::Vector3 fwd = {0.0f, -std::sin(rx), std::cos(rx)};
    // 前方 8m・下へ 3m に置き、泡が水面から飛び出さないよう高さに上限を掛ける
    float y = cam.position.y + fwd.y * 8.0f - 3.0f;
    y = (std::min)(y, waterY - kMinDepth - kBoxHalfH);
    tr->position = {cam.position.x + fwd.x * 8.0f, y, cam.position.z + fwd.z * 8.0f};
  }

  /// @brief エミッタのエンティティを破棄する
  void Destroy() {
    if (auto e = entity_.lock()) e->Destroy();
    entity_.reset();
  }

private:
  std::weak_ptr<Entity> entity_; ///< エミッタのエンティティ
};

// ------------------------------------------------------------------
// 飛び込み
// ------------------------------------------------------------------

/// @brief 飛び込みの調整値（JSON キーは "dive*"。Title / Result 共通）
struct DiveParams {
  bool enabled = true;        ///< 飛び込み演出を使うか
  float anticipation = 0.35f; ///< ため（秒）
  float lift = 1.5f;          ///< ための持ち上がり（m）
  float plunge = 0.85f;       ///< 水面までの落下（秒）
  float fovBoost = 1.35f;     ///< 落下中の画角の広がり（倍率）
  float sink = 1.7f;          ///< 着水から depth まで沈む時間（秒）
  float depth = 30.0f;        ///< 沈む深さ（m）。UnderwaterLook の deepDepth より深くしておく
  float lookUpPitch = -0.45f; ///< 沈みながら向く角度（rad。負で見上げ）
  float tilt = 0.9f;          ///< 真下向き → lookUpPitch にかける時間（秒）
  float surfaceBlend = 0.25f; ///< 着水後、水中エフェクトが掛かりきるまで（秒）
  bool bubbles = true;        ///< 着水後に泡を出すか

  /// @brief 調整値を JSON へ書き込む（"dive*" キー）
  /// @param j 書き込み先の JSON（呼び出し側の Serialize 用オブジェクト）
  void WriteJson(nlohmann::json &j) const {
    j["diveEnabled"] = enabled;
    j["diveAnticipation"] = anticipation;
    j["diveLift"] = lift;
    j["divePlunge"] = plunge;
    j["diveFovBoost"] = fovBoost;
    j["diveSink"] = sink;
    j["diveDepth"] = depth;
    j["diveLookUpPitch"] = lookUpPitch;
    j["diveTilt"] = tilt;
    j["diveSurfaceBlend"] = surfaceBlend;
    j["diveBubbles"] = bubbles;
  }
  /// @brief JSON から調整値を読み込む（"dive*" キー）
  /// @param j 読み込む JSON（無いキー・型が違うキーは現在の値のまま）
  void ReadJson(const nlohmann::json &j) {
    auto f = [&](const char *k, float &o) { if (j.contains(k) && j[k].is_number()) o = j[k].get<float>(); };
    auto b = [&](const char *k, bool &o) { if (j.contains(k) && j[k].is_boolean()) o = j[k].get<bool>(); };
    b("diveEnabled", enabled);
    f("diveAnticipation", anticipation);
    f("diveLift", lift);
    f("divePlunge", plunge);
    f("diveFovBoost", fovBoost);
    f("diveSink", sink);
    f("diveDepth", depth);
    f("diveLookUpPitch", lookUpPitch);
    f("diveTilt", tilt);
    f("diveSurfaceBlend", surfaceBlend);
    b("diveBubbles", bubbles);
  }
#if RC_ENABLE_IMGUI
  /// @brief 飛び込みの調整 UI を ImGui に描画する
  void DrawImGui() {
    ImGui::Checkbox("Dive Enabled", &enabled);
    ImGui::DragFloat("Dive Anticipation (s)", &anticipation, 0.01f, 0.0f, 2.0f);
    ImGui::DragFloat("Dive Lift (m)", &lift, 0.05f, 0.0f, 10.0f);
    ImGui::DragFloat("Dive Plunge (s)", &plunge, 0.01f, 0.1f, 5.0f);
    ImGui::DragFloat("Dive FOV Boost", &fovBoost, 0.01f, 1.0f, 2.0f);
    ImGui::DragFloat("Dive Sink (s)", &sink, 0.01f, 0.1f, 10.0f);
    ImGui::DragFloat("Dive Depth (m)", &depth, 0.5f, 1.0f, 200.0f);
    ImGui::DragFloat("Dive Look Up Pitch", &lookUpPitch, 0.01f, -1.5f, 1.5708f);
    ImGui::DragFloat("Dive Tilt (s)", &tilt, 0.01f, 0.0f, 5.0f);
    ImGui::DragFloat("Dive Surface Blend (s)", &surfaceBlend, 0.01f, 0.0f, 2.0f);
    ImGui::Checkbox("Dive Bubbles", &bubbles);
  }
#endif
};

/// @brief 真上視点のカメラを水へ飛び込ませ、深海色まで沈める
/// @details ため → 落下（画角が広がる）→ 着水（水中一式＋ラジアルブラー＋上へ昇る水滴＋泡）
///          → 見上げながら沈む → 深さが look.deepDepth に達したら IsDone()。
///          IsDone() のあいだも緩く沈み続けるので、呼び出し側はその間に遷移を要求する。
class DiveSequence {
public:
  /// @brief 飛び込みの段階（未開始 / ため / 落下 / 沈降 / 暗くなりきった）
  enum class Phase { None, Anticipate, Plunge, Sink, Done };

  /// @brief 飛び込みを開始する（カメラの現在の位置・向き・画角を開始状態として覚える）
  /// @param cam     動かすカメラ（Transform と CameraComponent が必要）
  /// @param waterY  水面のワールド Y
  /// @param p       飛び込みの調整値（演出中は参照を保持するので寿命に注意）
  /// @param look    水中の見た目（演出中は参照を保持するので寿命に注意）
  /// @param bubbles 泡のエミッタ（nullptr なら泡なし）
  /// @return 開始できた（または既に動作中）なら true。カメラが不正なら false
  bool Begin(std::shared_ptr<Entity> cam, float waterY, const DiveParams &p,
             const UnderwaterLook::Params &look, Bubbles *bubbles) {
    if (phase_ != Phase::None) return true;
    if (!cam) return false;
    auto *tr = cam->GetComponent<TransformComponent>();
    auto *cc = cam->GetComponent<CameraComponent>();
    if (!tr || !cc) return false;

    *this = DiveSequence{};
    p_ = &p;
    look_ = &look;
    bubbles_ = bubbles;
    camera_ = cam;
    basePos_ = tr->position;
    baseRot_ = tr->rotation;
    baseFov_ = cc->fovY;
    waterY_ = waterY;
    depth_ = waterY - tr->position.y;
    phase_ = (p.anticipation > 0.0f) ? Phase::Anticipate : Phase::Plunge;
    return true;
  }

  /// @brief 飛び込みを終える（水中エフェクト・レンズ演出・泡を止めて状態をリセットする）
  /// @param restoreCamera カメラの位置・向き・画角を開始時へ戻すか（中断用）
  void End(bool restoreCamera) {
    PostProcess *pp = RC::GetRenderContext().GetPostProcess();
    if (effectsOn_) UnderwaterLook::RemoveStack(pp);
    lens_.Clear(pp);
    if (bubbles_) bubbles_->SetEmission(0);
    if (restoreCamera) {
      if (auto cam = camera_.lock()) {
        if (auto *tr = cam->GetComponent<TransformComponent>()) {
          tr->position = basePos_;
          tr->rotation = baseRot_;
        }
        if (auto *cc = cam->GetComponent<CameraComponent>()) cc->fovY = baseFov_;
      }
    }
    *this = DiveSequence{};
  }

  /// @brief 段階ごとにカメラを動かし、着水後は深さに応じて水中エフェクトを更新する（毎フレーム呼ぶ）
  /// @param dt 経過時間（秒）
  /// @return カメラが消えていたら false（呼び出し側は演出を諦める）
  bool Update(float dt) {
    if (phase_ == Phase::None || !p_ || !look_) return true;
    auto cam = camera_.lock();
    if (!cam) return false;
    auto *tr = cam->GetComponent<TransformComponent>();
    auto *cc = cam->GetComponent<CameraComponent>();
    if (!tr || !cc) return false;
    const DiveParams &p = *p_;

    t_ += dt;
    switch (phase_) {
    case Phase::Anticipate: {
      // ため：少し持ち上がる
      const float u = t_ / (std::max)(p.anticipation, 1e-3f);
      tr->position.y = basePos_.y + p.lift * EaseOutCubic(u);
      if (u >= 1.0f) { phase_ = Phase::Plunge; t_ = 0.0f; }
      break;
    }
    case Phase::Plunge: {
      // 落下：加速しながら水面へ落ち、画角を広げて速度感を出す。水面に届いたら着水
      const float u = t_ / (std::max)(p.plunge, 1e-3f);
      const float e = EaseInQuad(u);
      const float startY = basePos_.y + ((p.anticipation > 0.0f) ? p.lift : 0.0f);
      tr->position.y = startY + (waterY_ - startY) * e;
      cc->fovY = baseFov_ * (1.0f + (p.fovBoost - 1.0f) * e);
      if (u >= 1.0f) {
        tr->position.y = waterY_;
        phase_ = Phase::Sink;
        t_ = 0.0f;
        OnSplash();
      }
      break;
    }
    case Phase::Sink: {
      // 沈降：減速しながら沈み、見上げる向きへ傾け、広げた画角を元へ戻す
      const float u = t_ / (std::max)(p.sink, 1e-3f);
      tr->position.y = waterY_ - p.depth * EaseOutQuad(u);
      const float tilt = (p.tilt > 0.0f) ? Smooth01(t_ / p.tilt) : 1.0f;
      tr->rotation.x = baseRot_.x + (p.lookUpPitch - baseRot_.x) * tilt;
      cc->fovY = baseFov_ * (p.fovBoost - (p.fovBoost - 1.0f) * EaseOutQuad(u));
      // 画面が深海色になりきった時点で終わり（沈みきるまで待つと暗い時間が延びるだけ）
      if (u >= 1.0f || (waterY_ - tr->position.y) >= look_->deepDepth) {
        phase_ = Phase::Done;
        t_ = 0.0f;
      }
      break;
    }
    case Phase::Done:
      tr->position.y -= kDoneSinkSpeed * dt; // 遷移が始まるまで止まって見えないように
      break;
    case Phase::None:
      break;
    }

    depth_ = waterY_ - tr->position.y;

    // 着水後：水中エフェクトを surfaceBlend 秒で掛けきり、深さに応じてフォグを暗くする
    if (effectsOn_) {
      submerged_ += dt;
      PostProcess *pp = RC::GetRenderContext().GetPostProcess();
      const float blend = (p.surfaceBlend > 0.0f) ? Smooth01(submerged_ / p.surfaceBlend) : 1.0f;
      UnderwaterLook::SetLerp(pp, blend);
      UnderwaterLook::ApplyFogByDepth(pp, *look_, depth_);
      // 暗くなりきる前に水滴を消しきる（残ったまま Game へ切り替わると一瞬で消えて見える）
      if (phase_ == Phase::Done) lens_.dropletTimer = (std::min)(lens_.dropletTimer, 0.2f);
      lens_.Update(pp, dt);
    }
    if (bubbles_) bubbles_->Follow(*tr, waterY_);
    return true;
  }

  /// @brief 演出中か
  /// @return Begin 後、End されるまで true
  bool IsActive() const { return phase_ != Phase::None; }
  /// @brief 画面が深海色まで暗くなりきったか（呼び出し側はここで遷移を要求する）
  /// @return Done 段階なら true
  bool IsDone() const { return phase_ == Phase::Done; }
  /// @brief Done 段階に入ってからの経過時間
  /// @return 経過時間（秒）。Done 以外では 0
  float DoneTime() const { return phase_ == Phase::Done ? t_ : 0.0f; }
  /// @brief 現在の段階
  /// @return 段階
  Phase GetPhase() const { return phase_; }
  /// @brief カメラの水面からの深さ
  /// @return 深さ（m。水面より下が正）
  float Depth() const { return depth_; }

  /// @brief 段階の名前（ログ・デバッグ表示用）
  /// @param ph 段階
  /// @return 小文字の段階名。不明なら "?"
  static const char *PhaseName(Phase ph) {
    switch (ph) {
    case Phase::None: return "none";
    case Phase::Anticipate: return "anticipate";
    case Phase::Plunge: return "plunge";
    case Phase::Sink: return "sink";
    case Phase::Done: return "done";
    }
    return "?";
  }

private:
  static constexpr float kDoneSinkSpeed = 4.0f; ///< Done 段階で沈み続ける速さ（m/秒）

  /// @brief 着水の瞬間に水中エフェクト一式・レンズ演出・泡を開始する
  void OnSplash() {
    submerged_ = 0.0f;
    if (PostProcess *pp = RC::GetRenderContext().GetPostProcess()) {
      UnderwaterLook::SetupLight(pp, *look_, waterY_);
      UnderwaterLook::AddStack(pp);
      UnderwaterLook::SetLerp(pp, 0.0f);
      UnderwaterLook::ApplyFogByDepth(pp, *look_, 0.0f);
      // 着水：泡が上へ昇る＋一瞬暗く沈む
      lens_.Trigger(pp, 1.0f, -1.3f, ExposureFlashDefaults::kDiveEv, ExposureFlashDefaults::kDiveDuration);
      effectsOn_ = true;
    }
    if (bubbles_ && p_->bubbles) bubbles_->SetEmission(Bubbles::kEmitPerFrame);
    Log::Print("[WaterCameraFx] dive: splash");
  }

  const DiveParams *p_ = nullptr;                 ///< 調整値（Begin で渡されたもの）
  const UnderwaterLook::Params *look_ = nullptr;  ///< 水中の見た目（Begin で渡されたもの）
  Bubbles *bubbles_ = nullptr;                    ///< 泡のエミッタ（nullptr 可）
  std::weak_ptr<Entity> camera_;                  ///< 動かすカメラ
  Phase phase_ = Phase::None;                     ///< 現在の段階
  float t_ = 0.0f;                                ///< 現在の段階に入ってからの経過時間（秒）
  RC::Vector3 basePos_{};                         ///< 開始時のカメラ位置
  RC::Vector3 baseRot_{};                         ///< 開始時のカメラ回転
  float baseFov_ = 0.45f;                         ///< 開始時の画角（rad）
  float waterY_ = 0.0f;                           ///< 水面のワールド Y
  float depth_ = 0.0f;                            ///< カメラの深さ（m）
  float submerged_ = 0.0f;                        ///< 着水してからの経過時間（秒）
  bool effectsOn_ = false;                        ///< 水中エフェクトを積んだか
  LensFx lens_;                                   ///< 着水時のレンズ演出
};

// ------------------------------------------------------------------
// 浮上（深海 → 水面 → 真上視点へ上昇）
// ------------------------------------------------------------------

/// @brief 浮上の調整値（JSON キーは "rise*"）
struct RiseParams {
  bool enabled = true;        ///< 浮上演出を使うか
  float depth = 26.0f;        ///< 浮上を始める深さ（m）。UnderwaterLook の deepDepth より深く
  float underwaterTime = 2.4f;///< 水面の直下まで上がる時間（秒）
  float ease = 2.2f;          ///< 水中の速度カーブ（1 - (1-t)^ease。暗い深海は速く抜ける）
  float pitchStart = -0.35f;  ///< 浮上開始時に見上げる角度（rad）
  float climbTime = 2.0f;     ///< 水面を抜けてから最終位置（真上視点）までの時間（秒）
  float surfaceBlend = 0.5f;  ///< 水面を抜けてから水中エフェクトが消えきるまで（秒）
  bool bubbles = true;        ///< 水中で泡を出すか

  /// @brief 調整値を JSON へ書き込む（"rise*" キー）
  /// @param j 書き込み先の JSON（呼び出し側の Serialize 用オブジェクト）
  void WriteJson(nlohmann::json &j) const {
    j["riseEnabled"] = enabled;
    j["riseDepth"] = depth;
    j["riseUnderwaterTime"] = underwaterTime;
    j["riseEase"] = ease;
    j["risePitchStart"] = pitchStart;
    j["riseClimbTime"] = climbTime;
    j["riseSurfaceBlend"] = surfaceBlend;
    j["riseBubbles"] = bubbles;
  }
  /// @brief JSON から調整値を読み込む（"rise*" キー）
  /// @param j 読み込む JSON（無いキー・型が違うキーは現在の値のまま）
  void ReadJson(const nlohmann::json &j) {
    auto f = [&](const char *k, float &o) { if (j.contains(k) && j[k].is_number()) o = j[k].get<float>(); };
    auto b = [&](const char *k, bool &o) { if (j.contains(k) && j[k].is_boolean()) o = j[k].get<bool>(); };
    b("riseEnabled", enabled);
    f("riseDepth", depth);
    f("riseUnderwaterTime", underwaterTime);
    f("riseEase", ease);
    f("risePitchStart", pitchStart);
    f("riseClimbTime", climbTime);
    f("riseSurfaceBlend", surfaceBlend);
    b("riseBubbles", bubbles);
  }
#if RC_ENABLE_IMGUI
  /// @brief 浮上の調整 UI を ImGui に描画する
  void DrawImGui() {
    ImGui::Checkbox("Rise Enabled", &enabled);
    ImGui::DragFloat("Rise Depth (m)", &depth, 0.5f, 1.0f, 200.0f);
    ImGui::DragFloat("Rise Underwater (s)", &underwaterTime, 0.05f, 0.2f, 10.0f);
    ImGui::DragFloat("Rise Ease", &ease, 0.05f, 1.0f, 5.0f);
    ImGui::DragFloat("Rise Pitch Start", &pitchStart, 0.01f, -1.5f, 1.5f);
    ImGui::DragFloat("Rise Climb (s)", &climbTime, 0.05f, 0.2f, 10.0f);
    ImGui::DragFloat("Rise Surface Blend (s)", &surfaceBlend, 0.01f, 0.0f, 3.0f);
    ImGui::Checkbox("Rise Bubbles", &bubbles);
  }
#endif
};

/// @brief カメラを深海に置き、水面へ浮上させてから元の位置・向き（真上視点など）へ上昇させる
/// @details Begin の時点でカメラが置かれている位置・向きが「最終位置」になる。
///          1. 水中  : 深海から水面の直下まで。暗い深海は速く抜け、水面の手前で減速する。
///                     向きは見上げ → 水平へ。深さに応じてフォグが明るくなる。
///          2. 上昇  : 水面を抜け（ラジアルブラー＋下へ垂れる水滴）、水平線を見ながら上がり、
///                     最後に最終の向き（真上視点なら真下）へ傾く。
///          水中エフェクトは Begin で積み、水面を抜けたら surfaceBlend 秒で外す。
class RiseSequence {
public:
  /// @brief 浮上の段階（未開始 / 水中 / 上昇 / 動きは終わり後始末中）
  enum class Phase { None, Underwater, Climb, Done };

  /// @brief 浮上を開始する（カメラを深海の開始位置へ移し、水中エフェクトを積む）
  /// @param cam     動かすカメラ（Transform が必要。現在の位置・向きが最終位置になる）
  /// @param waterY  水面のワールド Y
  /// @param p       浮上の調整値（演出中は参照を保持するので寿命に注意）
  /// @param look    水中の見た目（演出中は参照を保持するので寿命に注意）
  /// @param bubbles 泡のエミッタ（nullptr なら泡なし）
  /// @return 開始できたら true。カメラが不正なら false
  bool Begin(std::shared_ptr<Entity> cam, float waterY, const RiseParams &p,
             const UnderwaterLook::Params &look, Bubbles *bubbles) {
    if (!cam) return false;
    auto *tr = cam->GetComponent<TransformComponent>();
    if (!tr) return false;

    // 現在のカメラを最終位置として覚え、深海の開始位置・水面直下の中継点を決める
    *this = RiseSequence{};
    p_ = &p;
    look_ = &look;
    bubbles_ = bubbles;
    camera_ = cam;
    waterY_ = waterY;
    goalPos_ = tr->position;
    goalRot_ = tr->rotation;
    startPos_ = {goalPos_.x, waterY - p.depth, goalPos_.z};
    startRot_ = {p.pitchStart, goalRot_.y, goalRot_.z};
    surfacePos_ = {goalPos_.x, waterY - kUnderSurface, goalPos_.z};
    tr->position = startPos_;
    tr->rotation = startRot_;

    // 最初から完全に水中の見た目で始める。泡は水面に届かない深さから始まるときだけ出す
    if (PostProcess *pp = RC::GetRenderContext().GetPostProcess()) {
      UnderwaterLook::SetupLight(pp, look, waterY);
      UnderwaterLook::AddStack(pp);
      UnderwaterLook::SetLerp(pp, 1.0f);
      UnderwaterLook::ApplyFogByDepth(pp, look, p.depth);
      effectsOn_ = true;
    }
    if (bubbles_ && p.bubbles && p.depth > Bubbles::kMinDepth + 1.0f) {
      bubbles_->SetEmission(Bubbles::kEmitPerFrame);
      emitting_ = true;
    }
    phase_ = Phase::Underwater;
    Log::Print("[WaterCameraFx] rise begin");
    return true;
  }

  /// @brief 途中で止める。カメラは最終位置へ置き、画面効果は外す
  void End() {
    if (auto cam = camera_.lock()) {
      if (auto *tr = cam->GetComponent<TransformComponent>()) {
        tr->position = goalPos_;
        tr->rotation = goalRot_;
      }
    }
    PostProcess *pp = RC::GetRenderContext().GetPostProcess();
    if (effectsOn_) UnderwaterLook::RemoveStack(pp);
    lens_.Clear(pp);
    if (bubbles_) bubbles_->SetEmission(0);
    *this = RiseSequence{};
  }

  /// @brief 段階ごとにカメラを動かし、水面通過・水中エフェクトの解除・後始末を進める（毎フレーム呼ぶ）
  /// @param dt 経過時間（秒）
  /// @note カメラが消えていたら End して終了する。
  void Update(float dt) {
    if (phase_ == Phase::None || !p_ || !look_) return;
    PostProcess *pp = RC::GetRenderContext().GetPostProcess();
    auto cam = camera_.lock();
    auto *tr = cam ? cam->GetComponent<TransformComponent>() : nullptr;
    if (!tr) { End(); return; }
    const RiseParams &p = *p_;

    t_ += dt;
    // 水中：深海から水面直下へ（序盤ほど速い）。向きは見上げ → 水平へ
    if (phase_ == Phase::Underwater) {
      const float u = t_ / (std::max)(p.underwaterTime, 1e-3f);
      const float e = EaseOutPow(u, p.ease);
      tr->position = RC::Lerp(startPos_, surfacePos_, e);
      tr->rotation = RC::Lerp(startRot_, {0.0f, goalRot_.y, goalRot_.z}, Smooth01(u));
      if (u >= 1.0f) { phase_ = Phase::Climb; t_ = 0.0f; }
    } else if (phase_ == Phase::Climb) {
      const float u = t_ / (std::max)(p.climbTime, 1e-3f);
      tr->position = RC::Lerp(surfacePos_, goalPos_, Smooth01(u));
      // 水面を抜けた直後は水平線を見せ、上がりきる手前で最終の向きへ傾く
      const float r = Smooth01((u - 0.2f) / 0.8f);
      tr->rotation = RC::Lerp({0.0f, goalRot_.y, goalRot_.z}, goalRot_, r);
      if (u >= 1.0f) {
        tr->position = goalPos_;
        tr->rotation = goalRot_;
        phase_ = Phase::Done;
        t_ = 0.0f;
        Log::Print("[WaterCameraFx] rise finished");
      }
    }

    const float depth = waterY_ - tr->position.y;

    // 泡は水面に届かない深さまでで止める
    if (emitting_ && depth < Bubbles::kMinDepth + 1.0f) {
      bubbles_->SetEmission(0);
      emitting_ = false;
    }
    if (bubbles_) bubbles_->Follow(*tr, waterY_);

    // 水面を抜けた瞬間：出水の演出を出し、水中エフェクトを外し始める
    if (effectsOn_ && !surfaced_ && depth <= 0.0f) {
      surfaced_ = true;
      surfacedTime_ = 0.0f;
      // 出水：水滴が下へ垂れる＋一瞬白く飛ぶ
      lens_.Trigger(pp, 0.7f, 1.3f, ExposureFlashDefaults::kEmergeEv, ExposureFlashDefaults::kEmergeDuration);
    }
    // 水中では深さに応じてフォグを明るくし、水面を抜けた後は surfaceBlend 秒で水中エフェクトを外す
    if (effectsOn_) {
      if (!surfaced_) {
        UnderwaterLook::ApplyFogByDepth(pp, *look_, depth);
      } else {
        surfacedTime_ += dt;
        const float blend = (p.surfaceBlend > 0.0f) ? 1.0f - Smooth01(surfacedTime_ / p.surfaceBlend) : 0.0f;
        UnderwaterLook::SetLerp(pp, blend);
        if (blend <= 0.0f) {
          UnderwaterLook::RemoveStack(pp);
          effectsOn_ = false;
        }
      }
    }
    lens_.Update(pp, dt);

    // カメラが止まり、画面効果もすべて消えたら完全に終了
    if (phase_ == Phase::Done && !effectsOn_ && !lens_.Active()) {
      if (bubbles_) bubbles_->SetEmission(0);
      phase_ = Phase::None;
    }
  }

  /// @brief カメラがまだ動いているか（入力を止める判定に使う）
  /// @return Underwater / Climb 段階なら true
  bool IsMoving() const { return phase_ == Phase::Underwater || phase_ == Phase::Climb; }
  /// @brief 画面効果の後始末まで含めてまだ動いているか
  /// @return None 以外なら true
  bool IsActive() const { return phase_ != Phase::None; }

private:
  static constexpr float kUnderSurface = 0.6f; ///< 水中フェーズの終点（水面の少し下）

  const RiseParams *p_ = nullptr;                    ///< 調整値（Begin で渡されたもの）
  const UnderwaterLook::Params *look_ = nullptr;     ///< 水中の見た目（Begin で渡されたもの）
  Bubbles *bubbles_ = nullptr;                       ///< 泡のエミッタ（nullptr 可）
  std::weak_ptr<Entity> camera_;                     ///< 動かすカメラ
  Phase phase_ = Phase::None;                        ///< 現在の段階
  float t_ = 0.0f;                                   ///< 現在の段階に入ってからの経過時間（秒）
  float waterY_ = 0.0f;                              ///< 水面のワールド Y
  RC::Vector3 startPos_{}, surfacePos_{}, goalPos_{}; ///< 開始位置 / 水面直下の中継点 / 最終位置
  RC::Vector3 startRot_{}, goalRot_{};               ///< 開始時の向き / 最終の向き
  bool effectsOn_ = false;                           ///< 水中エフェクトを積んでいるか
  bool surfaced_ = false;                            ///< 水面を抜けたか
  float surfacedTime_ = 0.0f;                        ///< 水面を抜けてからの経過時間（秒）
  bool emitting_ = false;                            ///< 泡を出しているか
  LensFx lens_;                                      ///< 出水時のレンズ演出
};

// ------------------------------------------------------------------
// 空からの降下（Ascend 遷移で入ってきたとき：白 → 空 → 真上視点へ降りる）
// ------------------------------------------------------------------

/// @brief 降下の調整値（JSON キーは "descend*"）
struct DescendParams {
  bool enabled = true;       ///< 降下演出を使うか
  float height = 24.0f;      ///< 最終位置からどれだけ上から降りてくるか（m）
  float pitchStart = 0.08f;  ///< 降下開始時の向き（rad。0 で水平線、正で見下ろし）
  float duration = 2.2f;     ///< 最終位置まで降りる時間（秒）
  float flashEv = 2.5f;      ///< 白く飛んだ状態の露出（EV）。これが duration の前半で 0 へ戻る
  float flashDuration = 1.1f;///< 露出が戻りきるまで（秒）

  /// @brief 調整値を JSON へ書き込む（"descend*" キー）
  /// @param j 書き込み先の JSON（呼び出し側の Serialize 用オブジェクト）
  void WriteJson(nlohmann::json &j) const {
    j["descendEnabled"] = enabled;
    j["descendHeight"] = height;
    j["descendPitchStart"] = pitchStart;
    j["descendDuration"] = duration;
    j["descendFlashEv"] = flashEv;
    j["descendFlashDuration"] = flashDuration;
  }
  /// @brief JSON から調整値を読み込む（"descend*" キー）
  /// @param j 読み込む JSON（無いキー・型が違うキーは現在の値のまま）
  void ReadJson(const nlohmann::json &j) {
    auto f = [&](const char *k, float &o) { if (j.contains(k) && j[k].is_number()) o = j[k].get<float>(); };
    if (j.contains("descendEnabled") && j["descendEnabled"].is_boolean()) enabled = j["descendEnabled"].get<bool>();
    f("descendHeight", height);
    f("descendPitchStart", pitchStart);
    f("descendDuration", duration);
    f("descendFlashEv", flashEv);
    f("descendFlashDuration", flashDuration);
  }
};

/// @brief カメラを最終位置の上空へ置き、水平線を見た状態から最終位置・向きへ降ろす
/// @details Begin の時点でカメラが置かれている位置・向きが「最終位置」になる（RiseSequence と同じ）。
///          画面は白く飛んだ露出（flashEv）から始まり、降りながら元の明るさへ戻る。
///          ステージクリアの「空へ抜ける」（ClearAscendScript → Ascend 遷移）の受け側。
class DescendSequence {
public:
  /// @brief 降下を開始する（カメラを上空の開始位置へ移し、白く飛んだ露出から始める）
  /// @param cam 動かすカメラ（Transform が必要。現在の位置・向きが最終位置になる）
  /// @param p   降下の調整値（演出中は参照を保持するので寿命に注意）
  /// @return 開始できたら true。カメラが不正なら false
  bool Begin(std::shared_ptr<Entity> cam, const DescendParams &p) {
    if (!cam) return false;
    auto *tr = cam->GetComponent<TransformComponent>();
    if (!tr) return false;
    *this = DescendSequence{};
    p_ = &p;
    camera_ = cam;
    goalPos_ = tr->position;
    goalRot_ = tr->rotation;
    startPos_ = {goalPos_.x, goalPos_.y + p.height, goalPos_.z};
    startRot_ = {p.pitchStart, goalRot_.y, goalRot_.z};
    tr->position = startPos_;
    tr->rotation = startRot_;
    flash_.TriggerFromPeak(RC::GetRenderContext().GetPostProcess(), p.flashEv, p.flashDuration);
    moving_ = true;
    Log::Print("[WaterCameraFx] descend begin");
    return true;
  }

  /// @brief カメラを最終位置へ降ろし、露出を戻していく（毎フレーム呼ぶ）
  /// @param dt 経過時間（秒）
  /// @note カメラが止まった後も露出の減衰は続ける。カメラが消えていたら End する。
  void Update(float dt) {
    if (!p_) return;
    PostProcess *pp = RC::GetRenderContext().GetPostProcess();
    flash_.Update(pp, dt);
    if (!moving_) return;
    auto cam = camera_.lock();
    auto *tr = cam ? cam->GetComponent<TransformComponent>() : nullptr;
    if (!tr) { End(); return; }
    t_ += dt;
    const float u = t_ / (std::max)(p_->duration, 1e-3f);
    // 位置は最初に速く、最後にふわりと止まる。向きは少し遅れて真下（最終の向き）へ傾く
    tr->position = RC::Lerp(startPos_, goalPos_, EaseOutCubic(u));
    tr->rotation = RC::Lerp(startRot_, goalRot_, Smooth01((u - 0.15f) / 0.85f));
    if (u >= 1.0f) {
      tr->position = goalPos_;
      tr->rotation = goalRot_;
      moving_ = false;
      Log::Print("[WaterCameraFx] descend finished");
    }
  }

  /// @brief 途中で止める。カメラは最終位置へ置き、露出は戻す
  void End() {
    if (auto cam = camera_.lock()) {
      if (auto *tr = cam->GetComponent<TransformComponent>()) {
        tr->position = goalPos_;
        tr->rotation = goalRot_;
      }
    }
    flash_.Clear(RC::GetRenderContext().GetPostProcess());
    *this = DescendSequence{};
  }

  /// @brief カメラがまだ動いているか（入力・UI を止める判定に使う）
  /// @return 降下中なら true
  bool IsMoving() const { return moving_; }
  /// @brief 露出の後始末まで含めてまだ動いているか
  /// @return 降下中または露出フラッシュ中なら true
  bool IsActive() const { return moving_ || flash_.Active(); }

private:
  const DescendParams *p_ = nullptr;   ///< 調整値（Begin で渡されたもの）
  std::weak_ptr<Entity> camera_;       ///< 動かすカメラ
  float t_ = 0.0f;                     ///< 降下開始からの経過時間（秒）
  bool moving_ = false;                ///< 降下中か
  RC::Vector3 startPos_{}, goalPos_{}; ///< 開始位置 / 最終位置
  RC::Vector3 startRot_{}, goalRot_{}; ///< 開始時の向き / 最終の向き
  ExposureFlash flash_;                ///< 白く飛んだ露出からの戻し
};

// ------------------------------------------------------------------
// 空へ抜ける（ステージクリア：その場から空へ跳ね上がり、白く飛ぶ）
// ------------------------------------------------------------------

/// @brief 跳ね上がりの調整値（JSON キーは ClearAscendScript 側で持つ）
struct AscendParams {
  float anticipation = 0.25f; ///< 沈み込む「ため」（秒）
  float dip = 0.6f;           ///< ための沈み込み（m）
  float launch = 1.3f;        ///< 跳ね上がる時間（秒）
  float height = 30.0f;       ///< 跳ね上がる高さ（m。水中から始まったときは水面までの深さを足す）
  float lookUpPitch = -0.8f;  ///< 跳ね上がりながら向く角度（rad。負で見上げ）
  float fovBoost = 1.25f;     ///< 跳ね上がり中の画角の広がり（倍率）
  float whiteStart = 0.45f;   ///< launch のこの割合から白へ飛ばし始める（0〜1）
  float whiteEv = 3.0f;       ///< 白く飛ばすときの露出の上限（EV）
};

/// @brief その場から空へ跳ね上がり、露出と白い幕で画面を白く飛ばす
/// @details ため → 跳ね上がり（加速しながら上へ、見上げながら、画角が広がる）。
///          跳ね上がりの後半で露出を上げ、白い幕（WhiteAlpha）を重ねていき、
///          幕が 1 に届いたら IsDone()。呼び出し側はそこで Ascend 遷移を要求する
///          （SceneManager は同じ白へ抜けるので継ぎ目が見えない）。
///          白い幕はポストプロセスの後に描く必要があるので、呼び出し側の OnOverlayRender で描く。
class AscendSequence {
public:
  /// @brief 跳ね上がりの段階（未開始 / ため / 跳ね上がり / 白くなりきった）
  enum class Phase { None, Anticipate, Launch, Done };

  /// @brief 跳ね上がりを開始する（カメラの現在の位置・向き・画角を開始状態として覚える）
  /// @param cam    動かすカメラ（Transform と CameraComponent が必要）
  /// @param waterY 水面のワールド Y（水中から始まったときに上昇量へ深さを足すのに使う）
  /// @param p      跳ね上がりの調整値（演出中は参照を保持するので寿命に注意）
  /// @return 開始できたら true。カメラが不正なら false
  bool Begin(std::shared_ptr<Entity> cam, float waterY, const AscendParams &p) {
    if (!cam) return false;
    auto *tr = cam->GetComponent<TransformComponent>();
    auto *cc = cam->GetComponent<CameraComponent>();
    if (!tr || !cc) return false;
    *this = AscendSequence{};
    p_ = &p;
    camera_ = cam;
    basePos_ = tr->position;
    baseRot_ = tr->rotation;
    baseFov_ = cc->fovY;
    // 水中で終わったステージ（沈み洞など）でも、水面を抜けて空まで上がるように深さを足す
    const float below = (std::max)(0.0f, waterY - basePos_.y);
    rise_ = p.height + below;
    phase_ = (p.anticipation > 0.0f) ? Phase::Anticipate : Phase::Launch;
    Log::Print("[WaterCameraFx] ascend begin");
    return true;
  }

  /// @brief 段階ごとにカメラを動かし、後半で露出と白い幕を上げていく（毎フレーム呼ぶ）
  /// @param dt 経過時間（秒）
  /// @return カメラが消えていたら false
  bool Update(float dt) {
    if (phase_ == Phase::None || !p_) return true;
    auto cam = camera_.lock();
    if (!cam) return false;
    auto *tr = cam->GetComponent<TransformComponent>();
    auto *cc = cam->GetComponent<CameraComponent>();
    if (!tr || !cc) return false;
    const AscendParams &p = *p_;
    PostProcess *pp = RC::GetRenderContext().GetPostProcess();

    t_ += dt;
    switch (phase_) {
    case Phase::Anticipate: {
      // ため：少し沈み込む
      const float u = t_ / (std::max)(p.anticipation, 1e-3f);
      tr->position.y = basePos_.y - p.dip * EaseOutCubic(u);
      if (u >= 1.0f) { phase_ = Phase::Launch; t_ = 0.0f; }
      break;
    }
    case Phase::Launch: {
      const float u = (std::min)(t_ / (std::max)(p.launch, 1e-3f), 1.0f);
      const float startY = basePos_.y - ((p.anticipation > 0.0f) ? p.dip : 0.0f);
      // 最初は速く飛び出し（EaseOutQuad）、そのまま空へ抜けていく
      tr->position.y = startY + (rise_ + p.dip) * EaseOutQuad(u);
      tr->rotation.x = baseRot_.x + (p.lookUpPitch - baseRot_.x) * Smooth01(u / 0.8f);
      const float fovK = std::sin(3.14159265f * (std::min)(u * 1.2f, 1.0f)); // 途中で広がって戻る
      cc->fovY = baseFov_ * (1.0f + (p.fovBoost - 1.0f) * fovK);
      // 後半で白へ：露出を上げ、白い幕を重ねる
      const float w = Smooth01((u - p.whiteStart) / (std::max)(1.0f - p.whiteStart, 1e-3f));
      white_ = w;
      flash_.Hold(pp, p.whiteEv * w);
      if (u >= 1.0f) { phase_ = Phase::Done; t_ = 0.0f; white_ = 1.0f; }
      break;
    }
    case Phase::Done:
      tr->position.y += 6.0f * dt; // 遷移が始まるまで止まって見えないように
      break;
    case Phase::None:
      break;
    }
    return true;
  }

  /// @brief 途中で止める（露出を戻し、自分で積んだ ColorGrade を外す）
  /// @param restoreCamera カメラの位置・向き・画角を開始時へ戻すか
  void End(bool restoreCamera) {
    flash_.Clear(RC::GetRenderContext().GetPostProcess());
    if (restoreCamera) {
      if (auto cam = camera_.lock()) {
        if (auto *tr = cam->GetComponent<TransformComponent>()) {
          tr->position = basePos_;
          tr->rotation = baseRot_;
        }
        if (auto *cc = cam->GetComponent<CameraComponent>()) cc->fovY = baseFov_;
      }
    }
    *this = AscendSequence{};
  }

  /// @brief 演出中か
  /// @return Begin 後、End されるまで true
  bool IsActive() const { return phase_ != Phase::None; }
  /// @brief 白い幕が 1 に届いたか（呼び出し側はここで Ascend 遷移を要求する）
  /// @return Done 段階なら true
  bool IsDone() const { return phase_ == Phase::Done; }
  /// @brief Done 段階に入ってからの経過時間
  /// @return 経過時間（秒）。Done 以外では 0
  float DoneTime() const { return phase_ == Phase::Done ? t_ : 0.0f; }
  /// @brief 白い幕の不透明度。呼び出し側が OnOverlayRender で画面全体に敷く
  /// @return 不透明度（0〜1）
  float WhiteAlpha() const { return white_; }

private:
  const AscendParams *p_ = nullptr; ///< 調整値（Begin で渡されたもの）
  std::weak_ptr<Entity> camera_;    ///< 動かすカメラ
  Phase phase_ = Phase::None;       ///< 現在の段階
  float t_ = 0.0f;                  ///< 現在の段階に入ってからの経過時間（秒）
  RC::Vector3 basePos_{};           ///< 開始時のカメラ位置
  RC::Vector3 baseRot_{};           ///< 開始時のカメラ回転
  float baseFov_ = 0.45f;           ///< 開始時の画角（rad）
  float rise_ = 0.0f;               ///< 跳ね上がる高さ（m。水中開始なら水面までの深さを含む）
  float white_ = 0.0f;              ///< 白い幕の不透明度（0〜1）
  ExposureFlash flash_;             ///< 白へ飛ばす露出
};

} // namespace WaterCameraFx
