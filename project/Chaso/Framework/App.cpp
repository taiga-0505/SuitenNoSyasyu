#include "App.h"
#include "RC.h"
#include "SceneManager.h"
#include "Audio/AudioEngine.h"
#include "../Editor/DebugBridge.h"
#include "Common/ResourcePath.h"
#include <cassert>
#include <chrono>
#include <format>
#include <fstream>
#include <nlohmann/json.hpp>

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3d12.lib")
#include <dxgidebug.h>
#include <wrl.h>
inline void ReportLiveObjectsDbg(const char *tag) {
  using Microsoft::WRL::ComPtr;
  OutputDebugStringA(
      ("==== LIVE REPORT @" + std::string(tag) + " ====\n").c_str());

  // Report DXGI
  ComPtr<IDXGIDebug> dxgiDbg;
  if (SUCCEEDED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&dxgiDbg)))) {
    dxgiDbg->ReportLiveObjects(DXGI_DEBUG_ALL, DXGI_DEBUG_RLO_ALL);
  }
}

App::App() {}
App::~App() {}

bool App::Init() {
  auto totalStart = std::chrono::high_resolution_clock::now();
  Log::Print("[App] 初期化開始...");

#if defined(_DEBUG)
  Log::Print("[App] Build Configuration: Debug");
#elif defined(RC_DEVELOPMENT)
  Log::Print("[App] Build Configuration: Development");
#else
  Log::Print("[App] Build Configuration: Release");
#endif

  auto stepStart = std::chrono::high_resolution_clock::now();

  // 設定のロード
  LoadAppConfig();

  // Window
  window_ = std::make_unique<Window>();
  window_->Initialize(appConfig_.title.c_str(), appConfig_.width, appConfig_.height, appConfig_.fullscreen);

  auto now = std::chrono::high_resolution_clock::now();
  Log::Print(std::format("[App] Window 生成完了 (Time: {:.3f}ms)", std::chrono::duration<float, std::milli>(now - stepStart).count()));
  stepStart = now;

  // DX12 Core
  coreDesc_.width = appConfig_.width;
  coreDesc_.height = appConfig_.height;
  core_.Init(window_->GetHwnd(), coreDesc_);
  cl_ = core_.CL();
  device_ = core_.GetDevice();
  assert(device_);

  now = std::chrono::high_resolution_clock::now();
  Log::Print(std::format("[App] DirectX12 Core 初期化完了 (Time: {:.3f}ms)", std::chrono::duration<float, std::milli>(now - stepStart).count()));
  stepStart = now;

  // Input
  input_ = std::make_unique<Input>(window_->GetHwnd());

  now = std::chrono::high_resolution_clock::now();
  Log::Print(std::format("[App] Input 初期化完了 (Time: {:.3f}ms)", std::chrono::duration<float, std::milli>(now - stepStart).count()));
  stepStart = now;

  // ImGui
  imgui_.Init(window_->GetHwnd(), core_);

  now = std::chrono::high_resolution_clock::now();
  Log::Print(std::format("[App] ImGui 初期化完了 (Time: {:.3f}ms)", std::chrono::duration<float, std::milli>(now - stepStart).count()));
  stepStart = now;

  // PipelineManager
  pm_.Init(device_, coreDesc_.rtvFormat, coreDesc_.dsvFormat);
  pm_.RegisterDefaultPipelines();

  now = std::chrono::high_resolution_clock::now();
  Log::Print(std::format("[App] PipelineManager 初期化完了(Time: {:.3f}ms)", std::chrono::duration<float, std::milli>(now - stepStart).count()));
  stepStart = now;

  // PostProcess
  // オフスクリーンレンダリング用のテクスチャを初期化（PSOと同じRTVフォーマットを使用）
  renderTexture_.Initialize(&core_, appConfig_.width, appConfig_.height, coreDesc_.rtvFormat);
  viewportTexture_.Initialize(&core_, appConfig_.width, appConfig_.height, coreDesc_.rtvFormat);
  postProcess_ = std::make_unique<PostProcess>();
  postProcess_->Initialize(&core_, &pm_, appConfig_.width, appConfig_.height);

  now = std::chrono::high_resolution_clock::now();
  Log::Print(std::format("[App] PostProcess 初期化完了 (Time: {:.3f}ms)", std::chrono::duration<float, std::milli>(now - stepStart).count()));
  stepStart = now;

  // SceneContext / RC
  sceneCtx_.core = &core_;
  sceneCtx_.input = input_.get();
  sceneCtx_.app = &appConfig_;
  sceneCtx_.imgui = (RC_ENABLE_IMGUI ? &imgui_ : nullptr);
  sceneCtx_.pipelineManager = &pm_;
  sceneCtx_.postProcess = postProcess_.get();
  RC::Init(sceneCtx_);

  // EditorManagerはTextureをロードするため、RC::Initの後に初期化する
  editorManager_.Initialize();

  now = std::chrono::high_resolution_clock::now();
  Log::Print(std::format("[App] RenderContext 初期化完了 (Time: {:.3f}ms)", std::chrono::duration<float, std::milli>(now - stepStart).count()));
  stepStart = now;

  // Audio（初期シーンのロードで AudioSourceComponent がクリップを読むため、Game より先に）
  // オーディオデバイスが無い環境では false が返るが、その場合は無音で続行する
  AudioEngine::Get().Init();

  // Game (Initial Scene Load) — 起動シーンは Resources/Setting/AppConfig.json の "bootScene"
  game_.SetBootScene(appConfig_.bootScene);
  game_.Init(sceneCtx_);

  // 撮影モード（F9）からシーンを飛ばせるようにする。
  // Scene からは SceneManager へ辿れないため、ここで一度だけ口を渡す。
  DebugBridge::SetSceneRequest(
      [this](const std::string &name) { game_.RequestChange(name); });

  auto totalEnd = std::chrono::high_resolution_clock::now();
  Log::Print(std::format("[App] 初期化完了 (Total Time: {:.3f}s)", std::chrono::duration<float>(totalEnd - totalStart).count()));

  return true;
}

int App::Run() {
  // ====================
  // Main Loop
  // ====================
  auto prevTime = std::chrono::high_resolution_clock::now();

  // メッセージループ
  while (msg_.message != WM_QUIT) {
    if (PeekMessage(&msg_, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg_);
      DispatchMessage(&msg_);
    } else {
#if RC_ENABLE_IMGUI
      // 解像度変更のリクエストを処理（GPUとコマンドリストが安全な状態で行う）
      auto resizeReq = editorManager_.GetResizeRequest();
      if (resizeReq.pending) {
          ResizeWindow(resizeReq.width, resizeReq.height, resizeReq.fullscreen);
          editorManager_.ClearResizeRequest();
      }
#endif

      auto currentTime = std::chrono::high_resolution_clock::now();
      sceneCtx_.deltaTime = std::chrono::duration<float>(currentTime - prevTime).count();
      if (sceneCtx_.deltaTime > 0.1f) sceneCtx_.deltaTime = 0.1f; // clamp delta time
      prevTime = currentTime;

#if RC_ENABLE_IMGUI
      // ImGui フレーム開始
      imgui_.NewFrame();

      // エディタのUI構築（DockSpace, MenuBarなど）
      editorManager_.Update(&core_, [this]() {
          // ゲーム（シーン）のUIをMenuBarの中（Windowの右）に構築する
          game_.DrawDebugUI(sceneCtx_);
      }, game_.GetCurrentScene());

      // 前フレームのViewportホバー状態を入力クラスに伝達
      input_->SetViewportHovered(editorManager_.IsViewportHovered());

      // プレイ状態の同期
      PlayState currentPlayState = editorManager_.GetPlayState();

      if (editorManager_.IsRestartRequested()) {
          game_.RestoreCurrentScene(sceneCtx_);
          editorManager_.ClearRestartRequest();
          // リスタートしたら必ず再生状態になるようにする
          currentPlayState = PlayState::Playing;
          sceneCtx_.playState = PlayState::Playing;
      } else {
          if (sceneCtx_.playState != currentPlayState) {
              if (currentPlayState == PlayState::Playing && sceneCtx_.playState == PlayState::Stopped) {
                  // 停止中から再生開始した瞬間：現在のシーンのバックアップを取る
                  game_.BackupCurrentScene();
                  if (sceneCtx_.camera) {
                      sceneCtx_.camera->SetUseDebug(false);
                  }
              } else if (currentPlayState == PlayState::Stopped) {
                  // 停止された場合、バックアップからシーンを復元する
                  game_.RestoreCurrentScene(sceneCtx_);
                  // 鳴り残っている SE / BGM も止めて、編集モードは無音から始める
                  AudioEngine::Get().StopAll();
              }
              sceneCtx_.playState = currentPlayState;
          }
      }
#endif

      // 更新
      Update();

      // ポストプロセスの時間更新
      if (postProcess_) {
        postProcess_->UpdateTime(sceneCtx_.deltaTime);
      }

      // 描画
      core_.BeginFrame();

      // --- オフスクリーンレンダリング開始 ---
      renderTexture_.TransitionToRenderTarget(cl_);

      // RenderTextureをクリアしてセット（青）
      float clearColor[] = {0.1f, 0.25f, 0.5f, 1.0f};
      cl_->ClearRenderTargetView(renderTexture_.GetRTV(), clearColor, 0, nullptr);

      // 描画先をRenderTextureに、深度バッファはそのまま
      D3D12_CPU_DESCRIPTOR_HANDLE rtv = renderTexture_.GetRTV();
      D3D12_CPU_DESCRIPTOR_HANDLE dsv = core_.Dsv();
      cl_->OMSetRenderTargets(1, &rtv, FALSE, &dsv);

      sceneCtx_.currentRTV = rtv;
      sceneCtx_.currentDSV = dsv;
      sceneCtx_.currentColorResource = renderTexture_.GetResource().Get();

      Render();

      // --- オフスクリーンレンダリング終了 ---
      renderTexture_.TransitionToShaderResource(cl_);

      // 描画先をバックバッファ（画面）に戻す
      D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv = core_.CurrentRTV();
      cl_->OMSetRenderTargets(1, &backBufferRtv, FALSE, nullptr);

      // ビューポートとシザー矩形をバックバッファサイズに再設定（必須）
      core_.ResetViewportScissorToBackbuffer(appConfig_.width, appConfig_.height);

#if !RC_ENABLE_IMGUI
      // 通常モード：ポストプロセスをバックバッファに転送
      postProcess_->Draw(cl_, renderTexture_);

      // ポストプロセス後のオーバーレイ（ポーズメニューなど、画面効果を受けない UI）。
      // PostProcess::Draw が最終出力先（バックバッファ）をレンダーターゲットにしたまま戻るので、
      // そのまま上に描く。2D パイプラインは深度を使わないので DSV は要らない。
      game_.RenderOverlay(sceneCtx_, cl_);
#endif

#if RC_ENABLE_IMGUI
      // エディタモード：ポストプロセスの出力を viewportTexture_ に書き込む
      postProcess_->Draw(cl_, renderTexture_, &viewportTexture_);

      // ポストプロセス後のオーバーレイ（viewportTexture_ がレンダーターゲットのまま）
      game_.RenderOverlay(sceneCtx_, cl_);

      // Viewport 描画用にSRV状態へ遷移
      viewportTexture_.TransitionToShaderResource(cl_);

      // Particle Preview 描画
      editorManager_.RenderParticlePreview(cl_, &core_, &pm_, sceneCtx_.deltaTime);

      // PostProcess等でOMSetRenderTargetsが変更されているため、バックバッファに戻す
      D3D12_CPU_DESCRIPTOR_HANDLE backRtv = core_.CurrentRTV();
      cl_->OMSetRenderTargets(1, &backRtv, FALSE, &dsv);
      core_.ResetViewportScissorToBackbuffer(appConfig_.width, appConfig_.height);

      // エディタの各パネル描画（Viewport含む）
      editorManager_.DrawUI(viewportTexture_.GetSRVGPU(), &core_, &pm_, sceneCtx_.deltaTime, game_.GetCurrentScene());

      // ImGui 描画
      imgui_.Render(cl_);
#endif
      core_.EndFrame();
    }
  }
  return static_cast<int>(msg_.wParam);
}

void App::Update() {

#if RC_ENABLE_IMGUI
  // ====================
  // Debug UI
  // ====================
  // デバッグUI描画 (App::RunのeditorManager_.Updateの前に移動しました)
#endif

  // ====================
  // Input
  // ====================
  // 入力更新
  input_->Update();

  // ====================
  // Game
  // ====================
  // ゲーム更新
  game_.Update(sceneCtx_);

  // ====================
  // Audio
  // ====================
  // 鳴り終わった SE の回収と、持ち主のいない BGM の停止。
  // シーン遷移（OnExit → OnEnter → 新シーンの Update）が同じフレーム内で終わった後に
  // 呼ばれるので、次のシーンが同じ BGM を要求していれば途切れない。
  AudioEngine::Get().Update();
}

void App::Render() {
  // ====================
  // Game Render
  // ====================
  // 選択中のエンティティIDをシーンに伝達（ギズモ描画用）
  if (auto* scene = game_.GetCurrentScene()) {
    scene->SetSelectedEntityId(editorManager_.GetSelectedEntityId());
  }
  // ゲーム描画
  game_.Render(sceneCtx_, cl_);
}

void App::Term() {
#if RC_ENABLE_IMGUI
  // エディタ設定の保存
  editorManager_.SaveConfig();
#endif

  // ====================
  // GPU Wait
  // ====================
  // GPU 完了待ち
  core_.WaitForGPU();

  // ====================
  // Editor
  // ====================
  editorManager_.Term();

  // ====================
  // Game
  // ====================
  // ゲーム終了
  game_.Term();

  // ====================
  // Audio
  // ====================
  // シーン（エンティティ）の破棄が終わってから、全ボイスとクリップを解放する
  AudioEngine::Get().Term();

  // ====================
  // Render Layer
  // ====================
  // RenderCommon 終了
  RC::Term();

  // ====================
  // Pipeline / ImGui / RenderObjects
  // ====================
  // 追加リソースを解放
  renderTexture_.Release();
  viewportTexture_.Release();
  postProcess_.reset();

  // パイプラインと ImGui 終了
  pm_.Term();
  imgui_.Shutdown();

  // ====================
  // Core
  // ====================
  // コア終了
  core_.Term();
  device_ = nullptr;
  cl_ = nullptr;

  // ====================
  // Input / Window
  // ====================
  // 入力とウィンドウ破棄
  input_.reset();
  window_.reset();

  ReportLiveObjectsDbg("FINAL Report at the end of App::Term");
}

/// @brief ビルド構成名（AppConfig.json の構成別キーに使う）
static const char *BuildConfigName_() {
#if defined(RC_DEVELOPMENT)
  return "Development";
#elif defined(_DEBUG)
  return "Debug";
#else
  return "Release";
#endif
}

/// @brief 文字列 or 構成別オブジェクトの値を読む
/// @details "bootScene": "Title"                                   → 全構成で "Title"
///          "bootScene": { "Debug": "Sample", "Development": "Stage1", "Release": "Title" }
///                                                                   → 構成ごとに切り替え（無いキーは "Release" → 既定値の順）
static std::string ReadPerConfigString_(const nlohmann::json &j, const char *key, const std::string &fallback) {
  if (!j.contains(key)) return fallback;
  const auto &v = j[key];
  if (v.is_string()) return v.get<std::string>();
  if (v.is_object()) {
    const char *cfg = BuildConfigName_();
    if (v.contains(cfg) && v[cfg].is_string()) return v[cfg].get<std::string>();
    if (v.contains("Release") && v["Release"].is_string()) return v["Release"].get<std::string>();
  }
  return fallback;
}

void App::LoadAppConfig() {
  bool loaded = false;
  std::ifstream ifs(Chaso::PrepareSettingPath(Chaso::kAppConfigPath));
  if (ifs) {
    try {
      nlohmann::json j;
      ifs >> j;
      if (j.contains("width")) appConfig_.width = j["width"];
      if (j.contains("height")) appConfig_.height = j["height"];
      if (j.contains("fullscreen")) appConfig_.fullscreen = j["fullscreen"];
      appConfig_.title = ReadPerConfigString_(j, "title", appConfig_.title);
      appConfig_.bootScene = ReadPerConfigString_(j, "bootScene", appConfig_.bootScene);
      loaded = true;
    } catch (...) {
      Log::Print("[App] Failed to parse AppConfig.json");
    }
  }

#if defined(_DEBUG) || defined(RC_DEVELOPMENT)
  // Debug / Development ビルドは AppConfig.json の値に関わらず常にフルスクリーンで起動する
  // （実際のサイズは Window::Initialize がモニター解像度に合わせて決定する）
  appConfig_.fullscreen = true;
#endif

  Log::Print(std::format("[App] {} AppConfig ({}): {}x{} Fullscreen:{} Title:\"{}\" BootScene:\"{}\"",
                         loaded ? "Loaded" : "Default", BuildConfigName_(),
                         appConfig_.width, appConfig_.height, appConfig_.fullscreen,
                         appConfig_.title, appConfig_.bootScene));
}

void App::SaveAppConfig() {
  // title / bootScene（構成別オブジェクトのことがある）など他のキーを壊さないよう、
  // 既存の JSON を読み込んでからウィンドウ設定だけ上書きする
  nlohmann::json j = nlohmann::json::object();
  {
    std::ifstream in(Chaso::PrepareSettingPath(Chaso::kAppConfigPath));
    if (in) {
      try { in >> j; } catch (...) { j = nlohmann::json::object(); }
      if (!j.is_object()) j = nlohmann::json::object();
    }
  }
  j["width"] = appConfig_.width;
  j["height"] = appConfig_.height;
  j["fullscreen"] = appConfig_.fullscreen;

  std::ofstream ofs(Chaso::kAppConfigPath);
  if (ofs) {
    ofs << j.dump(4);
    Log::Print("[App] Saved AppConfig.json");
  }
}

void App::ResizeWindow(int width, int height, bool fullscreen) {
  if (appConfig_.width == width && appConfig_.height == height && appConfig_.fullscreen == fullscreen) {
    return;
  }

  appConfig_.width = width;
  appConfig_.height = height;
  appConfig_.fullscreen = fullscreen;

  // GPU Wait
  core_.WaitForGPU();

  // 1. Window Resize
  window_->Resize(appConfig_.width, appConfig_.height, appConfig_.fullscreen);

  // 2. Save Config
  SaveAppConfig();

  // 3. Core Resize
  coreDesc_.width = appConfig_.width;
  coreDesc_.height = appConfig_.height;
  core_.Resize(appConfig_.width, appConfig_.height);

  // 4. RenderTexture Resize (ディスクリプタを再利用して再初期化)
  renderTexture_.Initialize(&core_, appConfig_.width, appConfig_.height, coreDesc_.rtvFormat);
  viewportTexture_.Initialize(&core_, appConfig_.width, appConfig_.height, coreDesc_.rtvFormat);

  // 5. PostProcess Resize
  if (postProcess_) {
    postProcess_->Resize(appConfig_.width, appConfig_.height);
  }
}
