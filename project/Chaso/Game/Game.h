#pragma once
#include "Scene.h"
#include "SceneManager.h"
#include <string>

/// @class Game
/// @brief アプリケーション全体のメインロジックを統括するクラス
/// @details シーン遷移（SceneManager）の初期化と更新を管理します。
///          オーディオはエンジン側の AudioEngine（シングルトン）と、シーン内の
///          AudioSourceComponent が担当するため、ここでは扱いません。
class Game {
public:
  Game() = default;
  ~Game() = default;

  /// @brief 起動時に読み込むシーン名を設定する
  /// @param name 起動シーン名
  /// @note Init より前に呼ぶこと。未設定なら "Title" が使われる
  void SetBootScene(const std::string &name) { bootScene_ = name; }

  /// @brief 初期化（シーンと遷移表の登録、起動シーンへの遷移）
  /// @param ctx シーンコンテキスト
  void Init(SceneContext &ctx);

  /// @brief 更新処理
  /// @param ctx シーンコンテキスト
  void Update(SceneContext &ctx);

  /// @brief 描画処理
  /// @param ctx シーンコンテキスト
  /// @param cl グラフィックスコマンドリスト
  void Render(SceneContext &ctx, ID3D12GraphicsCommandList *cl);

  /// @brief ポストプロセス後のオーバーレイ描画（ポーズメニューなど画面効果を受けない UI）
  /// @param ctx シーンコンテキスト
  /// @param cl グラフィックスコマンドリスト
  void RenderOverlay(SceneContext &ctx, ID3D12GraphicsCommandList *cl);

  /// @brief 終了処理
  void Term();

  /// @brief デバッグ用（ImGui）のシーンメニューを表示する
  /// @param ctx シーンコンテキスト
  /// @details シーン切替・新規作成・保存・削除を行うメニュー。RC_ENABLE_IMGUI 無効時は何もしない。
  void DrawDebugUI(SceneContext &ctx);

  /// @brief 外部からシーン遷移を要求する
  /// @param name 遷移先のシーン名
  void RequestChange(const std::string &name);

  /// @brief 現在実行中のシーン名を取得する
  /// @return シーン名
  const std::string &CurrentSceneName() const;

  /// @brief 現在のシーンを再読み込みする
  /// @param ctx シーンコンテキスト
  void ReloadCurrentScene(SceneContext &ctx);

  /// @brief 現在のシーンのメモリバックアップを取る
  void BackupCurrentScene();

  /// @brief 現在のシーンをメモリバックアップから復元する
  /// @param ctx シーンコンテキスト
  void RestoreCurrentScene(SceneContext &ctx);

  /// @brief 現在実行中のシーンのポインタを取得する
  /// @return シーンのポインタ（なければnullptr）
  Scene* GetCurrentScene();

  /// @brief シーンマネージャーを取得する
  /// @return シーンマネージャーの参照
  Scene::SceneManager& GetSceneManager() { return sceneMgr_; }

private:
  /// @brief 使用する全てのシーンを SceneManager に登録する
  void registerScenes_();

private:
  Scene::SceneManager sceneMgr_; ///< シーンマネージャー
  std::string bootScene_ = "Title"; ///< 起動シーン名（App が AppConfig.json から渡す）

  /// @brief データ駆動シーンの JSON ファイルを置くディレクトリ
  static inline const std::string kSceneDir = "Resources/Scenes";

  /// @brief エディタ UI: 新規シーン名の入力バッファ
  char newSceneNameBuf_[128] = {};
};
