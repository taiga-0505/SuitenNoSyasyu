#include "ECS/ScriptableEntity.h"
#include "ECS/ScriptRegistry.h"
#include "ECS/TransformComponent.h"
#include "ECS/ModelRendererComponent.h"
#include "ECS/PrimitiveMeshComponent.h"
#include "ECS/ColliderComponent.h"
#include "ECS/NativeScriptComponent.h"
#include "ECS/CameraComponent.h"
#include "Scene.h"
#include "RenderCommon.h"
#include "Common/Log/Log.h"

#if RC_ENABLE_IMGUI
#include "imgui/imgui.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

// =====================================================================
// A-03: ウェーブの敵スポナー
// =====================================================================
// 空エンティティに 1 つ付けて、シーン上の湧かせたい場所へ置く。
// 自分の TransformComponent がそのまま出現地点になるので、エディタで
// ギズモを見ながら配置できる（JSON に座標を手打ちしなくてよい）。
//
// WaveManager とは Entity のタグだけでやりとりする:
//   読む: WaveManager の wave_active が自分の waveId と一致したら湧かせる
//   書く: 自分に wave_spawner / spawner_wave_id / spawn_done
//         生成した敵に wave_id（WaveManager がこれで生存数を数える）
//
// 敵スクリプトへのパラメータは NativeScriptComponent の pendingData 経由で渡す。
// スクリプト実体は次の Update まで作られないので、生成直後にメンバへ代入する
// ことはできない。pendingData に載せておけば通常の Deserialize と同じ経路を通る。

/// @brief ウェーブ開始時に敵を動的生成するスクリプト
class WaveSpawnerScript : public ScriptableEntity {
public:
  // --- インスペクタ / JSON から設定する項目 ---
  /// @brief 担当するウェーブ id
  int waveId = 1;
  /// @brief 進行状況を見に行く WaveManager のエンティティ名
  std::string waveManagerName = "WaveManager";
  /// @brief 生成する敵に付けるスクリプト名（REGISTER_SCRIPT の登録名）
  std::string enemyScript = "SharkEnemyScript";
  /// @brief 生成する敵のエンティティ名の接頭辞
  std::string enemyName = "WaveEnemy";
  /// @brief 敵のモデル。空ならデバッグ用の球で代用する
  /// @details 既定はゲーム用の軽量サメ（shark.obj は 11MB・11 万三角形で、出現のたびに読むと重い）
  std::string modelPath = "Resources/model/shark/shark_game.glb";
  /// @brief 生成する体数
  int count = 3;
  /// @brief この地点を中心にどれだけ散らすか（0 なら全員同じ座標）
  float spreadRadius = 6.0f;
  /// @brief 1 体ごとの出現間隔（秒）
  float spawnInterval = 0.35f;
  /// @brief ウェーブ開始から 1 体目までの待ち（秒）
  float startDelay = 0.0f;
  /// @brief 生成する敵のスケール
  RC::Vector3 enemyScale = {4.0f, 4.0f, 4.0f};
  /// @brief 生成する敵のコライダー半径（ローカル単位）
  /// @details 実際の当たり半径は colliderRadius × enemyScale の最大成分になる
  ///          （WaterBullet などが Transform のスケールを掛けるため）。
  ///          例: サメ 0.5 × 4 = 2m、船 0.4 × 9 = 3.6m。
  ///          ワールドの m のつもりで大きな値を入れると、船（z スケール 9）では 3.0 で 27m になり、
  ///          自機が球の中に入って「撃った瞬間に当たる」状態になるので注意。
  float colliderRadius = 0.5f;
  /// @brief 敵スクリプトへ渡すパラメータ（"maxHp" などをここに書く）
  nlohmann::json enemyParams;
  /// @brief 複数スクリプトを載せたいときの指定
  /// @details `[{"name": "ShipEnemyScript", "params": {...}}, {"name": "BuoyancyScript", ...}]`
  ///          という配列。船（T-15）は AI と浮力の 2 本を必要とするため用意した。
  ///          省略した場合は enemyScript / enemyParams の 1 本だけを載せる。
  nlohmann::json scripts;
  /// @brief modelPath が空のときに出すプリミティブ形状
  /// @details sphere / box / plane / cylinder / cone / torus / capsule
  std::string primitiveShape = "sphere";
  /// @brief プリミティブで代用するときの色
  RC::Vector4 primitiveColor = {0.9f, 0.3f, 0.3f, 1.0f};
  /// @brief 配置確認用のギズモを出すか
  bool drawGizmo = true;
  /// @brief 出現地点を「スポナーの位置」ではなく「自機の前方遠く」にする
  /// @details ウェーブ開始時の自機（カメラ）位置から前方 aheadDistance の地点を
  ///          中心に spreadRadius で散らす。スポナーを自機の近くに置いていると
  ///          湧いた瞬間に攻撃圏内へ入って一斉に突進してくるため、その対策。
  ///          高さ（y）はスポナー自身の y を使う（＝水面の高さとして置いておく）。
  bool spawnAheadOfPlayer = false;
  /// @brief spawnAheadOfPlayer 時、自機の前方どれだけ先に出すか（m）
  float aheadDistance = 50.0f;
  /// @brief spawnAheadOfPlayer 時、1 体ごとに左右交互へずらす幅（m）
  /// @details 同じ方向から縦一列に来ると連続で当たるので、左右に振り分ける。
  float aheadSideStep = 8.0f;

  /// @brief スポナーの設定を JSON に書き出す
  /// @return 設定項目を格納した JSON（enemyParams・scripts は指定がある場合のみ含める）
  nlohmann::json Serialize() override {
    nlohmann::json j;
    j["waveId"] = waveId;
    j["waveManagerName"] = waveManagerName;
    j["enemyScript"] = enemyScript;
    j["enemyName"] = enemyName;
    j["modelPath"] = modelPath;
    j["count"] = count;
    j["spreadRadius"] = spreadRadius;
    j["spawnInterval"] = spawnInterval;
    j["startDelay"] = startDelay;
    j["enemyScale"] = {enemyScale.x, enemyScale.y, enemyScale.z};
    j["colliderRadius"] = colliderRadius;
    j["drawGizmo"] = drawGizmo;
    j["primitiveShape"] = primitiveShape;
    j["primitiveColor"] = {primitiveColor.x, primitiveColor.y, primitiveColor.z, primitiveColor.w};
    if (!enemyParams.is_null()) j["enemyParams"] = enemyParams;
    if (scripts.is_array() && !scripts.empty()) j["scripts"] = scripts;
    j["spawnAheadOfPlayer"] = spawnAheadOfPlayer;
    j["aheadDistance"] = aheadDistance;
    j["aheadSideStep"] = aheadSideStep;
    return j;
  }

  /// @brief JSON からスポナーの設定を読み込む
  /// @param j 読み込み元の JSON（存在するキーだけ反映する）
  void Deserialize(const nlohmann::json &j) override {
    if (j.contains("waveId")) waveId = j["waveId"].get<int>();
    if (j.contains("waveManagerName")) waveManagerName = j["waveManagerName"].get<std::string>();
    if (j.contains("enemyScript")) enemyScript = j["enemyScript"].get<std::string>();
    if (j.contains("enemyName")) enemyName = j["enemyName"].get<std::string>();
    if (j.contains("modelPath")) modelPath = j["modelPath"].get<std::string>();
    if (j.contains("count")) count = j["count"].get<int>();
    if (j.contains("spreadRadius")) spreadRadius = j["spreadRadius"].get<float>();
    if (j.contains("spawnInterval")) spawnInterval = j["spawnInterval"].get<float>();
    if (j.contains("startDelay")) startDelay = j["startDelay"].get<float>();
    if (j.contains("colliderRadius")) colliderRadius = j["colliderRadius"].get<float>();
    if (j.contains("drawGizmo")) drawGizmo = j["drawGizmo"].get<bool>();
    if (j.contains("enemyScale") && j["enemyScale"].is_array() && j["enemyScale"].size() >= 3) {
      enemyScale = {j["enemyScale"][0].get<float>(), j["enemyScale"][1].get<float>(),
                    j["enemyScale"][2].get<float>()};
    }
    if (j.contains("enemyParams")) enemyParams = j["enemyParams"];
    if (j.contains("scripts")) scripts = j["scripts"];
    if (j.contains("spawnAheadOfPlayer")) spawnAheadOfPlayer = j["spawnAheadOfPlayer"].get<bool>();
    if (j.contains("aheadDistance")) aheadDistance = j["aheadDistance"].get<float>();
    if (j.contains("aheadSideStep")) aheadSideStep = j["aheadSideStep"].get<float>();
    if (j.contains("primitiveShape")) primitiveShape = j["primitiveShape"].get<std::string>();
    if (j.contains("primitiveColor") && j["primitiveColor"].is_array() &&
        j["primitiveColor"].size() >= 4) {
      const auto &c = j["primitiveColor"];
      primitiveColor = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>(),
                        c[3].get<float>()};
    }
  }

protected:
  /// @brief WaveManager に見つけてもらうための目印タグを立て、湧かせ状態を初期化する
  void OnCreate() override {
    if (Entity *self = GetEntity()) {
      // WaveManager は型を知らずにスポナーを探すので、目印をタグで出しておく
      self->SetTag("wave_spawner", 1);
      self->SetTag("spawner_wave_id", waveId);
      // ランタイム専用タグの消し込み。タグは Entity::Serialize で JSON へ往復するため、
      // プレイ中に保存されたシーンでは spawn_done が焼き付いていることがある。
      // 残っていると「もう湧かせ済み」と誤判定して敵が 1 体も出ない。
      self->ClearTag("spawn_done");
    }
    spawningWave_ = 0;
    spawnedCount_ = 0;
    elapsed_ = 0.0f;
  }

  /// @brief WaveManager の wave_active を監視し、担当ウェーブが始まったら一定間隔で敵を生成する
  /// @param deltaTime 前フレームからの経過時間（秒）
  void OnUpdate(float deltaTime) override {
    Entity *self = GetEntity();
    if (!self) return;

    // waveId をインスペクタで変えたときに追従させる
    if (self->GetTagInt("spawner_wave_id", 0) != waveId) {
      self->SetTag("spawner_wave_id", waveId);
    }

    // 進行状況を見る WaveManager を名前で探す
    Scene *scene = GetScene();
    if (!scene) return;
    Entity *manager = waveManagerName.empty() ? nullptr
                                              : scene->FindEntityByName(waveManagerName);
    if (!manager) return;

    const int active = manager->GetTagInt("wave_active", 0);

    if (spawningWave_ == 0) {
      // 待機中。募集がかかっていて、まだこのウェーブを湧かせていなければ開始する。
      if (active == waveId && self->GetTagInt("spawn_done", 0) != waveId) {
        spawningWave_ = waveId;
        spawnedCount_ = 0;
        elapsed_ = 0.0f;
      } else if (active == 0 && self->GetTagInt("spawn_done", 0) != 0) {
        // ウェーブが終わったら次の周回に備えて済み記録を落とす
        self->ClearTag("spawn_done");
      }
      return;
    }

    // 湧かせ中。募集が下りたら（強制クリアなど）打ち切る。
    if (active != spawningWave_) {
      spawningWave_ = 0;
      return;
    }

    elapsed_ += deltaTime;
    // 1 フレームに複数体ぶんの時間が経つこともあるので while で追いつかせる
    while (spawnedCount_ < count) {
      const float due = startDelay + spawnInterval * static_cast<float>(spawnedCount_);
      if (elapsed_ < due) break;
      SpawnOne(scene, spawnedCount_);
      ++spawnedCount_;
    }

    // 全員出し終えたら済み記録を立てて待機へ戻る（WaveManager はこのタグで湧き終わりを判定する）
    if (spawnedCount_ >= count) {
      self->SetTag("spawn_done", spawningWave_);
      spawningWave_ = 0;
    }
  }

public:
  /// @brief 出現地点・散らばり範囲・各敵の出現座標をギズモで描く
  /// @note spawnAheadOfPlayer 時の実際の出現位置（自機前方）は描かず、スポナー位置基準で描く
  void OnDebugRender() override {
    if (!drawGizmo) return;
    auto *tr = GetComponent<TransformComponent>();
    if (!tr) return;

    const RC::Vector4 color = {1.0f, 0.35f, 0.1f, 1.0f};
    RC::DrawWireSphere3D(tr->position, 0.7f, color, 12, 12, true);
    if (spreadRadius > 0.0f) {
      RC::DrawWireSphere3D(tr->position, spreadRadius, color, 20, 4, true);
    }
    // 実際の出現座標を並べて出す。散らばり方をエディタ上で確認できるようにするため。
    for (int i = 0; i < count; ++i) {
      RC::DrawWireSphere3D(SpawnPosition(tr->position, i), 0.4f, color, 8, 8, true);
    }
  }

#if RC_ENABLE_IMGUI
  /// @brief スポナーの設定編集 UI と、湧かせ状況・載せるスクリプトの一覧を表示する
  void OnImGui() override {
    ImGui::DragInt("Wave Id", &waveId, 1.0f, 1, 99);
    ImGui::DragInt("Count", &count, 1.0f, 1, 32);
    ImGui::DragFloat("Spread Radius", &spreadRadius, 0.1f, 0.0f, 100.0f);
    ImGui::DragFloat("Spawn Interval", &spawnInterval, 0.05f, 0.0f, 10.0f);
    ImGui::DragFloat("Start Delay", &startDelay, 0.05f, 0.0f, 20.0f);
    ImGui::DragFloat3("Enemy Scale", &enemyScale.x, 0.1f);
    ImGui::DragFloat("Collider Radius", &colliderRadius, 0.05f, 0.05f, 20.0f);
    ImGui::Checkbox("Draw Gizmo", &drawGizmo);
    ImGui::Checkbox("Spawn Ahead Of Player", &spawnAheadOfPlayer);
    if (spawnAheadOfPlayer) {
      ImGui::DragFloat("Ahead Distance", &aheadDistance, 0.5f, 0.0f, 200.0f);
      ImGui::DragFloat("Ahead Side Step", &aheadSideStep, 0.1f, 0.0f, 50.0f);
    }

    DrawTextField("Wave Manager", waveManagerName);
    DrawTextField("Enemy Script", enemyScript);
    DrawTextField("Enemy Name", enemyName);
    DrawTextField("Model Path", modelPath);
    if (modelPath.empty()) {
      DrawTextField("Primitive Shape", primitiveShape);
      ImGui::ColorEdit4("Primitive Color", &primitiveColor.x);
    }

    // ここから下は現在の状態の表示
    ImGui::Separator();
    if (scripts.is_array() && !scripts.empty()) {
      ImGui::Text("Scripts (%zu):", scripts.size());
      for (const auto &s : scripts) {
        if (s.contains("name")) {
          ImGui::BulletText("%s", s["name"].get<std::string>().c_str());
        }
      }
    }
    ImGui::Text("Spawning: %s", spawningWave_ != 0 ? "yes" : "no");
    ImGui::Text("Spawned: %d / %d", spawnedCount_, count);
    if (Entity *self = GetEntity()) {
      ImGui::Text("spawn_done: %d", self->GetTagInt("spawn_done", 0));
    }
    if (!enemyParams.is_null()) {
      ImGui::TextUnformatted(enemyParams.dump().c_str());
    }
  }
#endif

private:
#if RC_ENABLE_IMGUI
  /// @brief std::string を編集する ImGui のテキスト入力欄を出す
  /// @param label 入力欄のラベル
  /// @param value 編集対象の文字列（入力があれば書き換える）
  /// @note バッファが 128 バイトなので、それを超える文字列は切り詰められる
  static void DrawTextField(const char *label, std::string &value) {
    char buf[128] = {};
    std::snprintf(buf, sizeof(buf), "%s", value.c_str());
    if (ImGui::InputText(label, buf, sizeof(buf))) value = buf;
  }
#endif

  /// @brief index 番目の敵をどこに出すか
  /// @details 乱数ではなく黄金角のらせん（ひまわり配置）で決めている。
  ///          乱数だとプレイのたびに配置が変わって、難易度の調整結果が
  ///          自分の変更のせいなのか運のせいなのか切り分けられなくなるため。
  ///          円の内側と外側で密度が偏らないよう、半径は sqrt で取っている。
  /// @param base 散らばりの中心座標
  /// @param index 何体目か（0 始まり）
  /// @return 出現座標（spreadRadius が 0 以下か count が 1 以下なら base そのもの）
  RC::Vector3 SpawnPosition(const RC::Vector3 &base, int index) const {
    if (spreadRadius <= 0.0f || count <= 1) return base;

    const float kGoldenAngle = 2.39996323f; // ラジアン
    const float angle = kGoldenAngle * static_cast<float>(index);
    const float radius =
        spreadRadius * std::sqrt((static_cast<float>(index) + 0.5f) / static_cast<float>(count));

    RC::Vector3 pos = base;
    pos.x += std::cos(angle) * radius;
    pos.z += std::sin(angle) * radius;
    return pos;
  }

  /// @brief 自機（カメラ）の前方遠くの出現中心を求める
  /// @details 1 体ごとに現在のカメラ位置から計算するので、レール移動中でも
  ///          「いつも前方 aheadDistance 先」から湧く。左右は index で交互に振る
  ///          （0:左 1:右 2:さらに左 …）。カメラが見つからなければ fallback を返す。
  ///          前方ベクトルの取り方は SharkEnemyScript と揃えている。
  /// @param scene カメラを探すシーン
  /// @param fallback カメラが無いときに返す座標。高さ（y）は常にこの値を使う
  /// @param index 何体目か（左右の振り分けに使う）
  /// @return 出現中心の座標
  RC::Vector3 AheadOfPlayerPosition(Scene *scene, const RC::Vector3 &fallback, int index) const {
    const TransformComponent *camTr = nullptr;
    for (auto &e : scene->GetEntities()) {
      if (e && e->HasComponent<CameraComponent>()) {
        camTr = e->GetComponent<TransformComponent>();
        break;
      }
    }
    if (!camTr) return fallback;

    const float sy = std::sin(camTr->rotation.y);
    const float cy = std::cos(camTr->rotation.y);
    const RC::Vector3 fwd = {sy, 0.0f, cy};
    const RC::Vector3 right = {cy, 0.0f, -sy};

    // 偶数番は左、奇数番は右へ。2 体ごとに aheadSideStep ずつ外側へ広げる
    const float sign = (index % 2 == 0) ? -1.0f : 1.0f;
    const float side = sign * aheadSideStep * static_cast<float>(index / 2 + 1);

    RC::Vector3 pos = fallback;
    pos.x = camTr->position.x + fwd.x * aheadDistance + right.x * side;
    pos.y = fallback.y; // 高さはスポナーの y（水面）を使う
    pos.z = camTr->position.z + fwd.z * aheadDistance + right.z * side;
    return pos;
  }

  /// @brief 敵を 1 体生成する
  /// @param scene 生成先のシーン
  /// @param index 何体目か（エンティティ名と出現位置に使う）
  void SpawnOne(Scene *scene, int index) {
    auto *selfTr = GetComponent<TransformComponent>();
    RC::Vector3 base = selfTr ? selfTr->position : RC::Vector3{0.0f, 0.0f, 0.0f};
    if (spawnAheadOfPlayer) {
      base = AheadOfPlayerPosition(scene, base, index);
    }

    const std::string name =
        enemyName + "_w" + std::to_string(waveId) + "_" + std::to_string(index);
    auto entity = scene->CreateEntity(name);
    if (!entity) return;

    // 位置は散らばりを加えた座標、向きはスポナーの向きを引き継ぐ
    auto &tr = entity->AddComponent<TransformComponent>();
    tr.position = SpawnPosition(base, index);
    tr.scale = enemyScale;
    if (selfTr) tr.rotation = selfTr->rotation;

    // モデルが指定されていればそれを、無ければデバッグ用の球を出す。
    // 素材が揃う前でもウェーブの流れだけは確認できるようにしておきたいため。
    const bool usePrimitive = modelPath.empty();
    if (!usePrimitive) {
      auto &mr = entity->AddComponent<ModelRendererComponent>();
      mr.modelPath = modelPath;
    } else {
      auto &pm = entity->AddComponent<PrimitiveMeshComponent>();
      pm.type = ShapeFromString(primitiveShape);
      pm.color = primitiveColor;
      // ここでは meshHandle を作らない。InitDynamicEntityRuntime が
      // 形状に応じて meshHandle を作り直し、pm.color もマテリアルへ反映するため、
      // 先に自分で生成するとそのハンドルが誰にも参照されないまま残る。
      // Transform の同期だけ Init のあとで行う。
    }

    // 弾との当たり判定用の球コライダー
    auto &col = entity->AddComponent<ColliderComponent>();
    col.shape = ColliderComponent::Shape::Sphere;
    col.radius = colliderRadius;
    col.isTrigger = false;
    {
      // 当たり半径が大きすぎる設定を知らせる（ローカル単位の取り違え対策）
      const float worldR = colliderRadius * (std::max)((std::max)(std::abs(enemyScale.x),
                                                                 std::abs(enemyScale.y)),
                                                       std::abs(enemyScale.z));
      if (worldR > 8.0f) {
        Log::Print("[WaveSpawnerScript] warning: collider radius is " + std::to_string(worldR) +
                   "m in world (colliderRadius x max(enemyScale)). Is colliderRadius in local units?");
      }
    }

    auto &nsc = entity->AddComponent<NativeScriptComponent>();
    // スクリプト実体は次の Update で作られるので、生成直後にメンバへ代入することは
    // できない。pendingData に載せておけば通常の Deserialize と同じ経路で入る。
    if (scripts.is_array() && !scripts.empty()) {
      // 複数スクリプト指定（船は AI ＋ 浮力の 2 本が要る）
      for (const auto &s : scripts) {
        if (!s.contains("name")) continue;
        const std::string name = s["name"].get<std::string>();
        if (name.empty()) continue;
        nsc.AddScript(name);
        if (!nsc.scripts.empty() && s.contains("params")) {
          nsc.scripts.back().pendingData = s["params"];
        }
      }
    } else {
      nsc.AddScript(enemyScript);
      if (!nsc.scripts.empty() && !enemyParams.is_null()) {
        nsc.scripts.back().pendingData = enemyParams;
      }
    }
    nsc.SetScene(scene);
    if (GetSceneContext()) nsc.SetSceneContext(GetSceneContext());

    // WaveManager はこのタグで生存数を数える
    entity->SetTag("wave_id", waveId);

    scene->InitDynamicEntityRuntime(*entity);

    // 生成直後に PrimitiveMesh の Transform を同期して原点でのチラつきを防ぐ。
    // 形状の生成と色の反映は InitDynamicEntityRuntime が済ませているので、
    // ハンドルはそちらが入れたものを読み直す。
    if (usePrimitive) {
      if (auto *pm = entity->GetComponent<PrimitiveMeshComponent>()) {
        if (pm->meshHandle >= 0) {
          if (auto *pmTr = RC::GetPrimitiveMeshTransformPtr(pm->meshHandle)) {
            pmTr->scale = tr.scale;
            pmTr->rotation = tr.rotation;
            pmTr->translation = tr.position;
          }
        }
      }
    }

    Log::Print("[WaveSpawnerScript] spawned " + name);
  }

  /// @brief 形状名を PrimitiveType へ変換する（未知の名前は球にフォールバック）
  /// @param s 形状名（box / cube / plane / cylinder / cone / torus / capsule）
  /// @return 対応する PrimitiveType
  static PrimitiveType ShapeFromString(const std::string &s) {
    if (s == "box" || s == "cube") return PrimitiveType::Box;
    if (s == "plane")              return PrimitiveType::Plane;
    if (s == "cylinder")           return PrimitiveType::Cylinder;
    if (s == "cone")               return PrimitiveType::Cone;
    if (s == "torus")              return PrimitiveType::Torus;
    if (s == "capsule")            return PrimitiveType::Capsule;
    return PrimitiveType::Sphere;
  }

  int spawningWave_ = 0;  ///< 湧かせ中のウェーブ id（0 なら待機）
  int spawnedCount_ = 0;  ///< このウェーブで湧かせた体数
  float elapsed_ = 0.0f;  ///< ウェーブ開始からの経過秒
};

REGISTER_SCRIPT(WaveSpawnerScript)
