#include "Texture2D.h"
#include "Common/ResourcePath.h"
#include "DescriptorHeap/DescriptorHeap.h"
#include "function/function.h"
#include "Dx12/CommandContext/CommandContext.h"
#include "Common/Log/Log.h"
#include <format>
#include <filesystem>
#include <system_error>
#include <vector>

Microsoft::WRL::ComPtr<ID3D12Resource> Texture2D::LoadFromFile(SRVManager &srv, CommandContext &cmd, const std::string &path, bool srgb) {
  if (!LoadCPU(path, srgb)) {
    return nullptr;
  }
  return Upload(srv, cmd);
}

bool Texture2D::LoadCPU(const std::string &path, bool srgb) {
  // ---- 画像読み込み（存在しなければ白 1x1）----
  // ゲーム側 Resources → エンジン側（Chaso/Resources）の順で探す
  const std::string resolved = Chaso::ResolvePath(path);
  std::wstring wpath(resolved.begin(), resolved.end());
  DirectX::ScratchImage image;
  isFallback_ = false;

  bool isDDS = (resolved.size() >= 4 && resolved.substr(resolved.size() - 4) == ".dds");
  HRESULT hr;

  if (isDDS) {
    // DDS 読み込み
    hr = DirectX::LoadFromDDSFile(wpath.c_str(), DirectX::DDS_FLAGS_NONE,
                                  nullptr, image);
  } else {
    // 通常画像読み込み
    const DirectX::WIC_FLAGS wicFlags =
        srgb ? DirectX::WIC_FLAGS_FORCE_SRGB : DirectX::WIC_FLAGS_IGNORE_SRGB;
    hr = DirectX::LoadFromWICFile(wpath.c_str(), wicFlags, nullptr, image);
  }

  if (FAILED(hr)) {
    // 描画は続行できるよう白 1x1 で代替するが、黙って成功扱いにはしない。
    // （読み込めていないのに「ロード完了」と出ると原因調査が遠回りになる）
    isFallback_ = true;
    std::string absStr = "解決失敗";
    try {
      std::error_code ec;
      const auto abs = std::filesystem::absolute(path, ec);
      if (!ec)
        absStr = abs.string();
    } catch (...) {
      // ログ用途なので絶対パスが取れなくても続行する
    }
    Log::Print(std::format("[WARN][Texture] 読み込み失敗のため白1x1で代替します: {} (HRESULT={:08X}, 絶対パス: {})",
                           path, static_cast<uint32_t>(hr), absStr));
    // 白 1x1 を生成
    DirectX::ScratchImage white;
    white.Initialize2D(DXGI_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 1);
    auto img = white.GetImage(0, 0, 0);
    uint8_t *p = img->pixels;
    p[0] = 255; p[1] = 255; p[2] = 255; p[3] = 255;
    mipImages_ = std::move(white);
  } else {
    if (isDDS) {
      // DDS は通常ミップマップを含んでいるのでそのまま使う
      mipImages_ = std::move(image);
    } else {
      // ミップ生成
      const DirectX::TEX_FILTER_FLAGS mipFilter =
          srgb ? DirectX::TEX_FILTER_SRGB : DirectX::TEX_FILTER_DEFAULT;

      DirectX::ScratchImage mips;
      hr = DirectX::GenerateMipMaps(image.GetImages(), image.GetImageCount(),
                                    image.GetMetadata(), mipFilter, 0, mips);

      mipImages_ = SUCCEEDED(hr) ? std::move(mips) : std::move(image);
    }
  }

  metadata_ = mipImages_.GetMetadata();

  // sRGB 設定の反映
  if (srgb) {
    metadata_.format = DirectX::MakeSRGB(metadata_.format);
  } else {
    metadata_.format = DirectX::MakeLinear(metadata_.format);
  }
  path_ = path;
  return true;
}

Microsoft::WRL::ComPtr<ID3D12Resource> Texture2D::Upload(SRVManager &srv, CommandContext &cmd) {
  ID3D12Device *device = srv.Device();
  if (mipImages_.GetImageCount() == 0) {
    Log::Print("[Texture2D] LoadCPU must be called before Upload (or texture was cleared during load).");
    assert(false && "LoadCPU must be called before Upload (or texture was cleared during load).");
    return nullptr;
  }

  // ---- GPU テクスチャ作成 ----
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION(metadata_.dimension);
  desc.Width = (UINT)metadata_.width;
  desc.Height = (UINT)metadata_.height;
  desc.DepthOrArraySize = (UINT16)metadata_.arraySize;
  desc.MipLevels = (UINT16)metadata_.mipLevels;
  desc.Format = metadata_.format;
  desc.SampleDesc.Count = 1;

  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;

  HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                               D3D12_RESOURCE_STATE_COPY_DEST,
                                               nullptr, IID_PPV_ARGS(&resource_));
  if (FAILED(hr)) {
    Log::Print("[Texture2D] Failed to create committed resource for texture: " + path_);
    assert(false && "Failed to create committed resource for texture");
    return nullptr;
  }
  if (resource_) {
    std::wstring wpath(path_.begin(), path_.end());
    resource_->SetName((L"Texture: " + wpath).c_str());
  }

  // ---- UploadHeap の作成と転送 ----
  const UINT numSubresources = (UINT)(metadata_.mipLevels * metadata_.arraySize);

  std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(numSubresources);
  std::vector<UINT> numRows(numSubresources);
  std::vector<UINT64> rowSizesInBytes(numSubresources);
  uint64_t uploadBufferSize = 0;

  device->GetCopyableFootprints(&desc, 0, numSubresources, 0, layouts.data(), numRows.data(), rowSizesInBytes.data(), &uploadBufferSize);

  D3D12_HEAP_PROPERTIES uploadHeap{};
  uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
  D3D12_RESOURCE_DESC bufferDesc{};
  bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  bufferDesc.Width = uploadBufferSize;
  bufferDesc.Height = 1;
  bufferDesc.DepthOrArraySize = 1;
  bufferDesc.MipLevels = 1;
  bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
  bufferDesc.SampleDesc.Count = 1;
  bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

  Microsoft::WRL::ComPtr<ID3D12Resource> uploadRes;
  hr = device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                       D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadRes));
  if (FAILED(hr)) {
    Log::Print("[Texture2D] Failed to create upload heap for texture: " + path_);
    assert(false && "Failed to create upload heap for texture");
    return nullptr;
  }
  
  uint8_t* pData = nullptr;
  hr = uploadRes->Map(0, nullptr, reinterpret_cast<void**>(&pData));
  if (FAILED(hr)) {
    Log::Print("[Texture2D] Failed to map upload resource for texture: " + path_);
    assert(false && "Failed to map upload resource for texture");
    return nullptr;
  }

  // 全サブリソース（配列面 × ミップ）を転送
  for (size_t arrayIdx = 0; arrayIdx < metadata_.arraySize; ++arrayIdx) {
    for (size_t mip = 0; mip < metadata_.mipLevels; ++mip) {
      const UINT subresource = (UINT)(arrayIdx * metadata_.mipLevels + mip);
      const DirectX::Image *img = mipImages_.GetImage(mip, arrayIdx, 0);
      if (!img) continue;

      const auto& layout = layouts[subresource];
      uint8_t* destSlice = pData + layout.Offset;
      const uint8_t* srcSlice = img->pixels;
      UINT bytesPerRow = (UINT)std::min<UINT64>(img->rowPitch, rowSizesInBytes[subresource]);

      for (UINT y = 0; y < numRows[subresource]; ++y) {
        memcpy(destSlice + y * layout.Footprint.RowPitch, srcSlice + y * img->rowPitch, bytesPerRow);
      }

      D3D12_TEXTURE_COPY_LOCATION dst{};
      dst.pResource = resource_.Get();
      dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      dst.SubresourceIndex = subresource;

      D3D12_TEXTURE_COPY_LOCATION srcLocation{};
      srcLocation.pResource = uploadRes.Get();
      srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      srcLocation.PlacedFootprint = layout;

      cmd.List()->CopyTextureRegion(&dst, 0, 0, 0, &srcLocation, nullptr);
    }
  }

  uploadRes->Unmap(0, nullptr);
  cmd.Transition(resource_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

  // SRV 作成（Cubemap と 2D で分岐）
  if (metadata_.IsCubemap()) {
    srv_ = srv.CreateTextureCube(resource_.Get(), metadata_.format, (UINT)metadata_.mipLevels);
  } else {
    srv_ = srv.CreateTexture2D(resource_.Get(), metadata_.format, (UINT)metadata_.mipLevels);
  }

  return uploadRes;
}

void Texture2D::Term(SRVManager *srv) {
#if _DEBUG
  if (!srv && srv_.IsValid()) {
    assert(false && "Texture2D SRV is still allocated. Call Term(&srvMgr) "
                    "before destruction.");
  }
#endif
  if (resource_) {
    resource_.Reset();
  }
  if (srv && srv_.IsValid()) {
    // ※GPU が参照し終わったタイミングで呼ぶのが前提（終了時なら問題ない）
    srv->Free(srv_);
  }
  srv_ = {};
  path_.clear();
  mipImages_ = DirectX::ScratchImage{};
  metadata_ = DirectX::TexMetadata{};
}
