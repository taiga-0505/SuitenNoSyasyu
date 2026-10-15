#pragma once
#include "Fade/Fade.h"
#include "Scene.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace RC { class CameraController; }

// 前方宣言
class ISceneState;

/// @class SceneManager
/// @brief ゲームシーンの切り替えと管理を行うクラス
/// @details State パターンを使用して、通常時・フェードアウト時・ロード時・フェードイン時の各状態を管理する。
///          シーンの登録、遷移リクエストの受付、およびメインループからの更新・描画呼び出しを統括する。
class Scene::SceneManager {
public:
  SceneManager();
  ~SceneManager();

  std::unique_ptr<Fade> fade_ = nullptr; ///< フェード演出用オブジェクト
  static inline const float kFadeTime = 1.0f; ///< デフォルト（Dissolve）のフェード時間（秒）
  /// @brief Dive 遷移のフェード時間（秒）
  /// @details 飛び込み側（Title）が画面を深海色まで暗くしてから要求してくるので、
  ///          ここでは切り替えの継ぎ目を隠すぶんだけ短く抜ける。
  static inline const float kDiveFadeTime = 0.35f;
  /// @brief Ascend 遷移（空へ抜ける）のフェード時間（秒）
  /// @details 飛び出す側が画面を白く飛ばしてから要求してくるので、Dive と同じく短く抜ける。
  static inline const float kAscendFadeTime = 0.4f;

  /// @brief Dive 遷移で抜ける画面色を設定する
  /// @details Dive はゲーム側が画面をこの色まで暗くしてから要求する前提の遷移。
  ///          色はゲームごとに違うので、エンジンは既定の黒だけを持ち、
  ///          Application 側が起動時（静的初期化でも可）に設定する。
  /// @param color 抜ける画面色（RGBA）
  static void SetDiveScreenColor(const RC::Vector4 &color);

  /// @brief Dive 遷移で抜ける画面色を取得する
  /// @return 設定済みの画面色（未設定なら黒）
  static const RC::Vector4 &DiveScreenColor();

public:
  /// @brief 初期化
  /// @param ctx シーンコンテキスト
  void Init(SceneContext &ctx);

  /// @brief 終了処理
  /// @details SceneFlow に渡した this 捕捉のコールバックを外し、登録シーンと状態を破棄する。
  void Term();

  /// @brief 新しいシーンを登録する
  /// @param scene 登録するシーンの unique_ptr（Name() をキーにする。同名は上書き）
  void Register(std::unique_ptr<Scene> scene);

  /// @brief 次のフレームでのシーン切り替えをリクエストする（フェード演出を伴う）
  /// @param name 遷移先のシーン名
  /// @param transition 遷移演出。既定は黒へのディゾルブ（SceneTransition::Dissolve）
  /// @return 要求が受理されたら true
  /// @note 未登録のシーン名は受理しない。受理してしまうと ChangeImmediately が
  ///       current_ = nullptr にしてしまい、画面が真っ黒なまま復帰できなくなる。
  /// @note 既に別の遷移要求が処理待ちの場合も受理しない（フェード中の上書き防止）。
  bool RequestChange(const std::string &name,
                     SceneTransition transition = SceneTransition::Dissolve);

  /// @brief 処理中（要求済み〜フェードイン完了まで）の遷移演出を取得する
  /// @return 進行中の遷移演出。遷移していなければ SceneTransition::None
  SceneTransition ActiveTransition() const { return transition_; }

  /// @brief シーンを即座に切り替える（演出なし）
  /// @param name 遷移先のシーン名
  /// @param ctx シーンコンテキスト
  void ChangeImmediately(const std::string &name, SceneContext &ctx);

  /// @brief 現在のシーンを再読み込みする（OnExit -> OnEnter）
  /// @param ctx シーンコンテキスト
  void ReloadCurrentScene(SceneContext &ctx);

  /// @brief 現在のシーンをバックアップする
  /// @note DataDrivenScene のときだけ有効。それ以外のシーンでは何もしない
  void BackupCurrentScene();
  
  /// @brief バックアップから現在のシーンを復元する
  /// @param ctx シーンコンテキスト
  /// @note DataDrivenScene のときだけ有効。それ以外のシーンでは何もしない
  void RestoreCurrentScene(SceneContext &ctx);

  /// @brief 更新処理（現在の状態に応じた Update を呼び出す）
  /// @param ctx シーンコンテキスト
  void Update(SceneContext &ctx);

  /// @brief 描画処理（現在の状態に応じた Render を呼び出す）
  /// @param ctx シーンコンテキスト
  /// @param cl グラフィックスコマンドリスト
  void Render(SceneContext &ctx, ID3D12GraphicsCommandList *cl);

  /// @brief ポストプロセス後のオーバーレイ描画（現在のシーンの RenderOverlay を呼び出す）
  /// @param ctx シーンコンテキスト
  /// @param cl グラフィックスコマンドリスト
  /// @details 遷移の状態（フェード中など）に関わらず現在のシーンへ委譲する。
  ///          ディゾルブはポストプロセス側なので、ここで描いたものはフェードの影響を受けない。
  void RenderOverlay(SceneContext &ctx, ID3D12GraphicsCommandList *cl);

  /// @brief マネージャーの状態（ISceneState）を遷移させる
  /// @param newState 新しい状態オブジェクト
  void ChangeState(std::unique_ptr<ISceneState> newState);

  /// @brief 現在実行中のシーン名を取得する
  /// @return シーン名
  const std::string &CurrentName() const { return currentName_; }

  /// @brief 現在アクティブなシーンのポインタを取得する
  /// @return シーンへのポインタ
  Scene* GetCurrentScene() const { return current_; }

  /// @brief 登録済みの全シーン名を取得する
  /// @return シーン名の一覧（順序は不定）
  std::vector<std::string> GetSceneNames() const {
    std::vector<std::string> names;
    for (auto& [key, _] : scenes_) {
      names.push_back(key);
    }
    return names;
  }

  /// @brief ディレクトリ内の JSON ファイルを DataDrivenScene として登録する
  /// @param dirPath シーン JSON を置いたディレクトリ
  /// @note シーン名はファイル名（拡張子なし）。同名のシーンが登録済みなら飛ばす
  void LoadScenesFromDirectory(const std::string& dirPath);

  /// @brief 新しいシーンを作成して JSON に保存し、登録する
  /// @param name 新しいシーン名
  /// @param dirPath シーン JSON を保存するディレクトリ
  /// @return 作成できたら true。同名シーンが既にあれば false
  /// @details テンプレート（SceneTemplate.json）があれば複製し、無ければ
  ///          ディレクショナルライトとメインカメラだけの初期シーンを作る。
  bool CreateNewScene(const std::string& name, const std::string& dirPath);

  /// @brief 登録済みシーンを削除する（DataDrivenScene なら JSON ファイルも削除）
  /// @param name 削除するシーン名
  /// @return 削除できたら true。未登録または現在アクティブなシーンなら false
  bool DeleteScene(const std::string& name);

  /// @brief 現在のシーンをファイルへ保存する
  /// @return 保存できたら true。DataDrivenScene 以外のシーンは false
  bool SaveCurrentScene();

private:
  /// @brief 登録済みシーンを名前で検索する内部ヘルパー
  /// @param name シーン名
  /// @return 見つかったシーンのポインタ（なければ nullptr）
  Scene *get_(const std::string &name);

private:
  std::unordered_map<std::string, std::unique_ptr<Scene>> scenes_; ///< 登録済みシーンのマップ
  Scene *current_ = nullptr;        ///< 現在アクティブなシーンのポインタ
  std::string currentName_;        ///< 現在アクティブなシーンの名前
  std::string requested_;          ///< 遷移リクエストされたシーン名
  /// @brief 進行中の遷移演出。RequestChange で決まり、演出が終わって NormalState に
  ///        戻るまで保持する。演出なしの直接切り替え（起動時など）は None
  SceneTransition transition_ = SceneTransition::None;

  std::unique_ptr<ISceneState> state_; ///< 現在のマネージャー状態（NormalState / FadeOutState など）
  std::unique_ptr<RC::CameraController> camera_; ///< エディタカメラ（全シーン共有）

  // 状態クラスからのアクセスを許可
  friend class NormalState;
  friend class FadeOutState;
  friend class LoadingState;
  friend class FadeInState;
  friend class GrayscaleIntroState;
};
