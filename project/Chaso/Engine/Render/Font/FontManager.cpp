#include "FontManager.h"
#include "DeferredReleaseQueue/DeferredReleaseQueue.h"
#include <memory>

#include <algorithm>
#include <cmath>
#include <format>

#include "Common/Log/Log.h"
#include "Math/Math.h"
#include "function/function.h"

namespace RC {

// ============================================================================
// Init / Term
// ============================================================================

void FontManager::Init(ID3D12Device *device, SRVManager *srv) {
  device_ = device;
  srv_ = srv;
  fonts_.clear();
  frameSlot_ = 0;
  frameBegun_ = false;
  warnedOverflow_ = false;
}

void FontManager::Term() {
  for (auto &s : fonts_) {
    if (s.atlas) {
      s.atlas->Term(srv_);
      s.atlas.reset();
    }
    s.inUse = false;
    s.refCount = 0;
  }
  fonts_.clear();

  for (auto &f : frames_) {
    if (f.vb && f.vbMapped) {
      f.vb->Unmap(0, nullptr);
    }
    f.vbMapped = nullptr;
    f.vb.Reset();
    if (f.cbWVP && f.cbWVPMapped) {
      f.cbWVP->Unmap(0, nullptr);
    }
    f.cbWVPMapped = nullptr;
    f.cbWVP.Reset();
    f.vertexCursor = 0;
  }
  if (cbMaterial_ && cbMaterialMapped_) {
    cbMaterial_->Unmap(0, nullptr);
  }
  cbMaterialMapped_ = nullptr;
  cbMaterial_.Reset();

  device_ = nullptr;
  srv_ = nullptr;
  frameBegun_ = false;
}

bool FontManager::EnsureFrameBuffers_() {
  if (!device_) {
    return false;
  }
  if (cbMaterial_) {
    return true;
  }

  // 固定マテリアル（白・UV identity）。色は頂点カラーで与える
  cbMaterial_ = CreateBufferResource(device_, sizeof(SpriteMaterial),
                                     L"FontManager::cbMaterial_");
  if (!cbMaterial_ ||
      FAILED(cbMaterial_->Map(0, nullptr,
                              reinterpret_cast<void **>(&cbMaterialMapped_)))) {
    Log::Print("[Font] マテリアル CB の生成に失敗しました");
    cbMaterial_.Reset();
    cbMaterialMapped_ = nullptr;
    return false;
  }
  cbMaterialMapped_->color = {1.0f, 1.0f, 1.0f, 1.0f};
  cbMaterialMapped_->uvTransform = MakeIdentity4x4();

  const size_t vbBytes = sizeof(FontVertex) * 6ull * kMaxGlyphsPerFrame;
  for (uint32_t i = 0; i < kFrameCount; ++i) {
    auto &f = frames_[i];
    f.vb = CreateBufferResource(device_, vbBytes, L"FontManager::vb");
    if (!f.vb ||
        FAILED(f.vb->Map(0, nullptr, reinterpret_cast<void **>(&f.vbMapped)))) {
      Log::Print("[Font] 頂点リングバッファの生成に失敗しました");
      f.vb.Reset();
      f.vbMapped = nullptr;
      return false;
    }
    f.vertexCursor = 0;

    f.cbWVP = CreateBufferResource(device_, sizeof(TransformationMatrix),
                                   L"FontManager::cbWVP");
    if (!f.cbWVP ||
        FAILED(f.cbWVP->Map(0, nullptr,
                            reinterpret_cast<void **>(&f.cbWVPMapped)))) {
      Log::Print("[Font] WVP CB の生成に失敗しました");
      f.cbWVP.Reset();
      f.cbWVPMapped = nullptr;
      return false;
    }
    f.cbWVPMapped->World = MakeIdentity4x4();
    f.cbWVPMapped->worldInverseTranspose = MakeIdentity4x4();
    // フレーム途中で生成された場合に備え、既知の画面サイズで正射影を入れておく
    f.cbWVPMapped->WVP =
        (screenW_ > 0.0f && screenH_ > 0.0f)
            ? MakeOrthographicMatrix(0.0f, 0.0f, screenW_, screenH_, 0.0f,
                                     100.0f)
            : MakeIdentity4x4();
  }
  return true;
}

void FontManager::BeginFrame(float screenW, float screenH) {
  screenW_ = screenW;
  screenH_ = screenH;
  frameBegun_ = true;
  warnedOverflow_ = false;

  // GPU バッファはフォントを 1 つでもロードしたときに作る（未使用なら確保しない）
  if (!cbMaterial_) {
    return;
  }
  frameSlot_ = (frameSlot_ + 1) % kFrameCount;
  auto &f = frames_[frameSlot_];
  f.vertexCursor = 0;

  // 左上原点のピクセル座標 → クリップ空間
  if (f.cbWVPMapped) {
    f.cbWVPMapped->WVP =
        MakeOrthographicMatrix(0.0f, 0.0f, screenW_, screenH_, 0.0f, 100.0f);
    f.cbWVPMapped->World = MakeIdentity4x4();
  }
}

// ============================================================================
// Load / Unload
// ============================================================================

int FontManager::Load(const std::string &path, float sizePx,
                      uint32_t atlasSize) {
  if (!device_ || !srv_) {
    Log::Print("[Font] FontManager が初期化されていません");
    return -1;
  }

  // 同一 path × size は共有（使用中のもの、または未使用でキャッシュに残っているもの）
  for (size_t i = 0; i < fonts_.size(); ++i) {
    auto &s = fonts_[i];
    if (!s.atlas || s.path != path || s.sizePx != sizePx) {
      continue;
    }
    if (s.atlas->AtlasSize() < atlasSize) {
      if (!s.inUse) {
        // キャッシュのアトラスが小さい → 誰も使っていないので作り直す
        DestroySlot_(s);
        break;
      }
      Log::Print(std::format(
          "[Font] 既存ハンドル ({}px, atlas {}) を共有します。atlasSize={} は無視されます: {}",
          sizePx, s.atlas->AtlasSize(), atlasSize, Log::NormalizePath(path)));
    }
    ++s.refCount;
    s.inUse = true;
    return static_cast<int>(i);
  }

  // 描画用 GPU バッファ（頂点リング・CB）は初回ロード時に確保
  if (!EnsureFrameBuffers_()) {
    return -1;
  }

  auto atlas = std::make_unique<FontAtlas>();
  if (!atlas->Initialize(srv_, path, sizePx, atlasSize)) {
    Log::Print(std::format("[Font] 読み込み失敗: {} ({}px)",
                           Log::NormalizePath(path), sizePx));
    return -1;
  }

  // 空きスロット（アトラスを持たないもの）を再利用
  int handle = -1;
  for (size_t i = 0; i < fonts_.size(); ++i) {
    if (!fonts_[i].inUse && !fonts_[i].atlas) {
      handle = static_cast<int>(i);
      break;
    }
  }
  if (handle < 0) {
    fonts_.emplace_back();
    handle = static_cast<int>(fonts_.size() - 1);
  }

  auto &s = fonts_[handle];
  s.atlas = std::move(atlas);
  s.path = path;
  s.sizePx = sizePx;
  s.refCount = 1;
  s.inUse = true;

  return handle; // 詳細ログは FontAtlas::Initialize 側で出力済み
}

void FontManager::Unload(int handle) {
  if (!IsValid(handle)) {
    return;
  }
  auto &s = fonts_[handle];
  if (--s.refCount > 0) {
    return;
  }
  // すぐには破棄せずキャッシュへ（次の同じ Load で再利用する）
  s.refCount = 0;
  s.inUse = false;
  s.lastUsed = ++unloadCounter_;
  TrimCache_();
}

void FontManager::PurgeUnused() {
  for (auto &s : fonts_) {
    if (!s.inUse && s.atlas) {
      DestroySlot_(s);
    }
  }
}

void FontManager::DestroySlot_(Slot &s) {
  if (s.atlas) {
    Log::Print(std::format("[Font] 破棄完了: {} ({}px)",
                           Log::NormalizePath(s.path), s.sizePx));
    // アトラスのテクスチャと SRV は、直前のフレームの GPU が使い終わってから解放する
    std::shared_ptr<FontAtlas> atlas(std::move(s.atlas));
    SRVManager *srv = srv_;
    DeferredReleaseQueue::DeferCall([atlas, srv]() { atlas->Term(srv); });
  }
  s.path.clear();
  s.sizePx = 0.0f;
  s.refCount = 0;
  s.inUse = false;
  s.lastUsed = 0;
}

void FontManager::TrimCache_() {
  for (;;) {
    size_t cached = 0;
    Slot *oldest = nullptr;
    for (auto &s : fonts_) {
      if (s.inUse || !s.atlas) continue;
      ++cached;
      if (!oldest || s.lastUsed < oldest->lastUsed) oldest = &s;
    }
    if (cached <= kMaxCachedFonts || !oldest) return;
    DestroySlot_(*oldest);
  }
}

bool FontManager::IsValid(int handle) const {
  return handle >= 0 && handle < static_cast<int>(fonts_.size()) &&
         fonts_[handle].inUse && fonts_[handle].atlas &&
         fonts_[handle].atlas->IsLoaded();
}

FontAtlas *FontManager::Get(int handle) {
  return IsValid(handle) ? fonts_[handle].atlas.get() : nullptr;
}

const FontAtlas *FontManager::Get(int handle) const {
  return IsValid(handle) ? fonts_[handle].atlas.get() : nullptr;
}

// ============================================================================
// 計測
// ============================================================================

float FontManager::MeasureLine_(FontAtlas &atlas, std::u32string_view line,
                                float scale) {
  float w = 0.0f;
  for (char32_t cp : line) {
    if (cp == U'\r') {
      continue;
    }
    if (const GlyphInfo *g = atlas.GetGlyph(static_cast<uint32_t>(cp))) {
      w += g->advance * scale;
    }
  }
  return w;
}

Vector2 FontManager::Measure(int handle, std::u32string_view text, float scale,
                             float lineSpacing) {
  FontAtlas *atlas = Get(handle);
  if (!atlas) {
    return {0.0f, 0.0f};
  }
  float maxW = 0.0f;
  int lines = 0;
  size_t begin = 0;
  while (begin <= text.size()) {
    size_t end = text.find(U'\n', begin);
    if (end == std::u32string_view::npos) {
      end = text.size();
    }
    maxW = (std::max)(maxW, MeasureLine_(*atlas, text.substr(begin, end - begin),
                                       scale));
    ++lines;
    if (end == text.size()) {
      break;
    }
    begin = end + 1;
  }
  return {maxW, atlas->LineHeight() * scale * lineSpacing * lines};
}

float FontManager::LineHeight(int handle, float scale) const {
  const FontAtlas *atlas = Get(handle);
  return atlas ? atlas->LineHeight() * scale : 0.0f;
}

// ============================================================================
// 描画
// ============================================================================

void FontManager::DrawString(int handle, std::u32string_view text,
                             const Vector2 &pos, const Vector4 &color,
                             float scale, TextAlign align, float lineSpacing,
                             ID3D12GraphicsCommandList *cl) {
  FontAtlas *atlas = Get(handle);
  if (!atlas || !cl || text.empty() || scale <= 0.0f) {
    return;
  }
  if (!frameBegun_) {
    Log::Print("[Font] BeginFrame が呼ばれていません（PreDraw2D 後に描画してください）");
    return;
  }

  auto &frame = frames_[frameSlot_];
  if (!frame.vbMapped || !frame.cbWVP || !cbMaterial_ || screenW_ <= 0.0f) {
    return;
  }

  const bool snap = std::fabs(scale - 1.0f) < 1e-4f; // 等倍ならピクセルに吸着
  const float lineAdvance = atlas->LineHeight() * scale * lineSpacing;
  const uint32_t firstVertex = frame.vertexCursor;
  uint32_t glyphsWritten = 0;
  bool overflow = false;

  float baseline = pos.y + atlas->Ascent() * scale;

  size_t begin = 0;
  while (begin <= text.size() && !overflow) {
    size_t end = text.find(U'\n', begin);
    if (end == std::u32string_view::npos) {
      end = text.size();
    }
    const std::u32string_view line = text.substr(begin, end - begin);

    float penX = pos.x;
    if (align != TextAlign::Left) {
      const float w = MeasureLine_(*atlas, line, scale);
      penX -= (align == TextAlign::Center) ? w * 0.5f : w;
    }

    for (char32_t cp : line) {
      if (cp == U'\r') {
        continue;
      }
      const GlyphInfo *g = atlas->GetGlyph(static_cast<uint32_t>(cp));
      if (!g) {
        continue; // フォントに無い・アトラス満杯
      }
      if (g->hasBitmap) {
        if (frame.vertexCursor + 6 > kMaxGlyphsPerFrame * 6) {
          overflow = true;
          break;
        }
        float x0 = penX + g->offsetX * scale;
        float y0 = baseline + g->offsetY * scale;
        if (snap) {
          x0 = std::floor(x0 + 0.5f);
          y0 = std::floor(y0 + 0.5f);
        }
        const float x1 = x0 + g->width * scale;
        const float y1 = y0 + g->height * scale;

        FontVertex *v = frame.vbMapped + frame.vertexCursor;
        // 反時計/時計回りはカリング NONE なので不問。2 三角形で矩形
        v[0] = {{x0, y0, 0.0f, 1.0f}, {g->u0, g->v0}, color};
        v[1] = {{x1, y0, 0.0f, 1.0f}, {g->u1, g->v0}, color};
        v[2] = {{x0, y1, 0.0f, 1.0f}, {g->u0, g->v1}, color};
        v[3] = {{x1, y0, 0.0f, 1.0f}, {g->u1, g->v0}, color};
        v[4] = {{x1, y1, 0.0f, 1.0f}, {g->u1, g->v1}, color};
        v[5] = {{x0, y1, 0.0f, 1.0f}, {g->u0, g->v1}, color};
        frame.vertexCursor += 6;
        ++glyphsWritten;
      }
      penX += g->advance * scale;
    }

    baseline += lineAdvance;
    if (end == text.size()) {
      break;
    }
    begin = end + 1;
  }

  if (overflow && !warnedOverflow_) {
    warnedOverflow_ = true;
    Log::Print(std::format(
        "[Font] 1 フレームの最大グリフ数 ({}) を超えました。以降の文字は描かれません",
        kMaxGlyphsPerFrame));
  }
  if (glyphsWritten == 0) {
    return;
  }

  // 新規グリフがあれば GPU アトラスへ転送（Draw より前にコマンドを記録）
  atlas->Flush(cl, frameSlot_);

  D3D12_VERTEX_BUFFER_VIEW vbv{};
  vbv.BufferLocation = frame.vb->GetGPUVirtualAddress();
  vbv.SizeInBytes = static_cast<UINT>(sizeof(FontVertex) * 6ull * kMaxGlyphsPerFrame);
  vbv.StrideInBytes = sizeof(FontVertex);

  cl->IASetVertexBuffers(0, 1, &vbv);
  cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

  // RootParam (RootSignatureType::Sprite): 0=Material(PS b0), 1=WVP(VS b0), 2=SRV(t0)
  cl->SetGraphicsRootConstantBufferView(0, cbMaterial_->GetGPUVirtualAddress());
  cl->SetGraphicsRootConstantBufferView(1, frame.cbWVP->GetGPUVirtualAddress());
  cl->SetGraphicsRootDescriptorTable(2, atlas->Srv());

  cl->DrawInstanced(glyphsWritten * 6, 1, firstVertex, 0);
}

// ============================================================================
// 文字コード変換
// ============================================================================

std::u32string FontManager::DecodeUtf8(std::string_view s) {
  std::u32string out;
  out.reserve(s.size());
  size_t i = 0;
  while (i < s.size()) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    uint32_t cp = 0;
    size_t len = 0;
    if (c < 0x80) {
      cp = c;
      len = 1;
    } else if ((c & 0xE0) == 0xC0) {
      cp = c & 0x1F;
      len = 2;
    } else if ((c & 0xF0) == 0xE0) {
      cp = c & 0x0F;
      len = 3;
    } else if ((c & 0xF8) == 0xF0) {
      cp = c & 0x07;
      len = 4;
    } else {
      // 不正なリード バイト → U+FFFD
      out.push_back(0xFFFD);
      ++i;
      continue;
    }
    if (i + len > s.size()) {
      out.push_back(0xFFFD);
      break;
    }
    bool valid = true;
    for (size_t k = 1; k < len; ++k) {
      const unsigned char cc = static_cast<unsigned char>(s[i + k]);
      if ((cc & 0xC0) != 0x80) {
        valid = false;
        break;
      }
      cp = (cp << 6) | (cc & 0x3F);
    }
    if (!valid) {
      out.push_back(0xFFFD);
      ++i;
      continue;
    }
    out.push_back(static_cast<char32_t>(cp));
    i += len;
  }
  return out;
}

std::u32string FontManager::DecodeWide(std::wstring_view w) {
  std::u32string out;
  out.reserve(w.size());
  for (size_t i = 0; i < w.size(); ++i) {
    const uint32_t c = static_cast<uint16_t>(w[i]);
    if (c >= 0xD800 && c <= 0xDBFF && i + 1 < w.size()) {
      const uint32_t lo = static_cast<uint16_t>(w[i + 1]);
      if (lo >= 0xDC00 && lo <= 0xDFFF) {
        out.push_back(static_cast<char32_t>(
            0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00)));
        ++i;
        continue;
      }
    }
    out.push_back(static_cast<char32_t>(c));
  }
  return out;
}

} // namespace RC
