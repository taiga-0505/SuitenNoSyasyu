#pragma once
#include <d3d12.h>
#include <string>
#include <functional>
#include <memory>
#include <filesystem>
#include <vector>
#include <nlohmann/json.hpp>
#include "Particle/GPUParticle.h"
#include "Graphics/Texture/RenderTexture/RenderTexture.h"
#include "Camera/CameraController.h"
#include "Scene.h"

class Dx12Core;
class Scene;
class Entity;
enum class PlayState;

/// @class EditorManager
/// @brief ImGui によるエディタ UI（ドッキングレイアウト、テーマ、各種パネル）を管理するクラス
class EditorManager {
public:
  EditorManager() = default;
  ~EditorManager() = default;

  /// @brief エディタの初期化（アイコン読み込み・テーマ設定・設定ファイルの読み込み）
  /// @note アイコンのテクスチャを読むため、RC::Init の後に呼ぶこと。
  void Initialize();

  /// @brief エディタの終了処理（Particle Editor のリソースの解放）
  void Term();

  /// @brief 毎フレームの更新処理（メニューバーやドッキングスペースの構築）
  /// @param core         Dx12Core インスタンスへのポインタ
  /// @param onMenuAppend メニューバー追加用コールバック
  /// @param currentScene 現在のシーン
  void Update(Dx12Core* core = nullptr, std::function<void()> onMenuAppend = nullptr, Scene* currentScene = nullptr);

  /// @brief 各種パネルの描画
  /// @param viewportSrv  ゲーム画面（Viewport）に表示するテクスチャの SRV ハンドル
  /// @param core         Dx12Core インスタンスへのポインタ
  /// @param pm           PipelineManager へのポインタ
  /// @param deltaTime    前のフレームからの経過時間（秒）
  /// @param currentScene 現在のシーン
  void DrawUI(D3D12_GPU_DESCRIPTOR_HANDLE viewportSrv, Dx12Core* core = nullptr, class PipelineManager* pm = nullptr, float deltaTime = 0.0f, Scene* currentScene = nullptr);

  /// @brief Viewport ウィンドウがホバーされているか取得する
  /// @return ホバーされていれば true
  bool IsViewportHovered() const { return isViewportHovered_; }
  /// @brief Viewport ウィンドウのホバー状態を設定する
  /// @param hovered ホバーされているか
  void SetViewportHovered(bool hovered) { isViewportHovered_ = hovered; }

  /// @brief Particle Editor 用の専用プレビュー画面を描画する
  /// @param cl        描画用コマンドリスト
  /// @param core      Dx12Core のインスタンス
  /// @param pm        PipelineManager へのポインタ
  /// @param deltaTime デルタタイム（秒）
  /// @note Particle Editor が開いていて初期化済みのときだけ描画する。
  void RenderParticlePreview(ID3D12GraphicsCommandList* cl, Dx12Core* core, class PipelineManager* pm, float deltaTime);

  /// @brief 現在の Render Queue の状態をテキストダンプとして出力する
  /// @details 出力先は ../logs/render_queue/dump_<日時>.txt。
  void ExportRenderQueueDump();

  /// @brief パネルの表示状態をエディタ設定ファイルへ保存する
  void SaveConfig();
  /// @brief パネルの表示状態をエディタ設定ファイルから読み込む
  /// @note ファイルが無い・読めない場合は現在の値のままにする。
  void LoadConfig();

  /// @brief 現在の再生状態を取得する
  /// @return 再生状態
  PlayState GetPlayState() const;

  /// @brief 再生状態を設定する（外部からのリセット用）
  /// @param state 設定する再生状態
  void SetPlayState(PlayState state);

  /// @brief 再起動（リスタート）がリクエストされたか
  /// @return リクエストされていれば true
  bool IsRestartRequested() const { return restartRequested_; }
  /// @brief 再起動（リスタート）のリクエストを取り消す（処理後に App が呼ぶ）
  void ClearRestartRequest() { restartRequested_ = false; }

  /// @brief 選択中のエンティティ ID を取得する
  /// @return 選択中のエンティティ ID（未選択なら 0）
  uint32_t GetSelectedEntityId() const;

  /// @struct ResizeRequest
  /// @brief エディタから App へのウィンドウリサイズ要求
  struct ResizeRequest {
    bool pending = false;    ///< 未処理の要求があるか
    int width = 0;           ///< 要求するウィンドウ幅
    int height = 0;          ///< 要求するウィンドウ高さ
    bool fullscreen = false; ///< フルスクリーンにするか
  };

  /// @brief ウィンドウリサイズの要求を取得する
  /// @return リサイズ要求（pending が false なら要求なし）
  const ResizeRequest& GetResizeRequest() const { return resizeRequest_; }
  /// @brief ウィンドウリサイズの要求を処理済みにする
  void ClearResizeRequest() { resizeRequest_.pending = false; }

private:
  /// @brief ダークテーマを適用する
  void ApplyDarkTheme();

  /// @brief 初回起動時やリセット時にデフォルトのドッキングレイアウトを構築する
  void SetupDockingLayout();

  /// @brief ツリーノードとしてエンティティを描画する（再帰）
  /// @param e            描画するエンティティ
  /// @param currentScene 現在のシーン
  /// @param childrenMap  親 GUID から子エンティティ一覧を引くマップ
  void DrawEntityNode(std::shared_ptr<Entity> e, Scene* currentScene, const std::unordered_map<uint64_t, std::vector<std::shared_ptr<Entity>>>& childrenMap);

  /// @brief 空のオブジェクト（Transform のみ）を生成して選択状態にする
  /// @param currentScene 現在のシーン
  /// @param parentGuid 親エンティティの GUID（0 でルート）
  /// @return 生成したエンティティ（シーンが無い場合は nullptr）
  std::shared_ptr<Entity> CreateEmptyEntity(Scene* currentScene, uint64_t parentGuid = 0);

  // ============================================================
  // ショートカット / Undo・Redo / コピー＆ペースト
  // ============================================================

  /// @brief F2・Ctrl+C/V/Z/Y などのエディタショートカットを処理する
  /// @param core Dx12Core（スクリーンショット要求用）
  /// @param currentScene 現在のシーン
  /// @details ImGui フレーム内（EditorManager::Update の先頭）で呼ぶこと
  void HandleShortcuts(Dx12Core* core, Scene* currentScene);

  /// @brief 前回コミット時からシーンが変化していれば Undo 履歴へ積む
  /// @param currentScene 現在のシーン
  /// @details ウィジェット操作中・ギズモ操作中は積まないため、
  ///          1 回のドラッグ操作が 1 ステップにまとまる。
  void CommitHistoryIfChanged(Scene* currentScene);

  /// @brief Undo 履歴を初期化する（シーン切り替え時など）
  /// @param currentScene 現在のシーン
  void ResetHistory(Scene* currentScene);

  /// @brief 1 ステップ戻す
  /// @param currentScene 現在のシーン
  void Undo(Scene* currentScene);

  /// @brief 1 ステップ進める
  /// @param currentScene 現在のシーン
  void Redo(Scene* currentScene);

  /// @brief 選択中のエンティティ（と子孫）を内部クリップボードへコピーする
  /// @param currentScene 現在のシーン
  void CopySelectedEntity(Scene* currentScene);

  /// @brief 内部クリップボードの内容を新しいエンティティとして貼り付ける
  /// @param currentScene 現在のシーン
  void PasteEntityClipboard(Scene* currentScene);

  /// @brief スナップショットを適用し、GUID を頼りに選択状態を復元する
  /// @param currentScene 現在のシーン
  /// @param snapshot     適用するスナップショット
  void ApplySnapshot(Scene* currentScene, const nlohmann::json& snapshot);

private:
  bool firstLayout_ = true; ///< 初回レイアウト構築フラグ
  bool resetLayout_ = false; ///< レイアウトリセット要求フラグ
  bool showDemoWindow_ = false; ///< ImGui デモウィンドウの表示フラグ
  bool showPerfWindow_ = false; ///< パフォーマンス（FPS）ウィンドウの表示フラグ
  bool showRenderQueue_ = false; ///< Render Queue デバッグウィンドウの表示フラグ
  bool showParticleEditor_ = false; ///< Particle Editor ウィンドウの表示フラグ
  bool showEnvironmentWindow_ = false; ///< Environment Settings ウィンドウの表示フラグ
  bool showPostEffectWindow_ = false; ///< Post Effect Settings ウィンドウの表示フラグ
  bool showSceneFlow_ = false;        ///< Scene Flow（シーン遷移表）ウィンドウの表示フラグ
  bool showKeyHelp_ = false;          ///< キー操作一覧（Help）ウィンドウの表示フラグ
  bool isViewportHovered_ = false; ///< Viewport ウィンドウがホバーされているか

  PlayState playState_; ///< エディタ上での現在の再生状態
  bool restartRequested_ = false; ///< リスタート要求フラグ

  // --- ツールバーのアイコン（テクスチャ ID。未読み込みなら -1） ---
  int playIconTex_ = -1;    ///< 再生ボタン
  int pauseIconTex_ = -1;   ///< 一時停止ボタン
  int stopIconTex_ = -1;    ///< 停止ボタン
  int restartIconTex_ = -1; ///< リスタートボタン

  // --- Hierarchy のアイコン ---
  int eyeVisibleTex_ = -1;   ///< 表示中
  int eyeHiddenTex_ = -1;    ///< 非表示
  int lockLockedTex_ = -1;   ///< ロック中
  int lockUnlockedTex_ = -1; ///< ロック解除

  // --- フォルダ / ファイルのアイコン（Content Browser など） ---
  int folderIconTex_ = -1;   ///< フォルダ
  int fileIconTex_ = -1;     ///< 一般ファイル
  int fileImageTex_ = -1;    ///< 画像ファイル
  int file3DTex_ = -1;       ///< 3D モデルファイル
  int fileMaterialTex_ = -1; ///< マテリアルファイル
  int fileDocTex_ = -1;      ///< ドキュメントファイル
  int fileFontTex_ = -1;     ///< フォントファイル

  std::weak_ptr<Entity> selectedEntity_; ///< Inspector 表示用の選択エンティティ
  bool dragging2D_ = false;      ///< Viewport 上で 2D 要素（Text/Sprite）をドラッグ中か
  int drag2DMode_ = 0;           ///< 0=なし 1=移動 2=四隅ハンドルでサイズ変更
  float drag2DAnchorX_ = 0.0f;   ///< サイズ変更時に固定する対角コーナーの X（ゲーム px）
  float drag2DAnchorY_ = 0.0f;   ///< サイズ変更時に固定する対角コーナーの Y（ゲーム px）
  float drag2DBaseW_ = 1.0f;     ///< サイズ変更開始時の矩形幅（ゲーム px）
  float drag2DBaseH_ = 1.0f;     ///< サイズ変更開始時の矩形高さ（ゲーム px）
  float drag2DBaseScale_ = 1.0f; ///< サイズ変更開始時の Text scale
  float drag2DLastX_ = 0.0f;     ///< 2D ドラッグの前フレームマウス座標 X
  float drag2DLastY_ = 0.0f;     ///< 2D ドラッグの前フレームマウス座標 Y
  uint32_t renamingEntityId_ = 0; ///< 名前変更中のエンティティ ID
  bool focusRename_ = false; ///< 名前変更用のフォーカスフラグ
  uint64_t expandEntityGuid_ = 0; ///< 子を追加した直後に Hierarchy で展開する親の GUID（0 で無効）
  std::filesystem::path currentDirectory_ = "Resources"; ///< コンテンツブラウザの現在ディレクトリ

  ResizeRequest resizeRequest_; ///< ウィンドウリサイズ要求

  // --- Undo / Redo（スナップショット方式） ---
  bool historyEnabled_ = true;                ///< 履歴機能のオン/オフ（切り分け用）
  static constexpr size_t kMaxHistory = 50;   ///< 履歴の最大数
  static constexpr float kHistoryPollInterval = 0.15f; ///< 変更検知の間隔（秒）
  std::vector<nlohmann::json> undoStack_;     ///< 変更前の状態（古い順）
  std::vector<nlohmann::json> redoStack_;     ///< やり直し用の状態
  nlohmann::json historyCurrent_;             ///< 直近にコミットした状態
  size_t historyHash_ = 0;                    ///< historyCurrent_ のハッシュ
  nlohmann::json historyPending_;             ///< 確定待ちの状態（変化が落ち着くまで保留）
  size_t historyPendingHash_ = 0;             ///< historyPending_ のハッシュ（0 なら保留なし）
  bool historyValid_ = false;                 ///< 履歴が初期化済みか
  bool historyResync_ = false;                ///< 復元直後（次の差分は履歴に積まない）
  bool historyErrorLogged_ = false;           ///< スナップショット失敗を一度だけログする
  int historyBlockLogged_ = -1;               ///< 履歴が止まっている理由（重複ログ抑制用）
  const void* historySceneKey_ = nullptr;     ///< 履歴の対象シーン（切り替え検知用）
  float historyPollTimer_ = 0.0f;             ///< 変更検知のタイマー

  // --- エンティティのコピー＆ペースト ---
  nlohmann::json entityClipboard_; ///< コピーしたエンティティ群（先頭がルート）

  // --- ギズモ設定 ---
  int gizmoOperation_ = 7; ///< ギズモの操作種別（ImGuizmo::OPERATION の値。既定 7 = ImGuizmo::TRANSLATE）
  int gizmoMode_ = 0;      ///< ギズモの座標系（ImGuizmo::MODE の値。既定 0 = ImGuizmo::LOCAL）

  // --- パーティクルエディタ ---
  std::unique_ptr<GPUParticle> peParticle_; ///< Particle Editor 用のプレビュー用 GPUParticle
  bool peInitialized_ = false;  ///< Particle Editor が初期化済みか
  char peJsonPath_[256] = "Resources/Particle/default.json"; ///< JSON の保存 / 読み込みパス
  char peTexPath_[256] = "Resources/Particle/circle.png"; ///< テクスチャパス

  // プレビュー描画用リソース
  RenderTexture peRenderTexture_; ///< プレビュー描画先
  RC::CameraController peCamera_; ///< プレビュー専用カメラ
};
