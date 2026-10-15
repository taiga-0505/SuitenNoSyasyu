// ============================================================================
// RenderFont.cpp
// ----------------------------------------------------------------------------
// RC::名前空間の Font / 文字列描画 API 実装
//   LoadFont / UnloadFont / DrawString / MeasureString / GetFontLineHeight
// ============================================================================

#include "RenderCommon.h"
#include "RenderContext.h"

namespace RC {

int LoadFont(const std::string &path, float sizePx, uint32_t atlasSize) {
  auto &rc = GetRenderContext();
  if (!rc.IsInitialized()) {
    return -1;
  }
  return rc.Fonts().Load(path, sizePx, atlasSize);
}

void UnloadFont(int fontHandle) {
  auto &rc = GetRenderContext();
  if (!rc.IsInitialized()) {
    return;
  }
  rc.Fonts().Unload(fontHandle);
}

namespace {

/// @brief デコード済みの文字列を "font" パイプラインで描画する（DrawString の共通処理）
/// @param fontHandle  フォントハンドル
/// @param text        UTF-32 の文字列
/// @param pos         描画位置（スクリーン座標）
/// @param color       文字色
/// @param scale       拡大率
/// @param align       揃え方
/// @param lineSpacing 行間
/// @note 未初期化・コマンドリスト無し・無効なハンドル・空文字列・PSO が無いときは何もしない。
void DrawStringImpl_(int fontHandle, std::u32string_view text,
                     const Vector2 &pos, const Vector4 &color, float scale,
                     TextAlign align, float lineSpacing) {
  auto &ctx = GetRenderContext();
  if (!ctx.IsInitialized() || !ctx.CL()) {
    return;
  }
  if (!ctx.Fonts().IsValid(fontHandle) || text.empty()) {
    return;
  }

  // "font" PSO（Sprite 系ルートシグネチャ・頂点カラー付き・カリング無し）
  if (!ctx.BindPipeline("font")) {
    return;
  }

  ctx.AddCommandHistory("String", fontHandle);
  ctx.Fonts().DrawString(fontHandle, text, pos, color, scale, align,
                         lineSpacing, ctx.CL());
}

} // namespace

void DrawString(int fontHandle, const std::string &utf8, const Vector2 &pos,
                const Vector4 &color, float scale, TextAlign align,
                float lineSpacing) {
  DrawStringImpl_(fontHandle, FontManager::DecodeUtf8(utf8), pos, color,
                  scale, align, lineSpacing);
}

void DrawString(int fontHandle, const std::wstring &text, const Vector2 &pos,
                const Vector4 &color, float scale, TextAlign align,
                float lineSpacing) {
  DrawStringImpl_(fontHandle, FontManager::DecodeWide(text), pos, color,
                  scale, align, lineSpacing);
}

Vector2 MeasureString(int fontHandle, const std::string &utf8, float scale,
                      float lineSpacing) {
  auto &rc = GetRenderContext();
  if (!rc.IsInitialized()) {
    return {0.0f, 0.0f};
  }
  return rc.Fonts().Measure(fontHandle, FontManager::DecodeUtf8(utf8), scale,
                            lineSpacing);
}

Vector2 MeasureString(int fontHandle, const std::wstring &text, float scale,
                      float lineSpacing) {
  auto &rc = GetRenderContext();
  if (!rc.IsInitialized()) {
    return {0.0f, 0.0f};
  }
  return rc.Fonts().Measure(fontHandle, FontManager::DecodeWide(text), scale,
                            lineSpacing);
}

float GetFontLineHeight(int fontHandle, float scale) {
  auto &rc = GetRenderContext();
  if (!rc.IsInitialized()) {
    return 0.0f;
  }
  return rc.Fonts().LineHeight(fontHandle, scale);
}

} // namespace RC
