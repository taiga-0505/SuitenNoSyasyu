#include "ECS/ScriptableEntity.h"
#include "ECS/ScriptRegistry.h"
#include "ECS/TransformComponent.h"
#include "ECS/ColliderComponent.h"
#include "ECS/ModelRendererComponent.h"
#include "ECS/PrimitiveMeshComponent.h"
#include "ECS/GPUParticleComponent.h"
#include "ECS/NativeScriptComponent.h"
#include "ECS/CameraComponent.h"
#include "Particle/GPUParticle.h"
#include "RenderCommon.h"
#include "Scene.h"
#include "Common/Log/Log.h"
#include "Game/Framework/GameSession.h"
#include "Game/Framework/PartPose.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <memory>
#include <string>
#include <vector>

#if RC_ENABLE_IMGUI
#include "imgui/imgui.h"
#endif

// =====================================================================
// 宝箱：弾を何発か当てると開いてポイント（スコア）が手に入る
// =====================================================================
// 敵（EnemyBaseScript）とは別物として扱う。理由は 2 つ。
//   ・敵として数えると「全滅でクリア」の判定や敵 HP バーの集計に混ざってしまう
//   ・撃破数ではなく「宝箱取得数」としてリザルトに出したい
// そのためタグは `is_enemy` ではなく `is_item` を立て、
// WaterBullet 側は `is_item` を当たり判定の対象に含めている。
//
// 弾からダメージを受ける仕組みは敵と同じ（疎結合のタグ方式）:
//   WaterBullet が `pending_damage` を積む → ここが OnUpdate で拾って TakeDamage
//
// 見た目は ModelRendererComponent でも PrimitiveMeshComponent でもよい。
// どちらも無いときは、見えないと困るので茶色い Box を仮で付ける。
//
// JSON（scriptDataList）で指定できる値:
//   "maxHp"        : 何発当てれば開くか（既定 3）。"hp" だけ書いてもよい
//   "points"       : 開いたときに加算されるスコア（既定 100）
//   "bobAmplitude" : ふわふわ上下する幅(m)。0 で静止（既定 0.2）
//   "bobSpeed"     : 上下の速さ（既定 1.5）
//   "spinSpeedDeg" : Y 軸回転の速さ(度/秒)。0 で回さない（既定 0）
//   "openDuration" : 開いてから実体を消すまでの秒数（既定 1.2）
//   "chestLook"       : 宝箱の見た目にするか（既定 true。false で従来の箱のまま）
//   "baseHeightRatio" : 本体の箱を下箱として何割の高さにするか（既定 0.62。残りにふたが載る）
//   "lidOpenDeg"      : 開いたときにふたが開く角度（度、既定 105）
//   "lidColor" / "metalColor" / "treasureColor" : ふた・金具・中の財宝の色 RGBA
//
// 見た目（宝箱）
//   本体の Box をそのまま下箱にし、ふた（箱＋かまぼこ形）・金の縁取り・角金具・帯・錠前・中の財宝を
//   別エンティティ（PrimitiveMesh）で付ける。毎フレーム本体の位置・回転・大きさに合わせて置くので、
//   ふわふわ上下・被弾の点滅・開いたときに跳ねて回って縮む演出にもそのまま付いてくる。
//   錠前は -Z 側（レールを進む自機から見える面）、ちょうつがいは +Z 側。開くとふたが後ろへ倒れ、
//   中の財宝（光る金貨の山）がせり上がる。
//   当たり判定・HP・スコアは今までどおり本体が持つ（部位には当たり判定もスクリプトも無い）。
//   部位は再生中だけ作る（編集モードで作るとシーン保存や Undo の対象に混ざるため）。
//   ※ エンジンの描画は Transform の親子を解決しないので、部位のワールド姿勢は PartPose で合成して書き込む。

/// @brief 宝箱。弾を当てると HP が減り、0 になると開いてポイントを獲得する
class TreasureChestScript : public ScriptableEntity {
public:
  int hp = 3;
  int maxHp = 3;
  /// @brief 開いたときにスコアへ加算する点数
  int points = 100;

  // --- 待機中の演出 ---
  float bobAmplitude = 0.2f;
  float bobSpeed = 1.5f;
  float spinSpeedDeg = 0.0f;

  // --- 開いたあとの演出 ---
  /// @brief 開いてから実体を消すまでの秒数
  float openDuration = 1.2f;
  /// @brief 開いた瞬間に飛び上がる高さ(m)
  float popHeight = 1.0f;

  bool isOpened = false;

  // --- 見た目（宝箱）---
  bool chestLook = true;
  float baseHeightRatio = 0.62f;
  float lidOpenDeg = 105.0f;
  RC::Vector4 lidColor = {0.46f, 0.27f, 0.11f, 1.0f};
  RC::Vector4 metalColor = {0.93f, 0.74f, 0.26f, 1.0f};
  RC::Vector4 treasureColor = {1.0f, 0.86f, 0.32f, 1.0f};

protected:
  /// @brief 宝箱の設定を JSON に書き出す
  /// @return HP・ポイント・演出・見た目の各パラメータを格納した JSON
  nlohmann::json Serialize() override {
    nlohmann::json j;
    j["hp"] = hp;
    j["maxHp"] = maxHp;
    j["points"] = points;
    j["bobAmplitude"] = bobAmplitude;
    j["bobSpeed"] = bobSpeed;
    j["spinSpeedDeg"] = spinSpeedDeg;
    j["openDuration"] = openDuration;
    j["popHeight"] = popHeight;
    auto v4 = [](const RC::Vector4& c) { return nlohmann::json{c.x, c.y, c.z, c.w}; };
    j["chestLook"] = chestLook;
    j["baseHeightRatio"] = baseHeightRatio;
    j["lidOpenDeg"] = lidOpenDeg;
    j["lidColor"] = v4(lidColor);
    j["metalColor"] = v4(metalColor);
    j["treasureColor"] = v4(treasureColor);
    return j;
  }

  /// @brief JSON から宝箱の設定を読み込む
  /// @param j 読み込み元の JSON（存在するキーだけ反映する）
  void Deserialize(const nlohmann::json& j) override {
    // EnemyBaseScript と同じ流儀: "maxHp" だけ書けば hp も満タンに揃える
    if (j.contains("maxHp")) {
      maxHp = j["maxHp"].get<int>();
      hp = maxHp;
    }
    if (j.contains("hp")) {
      hp = j["hp"].get<int>();
      if (!j.contains("maxHp")) maxHp = hp;
    }
    if (j.contains("points")) points = j["points"].get<int>();
    if (j.contains("bobAmplitude")) bobAmplitude = j["bobAmplitude"].get<float>();
    if (j.contains("bobSpeed")) bobSpeed = j["bobSpeed"].get<float>();
    if (j.contains("spinSpeedDeg")) spinSpeedDeg = j["spinSpeedDeg"].get<float>();
    if (j.contains("openDuration")) openDuration = j["openDuration"].get<float>();
    if (j.contains("popHeight")) popHeight = j["popHeight"].get<float>();
    // 色は要素数 4 以上の配列のときだけ反映するヘルパー
    auto readV4 = [&](const char* key, RC::Vector4& out) {
      if (!j.contains(key) || !j[key].is_array() || j[key].size() < 4) return;
      const auto& c = j[key];
      out = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>(), c[3].get<float>()};
    };
    if (j.contains("chestLook") && j["chestLook"].is_boolean()) chestLook = j["chestLook"].get<bool>();
    if (j.contains("baseHeightRatio") && j["baseHeightRatio"].is_number()) baseHeightRatio = j["baseHeightRatio"].get<float>();
    if (j.contains("lidOpenDeg") && j["lidOpenDeg"].is_number()) lidOpenDeg = j["lidOpenDeg"].get<float>();
    readV4("lidColor", lidColor);
    readV4("metalColor", metalColor);
    readV4("treasureColor", treasureColor);
    // 不正な値で即開いたり開けなくなったりしないよう、HP を 1 以上に補正する
    if (maxHp < 1) maxHp = 1;
    if (hp < 1) hp = maxHp;
  }

  /// @brief 弾の当たり判定対象のタグを立て、コライダーや見た目が無ければ補い、演出の状態を初期化する
  void OnCreate() override {
    Entity* self = GetEntity();
    if (!self) return;

    // 弾の当たり判定対象にする（敵とは別のタグ）
    self->SetTag("is_item", 1);

    // コライダーがなければフェールセーフとして追加
    if (!self->HasComponent<ColliderComponent>()) {
      auto* col = &self->AddComponent<ColliderComponent>();
      col->shape = ColliderComponent::Shape::Sphere;
      col->radius = 1.0f; // スケール依存
      col->isTrigger = false;
    }

    // 見た目が何も無ければ仮の箱を付ける（モデル差し替え前でも動作確認できるように）
    if (!self->HasComponent<ModelRendererComponent>() && !self->HasComponent<PrimitiveMeshComponent>()) {
      auto* pm = &self->AddComponent<PrimitiveMeshComponent>();
      pm->type = PrimitiveType::Box;
      pm->meshHandle = RC::GenerateBox(1.0f, 1.0f, 1.0f);
      if (pm->meshHandle >= 0) {
        if (auto* mat = RC::GetPrimitiveMeshMaterialPtr(pm->meshHandle)) {
          mat->color = {0.55f, 0.35f, 0.15f, 1.0f};
        }
      }
    }

    // 基準位置は最初の OnUpdate で取る（JSON の Transform が確実に入ったあと）
    hasBase_ = false;
    isOpened = false;
    openTimer_ = 0.0f;
    bobTime_ = 0.0f;
  }

  /// @brief 被弾処理・被弾フラッシュの解除・待機中の上下動と、開いたあとの演出・消滅を行う
  /// @param deltaTime 前フレームからの経過時間（秒）
  void OnUpdate(float deltaTime) override {
    Entity* self = GetEntity();
    if (!self) return;

    // 弾が積んだダメージを拾う
    int dmg = self->GetTagInt("pending_damage", 0);
    if (dmg > 0) {
      self->ClearTag("pending_damage");
      TakeDamage(dmg);
    }

    // 被弾フラッシュを戻す
    if (flashTimer_ > 0.0f) {
      flashTimer_ -= deltaTime;
      if (flashTimer_ <= 0.0f) RestoreColor();
    }

    auto* tr = self->GetComponent<TransformComponent>();
    if (!tr) return;
    // 宝箱の部位は基準の大きさを覚える前に作る（下箱の高さを縮めるので、その値を基準にする）
    if (!chestBuilt_ && chestLook) {
      SceneContext* ctx = GetSceneContext();
      if (ctx && ctx->isPlaying()) BuildChest(*tr);
    }
    if (!hasBase_) {
      basePosition_ = tr->position;
      baseScale_ = tr->scale;
      baseRotation_ = tr->rotation;
      hasBase_ = true;
    }

    if (!isOpened) {
      // 待機中: ふわふわ上下 ＋ 任意で回転
      bobTime_ += deltaTime;
      tr->position = basePosition_;
      tr->position.y += std::sin(bobTime_ * bobSpeed) * bobAmplitude;
      if (spinSpeedDeg != 0.0f) {
        tr->rotation.y = baseRotation_.y + bobTime_ * spinSpeedDeg * (3.14159265f / 180.0f);
      }
      // UI 用に HP を公開
      self->SetTag("current_hp", hp);
      self->SetTag("max_hp", maxHp);
      PlaceChest(*tr, 0.0f);
      return;
    }

    // 開いたあと: ぴょんと跳ねて、くるっと回りながら縮んで消える
    openTimer_ += deltaTime;
    const float t = (openDuration > 0.0f) ? (openTimer_ / openDuration) : 1.0f;
    const float clamped = (t < 0.0f) ? 0.0f : (t > 1.0f ? 1.0f : t);
    // 放物線で跳ねる（0 → 頂点 → 0）
    const float hop = 4.0f * clamped * (1.0f - clamped) * popHeight;
    tr->position = basePosition_;
    tr->position.y += hop;
    tr->rotation.y = baseRotation_.y + clamped * 6.28318530718f;
    // 後半で縮む
    const float shrink = (clamped < 0.5f) ? 1.0f : (1.0f - (clamped - 0.5f) * 2.0f);
    const float s = (shrink < 0.0f) ? 0.0f : shrink;
    tr->scale = {baseScale_.x * s, baseScale_.y * s, baseScale_.z * s};
    // ふたは跳ね上がる前半（openDuration の 35%）で一気に開く
    {
      const float lt = (openDuration > 0.0f) ? (std::min)(1.0f, openTimer_ / (openDuration * 0.35f)) : 1.0f;
      const float eased = 1.0f - (1.0f - lt) * (1.0f - lt) * (1.0f - lt);
      PlaceChest(*tr, eased);
    }

    if (openTimer_ >= openDuration) {
      self->Destroy();
    }
  }

  /// @brief 宝箱の部位エンティティをすべて破棄する
  void OnDestroy() override {
    for (auto& p : chestParts_) {
      if (auto e = p.entity.lock()) e->Destroy();
    }
    chestParts_.clear();
    chestBuilt_ = false;
  }

public:
  /// @brief HP・ポイント・演出パラメータをインスペクタで編集できるようにする
  void OnImGui() override {
#if RC_ENABLE_IMGUI
    ImGui::DragInt("HP##TreasureChest", &hp, 1, 0, maxHp);
    ImGui::DragInt("Max HP##TreasureChest", &maxHp, 1, 1, 100);
    ImGui::DragInt("Points##TreasureChest", &points, 10, 0, 100000);
    ImGui::DragFloat("Bob Amplitude##TreasureChest", &bobAmplitude, 0.01f, 0.0f, 5.0f);
    ImGui::DragFloat("Bob Speed##TreasureChest", &bobSpeed, 0.1f, 0.0f, 20.0f);
    ImGui::DragFloat("Spin Speed (deg/s)##TreasureChest", &spinSpeedDeg, 1.0f, -720.0f, 720.0f);
    ImGui::DragFloat("Open Duration##TreasureChest", &openDuration, 0.05f, 0.1f, 10.0f);
    ImGui::DragFloat("Pop Height##TreasureChest", &popHeight, 0.05f, 0.0f, 10.0f);
    ImGui::Text("Opened: %s", isOpened ? "yes" : "no");
#endif
  }

  /// @brief ダメージを受けて HP を減らし、被弾フラッシュを出す。HP が 0 以下になったら開く
  /// @param damage 受けるダメージ量
  /// @note 開いたあとは何もしない
  void TakeDamage(int damage) {
    if (isOpened) return;
    hp -= damage;
    Log::Print(std::format("[TreasureChest] {} took {} damage. HP {}/{}", GetEntity()->GetName(), damage,
                           hp, maxHp));

    // 敵と同じく白っぽく光らせて手応えを出す
    SaveOriginalColor();
    ApplyColor({1.0f, 1.0f, 0.6f, 1.0f});
    flashTimer_ = 0.15f;

    if (hp <= 0) {
      hp = 0;
      Open();
    }
  }

private:
  RC::Vector3 basePosition_ = {0.0f, 0.0f, 0.0f};
  RC::Vector3 baseScale_ = {1.0f, 1.0f, 1.0f};
  RC::Vector3 baseRotation_ = {0.0f, 0.0f, 0.0f};
  bool hasBase_ = false;
  float bobTime_ = 0.0f;
  float openTimer_ = 0.0f;
  float flashTimer_ = 0.0f;
  RC::Vector4 originalColor_ = {1.0f, 1.0f, 1.0f, 1.0f};
  bool hasSavedColor_ = false;

  /// @brief 開く。ポイントを加算し、以後は弾が当たらないようにする
  void Open() {
    if (isOpened) return;
    isOpened = true;
    openTimer_ = 0.0f;
    RestoreColor();

    Entity* self = GetEntity();
    if (!self) return;

    // 以後は弾が当たらないように
    self->ClearTag("is_item");
    self->ClearTag("current_hp");
    self->ClearTag("max_hp");
    if (auto* col = self->GetComponent<ColliderComponent>()) col->SetEnabled(false);

    // スコア加算: プレイヤー（RailShooterController が載っている Entity）の score_add へ積む
    AddScore(points);

    // リザルト用の取得数
    GameSession::Get().AddChestCollected();

    Log::Print(std::format("[TreasureChest] {} opened! +{} points", self->GetName(), points));

    // 開いた瞬間の泡エフェクト（敵撃破と同じ GPU 泡）
    SpawnBubbles();
  }

  /// @brief プレイヤーの score_add タグへスコアを積む
  /// @param amount 加算する点数（0 以下なら何もしない）
  /// @details 積み先は is_player タグを持つエンティティ。見つからなければカメラを持つエンティティで代用する。
  void AddScore(int amount) {
    if (amount <= 0) return;
    Scene* scene = GetScene();
    if (!scene) return;

    // 前回見つけた積み先がまだ有効ならそれを使い、無効ならシーンから探し直す
    std::shared_ptr<Entity> target = cachedPlayer_.lock();
    if (!target || target->IsPendingDestroy() || !target->IsActive()) {
      target = nullptr;
      std::shared_ptr<Entity> cameraFallback = nullptr;
      for (auto& e : scene->GetEntities()) {
        if (!e || !e->IsActive() || e->IsPendingDestroy()) continue;
        if (e->GetTagInt("is_player", 0) == 1) {
          target = e;
          break;
        }
        if (!cameraFallback && e->HasComponent<CameraComponent>()) cameraFallback = e;
      }
      if (!target) target = cameraFallback;
      cachedPlayer_ = target;
    }
    if (target) {
      int scoreAdd = target->GetTagInt("score_add", 0);
      target->SetTag("score_add", scoreAdd + amount);
    }
  }
  std::weak_ptr<Entity> cachedPlayer_;

  /// @brief 宝箱の位置に GPU 泡エフェクトのエミッターを生成する
  void SpawnBubbles() {
    Scene* scene = GetScene();
    Entity* self = GetEntity();
    if (!scene || !self) return;

    auto emitter = scene->CreateEntity("GPUBubbleEmitter");
    auto* tr = &emitter->AddComponent<TransformComponent>();
    if (auto* myTr = self->GetComponent<TransformComponent>()) tr->position = myTr->position;
    auto* gpu = &emitter->AddComponent<GPUParticleComponent>();
    if (gpu->particleSystem) gpu->particleSystem->SetPipelinePrefix("gpu_particle_bubble");
    auto* nsc = &emitter->AddComponent<NativeScriptComponent>();
    nsc->AddScript("GPUBubbleEmitter");
    nsc->SetScene(scene);
    if (GetSceneContext()) nsc->SetSceneContext(GetSceneContext());
    scene->InitDynamicEntityRuntime(*emitter);
  }

  // --- 色の保存／適用（EnemyBaseScript と同じ方式）---
  /// @brief 被弾フラッシュで色を変える前に、本体の元の色を一度だけ保存する
  void SaveOriginalColor() {
    if (hasSavedColor_) return;
    Entity* self = GetEntity();
    if (!self) return;
    if (auto* mr = self->GetComponent<ModelRendererComponent>()) {
      originalColor_ = mr->color;
      hasSavedColor_ = true;
    } else if (auto* pm = self->GetComponent<PrimitiveMeshComponent>()) {
      if (pm->meshHandle >= 0) {
        if (auto* mat = RC::GetPrimitiveMeshMaterialPtr(pm->meshHandle)) {
          originalColor_ = mat->color;
          hasSavedColor_ = true;
        }
      }
    }
  }

  /// @brief 本体と宝箱の部位に色を適用する
  /// @param color 適用する色
  /// @param restoreParts true なら部位には color ではなく各部位の元の色を戻す
  void ApplyColor(const RC::Vector4& color, bool restoreParts = false) {
    // 宝箱の部位も同じ色で点滅させる（戻すときは部位それぞれの色へ）
    for (auto& p : chestParts_) {
      auto e = p.entity.lock();
      if (!e) continue;
      auto* ppm = e->GetComponent<PrimitiveMeshComponent>();
      if (!ppm || ppm->meshHandle < 0) continue;
      if (auto* mat = RC::GetPrimitiveMeshMaterialPtr(ppm->meshHandle)) mat->color = restoreParts ? p.color : color;
    }
    Entity* self = GetEntity();
    if (!self) return;
    if (auto* mr = self->GetComponent<ModelRendererComponent>()) {
      mr->color = color;
    } else if (auto* pm = self->GetComponent<PrimitiveMeshComponent>()) {
      if (pm->meshHandle >= 0) {
        if (auto* mat = RC::GetPrimitiveMeshMaterialPtr(pm->meshHandle)) mat->color = color;
      }
    }
  }

  /// @brief 保存しておいた元の色へ戻す（部位もそれぞれの元の色へ戻す）
  void RestoreColor() {
    if (hasSavedColor_) ApplyColor(originalColor_, /*restoreParts=*/true);
  }

  // --- 宝箱の部位 ---
  /// @brief 宝箱の部位の分類（Base: 下箱と一緒に動く / Lid: ふたと一緒に開く / Treasure: 開くとせり上がる）
  enum class ChestGroup { Base, Lid, Treasure };
  /// @brief 宝箱の部位 1 つぶんの情報（本体ローカルでの配置・分類・元の色）
  struct ChestPart {
    std::weak_ptr<Entity> entity;
    RC::Vector3 offset{};                      ///< 本体中心からの位置（m、本体ローカル、縮小前）
    RC::Vector3 scale{};                       ///< Transform の scale（m、縮小前）
    PartPose::Mat3 local = PartPose::Identity(); ///< 本体に対する部位の向き
    RC::Vector4 color{1, 1, 1, 1};
    ChestGroup group = ChestGroup::Base;
  };
  std::vector<ChestPart> chestParts_;
  bool chestBuilt_ = false;
  float chestTopY_ = 0.5f;   ///< 下箱の上面の高さ（本体中心から、m）
  float chestDepth_ = 1.0f;  ///< 奥行き（m）。ちょうつがいの位置に使う

  /// @brief 宝箱の部位エンティティを 1 つ生成して chestParts_ に登録する
  /// @param type 部位のプリミティブ形状
  /// @param color 部位の色（被弾色から戻すときの色にもなる）
  /// @param offset 本体中心からの位置（m、本体ローカル、縮小前）
  /// @param scale 部位の Transform の scale（m、縮小前）
  /// @param group 部位の分類（ふたの開閉・財宝のせり上がりの対象を決める）
  /// @param local 本体に対する部位の向き
  /// @param unlit true ならライティングせず、自分で光っているように見せる
  void AddChestPart(PrimitiveType type, const RC::Vector4& color, const RC::Vector3& offset,
                    const RC::Vector3& scale, ChestGroup group,
                    const PartPose::Mat3& local = PartPose::Identity(), bool unlit = false) {
    Scene* scene = GetScene();
    Entity* self = GetEntity();
    if (!scene || !self) return;
    auto e = scene->CreateEntity(self->GetName() + "_part");
    if (!e) return;
    e->SetParentGuid(self->Guid()); // Hierarchy で宝箱の下にまとまるように（描画上の親子ではない）
    // 実行時だけの部位なのでシーン保存に含めない
    e->SetTag("transient", 1);
    // PlaceChest で配置されるまで大きく映らないよう、本体の位置に極小スケールで仮置きする
    auto& ptr = e->AddComponent<TransformComponent>();
    if (auto* tr = self->GetComponent<TransformComponent>()) ptr.position = tr->position;
    ptr.scale = {0.01f, 0.01f, 0.01f};
    auto& pm = e->AddComponent<PrimitiveMeshComponent>();
    pm.type = type;
    pm.color = color;
    // 下箱・ふたの金色の部位（金具）だけ光沢を強くする
    pm.shininess = (group == ChestGroup::Base || group == ChestGroup::Lid) && color.x > 0.8f ? 64.0f : 12.0f;
    if (unlit) pm.lightingMode = 0; // 財宝は自分で光って見えるようにライティングしない
    scene->InitDynamicEntityRuntime(*e);
    ChestPart p;
    p.entity = e;
    p.offset = offset;
    p.scale = scale;
    p.local = local;
    p.color = color;
    p.group = group;
    chestParts_.push_back(p);
  }

  /// @brief 本体の箱を下箱にして、ふた・金具・錠前・財宝を付ける
  /// @details 大きさは本体の scale（幅 W・高さ・奥行き D、m）から決める。+X が横幅、-Z が正面（錠前側）。
  ///          下箱は本体の高さの baseHeightRatio 倍に縮め、その上にふた（高さ ≒ 0.4m）を載せる。
  ///          本体の当たり判定は半径 × scale の最大値（横幅）なので、高さを縮めても大きさは変わらない。
  /// @param tr 本体の Transform（高さを baseHeightRatio 倍に縮める）
  void BuildChest(TransformComponent& tr) {
    using namespace PartPose;
    chestBuilt_ = true;
    const float ratio = std::clamp(baseHeightRatio, 0.2f, 1.0f);
    tr.scale.y *= ratio;
    const float W = (std::max)(std::fabs(tr.scale.x), 0.2f);
    const float Hb = (std::max)(std::fabs(tr.scale.y), 0.1f);
    const float D = (std::max)(std::fabs(tr.scale.z), 0.2f);
    const float yT = Hb * 0.5f;
    chestTopY_ = yT;
    chestDepth_ = D;
    const float front = -D * 0.5f; // 錠前側
    const RC::Vector4 dark = {0.08f, 0.06f, 0.04f, 1.0f};
    const Mat3 lying = RotZ(kHalfPi); // 円柱の軸（Y）を横（X）へ寝かせる。ローカル X が上下の半径になる
    const float bandX = W * 0.28f;
    const float lidH = 0.2f;   // ふたの箱の部分の高さ
    const float domeH = 0.2f;  // かまぼこの高さ（下半分はふたの箱に隠れる）

    // --- 下箱の金具 ---
    AddChestPart(PrimitiveType::Box, metalColor, {0, yT - 0.035f, 0}, {W + 0.06f, 0.07f, D + 0.06f}, ChestGroup::Base);
    AddChestPart(PrimitiveType::Box, metalColor, {0, -yT + 0.03f, 0}, {W + 0.06f, 0.06f, D + 0.06f}, ChestGroup::Base);
    for (float sx : {1.0f, -1.0f}) {
      for (float sz : {1.0f, -1.0f}) {
        AddChestPart(PrimitiveType::Box, metalColor, {sx * W * 0.5f, 0, sz * D * 0.5f}, {0.09f, Hb + 0.02f, 0.09f},
                     ChestGroup::Base);
      }
      AddChestPart(PrimitiveType::Box, metalColor, {sx * bandX, 0, 0}, {0.1f, Hb + 0.02f, D + 0.04f}, ChestGroup::Base);
    }
    // 錠前（正面の金の板＋鍵穴）
    AddChestPart(PrimitiveType::Box, metalColor, {0, yT - 0.14f, front - 0.025f}, {0.28f, 0.3f, 0.05f}, ChestGroup::Base);
    AddChestPart(PrimitiveType::Box, dark, {0, yT - 0.16f, front - 0.055f}, {0.05f, 0.11f, 0.02f}, ChestGroup::Base);

    // --- ふた（箱＋かまぼこ。ちょうつがいを軸に開く）---
    AddChestPart(PrimitiveType::Box, lidColor, {0, yT + lidH * 0.5f, 0}, {W, lidH, D}, ChestGroup::Lid);
    AddChestPart(PrimitiveType::Cylinder, lidColor, {0, yT + lidH, 0}, {domeH, W * 0.995f, D * 0.5f * 0.995f},
                 ChestGroup::Lid, lying);
    AddChestPart(PrimitiveType::Box, metalColor, {0, yT + 0.03f, 0}, {W + 0.06f, 0.06f, D + 0.06f}, ChestGroup::Lid);
    for (float sx : {1.0f, -1.0f}) {
      AddChestPart(PrimitiveType::Box, metalColor, {sx * bandX, yT + lidH * 0.5f, 0}, {0.1f, lidH + 0.02f, D + 0.04f},
                   ChestGroup::Lid);
      AddChestPart(PrimitiveType::Cylinder, metalColor, {sx * bandX, yT + lidH, 0},
                   {domeH + 0.02f, 0.1f, D * 0.5f + 0.02f}, ChestGroup::Lid, lying);
    }
    // 掛け金（ふたから錠前へ垂れる）
    AddChestPart(PrimitiveType::Box, metalColor, {0, yT + 0.02f, front - 0.035f}, {0.15f, 0.2f, 0.04f}, ChestGroup::Lid);

    // --- 中の財宝（閉じている間は下箱とふたの中に隠れている）---
    AddChestPart(PrimitiveType::Box, treasureColor, {0, yT - 0.04f, 0}, {W * 0.9f, 0.04f, D * 0.9f},
                 ChestGroup::Treasure, Identity(), true);
    const RC::Vector3 piles[3] = {{-W * 0.22f, yT - 0.05f, -D * 0.05f}, {W * 0.02f, yT - 0.02f, D * 0.08f},
                                  {W * 0.22f, yT - 0.06f, -D * 0.02f}};
    for (const auto& pp : piles) {
      AddChestPart(PrimitiveType::Sphere, treasureColor, pp, {W * 0.17f, 0.1f, D * 0.2f}, ChestGroup::Treasure,
                   Identity(), true);
    }

    PlaceChest(tr, 0.0f); // 生成直後に原点で 1 フレーム映らないように
    Log::Print(std::format("[TreasureChest] {} built {} parts", GetEntity() ? GetEntity()->GetName() : "", chestParts_.size()));
  }

  /// @brief 部位を本体の位置・回転・大きさに合わせる
  /// @param tr 本体の Transform（位置・回転・大きさの基準）
  /// @param lidT ふたの開き具合（0: 閉じている / 1: lidOpenDeg まで開いた）
  void PlaceChest(const TransformComponent& tr, float lidT) {
    using namespace PartPose;
    if (chestParts_.empty()) return;
    // 開く演出で本体が縮むので、その倍率を部位にも掛ける
    const float k = (hasBase_ && std::fabs(baseScale_.x) > 1e-4f) ? tr.scale.x / baseScale_.x : 1.0f;
    const Mat3 body = FromEngineEuler(tr.rotation);
    // ちょうつがい（上面の奥の辺）まわりに回す。正の角で正面側が持ち上がる
    const Mat3 open = RotX(lidOpenDeg * kDeg * lidT);
    const RC::Vector3 hinge = {0.0f, chestTopY_, chestDepth_ * 0.5f};
    for (const auto& p : chestParts_) {
      auto e = p.entity.lock();
      if (!e) continue;
      auto* ptr = e->GetComponent<TransformComponent>();
      if (!ptr) continue;
      RC::Vector3 off = p.offset;
      Mat3 loc = p.local;
      if (p.group == ChestGroup::Lid) {
        off = AddV(hinge, Apply({off.x - hinge.x, off.y - hinge.y, off.z - hinge.z}, open));
        loc = MatMul(loc, open);
      } else if (p.group == ChestGroup::Treasure) {
        off.y += 0.12f * lidT; // ふたが開くと財宝がせり上がる
      }
      ptr->position = AddV(tr.position, Apply(ScaleV(off, k), body));
      ptr->rotation = ToEngineEuler(MatMul(loc, body));
      ptr->scale = ScaleV(p.scale, k);
      // PrimitiveMesh は次フレーム冒頭まで同期されないので、本体と 1 フレームずれないよう即座に写す
      if (auto* pm = e->GetComponent<PrimitiveMeshComponent>()) {
        if (pm->meshHandle >= 0) {
          if (auto* t = RC::GetPrimitiveMeshTransformPtr(pm->meshHandle)) *t = ptr->ToTransform();
        }
      }
    }
  }
};

REGISTER_SCRIPT(TreasureChestScript)
