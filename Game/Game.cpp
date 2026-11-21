#include "Game.h"
#include "SceneFlow.h"
#include "imgui/imgui.h"
#include "Dx12/Dx12Core.h"
#include "Dx12/Utility/ScreenCapture.h"
#include "Render/RenderContext.h"
#include <shellapi.h>
#include <filesystem>

void Game::Init(SceneContext &ctx) {
  sceneMgr_.Init(ctx);
  registerScenes_();

  // 起動シーンはゲーム側の AppConfig.json ("bootScene") で決める（既定 "Title"）
  sceneMgr_.ChangeImmediately(bootScene_, ctx);
}

void Game::registerScenes_() {
  // エディタで管理している JSON ディレクトリからデータ駆動シーンを読み込む
  sceneMgr_.LoadScenesFromDirectory(kSceneDir);

  // シーン遷移表（Resources/SceneFlow.json）。無ければ空で始まり、エディタから作れる
  SceneFlow::Get().Load(SceneFlow::kDefaultPath);
}

void Game::Update(SceneContext &ctx) { sceneMgr_.Update(ctx); }

void Game::Render(SceneContext &ctx, ID3D12GraphicsCommandList *cl) {
  sceneMgr_.Render(ctx, cl);
}

void Game::RenderOverlay(SceneContext &ctx, ID3D12GraphicsCommandList *cl) {
  sceneMgr_.RenderOverlay(ctx, cl);
}

void Game::Term() {
  sceneMgr_.Term();
  // オーディオの終了処理は App::Term() の AudioEngine::Term() で行う
}

void Game::RequestChange(const std::string &name) {
  sceneMgr_.RequestChange(name);
}

const std::string &Game::CurrentSceneName() const {
  return sceneMgr_.CurrentName();
}

Scene* Game::GetCurrentScene() {
  return sceneMgr_.GetCurrentScene();
}

void Game::ReloadCurrentScene(SceneContext &ctx) {
  sceneMgr_.ReloadCurrentScene(ctx);
}

void Game::BackupCurrentScene() {
  sceneMgr_.BackupCurrentScene();
}

void Game::RestoreCurrentScene(SceneContext &ctx) {
  sceneMgr_.RestoreCurrentScene(ctx);
}

void Game::DrawDebugUI(SceneContext &ctx) {
#if RC_ENABLE_IMGUI
    if (ImGui::BeginMenu("Scene")) {
      auto sceneNames = sceneMgr_.GetSceneNames();
      const char *currentSceneName = CurrentSceneName().c_str();

      // 登録済みシーンの一覧。選択したシーンへ遷移を要求する
      for (auto& name : sceneNames) {
        bool is_selected = (name == currentSceneName);
        if (ImGui::MenuItem(name.c_str(), nullptr, is_selected)) {
          RequestChange(name);
        }
      }

      ImGui::Separator();

      // 新規シーン作成。作成に成功したら入力欄を空に戻す
      ImGui::InputText("##NewSceneName", newSceneNameBuf_, sizeof(newSceneNameBuf_));
      ImGui::SameLine();
      if (ImGui::MenuItem("New Scene")) {
        std::string newName(newSceneNameBuf_);
        if (!newName.empty()) {
          if (sceneMgr_.CreateNewScene(newName, kSceneDir)) {
            newSceneNameBuf_[0] = '\0';
          }
        }
      }

      ImGui::Separator();

      if (ImGui::MenuItem("Save Current Scene", "Ctrl+S")) {
        sceneMgr_.SaveCurrentScene();
      }

      ImGui::Separator();

      // シーン削除。実行中のシーンは消せないよう一覧から除外する
      if (ImGui::BeginMenu("Delete Scene")) {
        for (auto& name : sceneNames) {
          if (name == CurrentSceneName()) continue;
          if (ImGui::MenuItem(name.c_str())) {
            sceneMgr_.DeleteScene(name);
          }
        }
        ImGui::EndMenu();
      }

      ImGui::EndMenu();
    }
#endif
}
