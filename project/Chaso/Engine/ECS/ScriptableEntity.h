#pragma once
#include "Entity.h"
#include "Common/Math/MathUtils.h"
#include <string>

// Forward declarations
class Scene;
struct SceneContext;

/// @brief Base class for native scripts attached to entities.
/// Allows writing custom update logic similar to Unity's MonoBehaviour.
class ScriptableEntity {
public:
  virtual ~ScriptableEntity() = default;

  /// @brief Serialize script data to JSON
  virtual nlohmann::json Serialize() { return nlohmann::json::object(); }

  /// @brief Deserialize script data from JSON
  virtual void Deserialize(const nlohmann::json& j) {}

  /// @brief Called for drawing custom ImGui UI in the editor
  virtual void OnImGui() {}

  /// @brief Get a component attached to the same entity
  template <typename T> T* GetComponent() {
    return entity_->GetComponent<T>();
  }

  /// @brief Check if a component is attached to the same entity
  template <typename T> bool HasComponent() {
    return entity_->HasComponent<T>();
  }

  /// @brief Add a component to the same entity
  template <typename T, typename... Args> T& AddComponent(Args&&... args) {
    return entity_->AddComponent<T>(std::forward<Args>(args)...);
  }

  /// @brief Get the entity this script is attached to
  Entity* GetEntity() const { return entity_; }

  /// @brief Get the scene this script belongs to
  Scene* GetScene() const { return scene_; }

  /// @brief Get the current scene context
  SceneContext* GetSceneContext() const { return sceneContext_; }

  /// @brief Called during the render phase (command list is open)
  /// @details Use this for 2D drawing (DrawBox, DrawCircle, etc.)
  virtual void OnRender() {}

  /// @brief メイン 3D パス中に呼ばれる（再生中のみ・1 フレーム 1 回）
  /// @details エンティティを作らずに 3D の物を描きたいスクリプト用
  ///          （雨筋などの大量の粒を RC::DrawModelBatchColored でまとめて描く等）。
  ///          ここで積んだ 3D コマンドは深度テスト付きで描かれるので、
  ///          ロゴや船など手前の物に正しく隠れる。影パス・マスクパスでは呼ばれない。
  ///          2D 描画（DrawBox / DrawString 等）や状態の更新は行わないこと。
  virtual void OnRender3D() {}

  /// @brief ポストプロセスの後（最終画像の上）に呼ばれる 2D 描画
  /// @details 輪郭・水中・ビネット・ディゾルブなどの画面効果を受けたくない UI
  ///          （ポーズメニュー、設定画面など）はここで描く。OnRender で描いた HUD は
  ///          効果の下に沈むが、ここで描いたものは常に一番手前に出る。
  ///          2D 描画関数（DrawBox / DrawString 等）だけを使うこと。3D は描けない。
  ///          シーン遷移のディゾルブも掛からないので、遷移を要求したら自分で消すこと。
  virtual void OnOverlayRender() {}

  /// @brief シャドウパス中に呼ばれる。「影を落とす（光を遮る）」3D ジオメトリだけをここで描く
  /// @details スクリプトが OnRender で自前に 3D モデルを描いている場合（マップの壁・床、ドアなど）、
  ///          ここでも同じモデルを RC::DrawModel / RC::DrawModelBatchColored で描くこと。
  ///          描いた物はシャドウマップ（平行光源）とスポットライト影アトラスの両方に深度として書かれ、
  ///          その向こう側へ光が漏れなくなる。
  ///          1 フレームに「影を落とすライトの数」だけ呼ばれるので、2D 描画（HUD 等）や
  ///          ギズモ、状態の更新は行わないこと。既定では何も描かない（＝影を落とさない）。
  virtual void OnShadowRender() {}

  /// @brief マスクパス中に呼ばれる。「輪郭を強調したい」3D ジオメトリだけをここで描く
  /// @details 描いた物のシルエットがマスクRTへ白く書かれ、`PostEffectType::MaskOutline`
  ///          がその外周だけを塗る（インタラクトできる扉・アイテムの強調など）。
  ///          描画コードは OnShadowRender と同じで、`RC::DrawModel` 系をそのまま呼べば
  ///          エンジンが PSO をマスク用へ振り替える。
  ///          強調していないフレームは「何も描かない」こと（＝そのまま return する）。
  ///          1 フレームに 1 回だけ呼ばれる。2D 描画や状態の更新は行わないこと。
  virtual void OnMaskRender() {}

  /// @brief Called during the overlay 3D phase to draw gizmos
  /// @details 呼ばれる条件: F4（全デバッグ描画）ON、またはこのエンティティが選択中
  virtual void OnDebugRender() {}

  /// @brief コライダーデバッグ表示（F3）中に呼ばれる
  /// @details 呼ばれる条件は ColliderComponent のギズモと同じ
  ///          （F3 ON / F4 ON / このエンティティが選択中）。
  ///          感知範囲・視界・攻撃範囲など、ColliderComponent を持たない
  ///          「見えない当たり判定」をワイヤで可視化する用途に使う。
  ///          再生中・停止中を問わず呼ばれるので、実際の判定に使っている値をそのまま描くこと。
  virtual void OnColliderDebugRender() {}

  /// @brief Called when a collision occurs with another entity
  /// @param other The entity that this entity collided with
  /// @param contactPoint The world position where the collision occurred
  virtual void OnCollision(Entity* other, const RC::Vector3& contactPoint = {}) {}

  /// @brief 当たり判定付きで移動する（壁抜け防止＋壁ずり）
  /// @param delta このフレームの移動量（速度 × deltaTime）
  /// @param maxStep 1回の判定で進む最大距離(m)。シーン内の最も薄い壁の厚みより
  ///                小さくしておくこと（既定 0.1m）
  /// @return 実際に移動した量
  /// @details TransformComponent::position を直接書き換える代わりにこれを使うと、
  ///          移動量が maxStep 以下に分割されるためダッシュ中でも壁を貫通しない。
  ///          斜め移動で壁に当たった場合は壁に沿ってスライドする。
  /// @note 実体は Application 側の Scene.cpp で定義している（Scene の完全型が必要なため）
  RC::Vector3 MoveAndSlide(const RC::Vector3& delta, float maxStep = 0.1f);

  /// @brief 指定したシーンへの遷移を要求する
  /// @param name 遷移先のシーン名（Resources/Scenes/*.json のファイル名）
  /// @return 要求が受理されたら true（未登録のシーン名、遷移中、編集モード中は false）
  /// @details 実際の切り替えはフェード演出を挟んで数フレーム後に行われるため、
  ///          呼び出した直後もこのスクリプトと所属エンティティは生存している。
  ///          即時切り替え（ChangeImmediately）は呼び出し元自身が解放されて
  ///          use-after-free になるため、スクリプトには公開していない。
  /// @note 再生中（PlayState::Playing）以外では何もしない。編集モードで遷移すると
  ///       保存していないシーン編集が失われるため。
  /// @note 実体は Application 側の Scene.cpp で定義している（SceneContext の完全型が必要なため）
  bool RequestSceneChange(const std::string& name);

  /// @brief 遷移演出を指定してシーン遷移を要求する
  /// @param name 遷移先のシーン名
  /// @param transition 演出名。"dissolve"（既定の黒ディゾルブ）/ "dive"（水面へ飛び込む遷移）。
  ///                   名前の定義は Chaso/Game/Scene/Scene.h の SceneTransitions。
  ///                   不明な名前は "dissolve" として扱う。
  /// @details エンジン層のこのヘッダはアプリ層の SceneTransition 型を知らないため、
  ///          文字列で受けて Scene.cpp 側で変換する。その他の挙動は 1 引数版と同じ。
  bool RequestSceneChange(const std::string& name, const std::string& transition);

  /// @brief 遷移表（Resources/SceneFlow.json）に従って遷移を要求する
  /// @param trigger きっかけの名前（例: "start" / "back" / "retry"）
  /// @param arg 遷移先が "$arg" の行に渡す名前（スクリプトが行き先を決める場合）
  /// @return 要求が受理されたら true
  /// @details シーン名をスクリプトに書かずに済む。行き先と演出はエディタの
  ///          Window > Scene Flow で変えられる。表に行が無いきっかけは
  ///          Scene Flow の「未登録のきっかけ」に出るので、そこから追加できる。
  ///          再生中以外では何もしない（RequestSceneChange と同じ）。
  /// @note 実体は Application 側の Scene.cpp で定義している
  bool RequestTransition(const std::string& trigger, const std::string& arg = "");

  /// @brief 遷移表を引くだけ（遷移はしない）
  /// @param trigger きっかけの名前
  /// @param outTarget 遷移先シーン名（変数は解決済み。行き先が無ければ空）
  /// @param outTransition 演出名（"dissolve" / "dive"）
  /// @param arg 遷移先が "$arg" の行に渡す名前
  /// @return 表に行があれば true
  /// @details 遷移の前に独自の演出（飛び込みなど）を挟むスクリプトが、
  ///          演出名を見て挟むかどうかを決め、最後に RequestSceneChange(outTarget, ...) を呼ぶ。
  ///          行き先が無いボタン（最終面の「次へ」など）を隠す判定にも使える。
  bool LookupTransition(const std::string& trigger, std::string& outTarget,
                        std::string& outTransition, const std::string& arg = "");

protected:
  /// @brief Called when the script is created
  virtual void OnCreate() {}

  /// @brief Called every frame
  /// @param deltaTime Time elapsed since the last frame
  virtual void OnUpdate(float deltaTime) {}

  /// @brief Called when the script is destroyed
  virtual void OnDestroy() {}

private:
  Entity* entity_ = nullptr;
  Scene* scene_ = nullptr;
  SceneContext* sceneContext_ = nullptr;
  friend class NativeScriptComponent;
};
