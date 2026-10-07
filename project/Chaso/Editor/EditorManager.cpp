#include "EditorManager.h"
#include "Common/ResourcePath.h"
#include "CaptureMode.h"
#include "EditorExtension.h"
#include "SceneFlowPanel.h"
#include "KeyBindingsHelp.h"
#include "SceneFlow.h"
#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "imgui/ImGuizmo.h"
#include "Log/Log.h"
#include "Dx12/Dx12Core.h"
#include "Dx12/Utility/ScreenCapture.h"
#include "RC.h"
#include "../Game/Scene/Scene.h"
#include "Render/RenderContext.h"
#include "Graphics/PostProcess/PostProcess.h"
#include "Graphics/Texture/TextureManager/TextureManager.h"
#include "ECS/Entity.h"
#include "ECS/TransformComponent.h"
#include "ECS/ModelRendererComponent.h"
#include "ECS/SkyboxComponent.h"
#include "ECS/SkydomeComponent.h"
#include "ECS/LightComponent.h"
#include "ECS/CameraComponent.h"
#include "ECS/AnimationComponent.h"
#include "ECS/BoneAttachmentComponent.h"
#include "Graphics/Model/Animation.h" // RC::GetAnimationCount
#include "ECS/PrimitiveMeshComponent.h"
#include "ECS/TextMeshComponent.h"
#include "ECS/SpriteRendererComponent.h"
#include "ECS/TextRendererComponent.h"
#include "ECS/WaterComponent.h"
#include "ECS/RigidbodyComponent.h"
#include "Render/RenderCommon.h"
#include "Common/FrameProfiler.h"
#include <algorithm>
#include <array>
#include "Math/Math.h"
#include "Math/MathUtils.h"
#include "Camera/CameraMath.h"
#include "ECS/ColliderComponent.h"
#include "ECS/NativeScriptComponent.h"
#include "ECS/AudioSourceComponent.h"
#include "ECS/AudioListenerComponent.h"
#include "ECS/ScriptRegistry.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <format>
#include <memory>
#include <nlohmann/json.hpp>
#include <set>

namespace {

/// @brief 2D（スクリーン座標）で描かれるエンティティか（Text / Sprite）
bool Is2DEntity(const Entity& e) {
  if (e.GetComponent<TextRendererComponent>() != nullptr) return true;
  if (auto* spr = e.GetComponent<SpriteRendererComponent>()) {
    // ワールド空間スプライトは 3D 扱い（通常のギズモで操作する）
    return !spr->IsWorldSpace();
  }
  return false;
}

/// @brief 2D エンティティの画面上の矩形（ゲーム解像度ピクセル）を求める
/// @return 矩形を持つなら true
bool Get2DRect(const Entity& e, RC::Vector2& outMin, RC::Vector2& outMax) {
  auto* tr = e.GetComponent<TransformComponent>();
  if (!tr) return false;
  const float x = tr->position.x;
  const float y = tr->position.y;

  if (auto* txt = e.GetComponent<TextRendererComponent>()) {
    RC::Vector2 size{0.0f, 0.0f};
    if (txt->HasFont()) {
      size = RC::MeasureString(txt->fontHandle, txt->text, txt->scale, txt->lineSpacing);
    }
    // フォント未ロード・空文字でも掴めるように最小サイズを確保
    const float minH = txt->fontSize * txt->scale;
    if (size.x < 8.0f) size.x = (std::max)(8.0f, minH * 0.5f);
    if (size.y < 8.0f) size.y = (std::max)(8.0f, minH);
    float left = x;
    if (txt->align == TextAlign::Center) left -= size.x * 0.5f;
    else if (txt->align == TextAlign::Right) left -= size.x;
    outMin = {left, y};
    outMax = {left + size.x, y + size.y};
    return true;
  }
  if (auto* spr = e.GetComponent<SpriteRendererComponent>()) {
    if (spr->IsWorldSpace()) return false; // ワールド空間は 3D 扱い
    outMin = {x, y};
    outMax = {x + (std::max)(spr->size.x, 8.0f), y + (std::max)(spr->size.y, 8.0f)};
    return true;
  }
  return false;
}

} // namespace

void EditorManager::Initialize() {
#if RC_ENABLE_IMGUI
  playState_ = PlayState::Stopped;

  playIconTex_ = RC::GetRenderContext().Textures().LoadID("Resources/icons/play.png");
  pauseIconTex_ = RC::GetRenderContext().Textures().LoadID("Resources/icons/pause.png");
  stopIconTex_ = RC::GetRenderContext().Textures().LoadID("Resources/icons/stop.png");
  restartIconTex_ = RC::GetRenderContext().Textures().LoadID("Resources/icons/step_back.png");

  eyeVisibleTex_ = RC::GetRenderContext().Textures().LoadID("Resources/icons/eye_visible.png");
  eyeHiddenTex_ = RC::GetRenderContext().Textures().LoadID("Resources/icons/eye_hidden.png");
  lockLockedTex_ = RC::GetRenderContext().Textures().LoadID("Resources/icons/lock_locked.png");
  lockUnlockedTex_ = RC::GetRenderContext().Textures().LoadID("Resources/icons/lock_unlocked.png");

  folderIconTex_ = RC::GetRenderContext().Textures().LoadID("Resources/icons/folder.png");
  fileIconTex_ = RC::GetRenderContext().Textures().LoadID("Resources/icons/file.png");
  fileImageTex_ = RC::GetRenderContext().Textures().LoadID("Resources/icons/file_image.png");
  file3DTex_ = RC::GetRenderContext().Textures().LoadID("Resources/icons/file_3d.png");
  fileMaterialTex_ = RC::GetRenderContext().Textures().LoadID("Resources/icons/file_material.png");
  fileDocTex_ = RC::GetRenderContext().Textures().LoadID("Resources/icons/file_doc.png");
  fileFontTex_ = RC::GetRenderContext().Textures().LoadID("Resources/icons/file_font.png");

  ApplyDarkTheme();

  // 設定のロード
  LoadConfig();
#endif
}

uint32_t EditorManager::GetSelectedEntityId() const {
  if (auto e = selectedEntity_.lock()) return e->Id();
  return 0;
}

namespace {

/// @brief スナップショットを比較可能な形に正規化する
/// @details Entity::Serialize() は components_（unordered_map）を走査して配列を作るため、
///          Deserialize で作り直すと同じ内容でも配列の並びが変わりうる。
///          そのままハッシュを取ると復元直後に「変更あり」と誤検知してしまうので、
///          コンポーネント配列を type 名でソートして順序を固定する。
///          エンティティ自体の並びは Hierarchy の表示順なので触らない
///          （復元時も配列順は保たれるため比較には影響しない）。
void CanonicalizeSnapshot(nlohmann::json& snapshot) {
  if (!snapshot.is_array()) return;
  for (auto& ej : snapshot) {
    if (!ej.is_object()) continue;
    auto it = ej.find("components");
    if (it == ej.end() || !it->is_array()) continue;
    std::vector<nlohmann::json> comps(it->begin(), it->end());
    std::sort(comps.begin(), comps.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
      return a.value("type", std::string{}) < b.value("type", std::string{});
    });
    *it = comps;
  }
}

/// @brief スナップショットの内容ハッシュ（変更検知用）
/// @details 既定の dump() は不正な UTF-8 を含む文字列で例外を投げる。
///          エンティティ名やテキストに何が入るかは分からないので、
///          replace ハンドラを指定して絶対に throw させない。
size_t HashSnapshot(const nlohmann::json& j) {
  return std::hash<std::string>{}(
      j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
}

/// @brief 正規化済みのスナップショットを取得する
nlohmann::json CaptureCanonicalSnapshot(Scene* scene) {
  nlohmann::json snapshot = scene->CaptureEntitiesSnapshot();
  CanonicalizeSnapshot(snapshot);
  return snapshot;
}

/// @brief 指定エンティティとその子孫を階層順（親が先）に集める
std::vector<std::shared_ptr<Entity>> CollectSubtree(Scene* scene, const std::shared_ptr<Entity>& root) {
  std::vector<std::shared_ptr<Entity>> result;
  if (!scene || !root) return result;
  // 親子付けの取り回しで GUID が循環しても無限ループしないように既訪問を持つ
  std::set<uint64_t> visited;
  result.push_back(root);
  visited.insert(root->Guid());
  // 幅優先。result を走査しながら末尾へ追加していくため、インデックスで回す
  for (size_t i = 0; i < result.size(); ++i) {
    const uint64_t parentGuid = result[i]->Guid();
    for (const auto& e : scene->GetEntities()) {
      if (!e || e->IsPendingDestroy()) continue;
      if (e->ParentGuid() != parentGuid) continue;
      if (!visited.insert(e->Guid()).second) continue;
      result.push_back(e);
    }
  }
  return result;
}

} // namespace

// =================================================================
// ショートカット / Undo・Redo / コピー＆ペースト
// =================================================================

void EditorManager::HandleShortcuts(Dx12Core* core, Scene* currentScene) {
#if RC_ENABLE_IMGUI
  // --- F2: スクリーンショット（再生中でも常時有効） ---
  if (ImGui::IsKeyPressed(ImGuiKey_F2, false)) {
    if (core) core->RequestScreenshot();
  }

  // --- Ctrl+S: シーンを保存（未保存の Scene Flow があればそれも） ---
  // 再生中の状態を保存すると編集内容が壊れるので、編集モードのときだけ保存する。
  // テキスト入力中でも効かせる（入力欄の値は 1 文字ごとに反映されている）。
  if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S)) {
    if (playState_ != PlayState::Stopped) {
      Log::Print("[Editor] 再生中は保存できません（停止してから Ctrl+S）");
    } else {
      if (currentScene) currentScene->Save();
      if (SceneFlow::Get().IsDirty()) SceneFlow::Get().Save();
    }
  }

  // 以降は編集モード（Stopped）専用。
  // テキスト入力中はそちらの Ctrl+C / Ctrl+Z を優先する。
  if (playState_ != PlayState::Stopped) return;
  if (ImGui::GetIO().WantTextInput) return;
  if (!currentScene) return;

  if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_C)) {
    CopySelectedEntity(currentScene);
  }
  if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_V)) {
    PasteEntityClipboard(currentScene);
  }
  // Ctrl+Shift+N: 空のオブジェクトを作成（選択中のエンティティがあればその子として）
  if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_N)) {
    auto sel = selectedEntity_.lock();
    CreateEmptyEntity(currentScene, sel ? sel->Guid() : 0);
  }

  // Ctrl+Y と Ctrl+Shift+Z のどちらでも Redo できるようにする
  if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z) ||
      ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y)) {
    Redo(currentScene);
  } else if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z)) {
    Undo(currentScene);
  }
#else
  (void)core; (void)currentScene;
#endif
}

std::shared_ptr<Entity> EditorManager::CreateEmptyEntity(Scene* currentScene, uint64_t parentGuid) {
  if (!currentScene) return nullptr;

  // Unity の "Create Empty" 相当。描画系コンポーネントは持たず Transform だけを付ける。
  // 親子付けの基点やスクリプト用の器として使う想定。
  auto e = currentScene->CreateEntity("Empty");
  e->AddComponent<TransformComponent>();

  // 親が指定されていて実在する場合のみ子として配置する
  if (parentGuid != 0 && currentScene->FindEntityByGuid(parentGuid)) {
    e->SetParentGuid(parentGuid);
    expandEntityGuid_ = parentGuid; // 折りたたまれた親の下に隠れないよう次フレームで展開する
  } else {
    e->SetParentGuid(0);
  }

  selectedEntity_ = e;
  return e;
}

void EditorManager::ResetHistory(Scene* currentScene) {
  undoStack_.clear();
  redoStack_.clear();
  historyCurrent_ = nlohmann::json::array();
  historyHash_ = 0;
  historyPending_ = nullptr;
  historyPendingHash_ = 0;
  historyValid_ = false;
  historyResync_ = false;
  historySceneKey_ = currentScene;
  historyPollTimer_ = 0.0f;
}

void EditorManager::CommitHistoryIfChanged(Scene* currentScene) {
#if RC_ENABLE_IMGUI
  // 履歴が動いていない原因が分かるように、状態が変わった時だけ理由をログへ出す
  const auto logBlockReason = [this](int reason, const char* message) {
    if (historyBlockLogged_ == reason) return;
    historyBlockLogged_ = reason;
    Log::Print(message);
  };

  if (!historyEnabled_) {
    logBlockReason(3, "[Editor] 履歴: メニューで無効化されています");
    return;
  }
  if (!currentScene) {
    logBlockReason(1, "[Editor] 履歴: シーンが取得できないため停止中");
    return;
  }

  // シーンが切り替わったら履歴を作り直す（別シーンの状態を復元しないため）
  if (historySceneKey_ != currentScene) {
    ResetHistory(currentScene);
  }

  // 編集モード以外では履歴を取らない。再生で状態が変わるため、
  // 停止に戻った時点の状態を基準として再取得する。
  if (playState_ != PlayState::Stopped) {
    logBlockReason(2, "[Editor] 履歴: 編集モード(Stop)ではないため停止中");
    historyValid_ = false;
    return;
  }
  logBlockReason(0, "[Editor] 履歴: 有効");

  // 毎フレーム全体をシリアライズすると重いので間隔を空けて検知する
  historyPollTimer_ += ImGui::GetIO().DeltaTime;
  if (historyPollTimer_ < kHistoryPollInterval) return;
  historyPollTimer_ = 0.0f;

  // シリアライズは何が起きても履歴機能ごと黙って止めないように保護する
  nlohmann::json snapshot;
  size_t hash = 0;
  try {
    snapshot = CaptureCanonicalSnapshot(currentScene);
    hash = HashSnapshot(snapshot);
  } catch (const std::exception& ex) {
    if (!historyErrorLogged_) {
      historyErrorLogged_ = true;
      Log::Print(std::string("[Editor] 履歴のスナップショット取得に失敗: ") + ex.what());
    }
    return;
  }

  // 初回（または再生から戻った直後）は現在の状態を基準として記録するだけ
  if (!historyValid_) {
    historyCurrent_ = std::move(snapshot);
    historyHash_ = hash;
    historyPendingHash_ = 0;
    historyValid_ = true;
    historyResync_ = false;
    Log::Print(std::format("[Editor] 履歴の基準を取得 ({} エンティティ)", historyCurrent_.size()));
    return;
  }

  // 変化なし
  if (hash == historyHash_) {
    historyPendingHash_ = 0;
    historyPending_ = nullptr;
    historyResync_ = false;
    return;
  }

  // まだ値が動いている（ドラッグ中・入力中）＝ 確定しない。
  // ImGui や ImGuizmo の内部状態には依存せず「2回連続で同じ内容なら落ち着いた」と判定する。
  // 状態フラグが立ちっぱなしになって履歴が永久に止まる事故を避けるため。
  if (hash != historyPendingHash_) {
    historyPendingHash_ = hash;
    historyPending_ = std::move(snapshot);
    return;
  }

  // Undo/Redo・ペーストで復元した直後の差分は「ユーザーの編集」ではないので履歴に積まない。
  // ここで積むと Undo が同じ状態を往復し、Redo は消えてしまう。
  if (historyResync_) {
    historyResync_ = false;
    historyCurrent_ = std::move(historyPending_);
    historyHash_ = hash;
    historyPendingHash_ = 0;
    return;
  }

  undoStack_.push_back(std::move(historyCurrent_));
  if (undoStack_.size() > kMaxHistory) {
    undoStack_.erase(undoStack_.begin());
  }
  redoStack_.clear();
  historyCurrent_ = std::move(historyPending_);
  historyHash_ = hash;
  historyPendingHash_ = 0;
  Log::Print(std::format("[Editor] 変更を記録 (戻せる {} 手)", undoStack_.size()));
#else
  (void)currentScene;
#endif
}

void EditorManager::ApplySnapshot(Scene* currentScene, const nlohmann::json& snapshot) {
  if (!currentScene) return;

  // 復元でエンティティが作り直されるため、選択は GUID で引き直す
  uint64_t selectedGuid = 0;
  if (auto sel = selectedEntity_.lock()) selectedGuid = sel->Guid();

  currentScene->RestoreEntitiesSnapshot(snapshot);
  currentScene->FlushPendingEntities();

  selectedEntity_.reset();
  if (selectedGuid != 0) {
    if (auto restored = currentScene->FindEntityByGuid(selectedGuid)) {
      selectedEntity_ = restored;
    }
  }
  currentScene->SetSelectedEntityId(GetSelectedEntityId());
  renamingEntityId_ = 0;
  dragging2D_ = false;

  // 次回の変更検知では「復元による差分」として扱い、履歴を汚さない
  historyResync_ = true;
  historyPending_ = nullptr;
  historyPendingHash_ = 0;
  historyPollTimer_ = 0.0f;
}

void EditorManager::Undo(Scene* currentScene) {
  if (!currentScene) return;
  // キーは届いているのか、履歴が空なのかを切り分けられるようにログを出す
  if (undoStack_.empty()) {
    Log::Print("[Editor] Undo: 戻せる履歴がありません");
    return;
  }

  redoStack_.push_back(historyCurrent_);
  historyCurrent_ = undoStack_.back();
  undoStack_.pop_back();
  historyHash_ = HashSnapshot(historyCurrent_);
  historyValid_ = true;
  historyPollTimer_ = 0.0f;

  ApplySnapshot(currentScene, historyCurrent_);
  Log::Print(std::format("[Editor] Undo (残り {} / やり直し {})", undoStack_.size(), redoStack_.size()));
}

void EditorManager::Redo(Scene* currentScene) {
  if (!currentScene) return;
  if (redoStack_.empty()) {
    Log::Print("[Editor] Redo: やり直せる履歴がありません");
    return;
  }

  undoStack_.push_back(historyCurrent_);
  historyCurrent_ = redoStack_.back();
  redoStack_.pop_back();
  historyHash_ = HashSnapshot(historyCurrent_);
  historyValid_ = true;
  historyPollTimer_ = 0.0f;

  ApplySnapshot(currentScene, historyCurrent_);
  Log::Print(std::format("[Editor] Redo (戻せる {} / やり直し {})", undoStack_.size(), redoStack_.size()));
}

void EditorManager::CopySelectedEntity(Scene* currentScene) {
  auto selected = selectedEntity_.lock();
  if (!currentScene || !selected) return;

  const auto subtree = CollectSubtree(currentScene, selected);
  entityClipboard_ = nlohmann::json::array();
  for (const auto& e : subtree) {
    if (e) entityClipboard_.push_back(e->Serialize());
  }
  Log::Print(std::format("[Editor] Copied: {} ({} entities)", selected->Name(), entityClipboard_.size()));
}

void EditorManager::PasteEntityClipboard(Scene* currentScene) {
  if (!currentScene || !entityClipboard_.is_array() || entityClipboard_.empty()) return;

  // 貼り付けそのものも Undo の1ステップにする
  if (historyValid_) {
    undoStack_.push_back(historyCurrent_);
    if (undoStack_.size() > kMaxHistory) undoStack_.erase(undoStack_.begin());
    redoStack_.clear();
  }

  // コピー元の GUID を新しい GUID へ差し替える。
  // 親子関係はコピー範囲内なら新 GUID へ、範囲外ならそのまま（元と兄弟になる）。
  std::unordered_map<uint64_t, uint64_t> guidRemap;
  for (const auto& ej : entityClipboard_) {
    if (ej.contains("guid")) {
      guidRemap[ej["guid"].get<uint64_t>()] = Entity::GenerateGUID();
    }
  }

  std::shared_ptr<Entity> pastedRoot;
  std::vector<std::shared_ptr<Entity>> pasted;
  for (const auto& ej : entityClipboard_) {
    auto entity = std::make_shared<Entity>();
    entity->Deserialize(ej);

    if (auto it = guidRemap.find(entity->Guid()); it != guidRemap.end()) {
      entity->SetGuid(it->second);
    } else {
      entity->SetGuid(Entity::GenerateGUID());
    }
    if (auto it = guidRemap.find(entity->ParentGuid()); it != guidRemap.end()) {
      entity->SetParentGuid(it->second);
    }

    if (!pastedRoot) {
      pastedRoot = entity; // CollectSubtree はルートを先頭に入れている
      entity->SetName(entity->Name() + " Copy");
    }

    pasted.push_back(entity);
    currentScene->AddEntity(entity);
  }
  currentScene->FlushPendingEntities();

  // モデル・テクスチャ・ライト等のランタイムハンドルを作り直す。
  // Serialize にはパスしか含まれずハンドルは未設定のままなので、
  // ここで初期化しないと貼り付けたエンティティが描画されない。
  for (const auto& e : pasted) {
    if (e) currentScene->InitDynamicEntityRuntime(*e);
  }

  if (pastedRoot) {
    selectedEntity_ = pastedRoot;
    currentScene->SetSelectedEntityId(pastedRoot->Id());
    Log::Print(std::format("[Editor] Pasted: {}", pastedRoot->Name()));
  }

  // 貼り付け後の状態を基準として記録し直す
  historyCurrent_ = CaptureCanonicalSnapshot(currentScene);
  historyHash_ = HashSnapshot(historyCurrent_);
  historyValid_ = true;
  historyResync_ = true; // ランタイム初期化ぶんの差分を編集扱いにしない
  historyPending_ = nullptr;
  historyPendingHash_ = 0;
  historyPollTimer_ = 0.0f;
}

void EditorManager::ApplyDarkTheme() {
#if RC_ENABLE_IMGUI
  ImGuiStyle& style = ImGui::GetStyle();
  ImVec4* colors = style.Colors;

  // UE5/Unity-like dark theme
  colors[ImGuiCol_WindowBg]           = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);
  colors[ImGuiCol_ChildBg]            = ImVec4(0.12f, 0.12f, 0.12f, 1.00f);
  colors[ImGuiCol_PopupBg]            = ImVec4(0.10f, 0.10f, 0.10f, 0.98f);
  colors[ImGuiCol_Border]             = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
  colors[ImGuiCol_BorderShadow]       = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
  colors[ImGuiCol_FrameBg]            = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
  colors[ImGuiCol_FrameBgHovered]     = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
  colors[ImGuiCol_FrameBgActive]      = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);
  colors[ImGuiCol_TitleBg]            = ImVec4(0.12f, 0.12f, 0.12f, 1.00f);
  colors[ImGuiCol_TitleBgActive]      = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);
  colors[ImGuiCol_TitleBgCollapsed]   = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
  colors[ImGuiCol_MenuBarBg]          = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
  colors[ImGuiCol_ScrollbarBg]        = ImVec4(0.05f, 0.05f, 0.05f, 0.50f);
  colors[ImGuiCol_ScrollbarGrab]      = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
  colors[ImGuiCol_ScrollbarGrabHovered]= ImVec4(0.30f, 0.30f, 0.30f, 1.00f);
  colors[ImGuiCol_ScrollbarGrabActive]= ImVec4(0.40f, 0.40f, 0.40f, 1.00f);
  colors[ImGuiCol_CheckMark]          = ImVec4(0.85f, 0.45f, 0.05f, 1.00f); // Orange accent
  colors[ImGuiCol_SliderGrab]         = ImVec4(0.85f, 0.45f, 0.05f, 1.00f);
  colors[ImGuiCol_SliderGrabActive]   = ImVec4(0.95f, 0.55f, 0.15f, 1.00f);
  colors[ImGuiCol_Button]             = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
  colors[ImGuiCol_ButtonHovered]      = ImVec4(0.35f, 0.35f, 0.35f, 1.00f);
  colors[ImGuiCol_ButtonActive]       = ImVec4(0.85f, 0.45f, 0.05f, 1.00f);
  colors[ImGuiCol_Header]             = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
  colors[ImGuiCol_HeaderHovered]      = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
  colors[ImGuiCol_HeaderActive]       = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);
  colors[ImGuiCol_Separator]          = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
  colors[ImGuiCol_SeparatorHovered]   = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
  colors[ImGuiCol_SeparatorActive]    = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);
  colors[ImGuiCol_ResizeGrip]         = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
  colors[ImGuiCol_ResizeGripHovered]  = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);
  colors[ImGuiCol_ResizeGripActive]   = ImVec4(0.85f, 0.45f, 0.05f, 1.00f);
  colors[ImGuiCol_Tab]                = ImVec4(0.12f, 0.12f, 0.12f, 1.00f);
  colors[ImGuiCol_TabHovered]         = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
  colors[ImGuiCol_TabActive]          = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
  colors[ImGuiCol_TabUnfocused]       = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
  colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);
  colors[ImGuiCol_DockingPreview]     = ImVec4(0.85f, 0.45f, 0.05f, 0.50f);
  colors[ImGuiCol_DockingEmptyBg]     = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);

  style.WindowRounding    = 4.0f;
  style.ChildRounding     = 4.0f;
  style.FrameRounding     = 4.0f;
  style.PopupRounding     = 4.0f;
  style.ScrollbarRounding = 4.0f;
  style.GrabRounding      = 4.0f;
  style.TabRounding       = 4.0f;
  style.WindowBorderSize  = 1.0f;
  style.FrameBorderSize   = 1.0f;
#endif
}

void EditorManager::Update(Dx12Core* core, std::function<void()> onMenuAppend, Scene* currentScene) {
#if RC_ENABLE_IMGUI
  // ============================
  // 撮影モード（F11）
  // ============================
  // 録画用のモード。入っているあいだは
  // メニューバーも他のパネルも出さず、ゲーム画面だけにする。
  CaptureMode::HandleHotkeys(core);
  if (CaptureMode::IsActive()) {
    // 撮るのはプレイ画面なので、再生状態にしておく。
    // 停止中からの遷移は App 側がバックアップを取ってくれる。
    if (CaptureMode::WantsPlaying() && playState_ == PlayState::Stopped) {
      playState_ = PlayState::Playing;
    }
    // ホバー判定は CaptureMode::Draw が前フレームに出したものを使う。
    // WantCaptureMouse は使えない。撮影ビューの下に DockSpace のホスト窓が
    // 残っていて常に「何かをホバーしている」状態になるため、それで判定すると
    // どこにいても射撃が止まる。
    isViewportHovered_ = CaptureMode::IsGameHovered();
    return;
  }

  // ============================
  // ショートカット
  // ============================
  // 先に履歴を確定させてからキー入力を処理する。
  // 逆順だと Ctrl+Z の直前の変更が履歴に入らず1手ぶん取りこぼす。
  CommitHistoryIfChanged(currentScene);
  HandleShortcuts(core, currentScene);

  // ============================
  // メニューバー
  // ============================
  if (ImGui::BeginMainMenuBar()) {
    if (ImGui::BeginMenu("File")) {
      const bool canSave = (playState_ == PlayState::Stopped) && currentScene != nullptr;
      if (ImGui::MenuItem("Save Scene", "Ctrl+S", false, canSave)) {
        currentScene->Save();
        if (SceneFlow::Get().IsDirty()) SceneFlow::Get().Save();
      }
      ImGui::Separator();
      if (ImGui::MenuItem("Exit")) {
        PostQuitMessage(0);
      }
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
      const bool editable = (playState_ == PlayState::Stopped) && currentScene != nullptr;
      // 履歴が積まれているかを確認できるように件数を出す
      const std::string undoLabel = std::format("Undo ({})", undoStack_.size());
      const std::string redoLabel = std::format("Redo ({})", redoStack_.size());
      if (ImGui::MenuItem(undoLabel.c_str(), "Ctrl+Z", false, editable && !undoStack_.empty())) {
        Undo(currentScene);
      }
      if (ImGui::MenuItem(redoLabel.c_str(), "Ctrl+Y", false, editable && !redoStack_.empty())) {
        Redo(currentScene);
      }
      ImGui::Separator();
      if (ImGui::MenuItem("Copy", "Ctrl+C", false, editable && !selectedEntity_.expired())) {
        CopySelectedEntity(currentScene);
      }
      if (ImGui::MenuItem("Paste", "Ctrl+V", false, editable && entityClipboard_.is_array() && !entityClipboard_.empty())) {
        PasteEntityClipboard(currentScene);
      }
      ImGui::Separator();
      // 履歴機能が他の挙動に影響していないか切り分けるためのオン/オフ
      if (ImGui::MenuItem("Undo履歴を記録する", nullptr, &historyEnabled_)) {
        if (!historyEnabled_) ResetHistory(currentScene);
      }
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Window")) {
      ImGui::MenuItem("ImGui Demo", nullptr, &showDemoWindow_);
      ImGui::MenuItem("Performance (FPS)", nullptr, &showPerfWindow_);
      ImGui::MenuItem("Render Queue", nullptr, &showRenderQueue_);
      ImGui::MenuItem("Particle Editor", nullptr, &showParticleEditor_);
      ImGui::MenuItem("Environment Settings", nullptr, &showEnvironmentWindow_);
      ImGui::MenuItem("Post Effect Settings", nullptr, &showPostEffectWindow_);
      ImGui::MenuItem("Scene Flow (シーン遷移)", nullptr, &showSceneFlow_);
      // Application 側が EditorExtension::AddPanel で登録したパネル
      auto &extPanels = EditorExtension::Panels();
      if (!extPanels.empty()) {
        ImGui::Separator();
        for (auto &panel : extPanels) {
          ImGui::MenuItem(panel.name.c_str(), nullptr, &panel.open);
        }
      }
      ImGui::Separator();
      if (ImGui::MenuItem("撮影モード (F11)")) {
        CaptureMode::SetActive(true);
      }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("ゲーム画面だけを全画面にします。F10 で録画、もう一度 F11 で戻る");
      }
      ImGui::Separator();
      if (ImGui::MenuItem("Reset Layout")) {
        resetLayout_ = true;
      }
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
      ImGui::MenuItem("キー操作一覧", nullptr, &showKeyHelp_);
      ImGui::EndMenu();
    }

    if (onMenuAppend) {
      onMenuAppend();
    }

    if (currentScene) {
      if (ImGui::BeginMenu("Add")) {
        if (ImGui::MenuItem("Empty", "Ctrl+Shift+N")) {
          CreateEmptyEntity(currentScene, 0);
        }
        ImGui::Separator();
        if (ImGui::BeginMenu("Mesh")) {
          if (ImGui::MenuItem("Cube")) {
            auto e = currentScene->CreateEntity("Cube");
            e->AddComponent<TransformComponent>();
            auto& pm = e->AddComponent<PrimitiveMeshComponent>();
            pm.type = PrimitiveType::Box;
            pm.meshHandle = RC::GenerateBox();
          }
          if (ImGui::MenuItem("Sphere")) {
            auto e = currentScene->CreateEntity("Sphere");
            e->AddComponent<TransformComponent>();
            auto& pm = e->AddComponent<PrimitiveMeshComponent>();
            pm.type = PrimitiveType::Sphere;
            pm.meshHandle = RC::GenerateSphere();
          }
          if (ImGui::MenuItem("Plane")) {
            auto e = currentScene->CreateEntity("Plane");
            e->AddComponent<TransformComponent>();
            auto& pm = e->AddComponent<PrimitiveMeshComponent>();
            pm.type = PrimitiveType::Plane;
            pm.meshHandle = RC::GeneratePlane();
          }
          if (ImGui::MenuItem("Cylinder")) {
            auto e = currentScene->CreateEntity("Cylinder");
            e->AddComponent<TransformComponent>();
            auto& pm = e->AddComponent<PrimitiveMeshComponent>();
            pm.type = PrimitiveType::Cylinder;
            pm.meshHandle = RC::GenerateCylinder();
          }
          if (ImGui::MenuItem("Cone")) {
            auto e = currentScene->CreateEntity("Cone");
            e->AddComponent<TransformComponent>();
            auto& pm = e->AddComponent<PrimitiveMeshComponent>();
            pm.type = PrimitiveType::Cone;
            pm.meshHandle = RC::GenerateCone();
          }
          ImGui::Separator();
          if (ImGui::MenuItem("3D Text (立体文字)")) {
            auto e = currentScene->CreateEntity("TextMesh");
            e->AddComponent<TransformComponent>();
            auto& tm = e->AddComponent<TextMeshComponent>();
            tm.text = "Text";
            // メッシュはシーンの更新ループ（EnsureTextMesh）で設定から自動生成される
          }
          ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Environment")) {
          if (ImGui::MenuItem("Skybox")) {
            auto e = currentScene->CreateEntity("Skybox");
            e->AddComponent<TransformComponent>();
            e->AddComponent<SkyboxComponent>();
          }
          if (ImGui::MenuItem("Skydome")) {
            auto e = currentScene->CreateEntity("Skydome");
            e->AddComponent<TransformComponent>();
            auto& sd = e->AddComponent<SkydomeComponent>();
            sd.skydomeHandle = RC::GenerateSkydomeEx(-1);
          }
          if (ImGui::MenuItem("Water")) {
            auto e = currentScene->CreateEntity("Water");
            e->AddComponent<TransformComponent>();
            auto& water = e->AddComponent<WaterComponent>();
            water.meshHandle = RC::GenerateWaterPlane(
                water.planeWidth, water.planeHeight, water.segments);
          }
          ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Light")) {
          if (ImGui::MenuItem("Directional Light")) {
            auto e = currentScene->CreateEntity("Directional Light");
            e->AddComponent<TransformComponent>();
            e->AddComponent<DirectionalLightComponent>();
          }
          if (ImGui::MenuItem("Point Light")) {
            auto e = currentScene->CreateEntity("Point Light");
            e->AddComponent<TransformComponent>();
            e->AddComponent<PointLightComponent>();
          }
          if (ImGui::MenuItem("Spot Light")) {
            auto e = currentScene->CreateEntity("Spot Light");
            e->AddComponent<TransformComponent>();
            e->AddComponent<SpotLightComponent>();
          }
          if (ImGui::MenuItem("Area Light")) {
            auto e = currentScene->CreateEntity("Area Light");
            e->AddComponent<TransformComponent>();
            e->AddComponent<AreaLightComponent>();
          }
          ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Camera")) {
          auto e = currentScene->CreateEntity("Camera");
          e->AddComponent<TransformComponent>();
          e->AddComponent<CameraComponent>();
        }
        if (ImGui::MenuItem("Sprite")) {
          auto e = currentScene->CreateEntity("Sprite");
          auto& tr = e->AddComponent<TransformComponent>();
          tr.position = {100.0f, 100.0f, 0.0f}; // スクリーン座標（ピクセル、左上原点）
          auto& spr = e->AddComponent<SpriteRendererComponent>();
          spr.spritePath = "Resources/uvChecker.png"; // Inspector / ドラッグ&ドロップで差し替え可
          selectedEntity_ = e;
        }
        if (ImGui::MenuItem("Text")) {
          auto e = currentScene->CreateEntity("Text");
          auto& tr = e->AddComponent<TransformComponent>();
          tr.position = {100.0f, 100.0f, 0.0f}; // スクリーン座標（ピクセル、左上原点）
          auto& txt = e->AddComponent<TextRendererComponent>();
          txt.text = "Text";
          // フォントは描画時に TextRendererComponent の設定から自動ロードされる
        }
        if (ImGui::BeginMenu("Audio")) {
          if (ImGui::MenuItem("BGM")) {
            // 空のオブジェクトに BGM を持たせる。再生開始（Playing）と同時に鳴り始める
            auto e = currentScene->CreateEntity("BGM");
            e->AddComponent<TransformComponent>();
            auto& audio = e->AddComponent<AudioSourceComponent>();
            audio.bus = AudioBus::BGM;
            audio.AddClip("main", "Resources/Sounds/bgm_main.mp3", 1.0f, /*loop=*/true);
            audio.playOnAwake = "main"; // Inspector / ドラッグ&ドロップで差し替え可
            selectedEntity_ = e;
          }
          if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("同じ曲を次のシーンにも置いておくと途切れずに続きます");
          }
          if (ImGui::MenuItem("Audio Source (SE)")) {
            auto e = currentScene->CreateEntity("Audio Source");
            e->AddComponent<TransformComponent>();
            auto& audio = e->AddComponent<AudioSourceComponent>();
            audio.bus = AudioBus::SE;
            selectedEntity_ = e;
          }
          if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("スクリプトから GetComponent<AudioSourceComponent>()->Play(\"名前\") で鳴らします");
          }
          if (ImGui::MenuItem("Audio Source (3D SE)")) {
            // 位置から聞こえる SE。エンティティの Transform を動かすと定位・音量が変わる
            auto e = currentScene->CreateEntity("Audio Source 3D");
            e->AddComponent<TransformComponent>();
            auto& audio = e->AddComponent<AudioSourceComponent>();
            audio.bus = AudioBus::SE;
            audio.spatialBlend = 1.0f;
            audio.minDistance = 1.0f;
            audio.maxDistance = 50.0f;
            selectedEntity_ = e;
          }
          if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("エンティティの位置から聞こえる SE（左右の定位・距離減衰・ドップラー）。\n聞き手は Audio Listener、無ければカメラ");
          }
          ImGui::EndMenu();
        }
        ImGui::EndMenu();
      }
    }

    // キャプチャ・録画機能の直接ボタン
    if (ImGui::MenuItem("スクリーンショット")) {
      if (core) core->RequestScreenshot();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("F2");

    bool isRecording = core && core->GetVideoRecorder().IsRecording();
    const char* recLabel = isRecording ? "画面録画停止" : "画面録画開始";
    if (ImGui::MenuItem(recLabel)) {
      if (core) {
        if (isRecording) core->StopRecording();
        else core->StartRecording();
      }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("F10");

    // ----------------------------
    // 中央の Play / Pause / Stop / Restart ボタン
    // ----------------------------
    float playButtonWidth = 40.0f;
    float playButtonsTotalWidth = playButtonWidth * 4.0f + ImGui::GetStyle().ItemSpacing.x * 3.0f;
    ImGui::SameLine((ImGui::GetWindowWidth() - playButtonsTotalWidth) * 0.5f);

    // Play ボタン
    if (playState_ == PlayState::Playing) {
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.6f, 0.2f, 1.0f));
    } else {
      ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_Button));
    }
    ImTextureID playId = (ImTextureID)RC::GetRenderContext().Textures().GetSrv(playIconTex_).ptr;
    if (ImGui::ImageButton("##Play", playId, ImVec2(16.0f, 16.0f))) {
      playState_ = PlayState::Playing;
    }
    ImGui::PopStyleColor();

    ImGui::SameLine();

    // Pause ボタン
    if (playState_ == PlayState::Paused) {
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.6f, 0.2f, 1.0f));
    } else {
      ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_Button));
    }
    ImTextureID pauseId = (ImTextureID)RC::GetRenderContext().Textures().GetSrv(pauseIconTex_).ptr;
    if (ImGui::ImageButton("##Pause", pauseId, ImVec2(16.0f, 16.0f))) {
      playState_ = PlayState::Paused;
    }
    ImGui::PopStyleColor();

    ImGui::SameLine();

    // Restart ボタン
    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_Button));
    ImTextureID restartId = (ImTextureID)RC::GetRenderContext().Textures().GetSrv(restartIconTex_).ptr;
    if (ImGui::ImageButton("##Restart", restartId, ImVec2(16.0f, 16.0f))) {
      restartRequested_ = true;
      playState_ = PlayState::Playing;
    }
    ImGui::PopStyleColor();

    ImGui::SameLine();

    // Stop ボタン
    if (playState_ == PlayState::Stopped) {
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.2f, 0.2f, 1.0f));
    } else {
      ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_Button));
    }
    ImTextureID stopId = (ImTextureID)RC::GetRenderContext().Textures().GetSrv(stopIconTex_).ptr;
    if (ImGui::ImageButton("##Stop", stopId, ImVec2(16.0f, 16.0f))) {
      // 停止ボタンが押された場合、App側で検知してシーンをリロードさせる
      playState_ = PlayState::Stopped;
    }
    ImGui::PopStyleColor();

    ImGui::SameLine();

    // ----------------------------
    // 右上のウィンドウコントロールボタン
    // ----------------------------
    float buttonWidth = 36.0f;
    float buttonCount = 2.0f;
    float totalWidth = buttonWidth * buttonCount;
    float menuBarHeight = ImGui::GetWindowSize().y;

    // カーソルを右端へ移動
    ImGui::SameLine(ImGui::GetWindowWidth() - totalWidth);

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0)); // ボタン間の隙間をなくす
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0)); // 背景透明
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.3f, 0.3f, 0.5f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.4f, 0.4f, 0.4f, 0.5f));

    HWND hwnd = (HWND)ImGui::GetMainViewport()->PlatformHandleRaw;

    // 最小化ボタン (-)
    if (ImGui::Button("ー", ImVec2(buttonWidth, menuBarHeight))) {
        if (hwnd) ShowWindow(hwnd, SW_MINIMIZE);
    }
    ImGui::SameLine();
    // 閉じる (X)
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8f, 0.2f, 0.2f, 0.8f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.9f, 0.3f, 0.3f, 0.8f));
    if (ImGui::Button("X", ImVec2(buttonWidth, menuBarHeight))) {
        if (hwnd) PostMessage(hwnd, WM_CLOSE, 0, 0);
        else PostQuitMessage(0);
    }

    ImGui::PopStyleColor(2); // Xボタン用の色を戻す
    ImGui::PopStyleColor(3); // 透明背景などの色を戻す
    ImGui::PopStyleVar(2); // FramePadding, ItemSpacing

    ImGui::EndMainMenuBar();
  }

  // レイアウト初期化処理
  SetupDockingLayout();

#endif
}

void EditorManager::SetupDockingLayout() {
#if RC_ENABLE_IMGUI
  ImGuiID dockspace_id = ImGui::GetID("MainDockSpace");

  if (firstLayout_) {
    firstLayout_ = false;
    // imgui.ini からロードされたノードが存在しない場合のみ初回レイアウトを構築
    if (ImGui::DockBuilderGetNode(dockspace_id) == nullptr) {
      resetLayout_ = true;
    }
  }

  if (!resetLayout_) return;
  resetLayout_ = false;

  // 既存のレイアウトをクリア
  ImGui::DockBuilderRemoveNode(dockspace_id);
  ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
  ImGui::DockBuilderSetNodeSize(dockspace_id, ImGui::GetMainViewport()->Size);

  // ウィンドウを分割していく
  ImGuiID dock_main_id = dockspace_id;
  ImGuiID dock_id_left = ImGui::DockBuilderSplitNode(dock_main_id, ImGuiDir_Left, 0.20f, nullptr, &dock_main_id);
  ImGuiID dock_id_right = ImGui::DockBuilderSplitNode(dock_main_id, ImGuiDir_Right, 0.25f, nullptr, &dock_main_id);
  ImGuiID dock_id_bottom = ImGui::DockBuilderSplitNode(dock_main_id, ImGuiDir_Down, 0.30f, nullptr, &dock_main_id);

  // パネルを各ノードに割り当て
  ImGui::DockBuilderDockWindow("Viewport", dock_main_id);
  ImGui::DockBuilderDockWindow("Hierarchy", dock_id_left);
  ImGui::DockBuilderDockWindow("Inspector", dock_id_right);
  ImGui::DockBuilderDockWindow("Content Browser", dock_id_bottom);
  ImGui::DockBuilderDockWindow("Console", dock_id_bottom);

  ImGui::DockBuilderFinish(dockspace_id);
#endif
}

void EditorManager::DrawEntityNode(std::shared_ptr<Entity> e, Scene* currentScene, const std::unordered_map<uint64_t, std::vector<std::shared_ptr<Entity>>>& childrenMap) {
#if RC_ENABLE_IMGUI
    if (!e || e->IsPendingDestroy()) return;

    // 再帰的に子エンティティへ処理を適用するヘルパー
    auto applyToChildren = [&](std::shared_ptr<Entity> parent, bool state, auto func) {
        auto applyRecursive = [&](std::shared_ptr<Entity> node, auto& self) -> void {
            if (!node) return;
            func(node, state);
            auto it = childrenMap.find(node->Guid());
            if (it != childrenMap.end()) {
                for (auto& child : it->second) {
                    self(child, self);
                }
            }
        };
        auto it = childrenMap.find(parent->Guid());
        if (it != childrenMap.end()) {
            for (auto& child : it->second) {
                applyRecursive(child, applyRecursive);
            }
        }
    };

    ImGui::PushID(e->Id());

    // --- アクティブ切り替え ---
    {
      bool active = e->IsActive();
      if (ImGui::Checkbox("##active", &active)) {
        e->SetActive(active);
        if (e->IsFolder()) {
            applyToChildren(e, active, [](std::shared_ptr<Entity> node, bool s) { node->SetActive(s); });
        }
      }
    }
    ImGui::SameLine(0, 4);

    // --- 目アイコン（可視切り替え） ---
    {
      int texId = e->IsVisible() ? eyeVisibleTex_ : eyeHiddenTex_;
      auto srv = RC::GetRenderContext().Textures().GetSrv(texId);
      ImVec4 tint = e->IsVisible() ? ImVec4(1,1,1,1) : ImVec4(0.7f,0.7f,0.7f,0.9f);
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0,0,0,0));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f,0.3f,0.3f,0.5f));
      ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.2f,0.2f,0.2f,0.7f));
      if (srv.ptr) {
        if (ImGui::ImageButton("##vis", (ImTextureID)srv.ptr, ImVec2(18, 18), ImVec2(0,0), ImVec2(1,1), ImVec4(0,0,0,0), tint)) {
          bool newState = !e->IsVisible();
          e->SetVisible(newState);
          if (e->IsFolder()) {
              applyToChildren(e, newState, [](std::shared_ptr<Entity> node, bool s) { node->SetVisible(s); });
          }
        }
      } else {
        if (ImGui::SmallButton(e->IsVisible() ? "V" : "-")) {
          bool newState = !e->IsVisible();
          e->SetVisible(newState);
          if (e->IsFolder()) {
              applyToChildren(e, newState, [](std::shared_ptr<Entity> node, bool s) { node->SetVisible(s); });
          }
        }
      }
      ImGui::PopStyleColor(3);
    }
    ImGui::SameLine(0, 2);

    // --- 鍵アイコン（ロック切り替え） ---
    {
      int texId = e->IsLocked() ? lockLockedTex_ : lockUnlockedTex_;
      auto srv = RC::GetRenderContext().Textures().GetSrv(texId);
      ImVec4 tint = e->IsLocked() ? ImVec4(1,0.8f,0.2f,1) : ImVec4(0.7f,0.7f,0.7f,0.9f);
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0,0,0,0));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f,0.3f,0.3f,0.5f));
      ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.2f,0.2f,0.2f,0.7f));
      if (srv.ptr) {
        if (ImGui::ImageButton("##lock", (ImTextureID)srv.ptr, ImVec2(18, 18), ImVec2(0,0), ImVec2(1,1), ImVec4(0,0,0,0), tint)) {
          e->SetLocked(!e->IsLocked());
        }
      } else {
        if (ImGui::SmallButton(e->IsLocked() ? "L" : "U")) {
          e->SetLocked(!e->IsLocked());
        }
      }
      ImGui::PopStyleColor(3);
    }
    ImGui::SameLine(0, 4);

    // フォルダアイコンの表示（isFolderの場合）
    if (e->IsFolder()) {
        auto srv = RC::GetRenderContext().Textures().GetSrv(folderIconTex_);
        if (srv.ptr) {
            ImGui::Image((ImTextureID)srv.ptr, ImVec2(16, 16));
            ImGui::SameLine(0, 4);
        }
    }

    auto it = childrenMap.find(e->Guid());
    bool hasChildren = (it != childrenMap.end() && !it->second.empty());

    // --- エンティティ名（垂直中央揃え） ---
    ImGui::AlignTextToFramePadding();
    bool isOpen = false;

    if (renamingEntityId_ == e->Id()) {
        char nameBuf[256];
        strncpy_s(nameBuf, sizeof(nameBuf), e->Name().c_str(), _TRUNCATE);

        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        if (focusRename_) {
            ImGui::SetKeyboardFocusHere();
            focusRename_ = false;
        }
        if (ImGui::InputText("##rename", nameBuf, sizeof(nameBuf), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
            e->SetName(nameBuf);
            renamingEntityId_ = 0;
        } else if (ImGui::IsItemDeactivated()) {
            e->SetName(nameBuf);
            renamingEntityId_ = 0;
        }
    } else {
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (!hasChildren) {
            flags |= ImGuiTreeNodeFlags_Leaf;
        }
        if (selectedEntity_.lock() == e) {
            flags |= ImGuiTreeNodeFlags_Selected;
        }

        // フォルダの場合はデフォルトで開く
        if (e->IsFolder()) flags |= ImGuiTreeNodeFlags_DefaultOpen;

        // 子を追加した直後の親は展開して新しい子を見せる。
        // 子が entities_ に反映される（Leaf でなくなる）まで待ってから開く。
        if (expandEntityGuid_ != 0 && expandEntityGuid_ == e->Guid() && hasChildren) {
            ImGui::SetNextItemOpen(true);
            expandEntityGuid_ = 0;
        }

        isOpen = ImGui::TreeNodeEx("##node", flags, "%s", e->Name().c_str());

        // ドラッグ元
        if (ImGui::BeginDragDropSource()) {
            uint64_t dragGuid = e->Guid();
            ImGui::SetDragDropPayload("HIERARCHY_ENTITY", &dragGuid, sizeof(uint64_t));
            ImGui::Text("Move %s", e->Name().c_str());
            ImGui::EndDragDropSource();
        }

        // ドロップ先 (フォルダの場合のみ)
        if (e->IsFolder()) {
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("HIERARCHY_ENTITY")) {
                    uint64_t draggedGuid = *(const uint64_t*)payload->Data;
                    auto draggedE = currentScene->FindEntityByGuid(draggedGuid);
                    if (draggedE && draggedE != e) {
                        // 循環参照チェック (自分がドラッグされた要素の子孫でないか)
                        bool isDescendant = false;
                        auto curr = e;
                        while (curr && curr->ParentGuid() != 0) {
                            if (curr->ParentGuid() == draggedGuid) { isDescendant = true; break; }
                            curr = currentScene->FindEntityByGuid(curr->ParentGuid());
                        }
                        if (!isDescendant) {
                            draggedE->SetParentGuid(e->Guid());
                        }
                    }
                }
                ImGui::EndDragDropTarget();
            }
        }

        if (ImGui::IsItemClicked(0) && !e->IsLocked()) {
            selectedEntity_ = e;
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0) && !e->IsLocked()) {
            renamingEntityId_ = e->Id();
            focusRename_ = true;
        }

        // 右クリックメニュー (Delete, Rename, Create Empty, Create Folder)
        if (!e->IsLocked()) {
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Rename")) {
                    renamingEntityId_ = e->Id();
                    focusRename_ = true;
                }
                if (ImGui::MenuItem("Create Empty inside")) {
                    CreateEmptyEntity(currentScene, e->Guid());
                }
                if (e->IsFolder() || true) {
                    if (ImGui::MenuItem("Create Folder inside")) {
                        auto folder = currentScene->CreateEntity("New Folder");
                        folder->SetIsFolder(true);
                        folder->SetParentGuid(e->Guid());
                        expandEntityGuid_ = e->Guid();
                    }
                }
                if (ImGui::MenuItem("Delete")) {
                    currentScene->DestroyEntityRecursive(e);
                    if (selectedEntity_.lock() == e) {
                        selectedEntity_.reset();
                    }
                }
                ImGui::EndPopup();
            }
        }
    }

    if (isOpen) {
        if (hasChildren) {
            for (auto& child : it->second) {
                DrawEntityNode(child, currentScene, childrenMap);
            }
        }
        ImGui::TreePop();
    }

    ImGui::PopID();
#endif
}

void EditorManager::DrawUI(D3D12_GPU_DESCRIPTOR_HANDLE viewportSrv, Dx12Core* core, PipelineManager* pm, float deltaTime, Scene* currentScene) {
#if RC_ENABLE_IMGUI
  // 撮影モード中はゲーム画面だけを描いて抜ける。
  // 他のパネルを Begin しなければ、そのまま画面から消える。
  if (CaptureMode::IsActive()) {
    CaptureMode::Draw(viewportSrv, core, currentScene, deltaTime);
    return;
  }

  ImGuizmo::BeginFrame();

  if (showDemoWindow_) {
    ImGui::ShowDemoWindow(&showDemoWindow_);
  }

  // シーン遷移表
  if (showSceneFlow_) {
    SceneFlowPanel::Draw(&showSceneFlow_);
  }

  // キー操作一覧（Help）
  if (showKeyHelp_) {
    KeyBindingsHelp::Draw(&showKeyHelp_);
  }

  // Application 側が EditorExtension::AddPanel で登録したパネル
  for (auto &panel : EditorExtension::Panels()) {
    if (!panel.open || !panel.draw) continue;
    if (ImGui::Begin(panel.name.c_str(), &panel.open)) {
      panel.draw(currentScene);
    }
    ImGui::End();
  }

  // Performance パネル
  if (showPerfWindow_) {
    if (ImGui::Begin("Performance", &showPerfWindow_)) {
      ImGuiIO &io = ImGui::GetIO();
      io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

      const float fps = io.Framerate;
      const float frameTime = 1000.0f / (fps > 0.0f ? fps : 1.0f);

      static float fpsDropTimer = 0.0f;
      if (fpsDropTimer > 0.0f) {
          fpsDropTimer -= io.DeltaTime;
      }
      // FPSが30を下回った場合に警告ログを出力 (起動直後などの0FPSは除外)
      if (fps > 0.0f && fps < 30.0f && fpsDropTimer <= 0.0f) {
          Log::Print(std::format("[Performance] Warning: FPS dropped to {:.1f} ({:.2f} ms)", fps, frameTime));
          fpsDropTimer = 5.0f; // 連続出力を防ぐためのクールダウン（5秒）
      }

      // フレームタイム・FPSの履歴バッファ
      static float fpsHistory[120] = {0};
      static float msHistory[120] = {0};
      static int historyIdx = 0;

      fpsHistory[historyIdx] = fps;
      msHistory[historyIdx] = frameTime;
      historyIdx = (historyIdx + 1) % 120;

      if (ImGui::CollapsingHeader("Timing & Performance", ImGuiTreeNodeFlags_DefaultOpen)) {
          ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "FPS: %.1f", fps);
          ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Frame Time: %.3f ms", frameTime);
          if (core) {
              // どちらがボトルネックかの切り分け用。CPU と GPU は並行して動くので、
              // おおよそ Frame Time ≒ max(CPU, GPU) + Capture（＋VSync 待ち）。
              // Wait（BeginFrame で GPU の完了を待った時間）が大きいほど GPU 側が詰まっている
              ImGui::Text("CPU: %.2f ms   GPU: %.2f ms   Wait(GPU): %.2f ms   Capture: %.2f ms",
                          core->CpuFrameMs(), core->GpuFrameMs(), core->GpuWaitMs(),
                          core->CaptureMs());
          }
          {
              const auto st = RC::GetModelInstancingStats();
              ImGui::Text("Instancing: %u drawn / %u batches / %u draws (flush %.2f ms)",
                          st.instancesDrawn, st.batches, st.drawCalls, st.flushMs);
              const auto ep = RC::GetEffectParticleStats();
              ImGui::Text("Effect particles: %u alive / %u drawn / %u draws (all passes)",
                          ep.alive, ep.drawn, ep.drawCalls);
          }
          // CPU 時間の内訳（前フレーム）。重い順に並べる。入れ子の区間は親にも含まれる
          if (ImGui::TreeNodeEx("CPU Breakdown (prev frame)", ImGuiTreeNodeFlags_DefaultOpen)) {
              const auto &prof = FrameProfiler::Get();
              const size_t n = prof.LastCount();
              std::array<size_t, FrameProfiler::kMaxEntries> order{};
              for (size_t i = 0; i < n; ++i) order[i] = i;
              std::sort(order.begin(), order.begin() + n, [&](size_t a, size_t b) {
                  return prof.Last()[a].ms > prof.Last()[b].ms;
              });
              if (ImGui::BeginTable("##cpuBreakdown", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
                  ImGui::TableSetupColumn("Section");
                  ImGui::TableSetupColumn("ms");
                  ImGui::TableSetupColumn("calls");
                  ImGui::TableHeadersRow();
                  for (size_t k = 0; k < n; ++k) {
                      const auto &e = prof.Last()[order[k]];
                      ImGui::TableNextRow();
                      ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(e.name);
                      ImGui::TableSetColumnIndex(1); ImGui::Text("%.2f", e.ms);
                      ImGui::TableSetColumnIndex(2); ImGui::Text("%u", e.calls);
                  }
                  ImGui::EndTable();
              }
              ImGui::TreePop();
          }

          ImGui::Separator();

          char overlayFps[32];
          sprintf_s(overlayFps, "Avg FPS: %.1f", fps);
          ImGui::PlotLines("##FPS", fpsHistory, 120, historyIdx, overlayFps, 0.0f, 120.0f, ImVec2(ImGui::GetContentRegionAvail().x, 60.0f));

          char overlayMs[32];
          sprintf_s(overlayMs, "Avg %.2f ms", frameTime);
          ImGui::PlotLines("##MS", msHistory, 120, historyIdx, overlayMs, 0.0f, 33.0f, ImVec2(ImGui::GetContentRegionAvail().x, 60.0f));
      }

      if (core) {
          if (ImGui::CollapsingHeader("Graphics Info", ImGuiTreeNodeFlags_DefaultOpen)) {
              ImGui::Text("Viewport: %.0f x %.0f", core->Viewport().Width, core->Viewport().Height);
              ImGui::Text("Frame Buffer Count: %u", core->FrameCount());
              ImGui::Text("Completed Fence: %llu", core->GetCompletedFenceValue());
          }
      }

      if (ImGui::CollapsingHeader("Editor Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
          if (core) {
              float currentFps = core->GetTargetFps();
              bool isFixed = currentFps > 0.0f;

              const char* fpsOptions[] = { "30 FPS", "60 FPS", "120 FPS", "144 FPS", "Uncapped" };
              float fpsValues[] = { 30.0f, 60.0f, 120.0f, 144.0f, 0.0f };

              int currentItem = 1; // default to 60 FPS
              if (!isFixed) {
                  currentItem = 4;
              } else {
                  for (int i = 0; i < 4; ++i) {
                      if (std::abs(fpsValues[i] - currentFps) < 0.1f) {
                          currentItem = i;
                          break;
                      }
                  }
              }

              if (ImGui::Combo("Target FPS", &currentItem, fpsOptions, 5)) {
                  float newFps = fpsValues[currentItem];
                  core->SetTargetFps(newFps);
                  core->EnableFixFps(newFps > 0.0f);
              }

              ImGui::Separator();

              // Resolution Settings
#if defined(_DEBUG) || defined(RC_DEVELOPMENT)
              const char* resOptions[] = {
                  "Borderless Fullscreen",
                  "1920 x 1080 (Borderless)",
                  "1600 x 900 (Borderless)",
                  "1280 x 720 (Borderless)"
              };
#else
              const char* resOptions[] = {
                  "Borderless Fullscreen",
                  "1920 x 1080 (Windowed)",
                  "1600 x 900 (Windowed)",
                  "1280 x 720 (Windowed)"
              };
#endif

              int currentResItem = 0;
              float w = core->Viewport().Width;
              float h = core->Viewport().Height;

              int screenW = GetSystemMetrics(SM_CXSCREEN);
              int screenH = GetSystemMetrics(SM_CYSCREEN);

              if (w == screenW && h == screenH) currentResItem = 0;
              else if (w == 1920 && h == 1080) currentResItem = 1;
              else if (w == 1600 && h == 900) currentResItem = 2;
              else if (w == 1280 && h == 720) currentResItem = 3;
              else currentResItem = 0; // fallback

              if (ImGui::Combo("Window Resolution", &currentResItem, resOptions, 4)) {
                  resizeRequest_.pending = true;
                  if (currentResItem == 0) {
                      resizeRequest_.width = screenW;
                      resizeRequest_.height = screenH;
                      resizeRequest_.fullscreen = true;
                  } else if (currentResItem == 1) {
                      resizeRequest_.width = 1920;
                      resizeRequest_.height = 1080;
                      resizeRequest_.fullscreen = false;
                  } else if (currentResItem == 2) {
                      resizeRequest_.width = 1600;
                      resizeRequest_.height = 900;
                      resizeRequest_.fullscreen = false;
                  } else if (currentResItem == 3) {
                      resizeRequest_.width = 1280;
                      resizeRequest_.height = 720;
                      resizeRequest_.fullscreen = false;
                  }
              }
              ImGui::Separator();
          }

          if (ImGui::Button("Save Window Layout")) { SaveConfig(); }
          ImGui::SameLine();
          if (ImGui::Button("Load Window Layout")) { LoadConfig(); }
      }
    }
    ImGui::End();
  }

  // Render Queue パネル
  if (showRenderQueue_) {
    if (ImGui::Begin("Render Queue", &showRenderQueue_)) {
      const auto& queue = RC::GetRenderContext().GetLastCommandHistory();
      ImGui::Text("Total Commands: %zu", queue.size());

      ImGui::SameLine();
      if (ImGui::Button("Export Dump to File")) {
        ExportRenderQueueDump();
      }

      ImGui::Separator();
      if (ImGui::CollapsingHeader("Command Execution Order (3D + 2D)", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::BeginTable("RenderQueueTable", 8, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
          ImGui::TableSetupColumn("Order", ImGuiTableColumnFlags_WidthFixed, 40.0f);
          ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
          ImGui::TableSetupColumn("Index", ImGuiTableColumnFlags_WidthFixed, 50.0f);
          ImGui::TableSetupColumn("SortKey", ImGuiTableColumnFlags_WidthFixed, 150.0f);
          ImGui::TableHeadersRow();

          for (size_t i = 0; i < queue.size(); ++i) {
            const auto& cmd = queue[i];

            std::string displayName(cmd.debugName); // debugName は string_view
            if (cmd.debugIndex >= 0) {
              std::string resourceName = "";
              if (cmd.debugName.find("Model") != std::string::npos) {
                if (auto* m = RC::GetRenderContext().Models().Get(cmd.debugIndex)) {
                  resourceName = std::filesystem::path(m->GetFilePath()).filename().string();
                }
              } else if (cmd.debugName.find("Sprite") != std::string::npos) {
                if (auto* s = RC::GetRenderContext().Sprites().Get(cmd.debugIndex)) {
                  resourceName = std::filesystem::path(s->GetFilePath()).filename().string();
                }
              }

              if (!resourceName.empty()) {
                displayName += " [" + resourceName + "]";
              }
            }

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("[%zu]", i);
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%s", displayName.c_str());
            ImGui::TableSetColumnIndex(2);
            if (cmd.debugIndex >= 0) {
              ImGui::Text("%d", cmd.debugIndex);
            } else {
              ImGui::Text("-");
            }
            ImGui::TableSetColumnIndex(3);
            if (cmd.sortKey != 0) {
              ImGui::Text("%016llX", cmd.sortKey);
            } else {
              ImGui::Text("-");
            }
            ImGui::TableSetColumnIndex(4);
            if (cmd.sortKey != 0) {
              uint8_t layer = static_cast<uint8_t>(cmd.sortKey >> 56);
              std::string layerStr = (layer == 0) ? "Opaque" : (layer == 1) ? "Alpha" : (layer == 2) ? "Glass" : (layer == 3) ? "Overlay" : "?";
              ImGui::Text("%d(%s)", layer, layerStr.c_str());
            } else { ImGui::Text("-"); }
            ImGui::TableSetColumnIndex(5);
            if (cmd.sortKey != 0) {
              uint32_t depth24 = static_cast<uint32_t>(cmd.sortKey & 0x00FFFFFF);
              ImGui::Text("%u", depth24);
            } else { ImGui::Text("-"); }
            ImGui::TableSetColumnIndex(6);
            if (cmd.sortKey != 0) {
              uint16_t psoHash = static_cast<uint16_t>((cmd.sortKey >> 40) & 0xFFFF);
              ImGui::Text("%04X", psoHash);
            } else { ImGui::Text("-"); }
            ImGui::TableSetColumnIndex(7);
            if (cmd.sortKey != 0) {
              uint16_t texHash = static_cast<uint16_t>((cmd.sortKey >> 24) & 0xFFFF);
              ImGui::Text("%04X", texHash);
            } else { ImGui::Text("-"); }
          }
          ImGui::EndTable();
        }
      }
    }
    ImGui::End();
  }

  // Environment Settings パネル
  if (showEnvironmentWindow_) {
    if (ImGui::Begin("Environment Settings (環境設定)", &showEnvironmentWindow_)) {
      if (!currentScene) {
        ImGui::Text("No active scene.");
      } else {
        std::shared_ptr<Entity> skyEntity = nullptr;
        for (auto& e : currentScene->GetEntities()) {
          if (e->HasComponent<SkyboxComponent>() || e->HasComponent<SkydomeComponent>()) {
            skyEntity = e;
            break;
          }
        }

        if (skyEntity) {
          ImGui::Text("Current Environment Entity: %s", skyEntity->GetName().c_str());
          if (ImGui::Button("Select in Hierarchy")) {
            selectedEntity_ = skyEntity;
          }
          ImGui::Separator();
          ImGui::TextDisabled("Select the entity to edit details in the Inspector.");
        } else {
          ImGui::Text("No Skybox or Skydome in the scene.");
          ImGui::Separator();
          if (ImGui::Button("Create Skydome")) {
            auto e = currentScene->CreateEntity("Environment (Skydome)");
            e->AddComponent<TransformComponent>();
            auto& sd = e->AddComponent<SkydomeComponent>();
            sd.skydomeHandle = RC::GenerateSkydomeEx(-1);
            selectedEntity_ = e;
          }
          if (ImGui::Button("Create Skybox")) {
            auto e = currentScene->CreateEntity("Environment (Skybox)");
            e->AddComponent<TransformComponent>();
            e->AddComponent<SkyboxComponent>();
            selectedEntity_ = e;
          }
        }
      }
    }
    ImGui::End();
  }

  // Post Effect Settings パネル
  if (showPostEffectWindow_) {
    if (ImGui::Begin("Post Effect Settings", &showPostEffectWindow_)) {
      if (auto* postProcess = RC::GetRenderContext().GetPostProcess()) {
          postProcess->DrawImGui("Post Process Effects");
      } else {
          ImGui::Text("No PostProcess instance.");
      }
    }
    ImGui::End();
  }

  // Viewport パネル
  // カーソルロック（視点操作）の戻し先。Viewport 画像が描かれた場合だけ下で画像中央を指定し、
  // 描かれなかったフレームはウィンドウ中央に戻す。
  if (auto* input = Input::GetInstance()) input->ClearCursorLockCenter();
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0)); // 余白なし
  if (ImGui::Begin("Viewport")) {
    isViewportHovered_ = ImGui::IsWindowHovered();
    ImVec2 vMin = ImGui::GetWindowContentRegionMin();
    ImVec2 vMax = ImGui::GetWindowContentRegionMax();
    vMin.x += ImGui::GetWindowPos().x;
    vMin.y += ImGui::GetWindowPos().y;
    vMax.x += ImGui::GetWindowPos().x;
    vMax.y += ImGui::GetWindowPos().y;

    float width = vMax.x - vMin.x;
    float height = vMax.y - vMin.y;

    // パネル内に収まる 16:9 の矩形を計算して中央に配置する（レターボックス）
    constexpr float kViewportAspect = 16.0f / 9.0f;
    if (width > 0 && height > 0) {
      float imgW = width;
      float imgH = imgW / kViewportAspect;
      if (imgH > height) {
        imgH = height;
        imgW = imgH * kViewportAspect;
      }
      // 余白（レターボックス）部分を黒で塗りつぶす
      ImGui::GetWindowDrawList()->AddRectFilled(vMin, vMax, IM_COL32(0, 0, 0, 255));
      // 以降の処理（マウス座標変換・ピッキング・ギズモ）はすべて画像矩形基準で行う
      vMin.x += (width - imgW) * 0.5f;
      vMin.y += (height - imgH) * 0.5f;
      vMax.x = vMin.x + imgW;
      vMax.y = vMin.y + imgH;
      width = imgW;
      height = imgH;
    }

      // ゲーム描画用SRVをImGuiのImageとして表示
    if (viewportSrv.ptr != 0 && width > 0 && height > 0) {
      ImGui::SetCursorScreenPos(vMin); // 中央寄せした位置から描画
      ImGui::Image((ImTextureID)viewportSrv.ptr, ImVec2(width, height));
      bool isHoveringImage = ImGui::IsItemHovered();

      // ===== Mouse Position Update for Game =====
      float currentMouseX = ImGui::GetMousePos().x - vMin.x;
      float currentMouseY = ImGui::GetMousePos().y - vMin.y;
      if (core) {
          float gameW = core->Viewport().Width;
          float gameH = core->Viewport().Height;
          float scaledX = (currentMouseX / width) * gameW;
          float scaledY = (currentMouseY / height) * gameH;
          if (auto input = Input::GetInstance()) {
              input->SetGameMousePosition(scaledX, scaledY);
              // ImGui の座標はクライアント座標（マルチビューポート無効）なので、そのまま渡せる
              input->SetCursorLockCenter((vMin.x + vMax.x) * 0.5f, (vMin.y + vMax.y) * 0.5f);
          }
      }

      // ===== 2D 要素（Text / Sprite）の枠表示・クリック選択・ドラッグ移動 =====
      // 3D のギズモの代わりに、スクリーン座標の矩形を直接ドラッグして位置を変える
      bool consumed2DClick = false;
      if (currentScene && (!currentScene->GetContext() || !currentScene->GetContext()->isPlaying())) {
          float gameW = width, gameH = height;
          if (core) { gameW = core->Viewport().Width; gameH = core->Viewport().Height; }
          else if (auto* sctx = RC::GetRenderContext().Ctx(); sctx && sctx->app) { gameW = (float)sctx->app->width; gameH = (float)sctx->app->height; }
          const float sx = (gameW > 0.0f) ? width / gameW : 1.0f;
          const float sy = (gameH > 0.0f) ? height / gameH : 1.0f;
          auto toScreen = [&](const RC::Vector2& g) { return ImVec2(vMin.x + g.x * sx, vMin.y + g.y * sy); };

          ImDrawList* dl = ImGui::GetWindowDrawList();
          const ImVec2 mouse = ImGui::GetMousePos();
          auto selected = selectedEntity_.lock();

          // ホバー判定（後ろ＝上に描かれるものを優先）
          std::shared_ptr<Entity> hovered2D;
          const auto& ents = currentScene->GetEntities();
          for (auto it = ents.rbegin(); it != ents.rend(); ++it) {
              const auto& e = *it;
              if (!e || e->IsPendingDestroy() || !e->IsVisible() || !e->IsActive()) continue;
              RC::Vector2 mn, mx;
              if (!Get2DRect(*e, mn, mx)) continue;
              const ImVec2 a = toScreen(mn), b = toScreen(mx);
              if (isHoveringImage && mouse.x >= a.x && mouse.x <= b.x && mouse.y >= a.y && mouse.y <= b.y) {
                  hovered2D = e;
                  break;
              }
          }

          // 選択中の 2D 要素の矩形と四隅ハンドルのホバー判定
          const float kHandleHit = 7.0f; // ハンドルの当たり半径（画面 px）
          int hoveredCorner = -1;        // 0=左上 1=右上 2=左下 3=右下
          RC::Vector2 selMin{}, selMax{};
          const bool selectedIs2D = selected && Is2DEntity(*selected) && selected->IsVisible() && Get2DRect(*selected, selMin, selMax);
          if (selectedIs2D && isHoveringImage) {
              const ImVec2 a = toScreen(selMin), b = toScreen(selMax);
              const ImVec2 corners[4] = { a, ImVec2(b.x, a.y), ImVec2(a.x, b.y), b };
              for (int i = 0; i < 4; ++i) {
                  if (std::fabs(mouse.x - corners[i].x) <= kHandleHit && std::fabs(mouse.y - corners[i].y) <= kHandleHit) {
                      hoveredCorner = i;
                      break;
                  }
              }
          }

          // 枠の描画
          for (const auto& e : ents) {
              if (!e || e->IsPendingDestroy() || !e->IsVisible() || !e->IsActive()) continue;
              if (e != selected && e != hovered2D) continue;
              RC::Vector2 mn, mx;
              if (!Get2DRect(*e, mn, mx)) continue;
              const ImVec2 a = toScreen(mn), b = toScreen(mx);
              if (e == selected) {
                  dl->AddRect(a, b, IM_COL32(255, 170, 40, 255), 0.0f, 0, 2.0f);
                  // 四隅ハンドル（ドラッグでサイズ変更）
                  const ImVec2 corners[4] = { a, ImVec2(b.x, a.y), ImVec2(a.x, b.y), b };
                  for (int i = 0; i < 4; ++i) {
                      const bool hot = (hoveredCorner == i) || (drag2DMode_ == 2 && dragging2D_);
                      const float hs = hot ? 6.0f : 4.0f;
                      const ImVec2& c = corners[i];
                      dl->AddRectFilled(ImVec2(c.x - hs, c.y - hs), ImVec2(c.x + hs, c.y + hs),
                                        hot ? IM_COL32(255, 255, 255, 255) : IM_COL32(255, 170, 40, 255));
                  }
                  // 位置・サイズラベル
                  if (auto* tr = e->GetComponent<TransformComponent>()) {
                      char buf[96];
                      if (auto* txt = e->GetComponent<TextRendererComponent>()) {
                          snprintf(buf, sizeof(buf), "(%.0f, %.0f)  %.0fpx x%.2f", tr->position.x, tr->position.y, txt->fontSize, txt->scale);
                      } else {
                          snprintf(buf, sizeof(buf), "(%.0f, %.0f)  %.0f x %.0f", tr->position.x, tr->position.y, mx.x - mn.x, mx.y - mn.y);
                      }
                      dl->AddText(ImVec2(a.x, a.y - 16.0f), IM_COL32(255, 220, 150, 255), buf);
                  }
              } else {
                  dl->AddRect(a, b, IM_COL32(255, 255, 255, 120), 0.0f, 0, 1.0f);
              }
          }

          // クリック：ハンドル上ならサイズ変更開始、要素上なら選択して移動開始
          if (isHoveringImage && ImGui::IsMouseClicked(0) && !ImGuizmo::IsOver()) {
              if (selectedIs2D && hoveredCorner >= 0 && !selected->IsLocked()) {
                  dragging2D_ = true;
                  drag2DMode_ = 2;
                  // 反対側のコーナーを固定点にする
                  drag2DAnchorX_ = (hoveredCorner == 0 || hoveredCorner == 2) ? selMax.x : selMin.x;
                  drag2DAnchorY_ = (hoveredCorner == 0 || hoveredCorner == 1) ? selMax.y : selMin.y;
                  drag2DBaseW_ = (std::max)(selMax.x - selMin.x, 1.0f);
                  drag2DBaseH_ = (std::max)(selMax.y - selMin.y, 1.0f);
                  if (auto* txt = selected->GetComponent<TextRendererComponent>()) drag2DBaseScale_ = txt->scale;
                  else drag2DBaseScale_ = 1.0f;
                  consumed2DClick = true;
              } else if (hovered2D) {
                  if (!hovered2D->IsLocked()) {
                      selectedEntity_ = hovered2D;
                      dragging2D_ = true;
                      drag2DMode_ = 1;
                      drag2DLastX_ = mouse.x;
                      drag2DLastY_ = mouse.y;
                  }
                  consumed2DClick = true;
              }
          }

          // ドラッグ中の更新
          if (dragging2D_) {
              auto sel = selectedEntity_.lock();
              if (!ImGui::IsMouseDown(0) || !sel || !Is2DEntity(*sel)) {
                  // 終了：Text のサイズ変更は scale を fontSize に焼き込んで等倍に戻す（にじみ防止）
                  if (drag2DMode_ == 2 && sel) {
                      if (auto* txt = sel->GetComponent<TextRendererComponent>()) {
                          if (std::fabs(txt->scale - 1.0f) > 1e-3f) {
                              txt->fontSize = (std::max)(4.0f, std::round(txt->fontSize * txt->scale));
                              txt->scale = 1.0f;
                          }
                      }
                  }
                  dragging2D_ = false;
                  drag2DMode_ = 0;
              } else if (!sel->IsLocked()) {
                  auto* tr = sel->GetComponent<TransformComponent>();
                  if (drag2DMode_ == 1 && tr) {
                      // 移動（ゲーム解像度のピクセル単位に変換して加算）
                      tr->position.x += (mouse.x - drag2DLastX_) / sx;
                      tr->position.y += (mouse.y - drag2DLastY_) / sy;
                      if (ImGui::GetIO().KeyShift) { // Shift でピクセル吸着
                          tr->position.x = std::round(tr->position.x);
                          tr->position.y = std::round(tr->position.y);
                      }
                      drag2DLastX_ = mouse.x;
                      drag2DLastY_ = mouse.y;
                  } else if (drag2DMode_ == 2 && tr) {
                      // サイズ変更：固定コーナーとマウス位置で新しい矩形を決める
                      const float gx = (mouse.x - vMin.x) / sx;
                      const float gy = (mouse.y - vMin.y) / sy;
                      float newW = (std::max)(std::fabs(gx - drag2DAnchorX_), 1.0f);
                      float newH = (std::max)(std::fabs(gy - drag2DAnchorY_), 1.0f);
                      if (auto* txt = sel->GetComponent<TextRendererComponent>()) {
                          // 文字は等比：幅と高さのうち大きい方の比率で scale を決める
                          const float ratio = (std::max)(newW / drag2DBaseW_, newH / drag2DBaseH_);
                          txt->scale = (std::max)(0.05f, drag2DBaseScale_ * ratio);
                          newW = drag2DBaseW_ * ratio;
                          newH = drag2DBaseH_ * ratio;
                          const float newMinX = (gx < drag2DAnchorX_) ? drag2DAnchorX_ - newW : drag2DAnchorX_;
                          const float newMinY = (gy < drag2DAnchorY_) ? drag2DAnchorY_ - newH : drag2DAnchorY_;
                          // 揃えに応じて position.x を矩形から逆算
                          if (txt->align == TextAlign::Center) tr->position.x = newMinX + newW * 0.5f;
                          else if (txt->align == TextAlign::Right) tr->position.x = newMinX + newW;
                          else tr->position.x = newMinX;
                          tr->position.y = newMinY;
                      } else if (auto* spr = sel->GetComponent<SpriteRendererComponent>()) {
                          if (ImGui::GetIO().KeyShift) { // Shift で縦横比維持
                              const float ratio = (std::max)(newW / drag2DBaseW_, newH / drag2DBaseH_);
                              newW = drag2DBaseW_ * ratio;
                              newH = drag2DBaseH_ * ratio;
                          }
                          spr->size = { newW, newH };
                          tr->position.x = (gx < drag2DAnchorX_) ? drag2DAnchorX_ - newW : drag2DAnchorX_;
                          tr->position.y = (gy < drag2DAnchorY_) ? drag2DAnchorY_ - newH : drag2DAnchorY_;
                      }
                  }
                  if (ImGui::IsMouseDragging(0)) consumed2DClick = true;
              }
          }

          // カーソル
          if (isHoveringImage) {
              if (hoveredCorner >= 0 || (dragging2D_ && drag2DMode_ == 2)) {
                  const int c = (hoveredCorner >= 0) ? hoveredCorner : 3;
                  ImGui::SetMouseCursor((c == 0 || c == 3) ? ImGuiMouseCursor_ResizeNWSE : ImGuiMouseCursor_ResizeNESW);
              } else if (hovered2D || (dragging2D_ && drag2DMode_ == 1)) {
                  ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
              }
          }
      } else {
          dragging2D_ = false;
          drag2DMode_ = 0;
      }

      // ===== Mouse Picking =====
      if (!consumed2DClick && ImGui::IsMouseClicked(0) && isHoveringImage && !ImGui::IsMouseDragging(0) && currentScene &&
          (!currentScene->GetContext() || !currentScene->GetContext()->isPlaying()) &&
          !ImGuizmo::IsOver()) {
          float mouseX = ImGui::GetMousePos().x - vMin.x;
          float mouseY = ImGui::GetMousePos().y - vMin.y;
          RC::CameraController* cam = RC::GetRenderContext().Ctx()->camera;
          if (cam) {
              RC::Vector2 mousePosVec = { mouseX, mouseY };
              RC::Vector2 screenSize = { width, height };
              RC::Matrix4x4 view = cam->GetView();
              RC::Matrix4x4 proj = cam->GetProjection();
              RC::Ray ray = RC::CameraMath::ScreenPointToRay(mousePosVec, screenSize, view, proj);

              float minHitDistance = 999999.0f;
              std::shared_ptr<Entity> hitEntity = nullptr;

              for (const auto& e : currentScene->GetEntities()) {
                  if (!e->IsVisible()) continue;
                  if (Is2DEntity(*e)) continue; // 2D 要素は上の矩形判定で扱う
                  auto* tr = e->GetComponent<TransformComponent>();
                  if (!tr) continue;

                  float dist = 0.0f;
                  bool hit = false;

                  if (auto* col = e->GetComponent<ColliderComponent>()) {
                      if (col->IsEnabled()) {
                          RC::Vector3 scaledCenter = {
                              col->center.x * tr->scale.x,
                              col->center.y * tr->scale.y,
                              col->center.z * tr->scale.z
                          };
                          RC::Vector3 worldCenter = {
                              tr->position.x + scaledCenter.x,
                              tr->position.y + scaledCenter.y,
                              tr->position.z + scaledCenter.z
                          };
                          if (col->shape == ColliderComponent::Shape::Sphere) {
                              float maxScale = (std::max)((std::max)(std::abs(tr->scale.x), std::abs(tr->scale.y)), std::abs(tr->scale.z));
                              hit = RC::IntersectRaySphere(ray, worldCenter, col->radius * maxScale, dist);
                          } else if (col->shape == ColliderComponent::Shape::AABB) {
                              RC::Vector3 scaledSize = {
                                  std::abs(col->size.x * tr->scale.x),
                                  std::abs(col->size.y * tr->scale.y),
                                  std::abs(col->size.z * tr->scale.z)
                              };
                              RC::Vector3 halfSize = { scaledSize.x * 0.5f, scaledSize.y * 0.5f, scaledSize.z * 0.5f };
                              hit = RC::IntersectRayAABB(ray, ::Subtract(worldCenter, halfSize), ::Add(worldCenter, halfSize), dist);
                          }
                      }
                  } else if (auto* mr = e->GetComponent<ModelRendererComponent>()) {
                      if (mr->visible && mr->IsEnabled()) {
                          hit = RC::IntersectRaySphere(ray, tr->position, 1.0f, dist);
                      }
                  } else if (auto* pm = e->GetComponent<PrimitiveMeshComponent>()) {
                      if (pm->visible && pm->IsEnabled()) {
                          hit = RC::IntersectRaySphere(ray, tr->position, 1.0f, dist);
                      }
                  } else if (auto* tm = e->GetComponent<TextMeshComponent>()) {
                      if (tm->visible && tm->IsEnabled() && tm->HasMesh()) {
                          // 生成時のローカル AABB を位置・スケールに合わせて拡げた箱で判定（回転は無視）
                          const RC::Vector3 sc = { std::abs(tr->scale.x), std::abs(tr->scale.y), std::abs(tr->scale.z) };
                          RC::Vector3 lo = { tm->info.min.x * sc.x, tm->info.min.y * sc.y, tm->info.min.z * sc.z };
                          RC::Vector3 hi = { tm->info.max.x * sc.x, tm->info.max.y * sc.y, tm->info.max.z * sc.z };
                          // 厚さ 0 でも掴めるように最小厚を確保
                          if (hi.z - lo.z < 0.1f) { lo.z -= 0.05f; hi.z += 0.05f; }
                          hit = RC::IntersectRayAABB(ray, ::Add(tr->position, lo), ::Add(tr->position, hi), dist);
                      }
                  }

                  if (hit && dist >= 0.0f && dist < minHitDistance) {
                      minHitDistance = dist;
                      hitEntity = e;
                  }
              }

              if (hitEntity && !hitEntity->IsLocked()) {
                  selectedEntity_ = hitEntity;
              } else if (!hitEntity) {
                  selectedEntity_.reset();
              }
          }
      }

      // ===== Drop Target =====
      if (ImGui::BeginDragDropTarget()) {
          if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM")) {
              std::string droppedPath((const char*)payload->Data);
              std::filesystem::path p(droppedPath);
              std::string ext = p.extension().string();
              std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

              if (currentScene) {
                  auto e = currentScene->CreateEntity(p.stem().string());
                  auto& tr = e->AddComponent<TransformComponent>();

                  // ドラッグ先の座標計算 (Screen to World)
                  float mouseX = ImGui::GetMousePos().x - vMin.x;
                  float mouseY = ImGui::GetMousePos().y - vMin.y;
                  RC::Vector3 dropPos = {0, 0, 0};

                  // エディタ（またはゲーム）の現在のカメラを取得
                  RC::CameraController* cam = RC::GetRenderContext().Ctx()->camera;
                  if (cam) {
                      // NDC座標 (-1.0 ～ 1.0)
                      float ndcX = (2.0f * mouseX) / width - 1.0f;
                      float ndcY = 1.0f - (2.0f * mouseY) / height;

                      // ビュー・プロジェクション行列の計算
                      RC::Matrix4x4 view = cam->GetView();
                      RC::Matrix4x4 proj = cam->GetProjection();
                      RC::Matrix4x4 viewProj = ::Multiply(view, proj);
                      RC::Matrix4x4 invViewProj = ::Inverse(viewProj);

                      // Far平面上の点を計算
                      RC::Vector3 farPoint = ::Vector3Transform({ndcX, ndcY, 1.0f}, invViewProj);
                      RC::Vector3 camPos = cam->GetWorldPos();

                      // カメラからFar点へのレイ
                      RC::Vector3 rayDir = ::Normalize(::Subtract(farPoint, camPos));

                      // Y=0 の平面（地面）との交差を求める
                      if (std::abs(rayDir.y) > 0.001f) {
                          float t = -camPos.y / rayDir.y;
                          if (t > 0.0f) {
                              dropPos = ::Add(camPos, ::Multiply(rayDir, t));
                          } else {
                              dropPos = ::Add(camPos, ::Multiply(rayDir, 10.0f)); // カメラの後ろ側を向いてる場合は適当に前に置く
                          }
                      } else {
                          dropPos = ::Add(camPos, ::Multiply(rayDir, 10.0f)); // 水平に見ている場合は適当に前に置く
                      }
                  }
                  tr.position = dropPos;

                  if (ext == ".gltf" || ext == ".obj") {
                      auto& ren = e->AddComponent<ModelRendererComponent>();
                      ren.modelPath = droppedPath;
                      ren.modelHandle = RC::LoadModel(p.string());
                      // アニメーションを含むモデルには AnimationComponent を自動で付ける。
                      // モデルのロードは非同期なので、ハンドル経由ではなくファイルを直接見て判定する。
                      if (RC::GetAnimationCount(p.string()) > 0) {
                          e->AddComponent<AnimationComponent>();
                      }
                  } else if (ext == ".png" || ext == ".jpg" || ext == ".dds") {
                      // 画像はスプライトとして追加。位置はドロップしたスクリーン座標（ピクセル）
                      auto& spr = e->AddComponent<SpriteRendererComponent>();
                      spr.spritePath = droppedPath;
                      float gameW = width, gameH = height;
                      if (core) { gameW = core->Viewport().Width; gameH = core->Viewport().Height; }
                      const float gx = (width > 0.0f) ? mouseX / width * gameW : 0.0f;
                      const float gy = (height > 0.0f) ? mouseY / height * gameH : 0.0f;
                      tr.position = { gx - spr.size.x * 0.5f, gy - spr.size.y * 0.5f, 0.0f };
                      tr.rotation = { 0.0f, 0.0f, 0.0f };
                      tr.scale = { 1.0f, 1.0f, 1.0f };
                      // 実際のロードは DataDrivenScene の描画ループで行われる（spritePath を見て遅延ロード）
                      selectedEntity_ = e;
                  }
              }
          }
          ImGui::EndDragDropTarget();
      }
      // =======================

      // シェーディングモードのアイコン群をビューポート右上にオーバーレイ表示
      // 描画開始位置を決定 (上部のバーと重ならないようY座標を少し下げる)
      ImGui::SetCursorScreenPos(ImVec2(vMax.x - 186.0f, vMin.y + 24.0f));
      ImVec2 cursorPos = ImGui::GetCursorScreenPos();

      // 幅は「ボタン6個(24px) + 隙間5個(4px) = 164px」に左右余白6pxずつ足して 176px にする
      ImVec2 overlaySize = ImVec2(176.0f, 32.0f);

      // 再生中以外の場合のみ、ギズモ描画と各種オーバーレイUIを表示する
      if (playState_ != PlayState::Playing) {
          // ボタン群（高さ24px）の背景として、上下左右に余白を持たせた半透明の枠を描画
          // cursorPos は最初のボタンの左上絶対座標。枠は少し左・上にずらして描画する。
          ImGui::GetWindowDrawList()->AddRectFilled(
              ImVec2(cursorPos.x - 6.0f, cursorPos.y - 4.0f),
              ImVec2(cursorPos.x - 6.0f + overlaySize.x, cursorPos.y - 4.0f + overlaySize.y),
              IM_COL32(20, 20, 20, 160),
              6.0f
          );

          // シェーディングモード切替ボタンの描画を実行
          RC::DrawViewShadingModeImGui("");

          // ギズモ操作モード用UIを左上に描画
          ImGui::SetCursorScreenPos(ImVec2(vMin.x + 10.0f, vMin.y + 24.0f));
          ImVec2 leftCursorPos = ImGui::GetCursorScreenPos();
          ImVec2 leftOverlaySize = ImVec2(156.0f, 32.0f); // 枠の幅を少し広げてはみ出しを修正
          ImGui::GetWindowDrawList()->AddRectFilled(
              ImVec2(leftCursorPos.x - 6.0f, leftCursorPos.y - 4.0f),
              ImVec2(leftCursorPos.x - 6.0f + leftOverlaySize.x, leftCursorPos.y - 4.0f + leftOverlaySize.y),
              IM_COL32(20, 20, 20, 160),
              6.0f
          );

          ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 0.0f));
          if (ImGui::Button("T", ImVec2(24, 24))) gizmoOperation_ = 7; // TRANSLATE
          if (ImGui::IsItemHovered()) ImGui::SetTooltip("Translate");
          ImGui::SameLine();
          if (ImGui::Button("R", ImVec2(24, 24))) gizmoOperation_ = 120; // ROTATE
          if (ImGui::IsItemHovered()) ImGui::SetTooltip("Rotate");
          ImGui::SameLine();
          if (ImGui::Button("S", ImVec2(24, 24))) gizmoOperation_ = 896; // SCALE
          if (ImGui::IsItemHovered()) ImGui::SetTooltip("Scale");
          ImGui::SameLine();
          ImGui::Text("|");
          ImGui::SameLine();
          if (ImGui::Button("L", ImVec2(24, 24))) gizmoMode_ = 0; // LOCAL
          if (ImGui::IsItemHovered()) ImGui::SetTooltip("Local");
          ImGui::SameLine();
          if (ImGui::Button("W", ImVec2(24, 24))) gizmoMode_ = 1; // WORLD
          if (ImGui::IsItemHovered()) ImGui::SetTooltip("World");
          ImGui::PopStyleVar();

          // ImGuizmo のセットアップ
          ImGuizmo::SetDrawlist();
          ImGuizmo::SetRect(vMin.x, vMin.y, width, height);

          // 選択されているオブジェクトがあればギズモを表示
          if (auto hitEntity = selectedEntity_.lock(); hitEntity && !Is2DEntity(*hitEntity)) {
              if (auto* tr = hitEntity->GetComponent<TransformComponent>()) {
                  RC::CameraController* cam = RC::GetRenderContext().Ctx()->camera;
                  if (cam) {
                      RC::Matrix4x4 view = cam->GetView();
                      RC::Matrix4x4 proj = cam->GetProjection();

                      float* viewPtr = reinterpret_cast<float*>(&view);
                      float* projPtr = reinterpret_cast<float*>(&proj);

                      RC::Matrix4x4 worldMat = MakeAffineMatrix(tr->scale, tr->rotation, tr->position);
                      float* matrixPtr = reinterpret_cast<float*>(&worldMat);

                      ImGuizmo::Manipulate(viewPtr, projPtr, (ImGuizmo::OPERATION)gizmoOperation_, (ImGuizmo::MODE)gizmoMode_, matrixPtr);

                      if (ImGuizmo::IsUsing()) {
                          float translation[3], rotation[3], scale[3];
                          ImGuizmo::DecomposeMatrixToComponents(matrixPtr, translation, rotation, scale);

                          tr->position = {translation[0], translation[1], translation[2]};
                          tr->rotation = {rotation[0] * 3.14159265f / 180.0f, rotation[1] * 3.14159265f / 180.0f, rotation[2] * 3.14159265f / 180.0f};
                          tr->scale = {scale[0], scale[1], scale[2]};
                      }
                  }
              }
          }
      }
    } else {
      ImGui::Text("No Viewport Texture");
    }
  }
  ImGui::End();
  ImGui::PopStyleVar();

  // Hierarchy パネル
  if (ImGui::Begin("Hierarchy")) {
    if (currentScene) {
      // 親子関係のマップを構築
      std::unordered_map<uint64_t, std::vector<std::shared_ptr<Entity>>> childrenMap;
      std::vector<std::shared_ptr<Entity>> rootEntities;

      for (const auto& e : currentScene->GetEntities()) {
        if (!e || e->IsPendingDestroy()) continue;
        if (e->HasTag("transient") || e->HasTag("hide_in_hierarchy")) continue;
        if (e->ParentGuid() == 0 || currentScene->FindEntityByGuid(e->ParentGuid()) == nullptr) {
            rootEntities.push_back(e);
        } else {
            childrenMap[e->ParentGuid()].push_back(e);
        }
      }

      ImGui::BeginChild("HierarchyList", ImVec2(0, 0), false);
      for (const auto& e : rootEntities) {
          DrawEntityNode(e, currentScene, childrenMap);
      }

      // 何もない領域での右クリックメニュー（空オブジェクト・新規フォルダ作成など）
      if (ImGui::BeginPopupContextWindow("HierarchyBgContext", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
          if (ImGui::MenuItem("Create Empty")) {
              CreateEmptyEntity(currentScene, 0);
          }
          if (ImGui::MenuItem("Create Folder")) {
              auto folder = currentScene->CreateEntity("New Folder");
              folder->SetIsFolder(true);
              folder->SetParentGuid(0);
          }
          ImGui::EndPopup();
      }

      // ルート領域へのドロップ対応（ルート階層に移動）
      ImGui::Dummy(ImGui::GetContentRegionAvail());
      if (ImGui::BeginDragDropTarget()) {
          if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("HIERARCHY_ENTITY")) {
              uint64_t draggedGuid = *(const uint64_t*)payload->Data;
              auto draggedE = currentScene->FindEntityByGuid(draggedGuid);
              if (draggedE) {
                  draggedE->SetParentGuid(0); // ルートに移動
              }
          }
          ImGui::EndDragDropTarget();
      }
      ImGui::EndChild();
    }
  }
  ImGui::End();

  // Inspector パネル
  if (ImGui::Begin("Inspector")) {
    if (auto e = selectedEntity_.lock()) {
        std::function<void()> pendingRemove;
        char nameBuf[256];
        strncpy_s(nameBuf, sizeof(nameBuf), e->Name().c_str(), _TRUNCATE);

        bool active = e->IsActive();
        if (ImGui::Checkbox("##InspectorActive", &active)) {
            e->SetActive(active);
        }
        ImGui::SameLine();

        ImGui::Text("Entity:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        if (ImGui::InputText("##InspectorRename", nameBuf, sizeof(nameBuf), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
            e->SetName(nameBuf);
        } else if (ImGui::IsItemDeactivatedAfterEdit()) {
            e->SetName(nameBuf);
        }
        ImGui::Separator();

        if (ImGui::CollapsingHeader("Tags (タグ)", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Indent(8.0f);
            auto& tags = e->GetTagsRef();

            std::string tagToRemove = "";
            for (auto& [key, val] : tags) {
                ImGui::PushID(key.c_str());

                // タグ名と削除ボタンのみを表示 (値は隠蔽してシンプルにする)
                ImGui::AlignTextToFramePadding();
                ImGui::Text(" %s ", key.c_str());
                ImGui::SameLine(ImGui::GetContentRegionAvail().x - 22.0f);
                if (ImGui::Button("X", ImVec2(22, 0))) {
                    tagToRemove = key;
                }
                ImGui::PopID();
            }
            if (!tagToRemove.empty()) {
                tags.erase(tagToRemove);
            }

            ImGui::Separator();

            // Collect known tags
            std::set<std::string> knownTags = { "is_enemy", "is_player", "is_terrain", "is_item", "pending_damage", "impact_factor", "reused", "Shark", "Enemy" };
            if (currentScene) {
                for (const auto& sceneEntity : currentScene->GetEntities()) {
                    if (!sceneEntity) continue;
                    for (const auto& [k, v] : sceneEntity->GetTags()) {
                        knownTags.insert(k);
                    }
                }
            }

            std::vector<std::string> tagList;
            tagList.push_back("--- Select a tag ---");
            for (const auto& k : knownTags) {
                tagList.push_back(k);
            }
            tagList.push_back("+ New Tag...");

            static int selectedTagIdx = 0;
            static char newTagKey[64] = "";

            if (selectedTagIdx >= tagList.size()) selectedTagIdx = 0;

            const char* currentLabel = tagList[selectedTagIdx].c_str();

            bool isNewTagMode = (selectedTagIdx == tagList.size() - 1);
            float comboWidth = isNewTagMode ? ImGui::GetContentRegionAvail().x * 0.45f : ImGui::GetContentRegionAvail().x - 50.0f;

            ImGui::SetNextItemWidth(comboWidth);
            if (ImGui::BeginCombo("##TagSelector", currentLabel)) {
                for (int i = 0; i < tagList.size(); ++i) {
                    bool isSelected = (selectedTagIdx == i);
                    if (ImGui::Selectable(tagList[i].c_str(), isSelected)) {
                        selectedTagIdx = i;
                        if (i > 0 && i < tagList.size() - 1) {
                            strncpy_s(newTagKey, sizeof(newTagKey), tagList[i].c_str(), _TRUNCATE);
                        }
                    }
                    if (isSelected) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }

            ImGui::SameLine();

            if (isNewTagMode) {
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 50.0f);
                ImGui::InputText("##NewTagKey", newTagKey, sizeof(newTagKey));
                ImGui::SameLine();
            }

            if (ImGui::Button("Add", ImVec2(40.0f, 0.0f))) {
                std::string tagToAdd = isNewTagMode ? newTagKey : (selectedTagIdx > 0 ? tagList[selectedTagIdx] : "");
                if (!tagToAdd.empty() && tagToAdd != "--- Select a tag ---" && tagToAdd != "+ New Tag...") {
                    tags[tagToAdd] = 1; // 値は1固定
                    newTagKey[0] = '\0';
                    selectedTagIdx = 0;
                }
            }
            ImGui::Unindent(8.0f);
        }

        if (auto* tr = e->GetComponent<TransformComponent>()) {
            if (ImGui::CollapsingHeader("Transform (変形)", ImGuiTreeNodeFlags_DefaultOpen)) {
               ImGui::Indent(8.0f);
               bool enabled = tr->IsEnabled();
               if (ImGui::Checkbox("Enabled (有効化)##TR", &enabled)) tr->SetEnabled(enabled);
               ImGui::SameLine(ImGui::GetContentRegionAvail().x - 40.0f);
               if (ImGui::Button("Reset (リセット)##TR")) {
                   tr->position = {0.0f, 0.0f, 0.0f};
                   tr->rotation = {0.0f, 0.0f, 0.0f};
                   tr->scale = {1.0f, 1.0f, 1.0f};
               }
               const bool is2D = Is2DEntity(*e);
               if (is2D) {
                   // Text / Sprite はスクリーン座標 X/Y（ピクセル・左上原点）のみ使用する
                   ImGui::DragFloat2("Position XY (px)", &tr->position.x, 1.0f);
                   ImGui::TextDisabled("2D 要素は Position X/Y のみ使用（Viewport 上でドラッグ移動可）");
               } else {
               ImGui::DragFloat3("Position (位置)", &tr->position.x, 0.1f);

               // Euler 変換 (deg <-> rad)
               RC::Vector3 eulerDegrees = { tr->rotation.x * 180.0f / 3.14159265f, tr->rotation.y * 180.0f / 3.14159265f, tr->rotation.z * 180.0f / 3.14159265f };
               if (ImGui::DragFloat3("Rotation (回転)", &eulerDegrees.x, 1.0f)) {
                   tr->rotation = { eulerDegrees.x * 3.14159265f / 180.0f, eulerDegrees.y * 3.14159265f / 180.0f, eulerDegrees.z * 3.14159265f / 180.0f };
               }
               ImGui::DragFloat3("Scale (スケール)", &tr->scale.x, 0.1f);
               }
               ImGui::Unindent(8.0f);
            }
        }

        if (auto* ren = e->GetComponent<ModelRendererComponent>()) {
            bool headerOpen = ImGui::CollapsingHeader("Model Renderer (モデル描画)", ImGuiTreeNodeFlags_DefaultOpen);
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Remove Component")) pendingRemove = [e](){ e->RemoveComponent<ModelRendererComponent>(); };
                ImGui::EndPopup();
            }
            if (headerOpen) {
               ImGui::Indent(8.0f);
               bool enabled = ren->IsEnabled();
               if (ImGui::Checkbox("Enabled (有効化)##Model", &enabled)) ren->SetEnabled(enabled);
               ImGui::Checkbox("Visible (表示)##Model", &ren->visible);
               ImGui::Text("Model Handle: %d", ren->modelHandle);
               ImGui::Unindent(8.0f);
            }
            // ── Material セクション ──
            if (ren->HasModel() && ImGui::CollapsingHeader("Material (マテリアル)##Model", ImGuiTreeNodeFlags_DefaultOpen)) {
               ImGui::Indent(8.0f);

               // -- Base Color --
               ImGui::ColorEdit4("Base Color (基本色)##Model", &ren->color.x);

               // -- Texture --
               ImGui::Text("Texture (テクスチャ)");
               ImGui::SameLine();
               std::string texLabelStr = ren->texturePath.empty() ? "(None)##TexM" : std::filesystem::path(ren->texturePath).filename().string() + "##TexM";
               ImGui::Button(texLabelStr.c_str(), ImVec2(ImGui::GetContentRegionAvail().x - 60.0f, 0));
               if (ImGui::BeginDragDropTarget()) {
                   if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM")) {
                       std::string droppedPath((const char*)payload->Data);
                       std::filesystem::path p(droppedPath);
                       std::string ext = p.extension().string();
                       std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                       if (ext == ".png" || ext == ".jpg" || ext == ".dds") {
                           ren->texturePath = droppedPath;
                           ren->texOverride = RC::LoadTex(droppedPath);
                       }
                   }
                   ImGui::EndDragDropTarget();
               }
               if (!ren->texturePath.empty()) {
                   ImGui::SameLine();
                   if (ImGui::Button("X##TexModel", ImVec2(22, 0))) {
                       ren->texturePath.clear();
                       ren->texOverride = -1;
                   }
               }

               // -- Normal Map --
               ImGui::Text("Normal Map (法線マップ)");
               ImGui::SameLine();
               std::string normalLabelStr = ren->normalMapPath.empty() ? "(None)##NmapM" : std::filesystem::path(ren->normalMapPath).filename().string() + "##NmapM";
               ImGui::Button(normalLabelStr.c_str(), ImVec2(ImGui::GetContentRegionAvail().x - 60.0f, 0));
               if (ImGui::BeginDragDropTarget()) {
                   if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM")) {
                       std::string droppedPath((const char*)payload->Data);
                       std::filesystem::path p(droppedPath);
                       std::string ext = p.extension().string();
                       std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                       if (ext == ".png" || ext == ".jpg" || ext == ".dds") {
                           ren->normalMapPath = droppedPath;
                           ren->normalMapOverride = RC::LoadTex(droppedPath);
                           RC::SetModelNormalMap(ren->modelHandle, ren->normalMapOverride);
                       }
                   }
                   ImGui::EndDragDropTarget();
               }
               if (!ren->normalMapPath.empty()) {
                   ImGui::SameLine();
                   if (ImGui::Button("X##NmapModel", ImVec2(22, 0))) {
                       ren->normalMapPath.clear();
                       ren->normalMapOverride = -1;
                       RC::SetModelNormalMap(ren->modelHandle, -1);
                   }
               }

               // -- Roughness Map --
               ImGui::Text("Roughness Map (粗さマップ)");
               ImGui::SameLine();
               std::string roughnessLabelStr = ren->roughnessMapPath.empty() ? "(None)##RmapM" : std::filesystem::path(ren->roughnessMapPath).filename().string() + "##RmapM";
               ImGui::Button(roughnessLabelStr.c_str(), ImVec2(ImGui::GetContentRegionAvail().x - 60.0f, 0));
               if (ImGui::BeginDragDropTarget()) {
                   if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM")) {
                       std::string droppedPath((const char*)payload->Data);
                       std::filesystem::path p(droppedPath);
                       std::string ext = p.extension().string();
                       std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                       if (ext == ".png" || ext == ".jpg" || ext == ".dds") {
                           ren->roughnessMapPath = droppedPath;
                           ren->roughnessMapOverride = RC::LoadTex(droppedPath);
                           RC::SetModelRoughnessMap(ren->modelHandle, ren->roughnessMapOverride);
                       }
                   }
                   ImGui::EndDragDropTarget();
               }
               if (!ren->roughnessMapPath.empty()) {
                   ImGui::SameLine();
                   if (ImGui::Button("X##RmapModel", ImVec2(22, 0))) {
                       ren->roughnessMapPath.clear();
                       ren->roughnessMapOverride = -1;
                       RC::SetModelRoughnessMap(ren->modelHandle, -1);
                   }
               }

               ImGui::Separator();

               // -- GPU Material properties --
               if (Material* mat = RC::GetModelMaterialPtr(ren->modelHandle)) {
                   // Lighting Mode（Follow Light: シーンの DirectionalLight に追従 / それ以外: 個別固定）
                   const char* lightingModes[] = { "Follow Light (ライトに従う)", "None", "Lambert", "Half Lambert" };
                   int lightMode = ren->lightingMode + 1; // -1..2 -> 0..3
                   if (lightMode < 0) lightMode = 0;
                   if (lightMode > 3) lightMode = 3;
                   if (ImGui::Combo("Lighting (ライティング)##Model", &lightMode, lightingModes, 4)) {
                       ren->lightingMode = lightMode - 1;
                       if (ren->lightingMode >= 0) {
                           RC::SetModelLightingMode(ren->modelHandle, static_cast<LightingMode>(ren->lightingMode));
                       } else {
                           RC::ClearModelLightingModeOverride(ren->modelHandle);
                       }
                   }
                   if (ren->lightingMode < 0) {
                       ImGui::SameLine();
                       ImGui::TextDisabled("(現在: %s)", (mat->lightingMode >= 0 && mat->lightingMode <= 2) ? lightingModes[mat->lightingMode + 1] : "?");
                   }

                   // Shininess
                   ImGui::DragFloat("Shininess (光沢)##Model", &mat->shininess, 1.0f, 0.0f, 512.0f);

                   // Environment Reflection
                   if (ImGui::DragFloat("Env Reflection (環境反射)##Model", &mat->environmentCoefficient, 0.01f, 0.0f, 1.0f)) {
                       ren->environmentCoeff = mat->environmentCoefficient;
                   }

                   ImGui::Separator();

                   // -- UV Transform (Tiling & Offset) --
                   ImGui::Text("UV Transform (UV変換)");
                   // UV Tiling (scale)
                   float tilingX = mat->uvTransform.m[0][0];
                   float tilingY = mat->uvTransform.m[1][1];
                   float tiling[2] = { tilingX, tilingY };
                   if (ImGui::DragFloat2("Tiling (タイリング)##Model", tiling, 0.01f)) {
                       mat->uvTransform.m[0][0] = tiling[0];
                       mat->uvTransform.m[1][1] = tiling[1];
                   }
                   // UV Offset (translation)
                   float offsetX = mat->uvTransform.m[3][0];
                   float offsetY = mat->uvTransform.m[3][1];
                   float offset[2] = { offsetX, offsetY };
                   if (ImGui::DragFloat2("Offset (オフセット)##Model", offset, 0.01f)) {
                       mat->uvTransform.m[3][0] = offset[0];
                       mat->uvTransform.m[3][1] = offset[1];
                   }
               }

               ImGui::Unindent(8.0f);
            }
        }

        if (auto* pm = e->GetComponent<PrimitiveMeshComponent>()) {
            bool headerOpen = ImGui::CollapsingHeader("Primitive Mesh (基本図形)", ImGuiTreeNodeFlags_DefaultOpen);
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Remove Component")) pendingRemove = [e](){ e->RemoveComponent<PrimitiveMeshComponent>(); };
                ImGui::EndPopup();
            }
            if (headerOpen) {
               ImGui::Indent(8.0f);
               bool enabled = pm->IsEnabled();
               if (ImGui::Checkbox("Enabled (有効化)##PM", &enabled)) pm->SetEnabled(enabled);
               ImGui::Checkbox("Visible (表示)##PM", &pm->visible);
               static const char* typeNames[] = {
                   "Sphere", "Box", "Plane", "Cylinder", "Cone", "Torus", "Capsule"
               };
               int typeIdx = static_cast<int>(pm->type);
               ImGui::Text("Type (種類): %s", (typeIdx >= 0 && typeIdx < 7) ? typeNames[typeIdx] : "Unknown");
               ImGui::Text("Mesh Handle: %d", pm->meshHandle);
               ImGui::Unindent(8.0f);
            }
            // ── Material セクション ──
            if (pm->HasMesh() && ImGui::CollapsingHeader("Material (マテリアル)##PM", ImGuiTreeNodeFlags_DefaultOpen)) {
               ImGui::Indent(8.0f);

               // -- Texture --
               ImGui::Text("Texture (テクスチャ)");
               ImGui::SameLine();
               std::string texLabelStrPM = pm->texturePath.empty() ? "(None)##TexPM" : std::filesystem::path(pm->texturePath).filename().string() + "##TexPM";
               ImGui::Button(texLabelStrPM.c_str(), ImVec2(ImGui::GetContentRegionAvail().x - 60.0f, 0));
               if (ImGui::BeginDragDropTarget()) {
                   if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM")) {
                       std::string droppedPath((const char*)payload->Data);
                       std::filesystem::path p(droppedPath);
                       std::string ext = p.extension().string();
                       std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                       if (ext == ".png" || ext == ".jpg" || ext == ".dds") {
                           pm->texturePath = droppedPath;
                           pm->texOverride = RC::LoadTex(droppedPath);
                       }
                   }
                   ImGui::EndDragDropTarget();
               }
               if (!pm->texturePath.empty()) {
                   ImGui::SameLine();
                   if (ImGui::Button("X##TexPM", ImVec2(22, 0))) {
                       pm->texturePath.clear();
                       pm->texOverride = -1;
                   }
               }

               // -- Normal Map --
               ImGui::Text("Normal Map (法線マップ)");
               ImGui::SameLine();
               std::string normalLabelStrPM = pm->normalMapPath.empty() ? "(None)##NmapPM" : std::filesystem::path(pm->normalMapPath).filename().string() + "##NmapPM";
               ImGui::Button(normalLabelStrPM.c_str(), ImVec2(ImGui::GetContentRegionAvail().x - 60.0f, 0));
               if (ImGui::BeginDragDropTarget()) {
                   if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM")) {
                       std::string droppedPath((const char*)payload->Data);
                       std::filesystem::path p(droppedPath);
                       std::string ext = p.extension().string();
                       std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                       if (ext == ".png" || ext == ".jpg" || ext == ".dds") {
                           pm->normalMapPath = droppedPath;
                           pm->normalMapOverride = RC::LoadTex(droppedPath);
                           RC::SetPrimitiveMeshNormalMap(pm->meshHandle, pm->normalMapOverride);
                       }
                   }
                   ImGui::EndDragDropTarget();
               }
               if (!pm->normalMapPath.empty()) {
                   ImGui::SameLine();
                   if (ImGui::Button("X##NmapPM", ImVec2(22, 0))) {
                       pm->normalMapPath.clear();
                       pm->normalMapOverride = -1;
                       RC::SetPrimitiveMeshNormalMap(pm->meshHandle, -1);
                   }
               }

               // -- Roughness Map --
               ImGui::Text("Roughness Map (粗さマップ)");
               ImGui::SameLine();
               std::string roughnessLabelStrPM = pm->roughnessMapPath.empty() ? "(None)##RmapPM" : std::filesystem::path(pm->roughnessMapPath).filename().string() + "##RmapPM";
               ImGui::Button(roughnessLabelStrPM.c_str(), ImVec2(ImGui::GetContentRegionAvail().x - 60.0f, 0));
               if (ImGui::BeginDragDropTarget()) {
                   if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM")) {
                       std::string droppedPath((const char*)payload->Data);
                       std::filesystem::path p(droppedPath);
                       std::string ext = p.extension().string();
                       std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                       if (ext == ".png" || ext == ".jpg" || ext == ".dds") {
                           pm->roughnessMapPath = droppedPath;
                           pm->roughnessMapOverride = RC::LoadTex(droppedPath);
                           RC::SetPrimitiveMeshRoughnessMap(pm->meshHandle, pm->roughnessMapOverride);
                       }
                   }
                   ImGui::EndDragDropTarget();
               }
               if (!pm->roughnessMapPath.empty()) {
                   ImGui::SameLine();
                   if (ImGui::Button("X##RmapPM", ImVec2(22, 0))) {
                       pm->roughnessMapPath.clear();
                       pm->roughnessMapOverride = -1;
                       RC::SetPrimitiveMeshRoughnessMap(pm->meshHandle, -1);
                   }
               }

               ImGui::Separator();

               // -- GPU Material properties --
               if (Material* mat = RC::GetPrimitiveMeshMaterialPtr(pm->meshHandle)) {
                   // Base Color
                   // NOTE: コンポーネント側 (pm->color) を編集元にする。
                   //       GPU側 Material のみを書き換えるとシリアライズ対象外になり、
                   //       Scene保存時に色が失われる（毎回白に戻る）ため。
                   if (ImGui::ColorEdit4("Base Color (基本色)##PM", &pm->color.x)) {
                       mat->color = pm->color;
                   }

                   // Lighting Mode（Follow Light: シーンの DirectionalLight に追従 / それ以外: 個別固定）
                   const char* lightingModes[] = { "Follow Light (ライトに従う)", "None", "Lambert", "Half Lambert" };
                   int lightMode = pm->lightingMode + 1; // -1..2 -> 0..3
                   if (lightMode < 0) lightMode = 0;
                   if (lightMode > 3) lightMode = 3;
                   if (ImGui::Combo("Lighting (ライティング)##PM", &lightMode, lightingModes, 4)) {
                       pm->lightingMode = lightMode - 1;
                       if (pm->lightingMode >= 0) {
                           RC::SetPrimitiveMeshLightingMode(pm->meshHandle, static_cast<LightingMode>(pm->lightingMode));
                       } else {
                           RC::ClearPrimitiveMeshLightingModeOverride(pm->meshHandle);
                       }
                   }
                   if (pm->lightingMode < 0) {
                       ImGui::SameLine();
                       ImGui::TextDisabled("(現在: %s)", (mat->lightingMode >= 0 && mat->lightingMode <= 2) ? lightingModes[mat->lightingMode + 1] : "?");
                   }

                   // Shininess
                   if (ImGui::DragFloat("Shininess (光沢)##PM", &pm->shininess, 1.0f, 0.0f, 512.0f)) {
                       mat->shininess = pm->shininess;
                   }

                   // Environment Reflection
                   if (ImGui::DragFloat("Env Reflection (環境反射)##PM", &mat->environmentCoefficient, 0.01f, 0.0f, 1.0f)) {
                       pm->environmentCoeff = mat->environmentCoefficient;
                   }

                   ImGui::Separator();

                   // -- UV Transform (Tiling & Offset) --
                   ImGui::Text("UV Transform (UV変換)");
                   if (ImGui::DragFloat2("Tiling (タイリング)##PM", &pm->uvTiling.x, 0.01f)) {
                       mat->uvTransform.m[0][0] = pm->uvTiling.x;
                       mat->uvTransform.m[1][1] = pm->uvTiling.y;
                   }
                   if (ImGui::DragFloat2("Offset (オフセット)##PM", &pm->uvOffset.x, 0.01f)) {
                       mat->uvTransform.m[3][0] = pm->uvOffset.x;
                       mat->uvTransform.m[3][1] = pm->uvOffset.y;
                   }
               }

               ImGui::Unindent(8.0f);
            }
        }

        if (auto* tm = e->GetComponent<TextMeshComponent>()) {
            bool headerOpen = ImGui::CollapsingHeader("Text Mesh (立体文字)", ImGuiTreeNodeFlags_DefaultOpen);
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Remove Component")) pendingRemove = [e, tm](){
                    if (tm->meshHandle >= 0) RC::UnloadPrimitiveMesh(tm->meshHandle);
                    if (tm->outlineMeshHandle >= 0) RC::UnloadPrimitiveMesh(tm->outlineMeshHandle);
                    e->RemoveComponent<TextMeshComponent>();
                };
                ImGui::EndPopup();
            }
            if (headerOpen) {
               ImGui::Indent(8.0f);
               bool enabled = tm->IsEnabled();
               if (ImGui::Checkbox("Enabled (有効化)##TM", &enabled)) tm->SetEnabled(enabled);
               ImGui::Checkbox("Visible (表示)##TM", &tm->visible);

               // 文字列（複数行）。変更はシーン更新時に EnsureTextMesh が検知して再生成する
               {
                   static char textBuf[2048];
                   const size_t n = (std::min)(tm->text.size(), sizeof(textBuf) - 1);
                   memcpy(textBuf, tm->text.data(), n);
                   textBuf[n] = '\0';
                   if (ImGui::InputTextMultiline("Text (文字列)##TM", textBuf, sizeof(textBuf), ImVec2(-1, 80))) {
                       tm->text = textBuf;
                   }
               }

               // フォント選択（Resources/fonts 以下を走査）
               {
                   std::vector<std::string> fontFiles;
                   // ゲーム側 Resources/fonts とエンジン側 Chaso/Resources/fonts の両方を走査する
                   // （保存するパスは "Resources/fonts/..." に統一し、読み込み時に Chaso::ResolvePath で解決する）
                   for (const std::filesystem::path fontRoot : {std::filesystem::path("Resources/fonts"),
                                                                std::filesystem::path(Chaso::EngineRoot()) / "Resources" / "fonts"}) {
                       if (!std::filesystem::exists(fontRoot)) continue;
                       for (const auto& entry : std::filesystem::recursive_directory_iterator(fontRoot)) {
                           if (!entry.is_regular_file()) continue;
                           std::string ext = entry.path().extension().string();
                           std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){ return (char)std::tolower(c); });
                           if (ext == ".ttf" || ext == ".otf" || ext == ".ttc") {
                               const std::u8string u8 = (std::filesystem::path("Resources/fonts") / std::filesystem::relative(entry.path(), fontRoot)).generic_u8string();
                               std::string rel(reinterpret_cast<const char*>(u8.c_str()), u8.size());
                               if (std::find(fontFiles.begin(), fontFiles.end(), rel) == fontFiles.end()) fontFiles.push_back(std::move(rel)); // ゲーム側優先
                           }
                       }
                   }
                   std::sort(fontFiles.begin(), fontFiles.end());
                   auto shortName = [](const std::string& path) -> std::string {
                       static const std::string prefix = "Resources/fonts/";
                       return (path.rfind(prefix, 0) == 0) ? path.substr(prefix.size()) : path;
                   };
                   std::string preview = tm->fontPath.empty() ? "(none)" : shortName(tm->fontPath);
                   if (ImGui::BeginCombo("Font (フォント)##TM", preview.c_str())) {
                       for (const auto& f : fontFiles) {
                           const bool selected = (f == tm->fontPath);
                           if (ImGui::Selectable(shortName(f).c_str(), selected)) tm->fontPath = f;
                           if (selected) ImGui::SetItemDefaultFocus();
                       }
                       ImGui::EndCombo();
                   }
                   if (ImGui::BeginDragDropTarget()) {
                       if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM")) {
                           std::string dropped(static_cast<const char*>(payload->Data));
                           std::replace(dropped.begin(), dropped.end(), '\\', '/');
                           std::string ext = std::filesystem::path(dropped).extension().string();
                           std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){ return (char)std::tolower(c); });
                           if (ext == ".ttf" || ext == ".otf" || ext == ".ttc") tm->fontPath = dropped;
                       }
                       ImGui::EndDragDropTarget();
                   }
               }

               // ── 形状 ──
               ImGui::SeparatorText("Shape (形状)");
               ImGui::DragFloat("Size (1em の高さ)##TM", &tm->size, 0.01f, 0.01f, 100.0f, "%.2f");
               ImGui::DragFloat("Depth (厚さ)##TM", &tm->depth, 0.01f, 0.0f, 100.0f, "%.3f");
               ImGui::DragFloat("Curve Tolerance (曲線精度)##TM", &tm->curveTolerance, 0.0005f, 0.0005f, 0.05f, "%.4f");
               ImGui::SameLine();
               ImGui::TextDisabled("(?)");
               if (ImGui::IsItemHovered()) ImGui::SetTooltip("小さいほど曲線が滑らかになりますが頂点数が増えます（em 比）");
               {
                   const char* alignItems[] = { "Left (左揃え)", "Center (中央揃え)", "Right (右揃え)" };
                   int alignIdx = static_cast<int>(tm->align);
                   if (ImGui::Combo("Align (揃え)##TM", &alignIdx, alignItems, 3)) tm->align = static_cast<TextAlign>(alignIdx);
               }
               ImGui::DragFloat("Line Spacing (行間)##TM", &tm->lineSpacing, 0.01f, 0.5f, 3.0f);

               // ── 縁取り ──
               ImGui::SeparatorText("Outline (縁取り)");
               ImGui::Checkbox("Enabled (縁取りを描く)##OL", &tm->outlineEnabled);
               if (tm->outlineEnabled) {
                   ImGui::DragFloat("Width (太さ, em比)##OL", &tm->outlineWidth, 0.001f, 0.001f, 0.5f, "%.3f");
                   ImGui::SameLine();
                   ImGui::TextDisabled("(?)");
                   if (ImGui::IsItemHovered()) ImGui::SetTooltip("文字の高さ(1em)に対する比率。0.05 なら 1em の 5%% の幅で縁が付きます。\n太さの変更はメッシュを再生成します（色は即時反映）。");
                   ImGui::ColorEdit4("Color (縁取り色)##OL", &tm->outlineColor.x);
                   ImGui::Checkbox("Unlit (単色・ライティングなし)##OL", &tm->outlineUnlit);
                   if (tm->HasOutlineMesh()) {
                       ImGui::TextDisabled("Outline Mesh: %d  /  %u verts, %u tris", tm->outlineMeshHandle, tm->outlineInfo.vertexCount, tm->outlineInfo.triangleCount);
                   } else if (tm->built && tm->HasMesh()) {
                       ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "縁取りメッシュ未生成");
                   }
               }

               ImGui::SeparatorText("Info (情報)");
               if (tm->HasMesh()) {
                   ImGui::TextDisabled("Mesh Handle: %d  /  %u verts, %u tris", tm->meshHandle, tm->info.vertexCount, tm->info.triangleCount);
                   ImGui::TextDisabled("Bounds: (%.2f, %.2f, %.2f) - (%.2f, %.2f, %.2f)",
                                       tm->info.min.x, tm->info.min.y, tm->info.min.z,
                                       tm->info.max.x, tm->info.max.y, tm->info.max.z);
               } else if (tm->built) {
                   ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "メッシュ未生成（フォントが開けない／描く文字が無い）");
               }
               if (ImGui::Button("Rebuild (再生成)##TM")) tm->built = false;
               ImGui::Unindent(8.0f);
            }

            // ── Material セクション（PrimitiveMesh と同じ GPU Material を編集） ──
            if (ImGui::CollapsingHeader("Material (マテリアル)##TM", ImGuiTreeNodeFlags_DefaultOpen)) {
               ImGui::Indent(8.0f);

               // テクスチャスロット共通 UI（ドラッグ＆ドロップ＋クリアボタン）
               auto textureSlot = [&](const char* label, const char* id, std::string& path, int& handle, auto&& onChanged) {
                   ImGui::Text("%s", label);
                   ImGui::SameLine();
                   std::string btn = (path.empty() ? std::string("(None)") : std::filesystem::path(path).filename().string()) + "##" + id;
                   ImGui::Button(btn.c_str(), ImVec2(ImGui::GetContentRegionAvail().x - 60.0f, 0));
                   if (ImGui::BeginDragDropTarget()) {
                       if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM")) {
                           std::string droppedPath((const char*)payload->Data);
                           std::string ext = std::filesystem::path(droppedPath).extension().string();
                           std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                           if (ext == ".png" || ext == ".jpg" || ext == ".dds") {
                               path = droppedPath;
                               handle = RC::LoadTex(droppedPath);
                               onChanged();
                           }
                       }
                       ImGui::EndDragDropTarget();
                   }
                   if (!path.empty()) {
                       ImGui::SameLine();
                       std::string clr = std::string("X##") + id;
                       if (ImGui::Button(clr.c_str(), ImVec2(22, 0))) {
                           path.clear();
                           handle = -1;
                           onChanged();
                       }
                   }
               };

               // Texture は描画時に texOverride として毎フレーム渡されるので差し替えは即時反映（再生成不要）
               textureSlot("Texture (テクスチャ)", "TexTM", tm->texturePath, tm->texOverride, []() {});
               textureSlot("Normal Map (法線マップ)", "NmapTM", tm->normalMapPath, tm->normalMapOverride, [&]() {
                   if (tm->HasMesh()) RC::SetPrimitiveMeshNormalMap(tm->meshHandle, tm->normalMapOverride);
               });
               textureSlot("Roughness Map (粗さマップ)", "RmapTM", tm->roughnessMapPath, tm->roughnessMapOverride, [&]() {
                   if (tm->HasMesh()) RC::SetPrimitiveMeshRoughnessMap(tm->meshHandle, tm->roughnessMapOverride);
               });

               ImGui::Separator();

               // GPU Material（メッシュ未生成でもコンポーネント側の値は編集でき、生成時に反映される）
               Material* mat = tm->HasMesh() ? RC::GetPrimitiveMeshMaterialPtr(tm->meshHandle) : nullptr;

               if (ImGui::ColorEdit4("Base Color (基本色)##TM", &tm->color.x)) {
                   if (mat) mat->color = tm->color;
               }

               const char* lightingModes[] = { "Follow Light (ライトに従う)", "None", "Lambert", "Half Lambert" };
               int lightMode = std::clamp(tm->lightingMode + 1, 0, 3);
               if (ImGui::Combo("Lighting (ライティング)##TM", &lightMode, lightingModes, 4)) {
                   tm->lightingMode = lightMode - 1;
                   if (tm->HasMesh()) {
                       if (tm->lightingMode >= 0) RC::SetPrimitiveMeshLightingMode(tm->meshHandle, static_cast<LightingMode>(tm->lightingMode));
                       else RC::ClearPrimitiveMeshLightingModeOverride(tm->meshHandle);
                   }
               }
               if (tm->lightingMode < 0 && mat) {
                   ImGui::SameLine();
                   ImGui::TextDisabled("(現在: %s)", (mat->lightingMode >= 0 && mat->lightingMode <= 2) ? lightingModes[mat->lightingMode + 1] : "?");
               }

               if (ImGui::DragFloat("Shininess (光沢)##TM", &tm->shininess, 1.0f, 0.0f, 512.0f)) {
                   if (mat) mat->shininess = tm->shininess;
               }
               if (ImGui::DragFloat("Env Reflection (環境反射)##TM", &tm->environmentCoeff, 0.01f, 0.0f, 1.0f)) {
                   if (mat) mat->environmentCoefficient = tm->environmentCoeff;
               }

               ImGui::Separator();
               ImGui::Text("UV Transform (UV変換)");
               ImGui::SameLine();
               ImGui::TextDisabled("(?)");
               if (ImGui::IsItemHovered()) ImGui::SetTooltip("表面・裏面は文字列全体に 0..1 で平面投影\n側面は輪郭に沿った周長 u（1em=1.0）/ 厚さ方向 v（表面側 0 → 裏面側 1）");
               if (ImGui::DragFloat2("Tiling (タイリング)##TM", &tm->uvTiling.x, 0.01f)) {
                   if (mat) { mat->uvTransform.m[0][0] = tm->uvTiling.x; mat->uvTransform.m[1][1] = tm->uvTiling.y; }
               }
               if (ImGui::DragFloat2("Offset (オフセット)##TM", &tm->uvOffset.x, 0.01f)) {
                   if (mat) { mat->uvTransform.m[3][0] = tm->uvOffset.x; mat->uvTransform.m[3][1] = tm->uvOffset.y; }
               }

               ImGui::Unindent(8.0f);
            }
        }

        if (auto* spr = e->GetComponent<SpriteRendererComponent>()) {
            bool headerOpen = ImGui::CollapsingHeader("Sprite Renderer (スプライト描画)", ImGuiTreeNodeFlags_DefaultOpen);
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Remove Component")) pendingRemove = [e](){ e->RemoveComponent<SpriteRendererComponent>(); };
                ImGui::EndPopup();
            }
            if (headerOpen) {
               ImGui::Indent(8.0f);
               bool enabled = spr->IsEnabled();
               if (ImGui::Checkbox("Enabled (有効化)##Spr", &enabled)) spr->SetEnabled(enabled);
               ImGui::Checkbox("Visible (表示)##Spr", &spr->visible);
               {
                   const char* spaceItems[] = {
                       "Screen (手前・スクリーン座標)",
                       "Screen Behind (奥・スクリーン座標)",
                       "World (ワールド座標・深度テストあり)",
                   };
                   int spaceIdx = static_cast<int>(spr->space);
                   if (ImGui::Combo("Space (描画空間)##Spr", &spaceIdx, spaceItems, IM_ARRAYSIZE(spaceItems))) {
                       spr->space = static_cast<SpriteSpace>(spaceIdx);
                   }
                   if (ImGui::IsItemHovered()) {
                       ImGui::SetTooltip(
                           "Screen        : 従来通り。常に3Dより手前\n"
                           "Screen Behind : 常に3Dより奥（背景用）\n"
                           "World         : Transform をワールド座標として扱い、\n"
                           "                モデルとモデルの間に挟み込めます");
                   }
               }
               if (spr->IsWorldSpace()) {
                   ImGui::TextDisabled("大きさは Transform の Scale で指定します");
               } else {
                   ImGui::DragFloat2("Size (サイズ)", &spr->size.x, 1.0f, 0.0f, 4096.0f);
               }
               ImGui::ColorEdit4("Color (色)##Spr", &spr->color.x);
               {
                   // 画像パス（Content Browser からドラッグ&ドロップで差し替え）
                   char pathBuf[512];
                   strncpy_s(pathBuf, sizeof(pathBuf), spr->spritePath.c_str(), _TRUNCATE);
                   if (ImGui::InputText("Image (画像)##Spr", pathBuf, sizeof(pathBuf), ImGuiInputTextFlags_EnterReturnsTrue)) {
                       spr->spritePath = pathBuf;
                   }
                   if (ImGui::BeginDragDropTarget()) {
                       if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM")) {
                           std::string dropped(static_cast<const char*>(payload->Data));
                           std::string ext = std::filesystem::path(dropped).extension().string();
                           std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){ return (char)std::tolower(c); });
                           if (ext == ".png" || ext == ".jpg" || ext == ".dds") spr->spritePath = dropped;
                       }
                       ImGui::EndDragDropTarget();
                   }
                   ImGui::TextDisabled("ここに画像をドロップすると差し替わります");
               }
               ImGui::Text("Sprite Handle: %d", spr->spriteHandle);
               ImGui::Unindent(8.0f);
            }
        }

        if (auto* txt = e->GetComponent<TextRendererComponent>()) {
            bool headerOpen = ImGui::CollapsingHeader("Text Renderer (文字描画)", ImGuiTreeNodeFlags_DefaultOpen);
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Remove Component")) pendingRemove = [e](){ e->RemoveComponent<TextRendererComponent>(); };
                ImGui::EndPopup();
            }
            if (headerOpen) {
               ImGui::Indent(8.0f);
               bool enabled = txt->IsEnabled();
               if (ImGui::Checkbox("Enabled (有効化)##Txt", &enabled)) txt->SetEnabled(enabled);
               ImGui::Checkbox("Visible (表示)##Txt", &txt->visible);

               // 文字列（複数行）
               {
                   static char textBuf[2048];
                   const size_t n = (std::min)(txt->text.size(), sizeof(textBuf) - 1);
                   memcpy(textBuf, txt->text.data(), n);
                   textBuf[n] = '\0';
                   if (ImGui::InputTextMultiline("Text (文字列)", textBuf, sizeof(textBuf), ImVec2(-1, 80))) {
                       txt->text = textBuf;
                   }
               }

               // フォント選択（Resources/fonts 以下を走査）
               {
                   std::vector<std::string> fontFiles;
                   // ゲーム側 Resources/fonts とエンジン側 Chaso/Resources/fonts の両方を走査する
                   // （保存するパスは "Resources/fonts/..." に統一し、読み込み時に Chaso::ResolvePath で解決する）
                   for (const std::filesystem::path fontRoot : {std::filesystem::path("Resources/fonts"),
                                                                std::filesystem::path(Chaso::EngineRoot()) / "Resources" / "fonts"}) {
                       if (!std::filesystem::exists(fontRoot)) continue;
                       for (const auto& entry : std::filesystem::recursive_directory_iterator(fontRoot)) {
                           if (!entry.is_regular_file()) continue;
                           std::string ext = entry.path().extension().string();
                           std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){ return (char)std::tolower(c); });
                           if (ext == ".ttf" || ext == ".otf" || ext == ".ttc") {
                               // 日本語ファイル名（アプリ明朝.otf 等）も正しく扱うため UTF-8 で保持する
                               const std::u8string u8 = (std::filesystem::path("Resources/fonts") / std::filesystem::relative(entry.path(), fontRoot)).generic_u8string();
                               std::string rel(reinterpret_cast<const char*>(u8.c_str()), u8.size());
                               if (std::find(fontFiles.begin(), fontFiles.end(), rel) == fontFiles.end()) fontFiles.push_back(std::move(rel)); // ゲーム側優先
                           }
                       }
                   }
                   std::sort(fontFiles.begin(), fontFiles.end());
                   // 表示は "Resources/fonts/" を省いた相対パス（例: Kiwi_Maru/KiwiMaru-Regular.ttf）
                   auto shortName = [](const std::string& path) -> std::string {
                       static const std::string prefix = "Resources/fonts/";
                       return (path.rfind(prefix, 0) == 0) ? path.substr(prefix.size()) : path;
                   };
                   std::string preview = txt->fontPath.empty() ? "(none)" : shortName(txt->fontPath);
                   if (ImGui::BeginCombo("Font (フォント)", preview.c_str())) {
                       for (const auto& f : fontFiles) {
                           const bool selected = (f == txt->fontPath);
                           if (ImGui::Selectable(shortName(f).c_str(), selected)) txt->fontPath = f;
                           if (selected) ImGui::SetItemDefaultFocus();
                       }
                       ImGui::EndCombo();
                   }
                   // Content Browser からのドラッグ&ドロップも受け付ける
                   if (ImGui::BeginDragDropTarget()) {
                       if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM")) {
                           std::string dropped(static_cast<const char*>(payload->Data));
                           std::replace(dropped.begin(), dropped.end(), '\\', '/');
                           std::string ext = std::filesystem::path(dropped).extension().string();
                           std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){ return (char)std::tolower(c); });
                           if (ext == ".ttf" || ext == ".otf" || ext == ".ttc") txt->fontPath = dropped;
                       }
                       ImGui::EndDragDropTarget();
                   }
               }

               ImGui::DragFloat("Font Size (px)", &txt->fontSize, 1.0f, 4.0f, 512.0f, "%.0f");
               ImGui::DragFloat("Scale (拡大率)##Txt", &txt->scale, 0.01f, 0.05f, 20.0f);
               ImGui::ColorEdit4("Color (色)##Txt", &txt->color.x);
               const char* alignItems[] = { "Left (左揃え)", "Center (中央揃え)", "Right (右揃え)" };
               int alignIdx = static_cast<int>(txt->align);
               if (ImGui::Combo("Align (揃え)", &alignIdx, alignItems, 3)) txt->align = static_cast<TextAlign>(alignIdx);
               ImGui::DragFloat("Line Spacing (行間)", &txt->lineSpacing, 0.01f, 0.5f, 3.0f);
               int atlas = static_cast<int>(txt->atlasSize);
               const char* atlasItems[] = { "512", "1024", "2048", "4096" };
               int atlasIdx = (atlas <= 512) ? 0 : (atlas <= 1024) ? 1 : (atlas <= 2048) ? 2 : 3;
               if (ImGui::Combo("Atlas Size (アトラス)", &atlasIdx, atlasItems, 4)) {
                   txt->atlasSize = 512u << atlasIdx;
                   txt->loadedSize = 0.0f; // 再ロードさせる
               }
               ImGui::TextDisabled("位置は Transform の Position X/Y（ピクセル、左上原点）を使用");
               ImGui::Text("Font Handle: %d", txt->fontHandle);
               ImGui::Unindent(8.0f);
            }
        }

        if (auto* anim = e->GetComponent<AnimationComponent>()) {
            bool headerOpen = ImGui::CollapsingHeader("Animation (アニメーション)", ImGuiTreeNodeFlags_DefaultOpen);
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Remove Component")) pendingRemove = [e](){ e->RemoveComponent<AnimationComponent>(); };
                ImGui::EndPopup();
            }
            if (headerOpen) {
               ImGui::Indent(8.0f);
               auto* animRen = e->GetComponent<ModelRendererComponent>();
               if (!animRen) {
                   ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
                                      "Model Renderer が必要です");
               }

               bool enabled = anim->IsEnabled();
               if (ImGui::Checkbox("Enabled (有効化)##Anim", &enabled)) anim->SetEnabled(enabled);
               ImGui::Checkbox("Playing (再生中)", &anim->playing);
               ImGui::DragFloat("Speed (再生速度)", &anim->speed, 0.05f, 0.0f, 10.0f);

               // -- Animation File --
               // 空ならモデル内蔵のアニメーションを使う。別ファイルを D&D で差し替えられる
               ImGui::Text("Anim File (アニメ)");
               ImGui::SameLine();
               std::string animLabelStr = anim->animationPath.empty()
                   ? "(Embedded)##AnimFile"
                   : std::filesystem::path(anim->animationPath).filename().string() + "##AnimFile";
               ImGui::Button(animLabelStr.c_str(), ImVec2(ImGui::GetContentRegionAvail().x - 60.0f, 0));
               if (ImGui::BeginDragDropTarget()) {
                   if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM")) {
                       std::string droppedPath((const char*)payload->Data);
                       std::filesystem::path ap(droppedPath);
                       std::string aext = ap.extension().string();
                       std::transform(aext.begin(), aext.end(), aext.begin(), ::tolower);
                       if (aext == ".gltf" || aext == ".glb" || aext == ".fbx" || aext == ".obj") {
                           anim->animationPath = droppedPath;
                           anim->animIndex = 0;
                           anim->MarkDirty(); // 次の更新で再アタッチさせる
                       }
                   }
                   ImGui::EndDragDropTarget();
               }
               if (!anim->animationPath.empty()) {
                   ImGui::SameLine();
                   if (ImGui::Button("X##AnimFile", ImVec2(22, 0))) {
                       anim->animationPath.clear();
                       anim->animIndex = 0;
                       anim->MarkDirty();
                   }
               }

               // -- Clip index (glTF は 1 ファイルに複数アニメーションを持てる) --
               // GetAnimationCount は assimp でファイルを開くため、変更時のみ取得してキャッシュする
               const std::string animEffPath = anim->animationPath.empty()
                   ? (animRen ? animRen->modelPath : std::string())
                   : anim->animationPath;
               if (anim->clipCount_ < 0 && !animEffPath.empty()) {
                   anim->clipCount_ = RC::GetAnimationCount(animEffPath);
               }
               if (anim->clipCount_ > 1) {
                   int clip = anim->animIndex;
                   if (ImGui::SliderInt("Clip (クリップ番号)", &clip, 0, anim->clipCount_ - 1)) {
                       anim->animIndex = clip;
                       // パスは変わらないので clipCount_ のキャッシュは保持したままにする
                       anim->attached_ = false;
                   }
               } else if (anim->clipCount_ == 1) {
                   ImGui::TextDisabled("Clip: 1 / 1");
               } else if (anim->clipCount_ == 0) {
                   ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
                                      "このファイルにアニメーションがありません");
               }

               // -- Clips (名前付きクリップ) --
               // ここに登録した名前をスクリプトから anim->PlayClip("Walk") のように指定する
               ImGui::Separator();
               ImGui::Text("Clips (名前付きクリップ)");
               ImGui::SameLine();
               if (ImGui::SmallButton("+##AddClip")) {
                   AnimationClip newClip;
                   newClip.name = "Clip" + std::to_string(anim->clips.size());
                   newClip.path = anim->animationPath; // 今表示中のファイルを初期値にする
                   newClip.index = anim->animIndex;
                   anim->clips.push_back(newClip);
               }
               ImGui::TextDisabled("Default: %s",
                                   anim->defaultClip.empty() ? "(None)" : anim->defaultClip.c_str());
               if (!anim->currentClip.empty()) {
                   ImGui::TextDisabled("Now Playing: %s", anim->currentClip.c_str());
               }

               int clipRemoveIdx = -1;
               for (int ci = 0; ci < static_cast<int>(anim->clips.size()); ++ci) {
                   AnimationClip& c = anim->clips[ci];
                   ImGui::PushID(ci);
                   const std::string clipHeader =
                       (c.name.empty() ? std::string("(Unnamed)") : c.name)
                       + (c.name == anim->defaultClip && !c.name.empty() ? "  [Default]" : "");
                   if (ImGui::TreeNode(clipHeader.c_str())) {
                       char clipNameBuf[64];
                       snprintf(clipNameBuf, sizeof(clipNameBuf), "%s", c.name.c_str());
                       if (ImGui::InputText("Name (名前)", clipNameBuf, sizeof(clipNameBuf))) {
                           c.name = clipNameBuf;
                       }

                       // ファイル。空ならモデル内蔵のアニメーションを使う
                       ImGui::Text("File");
                       ImGui::SameLine();
                       const std::string clipFileLabel = c.path.empty()
                           ? "(Embedded)##ClipFile"
                           : std::filesystem::path(c.path).filename().string() + "##ClipFile";
                       ImGui::Button(clipFileLabel.c_str(), ImVec2(ImGui::GetContentRegionAvail().x - 60.0f, 0));
                       if (ImGui::BeginDragDropTarget()) {
                           if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM")) {
                               std::string droppedPath((const char*)payload->Data);
                               std::filesystem::path cp(droppedPath);
                               std::string cext = cp.extension().string();
                               std::transform(cext.begin(), cext.end(), cext.begin(), ::tolower);
                               if (cext == ".gltf" || cext == ".glb" || cext == ".fbx" || cext == ".obj") {
                                   c.path = droppedPath;
                                   c.index = 0;
                               }
                           }
                           ImGui::EndDragDropTarget();
                       }
                       if (!c.path.empty()) {
                           ImGui::SameLine();
                           if (ImGui::Button("X##ClipFile", ImVec2(22, 0))) {
                               c.path.clear();
                               c.index = 0;
                           }
                       }

                       ImGui::InputInt("Index (クリップ番号)", &c.index);
                       if (c.index < 0) c.index = 0;
                       ImGui::Checkbox("Loop (ループ)", &c.loop);
                       if (!c.loop) {
                           ImGui::SameLine();
                           ImGui::TextDisabled("(終了後 Default へ戻る)");
                       }
                       ImGui::DragFloat("Speed (速度)", &c.speed, 0.05f, 0.0f, 10.0f);

                       if (ImGui::SmallButton("Play (試し再生)")) anim->PlayClip(c.name, 0.15f, true);
                       ImGui::SameLine();
                       if (ImGui::SmallButton("Set Default")) anim->defaultClip = c.name;
                       ImGui::SameLine();
                       if (ImGui::SmallButton("Remove")) clipRemoveIdx = ci;
                       ImGui::TreePop();
                   }
                   ImGui::PopID();
               }
               if (clipRemoveIdx >= 0) {
                   // 消したクリップが再生中／既定だった場合は参照を外しておく
                   const std::string removedName = anim->clips[clipRemoveIdx].name;
                   anim->clips.erase(anim->clips.begin() + clipRemoveIdx);
                   if (anim->defaultClip == removedName) anim->defaultClip.clear();
                   if (anim->currentClip == removedName) anim->currentClip.clear();
               }
               ImGui::Separator();

               // スキンデータがあるモデルのみ Show Skeleton を表示
               if (animRen && animRen->HasModel() && RC::HasModelSkinData(animRen->modelHandle)) {
                   ImGui::Checkbox("Show Skeleton (骨格表示)", &anim->showSkeleton);
               }
               ImGui::Unindent(8.0f);
            }
        }

        if (auto* ba = e->GetComponent<BoneAttachmentComponent>()) {
            bool headerOpen = ImGui::CollapsingHeader("Bone Attachment (ボーン追従)", ImGuiTreeNodeFlags_DefaultOpen);
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Remove Component")) pendingRemove = [e](){
                    // 上書きしたまま外すとモデルがその場に固定されるので、先に解除する
                    if (auto* r = e->GetComponent<ModelRendererComponent>()) {
                        if (r->HasModel()) RC::ClearModelWorldOverride(r->modelHandle);
                    }
                    e->RemoveComponent<BoneAttachmentComponent>();
                };
                ImGui::EndPopup();
            }
            if (headerOpen) {
               ImGui::Indent(8.0f);
               if (!e->GetComponent<ModelRendererComponent>()) {
                   ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
                                      "Model Renderer が必要です");
               }
               bool baEnabled = ba->IsEnabled();
               if (ImGui::Checkbox("Enabled (有効化)##BoneAtt", &baEnabled)) ba->SetEnabled(baEnabled);

               // -- Target (追従先エンティティ) --
               auto baTarget = (ba->targetGuid != 0 && currentScene)
                   ? currentScene->FindEntityByGuid(ba->targetGuid)
                   : nullptr;
               ImGui::Text("Target (追従先)");
               ImGui::SameLine();
               std::string baTargetLabel = baTarget
                   ? baTarget->Name() + "##BoneTarget"
                   : std::string("(None)##BoneTarget");
               ImGui::Button(baTargetLabel.c_str(), ImVec2(ImGui::GetContentRegionAvail().x - 60.0f, 0));
               if (ImGui::BeginDragDropTarget()) {
                   if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("HIERARCHY_ENTITY")) {
                       uint64_t draggedGuid = *(const uint64_t*)payload->Data;
                       if (draggedGuid != e->Guid()) { // 自分自身への追従は無限ループになるので弾く
                           ba->targetGuid = draggedGuid;
                           ba->jointName.clear();
                           ba->MarkTargetDirty();
                       }
                   }
                   ImGui::EndDragDropTarget();
               }
               if (ba->targetGuid != 0) {
                   ImGui::SameLine();
                   if (ImGui::Button("X##BoneTarget", ImVec2(22, 0))) {
                       ba->targetGuid = 0;
                       ba->jointName.clear();
                       ba->MarkTargetDirty();
                   }
               }
               ImGui::TextDisabled("Hierarchy からドラッグ&ドロップ");

               // -- Joint (追従先のボーン名) --
               // GetModelJointNames は毎フレーム呼ぶと重いので、対象変更時のみ取得してキャッシュする
               if (baTarget) {
                   auto* baTRen = baTarget->GetComponent<ModelRendererComponent>();
                   if (baTRen && baTRen->HasModel() && RC::IsModelReady(baTRen->modelHandle)) {
                       if (ba->jointNamesDirty_) {
                           ba->jointNamesCache_ = RC::GetModelJointNames(baTRen->modelHandle);
                           ba->jointNamesDirty_ = false;
                       }
                       if (ba->jointNamesCache_.empty()) {
                           ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
                                              "追従先にスケルトンがありません");
                       } else {
                           const char* jointPreview = ba->jointName.empty()
                               ? "(Model Root)" : ba->jointName.c_str();
                           if (ImGui::BeginCombo("Joint (ボーン)", jointPreview)) {
                               if (ImGui::Selectable("(Model Root)", ba->jointName.empty())) {
                                   ba->jointName.clear();
                               }
                               for (const auto& jn : ba->jointNamesCache_) {
                                   const bool selected = (jn == ba->jointName);
                                   if (ImGui::Selectable(jn.c_str(), selected)) ba->jointName = jn;
                                   if (selected) ImGui::SetItemDefaultFocus();
                               }
                               ImGui::EndCombo();
                           }
                           if (ImGui::Button("Refresh Joints##BoneAtt")) ba->MarkTargetDirty();
                       }
                   } else {
                       ImGui::TextDisabled("追従先のモデルを読み込み中...");
                   }
               }

               // -- Offset (Joint からのずらし) --
               ImGui::DragFloat3("Offset Pos (位置)", &ba->offsetPosition.x, 0.01f);
               ImGui::DragFloat3("Offset Rot (回転)", &ba->offsetRotation.x, 0.01f);
               ImGui::DragFloat3("Offset Scale (拡縮)", &ba->offsetScale.x, 0.01f);
               ImGui::TextDisabled("追従中は Transform の値は描画に使われません");
               ImGui::Unindent(8.0f);
            }
        }

        if (auto* skybox = e->GetComponent<SkyboxComponent>()) {
            bool headerOpen = ImGui::CollapsingHeader("Skybox (スカイボックス)", ImGuiTreeNodeFlags_DefaultOpen);
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Remove Component")) pendingRemove = [e](){ e->RemoveComponent<SkyboxComponent>(); };
                ImGui::EndPopup();
            }
            if (headerOpen) {
               ImGui::Indent(8.0f);
               bool enabled = skybox->IsEnabled();
               if (ImGui::Checkbox("Enabled (有効化)##Skybox", &enabled)) skybox->SetEnabled(enabled);
               ImGui::Checkbox("Visible (表示)##Skybox", &skybox->visible);
               ImGui::Text("Skybox Handle: %d", skybox->skyboxHandle);
               ImGui::Unindent(8.0f);
            }
            // ── Material セクション ──
            if (ImGui::CollapsingHeader("Material (マテリアル)##Skybox", ImGuiTreeNodeFlags_DefaultOpen)) {
               ImGui::Indent(8.0f);

               ImGui::ColorEdit4("Color (色)##Skybox", &skybox->color.x);

               // -- Cubemap Texture --
               ImGui::Text("Cubemap (キューブマップ)");
               ImGui::SameLine();
               std::string texLabelStr = skybox->skyboxPath.empty() ? "(None - Drop .dds)" : std::filesystem::path(skybox->skyboxPath).filename().string() + "##TexSkybox";
               ImGui::Button(texLabelStr.c_str(), ImVec2(ImGui::GetContentRegionAvail().x - 60.0f, 0));
               if (ImGui::BeginDragDropTarget()) {
                   if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM")) {
                       std::string droppedPath((const char*)payload->Data);
                       std::filesystem::path p(droppedPath);
                       std::string ext = p.extension().string();
                       std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                       if (ext == ".dds") {
                           skybox->skyboxPath = droppedPath;
                           if (skybox->skyboxHandle >= 0) RC::UnloadSkyBox(skybox->skyboxHandle);
                           skybox->skyboxHandle = RC::CreateSkyBox(droppedPath);
                       }
                   }
                   ImGui::EndDragDropTarget();
               }
               if (!skybox->skyboxPath.empty()) {
                   ImGui::SameLine();
                   if (ImGui::Button("X##TexSkybox", ImVec2(22, 0))) {
                       skybox->skyboxPath.clear();
                       if (skybox->skyboxHandle >= 0) RC::UnloadSkyBox(skybox->skyboxHandle);
                       skybox->skyboxHandle = -1;
                   }
               }

               ImGui::Unindent(8.0f);
            }
        }

        if (auto* skydome = e->GetComponent<SkydomeComponent>()) {
            bool headerOpen = ImGui::CollapsingHeader("Skydome (スカイドーム)", ImGuiTreeNodeFlags_DefaultOpen);
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Remove Component")) pendingRemove = [e](){ e->RemoveComponent<SkydomeComponent>(); };
                ImGui::EndPopup();
            }
            if (headerOpen) {
               ImGui::Indent(8.0f);
               bool enabled = skydome->IsEnabled();
               if (ImGui::Checkbox("Enabled (有効化)##Skydome", &enabled)) skydome->SetEnabled(enabled);
               ImGui::Checkbox("Visible (表示)##Skydome", &skydome->visible);
               ImGui::Text("Skydome Handle: %d", skydome->skydomeHandle);
               ImGui::Unindent(8.0f);
            }
            // ── Material セクション ──
            if (ImGui::CollapsingHeader("Material (マテリアル)##Skydome", ImGuiTreeNodeFlags_DefaultOpen)) {
               ImGui::Indent(8.0f);

               if (ImGui::ColorEdit4("Color (色)##Skydome", &skydome->color.x)) {
                   if (skydome->skydomeHandle < 0) {
                       skydome->skydomeHandle = RC::GenerateSkydomeEx(-1);
                   }
                   RC::SetSkydomeColor(skydome->skydomeHandle, skydome->color);
               }

               // -- Texture --
               ImGui::Text("Texture (テクスチャ)");
               ImGui::SameLine();
               std::string texLabelStr = skydome->texturePath.empty() ? "(None)##TexSkydome" : std::filesystem::path(skydome->texturePath).filename().string() + "##TexSkydome";
               ImGui::Button(texLabelStr.c_str(), ImVec2(ImGui::GetContentRegionAvail().x - 60.0f, 0));
               if (ImGui::BeginDragDropTarget()) {
                   if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM")) {
                       std::string droppedPath((const char*)payload->Data);
                       std::filesystem::path p(droppedPath);
                       std::string ext = p.extension().string();
                       std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                       if (ext == ".png" || ext == ".jpg" || ext == ".dds") {
                           skydome->texturePath = droppedPath;
                           skydome->texOverride = RC::LoadTex(droppedPath);
                           if (skydome->skydomeHandle < 0) {
                               skydome->skydomeHandle = RC::GenerateSkydomeEx(skydome->texOverride);
                           }
                       }
                   }
                   ImGui::EndDragDropTarget();
               }
               if (!skydome->texturePath.empty()) {
                   ImGui::SameLine();
                   if (ImGui::Button("X##TexSkydome", ImVec2(22, 0))) {
                       skydome->texturePath.clear();
                       skydome->texOverride = -1;
                   }
               }

               ImGui::Unindent(8.0f);
            }
        }

        if (auto* water = e->GetComponent<WaterComponent>()) {
            bool headerOpen = ImGui::CollapsingHeader("Water (水面)", ImGuiTreeNodeFlags_DefaultOpen);
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Remove Component")) pendingRemove = [e](){ e->RemoveComponent<WaterComponent>(); };
                ImGui::EndPopup();
            }
            if (headerOpen) {
               ImGui::Indent(8.0f);
               bool enabled = water->IsEnabled();
               if (ImGui::Checkbox("Enabled (有効化)##Water", &enabled)) water->SetEnabled(enabled);
               ImGui::Checkbox("Visible (表示)##Water", &water->visible);
               ImGui::Separator();
               ImGui::Text("Wave Parameters (波のパラメータ)");
               ImGui::DragFloat("Wave Height##W1", &water->waveHeight, 0.01f, 0.0f, 5.0f);
               ImGui::DragFloat("Wave Speed##W1", &water->waveSpeed, 0.05f, 0.0f, 10.0f);
               ImGui::DragFloat("Wave Freq##W1", &water->waveFreq, 0.05f, 0.0f, 5.0f);
               ImGui::DragFloat("Wave Height 2##W2", &water->waveHeight2, 0.01f, 0.0f, 5.0f);
               ImGui::DragFloat("Wave Speed 2##W2", &water->waveSpeed2, 0.05f, 0.0f, 10.0f);
               ImGui::DragFloat("Wave Freq 2##W2", &water->waveFreq2, 0.05f, 0.0f, 5.0f);
               ImGui::DragFloat("Steepness", &water->waveSteepness, 0.01f, 0.0f, 1.0f);
               ImGui::Separator();
               ImGui::Text("Water Color (水の色)");
               ImGui::ColorEdit4("Shallow (浅瀬)##Water", &water->shallowColor.x);
               ImGui::ColorEdit4("Deep (深海)##Water", &water->deepColor.x);
               ImGui::Separator();
               ImGui::Text("Material (マテリアル)");
               ImGui::DragFloat("Fresnel Power", &water->fresnelPower, 0.1f, 0.5f, 10.0f);
               ImGui::DragFloat("Specular Power", &water->specularPower, 1.0f, 1.0f, 512.0f);
               ImGui::DragFloat("Normal Scroll", &water->normalScrollSpeed, 0.001f, 0.0f, 0.5f);
               ImGui::DragFloat("Normal Strength", &water->normalStrength, 0.01f, 0.0f, 2.0f);
               ImGui::DragFloat("Env Reflection (環境反射)##Water", &water->environmentCoeff, 0.01f, 0.0f, 1.0f);
               ImGui::DragFloat("Crest Tint (高さで色付け)##Water", &water->crestTint, 0.01f, 0.0f, 1.0f);
               ImGui::Separator();
               ImGui::Text("Ocean Realism (海面のリアル化)");
               ImGui::DragFloat("Detail Waves (詳細波)##Water", &water->detailStrength, 0.01f, 0.0f, 3.0f);
               ImGui::DragFloat("Choppiness (尖り)##Water", &water->choppiness, 0.01f, 0.0f, 1.0f);
               ImGui::DragFloat("Whitecap (白波の濃さ)##Water", &water->whitecapStrength, 0.01f, 0.0f, 2.0f);
               ImGui::DragFloat("Whitecap Coverage (白波の量)##Water", &water->whitecapCoverage, 0.01f, 0.0f, 1.0f);
               ImGui::ColorEdit3("SSS Color (透過光の色)##Water", &water->sssColor.x);
               ImGui::DragFloat("SSS Strength (透過光の強さ)##Water", &water->sssColor.w, 0.01f, 0.0f, 3.0f);
               ImGui::DragFloat("Clarity m (透明度)##Water", &water->clarity, 0.1f, 0.0f, 50.0f);
               ImGui::DragFloat("Detail Fade m (遠景フェード)##Water", &water->detailFadeDistance, 1.0f, 0.0f, 2000.0f);
               ImGui::DragFloat("Normal Tile m (法線タイル, 0=UV)##Water", &water->normalTileSize, 0.1f, 0.0f, 200.0f);
               ImGui::Text("Min Displaced Wavelength: %.2f m", water->MinDisplacedWavelength());
               ImGui::Checkbox("Refraction (屈折)##Water", &water->refraction);
               ImGui::DragFloat("Refraction Strength (歪み)##Water", &water->refractionStrength, 0.001f, 0.0f, 0.2f);
               ImGui::DragFloat("Edge Fade m (交差部のぼかし)##Water", &water->edgeFade, 0.01f, 0.0f, 5.0f);
               ImGui::Separator();
               ImGui::Text("Mesh Handle: %d", water->meshHandle);
               ImGui::Unindent(8.0f);
            }
        }

        // ============================================
        // Lights
        // ============================================
        auto* tr = e->GetComponent<TransformComponent>();
        RC::Vector3 pos = tr ? tr->position : RC::Vector3{0, 0, 0};

        if (auto* dirLight = e->GetComponent<DirectionalLightComponent>()) {
            bool headerOpen = ImGui::CollapsingHeader("Directional Light (平行光源)", ImGuiTreeNodeFlags_DefaultOpen);
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Remove Component")) pendingRemove = [e](){ e->RemoveComponent<DirectionalLightComponent>(); };
                ImGui::EndPopup();
            }
            if (headerOpen) {
               ImGui::Indent(8.0f);
               bool enabled = dirLight->IsEnabled();
               if (ImGui::Checkbox("Enabled (有効化)##DirLight", &enabled)) dirLight->SetEnabled(enabled);
               ImGui::Checkbox("Visible (表示)##DirLight", &dirLight->visible);
               ImGui::ColorEdit4("Color (色)##DirLight", &dirLight->color.x);
               ImGui::DragFloat3("Direction (方向)##DirLight", &dirLight->direction.x, 0.05f);
               ImGui::DragFloat("Intensity (強度)##DirLight", &dirLight->intensity, 0.1f, 0.0f, 100.0f);
               ImGui::ColorEdit3("Ambient Color (環境光の色)##DirLight", &dirLight->ambientColor.x);
               ImGui::DragFloat("Ambient Intensity (環境光の強さ)##DirLight", &dirLight->ambientIntensity, 0.005f, 0.0f, 2.0f, "%.3f");
               ImGui::TextDisabled("(0 = ライトが当たった所だけ見える / 旧仕様の固定値は 0.2)");
               ImGui::Text("Handle: %d", dirLight->lightHandle);
               ImGui::Unindent(8.0f);
            }
        }

        if (auto* ptLight = e->GetComponent<PointLightComponent>()) {
            bool headerOpen = ImGui::CollapsingHeader("Point Light (点光源)", ImGuiTreeNodeFlags_DefaultOpen);
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Remove Component")) pendingRemove = [e](){ e->RemoveComponent<PointLightComponent>(); };
                ImGui::EndPopup();
            }
            if (headerOpen) {
               ImGui::Indent(8.0f);
               bool enabled = ptLight->IsEnabled();
               if (ImGui::Checkbox("Enabled (有効化)##PtLight", &enabled)) ptLight->SetEnabled(enabled);
               ImGui::Checkbox("Visible (表示)##PtLight", &ptLight->visible);
               ImGui::ColorEdit4("Color (色)##PtLight", &ptLight->color.x);
               ImGui::DragFloat("Intensity (強度)##PtLight", &ptLight->intensity, 0.1f, 0.0f, 100.0f);
               ImGui::DragFloat("Radius (半径)##PtLight", &ptLight->radius, 0.5f, 0.0f, 1000.0f);
               ImGui::DragFloat("Decay (減衰)##PtLight", &ptLight->decay, 0.1f, 0.0f, 10.0f);
               ImGui::Checkbox("Cast Shadow (影を落とす)##PtLight", &ptLight->castShadow);
               if (ImGui::IsItemHovered()) {
                   ImGui::SetTooltip("真下向きの広角シャドウ1枚で遮蔽を判定する（壁が垂直な前提）。\nOFF にすると壁を素通りする");
               }
               if (ptLight->castShadow) {
                   ImGui::DragFloat("Shadow Near (影の手前カット)##PtLight", &ptLight->shadowNear, 0.01f, 0.01f, 10.0f);
                   ImGui::Checkbox("Exclude Self (自分は遮らない)##PtLight", &ptLight->shadowExcludeSelf);
               }
               ImGui::Text("Handle: %d", ptLight->lightHandle);
               ImGui::Unindent(8.0f);
            }
        }

        if (auto* spLight = e->GetComponent<SpotLightComponent>()) {
            bool headerOpen = ImGui::CollapsingHeader("Spot Light (スポットライト)", ImGuiTreeNodeFlags_DefaultOpen);
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Remove Component")) pendingRemove = [e](){ e->RemoveComponent<SpotLightComponent>(); };
                ImGui::EndPopup();
            }
            if (headerOpen) {
               ImGui::Indent(8.0f);
               bool enabled = spLight->IsEnabled();
               if (ImGui::Checkbox("Enabled (有効化)##SpLight", &enabled)) spLight->SetEnabled(enabled);
               ImGui::Checkbox("Visible (表示)##SpLight", &spLight->visible);
               ImGui::ColorEdit4("Color (色)##SpLight", &spLight->color.x);
               ImGui::DragFloat3("Direction (方向)##SpLight", &spLight->direction.x, 0.05f);
               ImGui::DragFloat3("Offset (位置ずらし)##SpLight", &spLight->offset.x, 0.05f);
               if (ImGui::IsItemHovered()) {
                 ImGui::SetTooltip("エンティティ中心からのローカルオフセット。\nエンティティの回転に追従する。\n例: 足元なら Y をマイナスに。");
               }
               ImGui::DragFloat("Intensity (強度)##SpLight", &spLight->intensity, 0.1f, 0.0f, 100.0f);
               ImGui::DragFloat("Distance (距離)##SpLight", &spLight->distance, 0.5f, 0.0f, 1000.0f);
               ImGui::DragFloat("Decay (減衰)##SpLight", &spLight->decay, 0.1f, 0.0f, 10.0f);
               ImGui::DragFloat("CosAngle (角度)##SpLight", &spLight->cosAngle, 0.01f, 0.0f, 1.0f);
               ImGui::Checkbox("Cast Shadow (影を落とす)##SpLight", &spLight->castShadow);
               if (spLight->castShadow) {
                   ImGui::DragFloat("Shadow Near (影の手前カット)##SpLight", &spLight->shadowNear, 0.01f, 0.01f, 10.0f);
                   ImGui::Checkbox("Exclude Self (自分は遮らない)##SpLight", &spLight->shadowExcludeSelf);
               }
               ImGui::Text("Handle: %d", spLight->lightHandle);
               ImGui::Unindent(8.0f);
            }
        }

        if (auto* arLight = e->GetComponent<AreaLightComponent>()) {
            bool headerOpen = ImGui::CollapsingHeader("Area Light (エリアライト)", ImGuiTreeNodeFlags_DefaultOpen);
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Remove Component")) pendingRemove = [e](){ e->RemoveComponent<AreaLightComponent>(); };
                ImGui::EndPopup();
            }
            if (headerOpen) {
               ImGui::Indent(8.0f);
               bool enabled = arLight->IsEnabled();
               if (ImGui::Checkbox("Enabled (有効化)##ArLight", &enabled)) arLight->SetEnabled(enabled);
               ImGui::Checkbox("Visible (表示)##ArLight", &arLight->visible);
               ImGui::ColorEdit4("Color (色)##ArLight", &arLight->color.x);
               ImGui::DragFloat("Intensity (強度)##ArLight", &arLight->intensity, 0.1f, 0.0f, 100.0f);
               ImGui::DragFloat("Range (範囲)##ArLight", &arLight->range, 0.5f, 0.0f, 1000.0f);
               ImGui::DragFloat("Decay (減衰)##ArLight", &arLight->decay, 0.1f, 0.0f, 10.0f);
               ImGui::DragFloat("Half Width (幅/2)##ArLight", &arLight->halfWidth, 0.1f, 0.0f, 100.0f);
               ImGui::DragFloat("Half Height (高さ/2)##ArLight", &arLight->halfHeight, 0.1f, 0.0f, 100.0f);
               ImGui::TextDisabled("(Half Height = 0 で線光源(Tube)。right 方向に ±Half Width 伸びる)");
               ImGui::DragFloat3("Right (幅方向)##ArLight", &arLight->right.x, 0.05f);
               ImGui::DragFloat3("Up (高さ方向)##ArLight", &arLight->up.x, 0.05f);
               bool twoSided = arLight->twoSided;
               if (ImGui::Checkbox("Two Sided (両面)##ArLight", &twoSided)) arLight->twoSided = twoSided;
               ImGui::Checkbox("Cast Shadow (影を落とす)##ArLight", &arLight->castShadow);
               if (ImGui::IsItemHovered()) {
                   ImGui::SetTooltip("真下向きの広角シャドウ1枚で遮蔽を判定する（壁が垂直な前提）。\nOFF にすると壁を素通りする");
               }
               if (arLight->castShadow) {
                   ImGui::DragFloat("Shadow Near (影の手前カット)##ArLight", &arLight->shadowNear, 0.01f, 0.01f, 10.0f);
                   ImGui::Checkbox("Exclude Self (自分は遮らない)##ArLight", &arLight->shadowExcludeSelf);
               }
               ImGui::Text("Handle: %d", arLight->lightHandle);
               ImGui::Unindent(8.0f);
            }
        }

        // ============================================
        // Camera
        // ============================================
        if (auto* cam = e->GetComponent<CameraComponent>()) {
            bool headerOpen = ImGui::CollapsingHeader("Camera (カメラ)", ImGuiTreeNodeFlags_DefaultOpen);
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Remove Component")) pendingRemove = [e](){ e->RemoveComponent<CameraComponent>(); };
                ImGui::EndPopup();
            }
            if (headerOpen) {
               ImGui::Indent(8.0f);
               bool enabled = cam->IsEnabled();
               if (ImGui::Checkbox("Enabled (有効化)##Cam", &enabled)) cam->SetEnabled(enabled);
               ImGui::Checkbox("Main Camera (メインカメラ)", &cam->isMain);
               // FOV を度数で表示・編集
               float fovDeg = cam->fovY * 180.0f / 3.14159265f;
               if (ImGui::SliderFloat("FOV (視野角) (deg)", &fovDeg, 1.0f, 179.0f)) {
                   cam->fovY = fovDeg * 3.14159265f / 180.0f;
               }
               ImGui::DragFloat("Near Clip (近クリップ)", &cam->nearZ, 0.01f, 0.001f, 100.0f);
               ImGui::DragFloat("Far Clip (遠クリップ)", &cam->farZ, 1.0f, 0.1f, 10000.0f);
               ImGui::Unindent(8.0f);
            }
        }

        // ============================================
        // Collider
        // ============================================
        if (auto* col = e->GetComponent<ColliderComponent>()) {
            bool headerOpen = ImGui::CollapsingHeader("Collider (当たり判定)", ImGuiTreeNodeFlags_DefaultOpen);
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Remove Component")) pendingRemove = [e](){ e->RemoveComponent<ColliderComponent>(); };
                ImGui::EndPopup();
            }
            if (headerOpen) {
               ImGui::Indent(8.0f);
               bool enabled = col->IsEnabled();
               if (ImGui::Checkbox("Enabled (有効化)##Col", &enabled)) col->SetEnabled(enabled);
               ImGui::Checkbox("Is Trigger (物理反発しない)##Col", &col->isTrigger);
               static const char* colTypeNames[] = { "AABB", "Sphere", "Capsule" };
               int typeIdx = static_cast<int>(col->shape);
               if (ImGui::Combo("Shape (形状)##Col", &typeIdx, colTypeNames, 3)) {
                   col->shape = static_cast<ColliderComponent::Shape>(typeIdx);
               }
               if (col->shape == ColliderComponent::Shape::AABB) {
                   ImGui::DragFloat3("Center (中心)##Col", &col->center.x, 0.1f);
                   ImGui::DragFloat3("Size (サイズ)##Col", &col->size.x, 0.1f);
               } else if (col->shape == ColliderComponent::Shape::Sphere) {
                   ImGui::DragFloat3("Center (中心)##Col", &col->center.x, 0.1f);
                   ImGui::DragFloat("Radius (半径)##Col", &col->radius, 0.1f);
               }
               ImGui::Unindent(8.0f);
            }
        }

        if (auto* rb = e->GetComponent<RigidbodyComponent>()) {
            bool headerOpen = ImGui::CollapsingHeader("Rigidbody (物理演算)", ImGuiTreeNodeFlags_DefaultOpen);
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Remove Component")) pendingRemove = [e](){ e->RemoveComponent<RigidbodyComponent>(); };
                ImGui::EndPopup();
            }
            if (headerOpen) {
               ImGui::Indent(8.0f);
               bool enabled = rb->IsEnabled();
               if (ImGui::Checkbox("Enabled (有効化)##RB", &enabled)) rb->SetEnabled(enabled);
               ImGui::Checkbox("Use Gravity (重力を使用)", &rb->useGravity);
               ImGui::Checkbox("Is Kinematic (プログラムで動かす)", &rb->isKinematic);
               ImGui::DragFloat("Mass (質量)", &rb->mass, 0.1f, 0.001f, 10000.0f);
               ImGui::DragFloat3("Velocity (速度)", &rb->velocity.x, 0.1f);
               ImGui::Unindent(8.0f);
            }
        }

        if (auto* script = e->GetComponent<NativeScriptComponent>()) {
            bool headerOpen = ImGui::CollapsingHeader("Native Script (自作スクリプト)", ImGuiTreeNodeFlags_DefaultOpen);
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Remove Component")) pendingRemove = [e](){ e->RemoveComponent<NativeScriptComponent>(); };
                ImGui::EndPopup();
            }
            if (headerOpen) {
                ImGui::Indent(8.0f);
                bool enabled = script->IsEnabled();
                if (ImGui::Checkbox("Enabled (有効化)##Script", &enabled)) script->SetEnabled(enabled);

                for (size_t i = 0; i < script->scripts.size(); ++i) {
                    ImGui::PushID(static_cast<int>(i));
                    ImGui::Separator();
                    ImGui::Text("Script: %s", script->scripts[i].scriptTypeName.c_str());
                    ImGui::SameLine();
                    if (ImGui::Button("X##RemoveScript")) {
                        script->RemoveScriptAtIndex(i);
                        ImGui::PopID();
                        break; // 削除時はループを抜けて安全にする
                    }
                    if (script->scripts[i].instance) {
                        ImGui::Indent(8.0f);
                        script->scripts[i].instance->OnImGui();
                        ImGui::Unindent(8.0f);
                    }
                    ImGui::PopID();
                }

                ImGui::Separator();
                ImGui::Text("Add Script:");
                ImGui::SameLine();
                const auto& names = ScriptRegistry::GetScriptNames();
                if (ImGui::BeginCombo("##AddScriptCombo", "(Select Script...)")) {
                    for (const auto& n : names) {
                        if (ImGui::Selectable(n.c_str(), false)) {
                            script->AddScript(n);
                        }
                    }
                    ImGui::EndCombo();
                }

                ImGui::Unindent(8.0f);
            }
        }

        if (auto* audio = e->GetComponent<AudioSourceComponent>()) {
            bool headerOpen = ImGui::CollapsingHeader("Audio Source (音源)", ImGuiTreeNodeFlags_DefaultOpen);
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Remove Component")) pendingRemove = [e](){ e->RemoveComponent<AudioSourceComponent>(); };
                ImGui::EndPopup();
            }
            if (headerOpen) {
                ImGui::Indent(8.0f);
                bool enabled = audio->IsEnabled();
                if (ImGui::Checkbox("Enabled (有効化)##Audio", &enabled)) audio->SetEnabled(enabled);

                {
                    const char* busItems[] = {
                        "BGM (1本だけ・シーンをまたいで継続)",
                        "SE (重ね再生)",
                    };
                    int busIdx = static_cast<int>(audio->bus);
                    if (ImGui::Combo("Bus (バス)##Audio", &busIdx, busItems, IM_ARRAYSIZE(busItems))) {
                        audio->Stop(); // 鳴っている音は旧バスのものなので止める
                        audio->bus = static_cast<AudioBus>(busIdx);
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip(
                            "BGM : 常に1本だけ鳴ります。同じ曲なら再要求しても継続、\n"
                            "      次のシーンにも同じ曲を置いておけば途切れません\n"
                            "SE  : Play() のたびに重ねて鳴り、鳴り終わると自動で回収されます");
                    }
                }
                ImGui::SliderFloat("Volume (音量)##Audio", &audio->volume, 0.0f, 1.0f, "%.2f");

                {
                    // 再生開始時に自動で鳴らすスロット
                    const char* noneLabel = "(なし)";
                    const std::string preview = audio->playOnAwake.empty() ? noneLabel : audio->playOnAwake;
                    if (ImGui::BeginCombo("Play On Awake (開始時に再生)##Audio", preview.c_str())) {
                        if (ImGui::Selectable(noneLabel, audio->playOnAwake.empty())) audio->playOnAwake.clear();
                        for (size_t i = 0; i < audio->clips.size(); ++i) {
                            const auto& s = audio->clips[i];
                            ImGui::PushID(static_cast<int>(i));
                            const bool selected = (!s.name.empty() && s.name == audio->playOnAwake);
                            if (ImGui::Selectable(s.name.empty() ? "(名前なし)" : s.name.c_str(), selected)) {
                                audio->playOnAwake = s.name;
                            }
                            ImGui::PopID();
                        }
                        ImGui::EndCombo();
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("再生ボタンで Playing になった瞬間に鳴らすスロット。BGM は通常これを設定します");
                    }
                }

                // ============================================
                // 3D 音響（SE バスのみ）
                // ============================================
                if (!audio->IsBgm()) {
                    ImGui::SeparatorText("3D Sound (3D 音響)");
                    ImGui::SliderFloat("Spatial Blend (0=2D, 1=3D)##Audio", &audio->spatialBlend, 0.0f, 1.0f, "%.2f");
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip(
                            "0 = 2D（位置に関係なく鳴る。従来通り）\n"
                            "1 = 3D（このエンティティの位置から聞こえる。左右の定位・距離減衰・ドップラー）\n"
                            "聞き手は Audio Listener を付けたエンティティ、無ければ描画中のカメラ\n"
                            "スロットごとの Spatial で「常に 2D」「常に 3D」に上書きできます");
                    }
                    if (audio->Any3D()) {
                        if (!audio->Is3D()) {
                            ImGui::TextDisabled("Spatial Blend は 0 ですが、3D 指定のスロットがあるので距離設定が使われます");
                        }
                        if (!audio->HasTransform()) {
                            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f),
                                               "Transform がありません。位置が取れないため 2D で鳴ります");
                        }
                        if (!AudioEngine::Get().Is3DAvailable()) {
                            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f),
                                               "この環境では 3D 音響を初期化できませんでした（2D で鳴ります。Console 参照）");
                        }
                        ImGui::DragFloat("Min Distance (最大音量の距離)##Audio", &audio->minDistance, 0.1f, 0.0f, 10000.0f, "%.1f");
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("この距離までは減衰しません。この内側では音が全方向から聞こえます（真横を通過してもパンが暴れない）");
                        }
                        ImGui::DragFloat("Max Distance (無音になる距離)##Audio", &audio->maxDistance, 0.5f, 0.0f, 10000.0f, "%.1f");
                        if (audio->maxDistance < audio->minDistance) audio->maxDistance = audio->minDistance;
                        {
                            const char* rolloffItems[] = {
                                "Logarithmic (距離に反比例・現実的)",
                                "Linear (直線的に減衰)",
                            };
                            int rolloffIdx = static_cast<int>(audio->rolloff);
                            if (ImGui::Combo("Rolloff (減衰カーブ)##Audio", &rolloffIdx, rolloffItems, IM_ARRAYSIZE(rolloffItems))) {
                                audio->rolloff = static_cast<AudioRolloff>(rolloffIdx);
                            }
                            if (ImGui::IsItemHovered()) {
                                ImGui::SetTooltip(
                                    "Logarithmic: 距離が 2 倍で約半分の音量（-6dB）。Max Distance で 0 に着地\n"
                                    "Linear     : Min → Max で 1 → 0 に直線的に減衰");
                            }
                        }
                        ImGui::SliderFloat("Doppler Level (ドップラー)##Audio", &audio->dopplerLevel, 0.0f, 5.0f, "%.2f");
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("動く音源のピッチ変化。0 で無効、1 で物理的に正しい値（音速 343.5 ワールド単位/秒）");
                        }

                        // 減衰カーブのプレビュー（横軸 = 距離 0 ~ Max、縦軸 = 音量）
                        {
                            float curve[64];
                            const float maxD = (std::max)(audio->maxDistance, 0.01f);
                            for (int i = 0; i < IM_ARRAYSIZE(curve); ++i) {
                                const float d = maxD * static_cast<float>(i) / static_cast<float>(IM_ARRAYSIZE(curve) - 1);
                                curve[i] = AudioEngine::ComputeRolloff(d, audio->minDistance, audio->maxDistance, audio->rolloff);
                            }
                            ImGui::PlotLines("##AudioRolloffCurve", curve, IM_ARRAYSIZE(curve), 0, "Volume / Distance", 0.0f, 1.0f,
                                             ImVec2(0.0f, 50.0f));
                        }
                        if (audio->HasTransform()) {
                            const float dist = audio->DistanceToListener();
                            const float gain = audio->CurrentRolloffGain();
                            const RC::Vector3& v = audio->CurrentVelocity();
                            ImGui::TextDisabled("Listener: %.1f 先 / 減衰 %.0f%% / 速度 %.1f",
                                                dist, gain * 100.0f, std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z));
                        }
                        ImGui::TextDisabled("モノラルの音源が最も自然に定位します（ステレオは L/R を音源の左右に配置）");
                    }
                }

                ImGui::SeparatorText("Clips (音の一覧)");
                int removeIndex = -1;
                for (size_t i = 0; i < audio->clips.size(); ++i) {
                    auto& s = audio->clips[i];
                    ImGui::PushID(static_cast<int>(i));

                    // 名前（スクリプトから Play("名前") で指定する）。Enter か確定で反映
                    char clipNameBuf[64];
                    strncpy_s(clipNameBuf, sizeof(clipNameBuf), s.name.c_str(), _TRUNCATE);
                    ImGui::SetNextItemWidth(140.0f);
                    bool nameCommitted = ImGui::InputText("Name (名前)", clipNameBuf, sizeof(clipNameBuf), ImGuiInputTextFlags_EnterReturnsTrue);
                    nameCommitted = nameCommitted || ImGui::IsItemDeactivatedAfterEdit();
                    if (nameCommitted) {
                        const std::string newName = clipNameBuf;
                        const AudioClipSlot* dup = audio->FindSlot(newName);
                        if (dup && dup != &s) {
                            Log::Print("[Editor] Audio Source: 同じ名前のクリップが既にあります: " + newName);
                        } else {
                            audio->RenameClip(i, newName);
                        }
                    }

                    // ファイルパス（Content Browser からドラッグ&ドロップで差し替え）
                    char clipPathBuf[512];
                    strncpy_s(clipPathBuf, sizeof(clipPathBuf), s.path.c_str(), _TRUNCATE);
                    if (ImGui::InputText("File (音声ファイル)", clipPathBuf, sizeof(clipPathBuf), ImGuiInputTextFlags_EnterReturnsTrue)) {
                        s.path = clipPathBuf;
                    }
                    if (ImGui::BeginDragDropTarget()) {
                        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM")) {
                            std::string dropped(static_cast<const char*>(payload->Data));
                            std::string ext = std::filesystem::path(dropped).extension().string();
                            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){ return (char)std::tolower(c); });
                            if (ext == ".wav" || ext == ".mp3" || ext == ".m4a" || ext == ".aac" || ext == ".wma") {
                                s.path = dropped;
                            }
                        }
                        ImGui::EndDragDropTarget();
                    }

                    ImGui::SetNextItemWidth(140.0f);
                    ImGui::SliderFloat("Vol", &s.volume, 0.0f, 1.0f, "%.2f");
                    ImGui::SameLine();
                    ImGui::Checkbox("Loop (ループ)", &s.loop);

                    // スロットごとの 2D / 3D 上書き（SE バスのみ。再生中の音にもその場で反映される）
                    if (!audio->IsBgm()) {
                        const char* spatialItems[] = {
                            "Component (コンポーネント設定に従う)",
                            "2D (常に通常)",
                            "3D (常に 3D)",
                        };
                        int spatialIdx = static_cast<int>(s.spatial);
                        ImGui::SetNextItemWidth(230.0f);
                        if (ImGui::Combo("Spatial (2D/3D)", &spatialIdx, spatialItems, IM_ARRAYSIZE(spatialItems))) {
                            s.spatial = static_cast<AudioSpatialMode>(spatialIdx);
                        }
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip(
                                "Component: 上の Spatial Blend に従う\n"
                                "2D       : 位置に関係なく鳴る（UI 音・自分の足音など）\n"
                                "3D       : Spatial Blend が 0 でも位置から聞こえる\n"
                                "鳴っている音にもその場で反映されます");
                        }
                        ImGui::SameLine();
                        const float eff = audio->EffectiveBlend(s);
                        if (eff <= 0.0f) {
                            ImGui::TextDisabled("→ 2D");
                        } else if (eff >= 1.0f) {
                            ImGui::TextDisabled("→ 3D");
                        } else {
                            ImGui::TextDisabled("→ 3D %.0f%%", eff * 100.0f);
                        }
                    }

                    // 状態表示と試聴
                    if (s.path.empty()) {
                        ImGui::TextDisabled("ここに .wav / .mp3 をドロップしてください");
                    } else if (s.IsLoaded()) {
                        const int ch = AudioEngine::Get().ClipChannels(s.clipHandle);
                        ImGui::TextDisabled("Loaded: %.1f sec / %s", AudioEngine::Get().ClipDuration(s.clipHandle),
                                            ch == 1 ? "mono" : (ch == 2 ? "stereo" : "multi-ch"));
                        if (audio->EffectiveBlend(s) > 0.0f && ch > 1 && ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("3D 音響ではモノラル音源のほうが自然に定位します（ステレオでも鳴ります）");
                        }
                    } else {
                        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "ロード失敗（パスや形式を確認。Console 参照）");
                    }
                    if (ImGui::Button("Play (試聴)")) audio->Play(s.name);
                    ImGui::SameLine();
                    if (ImGui::Button("Stop (停止)")) audio->Stop();
                    ImGui::SameLine();
                    if (ImGui::Button("Remove (削除)")) removeIndex = static_cast<int>(i);

                    ImGui::PopID();
                    ImGui::Separator();
                }
                if (removeIndex >= 0) {
                    audio->RemoveClip(static_cast<size_t>(removeIndex));
                }
                if (ImGui::Button("+ Add Clip (音を追加)")) {
                    audio->AddClip(audio->MakeUniqueClipName(), "");
                }

                ImGui::Spacing();
                if (audio->IsBgm()) {
                    ImGui::Text("State: %s", audio->IsPlaying() ? "Playing (BGM)" : "Stopped");
                } else {
                    ImGui::Text("State: %zu voice(s) playing (3D: %zu)", audio->ActiveSeCount(), audio->ActiveSpatialCount());
                }
                ImGui::TextDisabled("Script: GetComponent<AudioSourceComponent>()->Play(\"name\")");
                ImGui::Unindent(8.0f);
            }
        }

        // ============================================
        // Audio Listener（3D 音響の聞き手）
        // ============================================
        if (auto* listener = e->GetComponent<AudioListenerComponent>()) {
            bool headerOpen = ImGui::CollapsingHeader("Audio Listener (3D 音響の聞き手)", ImGuiTreeNodeFlags_DefaultOpen);
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Remove Component")) pendingRemove = [e](){ e->RemoveComponent<AudioListenerComponent>(); };
                ImGui::EndPopup();
            }
            if (headerOpen) {
                ImGui::Indent(8.0f);
                bool enabled = listener->IsEnabled();
                if (ImGui::Checkbox("Enabled (有効化)##AudioListener", &enabled)) listener->SetEnabled(enabled);
                ImGui::Checkbox("Orient To Camera (向きはカメラに合わせる)##AudioListener", &listener->orientToCamera);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip(
                        "ON : 位置はこのエンティティ、向きはカメラ（三人称視点で画面の左右とスピーカーの左右が一致する）\n"
                        "OFF: 位置も向きもこのエンティティの Transform（+Z が正面）");
                }
                if (!e->GetComponent<TransformComponent>()) {
                    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "Transform がありません。カメラが聞き手になります");
                }
                const auto& pose = listener->LastPose();
                ImGui::TextDisabled("Pos (%.1f, %.1f, %.1f)  Fwd (%.2f, %.2f, %.2f)", pose.position.x, pose.position.y,
                                    pose.position.z, pose.forward.x, pose.forward.y, pose.forward.z);
                ImGui::TextDisabled("このコンポーネントが無いシーンでは描画中のカメラが聞き手になります");
                ImGui::Unindent(8.0f);
            }
        }

        ImGui::Separator();
        ImGui::Spacing();
        if (ImGui::Button("Add Component", ImVec2(-1, 30))) {
            ImGui::OpenPopup("AddComponentPopup");
        }
        if (ImGui::BeginPopup("AddComponentPopup")) {
            // Transform を持たないエンティティ（フォルダ等）にも後から付けられるようにする
            if (!e->GetComponent<TransformComponent>() && ImGui::MenuItem("Transform (変形)")) e->AddComponent<TransformComponent>();
            if (ImGui::MenuItem("Camera") && !e->GetComponent<CameraComponent>()) e->AddComponent<CameraComponent>();
            if (ImGui::MenuItem("Collider (当たり判定)") && !e->GetComponent<ColliderComponent>()) e->AddComponent<ColliderComponent>();
            if (ImGui::MenuItem("Rigidbody (物理演算)") && !e->GetComponent<RigidbodyComponent>()) e->AddComponent<RigidbodyComponent>();
            if (ImGui::MenuItem("Native Script") && !e->GetComponent<NativeScriptComponent>()) e->AddComponent<NativeScriptComponent>();
            if (ImGui::MenuItem("Text Renderer (文字描画)") && !e->GetComponent<TextRendererComponent>()) e->AddComponent<TextRendererComponent>();
            if (ImGui::MenuItem("Text Mesh (立体文字)") && !e->GetComponent<TextMeshComponent>()) e->AddComponent<TextMeshComponent>();
            if (ImGui::MenuItem("Audio Source (音源)") && !e->GetComponent<AudioSourceComponent>()) e->AddComponent<AudioSourceComponent>();
            if (ImGui::MenuItem("Audio Listener (3D 音響の聞き手)") && !e->GetComponent<AudioListenerComponent>()) e->AddComponent<AudioListenerComponent>();
            // Animation は ModelRenderer と組で使う。既に持っている場合は出さない
            if (!e->GetComponent<AnimationComponent>() && ImGui::MenuItem("Animation (アニメーション)")) e->AddComponent<AnimationComponent>();
            // 他エンティティのボーンに貼り付ける（武器を手に持たせる等）
            if (!e->GetComponent<BoneAttachmentComponent>() && ImGui::MenuItem("Bone Attachment (ボーン追従)")) e->AddComponent<BoneAttachmentComponent>();
            ImGui::EndPopup();
        }

    if (pendingRemove) pendingRemove();
      } else {
        ImGui::Text("No entity selected");
    }
  }
  ImGui::End();

  // Content Browser パネル
  if (ImGui::Begin("Content Browser")) {
    // フォルダのパスを表示
    std::string currentPathStr = currentDirectory_.string();
    // Windowsのバックスラッシュをスラッシュに置換
    std::replace(currentPathStr.begin(), currentPathStr.end(), '\\', '/');
    ImGui::Text("Assets: %s", currentPathStr.c_str());
    ImGui::SameLine();

    // 上の階層へ戻るボタン (Resourcesより上には行かないようにする簡易制御)
    if (ImGui::Button("Up") && currentDirectory_ != "Resources" && currentDirectory_ != "Resources/") {
      currentDirectory_ = currentDirectory_.parent_path();
    }
    ImGui::Separator();

    // グリッドレイアウトの計算
    float cellSize = 90.0f;
    float panelWidth = ImGui::GetContentRegionAvail().x;
    int columnCount = (int)(panelWidth / cellSize);
    if (columnCount < 1) columnCount = 1;

    if (ImGui::BeginTable("ContentTable", columnCount)) {
      if (std::filesystem::exists(currentDirectory_)) {
        for (const auto& entry : std::filesystem::directory_iterator(currentDirectory_)) {
          ImGui::TableNextColumn();
          const auto& path = entry.path();
          std::string filename = path.filename().string();

          if (entry.is_directory()) {
            // ディレクトリをアイコンで表示
            ImTextureID iconId = (ImTextureID)RC::GetRenderContext().Textures().GetSrv(folderIconTex_).ptr;
            ImGui::BeginGroup();
            bool clicked = ImGui::ImageButton(filename.c_str(), iconId, ImVec2(cellSize - 30, cellSize - 30));
            ImGui::TextWrapped("%s", filename.c_str());
            ImGui::EndGroup();

            if (clicked) {
              currentDirectory_ /= path.filename();
            }
          } else {
            // ファイルをアイコンで表示し、ドラッグ可能にする
            std::string ext = path.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

            int iconTexId = fileIconTex_;
            if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp") iconTexId = fileImageTex_;
            else if (ext == ".obj" || ext == ".blend" || ext == ".fbx" || ext == ".gltf") iconTexId = file3DTex_;
            else if (ext == ".mtl" || ext == ".mat") iconTexId = fileMaterialTex_;
            else if (ext == ".md" || ext == ".txt" || ext == ".json") iconTexId = fileDocTex_;
            else if (ext == ".ttf" || ext == ".otf") iconTexId = fileFontTex_;

            ImTextureID iconId = (ImTextureID)RC::GetRenderContext().Textures().GetSrv(iconTexId).ptr;
            ImGui::BeginGroup();
            ImGui::ImageButton(filename.c_str(), iconId, ImVec2(cellSize - 30, cellSize - 30));

            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
              std::string payloadPath = path.string();
              // Windowsのバックスラッシュをスラッシュに置換してペイロードに渡す
              std::replace(payloadPath.begin(), payloadPath.end(), '\\', '/');
              ImGui::SetDragDropPayload("CONTENT_BROWSER_ITEM", payloadPath.c_str(), payloadPath.size() + 1);
              ImGui::Text("Dragging: %s", filename.c_str());
              ImGui::EndDragDropSource();
            }

            ImGui::TextWrapped("%s", filename.c_str());
            ImGui::EndGroup();
          }
        }
      }
      ImGui::EndTable();
    }
  }
  ImGui::End();

  // Console パネル
  if (ImGui::Begin("Console")) {
    if (ImGui::Button("Clear")) {
      Log::ClearHistory();
    }
    ImGui::Separator();

    // スクロール可能な領域を作成
    ImGui::BeginChild("LogScrollRegion", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
    const auto& history = Log::GetHistory();
    for (const auto& msg : history) {
      ImVec4 color = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
      std::string lowerMsg = msg;
      std::transform(lowerMsg.begin(), lowerMsg.end(), lowerMsg.begin(), ::tolower);

      if (lowerMsg.find("error") != std::string::npos || lowerMsg.find("fail") != std::string::npos ||
          lowerMsg.find("exception") != std::string::npos || lowerMsg.find("fatal") != std::string::npos) {
        color = ImVec4(1.0f, 0.4f, 0.4f, 1.0f); // 赤 (エラー)
      } else if (lowerMsg.find("warn") != std::string::npos) {
        color = ImVec4(1.0f, 0.8f, 0.2f, 1.0f); // 黄 (警告)
      } else if (lowerMsg.find("success") != std::string::npos || lowerMsg.find("init") != std::string::npos ||
                 lowerMsg.find("create") != std::string::npos || lowerMsg.find("load") != std::string::npos ||
                 lowerMsg.find("save") != std::string::npos || lowerMsg.find("start") != std::string::npos) {
        color = ImVec4(0.5f, 1.0f, 0.5f, 1.0f); // 緑 (成功/初期化系)
      } else if (msg.find("[") == 0) {
        color = ImVec4(0.3f, 0.7f, 1.0f, 1.0f); // 水色 (システム/カテゴリタグあり)
      }

      ImGui::PushStyleColor(ImGuiCol_Text, color);
      ImGui::TextUnformatted(msg.c_str());
      ImGui::PopStyleColor();
    }

    // オートスクロール（一番下にいる場合のみ追従）
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
      ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
  }
  ImGui::End();


  // ============================
  // Particle Editor パネル
  // ============================
  if (showParticleEditor_) {
    if (ImGui::Begin("Particle Editor Parameters", &showParticleEditor_)) {
      if (!peParticle_) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Particle Editor Preview");
        ImGui::Separator();
        ImGui::TextWrapped("Initialize the preview particle system to start editing.");
        if (ImGui::Button("Initialize Preview Particle", ImVec2(-1, 40))) {
          peParticle_ = std::make_unique<GPUParticle>();
          if (core) {
            // リソース初期化 (背景色: 暗いグレー)
            peRenderTexture_.Initialize(core, 512, 512, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, {0.1f, 0.1f, 0.1f, 1.0f});

            // カメラ初期化
            peCamera_.Initialize(nullptr, {0.0f, 2.0f, -15.0f}, {0.0f, 0.0f, 0.0f}, 0.45f, 1.0f, 0.1f, 100.0f);

            SceneContext ctx{};
            ctx.core = core;
            ctx.pipelineManager = pm;
            peParticle_->Initialize(ctx);
            peParticle_->SetPreviewMode(true);
            peInitialized_ = true;
          }
        }
      } else {
        // --- パラメータ編集 UI ---
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.6f, 1.0f), "Emitter Parameters");
        ImGui::Separator();

        // エミッタ形状
        const char* shapeNames[] = {"Point", "Sphere", "Box", "Cone"};
        int shapeInt = static_cast<int>(peParticle_->emitterShape_);
        if (ImGui::Combo("Emitter Shape", &shapeInt, shapeNames, IM_ARRAYSIZE(shapeNames))) {
          peParticle_->emitterShape_ = static_cast<EmitterShape>(shapeInt);
        }

        // 形状別パラメータ
        if (peParticle_->emitterShape_ == EmitterShape::Sphere || peParticle_->emitterShape_ == EmitterShape::Cone) {
          ImGui::DragFloat("Shape Radius", &peParticle_->shapeRadius_, 0.1f, 0.0f, 50.0f);
        }
        if (peParticle_->emitterShape_ == EmitterShape::Cone) {
          float angleDeg = peParticle_->coneAngle_ * 180.0f / 3.14159265f;
          if (ImGui::DragFloat("Cone Angle (deg)", &angleDeg, 1.0f, 0.0f, 90.0f)) {
            peParticle_->coneAngle_ = angleDeg * 3.14159265f / 180.0f;
          }
        }
        if (peParticle_->emitterShape_ == EmitterShape::Box) {
          ImGui::DragFloat3("Box Size", &peParticle_->shapeBoxSize_.x, 0.1f, 0.0f, 50.0f);
        }

        // 殻（Electric 専用: 箱の丸み / 表面からの浮き / Y 軸回転）
        if (peParticle_->GetParticleType() == ParticleType::Electric) {
          if (peParticle_->emitterShape_ == EmitterShape::Box) {
            ImGui::SliderFloat("Shell Roundness", &peParticle_->shellRoundness_, 0.0f, 1.0f);
          }
          ImGui::DragFloat("Shell Margin", &peParticle_->shellMargin_, 0.005f, 0.0f, 2.0f, "%.3f m");
          float shellYawDeg = peParticle_->shellYaw_ * 180.0f / 3.14159265f;
          if (ImGui::DragFloat("Shell Yaw (deg)", &shellYawDeg, 1.0f, -360.0f, 360.0f)) {
            peParticle_->shellYaw_ = shellYawDeg * 3.14159265f / 180.0f;
          }
        }

        ImGui::Separator();
        ImGui::Text("Emission");
        int emit = static_cast<int>(peParticle_->GetEmitCount());
        if (ImGui::SliderInt("Emit Count", &emit, 0, 100)) {
          peParticle_->SetEmitCount(static_cast<uint32_t>(emit));
        }
        int maxP = static_cast<int>(peParticle_->GetMaxParticles());
        if (ImGui::SliderInt("Max Particles", &maxP, 256, 16384)) {
          peParticle_->SetMaxParticles(static_cast<uint32_t>(maxP));
        }

        ImGui::Separator();
        ImGui::Text("Lifetime & Scale");
        ImGui::DragFloat("Min Lifetime", &peParticle_->minLifeTime_, 0.1f, 0.1f, 30.0f);
        ImGui::DragFloat("Max Lifetime", &peParticle_->maxLifeTime_, 0.1f, 0.1f, 30.0f);
        ImGui::DragFloat("Min Scale", &peParticle_->minScale_, 0.01f, 0.01f, 5.0f);
        ImGui::DragFloat("Max Scale", &peParticle_->maxScale_, 0.01f, 0.01f, 5.0f);

        ImGui::Separator();
        ImGui::Text("Velocity & Gravity");
        ImGui::DragFloat3("Base Velocity", &peParticle_->baseVelocity_.x, 0.001f);
        ImGui::DragFloat("Velocity Variance", &peParticle_->velocityVariance_, 0.001f, 0.0f, 1.0f);
        ImGui::DragFloat("Gravity", &peParticle_->gravity_, 0.01f, 0.0f, 20.0f);

        ImGui::Separator();
        ImGui::Text("Position");
        ImGui::DragFloat3("Emitter Position", &peParticle_->emitterPosition_.x, 0.1f);
        ImGui::DragFloat3("Emitter Offset", &peParticle_->emitterOffset_.x, 0.05f);

        ImGui::Separator();
        ImGui::Text("Color");
        ImGui::ColorEdit4("Start Color", &peParticle_->startColor_.x);
        ImGui::ColorEdit4("End Color", &peParticle_->endColor_.x);

        // ParticleType と BlendMode
        ImGui::Separator();
        ImGui::Text("Rendering");
        const char* typeNames[] = {"Default", "Explosion", "Rain", "Fire", "Electric"};
        static_assert(IM_ARRAYSIZE(typeNames) == static_cast<int>(ParticleType::Count),
                      "typeNames と ParticleType の数を揃えること");
        int currentTypeInt = static_cast<int>(peParticle_->GetParticleType());
        if (ImGui::Combo("Particle Type", &currentTypeInt, typeNames, IM_ARRAYSIZE(typeNames))) {
          peParticle_->SetParticleType(static_cast<ParticleType>(currentTypeInt));
        }
        const char* blendNames[] = {"None", "Normal", "Add", "Subtract", "Multiply", "Screen", "Premultiplied"};
        int blendInt = static_cast<int>(peParticle_->GetBlendMode());
        if (ImGui::Combo("Blend Mode", &blendInt, blendNames, IM_ARRAYSIZE(blendNames))) {
          peParticle_->SetBlendMode(static_cast<BlendMode>(blendInt));
        }

        // Pipeline Prefix
        ImGui::Separator();
        ImGui::Text("Pipeline");
        const char* pipelineNames[] = {"gpu_particle", "gpu_particle_bubble", "gpu_particle_fire", "gpu_particle_electric"};
        int currentPipelineInt = 0;
        for (int i = 0; i < IM_ARRAYSIZE(pipelineNames); ++i) {
            if (peParticle_->GetPipelinePrefix() == pipelineNames[i]) {
                currentPipelineInt = i;
                break;
            }
        }
        if (ImGui::Combo("Pipeline Prefix", &currentPipelineInt, pipelineNames, IM_ARRAYSIZE(pipelineNames))) {
            peParticle_->SetPipelinePrefix(pipelineNames[currentPipelineInt]);
        }

        // プリセット（タイプ・パイプライン・寿命・色などをまとめて設定）
        ImGui::Separator();
        ImGui::Text("Presets");
        if (ImGui::Button("Electric (帯電)", ImVec2(-1, 0))) {
            peParticle_->ApplyElectricPreset();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("モデルの周りを電流がビリビリと這うプリセット。\n殻の大きさは Box Size（直径）と Emitter Offset で対象モデルに合わせる。");
        }

        // テクスチャ変更
        ImGui::Separator();
        ImGui::Text("Texture");
        ImGui::InputText("Texture Path", peTexPath_, sizeof(peTexPath_));
        ImGui::SameLine();
        if (ImGui::Button("Apply##Tex")) {
          peParticle_->SetTexture(std::string(peTexPath_));
        }

        // JSON Save / Load
        ImGui::Separator();
        ImGui::Text("File I/O");
        ImGui::InputText("JSON Path", peJsonPath_, sizeof(peJsonPath_));
        if (ImGui::Button("Save", ImVec2(100, 30))) {
          peParticle_->SaveToJson(std::string(peJsonPath_));
        }
        ImGui::SameLine();
        if (ImGui::Button("Load", ImVec2(100, 30))) {
          peParticle_->LoadFromJson(std::string(peJsonPath_));
          // テクスチャパスの同期
          #ifdef _MSC_VER
          strncpy_s(peTexPath_, sizeof(peTexPath_), peParticle_->texturePath_.c_str(), _TRUNCATE);
          #endif
        }
      }
    }
    ImGui::End();

    // プレビュー用ウィンドウ
    if (peInitialized_) {
      ImGui::SetNextWindowSizeConstraints(ImVec2(256, 256), ImVec2(2048, 2048));
      if (ImGui::Begin("Particle Editor Preview", &showParticleEditor_)) {
        ImVec2 contentSize = ImGui::GetContentRegionAvail();
        if (contentSize.x > 0 && contentSize.y > 0) {
          float size = std::min(contentSize.x, contentSize.y);
          // 中央に配置
          ImGui::SetCursorPos(ImVec2((contentSize.x - size) * 0.5f + ImGui::GetCursorPosX(),
                                     (contentSize.y - size) * 0.5f + ImGui::GetCursorPosY()));
          ImGui::Image((ImTextureID)peRenderTexture_.GetSRVGPU().ptr, ImVec2(size, size));
        }
      }
      ImGui::End();
    }
  }

  // === Screenshot Pop-out ===
  if (core) {
    ScreenCapture::DrawImGui(deltaTime, core);
  }

#endif
}

PlayState EditorManager::GetPlayState() const {
  return playState_;
}

void EditorManager::SetPlayState(PlayState state) {
  playState_ = state;
}

void EditorManager::ExportRenderQueueDump() {
  const auto& queue = RC::GetRenderContext().GetLastCommandHistory();
  auto now = std::chrono::system_clock::now();
  std::string timeStr = std::format("{:%Y-%m-%d_%H-%M-%S}", std::chrono::current_zone()->to_local(now));

  std::error_code ec;
  std::filesystem::create_directories("../logs/render_queue", ec);

  std::string filename = "../logs/render_queue/dump_" + timeStr + ".txt";
  std::ofstream ofs(filename);
  if (ofs) {
    ofs << "--- Render Queue Dump ---\n";
    ofs << "Total Commands: " << queue.size() << "\n\n";
    for (size_t i = 0; i < queue.size(); ++i) {
      const auto& cmd = queue[i];
      std::string displayName(cmd.debugName); // debugName は string_view
      if (cmd.debugIndex >= 0) {
        std::string resourceName = "";
        if (cmd.debugName.find("Model") != std::string::npos) {
          if (auto* m = RC::GetRenderContext().Models().Get(cmd.debugIndex)) {
            resourceName = std::filesystem::path(m->GetFilePath()).filename().string();
          }
        } else if (cmd.debugName.find("Sprite") != std::string::npos) {
          if (auto* s = RC::GetRenderContext().Sprites().Get(cmd.debugIndex)) {
            resourceName = std::filesystem::path(s->GetFilePath()).filename().string();
          }
        }
        if (!resourceName.empty()) {
          displayName += " [" + resourceName + "]";
        }
      }

      if (cmd.sortKey == 0) {
        ofs << std::format("[{}] {} (Index: {})\n", i, displayName, cmd.debugIndex);
      } else {
        uint8_t layer = static_cast<uint8_t>(cmd.sortKey >> 56);
        std::string layerStr = (layer == 0) ? "Opaque" : (layer == 1) ? "Alpha" : (layer == 2) ? "Glass" : (layer == 3) ? "Overlay" : "?";
        uint32_t depth24 = static_cast<uint32_t>(cmd.sortKey & 0x00FFFFFF);
        uint16_t psoHash = static_cast<uint16_t>((cmd.sortKey >> 40) & 0xFFFF);
        uint16_t texHash = static_cast<uint16_t>((cmd.sortKey >> 24) & 0xFFFF);

        ofs << std::format("[{}] {} (Index: {}) - Layer: {}({}), Depth24: {}, PSO: {:04X}, Tex: {:04X}, SortKey: {:016X}\n",
          i, displayName, cmd.debugIndex, layer, layerStr, depth24, psoHash, texHash, cmd.sortKey);
      }
    }
    Log::Print("[Editor] Exported Render Queue to: " + filename);
  } else {
    Log::Print("[Editor] Failed to open file for export: " + filename);
  }
}

void EditorManager::SaveConfig() {
  nlohmann::json j;
  j["showPerfWindow"] = showPerfWindow_;
  j["showRenderQueue"] = showRenderQueue_;
  j["showDemoWindow"] = showDemoWindow_;
  j["showParticleEditor"] = showParticleEditor_;
  j["showSceneFlow"] = showSceneFlow_;

  std::ofstream ofs(Chaso::PrepareSettingPath(Chaso::kEditorConfigPath));
  if (ofs) {
    ofs << j.dump(4);
  }
}

void EditorManager::LoadConfig() {
  std::ifstream ifs(Chaso::PrepareSettingPath(Chaso::kEditorConfigPath));
  if (ifs) {
    try {
      nlohmann::json j;
      ifs >> j;
      if (j.contains("showPerfWindow")) showPerfWindow_ = j["showPerfWindow"];
      if (j.contains("showRenderQueue")) showRenderQueue_ = j["showRenderQueue"];
      if (j.contains("showDemoWindow")) showDemoWindow_ = j["showDemoWindow"];
      if (j.contains("showParticleEditor")) showParticleEditor_ = j["showParticleEditor"];
      if (j.contains("showSceneFlow")) showSceneFlow_ = j["showSceneFlow"];
    } catch (...) {
      Log::Print("[Editor] Failed to parse EditorConfig.json");
    }
  }
}


void EditorManager::Term() {
  if (peParticle_) {
    peParticle_->Finalize();
    peParticle_.reset();
  }
  peRenderTexture_.Release();
  peInitialized_ = false;
}

void EditorManager::RenderParticlePreview(ID3D12GraphicsCommandList* cl, Dx12Core* core, PipelineManager* pm, float deltaTime) {
  if (!showParticleEditor_ || !peParticle_ || !peInitialized_) return;

  // RenderTargetへ状態遷移
  peRenderTexture_.TransitionToRenderTarget(cl);

  D3D12_CPU_DESCRIPTOR_HANDLE rtv = peRenderTexture_.GetRTV();
  cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

  // DescriptorHeapのセット（SRV, UAVを使うため）
  ID3D12DescriptorHeap* heaps[] = { core->SRV().Heap() };
  cl->SetDescriptorHeaps(1, heaps);

  // 背景のクリア
  const float clearColor[4] = { 0.1f, 0.1f, 0.1f, 1.0f }; // 暗いグレー
  cl->ClearRenderTargetView(rtv, clearColor, 0, nullptr);

  // ビューポートとシザー
  D3D12_VIEWPORT viewport{};
  viewport.Width = 512.0f;
  viewport.Height = 512.0f;
  viewport.MaxDepth = 1.0f;
  cl->RSSetViewports(1, &viewport);

  D3D12_RECT scissor{};
  scissor.right = 512;
  scissor.bottom = 512;
  cl->RSSetScissorRects(1, &scissor);

  // カメラ更新とSceneContext構築
  peCamera_.Update();
  SceneContext ctx{};
  ctx.core = core;
  ctx.pipelineManager = pm;
  ctx.camera = &peCamera_;
  ctx.deltaTime = deltaTime;

  // パーティクルの更新と描画
  peParticle_->Update(peCamera_.GetView(), peCamera_.GetProjection(), deltaTime);
  peParticle_->Render(ctx, cl);

  // SRVへ状態遷移（ImGuiで描画するため）
  peRenderTexture_.TransitionToShaderResource(cl);
}
