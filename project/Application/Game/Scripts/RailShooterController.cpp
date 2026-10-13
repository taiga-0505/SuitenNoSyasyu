#include "ECS/ScriptableEntity.h"
#include "ECS/ScriptRegistry.h"
#include "Input/Input.h"
#include "Engine/Input/Controller/Controller.h"
#include "RenderCommon.h"
#include "Engine/Render/RenderContext.h"
#include "Common/Math/MathUtils.h"
#include "Framework/App.h"
#include "Common/Log/Log.h"

#if RC_ENABLE_IMGUI
#include "imgui/imgui.h"
#endif

#include "Engine/Camera/CameraMath.h"
#include "Engine/Camera/CameraController.h"
#include "Engine/Graphics/PostProcess/PostProcess.h"
#include "ECS/TransformComponent.h"
#include "ECS/NativeScriptComponent.h"
#include "ECS/CameraComponent.h"
#include "ECS/PrimitiveMeshComponent.h"
#include "ECS/ColliderComponent.h"
#include "ECS/WaterComponent.h"
#include "ECS/LightComponent.h"
#include "Scene.h"
#include "Game/Framework/GameSession.h"
#include "Game/Framework/GameSettings.h"
#include "Game/Framework/StageProgress.h"
#include "Game/Framework/UnderwaterLook.h"
#include "Game/Framework/InkScreenFx.h"
#include "Game/Framework/ExposureFlash.h"
#include <algorithm>
#include <utility>
#include <cmath>
#include <cstdio>
#include <string>

namespace {
    bool IntersectSegmentAABB(const RC::Vector3& p0, const RC::Vector3& p1, const RC::Vector3& min, const RC::Vector3& max) {
        RC::Vector3 d = { p1.x - p0.x, p1.y - p0.y, p1.z - p0.z };
        float tmin = 0.0f;
        float tmax = 1.0f;
        
        // X axis
        if (std::abs(d.x) < 0.00001f) {
            if (p0.x < min.x || p0.x > max.x) return false;
        } else {
            float ood = 1.0f / d.x;
            float t1 = (min.x - p0.x) * ood;
            float t2 = (max.x - p0.x) * ood;
            if (t1 > t2) std::swap(t1, t2);
            if (t1 > tmin) tmin = t1;
            if (t2 < tmax) tmax = t2;
            if (tmin > tmax) return false;
        }
        // Y axis
        if (std::abs(d.y) < 0.00001f) {
            if (p0.y < min.y || p0.y > max.y) return false;
        } else {
            float ood = 1.0f / d.y;
            float t1 = (min.y - p0.y) * ood;
            float t2 = (max.y - p0.y) * ood;
            if (t1 > t2) std::swap(t1, t2);
            if (t1 > tmin) tmin = t1;
            if (t2 < tmax) tmax = t2;
            if (tmin > tmax) return false;
        }
        // Z axis
        if (std::abs(d.z) < 0.00001f) {
            if (p0.z < min.z || p0.z > max.z) return false;
        } else {
            float ood = 1.0f / d.z;
            float t1 = (min.z - p0.z) * ood;
            float t2 = (max.z - p0.z) * ood;
            if (t1 > t2) std::swap(t1, t2);
            if (t1 > tmin) tmin = t1;
            if (t2 < tmax) tmax = t2;
            if (tmin > tmax) return false;
        }
        return true;
    }

    bool RaycastTerrain(Scene* scene, const RC::Vector3& start, const RC::Vector3& end) {
        if (!scene) return false;
        for (auto& e : scene->GetEntities()) {
            if (e->GetTagInt("is_terrain", 0) == 1) {
                auto* col = e->GetComponent<ColliderComponent>();
                auto* tr = e->GetComponent<TransformComponent>();
                if (col && tr && col->IsEnabled() && col->shape == ColliderComponent::Shape::AABB) {
                    RC::Vector3 scaledSize = {
                        std::abs(col->size.x * tr->scale.x),
                        std::abs(col->size.y * tr->scale.y),
                        std::abs(col->size.z * tr->scale.z)
                    };
                    RC::Vector3 halfSize = { scaledSize.x * 0.5f, scaledSize.y * 0.5f, scaledSize.z * 0.5f };
                    RC::Vector3 center = {
                        tr->position.x + col->center.x * tr->scale.x,
                        tr->position.y + col->center.y * tr->scale.y,
                        tr->position.z + col->center.z * tr->scale.z
                    };
                    RC::Vector3 minPos = { center.x - halfSize.x, center.y - halfSize.y, center.z - halfSize.z };
                    RC::Vector3 maxPos = { center.x + halfSize.x, center.y + halfSize.y, center.z + halfSize.z };
                    
                    if (IntersectSegmentAABB(start, end, minPos, maxPos)) {
                        return true;
                    }
                }
            }
        }
        return false;
    }
}

/// @brief レールシューティングにおける照準（レティクル）操作を担うスクリプト
/// @details 照準は画面中央に固定し、マウスの移動量（と右スティック）でカメラの向きを変える
///          FPS 型の操作。カメラ（このエンティティの TransformComponent）の rotation は
///          「レールが与える基準の向き ＋ プレイヤーの視点オフセット（yaw / pitch）」で毎フレーム決める。
///          マウスカーソルは Input のカーソルロックで画面中央に固定・非表示にする。
///          ポーズ中（deltaTime = 0 で来るフレーム）はロックを外すので、ポーズメニューを
///          マウスで操作できる。閉じると次のフレームで再びロックする。
///          プレイヤーが変える感度・上下反転は GameSettings（ポーズメニューの設定画面）から読む。
class RailShooterController : public ScriptableEntity {
public:
    /// @brief レティクルのスクリーン座標。常に画面中央（毎フレーム解像度から求める）
    RC::Vector2 cursorPosition = { 640.0f, 360.0f };

    // 感度調整用パラメータ（プレイヤーが触る倍率・反転は GameSettings 側。ここは基準値）
    float lookSensitivity = 0.0025f;    ///< マウス 1 カウントあたりの回転量（rad）。GameSettings の倍率を掛けて使う
    float controllerLookSpeed = 2.5f;   ///< 右スティックを最大まで倒したときの回転速度（rad/s）。同上
    float deadzone = 0.2f;              ///< アナログスティックのデッドゾーン（20%）
    bool lockCursor = true;             ///< マウスカーソルを画面中央に固定・非表示にする

    // 視点の可動範囲（レールシューターなので真後ろまでは振り向けない）
    float maxYawDeg = 80.0f;    ///< 左右の最大角（度）
    float maxPitchDeg = 60.0f;  ///< 上下の最大角（度）
    
    // 描画設定（仮）
    RC::Vector4 reticleColor = { 1.0f, 0.0f, 0.0f, 1.0f };
    float reticleSize = 10.0f;

    // 射撃パラメータ
    float fireCooldown = 0.2f;
    float currentCooldown = 0.0f;
    float bulletSpeed = 40.0f;
    float bulletLifetime = 3.0f;

    // 演出用パラメータ
    float currentRecoil = 0.0f; // 射撃時のレティクル拡大量
    float recoilMax = 15.0f;    // 射撃時の最大反動量
    float recoilRecoverySpeed = 10.0f; // 反動の戻る速度

    float currentCameraShake = 0.0f; // カメラシェイクの現在強度
    float shakeMax = 0.5f;           // カメラシェイクの最大強度
    float shakeRecoverySpeed = 5.0f; // カメラシェイクの減衰速度

    // プレイヤーステータス
    int hp = 5;
    int maxHp = 5;
    float invincibleTimer = 0.0f;
    float invincibleDuration = 1.0f;
    bool isDead = false;
    int score = 0;

    enum class WeaponType { Normal, Spread, Heavy };
    WeaponType currentWeapon = WeaponType::Normal;

    // 敵全体（ボス）情報
    int totalEnemyHp = 0;
    int totalEnemyMaxHp = 0;
    bool isGameCleared = false;

    // 水中状態フラグ
    bool isUnderwater = false;
    float transitionTimer = 0.0f;
    float transitionSpeed = 2.0f; // 0.5秒で完全に切り替わる

    /// @brief 水面を出入りした瞬間の露出フラッシュ（着水で一瞬暗く、浮上で一瞬白く）
    /// @details 既定値は WaterCameraFx（Title / Select / Result の飛び込み・浮上）と同じ。
    ///          JSON の "waterFlash" キーで上書きできる。EV を 0 にすると出ない。
    float diveFlashEv = ExposureFlashDefaults::kDiveEv;
    float diveFlashDuration = ExposureFlashDefaults::kDiveDuration;
    float emergeFlashEv = ExposureFlashDefaults::kEmergeEv;
    float emergeFlashDuration = ExposureFlashDefaults::kEmergeDuration;
    ExposureFlash waterFlash_;

    /// @brief 水中の見た目（フォグ・光）。深さに応じて浅い色 → 深海色へ変わる。
    /// @details Title の飛び込み・Game 開始時の浮上（DeepRiseIntroScript）と同じ定義。
    ///          JSON の "underwater" キーで上書きできる。
    UnderwaterLook::Params look;

    /// @brief タコの墨で視界が塞がる演出（InkOverlay）。JSON の "ink" キーで上書きできる。
    /// @details 墨玉（InkBlobScript）が自分に `pending_ink` タグを積むので、それを拾って貼る。
    InkScreenFx ink;
    float waterHeight = 0.0f; ///< 水面の高さ（OnCreate でシーンの WaterComponent から拾う）
    RC::Vector3 sunDir_ = {-0.5f, -0.8f, 0.5f}; ///< 光の向き（OnCreate でシーンの DirectionalLight から拾う）
    bool hasSunDir_ = false;

    // HUD テキスト（スコア / GAME OVER）。PauseMenu と同じフォント・同じ 1280x720 基準レイアウトで、
    // ゲームのレンダーターゲット内へ描く（ImGui だとエディタのウィンドウ座標に出てしまい、
    // ビューポートの外へはみ出す）。
    static constexpr float kDesignW = 1280.0f;
    static constexpr float kDesignH = 720.0f;
    static constexpr float kScorePx = 30.0f;     ///< スコアの文字サイズ（1280x720 基準）
    static constexpr float kGameOverPx = 72.0f;  ///< GAME OVER の文字サイズ（1280x720 基準）
    std::string hudFontPath = "Resources/fonts/Kiwi_Maru/KiwiMaru-Medium.ttf";
    int scoreFont_ = -1;
    int gameOverFont_ = -1;
    float hudFontScale_ = 1.0f; ///< フォントをロードした時点の実解像度 / 基準解像度

    nlohmann::json Serialize() override {
        return {
            {"underwater", look.ToJson()},
            {"ink", ink.ToJson()},
            {"waterFlash", {
                {"diveEv", diveFlashEv},
                {"diveDuration", diveFlashDuration},
                {"emergeEv", emergeFlashEv},
                {"emergeDuration", emergeFlashDuration},
            }},
            {"look", {
                {"lookSensitivity", lookSensitivity},
                {"controllerLookSpeed", controllerLookSpeed},
                {"deadzone", deadzone},
                {"lockCursor", lockCursor},
                {"maxYawDeg", maxYawDeg},
                {"maxPitchDeg", maxPitchDeg},
            }},
        };
    }

    void Deserialize(const nlohmann::json& j) override {
        if (j.contains("underwater")) look.FromJson(j["underwater"]);
        if (j.contains("ink")) ink.FromJson(j["ink"]);
        if (j.contains("waterFlash") && j["waterFlash"].is_object()) {
            const auto& f = j["waterFlash"];
            auto readF = [&](const char* key, float& out) {
                if (f.contains(key) && f[key].is_number()) out = f[key].get<float>();
            };
            readF("diveEv", diveFlashEv);
            readF("diveDuration", diveFlashDuration);
            readF("emergeEv", emergeFlashEv);
            readF("emergeDuration", emergeFlashDuration);
        }
        if (j.contains("look")) {
            const auto& l = j["look"];
            if (l.contains("lookSensitivity"))     lookSensitivity     = l["lookSensitivity"].get<float>();
            if (l.contains("controllerLookSpeed")) controllerLookSpeed = l["controllerLookSpeed"].get<float>();
            if (l.contains("deadzone"))            deadzone            = l["deadzone"].get<float>();
            if (l.contains("lockCursor"))          lockCursor          = l["lockCursor"].get<bool>();
            if (l.contains("maxYawDeg"))           maxYawDeg           = l["maxYawDeg"].get<float>();
            if (l.contains("maxPitchDeg"))         maxPitchDeg         = l["maxPitchDeg"].get<float>();
        }
    }

    // 水滴演出（ Screen Droplets ）タイマーと設定
    float dropletTimer = 0.0f;
    float dropletDuration = 2.2f; // 着水・出水時から水滴が残りつづける秒数
    float dropletMaxIntensity = 1.0f;
    float dropletSpeed = 1.2f;
    float dropletDistortion = 0.06f;
    float dropletScale = 1.5f;

    // ダイブ時のスピードブラー演出 (RadialBlur) タイマー
    float radialBlurTimer = 0.0f;
    float radialBlurDuration = 0.45f;
    float radialBlurMaxWidth = 0.035f;

private:
    uint64_t bulletsFolderGuid_ = 0;

    // --- 視点操作の状態 ---
    /// @brief レール（シーン）が与えるカメラの基準の向き。視点オフセットはこれに足す
    /// @details 開始演出（DeepRiseIntroScript）が rotation を書き換えるので、演出が終わって
    ///          最初に操作を受け付けるフレームで拾う（演出は終了時に元の向きへ戻す）。
    RC::Vector3 baseRotation_ = { 0.0f, 0.0f, 0.0f };
    bool lookInitialized_ = false; ///< baseRotation_ を拾ったか
    float lookYaw_ = 0.0f;         ///< 基準からの左右オフセット（rad）
    float lookPitch_ = 0.0f;       ///< 基準からの上下オフセット（rad。正で下向き）
    /// @brief 前フレームが停止（ポーズ・一時停止）だったか。再開した瞬間の検出用
    bool wasInactive_ = false;
    /// @brief 再開直後、マウスボタンが一度離されるまで射撃を止める
    /// @details ポーズメニューの「つづける」をクリックした指がまだ離れていないフレームで
    ///          弾が出るのを防ぐ。
    bool suppressFireUntilRelease_ = false;

    /// @brief マウス入力をゲーム操作として扱ってよいか
    /// @details ロック中はカーソルが Viewport 中央に固定されるので常に true。
    ///          ロックを使わない設定のときは、エディタの Viewport 上にあるときだけ受け付ける
    ///          （インスペクタをドラッグして視点が回るのを防ぐ）。
    static bool IsMouseForGame(Input* input) {
        if (!input) return false;
        if (input->IsCursorLocked()) return true;
#if RC_ENABLE_IMGUI
        return input->IsViewportHovered();
#else
        return true;
#endif
    }

    /// @brief カーソルロックの ON/OFF を今のゲーム状態から決めて Input へ反映する
    /// @param active 操作を受け付けている（再生中でポーズも一時停止もしていない）か
    void UpdateCursorLock(Input* input, bool active) {
        if (!input) return;
        bool want = lockCursor && active;
        // F1 のデバッグカメラ中は右ドラッグで飛び回りたいのでロックしない
        if (SceneContext* ctx = GetSceneContext()) {
            if (ctx->camera && ctx->camera->IsUsingDebug()) want = false;
        }
        input->SetCursorLocked(want);
    }

    /// @brief 視点オフセットを基準の向きに足してカメラ（自エンティティ）の rotation に書く
    void ApplyLookToTransform() {
        auto* tr = GetComponent<TransformComponent>();
        if (!tr || !lookInitialized_) return;
        tr->rotation = {
            baseRotation_.x + lookPitch_,
            baseRotation_.y + lookYaw_,
            baseRotation_.z
        };
    }

    uint64_t GetBulletsFolder(Scene* scene) {
        if (bulletsFolderGuid_ != 0) return bulletsFolderGuid_;
        for (auto& e : scene->GetEntities()) {
            if (e->GetName() == "PlayerBullets" && e->IsFolder()) {
                bulletsFolderGuid_ = e->Guid();
                return bulletsFolderGuid_;
            }
        }
        auto folder = scene->CreateEntity("PlayerBullets");
        folder->SetIsFolder(true);
        bulletsFolderGuid_ = folder->Guid();
        return bulletsFolderGuid_;
    }

protected:
    void OnCreate() override {
        if (Entity* self = GetEntity()) {
            self->SetTag("is_player", 1);
        }
        // 感度・反転・音量の保存値（無ければ既定値）。最初の 1 フレームから反映させる
        GameSettings::Get().EnsureLoaded();

        // HUD フォント（PauseMenu と同じく、基準解像度のサイズ × 実解像度スケールでロード）
        {
            float w = kDesignW, h = kDesignH;
            auto& rc = RC::GetRenderContext();
            if (rc.Ctx() && rc.Ctx()->app && rc.Ctx()->app->width > 0 && rc.Ctx()->app->height > 0) {
                w = static_cast<float>(rc.Ctx()->app->width);
                h = static_cast<float>(rc.Ctx()->app->height);
            }
            hudFontScale_ = (std::min)(w / kDesignW, h / kDesignH);
            if (hudFontScale_ <= 0.0f) hudFontScale_ = 1.0f;
            scoreFont_ = RC::LoadFont(hudFontPath, std::round(kScorePx * hudFontScale_), 1024);
            gameOverFont_ = RC::LoadFont(hudFontPath, std::round(kGameOverPx * hudFontScale_), 1024);
            if (scoreFont_ < 0 || gameOverFont_ < 0) {
                Log::Print("[RailShooterController] failed to load HUD font: " + hudFontPath);
            }
        }

        if (auto* postProcess = RC::GetRenderContext().GetPostProcess()) {
            // 推奨3: 全体的な視認性向上のための深度ベースアウトラインを常時有効化
            postProcess->AddEffect(PostEffectType::DepthBasedOutline);
            float outlineColor[4] = {0.04f, 0.08f, 0.16f, 0.85f};
            postProcess->SetOutlineColor(outlineColor);
            postProcess->SetOutlineThickness(1.0f);
        }

        // 水面の高さはシーンの WaterComponent から拾う（無ければ 0）
        // 光柱の向きはシーンの DirectionalLight から拾う（無ければ PostProcess の既定）
        waterHeight = 0.0f;
        if (Scene* scene = GetScene()) {
            for (auto& e : scene->GetEntities()) {
                if (!e) continue;
                if (e->GetComponent<WaterComponent>()) {
                    if (auto* wtr = e->GetComponent<TransformComponent>()) waterHeight = wtr->position.y;
                }
                if (auto* dl = e->GetComponent<DirectionalLightComponent>()) {
                    if (dl->visible) { sunDir_ = dl->direction; hasSunDir_ = true; }
                }
            }
        }

        // 最初から水中に置かれている場合（DeepRiseIntroScript が深海から始めるとき）は、
        // 着水の演出（ラジアルブラー・水滴）を出さずに、水中エフェクトだけ静かに積んで
        // 「ずっと水中にいた」状態から始める。ここでやらないと最初の Update で
        // 「水上 → 水中」と誤検出して、暗いはずの画面が一瞬明るく抜ける。
        if (auto* tr = GetComponent<TransformComponent>()) {
            isUnderwater = (tr->position.y < waterHeight);
            transitionTimer = isUnderwater ? 1.0f : 0.0f;
            if (Entity* self = GetEntity()) {
                self->SetTag("is_underwater", isUnderwater ? 1 : 0);
            }
            if (auto* postProcess = RC::GetRenderContext().GetPostProcess()) {
                UnderwaterLook::SetupLight(postProcess, look, waterHeight, hasSunDir_ ? &sunDir_ : nullptr);
                if (isUnderwater) {
                    UnderwaterLook::AddStack(postProcess);
                    UnderwaterLook::SetLerp(postProcess, 1.0f);
                    UnderwaterLook::ApplyFogByDepth(postProcess, look, waterHeight - tr->position.y);
                    Log::Print("[RailShooterController] starts underwater (silent)");
                }
            }
        }
    }

    void OnUpdate(float deltaTime) override {
        auto* input = Input::GetInstance();

        // 視野角（設定画面の値）。CameraComponent::fovY はラジアン。
        // SyncMainCamera はポーズ中も毎フレーム射影行列を作り直すので、ポーズ中に
        // 設定画面で動かした値もその場で見える（dt の判定より前に置く）。
        // 編集モード（停止中）では書かない。書くとシーン JSON のカメラ設定を
        // プレイヤーの設定で上書き保存してしまう。
        if (SceneContext* ctx = GetSceneContext(); ctx && ctx->isPlaying()) {
            if (auto* cam = GetComponent<CameraComponent>()) {
                cam->fovY = GameSettings::Get().FovRadians();
            }
        }

        if (deltaTime <= 0.0f) {
            // ポーズ（ESC メニュー）・エディタの一時停止・停止中は処理しない。
            // メニューやエディタをマウスで操作できるように、カーソルのロックだけは外しておく
            // （再開した次のフレームで自動的に戻る）
            UpdateCursorLock(input, false);
            wasInactive_ = true;
            return;
        }
        if (!input) return;

        // === カーソルロック ===
        UpdateCursorLock(input, true);

        // 再開した瞬間：「つづける」を押した指が離れるまで射撃しない
        if (wasInactive_) {
            wasInactive_ = false;
            suppressFireUntilRelease_ = true;
        }
        if (suppressFireUntilRelease_ && !input->IsMousePressed(0)) {
            suppressFireUntilRelease_ = false;
        }

        // 開始演出（DeepRiseIntroScript の浮上）中は、水中判定と HUD 以外を止める
        const bool introPlaying = (GetEntity() && GetEntity()->GetTagInt("intro_playing", 0) == 1);

        // === 無敵タイマー ===
        if (invincibleTimer > 0.0f) {
            invincibleTimer -= deltaTime;
        }

        // === 水中判定とトランジション ===

        float depth = 0.0f; // 水面からの深さ（m）。水上なら 0
        if (auto* tr = GetComponent<TransformComponent>()) {
            depth = (std::max)(waterHeight - tr->position.y, 0.0f);
            bool currentUnderwater = (tr->position.y < waterHeight);
            if (currentUnderwater != isUnderwater) {
                isUnderwater = currentUnderwater;
                if (Entity* self = GetEntity()) {
                    self->SetTag("is_underwater", isUnderwater ? 1 : 0);
                }
                if (isUnderwater) {
                    Log::Print("[Event] Transition to Underwater (Dive)");
                    if (auto* postProcess = RC::GetRenderContext().GetPostProcess()) {
                        // 追加順 = 適用順。Underwater が最後に来るようにする。
                        // Underwater は UV を歪めるため、それより後に Caustics や
                        // LightShaft を乗せるとジオメトリと模様の位置がズレる。
                        // 先に光を乗せてから Underwater でまとめて歪めることで
                        // 位置が一致し、かつ模様自体も水で揺らぐようになる。
                        // （LightShaft → Caustics → Underwater → Vignette。Vignette は
                        //   水中での「密閉感・深海感・水圧」の演出）
                        UnderwaterLook::SetupLight(postProcess, look, waterHeight, hasSunDir_ ? &sunDir_ : nullptr);
                        UnderwaterLook::AddStack(postProcess);

                        // 推奨1: 水中突入（ダイブ）時の強烈な勢い・スピード感演出としての RadialBlur スタック
                        postProcess->AddEffect(PostEffectType::RadialBlur);
                        radialBlurTimer = radialBlurDuration;
                        postProcess->SetRadialBlurWidth(radialBlurMaxWidth);

                        // 遷移時（Dive/水上→水中）に気泡・水滴が「上に向かって」昇って消えていく演出
                        postProcess->AddEffect(PostEffectType::ScreenDroplets);
                        dropletTimer = dropletDuration;
                        dropletSpeed = -1.3f; // 負のスピード値で上方向に昇るようにスクロール
                        postProcess->SetScreenDropletsSpeed(dropletSpeed);

                        // 着水の衝撃：一瞬だけ暗く沈む
                        waterFlash_.Trigger(postProcess, diveFlashEv, diveFlashDuration);
                    }
                } else {
                    Log::Print("[Event] Transition to Surface (Emerge)");
                    if (auto* postProcess = RC::GetRenderContext().GetPostProcess()) {
                        // 出水（浮上）時も一時的にスピードブラーを発生させ離脱の爽快な衝撃を演出
                        postProcess->AddEffect(PostEffectType::RadialBlur);
                        radialBlurTimer = radialBlurDuration * 0.7f;
                        postProcess->SetRadialBlurWidth(radialBlurMaxWidth * 0.7f);

                        // 出水時（Emerge/水中→水上）は水滴が重力に従って「下に向かって」したたり落ちて消える演出
                        postProcess->AddEffect(PostEffectType::ScreenDroplets);
                        dropletTimer = dropletDuration;
                        dropletSpeed = 1.3f; // 正のスピード値で下方向に滴る
                        postProcess->SetScreenDropletsSpeed(dropletSpeed);

                        // 水面を抜けた解放感：一瞬だけ白く飛ぶ
                        waterFlash_.Trigger(postProcess, emergeFlashEv, emergeFlashDuration);
                    }
                }
            }
        }

        // トランジションの更新
        float targetTransition = isUnderwater ? 1.0f : 0.0f;
        if (transitionTimer != targetTransition) {
            if (transitionTimer < targetTransition) {
                transitionTimer += transitionSpeed * deltaTime;
                if (transitionTimer > targetTransition) transitionTimer = targetTransition;
            } else {
                transitionTimer -= transitionSpeed * deltaTime;
                if (transitionTimer < targetTransition) transitionTimer = targetTransition;
            }

            if (auto* postProcess = RC::GetRenderContext().GetPostProcess()) {
                // 水上⇔水中は S 字カーブでブレンド（Underwater / Caustics / LightShaft を同じ値で）
                UnderwaterLook::SetLerp(postProcess, UnderwaterLook::SmoothStep01(transitionTimer));

                // 完全に水上に戻りきったらエフェクト自体をRemoveする
                if (transitionTimer == 0.0f && !isUnderwater) {
                    UnderwaterLook::RemoveStack(postProcess);
                }
            }
        }

        // 水中にいる（または出入りの途中の）あいだは、深さに応じてフォグの色と距離を変える。
        // 水面直下は青く遠くまで見え、深くなるほど暗く近くしか見えない（深海）。
        if (isUnderwater || transitionTimer > 0.0f) {
            if (auto* postProcess = RC::GetRenderContext().GetPostProcess()) {
                UnderwaterLook::ApplyFogByDepth(postProcess, look, depth);
            }
        }

        // === 露出フラッシュの減衰更新 ===
        waterFlash_.Update(RC::GetRenderContext().GetPostProcess(), deltaTime);

        // === RadialBlur タイマーの減衰更新 ===
        if (radialBlurTimer > 0.0f) {
            radialBlurTimer -= deltaTime;
            if (auto* postProcess = RC::GetRenderContext().GetPostProcess()) {
                if (radialBlurTimer <= 0.0f) {
                    radialBlurTimer = 0.0f;
                    postProcess->RemoveEffect(PostEffectType::RadialBlur);
                } else {
                    float progress = radialBlurTimer / radialBlurDuration;
                    postProcess->SetRadialBlurWidth(radialBlurMaxWidth * progress * progress);
                }
            }
        }

        // === レンズ水滴タイマーの減衰更新 ===
        if (dropletTimer > 0.0f) {
            dropletTimer -= deltaTime;
            if (auto* postProcess = RC::GetRenderContext().GetPostProcess()) {
                if (dropletTimer <= 0.0f) {
                    dropletTimer = 0.0f;
                    postProcess->RemoveEffect(PostEffectType::ScreenDroplets);
                } else {
                    float progress = dropletTimer / dropletDuration;
                    // イーズアウト（滑らかで自然な乾燥と減衰曲線）
                    float currentIntensity = progress * progress * dropletMaxIntensity;
                    postProcess->SetScreenDropletsIntensity(currentIntensity);
                    postProcess->SetScreenDropletsSpeed(dropletSpeed);
                    postProcess->SetScreenDropletsDistortion(dropletDistortion);
                    postProcess->SetScreenDropletsScale(dropletScale);
                }
            }
        }

        // === ダメージ・スコア処理 ===
        Entity* self = GetEntity();
        if (self) {
            int dmg = self->GetTagInt("pending_damage", 0);
            if (dmg > 0) {
                self->ClearTag("pending_damage");
                TakeDamage(dmg);
            }
            int scoreAdd = self->GetTagInt("score_add", 0);
            if (scoreAdd > 0) {
                self->ClearTag("score_add");
                score += scoreAdd;
            }
            // タコの墨（強さ×100 の整数。1 フレームに複数当たったら 1 発ずつ貼る）
            int inkHit = self->GetTagInt("pending_ink", 0);
            if (inkHit > 0) {
                self->ClearTag("pending_ink");
                if (!isDead) {
                    while (inkHit > 0) {
                        const int one = (std::min)(inkHit, 100);
                        ink.AddHit(static_cast<float>(one) / 100.0f);
                        inkHit -= one;
                    }
                    currentCameraShake = (std::max)(currentCameraShake, shakeMax * 0.6f);
                }
            }
        }
        ink.Update(RC::GetRenderContext().GetPostProcess(), deltaTime, isUnderwater);

        // Result シーンから読めるように、現在の成績を GameSession へ写す。
        // score / hp はこのスクリプトのメンバなので、シーンを抜けると失われるため。
        GameSession::Get().SetScore(score);
        GameSession::Get().SetPlayerHp(hp, maxHp);

        // 敵の合計HPの集計、ダメージ処理、およびクリア判定
        totalEnemyHp = 0;
        totalEnemyMaxHp = 0;
        if (Scene* scene = GetScene()) {
            for (auto& e : scene->GetEntities()) {
                if (e->GetTagInt("is_enemy", 0) == 1) {
                    // タグが未設定なら初期化
                    int ehp = e->GetTagInt("current_hp", -1);
                    int eMaxHp = e->GetTagInt("max_hp", -1);
                    if (ehp == -1) { ehp = 30; e->SetTag("current_hp", 30); }
                    if (eMaxHp == -1) { eMaxHp = 30; e->SetTag("max_hp", 30); }

                    totalEnemyHp += ehp;
                    totalEnemyMaxHp += eMaxHp;
                }
                // クリア表示はレールの終点到達で出す。
                // 以前は「敵の合計 HP が 0」で判定していたが、EnemyBaseScript は撃破時に
                // is_enemy を外すため、最後の 1 体が死んだ瞬間に totalEnemyMaxHp も 0 に
                // 落ちる。つまり totalEnemyMaxHp > 0 && totalEnemyHp <= 0 は一度も
                // 成立せず、この表示はこれまで一切出ていなかった。
                if (e->HasTag("rail_finished")) {
                    isGameCleared = true;
                }
            }
        }

        // 開始演出中は操作を受け付けない（浮上が終わって GAME START が出てから）
        if (introPlaying) return;

        // Weapon Switching
        long wheel = input->GetMouseZ();
        if (wheel > 0) {
            int w = static_cast<int>(currentWeapon) - 1;
            if (w < 0) w = 2;
            currentWeapon = static_cast<WeaponType>(w);
        } else if (wheel < 0) {
            int w = static_cast<int>(currentWeapon) + 1;
            if (w > 2) w = 0;
            currentWeapon = static_cast<WeaponType>(w);
        }

        if (input->IsXInputConnected()) {
            if (input->IsXInputButtonTrigger(XINPUT_GAMEPAD_LEFT_SHOULDER)) {
                int w = static_cast<int>(currentWeapon) - 1;
                if (w < 0) w = 2;
                currentWeapon = static_cast<WeaponType>(w);
            }
            if (input->IsXInputButtonTrigger(XINPUT_GAMEPAD_RIGHT_SHOULDER)) {
                int w = static_cast<int>(currentWeapon) + 1;
                if (w > 2) w = 0;
                currentWeapon = static_cast<WeaponType>(w);
            }
        }

        // === 視点操作（画面中央固定エイム） ===

        // 1. 基準の向きを拾う（演出が終わって最初に操作できるフレーム）
        if (!lookInitialized_) {
            if (auto* tr = GetComponent<TransformComponent>()) {
                baseRotation_ = tr->rotation;
                lookInitialized_ = true;
            }
        }

        // デッドゾーン計算用の定数（GetXInputThumbRX/RYは-32768～32767）
        const float MAX_THUMB = 32767.0f;

        // 2. コントローラー（右スティック）入力の取得
        //    スティック上（+Y）で見上げたいので、pitch（正で下向き）に合わせて反転する
        float stickX = input->GetXInputThumbRX() / MAX_THUMB;
        float stickY = -input->GetXInputThumbRY() / MAX_THUMB;
        if (std::abs(stickX) < deadzone) stickX = 0.0f;
        if (std::abs(stickY) < deadzone) stickY = 0.0f;

        // 3. マウス入力（移動量）の取得。エディタ操作中は視点に使わない
        const bool mouseForGame = IsMouseForGame(input);
        float deltaMouseX = mouseForGame ? static_cast<float>(input->GetMouseX()) : 0.0f;
        float deltaMouseY = mouseForGame ? static_cast<float>(input->GetMouseY()) : 0.0f;

        // 4. 視点オフセットの更新
        // 決着後に演出スクリプト（ClearAscendScript / DeathSinkScript）がカメラを動かしている間は
        // 向きを書かない（"result_transition_owner" タグが目印）
        const bool cameraOwnedByOutro = (GetEntity() && GetEntity()->GetTagInt("result_transition_owner", 0) == 1);
        if (!isDead && !cameraOwnedByOutro) {
            // プレイヤーが設定画面で変えた倍率・反転（シーンをまたいで保持される）
            const GameSettings& settings = GameSettings::Get();
            const float mouseGain = lookSensitivity * settings.mouseSensitivity;
            const float stickGain = controllerLookSpeed * settings.controllerSensitivity * deltaTime;

            // マウスは移動量そのもの（フレームレート非依存）、スティックは倒し続けるので deltaTime を掛ける
            float dYaw   = deltaMouseX * mouseGain + stickX * stickGain;
            float dPitch = deltaMouseY * mouseGain + stickY * stickGain;
            if (settings.invertY) dPitch = -dPitch;

            constexpr float kDegToRad = 3.14159265358979f / 180.0f;
            const float maxYaw   = maxYawDeg * kDegToRad;
            const float maxPitch = maxPitchDeg * kDegToRad;
            lookYaw_   = RC::Clamp(lookYaw_ + dYaw, -maxYaw, maxYaw);
            lookPitch_ = RC::Clamp(lookPitch_ + dPitch, -maxPitch, maxPitch);
            // 力尽きたあとは向きを書かない（DeathSinkScript が沈みながら見上げさせるため）
            ApplyLookToTransform();
        }

        // 5. レティクルは常に画面中央
        auto& ctx = RC::GetRenderContext();
        float screenW = 1280.0f;
        float screenH = 720.0f;
        if (ctx.Ctx() && ctx.Ctx()->app) {
            screenW = static_cast<float>(ctx.Ctx()->app->width);
            screenH = static_cast<float>(ctx.Ctx()->app->height);
        }
        cursorPosition = { screenW * 0.5f, screenH * 0.5f };

        // 6. 射撃とクールダウン処理
        if (!isDead) {
            currentCooldown -= deltaTime;

            bool isFirePressed = false;
            // エディタ操作中のクリック、ポーズを閉じたクリックの残りでは撃たない
            if (mouseForGame && !suppressFireUntilRelease_) {
#if RC_ENABLE_IMGUI
                // ImGuiがマウスをキャプチャしていても、Viewport上なら射撃を許可する
                if (!ImGui::GetIO().WantCaptureMouse || input->IsViewportHovered() || input->IsCursorLocked()) {
                    isFirePressed = input->IsMousePressed(0);
                }
#else
                isFirePressed = input->IsMousePressed(0);
#endif
            }
            if (input->GetXInputRightTrigger() > 128) {
                isFirePressed = true;
            }

            if (isFirePressed && currentCooldown <= 0.0f) {
                if (currentWeapon == WeaponType::Heavy) currentCooldown = fireCooldown * 2.0f;
                else currentCooldown = fireCooldown;
                FireBullet();
            }
        }

        // 7. 演出（反動とシェイク）の更新
        if (currentRecoil > 0.0f) {
            currentRecoil -= recoilMax * recoilRecoverySpeed * deltaTime;
            if (currentRecoil < 0.0f) currentRecoil = 0.0f;
        }

        if (currentCameraShake > 0.0f) {
            currentCameraShake -= shakeMax * shakeRecoverySpeed * deltaTime;
            if (currentCameraShake < 0.0f) currentCameraShake = 0.0f;
        }

        // シーン内のカメラを探してShakeOffsetを適用
        if (Scene* scene = GetScene()) {
            for (auto& e : scene->GetEntities()) {
                if (auto* cam = e->GetComponent<CameraComponent>()) {
                    if (currentCameraShake > 0.0f) {
                        float sx = ((rand() % 100) / 50.0f - 1.0f) * currentCameraShake;
                        float sy = ((rand() % 100) / 50.0f - 1.0f) * currentCameraShake;
                        float sz = ((rand() % 100) / 50.0f - 1.0f) * currentCameraShake;
                        cam->shakeOffset = {sx, sy, sz};
                    } else {
                        cam->shakeOffset = {0.0f, 0.0f, 0.0f};
                    }
                    break;
                }
            }
        }
    }
    
    void FireBullet() {
        auto& ctx = RC::GetRenderContext();
        float screenW = 1280.0f;
        float screenH = 720.0f;
        if (ctx.Ctx() && ctx.Ctx()->app) {
            screenW = static_cast<float>(ctx.Ctx()->app->width);
            screenH = static_cast<float>(ctx.Ctx()->app->height);
        }

        int numBullets = (currentWeapon == WeaponType::Spread) ? 3 : 1;

        std::vector<std::shared_ptr<Entity>> inactiveBullets;
        if (Scene* scene = GetScene()) {
            for (auto& e : scene->GetEntities()) {
                if (e->GetName() == "PlayerBullet" && !e->IsActive() && !e->IsPendingDestroy()) {
                    inactiveBullets.push_back(e);
                    if (inactiveBullets.size() >= numBullets) break;
                }
            }
        }

        for (int i = 0; i < numBullets; ++i) {
            RC::Vector2 targetPos = cursorPosition;
            if (currentWeapon == WeaponType::Spread) {
                targetPos.x += (i - 1) * 80.0f; // -80, 0, 80 pixels offset
            }

            // Raycast計算
            RC::Ray ray = RC::CameraMath::ScreenPointToRay(
                targetPos, 
                {screenW, screenH}, 
                ctx.View(), 
                ctx.Proj()
            );

            Scene* scene = GetScene();
            if (!scene) continue;

            // Create bullet entity (Pooling)
            std::shared_ptr<Entity> bullet = nullptr;
            bool isNew = false;
            if (i < inactiveBullets.size()) {
                bullet = inactiveBullets[i];
                bullet->SetActive(true);
                bullet->SetTag("reused", 1);
            } else {
                bullet = scene->CreateEntity("PlayerBullet");
                isNew = true;
            }

            auto* tr = bullet->GetComponent<TransformComponent>();
            if (!tr) tr = &bullet->AddComponent<TransformComponent>();
            tr->position = ray.origin;
            bullet->SetParentGuid(GetBulletsFolder(scene));
            float scale = 0.3f;
            if (currentWeapon == WeaponType::Heavy) scale = 0.6f;
            else if (currentWeapon == WeaponType::Spread) scale = 0.2f;
            tr->scale = { scale, scale, scale };
            
            // メッシュの追加（描画用）
            auto* pm = bullet->GetComponent<PrimitiveMeshComponent>();
            if (!pm) {
                pm = &bullet->AddComponent<PrimitiveMeshComponent>();
                pm->type = PrimitiveType::Sphere;
                pm->meshHandle = RC::GenerateSphere(1.0f);
            }
            // 水の質感で描く（以前は DataDrivenScene がエンティティ名で判定していた）
            pm->drawStyle = PrimitiveDrawStyle::Water;
            if (pm->meshHandle >= 0) {
                if (auto* mat = RC::GetPrimitiveMeshMaterialPtr(pm->meshHandle)) {
                    mat->color = { 0.2f, 0.6f, 1.0f, 0.85f };
                }
            }
            
            // パラメータをTagとして渡す
            bullet->SetTag("dir_x", static_cast<int>(ray.direction.x * 1000.0f));
            bullet->SetTag("dir_y", static_cast<int>(ray.direction.y * 1000.0f));
            bullet->SetTag("dir_z", static_cast<int>(ray.direction.z * 1000.0f));
            bullet->SetTag("bullet_speed", static_cast<int>(bulletSpeed * 10.0f)); 
            bullet->SetTag("bullet_lifetime", static_cast<int>(bulletLifetime * 10.0f));

            if (currentWeapon == WeaponType::Normal) bullet->SetTag("bullet_type", 0);
            else if (currentWeapon == WeaponType::Spread) bullet->SetTag("bullet_type", 1);
            else if (currentWeapon == WeaponType::Heavy) bullet->SetTag("bullet_type", 2);

            // Scriptのアタッチ
            auto* nsc = bullet->GetComponent<NativeScriptComponent>();
            if (!nsc) {
                nsc = &bullet->AddComponent<NativeScriptComponent>();
                nsc->AddScript("WaterBullet");
            }
            nsc->SetScene(scene);
            if (GetSceneContext()) nsc->SetSceneContext(GetSceneContext());

            if (isNew) {
                scene->InitDynamicEntityRuntime(*bullet);
            } else {
                // PrimitiveMeshのTransformを即座に同期（チラつき防止）
                if (pm && pm->meshHandle >= 0) {
                    if (auto* pmTr = RC::GetPrimitiveMeshTransformPtr(pm->meshHandle)) {
                        pmTr->translation = tr->position;
                    }
                }
            }
        }

        // 演出のトリガー
        currentRecoil = recoilMax;
        currentCameraShake = shakeMax;
    }

    void OnDestroy() override {
        // Result などカーソルで操作するシーンへ移るので、ロックと非表示を必ず戻す
        if (auto* input = Input::GetInstance()) {
            input->SetCursorLocked(false);
        }
        // 墨はポストエフェクトに残るので、シーンを抜けるときに必ず剥がす
        ink.Clear(RC::GetRenderContext().GetPostProcess());
        // 露出のオフセットも PostProcess に残るので戻す（自分で積んだ ColorGrade も外れる）
        waterFlash_.Clear(RC::GetRenderContext().GetPostProcess());
        if (scoreFont_ >= 0) { RC::UnloadFont(scoreFont_); scoreFont_ = -1; }
        if (gameOverFont_ >= 0) { RC::UnloadFont(gameOverFont_); gameOverFont_ = -1; }
    }

    /// @brief ポストプロセスの後に描く HUD テキスト（水中の歪み・ビネットが乗らないように）
    void OnOverlayRender() override {
        if (GetEntity() && GetEntity()->GetTagInt("intro_playing", 0) == 1) return;

        // PauseMenu と同じ 1280x720 基準レイアウト（レターボックス分をオフセット）
        float screenW = kDesignW, screenH = kDesignH;
        auto& rc = RC::GetRenderContext();
        if (rc.Ctx() && rc.Ctx()->app) {
            screenW = static_cast<float>(rc.Ctx()->app->width);
            screenH = static_cast<float>(rc.Ctx()->app->height);
        }
        float s = (std::min)(screenW / kDesignW, screenH / kDesignH);
        if (s <= 0.0f) s = 1.0f;
        const float ox = (screenW - kDesignW * s) * 0.5f;
        const float oy = (screenH - kDesignH * s) * 0.5f;
        const float fs = (hudFontScale_ > 0.0f) ? (s / hudFontScale_) : 1.0f;

        auto drawText = [&](int font, const std::string& text, float dx, float dy,
                            const RC::Vector4& color, TextAlign align) {
            if (font < 0 || text.empty()) return;
            const float x = ox + dx * s;
            const float y = oy + dy * s;
            const float sh = 2.0f * s;
            RC::DrawString(font, text, {x + sh, y + sh}, {0.0f, 0.02f, 0.05f, 0.6f * color.w}, fs, align);
            RC::DrawString(font, text, {x, y}, color, fs, align);
        };

        if (isDead) {
            const float lh = (gameOverFont_ >= 0) ? RC::GetFontLineHeight(gameOverFont_, fs) / s : kGameOverPx;
            drawText(gameOverFont_, "GAME OVER", kDesignW * 0.5f, kDesignH * 0.5f - lh * 0.5f,
                     {1.0f, 0.25f, 0.25f, 1.0f}, TextAlign::Center);
            return;
        }

        if (!isGameCleared) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "SCORE  %06d", score);
            drawText(scoreFont_, buf, 24.0f, 18.0f, {1.0f, 1.0f, 1.0f, 1.0f}, TextAlign::Left);
        } else {
            // 終点に着いたときの帯（OnRender の緑の帯。画面の 40%〜60%）に文字を載せる。
            //   1 行目: STAGE CLEAR（最終面は ALL CLEAR）
            //   2 行目: 第 N 面 ○○ 踏破 ／ スコア
            const StageProgress &prog = StageProgress::Get();
            const int stage = prog.Current();
            const bool finalStage = prog.IsFinalStage();
            const float titleH = (gameOverFont_ >= 0) ? RC::GetFontLineHeight(gameOverFont_, fs) / s : kGameOverPx;
            const float subH = (scoreFont_ >= 0) ? RC::GetFontLineHeight(scoreFont_, fs) / s : kScorePx;
            const float gap = 6.0f;
            const float top = kDesignH * 0.5f - (titleH + gap + subH) * 0.5f;
            drawText(gameOverFont_, finalStage ? "ALL CLEAR" : "STAGE CLEAR", kDesignW * 0.5f, top,
                     {1.0f, 0.97f, 0.85f, 1.0f}, TextAlign::Center);

            std::string sub;
            if (stage >= 0) {
                sub = "第" + std::to_string(stage + 1) + "面　" + StageProgress::Info(stage).title +
                      (finalStage ? "　全航路 踏破" : "　踏破");
            } else {
                sub = "航路 踏破";
            }
            char scoreBuf[32];
            std::snprintf(scoreBuf, sizeof(scoreBuf), "　　SCORE %06d", score);
            sub += scoreBuf;
            drawText(scoreFont_, sub, kDesignW * 0.5f, top + titleH + gap, {1.0f, 1.0f, 1.0f, 1.0f},
                     TextAlign::Center);
        }

    }

    void OnRender() override {
        // 開始演出（深海からの浮上）中は HUD を出さない
        if (GetEntity() && GetEntity()->GetTagInt("intro_playing", 0) == 1) return;

        // Primitive 2D描画を用いてレティクルを描画する
        RC::DrawCircle(cursorPosition, reticleSize + currentRecoil, reticleColor, kFill, 1.0f);
        
        // 中心点を描画（見やすくするため黒の四角形）
        RC::DrawBox({cursorPosition.x - 2.0f, cursorPosition.y - 2.0f}, 
                    {cursorPosition.x + 2.0f, cursorPosition.y + 2.0f}, 
                    {0.0f, 0.0f, 0.0f, 1.0f});

        auto& ctx = RC::GetRenderContext();
        float screenW = 1280.0f;
        float screenH = 720.0f;
        if (ctx.Ctx() && ctx.Ctx()->app) {
            screenW = static_cast<float>(ctx.Ctx()->app->width);
            screenH = static_cast<float>(ctx.Ctx()->app->height);
        }

        // Weapon Indicator（元の表示）
        float wX = 20.0f; float wY = screenH - 80.0f;
        float alphaN = (currentWeapon == WeaponType::Normal) ? 0.9f : 0.3f;
        RC::DrawBox({wX, wY}, {wX+20.0f, wY+20.0f}, {0.2f, 0.6f, 1.0f, alphaN});
        float alphaS = (currentWeapon == WeaponType::Spread) ? 0.9f : 0.3f;
        RC::DrawBox({wX+25.0f, wY}, {wX+45.0f, wY+20.0f}, {0.2f, 0.8f, 0.4f, alphaS});
        float alphaH = (currentWeapon == WeaponType::Heavy) ? 0.9f : 0.3f;
        RC::DrawBox({wX+50.0f, wY}, {wX+70.0f, wY+20.0f}, {0.8f, 0.3f, 0.1f, alphaH});

        // === Player HP Gauge ===
        // 武器表示の下。元のゲージより長く・太くして読みやすくする
        const float barX = 20.0f;
        const float barW = 300.0f;
        const float barH = 16.0f;
        const float barY = screenH - 20.0f - barH - 14.0f; // 下端から 34px（武器表示と 10px 空ける）

        bool drawPlayerUI = true;
        if (invincibleTimer > 0.0f) {
            if (static_cast<int>(invincibleTimer * 10.0f) % 2 == 0) {
                drawPlayerUI = false;
            }
        }

        if (drawPlayerUI) {
            // Background
            RC::DrawBox({ barX, barY }, { barX + barW, barY + barH }, { 0.3f, 0.05f, 0.05f, 0.8f });

            // Foreground
            float hpRatio = (maxHp > 0) ? static_cast<float>((std::max)(hp, 0)) / static_cast<float>(maxHp) : 0.0f;
            RC::Vector4 hpColor = { 1.0f - hpRatio, hpRatio, 0.1f, 0.9f };
            RC::DrawBox({ barX, barY }, { barX + barW * hpRatio, barY + barH }, hpColor);

            // Border
            RC::DrawBox({ barX, barY }, { barX + barW, barY + barH }, { 1.0f, 1.0f, 1.0f, 0.6f }, kWire);
        }

        // === 敵ごとの個別HPバーを頭上に描画 ===
        if (Scene* scene = GetScene()) {
            RC::Matrix4x4 viewProj = Multiply(ctx.View(), ctx.Proj());
            for (auto& e : scene->GetEntities()) {
                if (e->GetTagInt("is_enemy", 0) == 1) {
                    int ehp = e->GetTagInt("current_hp", 30);
                    int eMaxHp = e->GetTagInt("max_hp", 30);
                    if (ehp <= 0 || eMaxHp <= 0) continue;

                    auto* tr = e->GetComponent<TransformComponent>();
                    if (!tr) continue;
                    
                    // 頭の少し上を計算
                    RC::Vector3 headPos = tr->position;
                    headPos.y += 1.2f; 
                    
                    // w(深度)計算による背後判定
                    float w = headPos.x * viewProj.m[0][3] + headPos.y * viewProj.m[1][3] +
                              headPos.z * viewProj.m[2][3] + 1.0f * viewProj.m[3][3];
                    if (w < 0.1f) continue; 
                    
                    // 地形（岩など）による遮蔽チェック
                    RC::Vector3 cameraPos = { 0, 0, 0 };
                    if (auto* myTr = GetComponent<TransformComponent>()) {
                        cameraPos = myTr->position;
                    }
                    if (RaycastTerrain(scene, cameraPos, headPos)) {
                        continue; // 岩に隠れている場合はHPバーを描画しない
                    }
                    
                    // スクリーン座標へ変換
                    RC::Vector3 screenPos = RC::CameraMath::WorldToScreenPoint(
                        headPos, {screenW, screenH}, ctx.View(), ctx.Proj());
                        
                    // 画面外に飛ばないようにクランプ（画面端で見えるようにする）
                    screenPos.x = RC::Clamp(screenPos.x, 50.0f, screenW - 50.0f);
                    screenPos.y = RC::Clamp(screenPos.y, 50.0f, screenH - 50.0f);
                    
                    // 描画
                    float eBarW = 100.0f;
                    float eBarH = 10.0f;
                    float eBarX = screenPos.x - eBarW * 0.5f;
                    float eBarY = screenPos.y;
                    
                    RC::DrawBox({ eBarX, eBarY }, { eBarX + eBarW, eBarY + eBarH }, { 0.3f, 0.05f, 0.05f, 0.8f });
                    
                    float eHpRatio = static_cast<float>(ehp) / static_cast<float>(eMaxHp);
                    RC::DrawBox({ eBarX, eBarY }, { eBarX + eBarW * eHpRatio, eBarY + eBarH }, { 0.9f, 0.2f, 0.2f, 0.9f });
                    
                    RC::DrawBox({ eBarX, eBarY }, { eBarX + eBarW, eBarY + eBarH }, { 1.0f, 1.0f, 1.0f, 0.5f }, kWire);
                }
            }
        }

        // === Game Over / Game Clear / Damage overlay ===
        if (isDead) {
            RC::DrawBox({ 0.0f, screenH * 0.4f }, { screenW, screenH * 0.6f },
                        { 0.8f, 0.1f, 0.1f, 0.7f });
        } else if (isGameCleared) {
            RC::DrawBox({ 0.0f, screenH * 0.4f }, { screenW, screenH * 0.6f },
                        { 0.1f, 0.8f, 0.2f, 0.6f });
        } else if (invincibleTimer > 0.0f) {
            float flashAlpha = (invincibleTimer / invincibleDuration) * 0.5f;
            RC::DrawBox({ 0.0f, 0.0f }, { screenW, screenH },
                        { 1.0f, 0.0f, 0.0f, flashAlpha });
        }
    }

public:
    void TakeDamage(int damage) {
        if (invincibleTimer > 0.0f || isDead) return;
        // リザルトの「被ダメージ」。無敵中に弾いたぶんは上の早期 return で除外され、
        // HP を割り込んだぶん（オーバーキル）は数えない。
        GameSession::Get().AddDamageTaken((std::min)(damage, hp));
        hp -= damage;
        invincibleTimer = invincibleDuration;
        
        // 演出
        currentCameraShake = shakeMax * 2.0f;

        if (hp <= 0) {
            hp = 0;
            isDead = true;
            if (Entity* self = GetEntity()) {
                self->SetTag("game_over", 1);
            }
        }
    }

    void OnImGui() override {
#if RC_ENABLE_IMGUI
        // スコア / GAME OVER は OnOverlayRender でゲーム画面内に描く（ここは Inspector 専用）
        ImGui::Text("Score: %06d", score);
        ImGui::Text("Reticle Pos: (%.1f, %.1f)", cursorPosition.x, cursorPosition.y);
        {
            constexpr float kRadToDeg = 180.0f / 3.14159265358979f;
            ImGui::Text("Look: yaw %.1f deg / pitch %.1f deg", lookYaw_ * kRadToDeg, lookPitch_ * kRadToDeg);
            Input* in = Input::GetInstance();
            const bool locked = in && in->IsCursorLocked();
            ImGui::Text("Cursor: %s  (ESC: pause menu releases it)", locked ? "LOCKED" : "free");
            if (ImGui::SmallButton("Reset Look")) {
                lookYaw_ = 0.0f;
                lookPitch_ = 0.0f;
            }
        }

        ImGui::Separator();
        ImGui::Text("Look Settings (base values; player multipliers live in GameSettings)");
        ImGui::DragFloat("Look Sensitivity (rad/count)", &lookSensitivity, 0.0001f, 0.0001f, 0.02f, "%.4f");
        ImGui::DragFloat("Controller Look Speed (rad/s)", &controllerLookSpeed, 0.1f, 0.1f, 10.0f);
        ImGui::SliderFloat("Deadzone", &deadzone, 0.0f, 1.0f);
        ImGui::Checkbox("Lock Cursor", &lockCursor);
        {
            GameSettings& gs = GameSettings::Get();
            ImGui::SliderFloat("Player Mouse Sens. (x)", &gs.mouseSensitivity, GameSettings::kSensitivityMin, GameSettings::kSensitivityMax);
            ImGui::SliderFloat("Player Stick Sens. (x)", &gs.controllerSensitivity, GameSettings::kSensitivityMin, GameSettings::kSensitivityMax);
            ImGui::Checkbox("Player Invert Y", &gs.invertY);
        }
        ImGui::DragFloat("Max Yaw (deg)", &maxYawDeg, 1.0f, 0.0f, 180.0f);
        ImGui::DragFloat("Max Pitch (deg)", &maxPitchDeg, 1.0f, 0.0f, 89.0f);
        
        ImGui::Separator();
        ImGui::Text("Reticle Settings");
        ImGui::ColorEdit4("Color", &reticleColor.x);
        ImGui::DragFloat("Size", &reticleSize, 0.5f, 1.0f, 100.0f);

        ImGui::Separator();
        ImGui::Text("Weapon Settings");
        ImGui::DragFloat("Fire Cooldown", &fireCooldown, 0.05f, 0.05f, 5.0f);
        ImGui::DragFloat("Bullet Speed", &bulletSpeed, 1.0f, 10.0f, 200.0f);
        ImGui::DragFloat("Bullet Lifetime", &bulletLifetime, 0.1f, 0.5f, 10.0f);

        ImGui::Separator();
        ImGui::Text("Player Stats");
        ImGui::DragInt("HP", &hp, 1, 0, maxHp);
        ImGui::Text("Score: %d", score);

        ImGui::Separator();
        ImGui::Text("Underwater Settings");
        ImGui::Text("Underwater: %s  blend=%.2f  water Y=%.1f", isUnderwater ? "yes" : "no",
                    transitionTimer, waterHeight);
        ImGui::DragFloat("Transition Speed", &transitionSpeed, 0.1f, 0.1f, 10.0f);
        ImGui::DragFloat("Dive Flash (EV)", &diveFlashEv, 0.05f, -4.0f, 4.0f);
        ImGui::DragFloat("Dive Flash (s)", &diveFlashDuration, 0.01f, 0.07f, 2.0f);
        ImGui::DragFloat("Emerge Flash (EV)", &emergeFlashEv, 0.05f, -4.0f, 4.0f);
        ImGui::DragFloat("Emerge Flash (s)", &emergeFlashDuration, 0.01f, 0.07f, 2.0f);
        if (ImGui::Button("Test Dive Flash")) {
            waterFlash_.Trigger(RC::GetRenderContext().GetPostProcess(), diveFlashEv, diveFlashDuration);
        }
        ImGui::SameLine();
        if (ImGui::Button("Test Emerge Flash")) {
            waterFlash_.Trigger(RC::GetRenderContext().GetPostProcess(), emergeFlashEv, emergeFlashDuration);
        }
        if (UnderwaterLook::DrawImGui(look)) {
            // 光の設定は水中に入る瞬間にしか流し込まないので、いじった値をその場で反映する
            if (auto* postProcess = RC::GetRenderContext().GetPostProcess()) {
                UnderwaterLook::SetupLight(postProcess, look, waterHeight, hasSunDir_ ? &sunDir_ : nullptr);
            }
        }

        ImGui::Separator();
        ImGui::Text("Ink Settings (タコの墨)");
        ink.DrawImGui();

        ImGui::Separator();
        ImGui::Text("Screen Droplets Settings (レンズ水滴・気泡演出)");
        ImGui::DragFloat("Droplet Duration", &dropletDuration, 0.1f, 0.5f, 10.0f);
        ImGui::SliderFloat("Max Intensity", &dropletMaxIntensity, 0.0f, 1.0f);
        ImGui::SliderFloat("Speed (正:下向き / 負:上向き)", &dropletSpeed, -5.0f, 5.0f);
        ImGui::SliderFloat("Distortion", &dropletDistortion, 0.0f, 0.2f);
        ImGui::SliderFloat("Scale", &dropletScale, 0.5f, 5.0f);
        if (ImGui::Button("Test Emerge Splash (水中→水上: 下に向かって滴り消える)")) {
            if (auto* postProcess = RC::GetRenderContext().GetPostProcess()) {
                postProcess->AddEffect(PostEffectType::ScreenDroplets);
                dropletTimer = dropletDuration;
                dropletSpeed = 1.3f;
                postProcess->SetScreenDropletsSpeed(dropletSpeed);
            }
        }
        if (ImGui::Button("Test Dive Splash (水上→水中: 上に向かって気泡が昇り消える)")) {
            if (auto* postProcess = RC::GetRenderContext().GetPostProcess()) {
                postProcess->AddEffect(PostEffectType::ScreenDroplets);
                dropletTimer = dropletDuration;
                dropletSpeed = -1.3f;
                postProcess->SetScreenDropletsSpeed(dropletSpeed);
            }
        }
#endif
    }
};

REGISTER_SCRIPT(RailShooterController)
