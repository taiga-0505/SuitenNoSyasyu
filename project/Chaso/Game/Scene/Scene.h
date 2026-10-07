#pragma once
#include <d3d12.h>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "AppConfig.h"
#include "ECS/Entity.h"
#include "ECS/NativeScriptComponent.h"
#include "ECS/ColliderBroadphase.h"
#include "Common/Math/MathTypes.h"
#include "Common/SceneContext.h"
#include "../Framework/GameModeBase.h"

/// @class Scene
/// @brief ゲームシーンの基底抽象クラス
/// @details 全てのゲームシーン（タイトル、ゲーム本編、リザルトなど）はこのクラスを継承して実装します。
/// シーンのライフサイクル（入場・退場・更新・描画）を定義します。
class Scene {
public:
  virtual ~Scene() = default;

  /// @brief シーン名を取得する
  /// @return シーン名（文字列リテラル）
  virtual const char *Name() const = 0;

  /// @brief シーンに遷移した瞬間に呼び出される
  /// @param ctx シーンコンテキスト
  virtual void OnEnter(SceneContext &) {}

  /// @brief シーンから離れる瞬間に呼び出される
  /// @param ctx シーンコンテキスト
  virtual void OnExit(SceneContext &) {}

  class SceneManager; ///< 前方宣言

  /// @brief シーンの更新処理
  /// @param sm シーンマネージャー（シーン遷移要求に使用）
  /// @param ctx シーンコンテキスト
  virtual void Update(SceneManager &sm, SceneContext &ctx) = 0;

  /// @brief シーンの描画処理
  /// @param ctx シーンコンテキスト
  /// @param cl グラフィックスコマンドリスト
  virtual void Render(SceneContext &ctx, ID3D12GraphicsCommandList *cl) = 0;

  /// @brief ポストプロセス後のオーバーレイ描画（2D のみ）
  /// @param ctx シーンコンテキスト
  /// @param cl グラフィックスコマンドリスト
  /// @details App がポストプロセスの最終出力をレンダーターゲットにしたあとで呼ぶ。
  ///          画面効果の影響を受けない UI（ポーズメニュー等）をここで描く。
  ///          既定では何もしない。
  virtual void RenderOverlay(SceneContext &, ID3D12GraphicsCommandList *) {}

  /// @brief オーディオ（AudioSourceComponent）の毎フレーム更新
  /// @param ctx シーンコンテキスト
  /// @details SceneManager が、シーン遷移の演出中（FadeIn / GrayscaleIntro など
  ///          Update() を止めている期間）も含めて毎フレーム必ず呼ぶ。
  ///          BGM は「持ち主が毎フレーム keep-alive を送る」方式なので、
  ///          Update() の中で行うとゲームを止めた瞬間に BGM が切れてしまう。
  virtual void UpdateAudio(SceneContext &) {}

  /// @brief シーンに属するエンティティのリストを取得
  const std::vector<std::shared_ptr<Entity>>& GetEntities() const { return entities_; }

  /// @brief 新しいエンティティを生成してシーンに追加する
  /// @param name エンティティの名前
  /// @details UpdateEntities のループ中に呼び出しても安全。
  ///          エンティティは pendingEntities_ に追加され、FlushPendingEntities() で
  ///          entities_ にマージされる。
  std::shared_ptr<Entity> CreateEntity(const std::string& name) {
      auto e = std::make_shared<Entity>(name);
      pendingEntities_.push_back(e);
      return e;
  }

  /// @brief pendingEntities_ に溜まった新規エンティティを entities_ にマージする
  /// @details UpdateEntities のループ完了後に呼ぶこと
  void FlushPendingEntities() {
      if (!pendingEntities_.empty()) {
          entities_.insert(entities_.end(),
                           std::make_move_iterator(pendingEntities_.begin()),
                           std::make_move_iterator(pendingEntities_.end()));
          pendingEntities_.clear();
      }
  }

  /// @brief IDでエンティティを削除マーク（次フレームで除去）
  /// @param entityId エンティティのID
  void RemoveEntity(uint32_t entityId) {
      for (auto& e : entities_) {
          if (e && e->GetId() == entityId) {
              e->Destroy();
              break;
          }
      }
  }

  /// @brief 破棄マークされたエンティティを実際に除去する
  void CleanupDestroyedEntities() {
      broadphase_.MarkDirty();
      entities_.erase(
          std::remove_if(entities_.begin(), entities_.end(),
              [this](const std::shared_ptr<Entity>& e) {
                  if (e && e->IsPendingDestroy()) {
                      ReleaseDynamicEntityRuntime(*e);
                      return true;
                  }
                  return false;
              }),
          entities_.end());
  }

  // ============================================================
  // エディタ用スナップショット（Undo / Redo / コピー＆ペースト）
  // ============================================================

  /// @brief 構築済みのエンティティをそのままシーンへ追加する
  /// @param e 追加するエンティティ（Deserialize 済みでもよい）
  /// @details CreateEntity と同様に pendingEntities_ 経由で追加するため、
  ///          更新ループ中に呼んでも安全。
  void AddEntity(std::shared_ptr<Entity> e) {
      if (e) pendingEntities_.push_back(std::move(e));
  }

  /// @brief シーン内の全エンティティを JSON 配列としてスナップショット化する
  /// @return エンティティ JSON の配列
  /// @details Undo 履歴用。破棄予定のエンティティは含めない。
  nlohmann::json CaptureEntitiesSnapshot() {
      FlushPendingEntities();
      nlohmann::json arr = nlohmann::json::array();
      for (auto& e : entities_) {
          if (!e || e->IsPendingDestroy()) continue;
          arr.push_back(e->Serialize());
      }
      return arr;
  }

  /// @brief スナップショットからシーンのエンティティを復元する
  /// @param snapshot CaptureEntitiesSnapshot() の戻り値
  /// @details 後始末の順序は OnExit / RestoreState と揃えて
  ///          「スクリプトへ通知 → ランタイムリソース解放」とする。
  ///          先に解放するとスクリプトの OnDestroy から解放済みハンドルを触りうる。
  void RestoreEntitiesSnapshot(const nlohmann::json& snapshot) {
      if (!snapshot.is_array()) return;

      for (auto& e : entities_) {
          if (!e) continue;
          if (auto* nsc = e->GetComponent<NativeScriptComponent>()) {
              nsc->DestroyAllScripts();
          }
      }
      for (auto& e : entities_) {
          if (e) ReleaseDynamicEntityRuntime(*e);
      }
      entities_.clear();
      pendingEntities_.clear();
      broadphase_.MarkDirty();

      for (auto& ej : snapshot) {
          auto entity = std::make_shared<Entity>();
          entity->Deserialize(ej);
          entities_.push_back(std::move(entity));
      }
      for (auto& e : entities_) {
          if (e) InitDynamicEntityRuntime(*e);
      }
  }

  /// @brief 選択中のライトエンティティのギズモ（ワイヤフレーム）を描画する
  /// @param selectedEntityId 選択中のエンティティID（0なら描画しない）
  /// @details PreDraw3D ～ PreDraw2D の間で呼ぶ必要がある
  void DrawLightGizmos(uint32_t selectedEntityId = 0);

  /// @brief 選択中のカメラエンティティのFrustumギズモを描画する
  /// @param selectedEntityId 選択中のエンティティID（0なら描画しない）
  /// @param aspect アスペクト比
  void DrawCameraGizmos(uint32_t selectedEntityId = 0, float aspect = 16.0f / 9.0f);

  /// @brief 選択中のコライダーエンティティのギズモを描画する
  /// @param selectedEntityId 選択中のエンティティID（0なら描画しない）
  void DrawColliderGizmos(uint32_t selectedEntityId = 0);

  /// @brief 選択中のエンティティIDを設定する
  void SetSelectedEntityId(uint32_t id) { selectedEntityId_ = id; }
  /// @brief 選択中のエンティティIDを取得する
  uint32_t GetSelectedEntityId() const { return selectedEntityId_; }

  /// @brief 全コライダーのデバッグ描画フラグを設定する
  void SetShowColliderGizmos(bool show) { showColliderGizmos_ = show; }
  /// @brief 全コライダーのデバッグ描画フラグを取得する
  bool GetShowColliderGizmos() const { return showColliderGizmos_; }

  /// @brief GameModeの取得
  GameModeBase* GetGameMode() const { return gameMode_.get(); }

  /// @brief 現在のSceneContextを取得する
  SceneContext* GetContext() const { return currentContext_; }

  // ============================================================
  // 当たり判定付き移動（キャラクターコントローラー用）
  // ============================================================

  /// @brief 指定エンティティのコライダーを testPos に置いたとき、
  ///        他のブロッキングコライダーと重なるかを判定する
  /// @param self 判定対象のエンティティ（自身は除外される）
  /// @param testPos self の TransformComponent::position をこの値に置き換えて判定する
  /// @param skin コライダーを内側に縮める余裕量（m）。床や壁と面が接した状態で
  ///             引っかかるのを防ぐため、既定で 0.05 だけ小さく判定する
  /// @param hitOut 重なった相手のエンティティを受け取る（不要なら nullptr）
  /// @return 重なっていれば true
  /// @note isTrigger のコライダー、無効なコライダーは無視する
  bool TestBlockingOverlap(Entity* self, const RC::Vector3& testPos,
                           float skin = 0.05f, Entity** hitOut = nullptr);

  /// @brief 壁抜け（トンネリング）しない移動を行う
  /// @param self 移動させるエンティティ
  /// @param delta このフレームの移動量（速度×deltaTime）
  /// @param maxStep 1回の判定で進む最大距離(m)。これを壁の最小厚みより小さく保つことで
  ///                高速移動時でも「飛び越え」が起きない。既定 0.1m
  /// @param skin TestBlockingOverlap に渡す余裕量
  /// @return 実際に移動した量（壁に阻まれた分は差し引かれる）
  /// @details delta を maxStep 以下に分割し、各ステップで X→Z→Y の順に
  ///          軸ごとに移動を試す。ブロックされた軸だけを取り消すため、
  ///          斜め移動で壁に当たっても壁に沿ってスライドする。
  ///          既にめり込んでいる場合はその軸の移動を許可して脱出できるようにする。
  RC::Vector3 MoveWithCollision(Entity* self, const RC::Vector3& delta,
                                float maxStep = 0.1f, float skin = 0.05f);

  /// @brief 動的に生成したエンティティのランタイムリソースを初期化する
  /// @param e 初期化するエンティティ
  /// @details 派生クラスでオーバーライドして、モデルロードやメッシュ生成を行う
  virtual void InitDynamicEntityRuntime(Entity& e) {}

  /// @brief シーンをファイルへ保存する（エディタの Ctrl+S が呼ぶ）
  /// @return 保存できたら true。保存先を持たないシーンは false
  virtual bool Save() { return false; }

  /// @brief 動的に生成したエンティティのランタイムリソースを解放する
  /// @param e 解放するエンティティ
  virtual void ReleaseDynamicEntityRuntime(Entity& e) {}

protected:
  std::vector<std::shared_ptr<Entity>> entities_; ///< シーン内のエンティティ一覧
  std::vector<std::shared_ptr<Entity>> pendingEntities_; ///< 更新ループ中に生成されたエンティティの一時バッファ
  uint32_t selectedEntityId_ = 0; ///< 選択中のエンティティID
  std::unique_ptr<GameModeBase> gameMode_ = std::make_unique<GameModeBase>(); ///< ゲームルールの管理
  SceneContext* currentContext_ = nullptr; ///< 現在フレームのSceneContext
  bool showColliderGizmos_ = false; ///< コライダーを全描画するデバッグフラグ
  bool showAllGizmos_ = false; ///< 全デバッグ描画（F4）フラグ

  /// @brief コライダーの粗い判定（DynamicBVH）。ResolveCollisions / TestBlockingOverlap が使う
  /// @details UpdateEntities と ResolveCollisions の先頭で全エンティティと同期する
  ///          （前フレームの ResolveCollisions 以降に、押し出し・アニメーションの読み戻し・
  ///          エディタ操作などで位置が変わっている可能性があるため）。
  ///          同期は 1 体あたりコンポーネント検索 4 回＋箱の比較で、動いていない物は BVH を触らない。
  ColliderBroadphase broadphase_;

  /// @brief Update all active entities
  /// @details インデックスベースループを使用し、ループ中の entities_ push_back による
  ///          イテレータ無効化を回避する（新規エンティティは pendingEntities_ 経由で追加される）
  void UpdateEntities(float deltaTime) {
    // スクリプトの MoveAndSlide（TestBlockingOverlap）が引く BVH を最新にしておく
    broadphase_.Sync(entities_);
    const size_t count = entities_.size(); // ループ前にサイズを固定
    for (size_t i = 0; i < count; ++i) {
      auto& e = entities_[i];
      if (e && e->IsActive() && !e->IsPendingDestroy()) {
        e->UpdateAll(deltaTime);
      }
    }
    FlushPendingEntities();
  }

  /// @brief Resolve physics collisions among entities
  void ResolveCollisions();

  /// @brief Remove entities marked for destruction
  void RemoveDeadEntities() {
    // 破棄予定のエンティティを先に別の入れ物へ退避して参照を残しておく。
    // entities_ から消しながら解放すると、スクリプトの OnDestroy から
    // GetEntities() を辿ったときに解放途中の要素を触ってしまう。
    std::vector<std::shared_ptr<Entity>> doomed;
    for (auto& e : entities_) {
      if (!e || e->IsPendingDestroy()) doomed.push_back(e);
    }
    if (doomed.empty()) return;

    broadphase_.MarkDirty();
    entities_.erase(
      std::remove_if(entities_.begin(), entities_.end(),
        [](const std::shared_ptr<Entity>& e) {
          return !e || e->IsPendingDestroy();
        }),
      entities_.end());

    // entities_ が正しい状態になってから後始末する。
    // 順序は OnExit と揃えて「スクリプトへ通知 → ランタイムリソース解放」とする。
    // 先に解放するとスクリプトの OnDestroy から解放済みハンドルを触りうるため。
    for (auto& e : doomed) {
      if (!e) continue;
      if (auto* nsc = e->GetComponent<NativeScriptComponent>()) {
        nsc->DestroyAllScripts();
      }
    }
    // ランタイムリソース（モデル／メッシュ／ライト等のハンドル）は shared_ptr の
    // 破棄では返らないため、ここで明示的に解放する。これを忘れると、撃破した敵や
    // 破棄されたエフェクトのぶんだけハンドルが積み上がり、シーンを抜けるまで回収されない。
    for (auto& e : doomed) {
      if (e) ReleaseDynamicEntityRuntime(*e);
    }
    doomed.clear();
  }

  /// @brief Destroy entity by name
  /// @return true if found and marked
  bool DestroyEntityByName(const std::string& name) {
    for (auto& e : entities_) {
      if (e && e->Name() == name) { e->Destroy(); return true; }
    }
    return false;
  }

  /// @brief Destroy entity by ID
  /// @return true if found and marked
  bool DestroyEntityById(uint32_t id) {
    for (auto& e : entities_) {
      if (e && e->Id() == id) { e->Destroy(); return true; }
    }
    return false;
  }

public:
  /// @brief Find entity by name
  /// @return Pointer to entity, or nullptr
  /// @note スクリプト側（ScriptableEntity 派生）から名前でエンティティを引くために
  ///       public にしている。protected のままだと Scene の派生クラス以外から
  ///       呼べず、T-20 のルート分岐がタグ参照先を辿れない。
  Entity* FindEntityByName(const std::string& name) {
    for (auto& e : entities_) {
      if (e && e->Name() == name) return e.get();
    }
    return nullptr;
  }

  /// @brief Find entity by ID
  std::shared_ptr<Entity> FindEntityById(uint32_t id) {
    for (auto& e : entities_) {
      if (e && e->Id() == id) return e;
    }
    return nullptr;
  }

  /// @brief Find entity by GUID
  std::shared_ptr<Entity> FindEntityByGuid(uint64_t guid) {
    for (auto& e : entities_) {
      if (e && e->Guid() == guid) return e;
    }
    return nullptr;
  }

  /// @brief Destroy an entity and all its children recursively
  void DestroyEntityRecursive(std::shared_ptr<Entity> entity) {
    if (!entity) return;
    entity->Destroy();
    for (auto& child : entities_) {
      if (child && child->ParentGuid() == entity->Guid()) {
        DestroyEntityRecursive(child);
      }
    }
  }
};
