#include "ModelManager.h"
#include "DeferredReleaseQueue/DeferredReleaseQueue.h"
#include <memory>

#include "Model/ModelObject.h"
#include "Model/ModelMesh.h"
#include "Common/Log/Log.h"

#include <filesystem>
#include <future>
#include <format>
#include <chrono>
#include "RenderCommon.h"
#include "Render/FrameResource.h"

namespace RC {

namespace {

static std::string NormalizeMeshKey_(const std::string &path) {
  namespace fs = std::filesystem;

  std::error_code ec;
  fs::path p(path);

  // 絶対パス化できるなら絶対パス。失敗したら相対のまま lexically_normal。
  fs::path abs = fs::absolute(p, ec);
  fs::path key = ec ? p.lexically_normal() : abs.lexically_normal();
  return key.string();
}

} // namespace

ModelManager::~ModelManager() = default;

void ModelManager::Init(ID3D12Device *device, TextureManager *texman) {
  device_ = device;
  texman_ = texman;
  models_.clear();
  meshCache_.clear();
}

void ModelManager::SetSkinningCS(PipelineManager *pm,
                                  SRVManager *srvMgr) {
  pipelineMgr_ = pm;
  srvMgr_ = srvMgr;
}

void ModelManager::Term() {
  models_.clear();
  meshCache_.clear();
  device_ = nullptr;
  texman_ = nullptr;
}

int ModelManager::AllocSlot_() {
  // すでにロックされた状態で呼ばれる想定（Load 等から）
  for (int i = 0; i < static_cast<int>(models_.size()); ++i) {
    if (!models_[i].inUse) {
      return i;
    }
  }
  models_.emplace_back();
  return static_cast<int>(models_.size()) - 1;
}

bool ModelManager::IsValid(int handle) const {
  return (handle >= 0 && handle < static_cast<int>(models_.size()) &&
          models_[handle].inUse && models_[handle].ptr);
}

::ModelObject *ModelManager::Get(int handle) {
  std::lock_guard lock(mtx_);
  if (!IsValid(handle)) {
    return nullptr;
  }
  return models_[handle].ptr.get();
}

const ::ModelObject *ModelManager::Get(int handle) const {
  if (!IsValid(handle)) {
    return nullptr;
  }
  return models_[handle].ptr.get();
}

std::shared_ptr<::ModelMesh> ModelManager::GetOrLoadMesh_(
    const std::string &path) {
  const std::string key = NormalizeMeshKey_(path);

  // すでにロード済みなら共有 (ロックを取得して確認)
  {
    std::lock_guard lock(mtx_);
    if (auto it = meshCache_.find(key); it != meshCache_.end()) {
      if (it->second) {
        return it->second;
      }
      meshCache_.erase(it);
    }
  }

  // 未ロードならロード (重い処理なのでロックを外す)
  auto mesh = std::make_shared<::ModelMesh>();
  if (!mesh->LoadObj(device_,path)) {
    return nullptr;
  }

  // キャッシュに登録
  {
    std::lock_guard lock(mtx_);
    if (auto it = meshCache_.find(key); it != meshCache_.end()) {
      if (it->second) {
        return it->second;
      }
    }
    meshCache_[key] = mesh;
  }

  return mesh;
}

int ModelManager::Load(const std::string &path) {
  std::lock_guard lock(mtx_);
  if (!device_) {
    return -1;
  }

  std::string npath = Log::NormalizePath(path);

  // 既に同じパスがロード中、またはロード済みか確認（Meshキャッシュの活用）
  // ただしハンドルはユニークに返したいので、新しいスロットは常に作る。
  const int handle = AllocSlot_();
  models_[handle].inUse = true;

  auto obj = std::make_unique<::ModelObject>();
  obj->SetReady(false); // まだ準備中
  obj->SetFilePath(npath);
  obj->SetTextureManager(texman_);

  // ポインタを先にセットしておく（Get で null を返さないように）
  models_[handle].ptr = std::move(obj);

  // --- 非同期ロードタスクの開始 ---
  auto task = std::async(std::launch::async, [this, handle, npath] {
    try {
      auto start = std::chrono::high_resolution_clock::now();

      // 1. メッシュのロード (MeshGenerator等で非常に重い処理)
      std::shared_ptr<::ModelMesh> mesh;
      {
        // GetOrLoadMesh_ 内部で適切にロックするため、ここでは全体をロックしない
        mesh = GetOrLoadMesh_(npath);
      }

      if (!mesh) {
        Log::Print("[Model] ロード失敗: " + npath);
        return;
      }

      // 2. モデルオブジェクトの初期化 (GPUリソース作成など)
      {
        std::lock_guard lock(mtx_);
        auto *ptr = Get(handle);
        if (ptr) {
          ptr->Initialize(device_);
          ptr->SetMesh(mesh);
          // CS スキニングの自動設定
          if (pipelineMgr_ && srvMgr_ && mesh->HasSkinData()) {
            ptr->Resource().SetSkinningCS(device_, pipelineMgr_, srvMgr_);
          }
          ptr->SetReady(true); // 読み込み完了
        }
      }

      auto end = std::chrono::high_resolution_clock::now();
      Log::Print(std::format(
          "[Model] ロード完了: {} (Time: {:.3f}ms)", npath,
          std::chrono::duration<float, std::milli>(end - start).count()));
    } catch (const std::exception &e) {
      Log::Print(std::format("[Model] ロード中に例外発生 ({}): {}", npath, e.what()));
    } catch (...) {
      Log::Print(std::format("[Model] ロード中に不明な例外発生 ({})", npath));
    }
  });

  // RenderContext にタスクを登録して、WaitAllLoads で待てるようにする
  RC::AddLoadingTask(std::move(task));

  return handle;
}

void ModelManager::Unload(int handle) {
  std::lock_guard lock(mtx_);
  if (!IsValid(handle)) {
    return;
  }
  Log::Print("[Model] 破棄完了: " + Log::NormalizePath(models_[handle].ptr->GetFilePath()));
  // CPU と GPU を並行させているので、直前のフレームの GPU がまだこのモデルを描いているかもしれない。
  // 実体の破棄（頂点バッファ・CS スキニング用バッファの解放）は GPU が使い終わってから行う
  DeferredReleaseQueue::DeferDelete(std::move(models_[handle].ptr));
  models_[handle].inUse = false;
}

Transform *ModelManager::GetTransformPtr(int handle) {
  if (!IsValid(handle)) {
    return nullptr;
  }
  return &models_[handle].ptr->T();
}

void ModelManager::SetColor(int handle, const Vector4 &color) {
  if (!IsValid(handle)) {
    return;
  }
  models_[handle].ptr->SetColor(color);
}

void ModelManager::SetShininess(int handle, float shininess) {
  if (!IsValid(handle)) {
    return;
  }
  models_[handle].ptr->SetShininess(shininess);
}

void ModelManager::SetLightingMode(int handle, LightingMode m) {
  if (!IsValid(handle)) {
    return;
  }
  models_[handle].ptr->SetLightingMode(m);
}

void ModelManager::ClearLightingModeOverride(int handle) {
  if (!IsValid(handle)) {
    return;
  }
  models_[handle].ptr->ClearLightingModeOverride();
}

void ModelManager::SetMesh(int handle, const std::string &path) {
  {
    std::lock_guard lock(mtx_);
    if (!IsValid(handle)) {
      return;
    }
  }

  auto mesh = GetOrLoadMesh_(path);
  if (!mesh) {
    return;
  }

  {
    std::lock_guard lock(mtx_);
    if (!IsValid(handle)) {
      return;
    }
    models_[handle].ptr->SetMesh(mesh);
    // Mesh を変えると mtl のテクスチャ参照も変わる可能性があるので戻す
    models_[handle].ptr->ResetTextureToMtl();
  }
}

void ModelManager::ResetCursor(int handle) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  if (!IsValid(handle))
    return;
  models_[handle].ptr->ResetBatchCursor();
}

void ModelManager::AttachAnimation(int handle) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  if (!IsValid(handle)) return;
  models_[handle].ptr->AttachAnimation();
}

void ModelManager::AttachAnimation(int handle, const std::string& filePath) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  if (!IsValid(handle)) return;
  models_[handle].ptr->AttachAnimation(filePath);
}

void ModelManager::AttachAnimation(int handle, const std::string& filePath, int animIndex) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  if (!IsValid(handle)) return;
  models_[handle].ptr->AttachAnimation(filePath, animIndex);
}

void ModelManager::CrossfadeAnimation(int handle, const std::string& filePath, float blendDuration) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  if (!IsValid(handle)) return;
  models_[handle].ptr->CrossfadeAnimation(filePath, blendDuration);
}

void ModelManager::CrossfadeAnimation(int handle, const std::string& filePath, int animIndex, float blendDuration) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  if (!IsValid(handle)) return;
  models_[handle].ptr->CrossfadeAnimation(filePath, animIndex, blendDuration);
}

void ModelManager::UpdateAnimation(int handle, float dt) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  if (!IsValid(handle)) return;
  models_[handle].ptr->UpdateAnimation(dt);
}

float ModelManager::GetAnimationDuration(int handle) const {
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  if (!IsValid(handle)) return 0.0f;
  return models_[handle].ptr->GetAnimationDuration();
}

void ModelManager::ResetAllBatchCursors() {
  std::lock_guard lock(mtx_);
  for (auto &s : models_) {
    if (s.inUse && s.ptr) {
      s.ptr->ResetBatchCursor();
      // CS スキニングの Dispatch フラグをリセット
      s.ptr->Resource().ResetSkinningDispatched();
    }
  }
}

void ModelManager::DispatchAllSkinning(ID3D12GraphicsCommandList* cl, RC::FrameResource& frame) {
  std::lock_guard lock(mtx_);
  for (auto &s : models_) {
    if (s.inUse && s.ptr && s.ptr->HasSkinData() && s.ptr->Resource().HasCSSkinning()) {
      if (!s.ptr->Resource().IsSkinningDispatched()) {
        s.ptr->Resource().DispatchSkinning(cl, s.ptr->GetSkinMatrices(), frame);
      }
    }
  }
}

} // namespace RC
