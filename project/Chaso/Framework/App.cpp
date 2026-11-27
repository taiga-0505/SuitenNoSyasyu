#include "App.h"
#include "RC.h"
#include "SceneManager.h"
#include "Audio/AudioEngine.h"
#include "../Editor/DebugBridge.h"
#include "Common/ResourcePath.h"
#include "Common/FrameProfiler.h"
#include "Common/GameTime.h"
#include "Input/InputActions.h"
#include "Render/RenderContext.h"
#include "Graphics/Texture/TextureManager/TextureManager.h"
#include <cassert>
#include <chrono>
#include <format>
#include <fstream>
#include <nlohmann/json.hpp>

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3d12.lib")
#include <dxgidebug.h>
#include <wrl.h>
/// @brief 生き残っている DXGI / D3D12 オブジェクトをデバッグ出力へ報告する（解放漏れの確認用）
/// @param tag 報告の見出しに付ける文字列
inline void ReportLiveObjectsDbg(const char *tag) {
  using Microsoft::WRL::ComPtr;
  OutputDebugStringA(
      ("==== LIVE REPORT @" + std::string(tag) + " ====\n").c_str());

  // DXGI の生存オブジェクトを報告する
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

  // ウィンドウ
  window_ = std::make_unique<Window>();
  window_->Initialize(appConfig_.title.c_str(), appConfig_.width, appConfig_.height, appConfig_.fullscreen);

  auto now = std::chrono::high_resolution_clock::now();
  Log::Print(std::format("[App] Window 生成完了 (Time: {:.3f}ms)", std::chrono::duration<float, std::milli>(now - stepStart).count()));
  stepStart = now;

  // DX12 コア
  coreDesc_.width = appConfig_.width;
  coreDesc_.height = appConfig_.height;
  core_.Init(window_->GetHwnd(), coreDesc_);
  cl_ = core_.CL();
  device_ = core_.GetDevice();
  assert(device_);

  now = std::chrono::high_resolution_clock::now();
  Log::Print(std::format("[App] DirectX12 Core 初期化完了 (Time: {:.3f}ms)", std::chrono::duration<float, std::milli>(now - stepStart).count()));
  stepStart = now;

  // 入力
  input_ = std::make_unique<Input>(window_->GetHwnd());

  now = std::chrono::high_resolution_clock::now();
  Log::Print(std::format("[App] Input 初期化完了 (Time: {:.3f}ms)", std::chrono::duration<float, std::milli>(now - stepStart).count()));
  stepStart = now;

  // ImGui の初期化
  imgui_.Init(window_->GetHwnd(), core_);

  now = std::chrono::high_resolution_clock::now();
  Log::Print(std::format("[App] ImGui 初期化完了 (Time: {:.3f}ms)", std::chrono::duration<float, std::milli>(now - stepStart).count()));
  stepStart = now;

  // パイプライン管理（PipelineManager）
  // HDR のときはシーン（とすべてのパイプラインの描き込み先）を浮動小数点にし、
  // 画面（スワップチェーン・Viewport）は従来の sRGB のまま。最後のコピーだけが画面形式で書く。
  const DXGI_FORMAT sceneFormat = appConfig_.hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : coreDesc_.rtvFormat;
  pm_.Init(device_, sceneFormat, coreDesc_.dsvFormat, coreDesc_.rtvFormat);
  pm_.RegisterDefaultPipelines();

  now = std::chrono::high_resolution_clock::now();
  Log::Print(std::format("[App] PipelineManager 初期化完了(Time: {:.3f}ms)", std::chrono::duration<float, std::milli>(now - stepStart).count()));
  stepStart = now;

  // ポストプロセス
  // オフスクリーンレンダリング用のテクスチャを初期化（PSO と同じ RTV フォーマットを使用）
  // シーンの描画先はパイプラインと同じ形式（HDR なら浮動小数点）。Viewport は画面と同じ形式
  renderTexture_.Initialize(&core_, appConfig_.width, appConfig_.height, pm_.RtvFormat());
  viewportTexture_.Initialize(&core_, appConfig_.width, appConfig_.height, coreDesc_.rtvFormat);
  postProcess_ = std::make_unique<PostProcess>();
  postProcess_->SetHdr(appConfig_.hdr);
  postProcess_->Initialize(&core_, &pm_, appConfig_.width, appConfig_.height);

  now = std::chrono::high_resolution_clock::now();
  Log::Print(std::format("[App] PostProcess 初期化完了 (Time: {:.3f}ms)", std::chrono::duration<float, std::milli>(now - stepStart).count()));
  stepStart = now;

  // シーンコンテキスト（SceneContext）と RC の初期化
  sceneCtx_.core = &core_;
  sceneCtx_.input = input_.get();
  sceneCtx_.app = &appConfig_;
  sceneCtx_.imgui = (RC_ENABLE_IMGUI ? &imgui_ : nullptr);
  sceneCtx_.pipelineManager = &pm_;
  sceneCtx_.postProcess = postProcess_.get();
  RC::Init(sceneCtx_);

  // EditorManager はテクスチャをロードするため、RC::Init の後に初期化する
  editorManager_.Initialize();

  now = std::chrono::high_resolution_clock::now();
  Log::Print(std::format("[App] RenderContext 初期化完了 (Time: {:.3f}ms)", std::chrono::duration<float, std::milli>(now - stepStart).count()));
  stepStart = now;

  // Audio（初期シーンのロードで AudioSourceComponent がクリップを読むため、Game より先に）
  // オーディオデバイスが無い環境では false が返るが、その場合は無音で続行する
  AudioEngine::Get().Init();

  // ゲーム（初期シーンのロード）。起動シーンは Resources/Setting/AppConfig.json の "bootScene"
  game_.SetBootScene(appConfig_.bootScene);
  // キーコンフィグ（無ければゲーム側の InputActions::Define の既定値のまま）
  Chaso::InputActions::Get().Load(Chaso::kInputActionsPath);

  game_.Init(sceneCtx_);

  // 撮影モード（F9）からシーンを飛ばせるようにする。
  // Scene からは SceneManager へ辿れないため、ここで一度だけ口を渡す。
  DebugBridge::SetSceneRequest(
      [this](const std::string &name) { game_.RequestChange(name); });

  // 起動引数 --autotest: シーンを自動で一周して撮影し、終了する
  if (autoTest_.ParseCommandLine(appConfig_.bootScene)) {
#if RC_ENABLE_IMGUI
    // エディタは停止状態で起動するので、スクリプトが動くよう再生状態にしておく
    editorManager_.SetPlayState(PlayState::Playing);
    // シーン側は「停止中」から始め、最初のフレームで Play ボタンを押したときと同じ切り替え
    // （シーンのバックアップ・時間のリセット・デバッグカメラ → ゲームカメラ）を通す。
    // sceneCtx_.playState の初期値は Playing なので、そのままだと切り替えが起きず、
    // デバッグカメラの視点のまま撮影されていた。
    sceneCtx_.playState = PlayState::Stopped;
#endif
  }

  auto totalEnd = std::chrono::high_resolution_clock::now();
  Log::Print(std::format("[App] 初期化完了 (Total Time: {:.3f}s)", std::chrono::duration<float>(totalEnd - totalStart).count()));

  return true;
}

int App::Run() {
  // ====================
  // メインループ
  // ====================
  auto prevTime = std::chrono::high_resolution_clock::now();

  // メッセージループ
  while (msg_.message != WM_QUIT) {
    if (PeekMessage(&msg_, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg_);
      DispatchMessage(&msg_);
    } else {
#if RC_ENABLE_IMGUI
      // 解像度変更のリクエストを処理（GPU とコマンドリストが安全な状態で行う）
      auto resizeReq = editorManager_.GetResizeRequest();
      if (resizeReq.pending) {
          ResizeWindow(resizeReq.width, resizeReq.height, resizeReq.fullscreen);
          editorManager_.ClearResizeRequest();
      }
#endif

      auto currentTime = std::chrono::high_resolution_clock::now();
      sceneCtx_.deltaTime = std::chrono::duration<float>(currentTime - prevTime).count();
      if (sceneCtx_.deltaTime > 0.1f) sceneCtx_.deltaTime = 0.1f; // 経過時間は最大 0.1 秒に抑える
      prevTime = currentTime;

      // シェーダー・テクスチャのホットリロード（前のフレームの描画が終わった後、次を記録する前に行う）
      PollHotReload(sceneCtx_.deltaTime);

#if RC_ENABLE_IMGUI
      // ImGui フレーム開始
      imgui_.NewFrame();

      // エディタの UI 構築（DockSpace、MenuBar など）
      {
        CHASO_PROFILE_SCOPE("Editor: UI build");
        editorManager_.Update(&core_, [this]() {
            // ゲーム（シーン）の UI を MenuBar の中（Window メニューの右）に構築する
            game_.DrawDebugUI(sceneCtx_);
        }, game_.GetCurrentScene());
      }

      // 前フレームの Viewport ホバー状態を入力クラスに伝達
      input_->SetViewportHovered(editorManager_.IsViewportHovered());

      // プレイ状態の同期
      PlayState currentPlayState = editorManager_.GetPlayState();

      if (editorManager_.IsRestartRequested()) {
          game_.RestoreCurrentScene(sceneCtx_);
          editorManager_.ClearRestartRequest();
          Chaso::Time::Get().Reset(); // 前のプレイのスロー・ヒットストップを持ち越さない
          // リスタートしたら必ず再生状態になるようにする
          currentPlayState = PlayState::Playing;
          sceneCtx_.playState = PlayState::Playing;
      } else {
          if (sceneCtx_.playState != currentPlayState) {
              if (currentPlayState == PlayState::Playing && sceneCtx_.playState == PlayState::Stopped) {
                  // 停止中から再生開始した瞬間：現在のシーンのバックアップを取る
                  game_.BackupCurrentScene();
                  Chaso::Time::Get().Reset();
                  if (sceneCtx_.camera) {
                      sceneCtx_.camera->SetUseDebug(false);
                  }
              } else if (currentPlayState == PlayState::Stopped) {
                  // 停止された場合、バックアップからシーンを復元する
                  game_.RestoreCurrentScene(sceneCtx_);
                  Chaso::Time::Get().Reset();
                  // 鳴り残っている SE / BGM も止めて、編集モードは無音から始める
                  AudioEngine::Get().StopAll();
              }
              sceneCtx_.playState = currentPlayState;
          }
      }
#endif

      // 更新
      {
        CHASO_PROFILE_SCOPE("App: Update (input/scene/audio)");
        Update();
      }

      // 自動テスト（遷移の要求と、撮影の要求。撮影はこのフレームの EndFrame で行われる）
      if (autoTest_.IsEnabled()) {
        autoTest_.Update(game_);
        if (const std::string path = autoTest_.TakeCaptureRequest(); !path.empty()) {
#if RC_ENABLE_IMGUI
          // エディタ UI を含まないゲーム画面（ポストプロセス + オーバーレイ後）を撮る
          core_.RequestCapture(path, viewportTexture_.GetResource().Get(),
                               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
#else
          core_.RequestCapture(path);
#endif
        }
      }

      // ポストプロセスの時間更新
      if (postProcess_) {
        postProcess_->UpdateTime(sceneCtx_.deltaTime);
      }

      // 描画
      {
        // コマンドアロケータの再利用前に、2 フレーム前の GPU 完了を待つ（ここが唯一の GPU 待ち）
        CHASO_PROFILE_SCOPE("App: BeginFrame (wait GPU N-2)");
        core_.BeginFrame();
      }
      // フレームごとの一時バッファを切り替える（CPU と GPU を並行させるため 1 フレーム 1 回）
      RC::BeginFrame();

      // --- オフスクリーンレンダリング開始 ---
      renderTexture_.TransitionToRenderTarget(cl_);

      // RenderTexture をクリアしてセット（青）
      float clearColor[] = {0.1f, 0.25f, 0.5f, 1.0f};
      cl_->ClearRenderTargetView(renderTexture_.GetRTV(), clearColor, 0, nullptr);

      // 描画先を RenderTexture に、深度バッファはそのまま
      D3D12_CPU_DESCRIPTOR_HANDLE rtv = renderTexture_.GetRTV();
      D3D12_CPU_DESCRIPTOR_HANDLE dsv = core_.Dsv();
      cl_->OMSetRenderTargets(1, &rtv, FALSE, &dsv);

      sceneCtx_.currentRTV = rtv;
      sceneCtx_.currentDSV = dsv;
      sceneCtx_.currentColorResource = renderTexture_.GetResource().Get();

      {
        CHASO_PROFILE_SCOPE("App: Render (scene command recording)");
        Render();
      }

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

      // HDR のとき：トーンマップ済みの絵（とオーバーレイ）を画面へ写す（HDR でなければ何もしない）
      postProcess_->Resolve(cl_, nullptr);
#endif

#if RC_ENABLE_IMGUI
      // エディタモード：ポストプロセスの出力を viewportTexture_ に書き込む
      {
        CHASO_PROFILE_SCOPE("App: PostProcess");
        postProcess_->Draw(cl_, renderTexture_, &viewportTexture_);
      }

      // ポストプロセス後のオーバーレイ（viewportTexture_ がレンダーターゲットのまま）
      game_.RenderOverlay(sceneCtx_, cl_);

      // HDR のとき：トーンマップ済みの絵（とオーバーレイ）を viewportTexture_ へ写す（HDR でなければ何もしない）
      postProcess_->Resolve(cl_, &viewportTexture_);

      // Viewport 描画用に SRV 状態へ遷移
      viewportTexture_.TransitionToShaderResource(cl_);

      // パーティクルエディタのプレビュー描画
      editorManager_.RenderParticlePreview(cl_, &core_, &pm_, sceneCtx_.deltaTime);

      // PostProcess 等で OMSetRenderTargets が変更されているため、バックバッファに戻す
      D3D12_CPU_DESCRIPTOR_HANDLE backRtv = core_.CurrentRTV();
      cl_->OMSetRenderTargets(1, &backRtv, FALSE, &dsv);
      core_.ResetViewportScissorToBackbuffer(appConfig_.width, appConfig_.height);

      // エディタの各パネル描画（Viewport 含む）
      {
        CHASO_PROFILE_SCOPE("Editor: UI draw");
        editorManager_.DrawUI(viewportTexture_.GetSRVGPU(), &core_, &pm_, sceneCtx_.deltaTime, game_.GetCurrentScene());

        // ImGui 描画
        imgui_.Render(cl_);
      }
#endif
      {
        // Present・GPU 完了待ち・録画・FPS 固定の待ちを含む（CPU の処理量ではない）
        CHASO_PROFILE_SCOPE("App: EndFrame (present/wait)");
        core_.EndFrame();
      }
      FrameProfiler::Get().EndFrame();
    }
  }
  return static_cast<int>(msg_.wParam);
}

void App::Update() {

#if RC_ENABLE_IMGUI
  // ====================
  // デバッグ UI
  // ====================
  // デバッグ UI の描画は App::Run の editorManager_.Update の前に移動した
#endif

  // ====================
  // 入力
  // ====================
  // 入力更新
  input_->Update();

  // アクション入力（"Jump" などの論理名）を計算する
  {
    Chaso::InputSnapshot snap;
    for (int k = 0; k < 256; ++k) snap.keys[k] = input_->IsKeyPressed(static_cast<uint8_t>(k));
    for (int m = 0; m < 4; ++m) snap.mouse[m] = input_->IsMousePressed(m); // DIMOUSESTATE は 4 ボタン
    snap.mouseWheel = static_cast<int>(input_->GetMouseZ()); // ホイールの回転量（このフレーム分）
    for (const auto &[name, bit] : Chaso::InputBinding::PadButtonNames()) {
      if (input_->IsXInputButtonPressed(static_cast<WORD>(bit))) snap.padButtons |= static_cast<uint16_t>(bit);
    }
    // 未接続時は XInput の状態が 0 のままなので、IsConnected（毎回 XInputGetState する）は呼ばない
    const auto stick = [](SHORT v) { return (std::max)(-1.0f, static_cast<float>(v) / 32767.0f); };
    snap.padAxes[Chaso::InputBinding::LX] = stick(input_->GetXInputThumbLX());
    snap.padAxes[Chaso::InputBinding::LY] = stick(input_->GetXInputThumbLY());
    snap.padAxes[Chaso::InputBinding::RX] = stick(input_->GetXInputThumbRX());
    snap.padAxes[Chaso::InputBinding::RY] = stick(input_->GetXInputThumbRY());
    snap.padAxes[Chaso::InputBinding::LT] = static_cast<float>(input_->GetXInputLeftTrigger()) / 255.0f;
    snap.padAxes[Chaso::InputBinding::RT] = static_cast<float>(input_->GetXInputRightTrigger()) / 255.0f;
    Chaso::InputActions::Get().Update(snap);
  }

  // ====================
  // 時間
  // ====================
  // ゲーム時間（タイムスケール・ヒットストップ・スロー）を進める。
  // シーンの更新は Chaso::Time::Get().DeltaTime() を使う（sceneCtx_.deltaTime は実時間のまま）。
  Chaso::Time::Get().Tick(sceneCtx_.deltaTime, sceneCtx_.isSimulating(), sceneCtx_.isPlaying());

  // ====================
  // ゲーム
  // ====================
  // ゲーム更新
  game_.Update(sceneCtx_);

  // ====================
  // オーディオ
  // ====================
  // 鳴り終わった SE の回収と、持ち主のいない BGM の停止。
  // シーン遷移（OnExit → OnEnter → 新シーンの Update）が同じフレーム内で終わった後に
  // 呼ばれるので、次のシーンが同じ BGM を要求していれば途切れない。
  AudioEngine::Get().Update();
}

void App::PollHotReload(float deltaTime) {
#if RC_ENABLE_IMGUI
  if (!hotReloadReady_) {
    // シェーダーは実行時に読む Resources/Shader を見る（Chaso/Resources 側はビルド時に同期される）
    shaderWatcher_.AddDirectory("Resources/Shader", {".hlsl", ".hlsli"});
    shaderWatcher_.SetInterval(0.5f);
    // テクスチャは「今読み込んでいる物」だけを見る（Resources 全体を毎回なめない）
    textureWatcher_.SetFileProvider([this]() {
      std::vector<std::string> files;
      textureResolvedToKey_.clear();
      for (const auto &key : RC::GetRenderContext().Textures().LoadedPaths()) {
        const std::string resolved =
            std::filesystem::path(Chaso::ResolvePath(key)).lexically_normal().generic_string();
        textureResolvedToKey_[resolved] = key;
        files.push_back(resolved);
      }
      return files;
    });
    textureWatcher_.SetInterval(1.0f);
    hotReloadReady_ = true;
  }

  const std::vector<std::string> shaders = shaderWatcher_.Poll(deltaTime);
  const std::vector<std::string> textures = textureWatcher_.Poll(deltaTime);
  if (shaders.empty() && textures.empty()) return;

  // 描画中の PSO・SRV を書き換えないよう、GPU が前のフレームを終えるのを待つ
  core_.WaitForGPU();

  // .hlsli（共通ヘッダー）は全パイプラインを作り直すので、何個変わっても 1 回だけ
  bool headerDone = false;
  for (const auto &path : shaders) {
    const bool isHeader = std::filesystem::path(path).extension() == ".hlsli";
    if (isHeader) {
      if (headerDone) continue;
      headerDone = true;
    }
    pm_.ReloadShaderFile(path);
  }

  for (const auto &path : textures) {
    auto it = textureResolvedToKey_.find(path);
    RC::GetRenderContext().Textures().Reload(it != textureResolvedToKey_.end() ? it->second : path);
  }
#else
  (void)deltaTime;
#endif
}

void App::Render() {
  // ====================
  // ゲーム描画
  // ====================
  // 選択中のエンティティ ID をシーンに伝達（ギズモ描画用）
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
  // GPU 完了待ち
  // ====================
  // GPU 完了待ち
  core_.WaitForGPU();

  // ====================
  // エディタ
  // ====================
  editorManager_.Term();

  // ====================
  // ゲーム
  // ====================
  // ゲーム終了
  game_.Term();

  // ====================
  // オーディオ
  // ====================
  // シーン（エンティティ）の破棄が終わってから、全ボイスとクリップを解放する
  AudioEngine::Get().Term();

  // ====================
  // 描画レイヤー
  // ====================
  // RenderCommon 終了
  RC::Term();

  // ====================
  // パイプライン / ImGui / 描画リソース
  // ====================
  // 追加リソースを解放
  renderTexture_.Release();
  viewportTexture_.Release();
  postProcess_.reset();

  // パイプラインと ImGui 終了
  pm_.Term();
  imgui_.Shutdown();

  // ====================
  // コア
  // ====================
  // コア終了
  core_.Term();
  device_ = nullptr;
  cl_ = nullptr;

  // ====================
  // 入力 / ウィンドウ
  // ====================
  // 入力とウィンドウ破棄
  input_.reset();
  window_.reset();

  ReportLiveObjectsDbg("FINAL Report at the end of App::Term");
}

/// @brief ビルド構成名（AppConfig.json の構成別キーに使う）
/// @return "Development" / "Debug" / "Release" のいずれか
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
/// @param j        読み込んだ AppConfig.json
/// @param key      読むキー名
/// @param fallback キーが無い・型が合わないときに返す既定値
/// @return 現在のビルド構成に対応する文字列
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
      if (j.contains("hdr") && j["hdr"].is_boolean()) appConfig_.hdr = j["hdr"].get<bool>();
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

  Log::Print(std::format("[App] {} AppConfig ({}): {}x{} Fullscreen:{} HDR:{} Title:\"{}\" BootScene:\"{}\"",
                         loaded ? "Loaded" : "Default", BuildConfigName_(),
                         appConfig_.width, appConfig_.height, appConfig_.fullscreen, appConfig_.hdr,
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

  // GPU 完了待ち（使用中のリソースを作り直すため）
  core_.WaitForGPU();

  // 1. ウィンドウのリサイズ
  window_->Resize(appConfig_.width, appConfig_.height, appConfig_.fullscreen);

  // 2. 設定の保存
  SaveAppConfig();

  // 3. コアのリサイズ
  coreDesc_.width = appConfig_.width;
  coreDesc_.height = appConfig_.height;
  core_.Resize(appConfig_.width, appConfig_.height);

  // 4. RenderTexture のリサイズ（ディスクリプタを再利用して再初期化）
  renderTexture_.Initialize(&core_, appConfig_.width, appConfig_.height, pm_.RtvFormat());
  viewportTexture_.Initialize(&core_, appConfig_.width, appConfig_.height, coreDesc_.rtvFormat);

  // 5. PostProcess のリサイズ
  if (postProcess_) {
    postProcess_->Resize(appConfig_.width, appConfig_.height);
  }
}
