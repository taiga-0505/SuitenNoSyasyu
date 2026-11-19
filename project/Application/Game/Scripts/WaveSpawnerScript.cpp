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
#include "Game/Framework/StateMachine.h"

#if RC_ENABLE_IMGUI
#include "imgui/imgui.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
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
//
// 敵の種類（サメ・船・鳥…）ごとの違いは「どのスクリプトを載せるか」と
// そのパラメータ（enemyScript / enemyParams / scripts）で JSON から決める。
// 種類ごとに C++ の分岐やクラスを増やすと、新しい敵を足すたびにスポナーを
// 書き換えることになるため、ここはあえてデータドリブンのままにしている。

namespace {

// ---------------------------------------------------------------------
// 敵の見た目の生成手順（Factory Method）
// ---------------------------------------------------------------------
// 以前は SpawnOne の中で「モデルか、プリミティブか」を bool で分岐し、
// 生成直後の処理と InitDynamicEntityRuntime 後の同期処理の 2 か所に if が散っていた。
// 見た目の種類ごとに「付け方」と「初期化後の後始末」をクラスへまとめ、
// どのクラスを使うかの判断は EnemyVisualBuilder::Create の 1 か所に集めた
// （生成の集約。SpawnOne は見た目の種類を知らずに済む）。

/// @brief 生成する敵に見た目（描画コンポーネント）を付ける手順の基底
class EnemyVisualBuilder {
public:
  virtual ~EnemyVisualBuilder() = default;

  /// @brief 見た目の描画コンポーネントを付ける（InitDynamicEntityRuntime の前に呼ぶ）
  /// @param entity 生成した敵
  virtual void AddVisual(Entity &entity) const = 0;

  /// @brief InitDynamicEntityRuntime の後に必要な同期を行う（既定は何もしない）
  /// @param entity 生成した敵
  /// @param tr 敵の Transform
  virtual void AfterRuntimeInit(Entity &entity, const TransformComponent &tr) const {
    (void)entity;
    (void)tr;
  }

  /// @brief 設定に合う生成手順を作る（Factory Method：どの手順を使うかの判断をここに集約する）
  /// @param modelPath モデルのパス（空ならプリミティブで代用する）
  /// @param primitiveShape プリミティブの形状名
  /// @param primitiveColor プリミティブの色
  /// @return 見た目の生成手順
  static std::unique_ptr<EnemyVisualBuilder> Create(const std::string &modelPath,
                                                    const std::string &primitiveShape,
                                                    const RC::Vector4 &primitiveColor);
};

/// @brief モデルで見た目を作る手順
class ModelVisualBuilder final : public EnemyVisualBuilder {
public:
  /// @brief 使うモデルを指定して作る
  /// @param modelPath モデルのパス
  explicit ModelVisualBuilder(const std::string &modelPath) : modelPath_(modelPath) {}

  /// @brief ModelRendererComponent を付ける
  /// @param entity 生成した敵
  void AddVisual(Entity &entity) const override {
    auto &mr = entity.AddComponent<ModelRendererComponent>();
    mr.modelPath = modelPath_;
  }

private:
  std::string modelPath_; ///< モデルのパス
};

/// @brief 素材が揃う前でもウェーブの流れを確認できるよう、プリミティブで代用する手順
class PrimitiveVisualBuilder final : public EnemyVisualBuilder {
public:
  /// @brief 形状と色を指定して作る
  /// @param shape 形状名（box / cube / plane / cylinder / cone / torus / capsule。他は球）
  /// @param color 色
  PrimitiveVisualBuilder(const std::string &shape, const RC::Vector4 &color)
      : type_(ShapeFromString(shape)), color_(color) {}

  /// @brief PrimitiveMeshComponent を付ける
  /// @param entity 生成した敵
  void AddVisual(Entity &entity) const override {
    auto &pm = entity.AddComponent<PrimitiveMeshComponent>();
    pm.type = type_;
    pm.color = color_;
    // ここでは meshHandle を作らない。InitDynamicEntityRuntime が
    // 形状に応じて meshHandle を作り直し、pm.color もマテリアルへ反映するため、
    // 先に自分で生成するとそのハンドルが誰にも参照されないまま残る。
    // Transform の同期だけ Init のあとで行う。
  }

  /// @brief 生成直後に PrimitiveMesh の Transform を同期して原点でのチラつきを防ぐ
  /// @param entity 生成した敵
  /// @param tr 敵の Transform
  void AfterRuntimeInit(Entity &entity, const TransformComponent &tr) const override {
    // 形状の生成と色の反映は InitDynamicEntityRuntime が済ませているので、
    // ハンドルはそちらが入れたものを読み直す。
    if (auto *pm = entity.GetComponent<PrimitiveMeshComponent>()) {
      if (pm->meshHandle >= 0) {
        if (auto *pmTr = RC::GetPrimitiveMeshTransformPtr(pm->meshHandle)) {
          pmTr->scale = tr.scale;
          pmTr->rotation = tr.rotation;
          pmTr->translation = tr.position;
        }
      }
    }
  }

private:
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

  PrimitiveType type_; ///< 形状
  RC::Vector4 color_;  ///< 色
};

std::unique_ptr<EnemyVisualBuilder> EnemyVisualBuilder::Create(const std::string &modelPath,
                                                               const std::string &primitiveShape,
                                                               const RC::Vector4 &primitiveColor) {
  // モデルが指定されていればそれを、無ければデバッグ用のプリミティブを出す。
  // 素材が揃う前でもウェーブの流れだけは確認できるようにしておきたいため。
  if (!modelPath.empty()) {
    return std::make_unique<ModelVisualBuilder>(modelPath);
  }
  return std::make_unique<PrimitiveVisualBuilder>(primitiveShape, primitiveColor);
}

} // namespace

/// @brief ウェーブ開始時に敵を動的生成するスクリプト
/// @details 「待機中」と「湧かせ中」の 2 状態を State パターンで持つ。以前は
///          spawningWave_ が 0 かどうかで if 分岐していたが、状態ごとの処理と
///          遷移条件を状態クラスに閉じ込め、次の状態も状態クラス自身が決めるようにした。
class WaveSpawnerScript : public ScriptableEntity {
public:
  /// @brief スポナーの設定を JSON に書き出す
  /// @return 設定項目を格納した JSON（enemyParams・scripts は指定がある場合のみ含める）
  nlohmann::json Serialize() override {
    nlohmann::json j;
    j["waveId"] = waveId_;
    j["waveManagerName"] = waveManagerName_;
    j["enemyScript"] = enemyScript_;
    j["enemyName"] = enemyName_;
    j["modelPath"] = modelPath_;
    j["count"] = count_;
    j["spreadRadius"] = spreadRadius_;
    j["spawnInterval"] = spawnInterval_;
    j["startDelay"] = startDelay_;
    j["enemyScale"] = {enemyScale_.x, enemyScale_.y, enemyScale_.z};
    j["colliderRadius"] = colliderRadius_;
    j["drawGizmo"] = drawGizmo_;
    j["primitiveShape"] = primitiveShape_;
    j["primitiveColor"] = {primitiveColor_.x, primitiveColor_.y, primitiveColor_.z, primitiveColor_.w};
    if (!enemyParams_.is_null()) j["enemyParams"] = enemyParams_;
    if (scripts_.is_array() && !scripts_.empty()) j["scripts"] = scripts_;
    j["spawnAheadOfPlayer"] = spawnAheadOfPlayer_;
    j["aheadDistance"] = aheadDistance_;
    j["aheadSideStep"] = aheadSideStep_;
    return j;
  }

  /// @brief JSON からスポナーの設定を読み込む
  /// @param j 読み込み元の JSON（存在するキーだけ反映する）
  void Deserialize(const nlohmann::json &j) override {
    if (j.contains("waveId")) waveId_ = j["waveId"].get<int>();
    if (j.contains("waveManagerName")) waveManagerName_ = j["waveManagerName"].get<std::string>();
    if (j.contains("enemyScript")) enemyScript_ = j["enemyScript"].get<std::string>();
    if (j.contains("enemyName")) enemyName_ = j["enemyName"].get<std::string>();
    if (j.contains("modelPath")) modelPath_ = j["modelPath"].get<std::string>();
    if (j.contains("count")) count_ = j["count"].get<int>();
    if (j.contains("spreadRadius")) spreadRadius_ = j["spreadRadius"].get<float>();
    if (j.contains("spawnInterval")) spawnInterval_ = j["spawnInterval"].get<float>();
    if (j.contains("startDelay")) startDelay_ = j["startDelay"].get<float>();
    if (j.contains("colliderRadius")) colliderRadius_ = j["colliderRadius"].get<float>();
    if (j.contains("drawGizmo")) drawGizmo_ = j["drawGizmo"].get<bool>();
    if (j.contains("enemyScale") && j["enemyScale"].is_array() && j["enemyScale"].size() >= 3) {
      enemyScale_ = {j["enemyScale"][0].get<float>(), j["enemyScale"][1].get<float>(),
                     j["enemyScale"][2].get<float>()};
    }
    if (j.contains("enemyParams")) enemyParams_ = j["enemyParams"];
    if (j.contains("scripts")) scripts_ = j["scripts"];
    if (j.contains("spawnAheadOfPlayer")) spawnAheadOfPlayer_ = j["spawnAheadOfPlayer"].get<bool>();
    if (j.contains("aheadDistance")) aheadDistance_ = j["aheadDistance"].get<float>();
    if (j.contains("aheadSideStep")) aheadSideStep_ = j["aheadSideStep"].get<float>();
    if (j.contains("primitiveShape")) primitiveShape_ = j["primitiveShape"].get<std::string>();
    if (j.contains("primitiveColor") && j["primitiveColor"].is_array() &&
        j["primitiveColor"].size() >= 4) {
      const auto &c = j["primitiveColor"];
      primitiveColor_ = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>(),
                         c[3].get<float>()};
    }
  }

  /// @brief 出現地点・散らばり範囲・各敵の出現座標をギズモで描く
  /// @note spawnAheadOfPlayer 時の実際の出現位置（自機前方）は描かず、スポナー位置基準で描く
  void OnDebugRender() override {
    if (!drawGizmo_) return;
    auto *tr = GetComponent<TransformComponent>();
    if (!tr) return;

    const RC::Vector4 color = {1.0f, 0.35f, 0.1f, 1.0f};
    RC::DrawWireSphere3D(tr->position, 0.7f, color, 12, 12, true);
    if (spreadRadius_ > 0.0f) {
      RC::DrawWireSphere3D(tr->position, spreadRadius_, color, 20, 4, true);
    }
    // 実際の出現座標を並べて出す。散らばり方をエディタ上で確認できるようにするため。
    for (int i = 0; i < count_; ++i) {
      RC::DrawWireSphere3D(SpawnPosition(tr->position, i), 0.4f, color, 8, 8, true);
    }
  }

#if RC_ENABLE_IMGUI
  /// @brief スポナーの設定編集 UI と、湧かせ状況・載せるスクリプトの一覧を表示する
  /// @note 状態クラスの完全型が要るので、定義は状態クラスの後に置く
  void OnImGui() override;
#endif

protected:
  /// @brief WaveManager に見つけてもらうための目印タグを立て、湧かせ状態を初期化する
  /// @note 状態クラスの完全型が要るので、定義は状態クラスの後に置く
  void OnCreate() override;

  /// @brief WaveManager の wave_active を読み取り、現在の状態（待機／湧かせ中）を 1 フレーム進める
  /// @param deltaTime 前フレームからの経過時間（秒）
  void OnUpdate(float deltaTime) override {
    Entity *self = GetEntity();
    if (!self) return;

    // waveId をインスペクタで変えたときに追従させる
    if (self->GetTagInt("spawner_wave_id", 0) != waveId_) {
      self->SetTag("spawner_wave_id", waveId_);
    }

    // 進行状況を見る WaveManager を名前で探す
    Scene *scene = GetScene();
    if (!scene) return;
    Entity *manager = waveManagerName_.empty() ? nullptr
                                               : scene->FindEntityByName(waveManagerName_);
    if (!manager) return;

    // 状態クラスはこの値を見て遷移を決める
    observedActiveWave_ = manager->GetTagInt("wave_active", 0);
    fsm_.Update(*this, deltaTime);
  }

private:
  // -------------------------------------------------------------------
  // 状態クラス（State パターン）。入れ子の private クラスなので friend 無しで本体に触れる
  // -------------------------------------------------------------------
  class WaitState;  ///< 待機中（募集を待つ）
  class SpawnState; ///< 湧かせ中

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
    if (spreadRadius_ <= 0.0f || count_ <= 1) return base;

    const float kGoldenAngle = 2.39996323f; // ラジアン
    const float angle = kGoldenAngle * static_cast<float>(index);
    const float radius =
        spreadRadius_ * std::sqrt((static_cast<float>(index) + 0.5f) / static_cast<float>(count_));

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
    const float side = sign * aheadSideStep_ * static_cast<float>(index / 2 + 1);

    RC::Vector3 pos = fallback;
    pos.x = camTr->position.x + fwd.x * aheadDistance_ + right.x * side;
    pos.y = fallback.y; // 高さはスポナーの y（水面）を使う
    pos.z = camTr->position.z + fwd.z * aheadDistance_ + right.z * side;
    return pos;
  }

  /// @brief 敵を 1 体生成する
  /// @param scene 生成先のシーン
  /// @param index 何体目か（エンティティ名と出現位置に使う）
  void SpawnOne(Scene *scene, int index) {
    auto *selfTr = GetComponent<TransformComponent>();
    RC::Vector3 base = selfTr ? selfTr->position : RC::Vector3{0.0f, 0.0f, 0.0f};
    if (spawnAheadOfPlayer_) {
      base = AheadOfPlayerPosition(scene, base, index);
    }

    const std::string name =
        enemyName_ + "_w" + std::to_string(waveId_) + "_" + std::to_string(index);
    auto entity = scene->CreateEntity(name);
    if (!entity) return;

    // 位置は散らばりを加えた座標、向きはスポナーの向きを引き継ぐ
    auto &tr = entity->AddComponent<TransformComponent>();
    tr.position = SpawnPosition(base, index);
    tr.scale = enemyScale_;
    if (selfTr) tr.rotation = selfTr->rotation;

    // 見た目はモデルかプリミティブか。どちらを使うかは生成手順のファクトリに任せる
    const std::unique_ptr<EnemyVisualBuilder> visual =
        EnemyVisualBuilder::Create(modelPath_, primitiveShape_, primitiveColor_);
    visual->AddVisual(*entity);

    // 弾との当たり判定用の球コライダー
    auto &col = entity->AddComponent<ColliderComponent>();
    col.shape = ColliderComponent::Shape::Sphere;
    col.radius = colliderRadius_;
    col.isTrigger = false;
    {
      // 当たり半径が大きすぎる設定を知らせる（ローカル単位の取り違え対策）
      const float worldR = colliderRadius_ * (std::max)((std::max)(std::abs(enemyScale_.x),
                                                                  std::abs(enemyScale_.y)),
                                                        std::abs(enemyScale_.z));
      if (worldR > 8.0f) {
        Log::Print("[WaveSpawnerScript] warning: collider radius is " + std::to_string(worldR) +
                   "m in world (colliderRadius x max(enemyScale)). Is colliderRadius in local units?");
      }
    }

    auto &nsc = entity->AddComponent<NativeScriptComponent>();
    // スクリプト実体は次の Update で作られるので、生成直後にメンバへ代入することは
    // できない。pendingData に載せておけば通常の Deserialize と同じ経路で入る。
    if (scripts_.is_array() && !scripts_.empty()) {
      // 複数スクリプト指定（船は AI ＋ 浮力の 2 本が要る）
      for (const auto &s : scripts_) {
        if (!s.contains("name")) continue;
        const std::string scriptName = s["name"].get<std::string>();
        if (scriptName.empty()) continue;
        nsc.AddScript(scriptName);
        if (!nsc.scripts.empty() && s.contains("params")) {
          nsc.scripts.back().pendingData = s["params"];
        }
      }
    } else {
      nsc.AddScript(enemyScript_);
      if (!nsc.scripts.empty() && !enemyParams_.is_null()) {
        nsc.scripts.back().pendingData = enemyParams_;
      }
    }
    nsc.SetScene(scene);
    if (GetSceneContext()) nsc.SetSceneContext(GetSceneContext());

    // WaveManager はこのタグで生存数を数える
    entity->SetTag("wave_id", waveId_);

    scene->InitDynamicEntityRuntime(*entity);

    // 見た目ごとの初期化後の後始末（プリミティブなら Transform の同期）
    visual->AfterRuntimeInit(*entity, tr);

    Log::Print("[WaveSpawnerScript] spawned " + name);
  }

  // --- インスペクタ / JSON から設定する項目 ---
  int waveId_ = 1;                                ///< 担当するウェーブ id
  std::string waveManagerName_ = "WaveManager";   ///< 進行状況を見に行く WaveManager のエンティティ名
  std::string enemyScript_ = "SharkEnemyScript";  ///< 生成する敵に付けるスクリプト名（REGISTER_SCRIPT の登録名）
  std::string enemyName_ = "WaveEnemy";           ///< 生成する敵のエンティティ名の接頭辞
  /// @brief 敵のモデル。空ならデバッグ用の球で代用する
  /// @details 既定はゲーム用の軽量サメ（shark.obj は 11MB・11 万三角形で、出現のたびに読むと重い）
  std::string modelPath_ = "Resources/model/shark/shark_game.glb";
  int count_ = 3;                 ///< 生成する体数
  float spreadRadius_ = 6.0f;     ///< この地点を中心にどれだけ散らすか（0 なら全員同じ座標）
  float spawnInterval_ = 0.35f;   ///< 1 体ごとの出現間隔（秒）
  float startDelay_ = 0.0f;       ///< ウェーブ開始から 1 体目までの待ち（秒）
  RC::Vector3 enemyScale_ = {4.0f, 4.0f, 4.0f}; ///< 生成する敵のスケール
  /// @brief 生成する敵のコライダー半径（ローカル単位）
  /// @details 実際の当たり半径は colliderRadius × enemyScale の最大成分になる
  ///          （WaterBullet などが Transform のスケールを掛けるため）。
  ///          例: サメ 0.5 × 4 = 2m、船 0.4 × 9 = 3.6m。
  ///          ワールドの m のつもりで大きな値を入れると、船（z スケール 9）では 3.0 で 27m になり、
  ///          自機が球の中に入って「撃った瞬間に当たる」状態になるので注意。
  float colliderRadius_ = 0.5f;
  nlohmann::json enemyParams_;    ///< 敵スクリプトへ渡すパラメータ（"maxHp" などをここに書く）
  /// @brief 複数スクリプトを載せたいときの指定
  /// @details `[{"name": "ShipEnemyScript", "params": {...}}, {"name": "BuoyancyScript", ...}]`
  ///          という配列。船（T-15）は AI と浮力の 2 本を必要とするため用意した。
  ///          省略した場合は enemyScript / enemyParams の 1 本だけを載せる。
  nlohmann::json scripts_;
  /// @brief modelPath が空のときに出すプリミティブ形状
  /// @details sphere / box / plane / cylinder / cone / torus / capsule
  std::string primitiveShape_ = "sphere";
  RC::Vector4 primitiveColor_ = {0.9f, 0.3f, 0.3f, 1.0f}; ///< プリミティブで代用するときの色
  bool drawGizmo_ = true;         ///< 配置確認用のギズモを出すか
  /// @brief 出現地点を「スポナーの位置」ではなく「自機の前方遠く」にする
  /// @details ウェーブ開始時の自機（カメラ）位置から前方 aheadDistance の地点を
  ///          中心に spreadRadius で散らす。スポナーを自機の近くに置いていると
  ///          湧いた瞬間に攻撃圏内へ入って一斉に突進してくるため、その対策。
  ///          高さ（y）はスポナー自身の y を使う（＝水面の高さとして置いておく）。
  bool spawnAheadOfPlayer_ = false;
  float aheadDistance_ = 50.0f;   ///< spawnAheadOfPlayer 時、自機の前方どれだけ先に出すか（m）
  /// @brief spawnAheadOfPlayer 時、1 体ごとに左右交互へずらす幅（m）
  /// @details 同じ方向から縦一列に来ると連続で当たるので、左右に振り分ける。
  float aheadSideStep_ = 8.0f;

  // --- 実行時の状態 ---
  StateMachine<WaveSpawnerScript> fsm_; ///< 待機／湧かせ中のステートマシン
  int observedActiveWave_ = 0;          ///< 今フレームに読んだ WaveManager の wave_active
  int spawnedCount_ = 0;                ///< このウェーブで湧かせた体数
  float elapsed_ = 0.0f;                ///< ウェーブ開始からの経過秒
};

// =====================================================================
// 状態クラスの定義
// =====================================================================

/// @brief 待機中。募集がかかっていて、まだこのウェーブを湧かせていなければ湧かせ中へ
class WaveSpawnerScript::WaitState final : public IState<WaveSpawnerScript> {
public:
  /// @brief 募集を見て湧かせ始めるか、ウェーブ終了後に済み記録を落とす
  /// @param owner 本体
  /// @param machine ステートマシン
  /// @param deltaTime 経過秒数（未使用）
  void OnUpdate(WaveSpawnerScript &owner, StateMachine<WaveSpawnerScript> &machine,
                float deltaTime) override;
  /// @brief 状態名
  /// @return "Wait"
  const char *Name() const override { return "Wait"; }
};

/// @brief 湧かせ中。一定間隔で敵を生成し、全員出し終えたら待機へ戻る
class WaveSpawnerScript::SpawnState final : public IState<WaveSpawnerScript> {
public:
  /// @brief 湧かせるウェーブを指定して作る
  /// @param waveId 湧かせるウェーブ id（途中で waveId が変わっても、このウェーブを湧かせ切る）
  explicit SpawnState(int waveId) : waveId_(waveId) {}

  /// @brief 湧かせ始めに体数と経過時間を 0 に戻す
  /// @param owner 本体
  /// @param machine ステートマシン（未使用）
  void OnEnter(WaveSpawnerScript &owner, StateMachine<WaveSpawnerScript> &machine) override {
    (void)machine;
    owner.spawnedCount_ = 0;
    owner.elapsed_ = 0.0f;
  }

  /// @brief 出現時刻が来た敵を生成し、全員出したら済み記録を立てて待機へ
  /// @param owner 本体
  /// @param machine ステートマシン
  /// @param deltaTime 経過秒数
  void OnUpdate(WaveSpawnerScript &owner, StateMachine<WaveSpawnerScript> &machine,
                float deltaTime) override;

  /// @brief 状態名
  /// @return "Spawn"
  const char *Name() const override { return "Spawn"; }

private:
  int waveId_; ///< 湧かせ中のウェーブ id
};

void WaveSpawnerScript::WaitState::OnUpdate(WaveSpawnerScript &owner,
                                            StateMachine<WaveSpawnerScript> &machine,
                                            float deltaTime) {
  (void)deltaTime;
  Entity *self = owner.GetEntity();
  if (!self) return;
  const int active = owner.observedActiveWave_;
  // 待機中。募集がかかっていて、まだこのウェーブを湧かせていなければ開始する。
  if (active == owner.waveId_ && self->GetTagInt("spawn_done", 0) != owner.waveId_) {
    if (owner.waveId_ != 0) {
      machine.ChangeState<SpawnState>(owner.waveId_);
    } else {
      // waveId 0 は「担当なし」。以前の実装は 0 を「待機中」の印にも使っていたため、
      // 0 番では湧かせ始めず、体数と経過時間だけが 0 に戻っていた。その挙動を保つ。
      owner.spawnedCount_ = 0;
      owner.elapsed_ = 0.0f;
    }
  } else if (active == 0 && self->GetTagInt("spawn_done", 0) != 0) {
    // ウェーブが終わったら次の周回に備えて済み記録を落とす
    self->ClearTag("spawn_done");
  }
}

void WaveSpawnerScript::SpawnState::OnUpdate(WaveSpawnerScript &owner,
                                             StateMachine<WaveSpawnerScript> &machine,
                                             float deltaTime) {
  // 募集が下りたら（強制クリアなど）打ち切る。
  if (owner.observedActiveWave_ != waveId_) {
    machine.ChangeState<WaitState>();
    return;
  }
  Scene *scene = owner.GetScene();
  if (!scene) return;

  owner.elapsed_ += deltaTime;
  // 1 フレームに複数体ぶんの時間が経つこともあるので while で追いつかせる
  while (owner.spawnedCount_ < owner.count_) {
    const float due = owner.startDelay_ + owner.spawnInterval_ * static_cast<float>(owner.spawnedCount_);
    if (owner.elapsed_ < due) break;
    owner.SpawnOne(scene, owner.spawnedCount_);
    ++owner.spawnedCount_;
  }

  // 全員出し終えたら済み記録を立てて待機へ戻る（WaveManager はこのタグで湧き終わりを判定する）
  if (owner.spawnedCount_ >= owner.count_) {
    if (Entity *self = owner.GetEntity()) {
      self->SetTag("spawn_done", waveId_);
    }
    machine.ChangeState<WaitState>();
  }
}

void WaveSpawnerScript::OnCreate() {
  if (Entity *self = GetEntity()) {
    // WaveManager は型を知らずにスポナーを探すので、目印をタグで出しておく
    self->SetTag("wave_spawner", 1);
    self->SetTag("spawner_wave_id", waveId_);
    // ランタイム専用タグの消し込み。タグは Entity::Serialize で JSON へ往復するため、
    // プレイ中に保存されたシーンでは spawn_done が焼き付いていることがある。
    // 残っていると「もう湧かせ済み」と誤判定して敵が 1 体も出ない。
    self->ClearTag("spawn_done");
  }
  spawnedCount_ = 0;
  elapsed_ = 0.0f;
  // 初期状態は待機。Update を待たずに今すぐ入れる
  fsm_.ChangeState<WaitState>();
  fsm_.ApplyPending(*this);
}

#if RC_ENABLE_IMGUI
void WaveSpawnerScript::OnImGui() {
  ImGui::DragInt("Wave Id", &waveId_, 1.0f, 1, 99);
  ImGui::DragInt("Count", &count_, 1.0f, 1, 32);
  ImGui::DragFloat("Spread Radius", &spreadRadius_, 0.1f, 0.0f, 100.0f);
  ImGui::DragFloat("Spawn Interval", &spawnInterval_, 0.05f, 0.0f, 10.0f);
  ImGui::DragFloat("Start Delay", &startDelay_, 0.05f, 0.0f, 20.0f);
  ImGui::DragFloat3("Enemy Scale", &enemyScale_.x, 0.1f);
  ImGui::DragFloat("Collider Radius", &colliderRadius_, 0.05f, 0.05f, 20.0f);
  ImGui::Checkbox("Draw Gizmo", &drawGizmo_);
  ImGui::Checkbox("Spawn Ahead Of Player", &spawnAheadOfPlayer_);
  if (spawnAheadOfPlayer_) {
    ImGui::DragFloat("Ahead Distance", &aheadDistance_, 0.5f, 0.0f, 200.0f);
    ImGui::DragFloat("Ahead Side Step", &aheadSideStep_, 0.1f, 0.0f, 50.0f);
  }

  DrawTextField("Wave Manager", waveManagerName_);
  DrawTextField("Enemy Script", enemyScript_);
  DrawTextField("Enemy Name", enemyName_);
  DrawTextField("Model Path", modelPath_);
  if (modelPath_.empty()) {
    DrawTextField("Primitive Shape", primitiveShape_);
    ImGui::ColorEdit4("Primitive Color", &primitiveColor_.x);
  }

  // ここから下は現在の状態の表示
  ImGui::Separator();
  if (scripts_.is_array() && !scripts_.empty()) {
    ImGui::Text("Scripts (%zu):", scripts_.size());
    for (const auto &s : scripts_) {
      if (s.contains("name")) {
        ImGui::BulletText("%s", s["name"].get<std::string>().c_str());
      }
    }
  }
  ImGui::Text("Spawning: %s", fsm_.IsIn<SpawnState>() ? "yes" : "no");
  ImGui::Text("Spawned: %d / %d", spawnedCount_, count_);
  if (Entity *self = GetEntity()) {
    ImGui::Text("spawn_done: %d", self->GetTagInt("spawn_done", 0));
  }
  if (!enemyParams_.is_null()) {
    ImGui::TextUnformatted(enemyParams_.dump().c_str());
  }
}
#endif

REGISTER_SCRIPT(WaveSpawnerScript)
