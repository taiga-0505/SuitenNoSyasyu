#include "EnemyBaseScript.h"
#include "ECS/ScriptRegistry.h"
#include "Common/Log/Log.h"
#include "ECS/TransformComponent.h"
#include "ECS/CameraComponent.h"
#include "ECS/ColliderComponent.h"
#include "Scene.h"
#include "Game/Framework/StateMachine.h"
#include <iostream>
#include <algorithm>
#include <cmath>
#include <memory>

#if RC_ENABLE_IMGUI
#include "imgui/imgui.h"
#endif

/// @brief サメ型の敵 AI（待機・接近・突進・引き返しの状態遷移で動く）
/// @details 水中に潜んでいて、接近しながら水面へ浮き上がり、突進したあとは
///          潜って自機の前方遠くまで引き返す。引き返し先に着くまでは次の突進をしない。
///          （以前は自機前方 18m で折り返していたため、画面いっぱいのまま
///            2 秒おきに突進してきて逃げ場が無かった。）
///
///          行動の切り替えは State Pattern で書いている。以前は enum SharkState と
///          1 つの巨大な switch で、状態ごとのタイマー（突進時間・クールダウン等）が
///          すべて本体のメンバに並んでいた。状態ごとにクラスを分けたことで、
///          各状態の処理・使う変数・次の状態への遷移条件が 1 か所にまとまった。
///            Wait → Approach → Attack → Retreat → Approach → ...
///          毎フレームの「自機との位置関係」は本体がまとめて計算して sense_ に入れ、
///          状態クラスはそれを読んで動く（高さの目標 targetY だけは状態が書き込む）。
class SharkEnemyScript : public EnemyBaseScript {
public:
    /// @brief 基底クラスの表示に加え、サメの AI パラメータ編集と現在の状態をインスペクタに出す
    void OnImGui() override {
        EnemyBaseScript::OnImGui();
#if RC_ENABLE_IMGUI
        ImGui::DragFloat("Detect Dist##Shark", &detectDistance_, 0.5f, 5.0f, 100.0f);
        ImGui::DragFloat("Swim Speed##Shark", &swimSpeed_, 0.5f, 1.0f, 50.0f);
        ImGui::DragFloat("Swim Sine Amp##Shark", &swimSineAmplitude_, 0.1f, 0.0f, 10.0f);
        ImGui::DragFloat("Swim Sine Freq##Shark", &swimSineFrequency_, 0.1f, 0.1f, 10.0f);
        ImGui::DragFloat("Attack Start Dist##Shark", &attackStartDistance_, 0.5f, 2.0f, 50.0f);
        ImGui::DragFloat("Attack Hit Dist##Shark", &attackHitDistance_, 0.1f, 0.5f, 10.0f);
        ImGui::DragFloat("Attack Speed##Shark", &attackSpeed_, 0.5f, 1.0f, 100.0f);
        ImGui::DragFloat("Cooldown Dur##Shark", &cooldownDuration_, 0.1f, 0.5f, 10.0f);
        ImGui::Separator();
        ImGui::DragFloat("Retreat Dist##Shark", &retreatDistance_, 0.5f, 10.0f, 120.0f);
        ImGui::DragFloat("Retreat Side##Shark", &retreatSide_, 0.5f, 0.0f, 40.0f);
        ImGui::DragFloat("Retreat Arrive R##Shark", &retreatArriveRadius_, 0.5f, 0.5f, 20.0f);
        ImGui::DragFloat("Retreat Max Dur##Shark", &retreatMaxDuration_, 0.1f, 1.0f, 30.0f);
        ImGui::Separator();
        ImGui::DragFloat("Dive Depth##Shark", &diveDepth_, 0.1f, 0.0f, 30.0f);
        ImGui::DragFloat("Vertical Speed##Shark", &verticalSpeed_, 0.1f, 0.5f, 30.0f);
        ImGui::DragFloat("Emerge Start Dist##Shark", &emergeStartDistance_, 0.5f, 2.0f, 80.0f);
        ImGui::Checkbox("Start Submerged##Shark", &startSubmerged_);
        ImGui::Text("State: %s  surfaceY: %.2f", fsm_.CurrentName(), surfaceY_);
        ImGui::DragFloat3("Model Rot Offset##Shark", &modelRotationOffsetDeg_.x, 1.0f, -360.0f, 360.0f);
#endif
    }

protected:
    /// @brief サメのパラメータを JSON に書き出す
    /// @return 基底クラスの体力に加え、索敵・泳ぎ・突進・引き返し・潜行の各パラメータを格納した JSON
    nlohmann::json Serialize() override {
        nlohmann::json j = EnemyBaseScript::Serialize(); // 体力（hp / maxHp）はここで入る
        j["detectDistance"] = detectDistance_;
        j["swimSpeed"] = swimSpeed_;
        j["swimSineAmplitude"] = swimSineAmplitude_;
        j["swimSineFrequency"] = swimSineFrequency_;
        j["attackStartDistance"] = attackStartDistance_;
        j["attackHitDistance"] = attackHitDistance_;
        j["attackSpeed"] = attackSpeed_;
        j["cooldownDuration"] = cooldownDuration_;
        j["retreatDistance"] = retreatDistance_;
        j["retreatSide"] = retreatSide_;
        j["retreatArriveRadius"] = retreatArriveRadius_;
        j["retreatMaxDuration"] = retreatMaxDuration_;
        j["diveDepth"] = diveDepth_;
        j["verticalSpeed"] = verticalSpeed_;
        j["emergeStartDistance"] = emergeStartDistance_;
        j["startSubmerged"] = startSubmerged_;
        j["modelRotOffset"] = { modelRotationOffsetDeg_.x, modelRotationOffsetDeg_.y, modelRotationOffsetDeg_.z };
        return j;
    }

    /// @brief JSON からサメのパラメータを読み込む
    /// @param j 読み込み元の JSON（存在するキーだけ反映する）
    void Deserialize(const nlohmann::json& j) override {
        EnemyBaseScript::Deserialize(j);
        if (j.contains("detectDistance")) detectDistance_ = j["detectDistance"].get<float>();
        if (j.contains("swimSpeed")) swimSpeed_ = j["swimSpeed"].get<float>();
        if (j.contains("swimSineAmplitude")) swimSineAmplitude_ = j["swimSineAmplitude"].get<float>();
        if (j.contains("swimSineFrequency")) swimSineFrequency_ = j["swimSineFrequency"].get<float>();
        if (j.contains("attackStartDistance")) attackStartDistance_ = j["attackStartDistance"].get<float>();
        if (j.contains("attackHitDistance")) attackHitDistance_ = j["attackHitDistance"].get<float>();
        if (j.contains("attackSpeed")) attackSpeed_ = j["attackSpeed"].get<float>();
        if (j.contains("cooldownDuration")) cooldownDuration_ = j["cooldownDuration"].get<float>();
        if (j.contains("retreatDistance")) retreatDistance_ = j["retreatDistance"].get<float>();
        if (j.contains("retreatSide")) retreatSide_ = j["retreatSide"].get<float>();
        if (j.contains("retreatArriveRadius")) retreatArriveRadius_ = j["retreatArriveRadius"].get<float>();
        if (j.contains("retreatMaxDuration")) retreatMaxDuration_ = j["retreatMaxDuration"].get<float>();
        if (j.contains("diveDepth")) diveDepth_ = j["diveDepth"].get<float>();
        if (j.contains("verticalSpeed")) verticalSpeed_ = j["verticalSpeed"].get<float>();
        if (j.contains("emergeStartDistance")) emergeStartDistance_ = j["emergeStartDistance"].get<float>();
        if (j.contains("startSubmerged")) startSubmerged_ = j["startSubmerged"].get<bool>();
        if (j.contains("modelRotOffset") && j["modelRotOffset"].size() == 3) {
            modelRotationOffsetDeg_.x = j["modelRotOffset"][0].get<float>();
            modelRotationOffsetDeg_.y = j["modelRotOffset"][1].get<float>();
            modelRotationOffsetDeg_.z = j["modelRotOffset"][2].get<float>();
        }
    }

    /// @brief 状態・既定 HP・初期の向きを設定し、生成時の高さを水面として記録する
    /// @details 状態クラスが完全型になったあとで定義する必要があるため、本体はクラス定義の外に書く。
    void OnCreate() override;

    /// @brief 撃破後の処理（仮の沈む処理）
    /// @param deltaTime 前フレームからの経過時間（秒）
    void OnUpdateDefeated(float deltaTime) override {
        auto* tr = GetComponent<TransformComponent>();
        if (!tr) return;
        // 仮の沈む処理
        tr->position.y -= 2.0f * deltaTime;
    }

    /// @brief 自機（カメラ）との位置関係を計算し、現在の状態に行動を任せたあと、高さを目標へ寄せる
    /// @param deltaTime 前フレームからの経過時間（秒）
    void OnUpdateAlive(float deltaTime) override {
        auto* tr = GetComponent<TransformComponent>();
        if (!tr) return;

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

        // カメラの前方ベクトル・右方向ベクトル
        RC::Vector3 camForward = { camSy, 0.0f, camCy };
        RC::Vector3 camRight = { camCy, 0.0f, -camSy };

        // 敵が自機から見て左右どちら側にいるかを算出
        float camToEnemyX = tr->position.x - camTr->position.x;
        float camToEnemyZ = tr->position.z - camTr->position.z;
        float sideDot = camToEnemyX * camRight.x + camToEnemyZ * camRight.z;
        float sideSign = (sideDot >= 0.0f) ? 1.0f : -1.0f;

        // 状態クラスへ渡す「このフレームの位置関係」をまとめる
        sense_.tr = tr;
        sense_.camera = mainCamera.get();
        sense_.camTr = camTr;
        sense_.distToCam = distToCam;
        sense_.dotXZ = dotXZ;
        // dotXZ > 0.2f は前方約150度以内。見えない横や後ろから攻撃されないようにする。
        sense_.isInFront = (dotXZ > 0.2f);
        // 自機の前方エリアにある再突入・復帰目標地点（前方 retreatDistance、横 retreatSide）
        sense_.frontTarget = {
            camTr->position.x + camForward.x * retreatDistance_ + camRight.x * (sideSign * retreatSide_),
            camTr->position.y,
            camTr->position.z + camForward.z * retreatDistance_ + camRight.z * (sideSign * retreatSide_)
        };
        // 高さの目標。状態ごとに決めて、最後にまとめて verticalSpeed で寄せる
        sense_.deepY = surfaceY_ - diveDepth_;
        sense_.targetY = tr->position.y;

        // 状態ごとの行動と遷移（遷移先は各状態クラスが決める）
        fsm_.Update(*this, deltaTime);

        // 高さを目標へ寄せる（潜る／浮き上がる）
        {
            float dy = sense_.targetY - tr->position.y;
            float step = verticalSpeed_ * deltaTime;
            if (std::fabs(dy) <= step) {
                tr->position.y = sense_.targetY;
            } else {
                tr->position.y += (dy > 0.0f ? step : -step);
            }
        }

        // このフレーム限りのポインタを持ち越さない
        sense_.tr = nullptr;
        sense_.camera = nullptr;
        sense_.camTr = nullptr;
    }

    /// @brief 破棄されたことをログに出す
    void OnDestroy() override {
        Log::Print("[SharkEnemyScript] OnDestroy");
    }

private:
    // --- 状態クラス（private な入れ子クラスなので friend 無しで本体の private メンバを使える） ---
    class WaitState;     ///< 水中で待機（その場で円を描いて巡回）
    class ApproachState; ///< 自機へ接近しながら浮上（背後にいるときは前方へ回り込む）
    class AttackState;   ///< 向きを固定して直進で突進
    class RetreatState;  ///< 潜って自機前方の遠い地点まで引き返す

    /// @brief 1 フレーム分の「自機との位置関係」。本体が計算し、状態クラスが読む
    /// @details 不変条件の無い受け渡し用のデータなので struct のままにしている。
    struct Sense {
        TransformComponent* tr = nullptr;           ///< 自身の Transform
        Entity* camera = nullptr;                   ///< 自機（カメラ）のエンティティ
        const TransformComponent* camTr = nullptr;  ///< 自機（カメラ）の Transform
        float distToCam = 0.0f;                     ///< 自機までの XZ 距離
        float dotXZ = 0.0f;                         ///< 自機の前方と「自機→自分」の内積
        bool isInFront = false;                     ///< 自機の前方（約 150 度以内）にいるか
        RC::Vector3 frontTarget = {0.0f, 0.0f, 0.0f}; ///< 自機前方の再突入・復帰目標地点
        float deepY = 0.0f;                         ///< 潜ったときの高さ
        float targetY = 0.0f;                       ///< 高さの目標（状態クラスが書き込む）
    };

    /// @brief モデルの向き補正（Y、ラジアン）
    /// @return modelRotationOffsetDeg_.y をラジアンにした値
    float ModelYawOffsetRad() const {
        return modelRotationOffsetDeg_.y * (3.14159265f / 180.0f);
    }

    // --- パラメータ（シーン JSON / インスペクタで設定する） ---
    float detectDistance_ = 30.0f;      ///< この距離に入ったら接近を始める
    float attackStartDistance_ = 10.0f; ///< この距離に入ったら突進を始める
    float attackHitDistance_ = 2.5f;    ///< 突進中、この距離に入ったら自機にダメージ
    float attackSpeed_ = 18.0f;         ///< 突進の速さ
    float cooldownDuration_ = 2.0f;     ///< 突進後、次の接近に移れるまでの最短秒数

    float retreatDistance_ = 45.0f;     ///< 突進後に引き返す地点（自機前方）の距離
    float retreatSide_ = 14.0f;         ///< 引き返す地点の横のずれ
    float retreatArriveRadius_ = 4.0f;  ///< 引き返し地点に「着いた」と見なす半径
    float retreatMaxDuration_ = 8.0f;   ///< 引き返しに掛けてよい最大秒数（着けなくてもこれで次の接近に移る）

    float diveDepth_ = 6.0f;            ///< 潜る深さ（水面からの距離）
    float verticalSpeed_ = 5.0f;        ///< 浮上・潜行の速さ（m/s）
    float emergeStartDistance_ = 24.0f; ///< この距離から浮上を始め、attackStartDistance で水面に出そろう
    bool startSubmerged_ = true;        ///< 生成時に水中から始めるか

    /// @brief モデルの初期回転・向き補正（度数法）
    /// @details shark_game.glb は頭が +X なので、Y を -90° して進行方向（+Z 基準）へ合わせる。
    ///          （以前の {180, 80, 180} は Rx(π)·Ry·Rz(π) ＝ Ry(π − yaw) になり、旋回すると逆向きに回っていた）
    RC::Vector3 modelRotationOffsetDeg_ = { 0.0f, -90.0f, 0.0f };

    // 泳ぎのパラメータ
    float swimSpeed_ = 6.0f;            ///< 泳ぐ速さ
    float swimSineAmplitude_ = 3.0f;    ///< 蛇行の振れ幅
    float swimSineFrequency_ = 2.0f;    ///< 蛇行の周波数
    float maxAttackDuration_ = 2.0f;    ///< 突進を続けてよい最大秒数

    // --- 実行時の状態 ---
    StateMachine<SharkEnemyScript> fsm_;  ///< 行動のステートマシン
    Sense sense_;                         ///< このフレームの位置関係（状態クラスが読む）
    std::weak_ptr<Entity> cachedTarget_;  ///< 自機（カメラ）のキャッシュ
    float swimTime_ = 0.0f;               ///< 蛇行の位相に使う経過時間（待機・接近で共有）
    float surfaceY_ = 0.0f;               ///< 生成時の高さ＝水面の高さとして扱う
};

// =====================================================================
// 状態クラス
// =====================================================================

/// @brief 水中で待機（その場で円を描いて巡回）。自機が検知範囲に入ったら接近へ
class SharkEnemyScript::WaitState final : public IState<SharkEnemyScript> {
public:
    /// @brief 待機中の巡回と、接近への遷移判定
    /// @param owner サメ本体
    /// @param machine ステートマシン
    /// @param deltaTime 前フレームからの経過秒数
    void OnUpdate(SharkEnemyScript& owner, StateMachine<SharkEnemyScript>& machine, float deltaTime) override;

    /// @brief 状態名
    /// @return "Wait"
    const char* Name() const override { return "Wait"; }
};

/// @brief 自機へ接近しながら浮上（背後にいるときは前方へ回り込む）。前方の攻撃開始距離で突進へ
class SharkEnemyScript::ApproachState final : public IState<SharkEnemyScript> {
public:
    /// @brief 接近・回り込み・浮上と、突進への遷移判定
    /// @param owner サメ本体
    /// @param machine ステートマシン
    /// @param deltaTime 前フレームからの経過秒数
    void OnUpdate(SharkEnemyScript& owner, StateMachine<SharkEnemyScript>& machine, float deltaTime) override;

    /// @brief 状態名
    /// @return "Approach"
    const char* Name() const override { return "Approach"; }
};

/// @brief 向きを固定して直進で突進。すれ違うか時間切れで引き返しへ
class SharkEnemyScript::AttackState final : public IState<SharkEnemyScript> {
public:
    /// @brief 突進・命中判定と、引き返しへの遷移判定
    /// @param owner サメ本体
    /// @param machine ステートマシン
    /// @param deltaTime 前フレームからの経過秒数
    void OnUpdate(SharkEnemyScript& owner, StateMachine<SharkEnemyScript>& machine, float deltaTime) override;

    /// @brief 状態名
    /// @return "Attack"
    const char* Name() const override { return "Attack"; }

private:
    float attackTimer_ = 0.0f; ///< 突進を始めてからの秒数
    bool hasHit_ = false;      ///< この突進で既に命中したか（ダメージは 1 回だけ）
};

/// @brief 潜って自機前方の遠い地点まで引き返す。着いたら（または時間切れで）再び接近へ
class SharkEnemyScript::RetreatState final : public IState<SharkEnemyScript> {
public:
    /// @brief クールダウンを開始する
    /// @param owner サメ本体
    /// @param machine ステートマシン
    void OnEnter(SharkEnemyScript& owner, StateMachine<SharkEnemyScript>& machine) override {
        (void)machine;
        cooldownTimer_ = owner.cooldownDuration_;
        retreatTimer_ = 0.0f;
    }

    /// @brief 引き返し移動と、接近への遷移判定
    /// @param owner サメ本体
    /// @param machine ステートマシン
    /// @param deltaTime 前フレームからの経過秒数
    void OnUpdate(SharkEnemyScript& owner, StateMachine<SharkEnemyScript>& machine, float deltaTime) override;

    /// @brief 状態名
    /// @return "Retreat"
    const char* Name() const override { return "Retreat"; }

private:
    float cooldownTimer_ = 0.0f; ///< 次の接近に移れるまでの残り秒数
    float retreatTimer_ = 0.0f;  ///< 引き返しを始めてからの秒数
};

// ---------------------------------------------------------------------
// 状態クラスの処理（すべての状態クラスが完全型になったあとで定義する）
// ---------------------------------------------------------------------

void SharkEnemyScript::WaitState::OnUpdate(SharkEnemyScript& owner, StateMachine<SharkEnemyScript>& machine,
                                           float deltaTime) {
    Sense& s = owner.sense_;
    // 水中で待機。プレイヤーが検知範囲内に入ったらApproachへ
    // 背後にいる場合でも、背後でスタックせず前方へ回り込むためApproachへ移行させる
    s.targetY = owner.startSubmerged_ ? s.deepY : owner.surfaceY_;
    if (s.distToCam <= owner.detectDistance_) {
        machine.ChangeState<ApproachState>();
        Log::Print("[SharkEnemyScript] Detected player! Switching to Approach.");
    } else {
        // 待機中はその場で円を描いてパトロール
        TransformComponent* tr = s.tr;
        owner.swimTime_ += deltaTime;
        float patrolSpeed = owner.swimSpeed_ * 0.5f;
        tr->rotation.y += patrolSpeed * 0.1f * deltaTime;
        // rotation.y にはモデルの向き補正が含まれるので、差し引いて実際の進行方向を求める
        float realAngleY = tr->rotation.y - owner.ModelYawOffsetRad();
        tr->position.x += std::sin(realAngleY) * patrolSpeed * deltaTime;
        tr->position.z += std::cos(realAngleY) * patrolSpeed * deltaTime;
    }
}

void SharkEnemyScript::ApproachState::OnUpdate(SharkEnemyScript& owner, StateMachine<SharkEnemyScript>& machine,
                                               float deltaTime) {
    Sense& s = owner.sense_;
    TransformComponent* tr = s.tr;

    // 目標地点の選定：
    // 前方にいるなら自機へ向かって接近、背後にいる（!isInFront）なら自機の前方へ回り込む
    {
        RC::Vector3 targetPos = s.isInFront ? s.camTr->position : s.frontTarget;
        RC::Vector3 toTarget = {
            targetPos.x - tr->position.x,
            targetPos.y - tr->position.y,
            targetPos.z - tr->position.z
        };
        float distToTarget = std::sqrt(toTarget.x * toTarget.x + toTarget.z * toTarget.z);

        if (distToTarget > 0.01f) {
            // 目標の方向を向く（モデルの向き補正を足して見た目を合わせる）
            float targetAngleY = std::atan2(toTarget.x, toTarget.z);
            tr->rotation.y = targetAngleY + owner.ModelYawOffsetRad();

            RC::Vector3 forward = { toTarget.x / distToTarget, 0.0f, toTarget.z / distToTarget };
            RC::Vector3 right = { forward.z, 0.0f, -forward.x };

            owner.swimTime_ += deltaTime;

            // 背後から回り込む際は少し速めに前方に復帰する
            float currentSpeed = s.isInFront ? owner.swimSpeed_ : (owner.swimSpeed_ * 1.3f);
            RC::Vector3 velocity = {
                forward.x * currentSpeed,
                0.0f,
                forward.z * currentSpeed
            };

            // 進行方向と直交する向きにサイン波の速度を足して左右に蛇行させる
            float lateralVel = std::cos(owner.swimTime_ * owner.swimSineFrequency_) * owner.swimSineAmplitude_ *
                               owner.swimSineFrequency_;
            velocity.x += right.x * lateralVel;
            velocity.z += right.z * lateralVel;

            tr->position.x += velocity.x * deltaTime;
            tr->position.z += velocity.z * deltaTime;
        }
    }

    // 浮上：前方にいて emergeStartDistance を切ったら、近づくにつれて水面へ上がる。
    // attackStartDistance に着く頃には水面に出そろう。背後にいる間は潜ったまま回り込む。
    if (s.isInFront) {
        float span = (std::max)(owner.emergeStartDistance_ - owner.attackStartDistance_, 0.01f);
        float t = (owner.emergeStartDistance_ - s.distToCam) / span;
        t = std::clamp(t, 0.0f, 1.0f);
        s.targetY = s.deepY + (owner.surfaceY_ - s.deepY) * t;
    } else {
        s.targetY = s.deepY;
    }

    // 攻撃開始条件：自機の前方（isInFront）にいて、かつ攻撃開始距離に入った時のみ突進開始！
    // （突進時間と命中フラグは AttackState が生成時に 0 / false で持つ）
    if (s.distToCam <= owner.attackStartDistance_ && s.isInFront) {
        machine.ChangeState<AttackState>();
        Log::Print("[SharkEnemyScript] Approaching -> Attack!");
    }
}

void SharkEnemyScript::AttackState::OnUpdate(SharkEnemyScript& owner, StateMachine<SharkEnemyScript>& machine,
                                             float deltaTime) {
    Sense& s = owner.sense_;
    TransformComponent* tr = s.tr;

    // 追尾（回転）をやめ、現在の向きに直進して突進する
    {
        float realAngleY = tr->rotation.y - owner.ModelYawOffsetRad();
        float sn = std::sin(realAngleY);
        float c = std::cos(realAngleY);
        RC::Vector3 forward = { sn, 0.0f, c };

        tr->position.x += forward.x * owner.attackSpeed_ * deltaTime;
        tr->position.z += forward.z * owner.attackSpeed_ * deltaTime;
    }
    s.targetY = owner.surfaceY_;

    attackTimer_ += deltaTime;

    // 攻撃ヒット距離に入ったらダメージ処理（1回のみ）
    if (!hasHit_ && s.distToCam <= owner.attackHitDistance_) {
        Log::Print("[SharkEnemyScript] Player HIT! Damage triggered.");
        s.camera->SetTag("pending_damage", 1);
        hasHit_ = true;
    }

    // 攻撃終了（引き返し移行）判定：
    // 1. 自機の真横〜背後に抜けた（dotXZ < -0.05f）
    // 2. ダメージを与えて少しすれ違った（hit後 0.25秒経過）
    // 3. 最大突進時間（maxAttackDuration）経過
    bool passedPlayer = (s.dotXZ < -0.05f);
    bool hitAndPast = (hasHit_ && attackTimer_ >= 0.25f);
    if (passedPlayer || hitAndPast || attackTimer_ >= owner.maxAttackDuration_) {
        Log::Print("[SharkEnemyScript] Attack Finished! To Retreat.");
        machine.ChangeState<RetreatState>(); // クールダウンは RetreatState::OnEnter で始める
    }
}

void SharkEnemyScript::RetreatState::OnUpdate(SharkEnemyScript& owner, StateMachine<SharkEnemyScript>& machine,
                                              float deltaTime) {
    Sense& s = owner.sense_;
    TransformComponent* tr = s.tr;

    // 潜って、自機前方の遠い再突入ポイント（frontTarget）へ引き返す。
    // 着くまで（または retreatMaxDuration 経過まで）は次の接近に入らないので、
    // 突進のたびに一度きちんと距離が開く。
    RC::Vector3 toTarget = {
        s.frontTarget.x - tr->position.x,
        s.frontTarget.y - tr->position.y,
        s.frontTarget.z - tr->position.z
    };
    float distToTarget = std::sqrt(toTarget.x * toTarget.x + toTarget.z * toTarget.z);

    if (distToTarget > 0.5f) {
        RC::Vector3 forward = { toTarget.x / distToTarget, 0.0f, toTarget.z / distToTarget };
        tr->position.x += forward.x * (owner.swimSpeed_ * 1.5f) * deltaTime;
        tr->position.z += forward.z * (owner.swimSpeed_ * 1.5f) * deltaTime;

        // 移動方向を向かせる
        float targetAngleY = std::atan2(toTarget.x, toTarget.z);
        tr->rotation.y = targetAngleY + owner.ModelYawOffsetRad();
    }
    s.targetY = s.deepY;

    cooldownTimer_ -= deltaTime;
    retreatTimer_ += deltaTime;
    bool arrived = (distToTarget <= owner.retreatArriveRadius_);
    bool timedOut = (retreatTimer_ >= owner.retreatMaxDuration_);
    if (cooldownTimer_ <= 0.0f && (arrived || timedOut)) {
        // 引き返し完了。再び Approach に戻り、近づきながら浮上する
        machine.ChangeState<ApproachState>();
        Log::Print("[SharkEnemyScript] Retreat done. Back to Approach.");
    }
}

// ---------------------------------------------------------------------
// 本体の処理のうち、状態クラスの完全型が必要なもの
// ---------------------------------------------------------------------

void SharkEnemyScript::OnCreate() {
    EnemyBaseScript::OnCreate(); // 敵タグ・コライダーが無ければ既定の球コライダーを追加
    Log::Print("[SharkEnemyScript] OnCreate");
    fsm_.ChangeState<WaitState>();
    fsm_.ApplyPending(*this);
    // サメの既定 HP（通常弾 3 発）。JSON やウェーブのスポナーから指定があればそちらを尊重する。
    SetDefaultHp(3);

    // モデルの向き補正（度数法）をラジアンに直して初期回転にする
    if (auto* tr = GetComponent<TransformComponent>()) {
        tr->rotation.x = modelRotationOffsetDeg_.x * (3.14159265f / 180.0f);
        tr->rotation.y = modelRotationOffsetDeg_.y * (3.14159265f / 180.0f);
        tr->rotation.z = modelRotationOffsetDeg_.z * (3.14159265f / 180.0f);

        // 置かれた高さを「水面の高さ」として覚えておき、潜る／浮く基準にする
        surfaceY_ = tr->position.y;
        if (startSubmerged_) {
            tr->position.y = surfaceY_ - diveDepth_;
        }
    }
    // コライダーの補完（球・半径 1・非トリガー）は EnemyBaseScript::OnCreate が同じ設定で行うので、ここでは行わない
}

REGISTER_SCRIPT(SharkEnemyScript)
