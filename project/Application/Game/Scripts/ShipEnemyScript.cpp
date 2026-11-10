#include "EnemyBaseScript.h"
#include "ECS/ScriptRegistry.h"
#include "ECS/TransformComponent.h"
#include "ECS/CameraComponent.h"
#include "ECS/ColliderComponent.h"
#include "ECS/PrimitiveMeshComponent.h"
#include "ECS/NativeScriptComponent.h"
#include "Common/Math/MathUtils.h"
#include "Common/Log/Log.h"
#include "Scene.h"
#include "RenderCommon.h"

#include <algorithm>
#include <cmath>
#include <random>

#if RC_ENABLE_IMGUI
#include "imgui/imgui.h"
#endif

// =====================================================================
// T-15: 新規敵タイプ「船」
// =====================================================================
// サメ（SharkEnemyScript）が「まっすぐ突っ込んでくる近接」なのに対し、
// 船は「旋回が遅く、横腹を向けて撃つ砲撃艦」にした。
// 敵の種類が増えても戦い方が変わらないと意味がないため、
// 移動と攻撃の両方でサメと別の性質を持たせている。
//
// 船らしさの本体は次の 2 つの制約:
//   ・船首方向にしか進めない（真横へは動けない）
//   ・旋回が遅い（既定 35°/s）
// この制約があるだけで「回り込まれる前に叩く」という判断が生まれる。
//
// 上下動と傾きは持たない
// ----------------------
// Y と pitch / roll は BuoyancyScript（C-01 / C-02）に任せる。
// 同じエンティティに BuoyancyScript を一緒に付けて使う前提で、
// このスクリプトが書くのは XZ 座標と船首方位（rotation.y）だけ。
// 担当が重ならないので、2 つのスクリプトが同じ値を取り合うことがない。
//
// 撃沈されたときも自前で沈めず、`flooding` タグを立てて
// BuoyancyScript に実効密度比を上げてもらう（＝船体が浸水して沈む）。
// 位置を直接下げるより、波に揺られながら沈んでいくぶん自然になる。
//
// 想定する運用
// ------------
// A-03 のウェーブ戦闘で使う敵。ウェーブ中はレールが止まるのでプレイヤーは
// その場に留まる。
// 逆に**レール移動中のプレイヤーには追随できない**（速力 4.5 に対して
// レールは 5.0）。これは意図した挙動で、振り切られた砲艦は脅威でなくなる。
//
// 持ち場は自機の正面の扇形
// ------------------------
// 自機の視点は正面から ±80° までしか振れない（RailShooterController の maxYawDeg）。
// 以前は自機の周りを一周していたため、周回のたびに背後へ回り込み、
// その間は撃ち返せない・船も撃ってこない「何も起きない時間」ができていた。
// いまは自機を中心に preferredDistance の円を回りつつ、正面から ±frontArcDeg を
// 超えたら回る向きを反転する（＝正面の扇の中を往復する）。反転は外側へ大回りで切り返す。
// 何かの拍子に扇の外（+20°）へ出たら、旋回と速力を上げて正面へ戻る（recover）。
// 数値検証（静止した自機・既定値）では、定常時の最大方位 62〜68°（視点限界 80° 以内）、
// 斉射可能な時間は 7〜8 割、背後から出現した場合も 2〜5 秒で視界へ戻る。

/// @brief 船の行動段階
enum class ShipState {
    Patrol,  ///< プレイヤーを見つけるまで、出現地点のまわりを巡航する
    Engage,  ///< 横腹を向ける位置へ回り込む
    Firing,  ///< 斉射中（操船は続ける）
    Reload,  ///< 再装填中
};

/// @brief 旋回の遅い砲撃艦
class ShipEnemyScript : public EnemyBaseScript {
public:
    // --- 探知 ---
    float detectDistance = 60.0f; ///< この距離まで近づかれたら交戦に入る
    bool  requireInView = true;   ///< プレイヤーの視界内にいるときだけ交戦する
    float viewDot = 0.2f;         ///< 視界判定のしきい値（0.2 で前方およそ150度）

    // --- 操船 ---
    float cruiseSpeed = 6.0f;        ///< 巡航速度(m/s)
    float engageSpeed = 4.5f;        ///< 交戦中の速度(m/s)
    float turnRateDeg = 35.0f;       ///< 旋回速度(度/秒)。小さいほど「船らしい」
    float preferredDistance = 28.0f; ///< 保ちたい交戦距離(m)。自機を中心にこの半径を回る（旋回半径の 3 倍が下限）
    float patrolRadius = 18.0f;      ///< 巡航で描く円の半径(m)
    float frontArcDeg = 50.0f;       ///< 自機の正面から左右この角度までを持ち場にする(度)。超えたら折り返す
    float recoverTurnMul = 2.5f;     ///< 持ち場の外へ出たときの旋回速度の倍率
    float recoverSpeedMul = 1.8f;    ///< 持ち場の外へ出たときの速力の倍率（自機が止まっているときだけ）

    // --- 砲撃 ---
    float broadsideToleranceDeg = 35.0f; ///< 「横腹を向けた」とみなす角度の許容(度)
    float fireMinDistance = 12.0f;       ///< これより近いと撃たない
    float fireMaxDistance = 45.0f;       ///< これより遠いと撃たない
    int   volleyCount = 3;               ///< 1 斉射の弾数
    float volleyInterval = 0.22f;        ///< 斉射内の発射間隔(秒)
    float reloadDuration = 3.0f;         ///< 再装填(秒)
    float shellSpeed = 22.0f;            ///< 砲弾の初速(m/s)
    /// @brief 砲弾の種類。0=通常(1ダメージ) / 2=重量弾(3ダメージ)
    /// @warning 1（拡散）は使えない。WaterBullet 側が拡散弾を
    ///          `PlayerBullet` としてしか扱っておらず、`EnemyBullet` で 1 を指定すると
    ///          向きが入らないまま速度ゼロで砲口から落ちる（技術的負債 D-13）。
    ///          誤って設定しても事故らないよう、使う直前に 0 か 2 へ丸めている。
    int   bulletType = 0;
    float gunOffset = 2.2f;              ///< 船体中心から砲までの横方向の距離(m)
    float gunHeight = 1.2f;              ///< 砲の高さ(m)
    float spreadDeg = 4.0f;              ///< 弾の散り(度)
    float leadFactor = 0.6f;             ///< 未来位置を読む強さ(0=読まない / 1=完全に読む)
    float shellShootRadius = 1.2f;       ///< 自機の弾がこの距離まで来たら砲弾を撃ち落とせる(m)

    // --- 見た目 ---
    float modelYawOffsetDeg = 0.0f; ///< モデルの向き補正(度)。箱の仮モデルでは 0
    bool  debugDraw = true;         ///< 交戦円・砲の位置などをギズモ描画する

protected:
    /// @brief 基底の体力に加えて、船のパラメータを JSON へ書き出す
    /// @return 全パラメータを格納した json
    nlohmann::json Serialize() override {
        nlohmann::json j = EnemyBaseScript::Serialize(); // hp / maxHp / deathDuration
        j["detectDistance"] = detectDistance;
        j["requireInView"] = requireInView;
        j["viewDot"] = viewDot;
        j["cruiseSpeed"] = cruiseSpeed;
        j["engageSpeed"] = engageSpeed;
        j["turnRateDeg"] = turnRateDeg;
        j["preferredDistance"] = preferredDistance;
        j["patrolRadius"] = patrolRadius;
        j["frontArcDeg"] = frontArcDeg;
        j["recoverTurnMul"] = recoverTurnMul;
        j["recoverSpeedMul"] = recoverSpeedMul;
        j["broadsideToleranceDeg"] = broadsideToleranceDeg;
        j["fireMinDistance"] = fireMinDistance;
        j["fireMaxDistance"] = fireMaxDistance;
        j["volleyCount"] = volleyCount;
        j["volleyInterval"] = volleyInterval;
        j["reloadDuration"] = reloadDuration;
        j["shellSpeed"] = shellSpeed;
        j["bulletType"] = bulletType;
        j["gunOffset"] = gunOffset;
        j["gunHeight"] = gunHeight;
        j["spreadDeg"] = spreadDeg;
        j["leadFactor"] = leadFactor;
        j["shellShootRadius"] = shellShootRadius;
        j["modelYawOffsetDeg"] = modelYawOffsetDeg;
        j["debugDraw"] = debugDraw;
        return j;
    }

    /// @brief JSON から基底の体力と船のパラメータを読み込む（含まれる項目だけ上書きする）
    /// @param j 読み込み元の json
    void Deserialize(const nlohmann::json& j) override {
        EnemyBaseScript::Deserialize(j);
        if (j.contains("detectDistance")) detectDistance = j["detectDistance"].get<float>();
        if (j.contains("requireInView")) requireInView = j["requireInView"].get<bool>();
        if (j.contains("viewDot")) viewDot = j["viewDot"].get<float>();
        if (j.contains("cruiseSpeed")) cruiseSpeed = j["cruiseSpeed"].get<float>();
        if (j.contains("engageSpeed")) engageSpeed = j["engageSpeed"].get<float>();
        if (j.contains("turnRateDeg")) turnRateDeg = j["turnRateDeg"].get<float>();
        if (j.contains("preferredDistance")) preferredDistance = j["preferredDistance"].get<float>();
        if (j.contains("patrolRadius")) patrolRadius = j["patrolRadius"].get<float>();
        if (j.contains("frontArcDeg")) frontArcDeg = j["frontArcDeg"].get<float>();
        if (j.contains("recoverTurnMul")) recoverTurnMul = j["recoverTurnMul"].get<float>();
        if (j.contains("recoverSpeedMul")) recoverSpeedMul = j["recoverSpeedMul"].get<float>();
        if (j.contains("broadsideToleranceDeg")) broadsideToleranceDeg = j["broadsideToleranceDeg"].get<float>();
        if (j.contains("fireMinDistance")) fireMinDistance = j["fireMinDistance"].get<float>();
        if (j.contains("fireMaxDistance")) fireMaxDistance = j["fireMaxDistance"].get<float>();
        if (j.contains("volleyCount")) volleyCount = j["volleyCount"].get<int>();
        if (j.contains("volleyInterval")) volleyInterval = j["volleyInterval"].get<float>();
        if (j.contains("reloadDuration")) reloadDuration = j["reloadDuration"].get<float>();
        if (j.contains("shellSpeed")) shellSpeed = j["shellSpeed"].get<float>();
        if (j.contains("bulletType")) bulletType = j["bulletType"].get<int>();
        if (j.contains("gunOffset")) gunOffset = j["gunOffset"].get<float>();
        if (j.contains("gunHeight")) gunHeight = j["gunHeight"].get<float>();
        if (j.contains("spreadDeg")) spreadDeg = j["spreadDeg"].get<float>();
        if (j.contains("leadFactor")) leadFactor = j["leadFactor"].get<float>();
        if (j.contains("shellShootRadius")) shellShootRadius = j["shellShootRadius"].get<float>();
        if (j.contains("modelYawOffsetDeg")) modelYawOffsetDeg = j["modelYawOffsetDeg"].get<float>();
        if (j.contains("debugDraw")) debugDraw = j["debugDraw"].get<bool>();
    }

    /// @brief 船としての既定値（HP・撃破演出時間）を設定し、出現位置・船首方位・巡航の回り方を決める
    void OnCreate() override {
        EnemyBaseScript::OnCreate();
        // 船はサメより硬く、沈むまでにも時間がかかる。
        // データ側で指定があればそちらを優先する（D-12）。
        if (!hpFromData) {
            hp = 40;
            maxHp = 40;
        }
        // 浸水して沈みきるのを見せるぶん長めに取る。
        // hp と同じく、データ側で指定があればそちらを優先する。
        if (!deathDurationFromData) deathDuration = 6.0f;

        state_ = ShipState::Patrol;
        if (auto* tr = GetComponent<TransformComponent>()) {
            spawnPos_ = tr->position;
            headingY_ = tr->rotation.y - modelYawOffsetDeg * kDeg2Rad;
        }
        // 巡航でどちら回りにするかは出現時に一度だけ決める。
        // 毎フレーム選び直すと、境目で左右に迷ってその場で首を振る。
        circleSign_ = (RandomUnit() < 0.5f) ? -1.0f : 1.0f;

        if (Entity* self = GetEntity()) {
            self->ClearTag("flooding"); // 前回のプレイで保存されていた場合の消し込み
        }
    }

    /// @brief 目標の速度推定、状態遷移（巡航・交戦・斉射・再装填）、操船を行う
    /// @param deltaTime 前フレームからの経過秒数
    /// @note 書き込むのは XZ 座標と rotation.y のみ。Y と pitch / roll は BuoyancyScript の担当。
    void OnUpdate(float deltaTime) override {
        EnemyBaseScript::OnUpdate(deltaTime); // 被弾処理・HP タグ・消滅タイマー

        auto* tr = GetComponent<TransformComponent>();
        if (!tr) return;

        // 撃沈後は操船も砲撃もしない。沈める処理は BuoyancyScript に任せる
        // （TakeDamage で flooding タグを立ててある）。
        if (isDead) return;
        if (deltaTime <= 0.0f) return;

        Scene* scene = GetScene();
        if (!scene) return;

        Entity* target = FindTarget(scene);
        if (!target) return;
        auto* targetTr = target->GetComponent<TransformComponent>();
        if (!targetTr) return;

        UpdateTargetVelocity(targetTr->position, deltaTime);

        // --- 目標との位置関係 ---
        const float dx = targetTr->position.x - tr->position.x;
        const float dz = targetTr->position.z - tr->position.z;
        const float dist = std::sqrt(dx * dx + dz * dz);
        lastDistance_ = dist;

        // --- 自機の正面から見た自分の方位 ---
        // 視点を振っても持ち場がぶれないよう、カメラの今の向きではなく
        // 「視点の基準の向き」（RailShooterController が base_yaw_mrad タグで出す）を使う。
        UpdateFrontYaw(target, targetTr);
        const float frontX = std::sin(frontYaw_);
        const float frontZ = std::cos(frontYaw_);
        const float rightX = frontZ;
        const float rightZ = -frontX;
        const float localF = (-dx) * frontX + (-dz) * frontZ; // 自機から見て前方への距離
        const float localR = (-dx) * rightX + (-dz) * rightZ; // 自機から見て右への距離
        const float bearing = std::atan2(localR, localF);     // 正面からの方位(rad, 右が正)
        lastBearing_ = bearing;

        // 持ち場（正面の扇）の外へ出たら recover。境目でばたつかないよう出入りに差をつける
        const float arc = std::clamp(frontArcDeg, 5.0f, 170.0f) * kDeg2Rad;
        if (!recovering_ && std::fabs(bearing) > arc + kRecoverEnterMarginDeg * kDeg2Rad) {
            recovering_ = true;
        } else if (recovering_ && std::fabs(bearing) < arc) {
            recovering_ = false;
        }

        // 交戦時に回る円の半径。旋回半径に比べて小さすぎると円に乗れず暴れるので下限を設ける
        const float orbitRadius = EngageRadius();

        switch (state_) {
        case ShipState::Patrol:
            if (dist <= detectDistance && IsSeenBy(targetTr, dx, dz, dist)) {
                // 回り込む向きは「今の船首から見て旋回量が少ないほう」を選ぶ。
                circleSign_ = ChooseCircleSign(tr->position, targetTr->position, orbitRadius);
                state_ = ShipState::Engage;
                Log::Print("[ShipEnemyScript] engaging");
            }
            break;

        case ShipState::Engage:
            if (dist > detectDistance * kDisengageMargin) {
                state_ = ShipState::Patrol; // 振り切られた
            } else if (!recovering_ && CanFire(dx, dz, dist)) {
                // 持ち場の外（自機が撃ち返せない位置）からは撃たない
                shotsLeft_ = (std::max)(volleyCount, 1);
                shotTimer_ = 0.0f;
                state_ = ShipState::Firing;
            }
            break;

        case ShipState::Firing:
            // volleyInterval ごとに 1 発ずつ撃ち、撃ち切ったら再装填へ
            shotTimer_ -= deltaTime;
            if (shotTimer_ <= 0.0f) {
                FireShell(scene, tr, targetTr->position);
                shotTimer_ = (std::max)(volleyInterval, 0.0f);
                if (--shotsLeft_ <= 0) {
                    reloadTimer_ = (std::max)(reloadDuration, 0.0f);
                    state_ = ShipState::Reload;
                }
            }
            break;

        case ShipState::Reload:
            reloadTimer_ -= deltaTime;
            if (reloadTimer_ <= 0.0f) state_ = ShipState::Engage;
            break;
        }

        // --- 操船（どの状態でも動き続ける。止まる船は的でしかない） ---
        const bool engaged = (state_ != ShipState::Patrol);
        const RC::Vector3 center = engaged ? targetTr->position : spawnPos_;
        const float radius = engaged ? orbitRadius : patrolRadius;
        float speed = engaged ? engageSpeed : cruiseSpeed;
        float turnMul = 1.0f;

        RC::Vector3 desired = OrbitDirection(tr->position, center, radius, circleSign_);

        if (engaged) {
            // 正面の扇の端まで来たら回る向きを反転する（＝扇の中を往復する）。
            // 「今の回り方で方位がさらに外へ開いていくか」で判定するので、
            // 扇の外にいる場合も、正面へ戻る向きに自然と揃う。
            flipCooldown_ -= deltaTime;
            const float vF = desired.x * frontX + desired.z * frontZ;
            const float vR = desired.x * rightX + desired.z * rightZ;
            const float bearingRate = localF * vR - localR * vF; // 方位の増える向き（符号だけ使う）
            const bool openingOut = (bearing > arc && bearingRate > 0.0f) ||
                                    (bearing < -arc && bearingRate < 0.0f);
            if (flipCooldown_ <= 0.0f && openingOut) {
                circleSign_ = -circleSign_;
                desired = OrbitDirection(tr->position, center, radius, circleSign_);
                flipCooldown_ = kFlipCooldown;
                // 円の上にいるなら外側へ大回りで切り返す（内側へ切ると自機へ突っ込んで射程を割る）。
                // 円から大きく外れているときは、素直に近いほうへ回す。
                if (std::fabs(dist - radius) < radius * kForcedTurnBand) {
                    const float shipRX = std::cos(headingY_); // 船の右舷方向
                    const float shipRZ = -std::sin(headingY_);
                    const float side = shipRX * dx + shipRZ * dz; // 正なら自機は右舷側
                    forcedTurn_ = (side > 0.0f) ? -1 : 1;           // 自機と反対側へ舵を切る
                } else {
                    forcedTurn_ = 0;
                }
            }

            // 持ち場の外では急いで戻る。レール移動中（自機が動いている）は追わない：
            // 振り切られた砲艦が脅威でなくなるのは意図どおり。
            if (recovering_) {
                turnMul = (std::max)(recoverTurnMul, 1.0f);
                const float targetSpeed = std::sqrt(targetVel_.x * targetVel_.x + targetVel_.z * targetVel_.z);
                if (targetSpeed < kTargetStillSpeed) speed *= (std::max)(recoverSpeedMul, 1.0f);
            }
        } else {
            forcedTurn_ = 0;
        }

        SteerTowards(desired, deltaTime, turnMul);

        // 船首方向にだけ進む（真横へは動けない）
        const float fx = std::sin(headingY_);
        const float fz = std::cos(headingY_);
        tr->position.x += fx * speed * deltaTime;
        tr->position.z += fz * speed * deltaTime;
        // Y と pitch / roll には触らない（BuoyancyScript の担当）
        tr->rotation.y = headingY_ + modelYawOffsetDeg * kDeg2Rad;
    }

public:
    /// @brief 基底の被ダメージ処理に加え、撃沈されたら船体を浸水させる
    /// @param damage 受けるダメージ量
    void TakeDamage(int damage) override {
        const bool wasDead = isDead;
        EnemyBaseScript::TakeDamage(damage);
        if (!wasDead && isDead) {
            if (Entity* self = GetEntity()) {
                // 位置を直接下げるのではなく、BuoyancyScript に実効密度比を
                // 上げてもらう。波に揺られながら沈むので自然に見えるうえ、
                // 浮力側と位置を取り合うこともない。
                self->SetTag("flooding", 1);
            }
            Log::Print("[ShipEnemyScript] sinking");
        }
    }

    /// @brief 船首方向・左右の砲の位置・交戦距離をギズモ描画する（debugDraw が偽なら何もしない）
    void OnDebugRender() override {
        if (!debugDraw) return;
        auto* tr = GetComponent<TransformComponent>();
        if (!tr) return;

        const RC::Vector4 col = { 1.0f, 0.75f, 0.2f, 1.0f };
        const float fx = std::sin(headingY_);
        const float fz = std::cos(headingY_);

        // 船首方向（進める向きはこれ 1 本だけ、というのが船の制約）
        RC::DrawLine3D(tr->position,
                       { tr->position.x + fx * 6.0f, tr->position.y, tr->position.z + fz * 6.0f },
                       col, true);
        // 左右の砲の位置
        const float rx = fz, rz = -fx;
        for (float s : { 1.0f, -1.0f }) {
            RC::DrawWireSphere3D({ tr->position.x + rx * gunOffset * s,
                                   tr->position.y + gunHeight,
                                   tr->position.z + rz * gunOffset * s },
                                 0.35f, col, 8, 8, true);
        }
        // 保とうとしている交戦距離
        RC::DrawWireSphere3D(tr->position, preferredDistance,
                             { col.x, col.y, col.z, 0.35f }, 24, 3, true);

        // 持ち場（自機の正面の扇）の両端。recover 中は赤
        if (hasLastTarget_ && hasFrontYaw_) {
            const RC::Vector4 arcCol = recovering_ ? RC::Vector4{ 1.0f, 0.2f, 0.2f, 0.8f }
                                                   : RC::Vector4{ 0.3f, 0.9f, 1.0f, 0.6f };
            const float len = EngageRadius() * 1.5f;
            const float arc = std::clamp(frontArcDeg, 5.0f, 170.0f) * kDeg2Rad;
            for (float s : { 1.0f, -1.0f }) {
                const float a = frontYaw_ + arc * s;
                RC::DrawLine3D({ lastTargetPos_.x, tr->position.y, lastTargetPos_.z },
                               { lastTargetPos_.x + std::sin(a) * len, tr->position.y,
                                 lastTargetPos_.z + std::cos(a) * len },
                               arcCol, true);
            }
        }
    }

#if RC_ENABLE_IMGUI
    /// @brief インスペクタに状態表示・各パラメータの編集欄・斉射サイクルの目安を表示する
    void OnImGui() override {
        EnemyBaseScript::OnImGui();
        ImGui::Text("State: %s  dist %.1fm", StateName(state_), lastDistance_);
        ImGui::Text("Heading: %.0f deg   Circle: %s",
                    headingY_ / kDeg2Rad, circleSign_ > 0.0f ? "CW" : "CCW");
        if (state_ == ShipState::Firing) ImGui::Text("Shots left: %d", shotsLeft_);
        if (state_ == ShipState::Reload) ImGui::Text("Reload: %.2fs", reloadTimer_);
        ImGui::Text("Target vel: (%.1f, %.1f, %.1f)", targetVel_.x, targetVel_.y, targetVel_.z);
        ImGui::Text("Bearing: %.0f deg  %s  (orbit r %.1fm)", lastBearing_ / kDeg2Rad,
                    recovering_ ? "RECOVER" : "in arc", EngageRadius());

        ImGui::Separator();
        ImGui::TextUnformatted("探知");
        ImGui::DragFloat("Detect Distance", &detectDistance, 0.5f, 1.0f, 300.0f);
        ImGui::Checkbox("Require In View", &requireInView);

        ImGui::TextUnformatted("操船");
        ImGui::DragFloat("Cruise Speed", &cruiseSpeed, 0.1f, 0.0f, 40.0f);
        ImGui::DragFloat("Engage Speed", &engageSpeed, 0.1f, 0.0f, 40.0f);
        ImGui::DragFloat("Turn Rate (deg/s)", &turnRateDeg, 1.0f, 1.0f, 360.0f);
        ImGui::DragFloat("Preferred Distance", &preferredDistance, 0.5f, 1.0f, 200.0f);
        ImGui::DragFloat("Patrol Radius", &patrolRadius, 0.5f, 1.0f, 200.0f);
        ImGui::DragFloat("Front Arc (deg)", &frontArcDeg, 1.0f, 5.0f, 170.0f);
        ImGui::DragFloat("Recover Turn Mul", &recoverTurnMul, 0.05f, 1.0f, 6.0f);
        ImGui::DragFloat("Recover Speed Mul", &recoverSpeedMul, 0.05f, 1.0f, 4.0f);

        ImGui::TextUnformatted("砲撃");
        ImGui::DragFloat("Broadside Tolerance (deg)", &broadsideToleranceDeg, 1.0f, 1.0f, 89.0f);
        ImGui::DragFloat("Fire Min Distance", &fireMinDistance, 0.5f, 0.0f, 200.0f);
        ImGui::DragFloat("Fire Max Distance", &fireMaxDistance, 0.5f, 1.0f, 300.0f);
        ImGui::DragInt("Volley Count", &volleyCount, 1, 1, 12);
        ImGui::DragFloat("Volley Interval", &volleyInterval, 0.01f, 0.0f, 3.0f);
        ImGui::DragFloat("Reload Duration", &reloadDuration, 0.1f, 0.0f, 30.0f);
        ImGui::DragFloat("Shell Speed", &shellSpeed, 0.5f, 1.0f, 200.0f);
        // 拡散(1)は EnemyBullet では機能しないので選ばせない（D-13）
        {
            static const char* kShellLabels[] = { "normal (1 dmg)", "heavy (3 dmg)" };
            int sel = (SanitizeBulletType(bulletType) == 2) ? 1 : 0;
            if (ImGui::Combo("Shell Type", &sel, kShellLabels, IM_ARRAYSIZE(kShellLabels))) {
                bulletType = (sel == 1) ? 2 : 0;
            }
            ImGui::SameLine();
            ImGui::Text("(g=%.1f)", ShellGravityFor(bulletType));
        }
        ImGui::DragFloat("Gun Offset", &gunOffset, 0.1f, 0.0f, 20.0f);
        ImGui::DragFloat("Gun Height", &gunHeight, 0.1f, -5.0f, 20.0f);
        ImGui::DragFloat("Spread (deg)", &spreadDeg, 0.1f, 0.0f, 45.0f);
        ImGui::SliderFloat("Lead Factor", &leadFactor, 0.0f, 1.5f);
        ImGui::DragFloat("Shell Shoot Radius", &shellShootRadius, 0.05f, 0.0f, 5.0f);
        ImGui::Checkbox("Debug Draw", &debugDraw);

        // 実際にどれくらいの手数になるかの目安。
        // プレイヤーには 1 秒の無敵時間があるので、斉射の 2 発目以降は
        // ほぼ当たっても入らない。volleyCount は圧のかけ方であって
        // ダメージ量ではない、という点に注意。
        const float cycle = static_cast<float>((std::max)(volleyCount, 1)) * volleyInterval
                          + (std::max)(reloadDuration, 0.0f);
        if (cycle > 0.0f) {
            ImGui::Separator();
            ImGui::Text("斉射サイクル %.2fs（プレイヤー無敵 1.0s のため実効は 1 サイクル 1 ダメージ）", cycle);
        }
    }
#endif

private:
    static constexpr float kPi = 3.14159265358979f;
    static constexpr float kDeg2Rad = kPi / 180.0f;
    /// @brief 交戦をやめる距離の倍率（探知距離ちょうどで切ると境目でばたつく）
    static constexpr float kDisengageMargin = 1.35f;
    /// @brief 持ち場の扇からこの角度(度)以上はみ出したら recover に入る（戻りは扇の内側に入ったとき）
    static constexpr float kRecoverEnterMarginDeg = 20.0f;
    /// @brief 回る向きを反転したあと、次の反転を受け付けるまでの秒数（切り返し中のばたつき防止）
    static constexpr float kFlipCooldown = 3.0f;
    /// @brief 「円の上にいる」とみなす幅（半径に対する比）。この中なら切り返しを外回りに固定する
    static constexpr float kForcedTurnBand = 0.35f;
    /// @brief 外回りの固定を解く角度差(度)。目標の向きとの差がこれ未満になったら通常の操舵へ戻す
    static constexpr float kForcedTurnReleaseDeg = 60.0f;
    /// @brief 交戦半径の下限＝旋回半径のこの倍数。小さい円では切り返しが間に合わず円から外れる
    static constexpr float kMinOrbitTurnRadii = 3.0f;
    /// @brief 自機が止まっているとみなす速さ(m/s)。これ未満のときだけ recover で増速する
    static constexpr float kTargetStillSpeed = 1.0f;

    /// @brief 状態の表示名を返す（ImGui 表示用）
    /// @param s 状態
    /// @return 状態名の文字列
    static const char* StateName(ShipState s) {
        switch (s) {
        case ShipState::Patrol: return "Patrol";
        case ShipState::Engage: return "Engage";
        case ShipState::Firing: return "Firing";
        case ShipState::Reload: return "Reload";
        }
        return "?";
    }

    /// @brief 0..1 の一様乱数を返す
    /// @return 0 以上 1 未満の乱数
    static float RandomUnit() {
        static std::mt19937 engine{ std::random_device{}() };
        static std::uniform_real_distribution<float> dist(0.0f, 1.0f);
        return dist(engine);
    }

    /// @brief 角度を -π..π に丸める
    /// @param a 角度 (rad)
    /// @return -π..π に丸めた角度 (rad)
    static float WrapPi(float a) {
        const float twoPi = kPi * 2.0f;
        while (a > kPi)  a -= twoPi;
        while (a < -kPi) a += twoPi;
        return a;
    }

    /// @brief プレイヤー（カメラ）を探す。見つけたものはキャッシュする
    /// @param scene 検索するシーン
    /// @return カメラを持つエンティティ。無ければ nullptr
    Entity* FindTarget(Scene* scene) {
        if (auto cached = cachedTarget_.lock()) {
            if (!cached->IsPendingDestroy() && cached->IsActive()) return cached.get();
        }
        for (auto& e : scene->GetEntities()) {
            if (e && e->HasComponent<CameraComponent>()) {
                cachedTarget_ = e;
                return e.get();
            }
        }
        return nullptr;
    }

    /// @brief 目標の速度を差分から推定する（偏差射撃に使う）
    /// @details レール上のプレイヤーはほぼ等速なので線形予測でよく当たる。
    ///          生の差分はフレーム単位で暴れるので指数平滑を掛けている。
    /// @param pos 目標の現在位置
    /// @param deltaTime 前フレームからの経過秒数
    void UpdateTargetVelocity(const RC::Vector3& pos, float deltaTime) {
        if (hasLastTarget_ && deltaTime > 1e-5f) {
            const RC::Vector3 raw = { (pos.x - lastTargetPos_.x) / deltaTime,
                                      (pos.y - lastTargetPos_.y) / deltaTime,
                                      (pos.z - lastTargetPos_.z) / deltaTime };
            const float t = RC::ExpSmoothingFactor(6.0f, deltaTime);
            targetVel_ = RC::Lerp(targetVel_, raw, t);
        }
        lastTargetPos_ = pos;
        hasLastTarget_ = true;
    }

    /// @brief プレイヤーの視界に入っているか（見えない位置から撃たれないようにする）
    /// @param targetTr 目標（プレイヤー）の Transform
    /// @param dx 船から目標への X 方向の差
    /// @param dz 船から目標への Z 方向の差
    /// @param dist 船と目標の XZ 平面上の距離
    /// @return 視界内なら true（requireInView が偽なら常に true）
    bool IsSeenBy(const TransformComponent* targetTr, float dx, float dz, float dist) const {
        if (!requireInView) return true;
        if (dist < 0.01f) return true;
        const float cy = std::cos(targetTr->rotation.y);
        const float sy = std::sin(targetTr->rotation.y);
        // 目標から見た自分の方向（-dx, -dz）と目標の前方との内積
        const float dot = sy * (-dx / dist) + cy * (-dz / dist);
        return dot > viewDot;
    }

    /// @brief 「自機の正面」の向きを更新する
    /// @details RailShooterController が出す視点の基準の向き（base_yaw_mrad タグ）を優先する。
    ///          タグが無いとき（別の操作スクリプト等）は、最初に見たカメラの向きで固定する。
    ///          毎フレームのカメラの向きを使うと、視点を振るたびに持ち場が動いてしまう。
    /// @param target 自機のエンティティ
    /// @param targetTr 自機の Transform
    void UpdateFrontYaw(const Entity* target, const TransformComponent* targetTr) {
        if (target && target->HasTag("base_yaw_mrad")) {
            frontYaw_ = static_cast<float>(target->GetTagInt("base_yaw_mrad", 0)) / 1000.0f;
            hasFrontYaw_ = true;
        } else if (!hasFrontYaw_) {
            frontYaw_ = targetTr->rotation.y;
            hasFrontYaw_ = true;
        }
    }

    /// @brief 交戦時に自機の周りを回る半径（preferredDistance。ただし旋回半径の kMinOrbitTurnRadii 倍が下限）
    /// @return 半径(m)
    float EngageRadius() const {
        const float omega = (std::max)(turnRateDeg, 1.0f) * kDeg2Rad;
        const float turnRadius = (std::max)(engageSpeed, 0.0f) / omega;
        return (std::max)(preferredDistance, turnRadius * kMinOrbitTurnRadii);
    }

    /// @brief 回り込む向きを「旋回量が少ないほう」で決める
    /// @param from 船の現在位置
    /// @param center 回り込む中心
    /// @param radius 回る半径
    /// @return 回り込む向き（+1 / -1）
    float ChooseCircleSign(const RC::Vector3& from, const RC::Vector3& center, float radius) const {
        float best = 1.0f;
        float bestTurn = 1e9f;
        for (float sign : { 1.0f, -1.0f }) {
            const RC::Vector3 d = OrbitDirection(from, center, radius, sign);
            const float need = std::fabs(WrapPi(std::atan2(d.x, d.z) - headingY_));
            if (need < bestTurn) { bestTurn = need; best = sign; }
        }
        return best;
    }

    /// @brief 目標点のまわりを半径 radius で回るために進みたい方向（XZ・単位長）
    /// @details 半径方向と接線方向のブレンド。
    ///          遠ければ内向き、近ければ外向き、ちょうどなら接線になる。
    ///          接線成分に sqrt(1 - e^2) を掛けてあるので合成は常に単位長。
    ///          「接線を向く」＝「目標に横腹を向ける」なので、
    ///          距離を保つ操船と砲門を向ける操船が 1 本の式で両立する。
    /// @param from 船の現在位置
    /// @param center 回る中心
    /// @param radius 回る半径
    /// @param sign 回る向き（+1 / -1）
    /// @return 進みたい方向（XZ 平面上の単位ベクトル）
    RC::Vector3 OrbitDirection(const RC::Vector3& from, const RC::Vector3& center,
                               float radius, float sign) const {
        float tx = center.x - from.x;
        float tz = center.z - from.z;
        const float dist = std::sqrt(tx * tx + tz * tz);
        if (dist < 1e-3f) return { 0.0f, 0.0f, 1.0f };
        tx /= dist;
        tz /= dist;

        // e: 半径からのずれの比（-1..1）。正なら外側にいるので内向き成分、負なら外向き成分になる
        const float r = (std::max)(radius, 1e-3f);
        const float e = std::clamp((dist - r) / r, -1.0f, 1.0f);
        const float tanW = std::sqrt((std::max)(0.0f, 1.0f - e * e));

        const float dx = tx * e + (-tz * sign) * tanW;
        const float dz = tz * e + (tx * sign) * tanW;
        const float len = std::sqrt(dx * dx + dz * dz);
        if (len < 1e-4f) return { tx, 0.0f, tz };
        return { dx / len, 0.0f, dz / len };
    }

    /// @brief 船首を desired へ向ける（旋回速度の上限つき）
    /// @details forcedTurn_ が立っている間は、目標との角度差が大きくても決めた側へ回す
    ///          （切り返しで逆を向くとき、近道が自機側になると内回りして突っ込むため）。
    /// @param desired 向けたい方向（XZ）
    /// @param deltaTime 前フレームからの経過秒数
    /// @param turnMul 旋回速度の倍率（recover 中に上げる）
    void SteerTowards(const RC::Vector3& desired, float deltaTime, float turnMul) {
        const float target = std::atan2(desired.x, desired.z);
        const float maxStep = (std::max)(turnRateDeg, 0.0f) * turnMul * kDeg2Rad * deltaTime;
        float d = WrapPi(target - headingY_);
        if (forcedTurn_ != 0) {
            if (std::fabs(d) < kForcedTurnReleaseDeg * kDeg2Rad) {
                forcedTurn_ = 0;
            } else if (forcedTurn_ > 0 && d < 0.0f) {
                d += kPi * 2.0f;
            } else if (forcedTurn_ < 0 && d > 0.0f) {
                d -= kPi * 2.0f;
            }
        }
        headingY_ = (std::fabs(d) <= maxStep)
                        ? WrapPi(target)
                        : WrapPi(headingY_ + (d > 0.0f ? maxStep : -maxStep));
    }

    /// @brief 撃てる状態か（距離が射程内で、かつ横腹を向けている）
    /// @param dx 船から目標への X 方向の差
    /// @param dz 船から目標への Z 方向の差
    /// @param dist 船と目標の XZ 平面上の距離
    /// @return 撃てるなら true
    bool CanFire(float dx, float dz, float dist) const {
        if (dist < fireMinDistance || dist > fireMaxDistance) return false;
        if (dist < 1e-3f) return false;

        const float fx = std::sin(headingY_);
        const float fz = std::cos(headingY_);
        // 船首と目標方向の内積。0 に近いほど真横。
        const float align = fx * (dx / dist) + fz * (dz / dist);
        // 許容 35 度なら、船首から見て 55〜125 度の範囲を「横腹」とみなす
        const float limit = std::cos((90.0f - std::clamp(broadsideToleranceDeg, 0.0f, 89.0f)) * kDeg2Rad);
        return std::fabs(align) <= limit;
    }

    /// @brief 砲弾を 1 発撃つ
    /// @param scene 砲弾を生成するシーン
    /// @param tr 船の Transform
    /// @param targetPos 目標（プレイヤー）の位置
    /// @note 目標側の舷の砲から、偏差と落下を補正した狙点へ向けて撃つ。
    void FireShell(Scene* scene, const TransformComponent* tr, const RC::Vector3& targetPos) {
        const float fx = std::sin(headingY_);
        const float fz = std::cos(headingY_);
        const float rx = fz;   // 右舷方向
        const float rz = -fx;

        // 目標がどちら舷にいるかで、使う砲を決める
        const float side =
            (rx * (targetPos.x - tr->position.x) + rz * (targetPos.z - tr->position.z)) >= 0.0f
                ? 1.0f : -1.0f;

        const RC::Vector3 muzzle = { tr->position.x + rx * gunOffset * side,
                                     tr->position.y + gunHeight,
                                     tr->position.z + rz * gunOffset * side };

        // 砲口から狙点への単位方向を求める
        const RC::Vector3 aim = SolveAimPoint(muzzle, targetPos);

        float dirX = aim.x - muzzle.x;
        float dirY = aim.y - muzzle.y;
        float dirZ = aim.z - muzzle.z;
        const float len = std::sqrt(dirX * dirX + dirY * dirY + dirZ * dirZ);
        if (len < 1e-3f) return;
        dirX /= len; dirY /= len; dirZ /= len;

        // 散り。Y 軸まわりに少し回すだけで十分ばらける
        if (spreadDeg > 0.0f) {
            const float a = (RandomUnit() * 2.0f - 1.0f) * spreadDeg * kDeg2Rad;
            const float c = std::cos(a), s = std::sin(a);
            const float nx = dirX * c + dirZ * s;
            const float nz = -dirX * s + dirZ * c;
            dirX = nx; dirZ = nz;
        }

        SpawnShell(scene, muzzle, { dirX, dirY, dirZ });
    }

    /// @brief 砲弾の狙点を解く（未来位置の予測＋落下ぶんの持ち上げ）
    /// @details WaterBullet は毎フレーム velocity.y に重力を足すため、
    ///          目標を直接狙うと必ず下へ外れる。28m 先で 2.4m、40m 先では 4.9m 落ちる。
    ///          落下量は飛翔時間から決まり、飛翔時間は狙点までの距離から決まるので、
    ///          2 回ほど反復して両者を折り合わせる。
    ///          数値検証では、静止目標で誤差 0.05m、横切る目標でも 0.32m まで詰まる
    ///          （補正なしだとそれぞれ 2.41m / 5.61m）。
    /// @param muzzle 砲口の位置
    /// @param targetPos 目標の現在位置
    /// @return 狙うべき点
    RC::Vector3 SolveAimPoint(const RC::Vector3& muzzle, const RC::Vector3& targetPos) const {
        const float speed = (std::max)(shellSpeed, 1.0f);
        const float shellGravity = ShellGravityFor(bulletType);
        RC::Vector3 aim = targetPos;

        for (int i = 0; i < kAimIterations; ++i) {
            const float dx = aim.x - muzzle.x;
            const float dy = aim.y - muzzle.y;
            const float dz = aim.z - muzzle.z;
            const float flight = std::sqrt(dx * dx + dy * dy + dz * dz) / speed;

            aim.x = targetPos.x + targetVel_.x * flight * leadFactor;
            aim.y = targetPos.y + targetVel_.y * flight * leadFactor
                  + 0.5f * shellGravity * flight * flight;
            aim.z = targetPos.z + targetVel_.z * flight * leadFactor;
        }
        return aim;
    }

    /// @brief 砲弾エンティティを出す（既存の EnemyBullet プールを流用）
    /// @param scene 砲弾を生成するシーン
    /// @param pos 発射位置（砲口）
    /// @param dir 発射方向（単位ベクトル）
    void SpawnShell(Scene* scene, const RC::Vector3& pos, const RC::Vector3& dir) {
        // 非アクティブな EnemyBullet があれば再利用し、無ければ新規に作る
        std::shared_ptr<Entity> shell = nullptr;
        for (auto& e : scene->GetEntities()) {
            if (e && e->GetName() == "EnemyBullet" && !e->IsActive() && !e->IsPendingDestroy()) {
                shell = e;
                shell->SetActive(true);
                shell->SetTag("reused", 1);
                break;
            }
        }
        const bool isNew = (shell == nullptr);
        if (isNew) shell = scene->CreateEntity("EnemyBullet");
        if (!shell) return;

        auto* btr = shell->GetComponent<TransformComponent>();
        if (!btr) btr = &shell->AddComponent<TransformComponent>();
        btr->position = pos;
        btr->scale = { 0.3f, 0.3f, 0.3f };

        auto* pm = shell->GetComponent<PrimitiveMeshComponent>();
        if (!pm) {
            pm = &shell->AddComponent<PrimitiveMeshComponent>();
            pm->type = PrimitiveType::Sphere;
        }
        // 水の質感で描く（以前は DataDrivenScene がエンティティ名で判定していた）
        pm->drawStyle = PrimitiveDrawStyle::Water;
        // EnemyBullet のプールは EnemyAI と共有していて、あちらは弾を赤く塗る。
        // 使い回した弾がそのままだと砲弾が赤く出るので、毎回塗り直す。
        pm->color = kShellColor;
        if (pm->meshHandle >= 0) {
            if (auto* mat = RC::GetPrimitiveMeshMaterialPtr(pm->meshHandle)) {
                mat->color = kShellColor;
            }
        }

        // 当たり判定（トリガー）と弾の挙動スクリプトを付ける。再利用時は既存のものを使う
        auto* col = shell->GetComponent<ColliderComponent>();
        if (!col) col = &shell->AddComponent<ColliderComponent>();
        col->shape = ColliderComponent::Shape::Sphere;
        col->radius = 1.0f;
        col->isTrigger = true;

        auto* nsc = shell->GetComponent<NativeScriptComponent>();
        if (!nsc) {
            nsc = &shell->AddComponent<NativeScriptComponent>();
            nsc->AddScript("WaterBullet");
            nsc->SetScene(scene);
            if (GetSceneContext()) nsc->SetSceneContext(GetSceneContext());
        }

        // WaterBullet は向きと速さをタグで受け取る（既存の作法に合わせる）。
        // 方向は 1000 倍、速さは 10 倍した整数で渡す。
        shell->SetTag("dir_x", static_cast<int>(dir.x * 1000.0f));
        shell->SetTag("dir_y", static_cast<int>(dir.y * 1000.0f));
        shell->SetTag("dir_z", static_cast<int>(dir.z * 1000.0f));
        shell->SetTag("bullet_speed", static_cast<int>(shellSpeed * 10.0f));
        shell->SetTag("bullet_type", SanitizeBulletType(bulletType));
        // 砲弾は自機の弾で撃ち落とせる（判定は WaterBullet 側）。半径は 100 倍の整数
        shell->SetTag("shootable", 1);
        shell->SetTag("shoot_radius", static_cast<int>(shellShootRadius * 100.0f));

        if (isNew) {
            scene->InitDynamicEntityRuntime(*shell);
            // 生成直後に PrimitiveMesh の Transform を合わせて原点でのチラつきを防ぐ。
            // ハンドルは Init が入れたものを読み直す。
            if (auto* newPm = shell->GetComponent<PrimitiveMeshComponent>()) {
                if (newPm->meshHandle >= 0) {
                    if (auto* pmTr = RC::GetPrimitiveMeshTransformPtr(newPm->meshHandle)) {
                        pmTr->scale = btr->scale;
                        pmTr->rotation = btr->rotation;
                        pmTr->translation = btr->position;
                    }
                }
            }
        }
    }

    /// @brief 弾種を実際に撃てるものへ丸める（1＝拡散は EnemyBullet では機能しない）
    /// @param t 指定された弾種
    /// @return 2 なら 2、それ以外は 0
    static int SanitizeBulletType(int t) { return (t == 2) ? 2 : 0; }

    /// @brief 弾種ごとに掛かっている重力の大きさ(m/s^2)
    /// @warning WaterBullet.cpp の `gravity` の初期値と分岐に揃えること
    ///          （通常 -3.0 / 重量弾 -6.0）。片方だけ変えると弾道補正がずれて当たらなくなる。
    /// @param type 弾種
    /// @return 重力の大きさ（正の値, m/s^2）
    static float ShellGravityFor(int type) {
        return (SanitizeBulletType(type) == 2) ? 6.0f : 3.0f;
    }

    /// @brief 狙点の反復回数。2 回で誤差はコライダー半径より十分小さくなる
    static constexpr int kAimIterations = 2;
    /// @brief 砲弾の色（鉄）
    static constexpr RC::Vector4 kShellColor = { 0.25f, 0.25f, 0.28f, 1.0f };

    ShipState state_ = ShipState::Patrol;
    float headingY_ = 0.0f;       ///< 船首方位(rad)。モデル補正を含まない素の向き
    float circleSign_ = 1.0f;     ///< 回り込む向き(+1 / -1)
    RC::Vector3 spawnPos_{ 0.0f, 0.0f, 0.0f }; ///< 出現地点（巡航の中心）
    float lastDistance_ = 0.0f;   ///< 直近の目標との距離（ImGui 表示用）
    float lastBearing_ = 0.0f;    ///< 直近の自機正面から見た方位(rad)（ImGui 表示用）

    float frontYaw_ = 0.0f;       ///< 自機の正面の向き(rad)。持ち場の扇の基準
    bool  hasFrontYaw_ = false;   ///< frontYaw_ を一度でも決めたか
    bool  recovering_ = false;    ///< 持ち場の外へ出て、急いで戻っている最中か
    float flipCooldown_ = 0.0f;   ///< 次に回る向きを反転できるまでの残り秒数
    int   forcedTurn_ = 0;        ///< 切り返し中に固定する舵の向き（+1=右 / -1=左 / 0=固定しない）

    int   shotsLeft_ = 0;
    float shotTimer_ = 0.0f;
    float reloadTimer_ = 0.0f;

    std::weak_ptr<Entity> cachedTarget_;
    RC::Vector3 lastTargetPos_{ 0.0f, 0.0f, 0.0f };
    RC::Vector3 targetVel_{ 0.0f, 0.0f, 0.0f };
    bool hasLastTarget_ = false;
};

REGISTER_SCRIPT(ShipEnemyScript)
