#include "EnemyBaseScript.h"
#include "ECS/ScriptRegistry.h"
#include "Common/Log/Log.h"
#include "ECS/TransformComponent.h"
#include "ECS/CameraComponent.h"
#include "ECS/ColliderComponent.h"
#include "Scene.h"
#include <iostream>
#include <algorithm>
#include <cmath>

#if RC_ENABLE_IMGUI
#include "imgui/imgui.h"
#endif

/// @brief サメ AI の行動状態
enum class SharkState {
    Wait,     ///< 水中で待機（その場で円を描いて巡回）
    Approach, ///< 自機へ接近しながら浮上（背後にいるときは前方へ回り込む）
    Attack,   ///< 向きを固定して直進で突進
    Retreat   ///< 潜って自機前方の遠い地点まで引き返す
};

/// @brief サメ型の敵 AI（待機・接近・突進・引き返しの状態遷移で動く）
/// @details 水中に潜んでいて、接近しながら水面へ浮き上がり、突進したあとは
///          潜って自機の前方遠くまで引き返す。引き返し先に着くまでは次の突進をしない。
///          （以前は自機前方 18m で折り返していたため、画面いっぱいのまま
///            2 秒おきに突進してきて逃げ場が無かった。）
class SharkEnemyScript : public EnemyBaseScript {
public:
    float detectDistance = 30.0f;
    float attackStartDistance = 10.0f;
    float attackHitDistance = 2.5f;
    float attackSpeed = 18.0f;
    float cooldownDuration = 2.0f;

    /// @brief 突進後に引き返す地点（自機前方）の距離と横のずれ
    float retreatDistance = 45.0f;
    float retreatSide = 14.0f;
    /// @brief 引き返し地点に「着いた」と見なす半径
    float retreatArriveRadius = 4.0f;
    /// @brief 引き返しに掛けてよい最大秒数（着けなくてもこれで次の接近に移る）
    float retreatMaxDuration = 8.0f;

    /// @brief 潜る深さ（水面からの距離）
    float diveDepth = 6.0f;
    /// @brief 浮上・潜行の速さ（m/s）
    float verticalSpeed = 5.0f;
    /// @brief この距離から浮上を始め、attackStartDistance で水面に出そろう
    float emergeStartDistance = 24.0f;
    /// @brief 生成時に水中から始めるか
    bool startSubmerged = true;

    // モデルの初期回転・向き補正（度数法）。shark_game.glb は頭が +X なので、Y を -90° して進行方向（+Z 基準）へ合わせる。
    // （以前の {180, 80, 180} は Rx(π)·Ry·Rz(π) ＝ Ry(π − yaw) になり、旋回すると逆向きに回っていた）
    RC::Vector3 modelRotationOffsetDeg = { 0.0f, -90.0f, 0.0f };

protected:
    /// @brief サメのパラメータを JSON に書き出す
    /// @return 基底クラスの体力に加え、索敵・泳ぎ・突進・引き返し・潜行の各パラメータを格納した JSON
    nlohmann::json Serialize() override {
        nlohmann::json j = EnemyBaseScript::Serialize(); // 体力（hp / maxHp）はここで入る
        j["detectDistance"] = detectDistance;
        j["swimSpeed"] = swimSpeed;
        j["swimSineAmplitude"] = swimSineAmplitude;
        j["swimSineFrequency"] = swimSineFrequency;
        j["attackStartDistance"] = attackStartDistance;
        j["attackHitDistance"] = attackHitDistance;
        j["attackSpeed"] = attackSpeed;
        j["cooldownDuration"] = cooldownDuration;
        j["retreatDistance"] = retreatDistance;
        j["retreatSide"] = retreatSide;
        j["retreatArriveRadius"] = retreatArriveRadius;
        j["retreatMaxDuration"] = retreatMaxDuration;
        j["diveDepth"] = diveDepth;
        j["verticalSpeed"] = verticalSpeed;
        j["emergeStartDistance"] = emergeStartDistance;
        j["startSubmerged"] = startSubmerged;
        j["modelRotOffset"] = { modelRotationOffsetDeg.x, modelRotationOffsetDeg.y, modelRotationOffsetDeg.z };
        return j;
    }

    /// @brief JSON からサメのパラメータを読み込む
    /// @param j 読み込み元の JSON（存在するキーだけ反映する）
    void Deserialize(const nlohmann::json& j) override {
        EnemyBaseScript::Deserialize(j);
        if (j.contains("detectDistance")) detectDistance = j["detectDistance"].get<float>();
        if (j.contains("swimSpeed")) swimSpeed = j["swimSpeed"].get<float>();
        if (j.contains("swimSineAmplitude")) swimSineAmplitude = j["swimSineAmplitude"].get<float>();
        if (j.contains("swimSineFrequency")) swimSineFrequency = j["swimSineFrequency"].get<float>();
        if (j.contains("attackStartDistance")) attackStartDistance = j["attackStartDistance"].get<float>();
        if (j.contains("attackHitDistance")) attackHitDistance = j["attackHitDistance"].get<float>();
        if (j.contains("attackSpeed")) attackSpeed = j["attackSpeed"].get<float>();
        if (j.contains("cooldownDuration")) cooldownDuration = j["cooldownDuration"].get<float>();
        if (j.contains("retreatDistance")) retreatDistance = j["retreatDistance"].get<float>();
        if (j.contains("retreatSide")) retreatSide = j["retreatSide"].get<float>();
        if (j.contains("retreatArriveRadius")) retreatArriveRadius = j["retreatArriveRadius"].get<float>();
        if (j.contains("retreatMaxDuration")) retreatMaxDuration = j["retreatMaxDuration"].get<float>();
        if (j.contains("diveDepth")) diveDepth = j["diveDepth"].get<float>();
        if (j.contains("verticalSpeed")) verticalSpeed = j["verticalSpeed"].get<float>();
        if (j.contains("emergeStartDistance")) emergeStartDistance = j["emergeStartDistance"].get<float>();
        if (j.contains("startSubmerged")) startSubmerged = j["startSubmerged"].get<bool>();
        if (j.contains("modelRotOffset") && j["modelRotOffset"].size() == 3) {
            modelRotationOffsetDeg.x = j["modelRotOffset"][0].get<float>();
            modelRotationOffsetDeg.y = j["modelRotOffset"][1].get<float>();
            modelRotationOffsetDeg.z = j["modelRotOffset"][2].get<float>();
        }
    }

    /// @brief 状態・既定 HP・初期の向きを設定し、生成時の高さを水面として記録する
    /// @details コライダーが無ければ球コライダーを追加する。
    void OnCreate() override {
        EnemyBaseScript::OnCreate();
        Log::Print("[SharkEnemyScript] OnCreate");
        state_ = SharkState::Wait;
        // サメの既定 HP（通常弾 3 発）。JSON やウェーブのスポナーから指定があればそちらを尊重する。
        if (!hpFromData) {
            hp = 3;
            maxHp = 3;
        }

        // モデルの向き補正（度数法）をラジアンに直して初期回転にする
        if (auto* tr = GetComponent<TransformComponent>()) {
            tr->rotation.x = modelRotationOffsetDeg.x * (3.14159265f / 180.0f);
            tr->rotation.y = modelRotationOffsetDeg.y * (3.14159265f / 180.0f);
            tr->rotation.z = modelRotationOffsetDeg.z * (3.14159265f / 180.0f);

            // 置かれた高さを「水面の高さ」として覚えておき、潜る／浮く基準にする
            surfaceY_ = tr->position.y;
            if (startSubmerged) {
                tr->position.y = surfaceY_ - diveDepth;
            }
        }

        // コライダーがなければ追加
        if (Entity* self = GetEntity()) {
            if (!self->HasComponent<ColliderComponent>()) {
                auto* col = &self->AddComponent<ColliderComponent>();
                col->shape = ColliderComponent::Shape::Sphere;
                col->radius = 1.0f; // スケールが4倍されているので半径1(直径2)でも十分大きい
                col->isTrigger = false;
            }
        }
    }

    /// @brief 自機（カメラ）との位置関係から状態を遷移させ、移動・突進・浮上／潜行を行う
    /// @param deltaTime 前フレームからの経過時間（秒）
    void OnUpdate(float deltaTime) override {
        EnemyBaseScript::OnUpdate(deltaTime);

        auto* tr = GetComponent<TransformComponent>();
        if (!tr) return;

        if (isDead) {
            // 仮の沈む処理
            tr->position.y -= 2.0f * deltaTime;
            return;
        }

        Scene* scene = GetScene();
        if (!scene) return;

        // 自機（カメラ）を探す。前回見つけたものがまだ有効ならそれを使う
        std::shared_ptr<Entity> mainCamera = cachedTarget_.lock();
        if (!mainCamera || mainCamera->IsPendingDestroy() || !mainCamera->IsActive()) {
            mainCamera = nullptr;
            for (auto& e : scene->GetEntities()) {
                if (e->HasComponent<CameraComponent>()) {
                    mainCamera = e;
                    cachedTarget_ = mainCamera;
                    break;
                }
            }
        }
        if (!mainCamera) return;

        auto* camTr = mainCamera->GetComponent<TransformComponent>();
        if (!camTr) return;

        // 自機までの距離は XZ 平面で測る（高さの差は潜行中でも無視する）
        RC::Vector3 toCam = {
            camTr->position.x - tr->position.x,
            camTr->position.y - tr->position.y,
            camTr->position.z - tr->position.z
        };
        float distToCam = std::sqrt(toCam.x * toCam.x + toCam.z * toCam.z);

        // カメラの前方にいるかどうかの判定（XZ平面での内積）
        float camCy = std::cos(camTr->rotation.y);
        float camSy = std::sin(camTr->rotation.y);
        float dotXZ = 0.0f;
        if (distToCam > 0.01f) {
            dotXZ = (camSy * (-toCam.x / distToCam)) + (camCy * (-toCam.z / distToCam));
        }
        // dotXZ > 0.2f は前方約150度以内。見えない横や後ろから攻撃されないようにする。
        bool isInFront = (dotXZ > 0.2f);

        // カメラの前方ベクトル・右方向ベクトル
        RC::Vector3 camForward = { camSy, 0.0f, camCy };
        RC::Vector3 camRight = { camCy, 0.0f, -camSy };

        // 敵が自機から見て左右どちら側にいるかを算出
        float camToEnemyX = tr->position.x - camTr->position.x;
        float camToEnemyZ = tr->position.z - camTr->position.z;
        float sideDot = camToEnemyX * camRight.x + camToEnemyZ * camRight.z;
        float sideSign = (sideDot >= 0.0f) ? 1.0f : -1.0f;

        // 自機の前方エリアにある再突入・復帰目標地点（前方 retreatDistance、横 retreatSide）
        RC::Vector3 frontTarget = {
            camTr->position.x + camForward.x * retreatDistance + camRight.x * (sideSign * retreatSide),
            camTr->position.y,
            camTr->position.z + camForward.z * retreatDistance + camRight.z * (sideSign * retreatSide)
        };

        // 高さの目標。状態ごとに決めて、最後にまとめて verticalSpeed で寄せる
        const float deepY = surfaceY_ - diveDepth;
        float targetY = tr->position.y;

        // 状態ごとの行動と遷移
        switch (state_) {
            case SharkState::Wait:
                // 水中で待機。プレイヤーが検知範囲内に入ったらApproachへ
                // 背後にいる場合でも、背後でスタックせず前方へ回り込むためApproachへ移行させる
                targetY = startSubmerged ? deepY : surfaceY_;
                if (distToCam <= detectDistance) {
                    state_ = SharkState::Approach;
                    Log::Print("[SharkEnemyScript] Detected player! Switching to Approach.");
                } else {
                    // 待機中はその場で円を描いてパトロール
                    swimTime_ += deltaTime;
                    float patrolSpeed = swimSpeed * 0.5f;
                    tr->rotation.y += patrolSpeed * 0.1f * deltaTime;
                    // rotation.y にはモデルの向き補正が含まれるので、差し引いて実際の進行方向を求める
                    float realAngleY = tr->rotation.y - (modelRotationOffsetDeg.y * (3.14159265f / 180.0f));
                    tr->position.x += std::sin(realAngleY) * patrolSpeed * deltaTime;
                    tr->position.z += std::cos(realAngleY) * patrolSpeed * deltaTime;
                }
                break;

            case SharkState::Approach:
                // 目標地点の選定：
                // 前方にいるなら自機へ向かって接近、背後にいる（!isInFront）なら自機の前方へ回り込む
                {
                    RC::Vector3 targetPos = isInFront ? camTr->position : frontTarget;
                    RC::Vector3 toTarget = {
                        targetPos.x - tr->position.x,
                        targetPos.y - tr->position.y,
                        targetPos.z - tr->position.z
                    };
                    float distToTarget = std::sqrt(toTarget.x * toTarget.x + toTarget.z * toTarget.z);

                    if (distToTarget > 0.01f) {
                        // 目標の方向を向く（モデルの向き補正を足して見た目を合わせる）
                        float targetAngleY = std::atan2(toTarget.x, toTarget.z);
                        tr->rotation.y = targetAngleY + (modelRotationOffsetDeg.y * (3.14159265f / 180.0f));

                        RC::Vector3 forward = { toTarget.x / distToTarget, 0.0f, toTarget.z / distToTarget };
                        RC::Vector3 right = { forward.z, 0.0f, -forward.x };

                        swimTime_ += deltaTime;

                        // 背後から回り込む際は少し速めに前方に復帰する
                        float currentSpeed = isInFront ? swimSpeed : (swimSpeed * 1.3f);
                        RC::Vector3 velocity = {
                            forward.x * currentSpeed,
                            0.0f,
                            forward.z * currentSpeed
                        };

                        // 進行方向と直交する向きにサイン波の速度を足して左右に蛇行させる
                        float lateralVel = std::cos(swimTime_ * swimSineFrequency) * swimSineAmplitude * swimSineFrequency;
                        velocity.x += right.x * lateralVel;
                        velocity.z += right.z * lateralVel;

                        tr->position.x += velocity.x * deltaTime;
                        tr->position.z += velocity.z * deltaTime;
                    }
                }

                // 浮上：前方にいて emergeStartDistance を切ったら、近づくにつれて水面へ上がる。
                // attackStartDistance に着く頃には水面に出そろう。背後にいる間は潜ったまま回り込む。
                if (isInFront) {
                    float span = (std::max)(emergeStartDistance - attackStartDistance, 0.01f);
                    float t = (emergeStartDistance - distToCam) / span;
                    t = std::clamp(t, 0.0f, 1.0f);
                    targetY = deepY + (surfaceY_ - deepY) * t;
                } else {
                    targetY = deepY;
                }

                // 攻撃開始条件：自機の前方（isInFront）にいて、かつ攻撃開始距離に入った時のみ突進開始！
                if (distToCam <= attackStartDistance && isInFront) {
                    state_ = SharkState::Attack;
                    attackTimer_ = 0.0f;
                    hasHit_ = false;
                    Log::Print("[SharkEnemyScript] Approaching -> Attack!");
                }
                break;

            case SharkState::Attack:
                // 追尾（回転）をやめ、現在の向きに直進して突進する
                {
                    float realAngleY = tr->rotation.y - (modelRotationOffsetDeg.y * (3.14159265f / 180.0f));
                    float s = std::sin(realAngleY);
                    float c = std::cos(realAngleY);
                    RC::Vector3 forward = { s, 0.0f, c };

                    tr->position.x += forward.x * attackSpeed * deltaTime;
                    tr->position.z += forward.z * attackSpeed * deltaTime;
                }
                targetY = surfaceY_;

                attackTimer_ += deltaTime;

                // 攻撃ヒット距離に入ったらダメージ処理（1回のみ）
                if (!hasHit_ && distToCam <= attackHitDistance) {
                    Log::Print("[SharkEnemyScript] Player HIT! Damage triggered.");
                    mainCamera->SetTag("pending_damage", 1);
                    hasHit_ = true;
                }

                // 攻撃終了（引き返し移行）判定：
                // 1. 自機の真横〜背後に抜けた（dotXZ < -0.05f）
                // 2. ダメージを与えて少しすれ違った（hit後 0.25秒経過）
                // 3. 最大突進時間（maxAttackDuration）経過
                {
                    bool passedPlayer = (dotXZ < -0.05f);
                    bool hitAndPast = (hasHit_ && attackTimer_ >= 0.25f);
                    if (passedPlayer || hitAndPast || attackTimer_ >= maxAttackDuration) {
                        Log::Print("[SharkEnemyScript] Attack Finished! To Retreat.");
                        state_ = SharkState::Retreat;
                        cooldownTimer_ = cooldownDuration;
                        retreatTimer_ = 0.0f;
                    }
                }
                break;

            case SharkState::Retreat:
                // 潜って、自機前方の遠い再突入ポイント（frontTarget）へ引き返す。
                // 着くまで（または retreatMaxDuration 経過まで）は次の接近に入らないので、
                // 突進のたびに一度きちんと距離が開く。
                {
                    RC::Vector3 toTarget = {
                        frontTarget.x - tr->position.x,
                        frontTarget.y - tr->position.y,
                        frontTarget.z - tr->position.z
                    };
                    float distToTarget = std::sqrt(toTarget.x * toTarget.x + toTarget.z * toTarget.z);

                    if (distToTarget > 0.5f) {
                        RC::Vector3 forward = { toTarget.x / distToTarget, 0.0f, toTarget.z / distToTarget };
                        tr->position.x += forward.x * (swimSpeed * 1.5f) * deltaTime;
                        tr->position.z += forward.z * (swimSpeed * 1.5f) * deltaTime;

                        // 移動方向を向かせる
                        float targetAngleY = std::atan2(toTarget.x, toTarget.z);
                        tr->rotation.y = targetAngleY + (modelRotationOffsetDeg.y * (3.14159265f / 180.0f));
                    }
                    targetY = deepY;

                    cooldownTimer_ -= deltaTime;
                    retreatTimer_ += deltaTime;
                    bool arrived = (distToTarget <= retreatArriveRadius);
                    bool timedOut = (retreatTimer_ >= retreatMaxDuration);
                    if (cooldownTimer_ <= 0.0f && (arrived || timedOut)) {
                        // 引き返し完了。再び Approach に戻り、近づきながら浮上する
                        state_ = SharkState::Approach;
                        Log::Print("[SharkEnemyScript] Retreat done. Back to Approach.");
                    }
                }
                break;
        }

        // 高さを目標へ寄せる（潜る／浮き上がる）
        {
            float dy = targetY - tr->position.y;
            float step = verticalSpeed * deltaTime;
            if (std::fabs(dy) <= step) {
                tr->position.y = targetY;
            } else {
                tr->position.y += (dy > 0.0f ? step : -step);
            }
        }
    }

    /// @brief 破棄されたことをログに出す
    void OnDestroy() override {
        Log::Print("[SharkEnemyScript] OnDestroy");
    }

public:
    /// @brief 基底クラスの表示に加え、サメの AI パラメータ編集と現在の状態をインスペクタに出す
    void OnImGui() override {
        EnemyBaseScript::OnImGui();
#if RC_ENABLE_IMGUI
        ImGui::DragFloat("Detect Dist##Shark", &detectDistance, 0.5f, 5.0f, 100.0f);
        ImGui::DragFloat("Swim Speed##Shark", &swimSpeed, 0.5f, 1.0f, 50.0f);
        ImGui::DragFloat("Swim Sine Amp##Shark", &swimSineAmplitude, 0.1f, 0.0f, 10.0f);
        ImGui::DragFloat("Swim Sine Freq##Shark", &swimSineFrequency, 0.1f, 0.1f, 10.0f);
        ImGui::DragFloat("Attack Start Dist##Shark", &attackStartDistance, 0.5f, 2.0f, 50.0f);
        ImGui::DragFloat("Attack Hit Dist##Shark", &attackHitDistance, 0.1f, 0.5f, 10.0f);
        ImGui::DragFloat("Attack Speed##Shark", &attackSpeed, 0.5f, 1.0f, 100.0f);
        ImGui::DragFloat("Cooldown Dur##Shark", &cooldownDuration, 0.1f, 0.5f, 10.0f);
        ImGui::Separator();
        ImGui::DragFloat("Retreat Dist##Shark", &retreatDistance, 0.5f, 10.0f, 120.0f);
        ImGui::DragFloat("Retreat Side##Shark", &retreatSide, 0.5f, 0.0f, 40.0f);
        ImGui::DragFloat("Retreat Arrive R##Shark", &retreatArriveRadius, 0.5f, 0.5f, 20.0f);
        ImGui::DragFloat("Retreat Max Dur##Shark", &retreatMaxDuration, 0.1f, 1.0f, 30.0f);
        ImGui::Separator();
        ImGui::DragFloat("Dive Depth##Shark", &diveDepth, 0.1f, 0.0f, 30.0f);
        ImGui::DragFloat("Vertical Speed##Shark", &verticalSpeed, 0.1f, 0.5f, 30.0f);
        ImGui::DragFloat("Emerge Start Dist##Shark", &emergeStartDistance, 0.5f, 2.0f, 80.0f);
        ImGui::Checkbox("Start Submerged##Shark", &startSubmerged);
        ImGui::Text("State: %s  surfaceY: %.2f", StateName(state_), surfaceY_);
        ImGui::DragFloat3("Model Rot Offset##Shark", &modelRotationOffsetDeg.x, 1.0f, -360.0f, 360.0f);
#endif
    }

private:
    /// @brief 状態を表示用の文字列に変換する
    /// @param s 変換する状態
    /// @return 状態名（未知の値なら "?"）
    static const char* StateName(SharkState s) {
        switch (s) {
            case SharkState::Wait: return "Wait";
            case SharkState::Approach: return "Approach";
            case SharkState::Attack: return "Attack";
            case SharkState::Retreat: return "Retreat";
        }
        return "?";
    }

    SharkState state_ = SharkState::Wait;
    std::weak_ptr<Entity> cachedTarget_;

    // 泳ぎのパラメータと状態ごとのタイマー
    float swimSpeed = 6.0f;
    float swimSineAmplitude = 3.0f;
    float swimSineFrequency = 2.0f;
    float swimTime_ = 0.0f;
    float cooldownTimer_ = 0.0f;
    float retreatTimer_ = 0.0f;
    float attackTimer_ = 0.0f;
    float maxAttackDuration = 2.0f;
    bool hasHit_ = false;
    float surfaceY_ = 0.0f; ///< 生成時の高さ＝水面の高さとして扱う
};

REGISTER_SCRIPT(SharkEnemyScript)
