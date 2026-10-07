#include "TextureManager.h"
#include "DeferredReleaseQueue/DeferredReleaseQueue.h"
#include <memory>
#include "DescriptorHeap/DescriptorHeap.h"
#include "SRVManager/SRVManager.h"
#include "Common/Log/Log.h"
#include <format>
#include <chrono>
#include <future>
#include "RenderCommon.h"

void TextureManager::Init(SRVManager *srv) {
  srv_ = srv;
  // テクスチャ転送用に独自のコマンドリスト（DirectQueue、バッファ数1）を作る
  loadCmd_.Init(srv->Device(), D3D12_COMMAND_LIST_TYPE_DIRECT, 1);
  
  // Init() 終了直後は Close されているので、記録開始のために一度 Reset しておく
  loadCmd_.List()->Reset(loadCmd_.GetAllocator(0), nullptr);
}

void TextureManager::Term() {

  // 残っている転送を全て待機
  if (srv_) {
    loadCmd_.FlushGPU();
  }
  ReleasePendingUploads();

  for (auto &[_, tex] : cache_) {
    tex.Term(srv_); // 必ず srv を渡す
  }
  cache_.clear();
  srv_ = nullptr;
  loadCmd_.Term();
}

void TextureManager::ReleasePendingUploads() {
  uint64_t completed = loadCmd_.GetCompletedFenceValue();
  auto it = pendingUploads_.begin();
  while (it != pendingUploads_.end()) {
    if (it->fenceValue <= completed) {
      it = pendingUploads_.erase(it);
    } else {
      ++it;
    }
  }
}

D3D12_GPU_DESCRIPTOR_HANDLE TextureManager::Load(const std::string &path,
                                                 bool srgb) {
  std::lock_guard lock(mtx_);
  assert(srv_);
  ReleasePendingUploads(); // 前の転送が終わっていれば解放

  std::string npath = Log::NormalizePath(path);
  auto it = cache_.find(npath);
  if (it != cache_.end() && it->second.IsLoaded()) {
    if (loggedPaths_.find(npath) == loggedPaths_.end()) {
      Log::Print(std::format("[Texture] キャッシュ利用: {}, SRV Index: {}", npath, it->second.SrvHandle().index));
      loggedPaths_.insert(npath);
    }
    return it->second.GpuSrv();
  }
  Texture2D &tex = cache_[npath];
  auto uploadRes = tex.LoadFromFile(*srv_, loadCmd_, npath, srgb);
  if (uploadRes) {
    Log::Print(std::format("[Texture] {}: {}, SRV Index: {}",
                           tex.IsFallback() ? "白1x1で代替" : "ロード完了", npath,
                           tex.SrvHandle().index));
    uint64_t fence = loadCmd_.ExecuteAndReset();
    pendingUploads_.push_back({uploadRes, fence});
  }

  return tex.GpuSrv();

}

Texture2D *TextureManager::Get(const std::string &path) {
  std::lock_guard lock(mtx_);
  std::string npath = Log::NormalizePath(path);
  auto it = cache_.find(npath);
  if (it == cache_.end())
    return nullptr;
  return &it->second;
}

TextureManager::TextureID TextureManager::LoadID(const std::string &path,
                                                 bool srgb) {
  std::string npath = Log::NormalizePath(path);
  TextureID id = 0;
  bool alreadyExists = false;

  {
    std::lock_guard lock(mtx_);
    // 既にIDがあるか確認
    if (auto itId = pathToId_.find(npath); itId != pathToId_.end()) {
      id = itId->second;
      alreadyExists = true;
    } else {
      // 新規ID発行（プレースホルダを作成）
      id = nextId_++;
      pathToId_[npath] = id;
      idToPath_[id] = npath;
    }
  }

  if (alreadyExists) {
    return id; // 既にロード中またはロード済みの場合はタスクを追加しない
  }

  // 実際のロード処理（非同期タスクとして実行）
  auto task = std::async(std::launch::async, [this, npath, srgb] {
    try {
      auto start = std::chrono::high_resolution_clock::now();

      auto &tex = [this, &npath]() -> Texture2D & {
        std::lock_guard lock(mtx_);
        return cache_[npath];
      }();

      // 1. CPUデコード (完全並列)
      if (!tex.LoadCPU(npath, srgb)) {
        Log::Print("[Texture] ロード失敗: " + npath);
        return;
      }

      // 2. GPU転送 (最小限のロック)
      Microsoft::WRL::ComPtr<ID3D12Resource> uploadRes;
      {
        std::lock_guard lock(mtx_);
        // 二重ロード防止（最終確認）
        if (!tex.IsLoaded()) {
          uploadRes = tex.Upload(*srv_, loadCmd_);
          if (uploadRes) {
            uint64_t fence = loadCmd_.ExecuteAndReset();
            pendingUploads_.push_back({uploadRes, fence});
          }
        }
      }

      if (tex.IsLoaded()) {
        auto end = std::chrono::high_resolution_clock::now();
        Log::Print(std::format(
            "[Texture] {}: {} (Time: {:.3f}ms), SRV Index: {}",
            tex.IsFallback() ? "白1x1で代替" : "ロード完了", npath,
            std::chrono::duration<float, std::milli>(end - start).count(),
            tex.SrvHandle().index));
      }
    } catch (const std::exception &e) {
      Log::Print(
          std::format("[Texture] ロード中に例外発生 ({}): {}", npath, e.what()));
    } catch (...) {
      Log::Print(std::format("[Texture] ロード中に不明な例外発生 ({})", npath));
    }
  });

  // RenderContext にタスク追加
  RC::AddLoadingTask(std::move(task));

  return id;
}

void TextureManager::Unload(TextureID id) {
  std::lock_guard lock(mtx_);
  auto it = idToPath_.find(id);
  if (it == idToPath_.end())
    return;

  std::string path = it->second;
  auto itTex = cache_.find(path);
  if (itTex != cache_.end()) {
    // テクスチャ本体と SRV の返却は、直前のフレームの GPU が使い終わってから行う。
    // （SRV を先に返すと、同じスロットが別のテクスチャに再利用されて、描画中のフレームが
    //   別の絵を読んでしまう）
    auto tex = std::make_shared<Texture2D>(std::move(itTex->second));
    SRVManager *srv = srv_;
    cache_.erase(itTex);
    DeferredReleaseQueue::DeferCall([tex, srv]() { tex->Term(srv); });
  }

  pathToId_.erase(path);
  idToPath_.erase(it);

  // ログ履歴からも消しておく（再度ロードした時にログが出るように）
  loggedPaths_.erase(path);
  Log::Print(std::format("[Texture] 破棄完了: {}", path));
}

D3D12_GPU_DESCRIPTOR_HANDLE TextureManager::GetSrv(TextureID id) const {
  std::lock_guard lock(mtx_);

  auto getWhiteTex = [&]() -> D3D12_GPU_DESCRIPTOR_HANDLE {
    std::string whitePath = Log::NormalizePath("Resources/white1x1.png");
    auto it = cache_.find(whitePath);
    if (it != cache_.end() && it->second.IsLoaded()) {
        return it->second.GpuSrv();
    }
    return D3D12_GPU_DESCRIPTOR_HANDLE{};
  };

  auto it = idToPath_.find(id);
  if (it == idToPath_.end())
    return getWhiteTex();
    
  auto itTex = cache_.find(it->second);
  if (itTex == cache_.end() || !itTex->second.IsLoaded())
    return getWhiteTex();
    
  return itTex->second.GpuSrv();
}

bool TextureManager::IsLoaded(TextureID id) const {
  std::lock_guard lock(mtx_);
  auto it = idToPath_.find(id);
  if (it == idToPath_.end())
    return false;
  auto itTex = cache_.find(it->second);
  return (itTex != cache_.end() && itTex->second.IsLoaded());
}

const DirectX::TexMetadata *TextureManager::GetMeta(TextureID id) const {
  std::lock_guard lock(mtx_);
  auto it = idToPath_.find(id);
  if (it == idToPath_.end())
    return nullptr;
  auto itTex = cache_.find(it->second);
  if (itTex == cache_.end() || !itTex->second.IsLoaded())
    return nullptr;
  return &itTex->second.Metadata(); // 既存の取得関数を流用
}

Texture2D *TextureManager::GetTexture(TextureID id) {
  std::lock_guard lock(mtx_);
  auto it = idToPath_.find(id);
  if (it == idToPath_.end())
    return nullptr;
  return Get(it->second);
}

std::string
TextureManager::GetPathBySrv(D3D12_GPU_DESCRIPTOR_HANDLE srv) const {
  std::lock_guard lock(mtx_);
  if (srv.ptr == 0)
    return {};

  for (const auto &kv : cache_) {
    const auto &path = kv.first;
    const auto &tex = kv.second;
    if (tex.IsLoaded() && tex.GpuSrv().ptr == srv.ptr) {
      return path;
    }
  }
  return {};
}
