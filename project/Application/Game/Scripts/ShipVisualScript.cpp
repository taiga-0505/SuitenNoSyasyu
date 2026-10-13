#include "ECS/ScriptableEntity.h"
#include "ECS/ScriptRegistry.h"
#include "ECS/TransformComponent.h"
#include "ECS/PrimitiveMeshComponent.h"
#include "Common/Log/Log.h"
#include "RenderCommon.h"
#include "Scene.h"
#include "Game/Framework/PartPose.h"

#if RC_ENABLE_IMGUI
#include "imgui/imgui.h"
#endif

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

// ============================================================================
// 敵の船（ShipEnemyScript）の見た目：箱 1 個の船体に、船首・甲板・舷側・船室・マスト・帆・旗・大砲を付ける
//
//   - 本体（スポナーが作る Box）はそのまま船体として使う。当たり判定・被弾の赤点滅・撃沈で沈む動きは
//     今までどおり本体が担当し、このスクリプトは「部位を本体の姿勢に合わせて置く」だけ。
//   - 部位の大きさは本体の scale（幅 x・高さ y・長さ z、m）を基準に決めるので、
//     スポナーの enemyScale を変えれば船全体の大きさが変わる。+Z が船首。
//   - 本体の姿勢は ShipEnemyScript（XZ と船首方位）と BuoyancyScript（高さと傾き）が書く。
//     両方が書き終わったあとの姿勢で部位を置きたいので、スクリプトの並びは
//     ShipEnemyScript → BuoyancyScript → ShipVisualScript にすること（NativeScript は並び順に更新される）。
//   - 部位は当たり判定もスクリプトも持たない（弾・ウェーブの生存数に影響しない）。
//   - 本体が被弾で赤く点滅したら、部位も同じ色にする（EnemyBaseScript::ApplyColor は本体しか塗らないため）。
//
//   ※ エンジンの描画は Transform の親子を解決しないので、部位のワールド姿勢は PartPose で合成して書き込む。
// ============================================================================

/// @brief ShipEnemyScript の船に部位を付けて帆船に見せる
class ShipVisualScript : public ScriptableEntity {
public:
  RC::Vector4 deckColor = {0.62f, 0.46f, 0.28f, 1.0f};
  RC::Vector4 trimColor = {0.24f, 0.15f, 0.08f, 1.0f};   ///< 舷側・マスト
  RC::Vector4 cabinColor = {0.84f, 0.80f, 0.70f, 1.0f};
  RC::Vector4 roofColor = {0.55f, 0.14f, 0.12f, 1.0f};
  RC::Vector4 sailColor = {0.93f, 0.90f, 0.82f, 1.0f};
  RC::Vector4 flagColor = {0.12f, 0.12f, 0.14f, 1.0f};   ///< 敵なので黒旗
  RC::Vector4 cannonColor = {0.16f, 0.16f, 0.18f, 1.0f};
  int cannonsPerSide = 3;
  float mastHeightScale = 1.0f; ///< マストと帆の高さの倍率
  float sailBillowDeg = 14.0f;  ///< 帆が風で前へはらむ角度

  /// @brief 見た目のパラメータを JSON に書き出す
  /// @return 各部位の色・片舷の大砲数・マスト高さ倍率・帆のはらみ角度を格納した JSON
  nlohmann::json Serialize() override {
    auto v4 = [](const RC::Vector4 &c) { return nlohmann::json{c.x, c.y, c.z, c.w}; };
    return {{"deckColor", v4(deckColor)},   {"trimColor", v4(trimColor)},     {"cabinColor", v4(cabinColor)},
            {"roofColor", v4(roofColor)},   {"sailColor", v4(sailColor)},     {"flagColor", v4(flagColor)},
            {"cannonColor", v4(cannonColor)}, {"cannonsPerSide", cannonsPerSide},
            {"mastHeightScale", mastHeightScale}, {"sailBillowDeg", sailBillowDeg}};
  }

  /// @brief JSON から見た目のパラメータを読み込む
  /// @param j 読み込み元の JSON（存在し型が合うキーだけ反映する）
  void Deserialize(const nlohmann::json &j) override {
    // 色は要素数 4 以上の配列のときだけ反映するヘルパー
    auto readV4 = [&](const char *key, RC::Vector4 &out) {
      if (!j.contains(key)) return;
      const auto &c = j[key];
      if (c.is_array() && c.size() >= 4) out = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>(), c[3].get<float>()};
    };
    readV4("deckColor", deckColor);
    readV4("trimColor", trimColor);
    readV4("cabinColor", cabinColor);
    readV4("roofColor", roofColor);
    readV4("sailColor", sailColor);
    readV4("flagColor", flagColor);
    readV4("cannonColor", cannonColor);
    if (j.contains("cannonsPerSide") && j["cannonsPerSide"].is_number()) cannonsPerSide = j["cannonsPerSide"].get<int>();
    if (j.contains("mastHeightScale") && j["mastHeightScale"].is_number()) mastHeightScale = j["mastHeightScale"].get<float>();
    if (j.contains("sailBillowDeg") && j["sailBillowDeg"].is_number()) sailBillowDeg = j["sailBillowDeg"].get<float>();
  }

#if RC_ENABLE_IMGUI
  /// @brief 部位数・船体寸法を表示し、帆のはらみ角度を編集できるようにする
  /// @note 色・大砲数・マスト高さは部位の生成時にしか反映されない（再出現で反映）
  void OnImGui() override {
    ImGui::Text("Parts: %d  Hull: %.1f x %.1f x %.1f m", static_cast<int>(parts_.size()), hull_.x, hull_.y, hull_.z);
    ImGui::DragFloat("Sail Billow##ShipVis", &sailBillowDeg, 0.5f, 0.0f, 45.0f);
    ImGui::TextDisabled("colors / cannons / mast height are applied on respawn");
  }
#endif

protected:
  /// @brief 部位を未生成の状態にしておく（実際の生成は最初の OnUpdate で行う）
  void OnCreate() override {
    // 部位は最初の Update で作る（OnCreate の時点では本体の scale がまだ入っていないことがあるため）
    built_ = false;
  }

  /// @brief 初回に部位を生成し、毎フレーム被弾色の同期と部位の配置を行う
  /// @param deltaTime 前フレームからの経過時間（秒）。旗のなびきに使う
  void OnUpdate(float deltaTime) override {
    Scene *scene = GetScene();
    Entity *self = GetEntity();
    if (!scene || !self) return;
    auto *tr = GetComponent<TransformComponent>();
    if (!tr) return;
    // 初回だけ部位を生成する（OnCreate の時点では本体の scale が確定していないことがあるため）
    if (!built_) Build(scene, *self, *tr);

    time_ += deltaTime;
    SyncTint();
    PlaceParts(*tr);
  }

  /// @brief 生成した部位エンティティをすべて破棄する
  void OnDestroy() override {
    for (auto &p : parts_) {
      if (auto e = p.entity.lock()) e->Destroy();
    }
    parts_.clear();
    built_ = false;
  }

private:
  using Mat3 = PartPose::Mat3;
  /// @brief 部位の動き方（Static: 本体に固定 / Flag: 旗としてなびかせる）
  enum class Role { Static, Flag };

  /// @brief 生成した部位 1 つぶんの情報（本体ローカルでの配置と元の色）
  struct Part {
    std::weak_ptr<Entity> entity;
    RC::Vector3 offset{};  ///< 本体中心からの位置（m、本体ローカル）
    RC::Vector3 scale{};   ///< Transform の scale（m）
    Mat3 local = PartPose::Identity();
    RC::Vector4 color{1, 1, 1, 1};
    Role role = Role::Static;
  };

  /// @brief 部位エンティティを 1 つ生成して parts_ に登録する
  /// @param scene 生成先のシーン
  /// @param self 船の本体エンティティ（名前・親 GUID・初期位置の元）
  /// @param type 部位のプリミティブ形状
  /// @param color 部位の色（被弾色から戻すときの色にもなる）
  /// @param offset 本体中心からの位置（m、本体ローカル）
  /// @param scale 部位の Transform の scale（m）
  /// @param local 本体に対する部位の向き
  /// @param role 部位の動き方
  void AddPart(Scene *scene, Entity &self, PrimitiveType type, const RC::Vector4 &color, const RC::Vector3 &offset,
           const RC::Vector3 &scale, const Mat3 &local = PartPose::Identity(), Role role = Role::Static) {
    auto e = scene->CreateEntity(self.GetName() + "_part");
    if (!e) return;
    e->SetParentGuid(self.Guid()); // Hierarchy で船の下にまとまるように（描画上の親子ではない）
    // PlaceParts で配置されるまで大きく映らないよう、本体の位置に極小スケールで仮置きする
    auto &ptr = e->AddComponent<TransformComponent>();
    if (auto *str = self.GetComponent<TransformComponent>()) ptr.position = str->position;
    ptr.scale = {0.01f, 0.01f, 0.01f};
    auto &pm = e->AddComponent<PrimitiveMeshComponent>();
    pm.type = type;
    pm.color = color;
    pm.shininess = 8.0f;
    scene->InitDynamicEntityRuntime(*e);
    // 毎フレーム PlaceParts で使う本体ローカルの配置情報を記録する
    Part p;
    p.entity = e;
    p.offset = offset;
    p.scale = scale;
    p.local = local;
    p.color = color;
    p.role = role;
    parts_.push_back(p);
  }

  /// @brief 本体の寸法を基準に船首・甲板・手すり・船室・マスト・帆・旗・大砲の部位を生成する
  /// @param scene 生成先のシーン
  /// @param self 船の本体エンティティ
  /// @param tr 本体の Transform（scale を船体の寸法として使う）
  void Build(Scene *scene, Entity &self, const TransformComponent &tr) {
    using namespace PartPose;
    built_ = true;
    // 本体の箱の寸法（幅 W・高さ H・長さ L）。箱は一辺 1 で生成されるので scale ＝ 寸法
    const float W = (std::max)(std::fabs(tr.scale.x), 0.5f);
    const float H = (std::max)(std::fabs(tr.scale.y), 0.3f);
    const float L = (std::max)(std::fabs(tr.scale.z), 1.0f);
    hull_ = {W, H, L};
    const float top = H * 0.5f;
    const float mast = (std::max)(mastHeightScale, 0.2f) * (std::min)(L * 0.7f, 6.5f);

    // 船体の色は本体（スポナーの primitiveColor）に揃える
    RC::Vector4 hullColor = {0.36f, 0.22f, 0.12f, 1.0f};
    if (auto *pm = self.GetComponent<PrimitiveMeshComponent>()) {
      hullColor = pm->color;
      if (pm->meshHandle >= 0) {
        if (auto *mat = RC::GetPrimitiveMeshMaterialPtr(pm->meshHandle)) hullColor = mat->color;
      }
    }
    baseHullColor_ = hullColor;

    const Mat3 diamond = RotY(kPi * 0.25f); // 45°回した箱で尖った船首
    const float d = W / std::sqrt(2.0f);
    // 船首（船体と同じ色。対角線の長さ＝船幅）
    AddPart(scene, self, PrimitiveType::Box, hullColor, {0.0f, 0.0f, L * 0.5f}, {d, H, d}, diamond);
    // 甲板（船体の上に薄い板）と船首の甲板
    AddPart(scene, self, PrimitiveType::Box, deckColor, {0.0f, top + 0.04f, 0.0f}, {W * 0.9f, 0.08f, L * 0.98f});
    AddPart(scene, self, PrimitiveType::Box, deckColor, {0.0f, top + 0.04f, L * 0.5f}, {d * 0.85f, 0.08f, d * 0.85f}, diamond);
    // 舷側の手すり
    for (float s : {1.0f, -1.0f}) {
      AddPart(scene, self, PrimitiveType::Box, trimColor, {s * (W * 0.5f - 0.06f), top + 0.2f, 0.0f}, {0.12f, 0.4f, L * 0.98f});
    }
    // 船尾の船室と屋根
    AddPart(scene, self, PrimitiveType::Box, cabinColor, {0.0f, top + 0.6f, -L * 0.34f}, {W * 0.72f, 1.2f, L * 0.2f});
    AddPart(scene, self, PrimitiveType::Box, roofColor, {0.0f, top + 1.25f, -L * 0.34f}, {W * 0.82f, 0.12f, L * 0.23f});
    // マスト（円柱は半径 1・高さ 1 で生成される）
    const float mastZ = L * 0.08f;
    AddPart(scene, self, PrimitiveType::Cylinder, trimColor, {0.0f, top + mast * 0.5f, mastZ}, {0.13f, mast, 0.13f});
    // 横帆（追い風で前へはらむ）。帆桁も付ける
    const float sailH = mast * 0.55f;
    const float sailY = top + mast * 0.58f;
    const float billow = sailBillowDeg * kDeg;
    AddPart(scene, self, PrimitiveType::Box, sailColor, {0.0f, sailY, mastZ + 0.2f + sailH * 0.5f * std::sin(billow)},
        {W * 1.5f, sailH, 0.1f}, RotX(billow));
    AddPart(scene, self, PrimitiveType::Cylinder, trimColor, {0.0f, sailY + sailH * 0.5f, mastZ + 0.15f},
        {0.07f, W * 1.6f, 0.07f}, RotZ(kHalfPi));
    // 旗（マストの先で船首側へなびく）
    AddPart(scene, self, PrimitiveType::Box, flagColor, {0.0f, top + mast - 0.3f, mastZ + 0.55f}, {0.04f, 0.5f, 1.0f},
        Identity(), Role::Flag);
    // 大砲：左右の舷側から外へ突き出す（ShipEnemyScript の gunOffset ≒ 2.2m に砲口が来る長さ）
    const int n = (std::max)(cannonsPerSide, 0);
    for (int i = 0; i < n; ++i) {
      const float t = (n == 1) ? 0.5f : static_cast<float>(i) / static_cast<float>(n - 1);
      const float z = -L * 0.18f + t * L * 0.4f;
      for (float s : {1.0f, -1.0f}) {
        AddPart(scene, self, PrimitiveType::Cylinder, cannonColor, {s * (W * 0.5f + 0.2f), top + 0.25f, z},
            {0.17f, 0.9f, 0.17f}, RotZ(kHalfPi)); // 円柱の軸（Y）を横（X）へ寝かせる
      }
    }
    PlaceParts(tr); // 生成直後に原点で 1 フレーム映らないように
    Log::Print("[ShipVisualScript] built " + std::to_string(parts_.size()) + " parts for " + self.GetName());
  }

  /// @brief 全部位を本体の位置・回転に合わせて配置する（旗はなびかせる）
  /// @param tr 本体の Transform
  void PlaceParts(const TransformComponent &tr) {
    using namespace PartPose;
    const Mat3 body = FromEngineEuler(tr.rotation);
    // 旗は Y 軸まわりに ±0.35rad を周期的に振ってなびかせる
    const float flag = 0.35f * std::sin(time_ * 5.0f);
    for (const auto &p : parts_) {
      auto e = p.entity.lock();
      if (!e) continue;
      auto *ptr = e->GetComponent<TransformComponent>();
      if (!ptr) continue;
      Mat3 local = p.local;
      if (p.role == Role::Flag) local = MatMul(RotY(flag), local);
      // 本体ローカルの位置・向きを本体の姿勢で回してワールドへ変換する
      ptr->position = AddV(tr.position, Apply(p.offset, body));
      ptr->rotation = ToEngineEuler(MatMul(local, body));
      ptr->scale = p.scale;
      // PrimitiveMesh は次フレーム冒頭まで同期されないので、本体と 1 フレームずれないよう即座に写す
      if (auto *pm = e->GetComponent<PrimitiveMeshComponent>()) {
        if (pm->meshHandle >= 0) {
          if (auto *t = RC::GetPrimitiveMeshTransformPtr(pm->meshHandle)) *t = ptr->ToTransform();
        }
      }
    }
  }

  /// @brief 本体が被弾色になっていたら部位も同じ色に、戻ったら元の色に戻す
  void SyncTint() {
    auto *pm = GetComponent<PrimitiveMeshComponent>();
    if (!pm || pm->meshHandle < 0) return;
    auto *mat = RC::GetPrimitiveMeshMaterialPtr(pm->meshHandle);
    if (!mat) return;
    // 本体の現在色が元の船体色から閾値以上ずれていれば被弾色とみなす
    const RC::Vector4 c = mat->color;
    const float diff = std::fabs(c.x - baseHullColor_.x) + std::fabs(c.y - baseHullColor_.y) +
                       std::fabs(c.z - baseHullColor_.z);
    const bool tinted = diff > 0.05f;
    // 状態も被弾色も前回と同じなら塗り直さない
    if (tinted == tinted_ && (!tinted || (c.x == lastTint_.x && c.y == lastTint_.y && c.z == lastTint_.z))) return;
    tinted_ = tinted;
    lastTint_ = c;
    // 被弾中は本体と同じ色、戻ったら各部位の元の色にする
    for (const auto &p : parts_) {
      auto e = p.entity.lock();
      if (!e) continue;
      auto *ppm = e->GetComponent<PrimitiveMeshComponent>();
      if (!ppm || ppm->meshHandle < 0) continue;
      if (auto *pmat = RC::GetPrimitiveMeshMaterialPtr(ppm->meshHandle)) pmat->color = tinted ? c : p.color;
    }
  }

  std::vector<Part> parts_;
  bool built_ = false;
  float time_ = 0.0f;
  RC::Vector3 hull_{0, 0, 0};
  RC::Vector4 baseHullColor_{1, 1, 1, 1};
  RC::Vector4 lastTint_{1, 1, 1, 1};
  bool tinted_ = false;
};

REGISTER_SCRIPT(ShipVisualScript)
