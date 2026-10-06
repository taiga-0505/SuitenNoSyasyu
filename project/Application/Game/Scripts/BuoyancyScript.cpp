#include "ECS/ScriptableEntity.h"
#include "ECS/ScriptRegistry.h"
#include "ECS/TransformComponent.h"
#include "ECS/RigidbodyComponent.h"
#include "ECS/WaterComponent.h"
#include "Common/Math/MathUtils.h"
#include "Common/Water/WaterSurface.h"
#include "Common/Log/Log.h"
#include "Scene.h"
#include "RenderCommon.h"
#include <cmath>
#include <algorithm>

#if RC_ENABLE_IMGUI
#include "imgui/imgui.h"
#endif

// =====================================================================
// C-01 / C-02: 浮力シミュレーション
// =====================================================================
// C-01（交差判定）: 物体の底面が水面より下にあるか、どれだけ沈んでいるかを
//                   RC::WaterSurface（Water.VS.hlsl と同じ Gerstner 波の式）で判定する。
// C-02（浮力挙動）: 沈み込み率に比例した上向きの加速度を与え、
//                   重力とつり合う深さで浮くようにする。
//
// 設計の要点
// ----------
// ・「密度比 densityRatio」を入力にした。物体密度 ÷ 水の密度。
//   アルキメデスの原理より、浮力加速度 = (g / densityRatio) × 沈み込み率。
//   つり合うのは 沈み込み率 == densityRatio のときなので、
//   densityRatio = 0.4 と書けば「4割沈んで浮く」と読める。
//   加速度をそのまま数値で持つより、見た目から逆算しやすい。
// ・4点サンプル（前後左右）にすると水面の傾きが取れるので、
//   船（T-15）が波で傾く表現に使える。1点サンプルなら上下だけ。
// ・水の抵抗は「線形（速度に比例）」と「2乗（速度の2乗に比例）」の2本立て。
//   最初は線形だけにしていたが、それだと**高いところから落としたときに深く潜り、
//   戻ってくるのがひどく遅くなる**（12m 落下で 3.9m 余分に沈み、水面へ戻るまで 6.2 秒。
//   落下高さを上げるほど悪化する）。実際の水の抵抗は速度の2乗に比例するので、
//   速く突っ込むほど急激に減速して**あまり深く潜らない**のが正しい。
//   2乗抵抗を入れると 12m 落下でも余分な沈み込みは 0.4m、水面復帰は 1.4 秒になり、
//   さらに落下高さをいくら上げても潜る深さがほとんど増えなくなる。
//   役割分担：quadraticDrag = 着水時にどれだけ潜るか
//             linearDrag    = そのあと何回ぷかぷか跳ねてから収まるか
// ・浮き沈みの「周期」は物体のサイズと密度で決まる（T = 2π√(densityRatio × 全高 / g)）。
//   跳ね返りが遅いと感じたら密度を下げるか物体を小さくする。ImGui に周期と
//   減衰比を表示しているので、それを見ながら合わせるのが早い。
// ・障害物（岩）は RC::GetWaterObstacles() でシェーダに渡っているものを
//   そのまま貰う。障害物は反射波を作るだけでなく「内側の波を平らに潰す」
//   マスクも持っているため、ここを省くと岩の近くで
//   「描画は平らなのに当たり判定だけ波打つ」ことになる。
//   （リストの中身は水面に載せた WaterObstacleScript がシーンから集めている＝D-01 解消済み。
//     取得口は変わっていないので、こちら側の書き方は当時のまま。）
class BuoyancyScript : public ScriptableEntity {
public:
    // --- C-01: 交差判定 ---
    float halfHeight = 1.0f;      ///< 物体の中心から底面までの距離(m)。喫水の基準
    float sampleRadius = 1.0f;    ///< 前後サンプルの距離(m)。船なら船体の長さの半分
    /// @brief 左右サンプルの距離(m)。0 なら sampleRadius と同じ
    /// @details 船のように前後に長い物体では、前後と左右で長さが違う。
    ///          同じ距離で取ると細い船体でも横揺れが縦揺れと同じ大きさで出てしまう。
    float sampleRadiusSide = 0.0f;
    bool  useFourSamples = true;  ///< 4点サンプル（水面の傾きを取る）

    // --- C-02: 浮力 ---
    float gravity = 9.81f;        ///< 重力加速度(m/s^2)
    float densityRatio = 0.45f;   ///< 物体密度 ÷ 水の密度。1.0 以上で沈む
    float linearDrag = 5.5f;      ///< 水中での速度減衰(1/s)。粘性ぶん。跳ねる回数を決める
    float quadraticDrag = 1.0f;   ///< 速度の2乗に比例する抵抗(1/m)。着水の潜り込み量を決める
    float maxRiseSpeed = 8.0f;    ///< 浮き上がり速度の上限(m/s)。跳ね上がり防止

    // --- 浸水（T-15: 撃沈された船を沈める） ---
    /// @brief 浸水しきったときの実効密度比。1.0 を超えると沈む
    float floodedDensityRatio = 1.35f;
    /// @brief 浸水が進みきる（また引ききる）までの時間(秒)
    float floodDuration = 2.5f;

    // --- 傾き ---
    bool  alignToSurface = true;  ///< 水面の傾きに合わせて姿勢を傾ける
    float tiltSharpness = 5.0f;   ///< 傾き追従の速さ。大きいほど機敏
    float maxTiltDeg = 18.0f;     ///< 傾きの上限(度)
    float pitchSign = -1.0f;      ///< pitch の符号。逆に傾いて見えたら反転する
    float rollSign = 1.0f;        ///< roll の符号。同上

    bool  debugDraw = true;       ///< サンプル点と水面線をギズモ描画する

    /// @brief RigidbodyComponent が内部で使っている重力加速度（固定値）
    static constexpr float kRigidbodyGravity = 9.81f;

private:
    RC::Vector3 velocity_{0.0f, 0.0f, 0.0f}; ///< Rigidbody が無い場合の自前速度
    bool  wasInWater_ = false;               ///< 前フレームに水に触れていたか
    float submergence_ = 0.0f;               ///< 直近の沈み込み率(0..1)。デバッグ表示用
    float waterHeight_ = 0.0f;               ///< 直近の水面高さ。デバッグ表示用
    float floodProgress_ = 0.0f;             ///< 浸水の進み具合(0..1)
    float effectiveDensity_ = 0.45f;         ///< 直近の実効密度比。デバッグ表示用
    RC::WaterWaveParams waveParams_{};       ///< 水面エンティティから取り込んだ波パラメータ
    bool  hasWater_ = false;

    // 水面シェーダと共有する障害物情報（毎フレーム取り直す）
    RC::WaterObstacle obstacles_[RC::WaterSurface::kMaxObstacles]{};
    int   obstacleCount_ = 0;
    float reflectStrength_ = 1.0f;
    float reflectRange_ = 3.0f;

    /// @brief シーン内の WaterComponent を探して波パラメータを取り込む
    /// @details 水面は 1 枚しか無い前提で最初に見つかったものを使う。
    ///          毎フレーム呼ぶのは、水面の波パラメータが ImGui で変わりうるため。
    ///          エンティティ数は多くないので線形探索で十分。
    void RefreshWaterParams() {
        hasWater_ = false;
        Scene* scene = GetScene();
        if (!scene) return;

        for (auto& e : scene->GetEntities()) {
            if (!e || !e->IsActive()) continue;
            auto* water = e->GetComponent<WaterComponent>();
            if (!water) continue;
            auto* tr = e->GetComponent<TransformComponent>();

            // 水面メッシュのワールド Y が静水面。シェーダも World を通した
            // 座標で波を計算しているので、ここも Transform の Y を基準にする。
            // 詳細波のパラメータも含めて WaterComponent から一括で取り込む（シェーダと同じ波形にするため）
            waveParams_ = water->ToWaveParams(tr ? tr->position.y : 0.0f);
            hasWater_ = true;
            break;
        }
        if (!hasWater_) return;

        // 障害物はシェーダへ渡っているものと同じ値を貰う
        RC::Vector4 raw[RC::WaterSurface::kMaxObstacles]{};
        obstacleCount_ = RC::GetWaterObstacles(raw, RC::WaterSurface::kMaxObstacles);
        if (obstacleCount_ > RC::WaterSurface::kMaxObstacles) {
            obstacleCount_ = RC::WaterSurface::kMaxObstacles;
        }
        for (int i = 0; i < obstacleCount_; ++i) {
            obstacles_[i].pos = { raw[i].x, raw[i].y, raw[i].z };
            obstacles_[i].radius = raw[i].w;
        }
        reflectStrength_ = RC::GetWaterReflectStrength();
        reflectRange_ = RC::GetWaterReflectRange();
    }

    /// @brief 指定 XZ の水面高さ（GPU と同じ時刻・同じ式・同じ障害物）
    float SampleWater(float x, float z) const {
        return RC::WaterSurface::SampleHeight(
            waveParams_, RC::GetWaterTime(), x, z,
            obstacles_, obstacleCount_, reflectStrength_, reflectRange_);
    }

protected:
    nlohmann::json Serialize() override {
        nlohmann::json j;
        j["halfHeight"] = halfHeight;
        j["sampleRadius"] = sampleRadius;
        j["sampleRadiusSide"] = sampleRadiusSide;
        j["useFourSamples"] = useFourSamples;
        j["gravity"] = gravity;
        j["densityRatio"] = densityRatio;
        j["linearDrag"] = linearDrag;
        j["quadraticDrag"] = quadraticDrag;
        j["maxRiseSpeed"] = maxRiseSpeed;
        j["floodedDensityRatio"] = floodedDensityRatio;
        j["floodDuration"] = floodDuration;
        j["alignToSurface"] = alignToSurface;
        j["tiltSharpness"] = tiltSharpness;
        j["maxTiltDeg"] = maxTiltDeg;
        j["pitchSign"] = pitchSign;
        j["rollSign"] = rollSign;
        j["debugDraw"] = debugDraw;
        return j;
    }

    void Deserialize(const nlohmann::json& j) override {
        if (j.contains("halfHeight")) halfHeight = j["halfHeight"].get<float>();
        if (j.contains("sampleRadius")) sampleRadius = j["sampleRadius"].get<float>();
        if (j.contains("sampleRadiusSide")) sampleRadiusSide = (std::max)(j["sampleRadiusSide"].get<float>(), 0.0f);
        if (j.contains("useFourSamples")) useFourSamples = j["useFourSamples"].get<bool>();
        if (j.contains("gravity")) gravity = j["gravity"].get<float>();
        if (j.contains("densityRatio")) densityRatio = j["densityRatio"].get<float>();
        if (j.contains("linearDrag")) linearDrag = j["linearDrag"].get<float>();
        if (j.contains("quadraticDrag")) quadraticDrag = j["quadraticDrag"].get<float>();
        // 負の値が入ると std::clamp の lo > hi になり未定義動作なので下限を切る
        if (j.contains("maxRiseSpeed")) maxRiseSpeed = (std::max)(j["maxRiseSpeed"].get<float>(), 0.0f);
        if (j.contains("floodedDensityRatio")) floodedDensityRatio = j["floodedDensityRatio"].get<float>();
        if (j.contains("floodDuration")) floodDuration = (std::max)(j["floodDuration"].get<float>(), 0.0f);
        if (j.contains("alignToSurface")) alignToSurface = j["alignToSurface"].get<bool>();
        if (j.contains("tiltSharpness")) tiltSharpness = j["tiltSharpness"].get<float>();
        if (j.contains("maxTiltDeg")) maxTiltDeg = j["maxTiltDeg"].get<float>();
        if (j.contains("pitchSign")) pitchSign = j["pitchSign"].get<float>();
        if (j.contains("rollSign")) rollSign = j["rollSign"].get<float>();
        if (j.contains("debugDraw")) debugDraw = j["debugDraw"].get<bool>();
    }

    void OnCreate() override {
        RefreshWaterParams();
        if (!hasWater_) {
            Log::Print("[BuoyancyScript] WaterComponent が見つかりません。浮力は無効になります。");
        }
        // ランタイム専用タグの消し込み。
        // reset_motion は次の Update で必ず消えるが、その 1 フレームのあいだに
        // シーンを保存すると JSON へ焼き付く。実害は無いが差分が汚れるので、
        // 他のスクリプトと同じく OnCreate でも落としておく。
        if (auto* self = GetEntity()) {
            self->ClearTag("reset_motion");
        }
        // 重力の扱いについて：
        // RigidbodyComponent::Update() は useGravity が立っていれば毎フレーム
        // 重力を積む。ここで rb->useGravity を false に書き換えてしまうと、
        // その値は RigidbodyComponent::Serialize() でシーン JSON に焼き付き、
        // 後からこのスクリプトを外すと無重力の物体が残ってしまう。
        // なので Rigidbody 側の設定は触らず、「相手が重力を掛けているなら
        // こちらは掛けない」と毎フレーム判断する（OnUpdate 参照）。
    }

    void OnUpdate(float deltaTime) override {
        // パルス的なタグは早期 return より前で消す。
        // 下の guard の後ろに置くと、エディタで一時停止した瞬間（deltaTime=0）に
        // water_entered が立ったまま固定され、再開時に他スクリプトが二重に反応する。
        if (auto* self = GetEntity()) {
            self->ClearTag("water_entered");
            self->ClearTag("water_exited");

            // 動作確認用: 速度をゼロに戻す。
            // 落下させて挙動を測るとき、前回の上下動の速度が残っていると
            // 同じ高さから落としても結果がばらつき、「落下高さを変えても
            // 復帰時間が変わらない」ことを示せない。撮影モードから立てる。
            if (self->GetTagInt("reset_motion", 0) != 0) {
                self->ClearTag("reset_motion");
                velocity_ = {0.0f, 0.0f, 0.0f};
                if (auto* rb = GetComponent<RigidbodyComponent>()) {
                    rb->velocity = {0.0f, 0.0f, 0.0f};
                }
            }
        }

        if (deltaTime <= 0.0f) return;

        auto* tr = GetComponent<TransformComponent>();
        if (!tr) return;

        RefreshWaterParams();
        if (!hasWater_) return;

        // -------------------------------------------------------------
        // C-01: 水面との交差判定
        // -------------------------------------------------------------
        const float centerH = SampleWater(tr->position.x, tr->position.z);
        waterHeight_ = centerH;

        float surfaceH = centerH;
        float hF = centerH, hB = centerH, hR = centerH, hL = centerH;
        const float sideRadius = (sampleRadiusSide > 0.0f) ? sampleRadiusSide : sampleRadius;
        if (useFourSamples && sampleRadius > 0.0f) {
            // サンプル点はエンティティの向き（yaw）に合わせて回す。
            // 世界軸のまま取ると、船（T-15）のように向きが変わる物体では
            // 前後の傾きが左右の傾きとして出てしまい、船体と波の関係が合わない。
            // 向きが変わらない箱やブイでは yaw = 0 なので従来と同じ結果になる。
            const float cy = std::cos(tr->rotation.y);
            const float sy = std::sin(tr->rotation.y);
            const float fwdX = sy * sampleRadius,  fwdZ = cy * sampleRadius;  // 船首方向
            const float rgtX = cy * sideRadius,    rgtZ = -sy * sideRadius;   // 右舷方向

            hF = SampleWater(tr->position.x + fwdX, tr->position.z + fwdZ);
            hB = SampleWater(tr->position.x - fwdX, tr->position.z - fwdZ);
            hR = SampleWater(tr->position.x + rgtX, tr->position.z + rgtZ);
            hL = SampleWater(tr->position.x - rgtX, tr->position.z - rgtZ);
            // 4点の平均を喫水の基準にする。1点だと波の山谷を拾って
            // 物体が細かく上下し、船体サイズより短い波で暴れてしまう。
            surfaceH = (hF + hB + hR + hL) * 0.25f;
            waterHeight_ = surfaceH;
        }

        const float fullHeight = (std::max)(halfHeight * 2.0f, 1e-3f);
        const float bottomY = tr->position.y - halfHeight;
        // 沈み込み量：底面から水面までの距離を物体の高さでクランプ
        const float depth = std::clamp(surfaceH - bottomY, 0.0f, fullHeight);
        submergence_ = depth / fullHeight;

        const bool inWater = (submergence_ > 0.0f);
        // クリアは OnUpdate 冒頭で済んでいる。ここでは状態が変わった瞬間だけ立てる。
        if (inWater != wasInWater_) {
            // 着水／離水の瞬間を他のスクリプトへ知らせる（水しぶきや SE 用）
            GetEntity()->SetTag("in_water", inWater ? 1 : 0);
            GetEntity()->SetTag(inWater ? "water_entered" : "water_exited", 1);
            wasInWater_ = inWater;
        }

        // -------------------------------------------------------------
        // C-02: 沈み込み量に応じた浮力
        // -------------------------------------------------------------
        // アルキメデス：浮力 = ρ_water × g × V_沈 なので、
        // 加速度に直すと (ρ_water / ρ_object) × g × 沈み込み率
        //             = (g / densityRatio) × 沈み込み率
        auto* rb = GetComponent<RigidbodyComponent>();
        RC::Vector3& vel = rb ? rb->velocity : velocity_;

        // Rigidbody が既に重力を積んでいるなら、こちらは浮力ぶんだけを足す。
        // 両方が重力を掛けると 2G になってしまう。
        // RigidbodyComponent::Update() は enabled かつ非 kinematic かつ
        // useGravity のときだけ重力を積む（その条件をそのまま写している）。
        const bool rbAppliesGravity =
            rb && rb->useGravity && !rb->isKinematic && rb->IsEnabled();

        // 実際に掛かっている重力の大きさ。RigidbodyComponent の重力は
        // -9.81 固定なので、そちらに任せる場合は同じ値で浮力を計算しないと
        // 「densityRatio の割合だけ沈む」という関係が崩れる。
        const float effectiveGravity = rbAppliesGravity ? kRigidbodyGravity : gravity;

        // 浸水（T-15: 撃沈された船）。`flooding` タグが立っている間、実効密度比を
        // floodedDensityRatio へ寄せていく。1.0 を超えると浮力より重力が勝って沈む。
        //
        // densityRatio そのものを書き換えないのは、それが「設定値」だから。
        // スクリプトが他人の（あるいは自分の）設定値を書き換えると
        // シリアライズ経由でシーンデータに焼き付き、あとからスクリプトを外しても
        // 沈んだままの物体が残る（D-09 で踏んだのと同じ形の事故）。
        // 状態は実効値だけを動かし、設定値は触らない。
        {
            const bool flooding =
                GetEntity() && GetEntity()->GetTagInt("flooding", 0) != 0;
            const float step = (floodDuration > 1e-3f) ? (deltaTime / floodDuration) : 1.0f;
            floodProgress_ = std::clamp(floodProgress_ + (flooding ? step : -step), 0.0f, 1.0f);
        }
        effectiveDensity_ = RC::Lerp(densityRatio, floodedDensityRatio, floodProgress_);

        const float ratio = (std::max)(effectiveDensity_, 1e-3f);
        const float buoyAccel = (effectiveGravity / ratio) * submergence_;
        const float netAccel = buoyAccel - (rbAppliesGravity ? 0.0f : gravity);

        vel.y += netAccel * deltaTime;

        // 水中では抵抗で減速させる。陰的に解くと dt が大きくても発散しない
        // （明示的に vel *= (1 - k*dt) だと k*dt > 1 で符号が反転する）。
        // 負の drag は damp が発散・符号反転するので使用時に下限を切る。
        if (submergence_ > 0.0f) {
            // ① 線形抵抗（粘性）: 跳ねる回数＝収まるまでの時間を決める
            const float k = (std::max)(linearDrag, 0.0f) * submergence_;
            float damp = 1.0f / (1.0f + k * deltaTime);

            // ② 2乗抵抗（圧力抗力）: 着水時にどれだけ潜るかを決める。
            //    速いほど強く効くので、高いところから落ちても深く潜らない。
            //    速度の大きさは 3 成分から取る（横方向の突っ込みにも効かせる）。
            const float q = (std::max)(quadraticDrag, 0.0f) * submergence_;
            if (q > 0.0f) {
                const float speed = RC::Length(vel);
                damp *= 1.0f / (1.0f + q * speed * deltaTime);
            }

            vel.x *= damp;
            vel.y *= damp;
            vel.z *= damp;
        }
        // 浮き上がりの上限（深く沈めてから離したときの打ち上げを抑える）
        // ImGui の DragFloat は Ctrl+クリックで範囲外の値を打ち込めるため、
        // std::clamp の lo > hi（＝未定義動作）を防ぐために使用時に下限を切る。
        const float riseLimit = (std::max)(maxRiseSpeed, 0.0f);
        vel.y = std::clamp(vel.y, -riseLimit * 4.0f, riseLimit);

        // Rigidbody が速度を積分してくれる場合は位置更新を任せる。
        // 二重に足すと速度が 2 倍で効いてしまう。
        // isKinematic または enabled=false の Rigidbody は Update() 自体を
        // 早期 return するので、その場合はこちらで積分する
        // （そうしないと一切動かない）。
        const bool integrateHere = (!rb || rb->isKinematic || !rb->IsEnabled());
        if (integrateHere) {
            tr->position.x += vel.x * deltaTime;
            tr->position.y += vel.y * deltaTime;
            tr->position.z += vel.z * deltaTime;
        }

        // -------------------------------------------------------------
        // 水面の傾きに姿勢を合わせる
        // -------------------------------------------------------------
        if (alignToSurface && useFourSamples && sampleRadius > 0.0f && submergence_ > 0.0f) {
            const float spanFwd = sampleRadius * 2.0f;
            const float spanSide = (std::max)(sideRadius * 2.0f, 1e-3f);
            // 負値が入ると std::clamp の lo > hi になるため下限を切る
            const float maxTilt = (std::max)(maxTiltDeg, 0.0f) * 3.14159265f / 180.0f;

            // 前後の高さ差 → pitch、左右の高さ差 → roll。
            // 傾きの分母はそれぞれのサンプル間隔にする（船体が細いほど横揺れは大きい）。
            float pitch = pitchSign * std::atan2(hF - hB, spanFwd);
            float roll  = rollSign  * std::atan2(hR - hL, spanSide);
            pitch = std::clamp(pitch, -maxTilt, maxTilt);
            roll  = std::clamp(roll,  -maxTilt, maxTilt);

            // 沈み込みが浅いほど傾きの影響も薄い
            pitch *= submergence_;
            roll  *= submergence_;

            // 指数平滑（フレームレートに依存しない補間）
            const float t = RC::ExpSmoothingFactor(tiltSharpness, deltaTime);
            tr->rotation.x = RC::Lerp(tr->rotation.x, pitch, t);
            tr->rotation.z = RC::Lerp(tr->rotation.z, roll, t);

        }
    }

public:
    void OnImGui() override {
#if RC_ENABLE_IMGUI
        ImGui::TextUnformatted("C-01: 交差判定");
        ImGui::DragFloat("Half Height", &halfHeight, 0.05f, 0.01f, 50.0f);
        ImGui::DragFloat("Sample Radius (fore/aft)", &sampleRadius, 0.05f, 0.0f, 50.0f);
        ImGui::DragFloat("Sample Radius (side, 0=same)", &sampleRadiusSide, 0.05f, 0.0f, 50.0f);
        ImGui::Checkbox("Use 4 Samples", &useFourSamples);

        ImGui::Separator();
        ImGui::TextUnformatted("C-02: 浮力");
        ImGui::DragFloat("Gravity", &gravity, 0.05f, 0.0f, 30.0f);
        ImGui::SliderFloat("Density Ratio", &densityRatio, 0.05f, 1.5f);
        ImGui::DragFloat("Linear Drag (bobs)", &linearDrag, 0.05f, 0.0f, 20.0f);
        ImGui::DragFloat("Quadratic Drag (splash)", &quadraticDrag, 0.05f, 0.0f, 20.0f);
        ImGui::DragFloat("Max Rise Speed", &maxRiseSpeed, 0.1f, 0.1f, 50.0f);

        ImGui::Separator();
        ImGui::TextUnformatted("浸水（T-15: 撃沈時）");
        ImGui::SliderFloat("Flooded Density", &floodedDensityRatio, 0.5f, 3.0f);
        ImGui::DragFloat("Flood Duration", &floodDuration, 0.05f, 0.0f, 20.0f);
        ImGui::Text("Flood Progress: %.2f  (effective density %.2f)",
                    floodProgress_, effectiveDensity_);
        if (Entity* self = GetEntity()) {
            bool flooding = self->GetTagInt("flooding", 0) != 0;
            if (ImGui::Checkbox("Flooding", &flooding)) {
                if (flooding) self->SetTag("flooding", 1);
                else          self->ClearTag("flooding");
            }
        }

        // --- 挙動の見立て（ここを見ながら数値を合わせるのが早い） -------------
        // 平衡点まわりを線形化すると単振動になる。
        //   加速度 = (g / densityRatio) * (沈み込み率) - g なので、
        //   平衡から δ ずれたときの復元加速度は (g / (densityRatio * 全高)) * δ。
        //   つまり ω² = g / (densityRatio * 全高)。
        {
            const float full = (std::max)(halfHeight * 2.0f, 1e-3f);
            const float dens = (std::max)(densityRatio, 1e-3f);
            const float omega = std::sqrt((std::max)(gravity, 0.0f) / (dens * full));
            const float period = (omega > 1e-4f) ? (6.28318531f / omega) : 0.0f;
            // 減衰比：平衡点では 沈み込み率 == densityRatio なので k = linearDrag * densityRatio
            const float zeta = (omega > 1e-4f)
                ? ((std::max)(linearDrag, 0.0f) * dens) / (2.0f * omega) : 0.0f;
            const char* feel =
                (zeta < 0.15f) ? "ぷかぷか跳ね続ける" :
                (zeta < 0.55f) ? "2〜3回跳ねて収まる（狙い）" :
                (zeta < 1.0f)  ? "ほとんど跳ねない" : "跳ねずにじわっと戻る";
            ImGui::Separator();
            ImGui::Text("上下動の周期   : %.2f s", period);
            ImGui::Text("減衰比         : %.2f  %s", zeta, feel);
            ImGui::Text("水没時の浮力   : %+.2f m/s^2 (重力比 %.2f)",
                        gravity / dens - gravity, (gravity > 0.0f) ? (1.0f / dens - 1.0f) : 0.0f);
            ImGui::Text("平衡時の露出   : 水面上に %.0f %%", (1.0f - dens) * 100.0f);
            ImGui::TextWrapped(
                "跳ね返りが遅いなら Density Ratio を下げるか物体を小さくする（周期が縮む）。"
                "着水で潜りすぎるなら Quadratic Drag を上げる。跳ねすぎるなら Linear Drag を上げる。");
        }

        ImGui::Separator();
        ImGui::Checkbox("Align To Surface", &alignToSurface);
        ImGui::DragFloat("Tilt Sharpness", &tiltSharpness, 0.1f, 0.1f, 30.0f);
        ImGui::DragFloat("Max Tilt (deg)", &maxTiltDeg, 0.5f, 0.0f, 80.0f);
        ImGui::DragFloat("Pitch Sign", &pitchSign, 1.0f, -1.0f, 1.0f);
        ImGui::DragFloat("Roll Sign", &rollSign, 1.0f, -1.0f, 1.0f);
        ImGui::Checkbox("Debug Draw", &debugDraw);

        ImGui::Separator();
        if (!hasWater_) {
            ImGui::TextUnformatted("[!] WaterComponent が見つかりません");
        }
        ImGui::Text("Water Height : %.3f", waterHeight_);
        ImGui::Text("Submergence  : %.1f %%", submergence_ * 100.0f);
        ImGui::Text("Equilibrium  : %.1f %% (= Density Ratio)", densityRatio * 100.0f);
        if (auto* rb = GetComponent<RigidbodyComponent>()) {
            ImGui::Text("Velocity Y   : %.3f (Rigidbody)", rb->velocity.y);
        } else {
            ImGui::Text("Velocity Y   : %.3f (self)", velocity_.y);
        }
#endif
    }

    void OnDebugRender() override {
#if RC_ENABLE_IMGUI
        if (!debugDraw || !hasWater_) return;
        auto* tr = GetComponent<TransformComponent>();
        if (!tr) return;

        const RC::Vector4 waterLineColor = { 0.2f, 0.7f, 1.0f, 1.0f };
        const RC::Vector4 bottomColor    = { 1.0f, 0.4f, 0.2f, 1.0f };

        // 水面のサンプル点（4点なら十字に描く）
        const float r = sampleRadius;
        const float rs = (sampleRadiusSide > 0.0f) ? sampleRadiusSide : sampleRadius;
        const float cy = std::cos(tr->rotation.y);
        const float sy = std::sin(tr->rotation.y);
        const RC::Vector3 pts[4] = {
            { tr->position.x + sy * r,  0.0f, tr->position.z + cy * r },
            { tr->position.x - sy * r,  0.0f, tr->position.z - cy * r },
            { tr->position.x + cy * rs, 0.0f, tr->position.z - sy * rs },
            { tr->position.x - cy * rs, 0.0f, tr->position.z + sy * rs },
        };
        if (useFourSamples && r > 0.0f) {
            for (const auto& p : pts) {
                const RC::Vector3 s = { p.x, SampleWater(p.x, p.z), p.z };
                RC::DrawWireSphere3D(s, 0.15f, waterLineColor, 8, 8, true);
            }
            RC::DrawLine3D({ pts[0].x, SampleWater(pts[0].x, pts[0].z), pts[0].z },
                           { pts[1].x, SampleWater(pts[1].x, pts[1].z), pts[1].z },
                           waterLineColor, true);
            RC::DrawLine3D({ pts[2].x, SampleWater(pts[2].x, pts[2].z), pts[2].z },
                           { pts[3].x, SampleWater(pts[3].x, pts[3].z), pts[3].z },
                           waterLineColor, true);
        }

        // 喫水線（水面）と底面を線でつないで、沈み込み量を目で見えるようにする
        const RC::Vector3 waterPt = { tr->position.x, waterHeight_, tr->position.z };
        const RC::Vector3 bottomPt = { tr->position.x, tr->position.y - halfHeight, tr->position.z };
        RC::DrawWireSphere3D(waterPt, 0.2f, waterLineColor, 10, 10, true);
        RC::DrawWireSphere3D(bottomPt, 0.2f, bottomColor, 10, 10, true);
        RC::DrawLine3D(waterPt, bottomPt, bottomColor, true);
#endif
    }
};

REGISTER_SCRIPT(BuoyancyScript)
