// ============================================================================
// FontAtlas.cpp
// ----------------------------------------------------------------------------
// DirectWrite によるグリフラスタライズ + 動的テクスチャアトラス
//   - ImGui / D2D / D3D11On12 非依存（dwrite.dll のみ）
//   - TTF / OTF(CFF) / TTC 対応
// ============================================================================

#include "FontAtlas.h"
#include "Common/ResourcePath.h"

#include <Windows.h>
#include <algorithm>
#include <cassert>
#include <cstring>
#include <format>

#include "Common/Log/Log.h"

#pragma comment(lib, "dwrite.lib")

namespace {

/// @brief UTF-8 文字列を UTF-16 に変換する（フォントパス用）
/// @param s 変換元（UTF-8。不正な場合は ANSI として解釈し直す）
/// @return 変換後のワイド文字列
std::wstring Utf8ToWide_(const std::string &s) {
  if (s.empty()) {
    return {};
  }
  const int len = MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                      static_cast<int>(s.size()), nullptr, 0);
  if (len <= 0) {
    // UTF-8 として不正なら ANSI(CP_ACP) として再解釈
    const int len2 = MultiByteToWideChar(CP_ACP, 0, s.data(),
                                         static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>((std::max)(len2, 0)), L'\0');
    if (len2 > 0) {
      MultiByteToWideChar(CP_ACP, 0, s.data(), static_cast<int>(s.size()),
                          w.data(), len2);
    }
    return w;
  }
  std::wstring w(static_cast<size_t>(len), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                      w.data(), len);
  return w;
}

/// @brief v を align の倍数へ切り上げる
/// @param v     元の値
/// @param align 揃える単位（0 不可）
/// @return 切り上げた値
constexpr uint32_t RoundUp_(uint32_t v, uint32_t align) {
  return (v + align - 1) / align * align;
}

} // namespace

// ============================================================================
// ライフサイクル
// ============================================================================

FontAtlas::~FontAtlas() {
#if _DEBUG
  // SRV が残っているのは運用ミス（Term(&srv) を呼ぶ）
  assert(!srv_.IsValid() && "FontAtlas::Term(&srv) を呼んでから破棄してください");
#endif
  Term(nullptr);
}

bool FontAtlas::Initialize(SRVManager *srv, const std::string &path,
                           float sizePx, uint32_t atlasSize,
                           uint32_t faceIndex) {
  Term(srv);

  if (!srv || !srv->Device()) {
    Log::Print("[FontAtlas] SRVManager が無効です");
    return false;
  }
  if (sizePx <= 0.0f) {
    Log::Print(std::format("[FontAtlas] 不正なフォントサイズ: {}", sizePx));
    return false;
  }

  device_ = srv->Device();
  path_ = path;
  sizePx_ = sizePx;

  // 512 の倍数へ丸め（RowPitch 256 / 配置オフセット 512 アライメントを満たす）
  atlasSize_ = (std::max)(kMinAtlasSize, RoundUp_(atlasSize, kMinAtlasSize));

  if (!CreateFontFace_(path, faceIndex)) {
    Term(srv);
    return false;
  }
  if (!CreateTexture_(srv)) {
    Term(srv);
    return false;
  }

  // CPU アトラス（0 初期化）。初回 Flush で全面転送して GPU 側も 0 にする
  cpuAtlas_.assign(static_cast<size_t>(atlasSize_) * atlasSize_, 0);
  shelfX_ = shelfY_ = shelfH_ = 0;
  atlasFull_ = warnedFull_ = false;
  glyphs_.clear();
  MarkDirty_(0, atlasSize_ - 1);

  Log::Print(std::format("[FontAtlas] 読み込み完了: {} ({}px, atlas {}x{})",
                         Log::NormalizePath(path_), sizePx_, atlasSize_,
                         atlasSize_));
  return true;
}

void FontAtlas::Term(SRVManager *srv) {
  for (auto &s : staging_) {
    if (s.res && s.mapped) {
      s.res->Unmap(0, nullptr);
    }
    s.mapped = nullptr;
    s.res.Reset();
  }
  if (srv && srv_.IsValid()) {
    srv->Free(srv_);
  }
  srv_ = {};
  texture_.Reset();

  fontFace_.Reset();
  factory2_.Reset();
  factory_.Reset();

  cpuAtlas_.clear();
  cpuAtlas_.shrink_to_fit();
  glyphs_.clear();
  dirtyMinY_ = UINT32_MAX;
  dirtyMaxY_ = 0;
  device_ = nullptr;
}

// ============================================================================
// 初期化ヘルパー
// ============================================================================

bool FontAtlas::CreateFontFace_(const std::string &path, uint32_t faceIndex) {
  HRESULT hr = DWriteCreateFactory(
      DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
      reinterpret_cast<IUnknown **>(factory_.ReleaseAndGetAddressOf()));
  if (FAILED(hr)) {
    Log::Print("[FontAtlas] DWriteCreateFactory に失敗しました");
    return false;
  }
  // グレースケール AA を使うために IDWriteFactory2 (Win8.1+) を試す。無くても動く
  factory_.As(&factory2_);

  const std::wstring wpath = Utf8ToWide_(Chaso::ResolvePath(path)); // ゲーム側 → エンジン側

  Microsoft::WRL::ComPtr<IDWriteFontFile> file;
  hr = factory_->CreateFontFileReference(wpath.c_str(), nullptr, &file);
  if (FAILED(hr)) {
    Log::Print("[FontAtlas] フォントファイルが開けません: " +
               Log::NormalizePath(path));
    return false;
  }

  BOOL isSupported = FALSE;
  DWRITE_FONT_FILE_TYPE fileType = DWRITE_FONT_FILE_TYPE_UNKNOWN;
  DWRITE_FONT_FACE_TYPE faceType = DWRITE_FONT_FACE_TYPE_UNKNOWN;
  UINT32 numFaces = 0;
  hr = file->Analyze(&isSupported, &fileType, &faceType, &numFaces);
  if (FAILED(hr) || !isSupported || numFaces == 0) {
    Log::Print("[FontAtlas] 対応していないフォント形式です: " +
               Log::NormalizePath(path));
    return false;
  }
  if (faceIndex >= numFaces) {
    Log::Print(std::format("[FontAtlas] faceIndex {} は範囲外です（フェイス数 {}）",
                           faceIndex, numFaces));
    faceIndex = 0;
  }

  IDWriteFontFile *files[] = {file.Get()};
  hr = factory_->CreateFontFace(faceType, 1, files, faceIndex,
                                DWRITE_FONT_SIMULATIONS_NONE, &fontFace_);
  if (FAILED(hr) || !fontFace_) {
    Log::Print("[FontAtlas] CreateFontFace に失敗しました: " +
               Log::NormalizePath(path));
    return false;
  }

  fontFace_->GetMetrics(&fontMetrics_);
  if (fontMetrics_.designUnitsPerEm == 0) {
    Log::Print("[FontAtlas] designUnitsPerEm が 0 です");
    return false;
  }
  designScale_ = sizePx_ / static_cast<float>(fontMetrics_.designUnitsPerEm);
  ascent_ = fontMetrics_.ascent * designScale_;
  descent_ = fontMetrics_.descent * designScale_;
  lineHeight_ =
      (fontMetrics_.ascent + fontMetrics_.descent + fontMetrics_.lineGap) *
      designScale_;
  return true;
}

bool FontAtlas::CreateTexture_(SRVManager *srv) {
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = atlasSize_;
  desc.Height = atlasSize_;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = DXGI_FORMAT_R8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  desc.Flags = D3D12_RESOURCE_FLAG_NONE;

  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;

  // 常時 PIXEL_SHADER_RESOURCE で保持し、Flush の間だけ COPY_DEST へ遷移させる
  HRESULT hr = device_->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &desc,
      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
      IID_PPV_ARGS(&texture_));
  if (FAILED(hr) || !texture_) {
    Log::Print("[FontAtlas] アトラステクスチャの生成に失敗しました");
    return false;
  }
  texture_->SetName((L"FontAtlas: " + Utf8ToWide_(path_)).c_str());

  srv_ = srv->CreateTexture2D(texture_.Get(), DXGI_FORMAT_R8_UNORM, 1);
  if (!srv_.IsValid()) {
    Log::Print("[FontAtlas] SRV の生成に失敗しました");
    return false;
  }
  return true;
}

// ============================================================================
// グリフ登録
// ============================================================================

const GlyphInfo *FontAtlas::GetGlyph(uint32_t codepoint) {
  if (!IsLoaded()) {
    return nullptr;
  }
  auto it = glyphs_.find(codepoint);
  if (it != glyphs_.end()) {
    return &it->second;
  }

  GlyphInfo info{};
  if (!Rasterize_(codepoint, info)) {
    return nullptr;
  }
  return &glyphs_.emplace(codepoint, info).first->second;
}

bool FontAtlas::AllocRect_(uint32_t w, uint32_t h, uint32_t &outX,
                           uint32_t &outY) {
  if (w > atlasSize_ || h > atlasSize_) {
    return false;
  }
  // 現在のシェルフに収まらなければ次のシェルフへ
  if (shelfX_ + w > atlasSize_) {
    shelfY_ += shelfH_;
    shelfX_ = 0;
    shelfH_ = 0;
  }
  if (shelfY_ + h > atlasSize_) {
    return false; // 満杯
  }
  outX = shelfX_;
  outY = shelfY_;
  shelfX_ += w;
  shelfH_ = (std::max)(shelfH_, h);
  return true;
}

void FontAtlas::MarkDirty_(uint32_t y0, uint32_t y1) {
  dirtyMinY_ = (std::min)(dirtyMinY_, y0);
  dirtyMaxY_ = (std::max)(dirtyMaxY_, y1);
}

bool FontAtlas::Rasterize_(uint32_t codepoint, GlyphInfo &out) {
  // --- コードポイント → グリフインデックス ---
  UINT32 cp = codepoint;
  UINT16 glyphIndex = 0;
  HRESULT hr = fontFace_->GetGlyphIndices(&cp, 1, &glyphIndex);
  if (FAILED(hr)) {
    return false;
  }
  // glyphIndex == 0 は .notdef（豆腐）。そのまま描いて「無い文字」を可視化する

  // --- 送り幅 ---
  DWRITE_GLYPH_METRICS gm{};
  hr = fontFace_->GetDesignGlyphMetrics(&glyphIndex, 1, &gm, FALSE);
  if (FAILED(hr)) {
    return false;
  }
  out.advance = static_cast<float>(gm.advanceWidth) * designScale_;

  // --- ラスタライズ（ベースライン原点 (0,0)）---
  DWRITE_GLYPH_RUN run{};
  run.fontFace = fontFace_.Get();
  run.fontEmSize = sizePx_;
  run.glyphCount = 1;
  run.glyphIndices = &glyphIndex;
  run.glyphAdvances = nullptr;
  run.glyphOffsets = nullptr;
  run.isSideways = FALSE;
  run.bidiLevel = 0;

  Microsoft::WRL::ComPtr<IDWriteGlyphRunAnalysis> analysis;
  if (factory2_) {
    hr = factory2_->CreateGlyphRunAnalysis(
        &run, nullptr, DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC,
        DWRITE_MEASURING_MODE_NATURAL, DWRITE_GRID_FIT_MODE_DEFAULT,
        DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE, 0.0f, 0.0f, &analysis);
  } else {
    hr = factory_->CreateGlyphRunAnalysis(
        &run, 1.0f, nullptr, DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC,
        DWRITE_MEASURING_MODE_NATURAL, 0.0f, 0.0f, &analysis);
  }
  if (FAILED(hr) || !analysis) {
    Log::Print(std::format("[FontAtlas] CreateGlyphRunAnalysis 失敗 (U+{:04X})",
                           codepoint));
    return false;
  }

  // テクスチャ形式:
  //   ALIASED_1x1   = 1px あたり 1 バイト。名前は "ALIASED" だが、IDWriteFactory2 の
  //                   DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE ではこの形式で 8bit グレースケールが返る
  //   CLEARTYPE_3x1 = 1px あたり RGB 3 バイト（IDWriteFactory (v1) の ClearType 出力）
  // グレースケール指定に対して 3x1 を要求すると「該当なし」で空矩形になるため、
  // まず適切な形式を試し、空なら念のためもう一方も試す（環境差の吸収）。
  DWRITE_TEXTURE_TYPE texType =
      factory2_ ? DWRITE_TEXTURE_ALIASED_1x1 : DWRITE_TEXTURE_CLEARTYPE_3x1;
  RECT bounds{};
  hr = analysis->GetAlphaTextureBounds(texType, &bounds);
  bool empty = FAILED(hr) || (bounds.right <= bounds.left) ||
               (bounds.bottom <= bounds.top);
  if (empty) {
    const DWRITE_TEXTURE_TYPE alt = (texType == DWRITE_TEXTURE_ALIASED_1x1)
                                        ? DWRITE_TEXTURE_CLEARTYPE_3x1
                                        : DWRITE_TEXTURE_ALIASED_1x1;
    RECT altBounds{};
    const HRESULT hr2 = analysis->GetAlphaTextureBounds(alt, &altBounds);
    if (SUCCEEDED(hr2) && altBounds.right > altBounds.left &&
        altBounds.bottom > altBounds.top) {
      texType = alt;
      bounds = altBounds;
      hr = hr2;
      empty = false;
    }
  }
  if (FAILED(hr)) {
    Log::Print(std::format(
        "[FontAtlas] GetAlphaTextureBounds 失敗 (U+{:04X}, hr=0x{:08X})",
        codepoint, static_cast<uint32_t>(hr)));
    return false;
  }
  if (empty) {
    // 空白など、絵の無いグリフ
    out.hasBitmap = false;
    return true;
  }
  const uint32_t bpp = (texType == DWRITE_TEXTURE_ALIASED_1x1) ? 1u : 3u;
  const int bw = bounds.right - bounds.left;
  const int bh = bounds.bottom - bounds.top;

  std::vector<BYTE> pixels(static_cast<size_t>(bw) * bh * bpp);
  hr = analysis->CreateAlphaTexture(texType, &bounds, pixels.data(),
                                    static_cast<UINT32>(pixels.size()));
  if (FAILED(hr)) {
    Log::Print(std::format(
        "[FontAtlas] CreateAlphaTexture 失敗 (U+{:04X}, hr=0x{:08X})",
        codepoint, static_cast<uint32_t>(hr)));
    return false;
  }

  if (glyphs_.empty()) {
    // 最初のグリフだけ診断ログ（形式・サイズの確認用）
    Log::Print(std::format(
        "[FontAtlas] 初回グリフ U+{:04X}: {}x{} px, texture={}, advance={:.1f}",
        codepoint, bw, bh,
        (texType == DWRITE_TEXTURE_ALIASED_1x1) ? "1x1(gray)" : "3x1(cleartype)",
        out.advance));
  }

  // --- アトラスへ配置 ---
  const uint32_t w = static_cast<uint32_t>(bw);
  const uint32_t h = static_cast<uint32_t>(bh);
  uint32_t x = 0, y = 0;
  if (!AllocRect_(w + kPadding * 2, h + kPadding * 2, x, y)) {
    atlasFull_ = true;
    if (!warnedFull_) {
      warnedFull_ = true;
      Log::Print(std::format(
          "[FontAtlas] アトラスが満杯です ({}x{}, {} glyphs): {} "
          "— LoadFont の atlasSize を増やしてください",
          atlasSize_, atlasSize_, glyphs_.size(), Log::NormalizePath(path_)));
    }
    return false;
  }
  const uint32_t px = x + kPadding;
  const uint32_t py = y + kPadding;

  for (uint32_t row = 0; row < h; ++row) {
    uint8_t *dst = &cpuAtlas_[static_cast<size_t>(py + row) * atlasSize_ + px];
    const BYTE *src = &pixels[static_cast<size_t>(row) * w * bpp];
    if (bpp == 1) {
      memcpy(dst, src, w);
    } else {
      for (uint32_t col = 0; col < w; ++col) {
        // RGB の平均（ClearType フォールバック時も自然なグレーになる）
        const uint32_t sum =
            src[col * 3 + 0] + src[col * 3 + 1] + src[col * 3 + 2];
        dst[col] = static_cast<uint8_t>(sum / 3);
      }
    }
  }
  // パディング行も含めて汚れ帯へ（周囲 0 を確実に GPU へ送る）
  MarkDirty_(y, (std::min)(y + h + kPadding * 2 - 1, atlasSize_ - 1));

  const float inv = 1.0f / static_cast<float>(atlasSize_);
  out.u0 = static_cast<float>(px) * inv;
  out.v0 = static_cast<float>(py) * inv;
  out.u1 = static_cast<float>(px + w) * inv;
  out.v1 = static_cast<float>(py + h) * inv;
  out.offsetX = static_cast<float>(bounds.left);
  out.offsetY = static_cast<float>(bounds.top);
  out.width = static_cast<float>(w);
  out.height = static_cast<float>(h);
  out.hasBitmap = true;
  return true;
}

// ============================================================================
// GPU 転送
// ============================================================================

void FontAtlas::Flush(ID3D12GraphicsCommandList *cl, uint32_t frameIndex) {
  if (!cl || !IsLoaded() || !IsDirty()) {
    return;
  }

  const uint32_t rowPitch = atlasSize_; // R8 なので 1 byte/px。512 の倍数
  const uint32_t y0 = dirtyMinY_;
  const uint32_t rows = dirtyMaxY_ - dirtyMinY_ + 1;

  // --- ステージング（フレームごとにリング）---
  Staging &st = staging_[frameIndex % kStagingCount];
  if (!st.res) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = static_cast<UINT64>(rowPitch) * atlasSize_;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    HRESULT hr = device_->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&st.res));
    if (FAILED(hr) || !st.res) {
      Log::Print("[FontAtlas] ステージングバッファの生成に失敗しました");
      return;
    }
    st.res->SetName(L"FontAtlas::Staging");
    hr = st.res->Map(0, nullptr, reinterpret_cast<void **>(&st.mapped));
    if (FAILED(hr) || !st.mapped) {
      st.res.Reset();
      st.mapped = nullptr;
      Log::Print("[FontAtlas] ステージングバッファの Map に失敗しました");
      return;
    }
  }

  // 汚れ帯を CPU アトラスと同じオフセットへコピー
  // （同一フレーム内で複数回 Flush しても、GPU が読む時点の内容は常に最新＝上位互換）
  const size_t offset = static_cast<size_t>(y0) * rowPitch;
  const size_t bytes = static_cast<size_t>(rows) * rowPitch;
  memcpy(st.mapped + offset, cpuAtlas_.data() + offset, bytes);

  // --- PSR → COPY_DEST ---
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = texture_.Get();
  barrier.Transition.Subresource = 0;
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
  cl->ResourceBarrier(1, &barrier);

  // --- 帯コピー ---
  D3D12_TEXTURE_COPY_LOCATION dst{};
  dst.pResource = texture_.Get();
  dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  dst.SubresourceIndex = 0;

  D3D12_TEXTURE_COPY_LOCATION src{};
  src.pResource = st.res.Get();
  src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  src.PlacedFootprint.Offset = offset; // 512 の倍数（rowPitch が 512 の倍数）
  src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8_UNORM;
  src.PlacedFootprint.Footprint.Width = atlasSize_;
  src.PlacedFootprint.Footprint.Height = rows;
  src.PlacedFootprint.Footprint.Depth = 1;
  src.PlacedFootprint.Footprint.RowPitch = rowPitch;

  cl->CopyTextureRegion(&dst, 0, y0, 0, &src, nullptr);

  // --- COPY_DEST → PSR ---
  std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
  cl->ResourceBarrier(1, &barrier);

  dirtyMinY_ = UINT32_MAX;
  dirtyMaxY_ = 0;
}
