#pragma once
#include "AppConfig.h"
#include "Dx12Core.h"
#include "Game.h"
#include "Graphics/PostProcess/PostProcess.h"
#include "Graphics/Texture/RenderTexture/RenderTexture.h"
#include "GraphicsPipeline/GraphicsPipeline.h"
#include "ImGuiManager/ImGuiManager.h"
#include "Editor/EditorManager.h"
#include "Input/Input.h"
#include "PipelineManager.h"
#include "Scene.h"
#include "Window/Window.h"
#include <memory>

class TitleScene;
class SelectScene;
class GameScene;
class ResultScene;
class GameOverScene;
class SampleScene;

/// @class App
/// @brief アプリケーション全体の最上位管理クラス
/// @details ウィンドウ生成、DirectX12 コア、入力システム、描画パイプライン、シーン管理などの
///          すべての主要サブシステムを保持し、メインループ（Update / Render）を統括する。
class App {
public:
  /// @brief コンストラクタ（サブシステムのインスタンス生成のみを行う）
  App();

  /// @brief デストラクタ
  ~App();

  /// @brief アプリケーションの初期化
  /// @details ウィンドウ作成、DX12 初期化、各マネージャーのセットアップ、初期シーンの構築を行う。
  /// @return 成功すれば true
  bool Init();

  /// @brief メインループの実行
  /// @details ウィンドウが閉じられるまで Update と Render を繰り返す。
  /// @return 終了コード（WM_QUIT の wParam）
  int Run();

  /// @brief アプリケーションの終了処理
  void Term();

  /// @brief ウィンドウの解像度とフルスクリーン設定を動的に変更する
  /// @param width      新しいウィンドウ幅
  /// @param height     新しいウィンドウ高さ
  /// @param fullscreen フルスクリーンにするか
  /// @note 現在の設定と同じなら何もしない。変更時は GPU 完了を待ってから作り直し、設定ファイルにも保存する。
  void ResizeWindow(int width, int height, bool fullscreen);

  /// @brief アプリケーション設定をファイルから読み込む
  void LoadAppConfig();

  /// @brief アプリケーション設定をファイルへ保存する
  void SaveAppConfig();

private:
  /// @brief フレーム更新処理
  void Update();

  /// @brief フレーム描画処理
  void Render();

private:
  ID3D12GraphicsCommandList *cl_ = nullptr; ///< 現在のフレームのコマンドリスト
  ID3D12Device *device_ = nullptr;            ///< D3D12 デバイス

  // 設定 & ログ
  AppConfig appConfig_; ///< アプリケーション設定（画面解像度など）

  // ウィンドウ / DX12
  std::unique_ptr<Window> window_; ///< メインウィンドウ
  Dx12Core core_;                 ///< DirectX12 コアシステム
  Dx12Core::Desc coreDesc_{};      ///< DX12 初期化設定

  // 入力
  std::unique_ptr<Input> input_; ///< 入力システム

  // ImGui / エディタ
  ImGuiManager imgui_; ///< ImGui 管理
  EditorManager editorManager_; ///< エディタ管理

  // パイプライン
  PipelineManager pm_; ///< グラフィックスパイプライン管理

  // オフスクリーンレンダリング / ポストプロセス
  RenderTexture renderTexture_;             ///< メイン描画用レンダーテクスチャ
  RenderTexture viewportTexture_;           ///< エディタ Viewport 描画用レンダーテクスチャ
  std::unique_ptr<PostProcess> postProcess_; ///< ポストプロセス（グレイスケール、ガウスぼかし等）

  // === シーン管理 ===
  // === Game（シーン切替などゲーム側の責務） ===
  Game game_;             ///< ゲームロジック・シーン遷移管理
  SceneContext sceneCtx_; ///< シーン間で共有されるコンテキストデータ

  // Win32 メッセージ
  MSG msg_{}; ///< Win32 メッセージ構造体
};
