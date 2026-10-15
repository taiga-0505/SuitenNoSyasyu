#pragma once

// FontManager
// -----------------------------------------------------------------------------
// 「フォント周り」の管理クラス
// - フォントはハンドル制（int）。1 ハンドル = 1 フォントファイル × 1 ピクセルサイズ
// - FontAtlas（DirectWrite ラスタライズ + 動的アトラス）の生成/破棄をここに集約
// - 文字列 → 頂点列の生成と描画もここで行う
//   * 頂点はフレームごとのリングバッファへ書き込むため、同じハンドルで
//     1 フレームに何度でも（位置・色を変えて）描ける（Sprite の 1 回制限なし）
// -----------------------------------------------------------------------------

#include <cstdint>
#include <d3d12.h>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <wrl/client.h>

#include "Font/FontAtlas.h"
#include "Math/Math.h"
#include "struct.h"

class SRVManager;

namespace RC {

/// @class FontManager
/// @brief フォントリソースと文字列描画を管理するマネージャクラス
class FontManager {
public:
  /// @brief フレームごとの頂点リングバッファ数（トリプルバッファ）
  static constexpr uint32_t kFrameCount = 3;
  /// @brief 1 フレームに描画できる最大グリフ数（超過分は描かれず警告ログ）
  static constexpr uint32_t kMaxGlyphsPerFrame = 8192;
  /// @brief 未使用のまま保持しておくフォントの上限（超えたら古いものから破棄）
  static constexpr size_t kMaxCachedFonts = 16;

  /// @brief デバイスと SRV 管理クラスを受け取って初期化する
  /// @param device D3D12 デバイス
  /// @param srv    SRV 管理クラス（アトラス SRV の確保に使う）
  /// @note GPU バッファはここでは作らず、最初のフォントを Load したときに作る。
  void Init(ID3D12Device *device, SRVManager *srv);

  /// @brief 終了処理。全フォントと GPU バッファを解放する
  void Term();

  /// @brief フレーム開始（PreDraw2D から毎フレーム 1 回呼ぶ）
  /// @param screenW 画面幅（ピクセル）
  /// @param screenH 画面高さ（ピクセル）
  void BeginFrame(float screenW, float screenH);

  /// @brief フォントをロードしてハンドルを返す
  /// @param path      フォントファイル（.ttf / .otf / .ttc）のパス
  /// @param sizePx    フォントサイズ（ピクセル）
  /// @param atlasSize アトラスの一辺（512 の倍数。日本語なら 1024〜2048 推奨）
  /// @return フォントハンドル（失敗時は -1）
  /// @note 同じ path × sizePx を再度 Load すると同じハンドルを返す（参照カウント）。
  int Load(const std::string &path, float sizePx, uint32_t atlasSize = 1024);

  /// @brief フォントを解放する
  /// @param handle フォントハンドル（呼び出し後は無効になる）
  /// @note 参照カウントが 0 になってもアトラスはすぐには破棄せずキャッシュに残す。
  ///       次に同じ path × sizePx が Load されたら再利用する（シーン遷移での再ロード防止）。
  ///       未使用キャッシュが kMaxCachedFonts を超えたら古いものから破棄する。
  /// @note GPU が参照を終えている前提（シーン終了時などに呼ぶ）。
  void Unload(int handle);

  /// @brief 未使用（参照カウント 0）のキャッシュ済みフォントをすべて破棄する
  /// @note GPU が参照を終えている前提。
  void PurgeUnused();

  /// @brief 有効なハンドルか
  /// @param handle フォントハンドル
  /// @return 使用中でアトラスが読み込み済みなら true
  bool IsValid(int handle) const;

  /// @brief ハンドルから FontAtlas を取得する
  /// @param handle フォントハンドル
  /// @return FontAtlas（無効なハンドルなら nullptr）
  FontAtlas *Get(int handle);
  /// @brief ハンドルから FontAtlas を取得する（const 版）
  /// @param handle フォントハンドル
  /// @return FontAtlas（無効なハンドルなら nullptr）
  const FontAtlas *Get(int handle) const;

  /// @brief 文字列を描画する
  /// @param handle      フォントハンドル
  /// @param text        UTF-32 文字列（'\n' で改行）
  /// @param pos         基準位置（ピクセル）。y は 1 行目の上端
  /// @param color       色（RGBA）
  /// @param scale       拡大率（1.0 でロード時サイズ）
  /// @param align       水平揃え
  /// @param lineSpacing 行送り倍率（1.0 で LineHeight）
  /// @param cl          コマンドリスト（"font" パイプラインがバインド済みであること）
  void DrawString(int handle, std::u32string_view text, const Vector2 &pos,
                  const Vector4 &color, float scale, TextAlign align,
                  float lineSpacing, ID3D12GraphicsCommandList *cl);

  /// @brief 文字列の描画サイズ（幅 = 最長行、高さ = 行数 × 行送り）を計測する
  /// @param handle      フォントハンドル
  /// @param text        UTF-32 文字列（'\n' で改行）
  /// @param scale       拡大率（1.0 でロード時サイズ）
  /// @param lineSpacing 行送り倍率（1.0 で LineHeight）
  /// @return 描画サイズ（ピクセル。無効なハンドルなら 0, 0）
  /// @note 未登録グリフはこの呼び出しでアトラスに登録される（転送は次の Draw 時）。
  Vector2 Measure(int handle, std::u32string_view text, float scale,
                  float lineSpacing);

  /// @brief 行送り量を取得する
  /// @param handle フォントハンドル
  /// @param scale  拡大率
  /// @return 行送り量（ピクセル。無効なハンドルなら 0）
  float LineHeight(int handle, float scale) const;

  // ===== 文字コード変換ユーティリティ =====

  /// @brief UTF-8 文字列を UTF-32 へ変換する
  /// @param utf8 UTF-8 文字列
  /// @return UTF-32 文字列（不正なバイト列は U+FFFD に置き換える）
  static std::u32string DecodeUtf8(std::string_view utf8);
  /// @brief UTF-16（wchar_t）文字列を UTF-32 へ変換する
  /// @param wide UTF-16 文字列（サロゲートペアは 1 文字にまとめる）
  /// @return UTF-32 文字列
  static std::u32string DecodeWide(std::wstring_view wide);

  /// @brief 使用中フォント数
  /// @return 使用中（ハンドルとして有効）のフォント数
  size_t InUseCount() const {
    size_t n = 0;
    for (const auto &s : fonts_) {
      if (s.inUse) ++n;
    }
    return n;
  }

private:
  /// @struct Slot
  /// @brief フォント 1 つ分の管理情報（インデックスがハンドル）
  struct Slot {
    std::unique_ptr<FontAtlas> atlas; ///< グリフアトラス（空きスロットなら null）
    std::string path;                 ///< フォントファイルのパス
    float sizePx = 0.0f;              ///< フォントサイズ（ピクセル）
    int refCount = 0;                 ///< Load された回数（参照カウント）
    bool inUse = false;     ///< ハンドルとして有効か（false でも atlas があればキャッシュ）
    uint64_t lastUsed = 0;  ///< 最後に Unload された順番（キャッシュ破棄の優先度）
  };

  /// @brief スロットのアトラスを破棄して空きにする
  /// @param s 対象のスロット
  /// @note アトラスの解放は GPU が使い終わるまで遅らせる（DeferredReleaseQueue）。
  void DestroySlot_(Slot &s);
  /// @brief 未使用キャッシュが上限を超えていたら古いものから破棄する
  void TrimCache_();

  /// @struct FrameBuffer
  /// @brief フレーム別の GPU バッファ
  struct FrameBuffer {
    Microsoft::WRL::ComPtr<ID3D12Resource> vb; ///< 頂点リング
    FontVertex *vbMapped = nullptr;            ///< 頂点リングのマップ先
    uint32_t vertexCursor = 0;                 ///< このフレームで次に書く頂点の位置

    Microsoft::WRL::ComPtr<ID3D12Resource> cbWVP; ///< 正射影行列
    TransformationMatrix *cbWVPMapped = nullptr;  ///< 正射影行列 CB のマップ先
  };

  /// @brief 1 行分の幅を計測する（改行を含まない範囲）
  /// @param atlas 対象のアトラス
  /// @param line  1 行分の文字列
  /// @param scale 拡大率
  /// @return 行の幅（ピクセル）
  float MeasureLine_(FontAtlas &atlas, std::u32string_view line, float scale);

  /// @brief 描画用の GPU バッファ（固定マテリアル CB・頂点リング・WVP CB）を必要なら作る
  /// @return 用意できていれば true、作成に失敗したら false
  bool EnsureFrameBuffers_();

private:
  ID3D12Device *device_ = nullptr; ///< D3D12 デバイス
  SRVManager *srv_ = nullptr;      ///< SRV 管理クラス

  std::vector<Slot> fonts_;    ///< フォントスロット（インデックス = ハンドル）
  uint64_t unloadCounter_ = 0; ///< Unload の通し番号（Slot::lastUsed に使う）

  FrameBuffer frames_[kFrameCount]{}; ///< フレーム別の GPU バッファ
  uint32_t frameSlot_ = 0; ///< BeginFrame ごとに進める自前のリングインデックス
  bool frameBegun_ = false;     ///< BeginFrame が一度でも呼ばれたか
  bool warnedOverflow_ = false; ///< このフレームでグリフ数超過を警告済みか

  Microsoft::WRL::ComPtr<ID3D12Resource> cbMaterial_; ///< 白・UV identity（固定）
  SpriteMaterial *cbMaterialMapped_ = nullptr;        ///< 固定マテリアル CB のマップ先

  float screenW_ = 0.0f; ///< 画面幅（ピクセル）
  float screenH_ = 0.0f; ///< 画面高さ（ピクセル）
};

} // namespace RC
