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
#include "Game/Framework/EntityPool.h"
#include "Game/Framework/StateMachine.h"
#include <algorithm>
#include <cstddef>
#include <memory>
#include <utility>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {
    /// @brief 線分と軸平行境界ボックス（AABB）が交差するか判定する（スラブ法）
    /// @param p0 線分の始点
    /// @param p1 線分の終点
    /// @param min AABB の最小座標
    /// @param max AABB の最大座標
    /// @return 交差していれば true
    bool IntersectSegmentAABB(const RC::Vector3& p0, const RC::Vector3& p1, const RC::Vector3& min, const RC::Vector3& max) {
        RC::Vector3 d = { p1.x - p0.x, p1.y - p0.y, p1.z - p0.z };
        // 線分を p0 + d*t (t = 0〜1) とし、各軸のスラブに入る/出る t の範囲を狭めていく
        float tmin = 0.0f;
        float tmax = 1.0f;

        // X 軸（方向成分がほぼ 0 なら平行なので、始点がスラブ内かだけを見る）
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
        // Y 軸
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
        // Z 軸
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

    /// @brief start→end の線分が地形（is_terrain タグ付きの AABB コライダー）に遮られるか判定する
    /// @param scene 地形を探すシーン
    /// @param start 線分の始点
    /// @param end 線分の終点
    /// @return いずれかの地形と交差していれば true（scene が nullptr なら false）
    bool RaycastTerrain(Scene* scene, const RC::Vector3& start, const RC::Vector3& end) {
        if (!scene) return false;
        for (auto& e : scene->GetEntities()) {
            if (e->GetTagInt("is_terrain", 0) == 1) {
                auto* col = e->GetComponent<ColliderComponent>();
                auto* tr = e->GetComponent<TransformComponent>();
                if (col && tr && col->IsEnabled() && col->shape == ColliderComponent::Shape::AABB) {
                    // コライダーのサイズ・中心にスケールを反映してワールド空間の AABB を作る
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

    // ------------------------------------------------------------------------
    // 武器（Strategy パターン）
    // ------------------------------------------------------------------------

    /// @brief 自機の武器の基底クラス（Strategy）
    /// @details 以前は `enum class WeaponType { Normal, Spread, Heavy }` を持ち、
    ///          連射間隔・発射数・狙いの散らし・弾の大きさ・bullet_type タグ・HUD の色を
    ///          それぞれ if / 三項演算子で分岐していた。武器を 1 つ足すと 6 か所以上を
    ///          直す必要があったため、「武器 1 つ = クラス 1 つ」に分け、撃つ側（RailShooterController）は
    ///          今持っている武器に問い合わせるだけにした。
    ///          ダメージ・重力・初速の既定値は弾側（WaterBullet の BulletKind）が bullet_type から決める。
    class IWeapon {
    public:
        virtual ~IWeapon() = default;

        /// @brief WaterBullet に渡す弾種（bullet_type タグの値）
        /// @return 0:通常 / 1:拡散 / 2:重い弾
        virtual int BulletTypeTag() const = 0;

        /// @brief 1 回の射撃で出す弾の数
        /// @return 弾の数
        virtual int ShotCount() const { return 1; }

        /// @brief shotIndex 発目が狙うスクリーン座標
        /// @param center レティクルの位置（画面中央）
        /// @param shotIndex 何発目か（0 始まり）
        /// @return 狙うスクリーン座標
        virtual RC::Vector2 AimPoint(const RC::Vector2& center, int shotIndex) const {
            (void)shotIndex;
            return center;
        }

        /// @brief 弾の見た目の大きさ（Transform の scale）
        /// @return 一辺の scale
        virtual float BulletScale() const = 0;

        /// @brief 次に撃てるまでの間隔
        /// @param baseCooldown 基準の連射間隔（秒）
        /// @return この武器の連射間隔（秒）
        virtual float Cooldown(float baseCooldown) const { return baseCooldown; }

        /// @brief HUD（左下の武器表示）のマスの色
        /// @param alpha 不透明度（選択中は濃く、それ以外は薄く）
        /// @return マスの色
        virtual RC::Vector4 HudColor(float alpha) const = 0;
    };

    /// @brief 通常弾：1 発ずつ、標準の大きさ
    class NormalWeapon final : public IWeapon {
    public:
        int BulletTypeTag() const override { return 0; }
        float BulletScale() const override { return 0.3f; }
        RC::Vector4 HudColor(float alpha) const override { return {0.2f, 0.6f, 1.0f, alpha}; }
    };

    /// @brief 拡散弾：小さな弾を左右に 80 ピクセルずつ散らして 3 発
    class SpreadWeapon final : public IWeapon {
    public:
        int BulletTypeTag() const override { return 1; }
        int ShotCount() const override { return 3; }
        RC::Vector2 AimPoint(const RC::Vector2& center, int shotIndex) const override {
            RC::Vector2 target = center;
            target.x += (shotIndex - 1) * 80.0f; // -80, 0, 80 ピクセルずらす
            return target;
        }
        float BulletScale() const override { return 0.2f; }
        RC::Vector4 HudColor(float alpha) const override { return {0.2f, 0.8f, 0.4f, alpha}; }
    };

    /// @brief 重い弾：大きな弾を 1 発。連射間隔は 2 倍
    class HeavyWeapon final : public IWeapon {
    public:
        int BulletTypeTag() const override { return 2; }
        float BulletScale() const override { return 0.6f; }
        float Cooldown(float baseCooldown) const override { return baseCooldown * 2.0f; }
        RC::Vector4 HudColor(float alpha) const override { return {0.8f, 0.3f, 0.1f, alpha}; }
    };

    /// @brief 持っている武器の一覧と、選択中の武器
    /// @details 以前は enum を int にキャストして ±1 し、範囲外を手で 0 / 2 に戻していた。
    ///          循環の規則と「何番目を選んでいるか」をこのクラスに閉じ込め、外からは
    ///          前へ / 次へ / 今の武器 だけを使わせる（カプセル化）。
    class WeaponSelector {
    public:
        /// @brief 通常 → 拡散 → 重い弾 の順で武器を持つ（HUD の並びもこの順）
        WeaponSelector() {
            weapons_.push_back(std::make_unique<NormalWeapon>());
            weapons_.push_back(std::make_unique<SpreadWeapon>());
            weapons_.push_back(std::make_unique<HeavyWeapon>());
        }

        /// @brief 選択中の武器
        /// @return 武器
        const IWeapon& Current() const { return *weapons_[index_]; }

        /// @brief 前の武器へ（先頭からは末尾へ循環）
        void SelectPrevious() { index_ = (index_ + weapons_.size() - 1) % weapons_.size(); }

        /// @brief 次の武器へ（末尾からは先頭へ循環）
        void SelectNext() { index_ = (index_ + 1) % weapons_.size(); }

        /// @brief 持っている武器の数
        /// @return 武器の数
        std::size_t Count() const { return weapons_.size(); }

        /// @brief i 番目の武器
        /// @param i 番号（0 始まり）
        /// @return 武器
        const IWeapon& At(std::size_t i) const { return *weapons_[i]; }

        /// @brief i 番目の武器を選んでいるか
        /// @param i 番号（0 始まり）
        /// @return 選択中なら true
        bool IsSelected(std::size_t i) const { return i == index_; }

    private:
        std::vector<std::unique_ptr<IWeapon>> weapons_; ///< 持っている武器（並び順 = 切り替え順）
        std::size_t index_ = 0;                          ///< 選択中の武器の番号
    };
}

/// @brief レールシューティングにおける照準（レティクル）操作を担うスクリプト
/// @details 照準は画面中央に固定し、マウスの移動量（と右スティック）でカメラの向きを変える
///          FPS 型の操作。カメラ（このエンティティの TransformComponent）の rotation は
///          「レールが与える基準の向き ＋ プレイヤーの視点オフセット（yaw / pitch）」で毎フレーム決める。
///          マウスカーソルは Input のカーソルロックで画面中央に固定・非表示にする。
///          ポーズ中（deltaTime = 0 で来るフレーム）はロックを外すので、ポーズメニューを
///          マウスで操作できる。閉じると次のフレームで再びロックする。
///          プレイヤーが変える感度・上下反転は GameSettings（ポーズメニューの設定画面）から読む。
///
///          設計:
///            - 武器は Strategy（IWeapon / WeaponSelector）。
///            - 自機弾は Object Pool（EntityPool）から取り出す。
///            - 水上 / 水中は State パターン（SurfaceState / UnderwaterState）。出入りした瞬間の
///              演出（ラジアルブラー・水滴・露出フラッシュ）を各状態の OnEnter に置く。
///            - 生存（プレイ中 / クリア後）/ 死亡も State パターン（PlayingState / ClearedState / DeadState）。
///              以前は isDead / isGameCleared の 2 つの bool の組み合わせで HUD と操作を分岐していた。
class RailShooterController : public ScriptableEntity {
public:
    /// @brief 水中の見た目・墨演出・水面フラッシュ・視点操作の設定を JSON に書き出す
    /// @return シリアライズした JSON
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

    /// @brief JSON から水中の見た目・墨演出・水面フラッシュ・視点操作の設定を読み込む
    /// @param j 読み込む JSON（無いキーは現在値のまま）
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

private:
    // --- 状態クラス（定義はクラス定義の後ろ） ---
    class SurfaceState;    ///< 水上
    class UnderwaterState; ///< 水中
    class LifeState;       ///< 生死の状態の基底（被弾・HUD の描き分けを持つ）
    class AliveState;      ///< 生きている状態の共通部分（被弾で HP が減る）
    class PlayingState;    ///< プレイ中
    class ClearedState;    ///< レールの終点に着いた（クリア表示）
    class DeadState;       ///< 力尽きた（GAME OVER）

    /// @brief HUD を 1280x720 基準で描くための配置（PauseMenu と同じレターボックス計算）
    /// @details 振る舞いの無い受け渡し用の値の束なので struct にしている。
    struct HudLayout {
        float s = 1.0f;  ///< 基準解像度 → 実解像度の倍率
        float ox = 0.0f; ///< レターボックス分の X オフセット
        float oy = 0.0f; ///< レターボックス分の Y オフセット
        float fs = 1.0f; ///< フォントの描画倍率（ロード時のスケールとの比）

        /// @brief 基準座標で指定した位置に、影（2px ずらした暗色）付きで文字を描く
        /// @param font フォント（負なら描かない）
        /// @param text 文字列（空なら描かない）
        /// @param dx 基準座標の X
        /// @param dy 基準座標の Y
        /// @param color 文字の色
        /// @param align 揃え
        void DrawShadowedText(int font, const std::string& text, float dx, float dy,
                              const RC::Vector4& color, TextAlign align) const {
            if (font < 0 || text.empty()) return;
            const float x = ox + dx * s;
            const float y = oy + dy * s;
            const float sh = 2.0f * s;
            RC::DrawString(font, text, {x + sh, y + sh}, {0.0f, 0.02f, 0.05f, 0.6f * color.w}, fs, align);
            RC::DrawString(font, text, {x, y}, color, fs, align);
        }
    };

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
    float demoAimSharpness = 7.0f; ///< 見せ用デモで照準を目標へ寄せる速さ（1/秒。大きいほど速く合う）

    // 描画設定（仮）
    RC::Vector4 reticleColor = { 1.0f, 0.0f, 0.0f, 1.0f };
    float reticleSize = 10.0f;

    // 射撃パラメータ（秒・速さ）
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

    // プレイヤーステータス（生死は lifeFsm_ の状態で表す）
    int hp = 5;
    int maxHp = 5;
    float invincibleTimer = 0.0f;
    float invincibleDuration = 1.0f;
    int score = 0;

    /// @brief 武器（Strategy）。ホイール / LB・RB で切り替える
    WeaponSelector weapons_;

    /// @brief 自機弾の Object Pool
    /// @details 以前はここで「非アクティブな PlayerBullet を集めて再利用し、足りなければ作る」処理を
    ///          直接書いていた（敵弾・砲弾も同じ処理を個別に持っていた）。取り出し規則を EntityPool に
    ///          まとめ、ここは「1 発ぶん取り出す」だけにした。返却は WaterBullet が非アクティブ化で行う。
    EntityPool bulletPool_{"PlayerBullet"};

    // 敵全体（ボス）情報
    int totalEnemyHp = 0;
    int totalEnemyMaxHp = 0;
    bool railFinished_ = false; ///< レールの終点に着いたか（rail_finished タグを見つけたら立つ。PlayingState が見る）

    // 水中の状態（水上 / 水中は waterFsm_ の状態で表す）
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

    // 水滴演出（ Screen Droplets ）タイマーと設定
    float dropletTimer = 0.0f;
    float dropletDuration = 3.4f; // 着水・出水時から水滴が残りつづける秒数（水膜が引いて水滴が流れ出るまで）
    float dropletMaxIntensity = 1.0f;
    float dropletSpeed = 1.2f;
    float dropletDistortion = 0.06f;
    float dropletScale = 1.5f;

    // ダイブ時のスピードブラー演出 (RadialBlur) タイマー
    float radialBlurTimer = 0.0f;
    float radialBlurDuration = 0.45f;
    float radialBlurMaxWidth = 0.035f;

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

    StateMachine<RailShooterController> waterFsm_; ///< 水上 / 水中
    StateMachine<RailShooterController> lifeFsm_;  ///< プレイ中 / クリア後 / 死亡

    /// @brief マウス入力をゲーム操作として扱ってよいか
    /// @details ロック中はカーソルが Viewport 中央に固定されるので常に true。
    ///          ロックを使わない設定のときは、エディタの Viewport 上にあるときだけ受け付ける
    ///          （インスペクタをドラッグして視点が回るのを防ぐ）。
    /// @param input 入力管理（nullptr なら false）
    /// @return ゲーム操作として扱ってよければ true
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
    /// @param input 反映先の入力管理（nullptr なら何もしない）
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

    /// @brief 自機の弾をまとめる "PlayerBullets" フォルダエンティティを取得する（無ければ作成）
    /// @param scene 検索・作成先のシーン
    /// @return フォルダエンティティの GUID（一度見つけたらキャッシュを返す）
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

    // --- 状態まわり（状態クラスが完全型である必要があるので、定義はクラスの後ろ） ---

    /// @brief 水中にいるか
    /// @return 水中の状態なら true
    bool IsUnderwater() const;

    /// @brief 力尽きているか
    /// @return 死亡の状態なら true
    bool IsDead() const;

    /// @brief 水上 / 水中の状態を、出入りの演出なしで設定する
    /// @param startUnderwater 水中から始めるなら true
    void InitWaterState(bool startUnderwater);

    /// @brief 生死の状態が無ければ「プレイ中」から始める
    void EnsureLifeState();

    /// @brief 今の生死の状態に応じた HUD の文字（スコア / クリア / GAME OVER）を描く
    /// @param layout HUD の配置
    void DrawLifeOverlayText(const HudLayout& layout);

    /// @brief 今の生死の状態に応じた全画面の帯（赤 / 緑 / 被弾フラッシュ）を描く
    /// @param screenW 画面の幅（ピクセル）
    /// @param screenH 画面の高さ（ピクセル）
    void DrawLifeScreenBand(float screenW, float screenH);

    /// @brief 自機の位置が水面より下か（水上 / 水中の状態が遷移の判定に使う）
    /// @return 水面より下なら true（Transform が無ければ false）
    bool IsBelowWaterSurface() {
        auto* tr = GetComponent<TransformComponent>();
        return tr && tr->position.y < waterHeight;
    }

    /// @brief レンズの水滴・気泡の演出を最大の強さから始める
    /// @param postProcess 演出を積むポストプロセス
    /// @param speed 流れる速さ（正: 下向き＝重力 / 負: 上向き＝気泡）
    /// @param mode 水滴の出方（Dive: 水面ラインが抜けて気泡が昇る / Surface: 水膜が引いて水滴が流れ出る）
    void StartScreenDroplets(PostProcess* postProcess, float speed, ScreenDropletSim::Mode mode) {
        dropletTimer = dropletDuration;
        dropletSpeed = speed;
        postProcess->SetScreenDropletsIntensity(dropletMaxIntensity);
        postProcess->SetScreenDropletsSpeed(dropletSpeed);
        postProcess->SetScreenDropletsDistortion(dropletDistortion);
        postProcess->SetScreenDropletsScale(dropletScale);
        postProcess->TriggerScreenDroplets(mode);
    }

    /// @brief 水上 → 水中へ入った瞬間の演出（UnderwaterState の OnEnter から呼ぶ）
    void PlayDiveFx() {
        if (Entity* self = GetEntity()) {
            self->SetTag("is_underwater", 1);
        }
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

            // 水中突入（ダイブ）時の勢い・スピード感の演出として RadialBlur を積む
            postProcess->AddEffect(PostEffectType::RadialBlur);
            radialBlurTimer = radialBlurDuration;
            postProcess->SetRadialBlurWidth(radialBlurMaxWidth);

            // 遷移時（Dive/水上→水中）：水面のラインが下から上へ抜け、レンズに残った気泡が浮力と走行風で外へ抜けていく
            // 負のスピード値で上方向（気泡）
            StartScreenDroplets(postProcess, -1.3f, ScreenDropletSim::Mode::Dive);

            // 着水の衝撃：一瞬だけ暗く沈む
            waterFlash_.Trigger(postProcess, diveFlashEv, diveFlashDuration);
        }
    }

    /// @brief 水中 → 水上へ出た瞬間の演出（SurfaceState の OnEnter から呼ぶ）
    void PlayEmergeFx() {
        if (Entity* self = GetEntity()) {
            self->SetTag("is_underwater", 0);
        }
        Log::Print("[Event] Transition to Surface (Emerge)");
        if (auto* postProcess = RC::GetRenderContext().GetPostProcess()) {
            // 出水（浮上）時も一時的にスピードブラーを発生させ離脱の爽快な衝撃を演出
            postProcess->AddEffect(PostEffectType::RadialBlur);
            radialBlurTimer = radialBlurDuration * 0.7f;
            postProcess->SetRadialBlurWidth(radialBlurMaxWidth * 0.7f);

            // 出水時（Emerge/水中→水上）：レンズを覆った水膜が中央から割れて引き、残った水滴が重力と走行風で外へ流れ出る
            // 正のスピード値で下方向（重力）
            StartScreenDroplets(postProcess, 1.3f, ScreenDropletSim::Mode::Surface);

            // 水面を抜けた解放感：一瞬だけ白く飛ぶ
            waterFlash_.Trigger(postProcess, emergeFlashEv, emergeFlashDuration);
        }
    }

    /// @brief 死亡したことをシーン側へ "game_over" タグで通知する（DeadState の OnEnter から呼ぶ）
    void NotifyGameOver() {
        if (Entity* self = GetEntity()) {
            self->SetTag("game_over", 1);
        }
    }

    /// @brief 今の画面サイズから HUD の配置（1280x720 基準のレターボックス）を求める
    /// @return HUD の配置
    HudLayout MakeHudLayout() const {
        float screenW = kDesignW, screenH = kDesignH;
        auto& rc = RC::GetRenderContext();
        if (rc.Ctx() && rc.Ctx()->app) {
            screenW = static_cast<float>(rc.Ctx()->app->width);
            screenH = static_cast<float>(rc.Ctx()->app->height);
        }
        HudLayout layout;
        float s = (std::min)(screenW / kDesignW, screenH / kDesignH);
        if (s <= 0.0f) s = 1.0f;
        layout.s = s;
        layout.ox = (screenW - kDesignW * s) * 0.5f;
        layout.oy = (screenH - kDesignH * s) * 0.5f;
        layout.fs = (hudFontScale_ > 0.0f) ? (s / hudFontScale_) : 1.0f;
        return layout;
    }

    /// @brief 通常時の HUD 文字：左上にスコア（PlayingState が呼ぶ）
    /// @param layout HUD の配置
    void DrawScoreText(const HudLayout& layout) const {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "SCORE  %06d", score);
        layout.DrawShadowedText(scoreFont_, buf, 24.0f, 18.0f, {1.0f, 1.0f, 1.0f, 1.0f}, TextAlign::Left);
    }

    /// @brief 死亡時の HUD 文字：画面中央に GAME OVER だけを出す（DeadState が呼ぶ）
    /// @param layout HUD の配置
    void DrawGameOverText(const HudLayout& layout) const {
        const float lh = (gameOverFont_ >= 0) ? RC::GetFontLineHeight(gameOverFont_, layout.fs) / layout.s : kGameOverPx;
        layout.DrawShadowedText(gameOverFont_, "GAME OVER", kDesignW * 0.5f, kDesignH * 0.5f - lh * 0.5f,
                                {1.0f, 0.25f, 0.25f, 1.0f}, TextAlign::Center);
    }

    /// @brief クリア時の HUD 文字（ClearedState が呼ぶ）
    /// @param layout HUD の配置
    /// @details 終点に着いたときの帯（OnRender の緑の帯。画面の 40%〜60%）に文字を載せる。
    ///            1 行目: STAGE CLEAR（最終面は ALL CLEAR）
    ///            2 行目: 第 N 面 ○○ 踏破 ／ スコア
    void DrawClearText(const HudLayout& layout) const {
        const StageProgress &prog = StageProgress::Get();
        const int stage = prog.Current();
        const bool finalStage = prog.IsFinalStage();
        const float titleH = (gameOverFont_ >= 0) ? RC::GetFontLineHeight(gameOverFont_, layout.fs) / layout.s : kGameOverPx;
        const float subH = (scoreFont_ >= 0) ? RC::GetFontLineHeight(scoreFont_, layout.fs) / layout.s : kScorePx;
        const float gap = 6.0f;
        const float top = kDesignH * 0.5f - (titleH + gap + subH) * 0.5f;
        layout.DrawShadowedText(gameOverFont_, finalStage ? "ALL CLEAR" : "STAGE CLEAR", kDesignW * 0.5f, top,
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
        layout.DrawShadowedText(scoreFont_, sub, kDesignW * 0.5f, top + titleH + gap, {1.0f, 1.0f, 1.0f, 1.0f},
                                TextAlign::Center);
    }

protected:
    /// @brief プレイヤータグ付け、設定・HUD フォントの読み込み、ポストエフェクト設定、水面高さ・光の向きの取得、開始時の水中状態の初期化を行う
    void OnCreate() override {
        // 敵弾などがタグで自機を識別できるようにする
        if (Entity* self = GetEntity()) {
            self->SetTag("is_player", 1);
        }
        // 生死の状態は「プレイ中」から始める
        EnsureLifeState();
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
            // 全体的な視認性向上のため、深度ベースのアウトラインを常時有効化する
            postProcess->AddEffect(PostEffectType::DepthBasedOutline);
            float outlineColor[4] = {0.04f, 0.08f, 0.16f, 0.85f};
            postProcess->SetOutlineColor(outlineColor);
            postProcess->SetOutlineThickness(1.0f);
        }

        // 水面の高さはシーンの WaterComponent から拾う（無ければ 0）
        // 光柱の向きはシーンの DirectionalLight から拾う（無ければ PostProcess の既定）
        waterHeight = 0.0f;
        float waterClarity = 0.0f; // 水中の輪郭線を薄めるのに使う（水が無ければ 0 = 薄めない）
        if (Scene* scene = GetScene()) {
            for (auto& e : scene->GetEntities()) {
                if (!e) continue;
                if (auto* water = e->GetComponent<WaterComponent>()) {
                    if (auto* wtr = e->GetComponent<TransformComponent>()) waterHeight = wtr->position.y;
                    // Water.PS の屈折は clarity <= 0 のとき 3m として扱うので合わせる
                    waterClarity = (water->clarity > 0.0f) ? water->clarity : 3.0f;
                }
                if (auto* dl = e->GetComponent<DirectionalLightComponent>()) {
                    if (dl->visible) { sunDir_ = dl->direction; hasSunDir_ = true; }
                }
            }
        }
        // 水中の物の輪郭線を、水の濁りに合わせて薄める（深い床の線だけ透けて見えないように）
        if (auto* postProcess = RC::GetRenderContext().GetPostProcess()) {
            postProcess->SetOutlineWaterFade(waterHeight, waterClarity);
        }

        // 最初から水中に置かれている場合（DeepRiseIntroScript が深海から始めるとき）は、
        // 着水の演出（ラジアルブラー・水滴）を出さずに、水中エフェクトだけ静かに積んで
        // 「ずっと水中にいた」状態から始める。ここでやらないと最初の Update で
        // 「水上 → 水中」と誤検出して、暗いはずの画面が一瞬明るく抜ける。
        if (auto* tr = GetComponent<TransformComponent>()) {
            const bool startUnderwater = (tr->position.y < waterHeight);
            InitWaterState(startUnderwater); // 演出なしで状態だけ入れる
            transitionTimer = startUnderwater ? 1.0f : 0.0f;
            if (Entity* self = GetEntity()) {
                self->SetTag("is_underwater", startUnderwater ? 1 : 0);
            }
            if (auto* postProcess = RC::GetRenderContext().GetPostProcess()) {
                UnderwaterLook::SetupLight(postProcess, look, waterHeight, hasSunDir_ ? &sunDir_ : nullptr);
                if (startUnderwater) {
                    UnderwaterLook::AddStack(postProcess);
                    UnderwaterLook::SetLerp(postProcess, 1.0f);
                    UnderwaterLook::ApplyFogByDepth(postProcess, look, waterHeight - tr->position.y);
                    Log::Print("[RailShooterController] starts underwater (silent)");
                }
            }
        } else {
            // Transform が無いときは「水上」扱い（タグは付けない）
            InitWaterState(false);
        }
    }

    /// @brief 視点操作・射撃・被ダメージ・水中／水上の切り替え演出・HUD 用の状態更新を毎フレーム行う
    /// @param deltaTime 前フレームからの経過時間（秒）。0 のフレームはポーズ中として扱う
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

        // OnCreate より先に Update が来ても、プレイ中・水上から始める（以前の既定値と同じ）
        EnsureLifeState();
        if (!waterFsm_.HasState()) InitWaterState(false);

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
            // 水面を跨いだかは今の状態（SurfaceState / UnderwaterState）が判定し、
            // 跨いだら次の状態の OnEnter で着水 / 出水の演出を出す
            waterFsm_.Update(*this, deltaTime);
        }

        // トランジションの更新（0=水上〜1=水中 へ transitionSpeed で近づける）
        const bool underwater = IsUnderwater();
        float targetTransition = underwater ? 1.0f : 0.0f;
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
                if (transitionTimer == 0.0f && !underwater) {
                    UnderwaterLook::RemoveStack(postProcess);
                }
            }
        }

        // 水中にいる（または出入りの途中の）あいだは、深さに応じてフォグの色と距離を変える。
        // 水面直下は青く遠くまで見え、深くなるほど暗く近くしか見えない（深海）。
        if (underwater || transitionTimer > 0.0f) {
            if (auto* postProcess = RC::GetRenderContext().GetPostProcess()) {
                UnderwaterLook::ApplyFogByDepth(postProcess, look, depth);
            }
        }

        // === 露出フラッシュの減衰更新 ===
        waterFlash_.Update(RC::GetRenderContext().GetPostProcess(), deltaTime);

        // === RadialBlur タイマーの減衰更新（残り割合の 2 乗で幅を絞り、切れたら除去） ===
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
                    // 水滴が流れ落ちる間は強度を保ち、最後の 35% で水を痩せさせて消す
                    // （最初から薄めると、流れている最中の水滴まで透けてしまう）
                    const float progress = dropletTimer / dropletDuration;
                    const float x = (std::min)(1.0f, progress / 0.35f);
                    float currentIntensity = x * x * (3.0f - 2.0f * x) * dropletMaxIntensity;
                    postProcess->SetScreenDropletsIntensity(currentIntensity);
                    postProcess->SetScreenDropletsSpeed(dropletSpeed);
                    postProcess->SetScreenDropletsDistortion(dropletDistortion);
                    postProcess->SetScreenDropletsScale(dropletScale);
                }
            }
        }

        // === ダメージ・スコア処理（他スクリプトがタグで積んだ値を取り出して反映し、タグを消す） ===
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
            // 合算値を 100（=強さ 1.0）ずつに分けて AddHit し、墨を受けたらカメラも揺らす
            int inkHit = self->GetTagInt("pending_ink", 0);
            if (inkHit > 0) {
                self->ClearTag("pending_ink");
                if (!IsDead()) {
                    while (inkHit > 0) {
                        const int one = (std::min)(inkHit, 100);
                        ink.AddHit(static_cast<float>(one) / 100.0f);
                        inkHit -= one;
                    }
                    currentCameraShake = (std::max)(currentCameraShake, shakeMax * 0.6f);
                }
            }
        }
        ink.Update(RC::GetRenderContext().GetPostProcess(), deltaTime, IsUnderwater());

        // Result シーンから読めるように、現在の成績を GameSession へ写す。
        // score / hp はこのスクリプトのメンバなので、シーンを抜けると失われるため。
        GameSession::Get().SetScore(score);
        GameSession::Get().SetPlayerHp(hp, maxHp);

        // 敵の合計HPの集計、およびクリア判定
        totalEnemyHp = 0;
        totalEnemyMaxHp = 0;
        if (Scene* scene = GetScene()) {
            for (auto& e : scene->GetEntities()) {
                if (e->GetTagInt("is_enemy", 0) == 1) {
                    // HP タグが未設定の敵は既定値 30 で初期化する
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
                    railFinished_ = true;
                }
            }
        }
        // プレイ中の状態が railFinished_ を見てクリア状態へ移る
        lifeFsm_.Update(*this, deltaTime);

        // 開始演出中は操作を受け付けない（浮上が終わって GAME START が出てから）
        if (introPlaying) return;

        // 武器切り替え（ホイール上 / LB で前の武器、ホイール下 / RB で次の武器。3 種を循環）
        long wheel = input->GetMouseZ();
        if (wheel > 0) {
            weapons_.SelectPrevious();
        } else if (wheel < 0) {
            weapons_.SelectNext();
        }

        if (input->IsXInputConnected()) {
            if (input->IsXInputButtonTrigger(XINPUT_GAMEPAD_LEFT_SHOULDER)) {
                weapons_.SelectPrevious();
            }
            if (input->IsXInputButtonTrigger(XINPUT_GAMEPAD_RIGHT_SHOULDER)) {
                weapons_.SelectNext();
            }
        }

        // === 視点操作（画面中央固定エイム） ===

        // 1. 基準の向きを拾う（演出が終わって最初に操作できるフレーム）
        if (!lookInitialized_) {
            if (auto* tr = GetComponent<TransformComponent>()) {
                baseRotation_ = tr->rotation;
                lookInitialized_ = true;
                // 「自機の正面」を敵 AI へ知らせる（視点は ±maxYawDeg までしか振れないので、
                // この向きから大きく外れた敵は撃てない）。ShipEnemyScript が持ち場の基準に使う。
                // タグは int なので 1000 倍（ミリラジアン）で渡す。
                if (Entity* selfEntity = GetEntity()) {
                    selfEntity->SetTag("base_yaw_mrad", static_cast<int>(baseRotation_.y * 1000.0f));
                }
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
        if (!IsDead() && !cameraOwnedByOutro) {
            // プレイヤーが設定画面で変えた倍率・反転（シーンをまたいで保持される）
            const GameSettings& settings = GameSettings::Get();
            const float mouseGain = lookSensitivity * settings.MouseSensitivity();
            const float stickGain = controllerLookSpeed * settings.ControllerSensitivity() * deltaTime;

            // マウスは移動量そのもの（フレームレート非依存）、スティックは倒し続けるので deltaTime を掛ける
            float dYaw   = deltaMouseX * mouseGain + stickX * stickGain;
            float dPitch = deltaMouseY * mouseGain + stickY * stickGain;
            if (settings.InvertY()) dPitch = -dPitch;

            constexpr float kDegToRad = 3.14159265358979f / 180.0f;
            const float maxYaw   = maxYawDeg * kDegToRad;
            const float maxPitch = maxPitchDeg * kDegToRad;
            // 見せ用デモ（ClearRunDemo）が照準を動かしているときは、マウスの代わりにその向きへ寄せる
            //   demo_look = 1: demo_look_yaw/pitch_mrad（ワールドの角度）へ / 2: 正面（オフセット 0）へ
            const int demoLook = GetEntity() ? GetEntity()->GetTagInt("demo_look", 0) : 0;
            if (demoLook != 0) {
                float targetYaw = 0.0f, targetPitch = 0.0f;
                if (demoLook == 1) {
                    const float yawAbs = static_cast<float>(GetEntity()->GetTagInt("demo_look_yaw_mrad", 0)) / 1000.0f;
                    const float pitchAbs = static_cast<float>(GetEntity()->GetTagInt("demo_look_pitch_mrad", 0)) / 1000.0f;
                    targetYaw = std::remainder(yawAbs - baseRotation_.y, 6.28318530718f);
                    targetPitch = pitchAbs - baseRotation_.x;
                }
                // 人が狙うように、少し遅れて追いつく
                const float k = 1.0f - std::exp(-demoAimSharpness * deltaTime);
                dYaw = (targetYaw - lookYaw_) * k;
                dPitch = (targetPitch - lookPitch_) * k;
            }
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
        //    クリアの海図が出ているあいだは撃たない（クリックは海図の早送りに使う）
        const bool clearChartPlaying = (GetEntity() && GetEntity()->GetTagInt("clear_chart_playing", 0) == 1);
        if (!IsDead() && !clearChartPlaying) {
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
            // 右トリガーは半分程度（0〜255 の 128 超）押し込んだら射撃
            if (input->GetXInputRightTrigger() > 128) {
                isFirePressed = true;
            }
            // 見せ用デモ（ClearRunDemo）が照準を合わせたら撃つ
            if (GetEntity() && GetEntity()->GetTagInt("demo_fire", 0) == 1) {
                isFirePressed = true;
            }

            // 連射間隔は武器が決める（Heavy は 2 倍）
            if (isFirePressed && currentCooldown <= 0.0f) {
                currentCooldown = weapons_.Current().Cooldown(fireCooldown);
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

        // シーン内の最初のカメラに、シェイク強度の範囲（±強度）のランダムなオフセットを適用
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

    /// @brief 現在の武器で弾を発射する（画面中央のレティクルからのレイ方向へ）
    /// @details 弾は Object Pool（EntityPool）から 1 発ずつ取り出す（非アクティブな "PlayerBullet" を
    ///          再利用し、無ければ新規作成）。取り出した弾はその場でアクティブになるので、
    ///          拡散弾の 2 発目以降が同じ弾を二重に取ることはない。
    ///          発射数・狙いの散らし・弾の大きさ・弾種は武器（IWeapon）が決める。
    ///          速度・寿命・方向・弾種はタグで WaterBullet に渡す。発射時に反動・カメラシェイクも加える。
    void FireBullet() {
        auto& ctx = RC::GetRenderContext();
        float screenW = 1280.0f;
        float screenH = 720.0f;
        if (ctx.Ctx() && ctx.Ctx()->app) {
            screenW = static_cast<float>(ctx.Ctx()->app->width);
            screenH = static_cast<float>(ctx.Ctx()->app->height);
        }

        const IWeapon& weapon = weapons_.Current();
        const int numBullets = weapon.ShotCount();

        for (int i = 0; i < numBullets; ++i) {
            // 武器ごとの狙い（拡散弾は左右に散らす）
            RC::Vector2 targetPos = weapon.AimPoint(cursorPosition, i);

            // 狙うスクリーン座標からカメラのレイを求める
            RC::Ray ray = RC::CameraMath::ScreenPointToRay(
                targetPos,
                {screenW, screenH},
                ctx.View(),
                ctx.Proj()
            );

            Scene* scene = GetScene();
            if (!scene) continue;

            // 弾エンティティをプールから取り出す（再利用なら reused タグ付き、無ければ新規作成）
            EntityPool::Lease lease = bulletPool_.Acquire(*scene);
            std::shared_ptr<Entity> bullet = lease.entity;
            const bool isNew = lease.isNew;
            if (!bullet) continue;

            auto* tr = bullet->GetComponent<TransformComponent>();
            if (!tr) tr = &bullet->AddComponent<TransformComponent>();
            tr->position = ray.origin;
            bullet->SetParentGuid(GetBulletsFolder(scene));
            // 武器ごとの弾の大きさ
            const float scale = weapon.BulletScale();
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

            // 弾のパラメータをタグで WaterBullet に渡す（タグは int のみなので方向は 1000 倍、速度・寿命は 10 倍）
            bullet->SetTag("dir_x", static_cast<int>(ray.direction.x * 1000.0f));
            bullet->SetTag("dir_y", static_cast<int>(ray.direction.y * 1000.0f));
            bullet->SetTag("dir_z", static_cast<int>(ray.direction.z * 1000.0f));
            bullet->SetTag("bullet_speed", static_cast<int>(bulletSpeed * 10.0f));
            bullet->SetTag("bullet_lifetime", static_cast<int>(bulletLifetime * 10.0f));
            // 弾種（ダメージ・重力などは WaterBullet が弾種から決める）
            bullet->SetTag("bullet_type", weapon.BulletTypeTag());

            // WaterBullet スクリプトを付け（未付与時のみ）、シーン情報を渡す
            auto* nsc = bullet->GetComponent<NativeScriptComponent>();
            if (!nsc) {
                nsc = &bullet->AddComponent<NativeScriptComponent>();
                nsc->AddScript("WaterBullet");
            }
            nsc->SetScene(scene);
            if (GetSceneContext()) nsc->SetSceneContext(GetSceneContext());

            // 新規作成した弾はランタイム初期化、再利用した弾はメッシュ位置だけ即時同期する
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

        // 発射の演出（レティクルの反動とカメラシェイク）を最大値から始める
        currentRecoil = recoilMax;
        currentCameraShake = shakeMax;
    }

    /// @brief カーソルロック・墨・露出オフセットを元に戻し、HUD フォントを解放する
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
    /// @details 何を描くか（スコア / クリア / GAME OVER）は今の生死の状態が決める。
    void OnOverlayRender() override {
        if (GetEntity() && GetEntity()->GetTagInt("intro_playing", 0) == 1) return;
        // クリアの海図（ClearAscendScript）が出ているあいだは、STAGE CLEAR の文字もスコアも海図に任せる
        if (GetEntity() && GetEntity()->GetTagInt("clear_chart_playing", 0) == 1) return;

        // PauseMenu と同じ 1280x720 基準レイアウト（レターボックス分をオフセット）
        const HudLayout layout = MakeHudLayout();
        DrawLifeOverlayText(layout);
    }

    /// @brief レティクル・武器表示・HP ゲージ・敵の頭上 HP バー・ゲームオーバー／クリア／被弾のオーバーレイを 2D 描画する
    void OnRender() override {
        // 開始演出（深海からの浮上）中は HUD を出さない
        if (GetEntity() && GetEntity()->GetTagInt("intro_playing", 0) == 1) return;
        // クリアの海図（ClearAscendScript）が出ているあいだは照準・HP・緑の帯を出さない
        if (GetEntity() && GetEntity()->GetTagInt("clear_chart_playing", 0) == 1) return;

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

        // 武器表示（左下に Normal / Spread / Heavy の 3 マス。選択中だけ濃く表示。色は武器が持つ）
        float wX = 20.0f; float wY = screenH - 80.0f;
        for (std::size_t i = 0; i < weapons_.Count(); ++i) {
            const float x0 = wX + 25.0f * static_cast<float>(i);
            const float alpha = weapons_.IsSelected(i) ? 0.9f : 0.3f;
            RC::DrawBox({x0, wY}, {x0 + 20.0f, wY + 20.0f}, weapons_.At(i).HudColor(alpha));
        }

        // === プレイヤーの HP ゲージ ===
        // 武器表示の下。元のゲージより長く・太くして読みやすくする
        const float barX = 20.0f;
        const float barW = 300.0f;
        const float barH = 16.0f;
        const float barY = screenH - 20.0f - barH - 14.0f; // 下端から 34px（武器表示と 10px 空ける）

        // 無敵中は 0.1 秒ごとに表示／非表示を切り替えて点滅させる
        bool drawPlayerUI = true;
        if (invincibleTimer > 0.0f) {
            if (static_cast<int>(invincibleTimer * 10.0f) % 2 == 0) {
                drawPlayerUI = false;
            }
        }

        if (drawPlayerUI) {
            // 背景
            RC::DrawBox({ barX, barY }, { barX + barW, barY + barH }, { 0.3f, 0.05f, 0.05f, 0.8f });

            // 残り HP（HP 割合で緑→赤に変化し、幅も割合に比例）
            float hpRatio = (maxHp > 0) ? static_cast<float>((std::max)(hp, 0)) / static_cast<float>(maxHp) : 0.0f;
            RC::Vector4 hpColor = { 1.0f - hpRatio, hpRatio, 0.1f, 0.9f };
            RC::DrawBox({ barX, barY }, { barX + barW * hpRatio, barY + barH }, hpColor);

            // 枠線
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

                    // クリップ空間の w（視点からの奥行き）を求め、カメラの背後（ほぼ 0 以下）なら描かない
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

                    // 頭上の HP バー（背景・残量・枠線）を描画
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

        // === ゲームオーバー（赤帯）／クリア（緑帯）／被弾直後（無敵残り時間に比例した赤い全画面フラッシュ）のオーバーレイ ===
        // どれを描くかは今の生死の状態が決める
        DrawLifeScreenBand(screenW, screenH);
    }

public:
    /// @brief ダメージを受ける（実際の処理は今の生死の状態に任せる）
    /// @param damage 減らす HP 量
    /// @note 生きている状態なら HP を減らし、無敵時間とカメラシェイクを開始する。
    ///       無敵時間中・死亡済み・見せ用デモ中は何もしない。HP が 0 以下になると死亡状態へ移り "game_over" タグを立てる
    void TakeDamage(int damage);

    /// @brief 状態表示と、視点・レティクル・武器・HP・水中演出・墨・画面水滴の調整／テスト UI を Inspector に出す
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
            // GameSettings の値は private なので、ローカルに写して編集し、変わったら書き戻す
            float mouseSens = gs.MouseSensitivity();
            float stickSens = gs.ControllerSensitivity();
            bool invertY = gs.InvertY();
            if (ImGui::SliderFloat("Player Mouse Sens. (x)", &mouseSens, GameSettings::kSensitivityMin, GameSettings::kSensitivityMax)) gs.SetMouseSensitivity(mouseSens);
            if (ImGui::SliderFloat("Player Stick Sens. (x)", &stickSens, GameSettings::kSensitivityMin, GameSettings::kSensitivityMax)) gs.SetControllerSensitivity(stickSens);
            if (ImGui::Checkbox("Player Invert Y", &invertY)) gs.SetInvertY(invertY);
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
        ImGui::Text("Underwater: %s  blend=%.2f  water Y=%.1f", IsUnderwater() ? "yes" : "no",
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
        if (ImGui::Button("Test Emerge Splash (水中→水上: 水膜が引いて水滴が流れ出る)")) {
            if (auto* postProcess = RC::GetRenderContext().GetPostProcess()) {
                StartScreenDroplets(postProcess, 1.3f, ScreenDropletSim::Mode::Surface);
            }
        }
        if (ImGui::Button("Test Dive Splash (水上→水中: 水面ラインが抜けて気泡が昇る)")) {
            if (auto* postProcess = RC::GetRenderContext().GetPostProcess()) {
                StartScreenDroplets(postProcess, -1.3f, ScreenDropletSim::Mode::Dive);
            }
        }
#endif
    }
};

// ============================================================================
// 水上 / 水中の状態（State パターン）
//   以前は isUnderwater の bool を毎フレーム位置と比べ、変わった瞬間に if / else で
//   着水・出水の演出を出していた。演出を「その状態に入った瞬間（OnEnter）」に置き、
//   水面を跨いだかの判定と次の状態の決定を各状態自身に持たせた。
//   OnCreate で最初の状態を入れるときは演出を出さない（playFx = false）。
// ============================================================================

/// @brief 水上にいる状態
class RailShooterController::SurfaceState final : public IState<RailShooterController> {
public:
    /// @brief 状態を作る
    /// @param playFx この状態に入ったときに出水の演出を出すか（OnCreate の初期状態では false）
    explicit SurfaceState(bool playFx) : playFx_(playFx) {}

    /// @brief 水中から上がってきたなら出水の演出を出す
    /// @param owner 自機
    /// @param machine ステートマシン（未使用）
    void OnEnter(RailShooterController& owner, StateMachine<RailShooterController>& machine) override {
        (void)machine;
        if (playFx_) owner.PlayEmergeFx();
    }

    /// @brief 水面より下に入ったら水中の状態へ移る
    /// @param owner 自機
    /// @param machine ステートマシン
    /// @param deltaTime 経過秒数（未使用）
    void OnUpdate(RailShooterController& owner, StateMachine<RailShooterController>& machine, float deltaTime) override;

    /// @brief 状態名
    /// @return "Surface"
    const char* Name() const override { return "Surface"; }

private:
    bool playFx_ = true; ///< 入ったときに出水の演出を出すか
};

/// @brief 水中にいる状態
class RailShooterController::UnderwaterState final : public IState<RailShooterController> {
public:
    /// @brief 状態を作る
    /// @param playFx この状態に入ったときに着水の演出を出すか（OnCreate の初期状態では false）
    explicit UnderwaterState(bool playFx) : playFx_(playFx) {}

    /// @brief 水上から飛び込んだなら着水の演出を出す
    /// @param owner 自機
    /// @param machine ステートマシン（未使用）
    void OnEnter(RailShooterController& owner, StateMachine<RailShooterController>& machine) override {
        (void)machine;
        if (playFx_) owner.PlayDiveFx();
    }

    /// @brief 水面より上に出たら水上の状態へ移る
    /// @param owner 自機
    /// @param machine ステートマシン
    /// @param deltaTime 経過秒数（未使用）
    void OnUpdate(RailShooterController& owner, StateMachine<RailShooterController>& machine, float deltaTime) override;

    /// @brief 状態名
    /// @return "Underwater"
    const char* Name() const override { return "Underwater"; }

private:
    bool playFx_ = true; ///< 入ったときに着水の演出を出すか
};

void RailShooterController::SurfaceState::OnUpdate(RailShooterController& owner,
                                                   StateMachine<RailShooterController>& machine, float deltaTime) {
    (void)deltaTime;
    if (owner.IsBelowWaterSurface()) {
        machine.ChangeState<UnderwaterState>(true);
    }
}

void RailShooterController::UnderwaterState::OnUpdate(RailShooterController& owner,
                                                      StateMachine<RailShooterController>& machine, float deltaTime) {
    (void)deltaTime;
    if (!owner.IsBelowWaterSurface()) {
        machine.ChangeState<SurfaceState>(true);
    }
}

// ============================================================================
// 生死の状態（State パターン）
//   以前は isDead / isGameCleared の 2 つの bool で、
//     - 被弾を受け付けるか、視点・射撃・墨を受け付けるか
//     - HUD の文字（スコア / STAGE CLEAR / GAME OVER）と全画面の帯（赤 / 緑 / 被弾フラッシュ）
//   を if / else の連鎖で分岐していた。これを「プレイ中 / クリア後 / 死亡」の 3 状態に分け、
//   各状態が自分の振る舞いを override する。
//     PlayingState --(レールの終点)--> ClearedState
//     PlayingState / ClearedState --(HP 0)--> DeadState（死亡からはどこへも行かない）
//   以前の実装でも「死亡」の表示が「クリア」より優先されていたので、死亡を終端にしても見た目は同じ。
// ============================================================================

/// @brief 生死の状態の基底。被弾の受け方と HUD の描き分けを持つ
class RailShooterController::LifeState : public IState<RailShooterController> {
public:
    /// @brief 視点操作・射撃・墨の受け付けなど、生きている前提の処理をしてよいか
    /// @return 生きていれば true
    virtual bool CanAct() const = 0;

    /// @brief ダメージを受ける
    /// @param owner 自機
    /// @param machine ステートマシン（死亡への遷移の予約に使う）
    /// @param damage 減らす HP 量
    virtual void TakeDamage(RailShooterController& owner, StateMachine<RailShooterController>& machine, int damage) = 0;

    /// @brief ポストプロセス後の HUD 文字を描く
    /// @param owner 自機
    /// @param layout HUD の配置
    virtual void DrawOverlayText(const RailShooterController& owner, const HudLayout& layout) const = 0;

    /// @brief 全画面の帯（赤 / 緑 / 被弾フラッシュ）を描く
    /// @param owner 自機
    /// @param screenW 画面の幅（ピクセル）
    /// @param screenH 画面の高さ（ピクセル）
    virtual void DrawScreenBand(const RailShooterController& owner, float screenW, float screenH) const = 0;
};

/// @brief 生きている状態の共通部分（プレイ中とクリア後で被弾の受け方は同じ）
class RailShooterController::AliveState : public RailShooterController::LifeState {
public:
    /// @brief 生きているので操作を受け付ける
    /// @return true
    bool CanAct() const override { return true; }

    /// @brief HP を減らし、無敵時間とカメラシェイクを開始する。HP が尽きたら死亡状態へ移る
    /// @param owner 自機
    /// @param machine ステートマシン
    /// @param damage 減らす HP 量
    /// @note 無敵時間中・見せ用デモ中は何もしない
    void TakeDamage(RailShooterController& owner, StateMachine<RailShooterController>& machine, int damage) override;
};

/// @brief プレイ中：左上にスコア、被弾直後は赤い全画面フラッシュ
class RailShooterController::PlayingState final : public RailShooterController::AliveState {
public:
    /// @brief レールの終点に着いたらクリア状態へ移る
    /// @param owner 自機
    /// @param machine ステートマシン
    /// @param deltaTime 経過秒数（未使用）
    void OnUpdate(RailShooterController& owner, StateMachine<RailShooterController>& machine, float deltaTime) override;

    /// @brief 左上にスコアを描く
    void DrawOverlayText(const RailShooterController& owner, const HudLayout& layout) const override {
        owner.DrawScoreText(layout);
    }

    /// @brief 被弾直後は無敵残り時間に比例した赤い全画面フラッシュ
    void DrawScreenBand(const RailShooterController& owner, float screenW, float screenH) const override {
        if (owner.invincibleTimer > 0.0f) {
            float flashAlpha = (owner.invincibleTimer / owner.invincibleDuration) * 0.5f;
            RC::DrawBox({ 0.0f, 0.0f }, { screenW, screenH },
                        { 1.0f, 0.0f, 0.0f, flashAlpha });
        }
    }

    /// @brief 状態名
    /// @return "Playing"
    const char* Name() const override { return "Playing"; }
};

/// @brief クリア後（レールの終点に着いた）：緑の帯に STAGE CLEAR とスコア
class RailShooterController::ClearedState final : public RailShooterController::AliveState {
public:
    /// @brief クリア後は死亡以外へは移らない（死亡は被弾で AliveState::TakeDamage が決める）
    void OnUpdate(RailShooterController& owner, StateMachine<RailShooterController>& machine, float deltaTime) override {
        (void)owner;
        (void)machine;
        (void)deltaTime;
    }

    /// @brief 帯の上に STAGE CLEAR とスコアを描く
    void DrawOverlayText(const RailShooterController& owner, const HudLayout& layout) const override {
        owner.DrawClearText(layout);
    }

    /// @brief 画面の 40%〜60% に緑の帯
    void DrawScreenBand(const RailShooterController& owner, float screenW, float screenH) const override {
        (void)owner;
        RC::DrawBox({ 0.0f, screenH * 0.4f }, { screenW, screenH * 0.6f },
                    { 0.1f, 0.8f, 0.2f, 0.6f });
    }

    /// @brief 状態名
    /// @return "Cleared"
    const char* Name() const override { return "Cleared"; }
};

/// @brief 力尽きた：GAME OVER。被弾も操作も受け付けない
class RailShooterController::DeadState final : public RailShooterController::LifeState {
public:
    /// @brief 死亡した瞬間にシーン側へ "game_over" タグで通知する（DeathSinkScript などが見る）
    void OnEnter(RailShooterController& owner, StateMachine<RailShooterController>& machine) override {
        (void)machine;
        owner.NotifyGameOver();
    }

    /// @brief 死亡からはどこへも移らない
    void OnUpdate(RailShooterController& owner, StateMachine<RailShooterController>& machine, float deltaTime) override {
        (void)owner;
        (void)machine;
        (void)deltaTime;
    }

    /// @brief 操作は受け付けない
    /// @return false
    bool CanAct() const override { return false; }

    /// @brief 死亡済みなのでダメージは受けない
    void TakeDamage(RailShooterController& owner, StateMachine<RailShooterController>& machine, int damage) override {
        (void)owner;
        (void)machine;
        (void)damage;
    }

    /// @brief 画面中央に GAME OVER だけを出す
    void DrawOverlayText(const RailShooterController& owner, const HudLayout& layout) const override {
        owner.DrawGameOverText(layout);
    }

    /// @brief 画面の 40%〜60% に赤い帯
    void DrawScreenBand(const RailShooterController& owner, float screenW, float screenH) const override {
        (void)owner;
        RC::DrawBox({ 0.0f, screenH * 0.4f }, { screenW, screenH * 0.6f },
                    { 0.8f, 0.1f, 0.1f, 0.7f });
    }

    /// @brief 状態名
    /// @return "Dead"
    const char* Name() const override { return "Dead"; }
};

void RailShooterController::AliveState::TakeDamage(RailShooterController& owner,
                                                   StateMachine<RailShooterController>& machine, int damage) {
    if (owner.invincibleTimer > 0.0f) return;
    // 見せ用デモ（ClearRunDemo）の間は被弾しない
    if (owner.GetEntity() && owner.GetEntity()->GetTagInt("demo_invincible", 0) == 1) return;
    // リザルトの「被ダメージ」。無敵中に弾いたぶんは上の早期 return で除外され、
    // HP を割り込んだぶん（オーバーキル）は数えない。
    GameSession::Get().AddDamageTaken((std::min)(damage, owner.hp));
    owner.hp -= damage;
    owner.invincibleTimer = owner.invincibleDuration;

    // 被弾の演出（射撃時の 2 倍の強さでカメラを揺らす）
    owner.currentCameraShake = owner.shakeMax * 2.0f;

    if (owner.hp <= 0) {
        owner.hp = 0;
        machine.ChangeState<DeadState>();
    }
}

void RailShooterController::PlayingState::OnUpdate(RailShooterController& owner,
                                                   StateMachine<RailShooterController>& machine, float deltaTime) {
    (void)deltaTime;
    if (owner.railFinished_) {
        machine.ChangeState<ClearedState>();
    }
}

// ============================================================================
// 状態クラスを使う RailShooterController のメンバ関数
// ============================================================================

bool RailShooterController::IsUnderwater() const {
    return waterFsm_.IsIn<UnderwaterState>();
}

bool RailShooterController::IsDead() const {
    const LifeState* life = lifeFsm_.As<LifeState>();
    return life && !life->CanAct();
}

void RailShooterController::InitWaterState(bool startUnderwater) {
    if (startUnderwater) {
        waterFsm_.ChangeState<UnderwaterState>(false);
    } else {
        waterFsm_.ChangeState<SurfaceState>(false);
    }
    waterFsm_.ApplyPending(*this);
}

void RailShooterController::EnsureLifeState() {
    if (lifeFsm_.HasState()) return;
    lifeFsm_.ChangeState<PlayingState>();
    lifeFsm_.ApplyPending(*this);
}

void RailShooterController::DrawLifeOverlayText(const HudLayout& layout) {
    EnsureLifeState();
    if (const LifeState* life = lifeFsm_.As<LifeState>()) {
        life->DrawOverlayText(*this, layout);
    }
}

void RailShooterController::DrawLifeScreenBand(float screenW, float screenH) {
    EnsureLifeState();
    if (const LifeState* life = lifeFsm_.As<LifeState>()) {
        life->DrawScreenBand(*this, screenW, screenH);
    }
}

void RailShooterController::TakeDamage(int damage) {
    EnsureLifeState();
    if (LifeState* life = lifeFsm_.As<LifeState>()) {
        life->TakeDamage(*this, lifeFsm_, damage);
    }
    // 死亡はこの場で確定させる（同じフレームの墨・視点・射撃の判定に間に合わせるため。以前と同じタイミング）
    lifeFsm_.ApplyPending(*this);
}

REGISTER_SCRIPT(RailShooterController)
