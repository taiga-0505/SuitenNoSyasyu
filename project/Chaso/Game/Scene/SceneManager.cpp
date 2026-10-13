#include "SceneManager.h"
#include "Scene.h"
#include "Common/Log/Log.h"
#include "Common/ResourcePath.h"
#include "RenderCommon.h"
#include "Fade/Fade.h"
#include "Camera/CameraController.h"
#include "DataDrivenScene/DataDrivenScene.h"
#include "SceneFlow.h"
#include <algorithm>
#include <chrono>
#include <format>
#include <cstdlib>
#include "Render/RenderContext.h"

namespace RC { class CameraController; }

class FadeOutState;
class FadeInState;
class GrayscaleIntroState;
class NormalState;
class LoadingState;

// For PostEffectType
#include "Graphics/PostProcess/PostProcess.h"

namespace {

/// @brief Dive 遷移で抜ける画面色の保存先
/// @details 関数内 static にしておくと、Application 側の静的初期化から
///          SetDiveScreenColor を呼ばれても初期化順の問題が起きない。
RC::Vector4 &DiveScreenColorStorage() {
  static RC::Vector4 color = {0.0f, 0.0f, 0.0f, 1.0f};
  return color;
}

/// @brief Ascend 遷移（空へ抜ける）で抜ける画面色。飛び出す側はこの白まで飛ばしてから要求する
constexpr RC::Vector4 kAscendScreenColor = {0.97f, 0.98f, 1.0f, 1.0f};

/// @brief 遷移演出ごとのフェード時間（秒）
float FadeTimeFor(SceneTransition transition) {
  switch (transition) {
  case SceneTransition::Dive: return Scene::SceneManager::kDiveFadeTime;
  case SceneTransition::Ascend: return Scene::SceneManager::kAscendFadeTime;
  default: return Scene::SceneManager::kFadeTime;
  }
}

/// @brief 切り替え後に GrayscaleIntro（白黒から色が戻る・その間ゲーム停止）を挟まない遷移か
/// @details Dive / Ascend は次のシーン側の演出（深海からの浮上・空からの降下）が
///          そのままオープニングになるので、色を抜かずにすぐシーンを動かす。
bool SkipsGrayscaleIntro(SceneTransition transition) {
  return transition == SceneTransition::Dive || transition == SceneTransition::Ascend;
}

const char *TransitionLogName(SceneTransition transition) {
  switch (transition) {
  case SceneTransition::Dive: return "dive";
  case SceneTransition::Ascend: return "ascend";
  case SceneTransition::Dissolve: return "dissolve";
  default: return "none";
  }
}

/// @brief Dissolve の「抜けた部分の色」と縁の色を遷移演出に合わせる
/// @details Dissolve のパラメータは PostProcess に残り続けるので、
///          どちらの遷移でも毎回明示的に設定する（前回の Dive の色を引きずらない）。
void ApplyDissolveLook(SceneTransition transition) {
  if (transition == SceneTransition::Dive) {
    // 飛び込み側は画面をこの色まで暗くしてから要求してくるので、
    // 同じ色へ抜けると継ぎ目が見えない。縁も光らせない（暗い水の中で橙は浮く）。
    const RC::Vector4 c = DiveScreenColorStorage();
    RC::SetDissolveBaseColor(c.x, c.y, c.z, 1.0f);
    RC::SetDissolveEdgeColor(c.x, c.y, c.z);
  } else if (transition == SceneTransition::Ascend) {
    // 飛び出す側は画面を白く飛ばしてから要求してくるので、同じ白へ抜ける
    const RC::Vector4 &c = kAscendScreenColor;
    RC::SetDissolveBaseColor(c.x, c.y, c.z, 1.0f);
    RC::SetDissolveEdgeColor(c.x, c.y, c.z);
  } else {
    RC::SetDissolveBaseColor(0.0f, 0.0f, 0.0f, 1.0f); // トランジションは黒で抜く
    RC::SetDissolveEdgeColor(1.0f, 0.4f, 0.3f);       // PostProcess の既定の縁色
  }
}

} // namespace

void Scene::SceneManager::SetDiveScreenColor(const RC::Vector4 &color) {
  DiveScreenColorStorage() = color;
}

const RC::Vector4 &Scene::SceneManager::DiveScreenColor() {
  return DiveScreenColorStorage();
}

// =================================================================
// 状態インタフェース
// =================================================================
class ISceneState {
public:
  virtual ~ISceneState() = default;
  virtual void Update(Scene::SceneManager &sm, SceneContext &ctx) = 0;
  virtual void Render(Scene::SceneManager &sm, SceneContext &ctx,
                      ID3D12GraphicsCommandList *cl) = 0;
};

// =================================================================
// 各状態クラスの定義
// =================================================================

class NormalState : public ISceneState {
public:
  void Update(Scene::SceneManager &sm, SceneContext &ctx) override;
  void Render(Scene::SceneManager &sm, SceneContext &ctx,
              ID3D12GraphicsCommandList *cl) override;
};

class FadeOutState : public ISceneState {
public:
  void Update(Scene::SceneManager &sm, SceneContext &ctx) override;
  void Render(Scene::SceneManager &sm, SceneContext &ctx,
              ID3D12GraphicsCommandList *cl) override;
private:
  float counter_ = 0.0f;
};

class LoadingState : public ISceneState {
public:
  void Update(Scene::SceneManager &sm, SceneContext &ctx) override;
  void Render(Scene::SceneManager &sm, SceneContext &ctx,
              ID3D12GraphicsCommandList *cl) override;
private:
  bool loaded_ = false;
};

class FadeInState : public ISceneState {
public:
  void Update(Scene::SceneManager &sm, SceneContext &ctx) override;
  void Render(Scene::SceneManager &sm, SceneContext &ctx,
              ID3D12GraphicsCommandList *cl) override;
private:
  float counter_ = 0.0f;
};

class GrayscaleIntroState : public ISceneState {
public:
  explicit GrayscaleIntroState(float duration = 1.8f) : duration_(duration) {}
  void Update(Scene::SceneManager &sm, SceneContext &ctx) override;
  void Render(Scene::SceneManager &sm, SceneContext &ctx,
              ID3D12GraphicsCommandList *cl) override;
private:
  float counter_ = 0.0f;
  float duration_ = 1.8f;
};

// =================================================================
// NormalState 実装
// =================================================================
void NormalState::Update(Scene::SceneManager &sm, SceneContext &ctx) {
  // 常にシーンの Update は呼ぶ（カメラ更新等を止めない）
  if (sm.current_) {
    sm.current_->Update(sm, ctx);
  }

  if (!sm.requested_.empty()) {
    Log::Print("[SceneState] NormalState -> FadeOutState (requested: " + sm.requested_ + ", " +
               TransitionLogName(sm.transition_) + ")");

    // Dissolveエフェクトを開始
    RC::AddPostEffect(PostEffectType::Dissolve);

    // 固定ノイズ (インデックス0) を使用する
    RC::SetDissolveNoiseIndex(0);

    RC::SetDissolveThreshold(0.0f);
    ApplyDissolveLook(sm.transition_); // Dissolve は黒、Dive は DiveScreenColor へ抜く

    sm.ChangeState(std::make_unique<FadeOutState>());
  }
}
void NormalState::Render(Scene::SceneManager &sm, SceneContext &ctx,
                         ID3D12GraphicsCommandList *cl) {
  if (sm.current_) {
    sm.current_->Render(ctx, cl);
  }
}

// =================================================================
// FadeOutState 実装
// =================================================================
void FadeOutState::Update(Scene::SceneManager &sm, SceneContext &ctx) {
  const float fadeTime = FadeTimeFor(sm.transition_);
  counter_ += 1.0f / 60.0f;
  if (counter_ > fadeTime) counter_ = fadeTime;

  float threshold = counter_ / fadeTime;
  RC::SetDissolveThreshold(threshold);

  // フェードアウト中は旧シーンの Update も継続
  if (sm.current_ && counter_ < fadeTime) {
    sm.current_->Update(sm, ctx);
  }

  if (counter_ >= fadeTime) {
    Log::Print("[SceneState] FadeOutState -> LoadingState");
    sm.ChangeState(std::make_unique<LoadingState>());
  }
}
void FadeOutState::Render(Scene::SceneManager &sm, SceneContext &ctx,
                          ID3D12GraphicsCommandList *cl) {
  // フェードアウト中は旧シーンを描画し続ける
  if (sm.current_) {
    sm.current_->Render(ctx, cl);
  } else {
    Log::Print("[SceneState] FadeOutState::Render - current_ is null!");
  }
}

// =================================================================
// LoadingState 実装
// - 画面が完全に黒い状態でシーン切り替えを行う
// - 切り替え完了後に FadeIn を開始する
// =================================================================
void LoadingState::Update(Scene::SceneManager &sm, SceneContext &ctx) {
  if (!loaded_) {
    // 画面が真っ黒（完全にDissolveされた状態）でシーン切り替え
    sm.ChangeImmediately(sm.requested_, ctx);
    loaded_ = true;

    // ChangeImmediately内でClearPostEffectsが呼ばれるため、再度Dissolveを適用
    RC::AddPostEffect(PostEffectType::Dissolve);
    RC::SetDissolveThreshold(1.0f);
    ApplyDissolveLook(sm.transition_);

    // シークエンシャルなオープニング演出準備：初期を白黒状態にセットして静止させる。
    // Dive / Ascend 遷移は次のシーン側の演出がそのままオープニングになるので挟まない。
    if (ctx.postProcess && !SkipsGrayscaleIntro(sm.transition_)) {
      ctx.postProcess->AddEffect(PostEffectType::Grayscale);
      ctx.postProcess->SetGrayscaleLerpFactor(1.0f);
    }

    // フェードインを開始（Dissolveから復帰）
    sm.ChangeState(std::make_unique<FadeInState>());

    // 新しいシーンの初期位置構成のため1回目だけUpdateを呼び、各オブジェクトの位置を決定
    if (sm.current_) {
      sm.current_->Update(sm, ctx);

      // この Update でスクリプトの OnCreate が走り、シーン側のポストエフェクト
      // （輪郭・水中など）が Dissolve の後ろに積まれる。Dissolve は「画面全体を
      // 隠す幕」なので、いったん外して末尾へ積み直し、必ず最後に掛かるようにする。
      // パラメータ（閾値・色）は PostProcess 側に残るので積み直しても消えない。
      if (RC::HasPostEffect(PostEffectType::Dissolve)) {
        RC::RemovePostEffect(PostEffectType::Dissolve);
        RC::AddPostEffect(PostEffectType::Dissolve);
      }
    }
  }
}
void LoadingState::Render(Scene::SceneManager &sm, SceneContext &ctx,
                          ID3D12GraphicsCommandList *cl) {
  // 画面は完全にDissolveされた状態
  RC::PreDraw3D(ctx, cl);
  RC::PreDraw2D(ctx, cl);
}

// =================================================================
// FadeInState 実装 (Dissolveがノイズより開ける期間：ゲーム進行は止めておく)
// =================================================================
void FadeInState::Update(Scene::SceneManager &sm, SceneContext &ctx) {
  const float fadeTime = FadeTimeFor(sm.transition_);
  counter_ += 1.0f / 60.0f;
  if (counter_ > fadeTime) counter_ = fadeTime;

  float threshold = 1.0f - (counter_ / fadeTime);
  RC::SetDissolveThreshold(threshold);

  // 【ゲーム完全停止】演出完了までシーン更新 (sm.current_->Update) を飛ばし、時間とアクションをフリーズ！

  if (counter_ >= fadeTime) {
    RC::RemovePostEffect(PostEffectType::Dissolve);
    if (SkipsGrayscaleIntro(sm.transition_)) {
      // 飛び込み・空へ抜ける遷移はここで終わり。深海／空に置かれたカメラを次のシーンの
      // スクリプト（DeepRiseIntroScript / ResultScreenScript など）が動かすので、すぐにシーンを動かし始める。
      Log::Print(std::string("[SceneState] FadeInState -> NormalState (") +
                 TransitionLogName(sm.transition_) + ")");
      sm.transition_ = SceneTransition::None;
      sm.ChangeState(std::make_unique<NormalState>());
      return;
    }
    // Dissolveが完全に明けきった後、静止したモノクロ世界にカラーが満ちる GrayscaleIntroState へバトンタッチ！
    sm.ChangeState(std::make_unique<GrayscaleIntroState>(1.8f));
  }
}
void FadeInState::Render(Scene::SceneManager &sm, SceneContext &ctx,
                         ID3D12GraphicsCommandList *cl) {
  // 静止したゲーム世界を美しく描画
  if (sm.current_) {
    sm.current_->Render(ctx, cl);
  } else {
    Log::Print("[SceneState] FadeInState::Render - current_ is null!");
  }
}

// =================================================================
// GrayscaleIntroState 実装 (モノクロからのシネマティックカラーライゼーション -> GAME START!!)
// =================================================================
void GrayscaleIntroState::Update(Scene::SceneManager &sm, SceneContext &ctx) {
  counter_ += 1.0f / 60.0f;
  if (counter_ > duration_) counter_ = duration_;

  // 白黒 (1.0f) から フルカラー (0.0f) へ滑らかに色彩復元
  float progress = counter_ / duration_;
  if (ctx.postProcess) {
    ctx.postProcess->SetGrayscaleLerpFactor(1.0f - progress);
  }

  // 【引き続きゲーム完全停止】色彩が蘇るまで敵の行動や射撃・移動操作を一時留保！

  if (counter_ >= duration_) {
    if (ctx.postProcess) {
      ctx.postProcess->RemoveEffect(PostEffectType::Grayscale);
    }
    Log::Print("[SceneState] All intro sequences complete! GAME START!");
    // すべてのオープニングの終わり、かつゲーム本番（NormalState）への覚醒！ここから時が動き始める！
    sm.transition_ = SceneTransition::None;
    sm.ChangeState(std::make_unique<NormalState>());
  }
}
void GrayscaleIntroState::Render(Scene::SceneManager &sm, SceneContext &ctx,
                                 ID3D12GraphicsCommandList *cl) {
  if (sm.current_) {
    sm.current_->Render(ctx, cl);
  } else {
    Log::Print("[SceneState] GrayscaleIntroState::Render - current_ is null!");
  }
}

// =================================================================
// SceneManager 実装
// =================================================================

Scene::SceneManager::SceneManager() = default;
Scene::SceneManager::~SceneManager() = default;

void Scene::SceneManager::Init(SceneContext &ctx) {
  float width = float(ctx.app->width);
  float height = float(ctx.app->height);

  // エディタカメラの初期化（全シーンで共有）
  camera_ = std::make_unique<RC::CameraController>();
  camera_->Initialize(ctx.input, RC::Vector3{0.0f, 0.35f, -15.0f},
                      RC::Vector3{0, 0, 0}, 0.45f, width / height, 0.1f, 100.0f);
#if RC_ENABLE_IMGUI
  camera_->SetUseDebug(true);
#else
  camera_->SetUseDebug(false);
#endif
  ctx.camera = camera_.get();

  // スクリプト（ScriptableEntity）からのシーン遷移要求の受け口を結線する。
  // SceneContext は App::sceneCtx_ の実体を通しで使うため、ここで一度差すだけでよい。
  // RequestChange のみを公開し、ChangeImmediately は意図的に渡さない。
  // ChangeImmediately をスクリプトの OnUpdate から呼ぶと OnExit ～ エンティティ破棄が
  // その場で走り、呼び出し元スクリプト自身が解放されて use-after-free になる。
  ctx.requestSceneChange = [this](const std::string &name, SceneTransition transition) {
    return RequestChange(name, transition);
  };

  // 遷移表（SceneFlow）のエディタが、登録済みシーンの一覧・いまのシーン・
  // 「試す」ボタンの遷移要求を使えるように結線する。
  auto &flow = SceneFlow::Get();
  flow.SetSceneNameProvider([this]() { return GetSceneNames(); });
  flow.SetCurrentSceneProvider([this]() { return currentName_; });
  flow.SetRequestFn([this](const std::string &name, SceneTransition transition) {
    return RequestChange(name, transition);
  });

  // Fadeコンポーネントを初期化
  fade_ = std::make_unique<Fade>();
  fade_->Init(ctx, width, height);

  // 初期状態をセット
  ChangeState(std::make_unique<NormalState>());
}

void Scene::SceneManager::Term() {
  // this を捕まえた結線を外す（SceneFlow はシングルトンなので SceneManager より長生きする）
  auto &flow = SceneFlow::Get();
  flow.SetSceneNameProvider(nullptr);
  flow.SetCurrentSceneProvider(nullptr);
  flow.SetRequestFn(nullptr);

  if (fade_) {
    fade_.reset();
  }
  scenes_.clear();
  current_ = nullptr;
  currentName_.clear();
  requested_.clear();
  state_.reset();
}

void Scene::SceneManager::Register(std::unique_ptr<Scene> scene) {
  const std::string key = scene->Name();
  scenes_[key] = std::move(scene);
}

bool Scene::SceneManager::RequestChange(const std::string &name,
                                        SceneTransition transition) {
  if (name.empty()) {
    Log::Print("[SceneManager] RequestChange: シーン名が空のため無視しました");
    return false;
  }

  // 未登録のシーン名を通すと ChangeImmediately で current_ が nullptr になり、
  // 各 State の Render が「current_ is null!」を吐き続けるだけの黒画面になる。
  // 原因の切り分けが難しいので、要求の時点で弾いて登録済み一覧をログに出す。
  if (scenes_.find(name) == scenes_.end()) {
    std::string names;
    for (auto &[key, _] : scenes_) {
      if (!names.empty()) names += ", ";
      names += key;
    }
    Log::Print("[SceneManager] RequestChange: 未登録のシーン名です: " + name);
    Log::Print("[SceneManager] 登録済みシーン: " + names);
    return false;
  }

  // 既に処理待ちの要求があるなら後から来たものは捨てる。
  // requested_ は ChangeImmediately までクリアされず、FadeOutState は旧シーンの
  // Update を回し続けるため、フェード中にスクリプトが行き先を書き換えられてしまう。
  // requested_ がクリアされたあと（LoadingState / FadeInState）もフェード演出は
  // 続いているので、state_ が NormalState 以外なら「遷移中」として弾く。
  const bool inTransition =
      !requested_.empty() ||
      (state_ && dynamic_cast<NormalState *>(state_.get()) == nullptr);
  if (inTransition) {
    Log::Print("[SceneManager] RequestChange: 遷移中のため無視しました (処理中: " +
               (requested_.empty() ? currentName_ : requested_) + " / 要求: " + name + ")");
    return false;
  }

  // 同名シーンへの要求は通す。OnExit -> OnEnter が走るため「リトライ」として機能する。

  requested_ = name;
  // None は「演出なし」の意味なので要求としては受けない（既定の Dissolve に丸める）
  transition_ = (transition == SceneTransition::None) ? SceneTransition::Dissolve : transition;
  return true;
}

void Scene::SceneManager::ChangeImmediately(const std::string &name,
                                            SceneContext &ctx) {
  Log::Print("[Scene] シーン切り替え: " + (currentName_.empty() ? "None" : currentName_) + " -> " + name);
  
  RC::ClearPostEffects();

  auto start = std::chrono::high_resolution_clock::now();

  if (current_) {
    current_->OnExit(ctx);
  }
  // 次のシーンが「どう入って来たか」を OnEnter より前に知らせる。
  // 演出付きの要求（RequestChange）以外は None（起動直後・エディタからの切り替え）。
  ctx.lastTransition = transition_;
  current_ = get_(name);
  if (current_) {
    current_->OnEnter(ctx);
    // ロード完了を自動待機（ユーザーが WaitAllLoads を書かなくても済むように）
    RC::WaitAllLoads();
  }
  currentName_ = name;
  requested_.clear();

  auto end = std::chrono::high_resolution_clock::now();
  std::chrono::duration<double> elapsed = end - start;
  Log::Print(std::format("[Scene] Load time: {:.4f} sec", elapsed.count()));

  requested_.clear();

  // 新しいシーンでのテクスチャログ出力を許可するためにリセット
  RC::ClearTextureLogHistory();

  // シーン切り替え直後のフレームで描画コマンドの順序をダンプする
  RC::GetRenderContext().RequestDumpCommandOrder();
}

void Scene::SceneManager::ReloadCurrentScene(SceneContext &ctx) {
  if (current_) {
    Log::Print("[Scene] Reloading scene: " + currentName_);
    RC::ClearPostEffects();
    current_->OnExit(ctx);
    current_->OnEnter(ctx);
    RC::WaitAllLoads();
    RC::ClearTextureLogHistory();
  }
}

void Scene::SceneManager::BackupCurrentScene() {
    if (current_) {
        // dynamic_cast checking if it's DataDrivenScene
        if (auto* dds = dynamic_cast<DataDrivenScene*>(current_)) {
            dds->BackupState();
            Log::Print("[Scene] Backup created for: " + currentName_);
        }
    }
}

void Scene::SceneManager::RestoreCurrentScene(SceneContext &ctx) {
    if (current_) {
        if (auto* dds = dynamic_cast<DataDrivenScene*>(current_)) {
            Log::Print("[Scene] Restoring scene from backup: " + currentName_);
            RC::ClearPostEffects();
            dds->RestoreState(ctx);
        }
    }
}

void Scene::SceneManager::Update(SceneContext &ctx) {
  // 常にFadeの更新は行う
  if (fade_) {
    fade_->Update();
  }

  // 現在の状態に更新処理を委譲（シーンのUpdate内でカメラモード切替等が行われる）
  if (state_) {
    state_->Update(*this, ctx);
  }

  // オーディオは状態に関係なく毎フレーム更新する。
  // 画面が完全に Dissolve されている間はシーンの Update を呼ばないが、
  // その間も BGM の keep-alive を送り続けないと BGM が止まってしまう。
  // ChangeImmediately（OnExit → OnEnter）と同じフレームで呼ばれるので、
  // 次のシーンが同じ BGM を持っていれば途切れずに引き継がれる。
  if (current_) {
    current_->UpdateAudio(ctx);
  }

  // カメラ更新 & ビュー/プロジェクション反映（全シーン共通）
  if (camera_) {
    camera_->Update();
    RC::SetCamera(camera_->GetView(), camera_->GetProjection(), camera_->GetWorldPos());
  }
}

void Scene::SceneManager::Render(SceneContext &ctx,
                                 ID3D12GraphicsCommandList *cl) {
  // 現在の状態に描画処理を委譲
  if (state_) {
    state_->Render(*this, ctx, cl);
  }
}

void Scene::SceneManager::RenderOverlay(SceneContext &ctx,
                                        ID3D12GraphicsCommandList *cl) {
  if (current_) {
    current_->RenderOverlay(ctx, cl);
  }
}

void Scene::SceneManager::ChangeState(std::unique_ptr<ISceneState> newState) {
  state_ = std::move(newState);
}

Scene *Scene::SceneManager::get_(const std::string &name) {
  auto it = scenes_.find(name);
  return (it == scenes_.end()) ? nullptr : it->second.get();
}

// =================================================================
// Data-driven scene management
// =================================================================
#include "DataDrivenScene/DataDrivenScene.h"
#include <filesystem>

void Scene::SceneManager::LoadScenesFromDirectory(const std::string& dirPath) {
  namespace fs = std::filesystem;
  if (!fs::exists(dirPath) || !fs::is_directory(dirPath)) {
    Log::Print("[SceneManager] Scene directory not found: " + dirPath);
    return;
  }

  for (auto& entry : fs::directory_iterator(dirPath)) {
    if (entry.is_regular_file() && entry.path().extension() == ".json") {
      std::string sceneName = entry.path().stem().string();
      if (scenes_.count(sceneName) > 0) continue;

      auto scene = std::make_unique<DataDrivenScene>(sceneName, entry.path().string());
      Log::Print("[SceneManager] Registered data-driven scene: " + sceneName);
      scenes_[sceneName] = std::move(scene);
    }
  }
}

bool Scene::SceneManager::CreateNewScene(const std::string& name, const std::string& dirPath) {
  if (scenes_.count(name) > 0) {
    Log::Print("[SceneManager] Scene already exists: " + name);
    return false;
  }

  namespace fs = std::filesystem;
  fs::create_directories(dirPath);
  std::string filePath = dirPath + "/" + name + ".json";

  auto scene = std::make_unique<DataDrivenScene>(name, filePath);

  // テンプレートはゲーム側 Resources/Template → エンジン側 Chaso/Resources/Template の順で探す
  std::string templatePath = Chaso::ResolvePath(dirPath + "/../Template/SceneTemplate.json");
  if (fs::exists(templatePath)) {
    fs::copy_file(templatePath, filePath, fs::copy_options::overwrite_existing);
    Log::Print("[SceneManager] Created new scene from template: " + templatePath);
    scene->Load();
    // テンプレート由来の sceneName ("SceneTemplate") が残るので、
    // 新しいシーン名で保存し直す。
    scene->Save();
  } else {
    // デフォルトのエンティティを追加 (Unityライクな初期状態)
    // NOTE: Scene::CreateEntity() は TransformComponent を自動付与しないため、
    //       呼び出し側で必ず明示的に AddComponent すること。
    //       Transform が無いと DataDrivenScene 側の同期ループ・FindMainCamera・
    //       ギズモ操作がすべてスキップされる。
    auto dirLight = scene->CreateEntity("Directional Light");
    dirLight->AddComponent<TransformComponent>();
    auto& dl = dirLight->AddComponent<DirectionalLightComponent>();
    dl.color = {1.0f, 1.0f, 1.0f, 1.0f};
    dl.direction = {0.0f, -1.0f, 0.5f}; // 斜め下
    dl.intensity = 1.0f;

    auto mainCam = scene->CreateEntity("Main Camera");
    auto& camTr = mainCam->AddComponent<TransformComponent>();
    camTr.position = {0.0f, 1.0f, -10.0f};
    auto& cam = mainCam->AddComponent<CameraComponent>();
    cam.isMain = true;

    scene->FlushPendingEntities();
    scene->Save();
  }

  Log::Print("[SceneManager] Created new scene: " + name);
  scenes_[name] = std::move(scene);
  return true;
}

bool Scene::SceneManager::DeleteScene(const std::string& name) {
  auto it = scenes_.find(name);
  if (it == scenes_.end()) {
    Log::Print("[SceneManager] Scene not found for deletion: " + name);
    return false;
  }

  if (current_ == it->second.get()) {
    Log::Print("[SceneManager] Cannot delete active scene: " + name);
    return false;
  }

  if (auto* dds = dynamic_cast<DataDrivenScene*>(it->second.get())) {
    namespace fs = std::filesystem;
    if (fs::exists(dds->FilePath())) {
      fs::remove(dds->FilePath());
      Log::Print("[SceneManager] Deleted scene file: " + dds->FilePath());
    }
  }

  scenes_.erase(it);
  Log::Print("[SceneManager] Deleted scene: " + name);
  return true;
}

bool Scene::SceneManager::SaveCurrentScene() {
  if (!current_) return false;
  if (auto* dds = dynamic_cast<DataDrivenScene*>(current_)) {
    return dds->Save();
  }
  Log::Print("[SceneManager] Current scene is not a DataDrivenScene, cannot save.");
  return false;
}
