#pragma once
#include <cstdint>
#include <d3d12.h>
#include <dwrite_2.h>
#include <string>
#include <unordered_map>
#include <vector>
#include <wrl/client.h>

#include "SRVManager/SRVManager.h"

/// @struct GlyphInfo
/// @brief アトラス上に登録された 1 文字分の情報
/// @details 座標はすべてピクセル単位。offsetX / offsetY はペン位置（ベースライン上）から
///          グリフビットマップ左上へのオフセットで、offsetY は通常負の値（上方向）になる。
struct GlyphInfo {
  float u0 = 0.0f, v0 = 0.0f; ///< アトラス UV 左上
  float u1 = 0.0f, v1 = 0.0f; ///< アトラス UV 右下
  float offsetX = 0.0f;       ///< ペン位置 → ビットマップ左上 X
  float offsetY = 0.0f;       ///< ベースライン → ビットマップ上端 Y（上方向が負）
  float width = 0.0f;         ///< ビットマップ幅（ピクセル）
  float height = 0.0f;        ///< ビットマップ高さ（ピクセル）
  float advance = 0.0f;       ///< 次の文字までのペン送り量（ピクセル）
  bool hasBitmap = false;     ///< 描画する絵があるか（空白などは false）
};

/// @class FontAtlas
/// @brief DirectWrite でフォントファイル（TTF/OTF）からグリフをラスタライズし、
///        R8_UNORM のテクスチャアトラスへ動的に登録・GPU 転送するクラス
/// @details
/// - ラスタライズは DirectWrite（IDWriteGlyphRunAnalysis）のみで行い、D2D / D3D11On12 / ImGui には依存しない。
/// - グリフは GetGlyph() で初めて要求されたときにラスタライズされ、シェルフ方式でアトラスに詰められる。
/// - CPU 側アトラスの更新は Flush() で「汚れた行の帯」だけをアップロードする。
///   Flush は描画コマンドリストに Copy を記録するため、同一フレーム内の後続 Draw から参照できる。
/// - 1 インスタンス = 1 フォントファイル × 1 ピクセルサイズ。別サイズは別インスタンスを作ること
///   （描画時の scale 引数で拡縮もできるが、大きく拡大するとぼやける）。
class FontAtlas {
public:
  /// @brief アトラス幅・高さの最小値（RowPitch を 512 バイト境界に揃えるため）
  static constexpr uint32_t kMinAtlasSize = 512;
  /// @brief アップロード用ステージングのリング数（トリプルバッファ）
  static constexpr uint32_t kStagingCount = 3;
  /// @brief グリフ間のパディング（にじみ防止）
  static constexpr uint32_t kPadding = 1;

  /// @brief コンストラクタ（Initialize を呼ぶまでは未初期化）
  FontAtlas() = default;
  /// @brief デストラクタ
  /// @note SRV は解放できないため、破棄前に Term(&srv) を呼んでおくこと（Debug ビルドでは assert で検査する）。
  ~FontAtlas();

  /// @brief コピー禁止（GPU リソースを所有するため）
  FontAtlas(const FontAtlas &) = delete;
  /// @brief コピー代入禁止（GPU リソースを所有するため）
  FontAtlas &operator=(const FontAtlas &) = delete;

  /// @brief フォントを読み込み、アトラステクスチャと SRV を作成する
  /// @param srv       SRV 管理クラス
  /// @param path      フォントファイルのパス（UTF-8。.ttf / .otf / .ttc）
  /// @param sizePx    フォントサイズ（ピクセル。em 高さ）
  /// @param atlasSize アトラスの一辺（512 の倍数に丸められる。例: 1024, 2048）
  /// @param faceIndex TTC 等でフェイスが複数ある場合のインデックス
  /// @return 成功なら true
  /// @note 既に初期化済みなら一度 Term してから作り直す。
  bool Initialize(SRVManager *srv, const std::string &path, float sizePx,
                  uint32_t atlasSize = 1024, uint32_t faceIndex = 0);

  /// @brief GPU リソースと SRV を解放し、未初期化状態に戻す
  /// @param srv SRV 管理クラス（GPU が参照を終えていること。nullptr なら SRV は解放しない）
  void Term(SRVManager *srv);

  /// @brief グリフ情報を取得する（未登録ならラスタライズしてアトラスへ追加）
  /// @param codepoint Unicode コードポイント（UTF-32）
  /// @return グリフ情報。フォントに無い／アトラス満杯などで失敗した場合は nullptr
  const GlyphInfo *GetGlyph(uint32_t codepoint);

  /// @brief 未転送のグリフを GPU テクスチャへ転送する
  /// @param cl         描画に使用しているコマンドリスト（Copy と Barrier を記録）
  /// @param frameIndex フレームインデックス（ステージングのリング選択に使用）
  /// @note 汚れがなければ何もしない。Draw の直前に毎回呼んでよい。
  void Flush(ID3D12GraphicsCommandList *cl, uint32_t frameIndex);

  /// @brief 転送待ちのグリフがあるか
  /// @return 未転送の行があれば true
  bool IsDirty() const { return dirtyMinY_ <= dirtyMaxY_; }

  // ===== ゲッター =====

  /// @brief アトラステクスチャの GPU SRV ハンドルを取得する
  /// @return GPU ディスクリプタハンドル
  D3D12_GPU_DESCRIPTOR_HANDLE Srv() const { return srv_.gpu; }
  /// @brief 初期化済みか
  /// @return フォントフェイスとテクスチャが揃っていれば true
  bool IsLoaded() const { return fontFace_ != nullptr && texture_ != nullptr; }
  /// @brief フォントサイズを取得する
  /// @return フォントサイズ（ピクセル）
  float SizePx() const { return sizePx_; }
  /// @brief ベースラインから上端までの高さを取得する
  /// @return 高さ（ピクセル）
  float Ascent() const { return ascent_; }
  /// @brief ベースラインから下端までの高さを取得する
  /// @return 高さ（ピクセル、正の値）
  float Descent() const { return descent_; }
  /// @brief 行送り量を取得する
  /// @return ascent + descent + lineGap（ピクセル）
  float LineHeight() const { return lineHeight_; }
  /// @brief アトラスの一辺を取得する
  /// @return 一辺の長さ（ピクセル）
  uint32_t AtlasSize() const { return atlasSize_; }
  /// @brief フォントファイルパスを取得する
  /// @return Initialize に渡されたパス
  const std::string &Path() const { return path_; }
  /// @brief 登録済みグリフ数を取得する
  /// @return グリフ数
  size_t GlyphCount() const { return glyphs_.size(); }
  /// @brief アトラスが満杯になって登録に失敗したことがあるか
  /// @return 一度でも満杯になっていれば true
  bool IsAtlasFull() const { return atlasFull_; }

private:
  /// @brief DirectWrite ファクトリとフォントフェイスを作成し、メトリクスを求める
  /// @param path      フォントファイルのパス（UTF-8）
  /// @param faceIndex フェイスのインデックス（範囲外なら 0 を使う）
  /// @return 成功なら true
  bool CreateFontFace_(const std::string &path, uint32_t faceIndex);
  /// @brief R8_UNORM のアトラステクスチャと SRV を作成する
  /// @param srv SRV 管理クラス
  /// @return 成功なら true
  bool CreateTexture_(SRVManager *srv);
  /// @brief 1 文字をラスタライズし、CPU アトラスへ書き込む
  /// @param codepoint Unicode コードポイント（UTF-32）
  /// @param out       結果のグリフ情報
  /// @return 成功なら true（空白など絵の無いグリフも true）
  bool Rasterize_(uint32_t codepoint, GlyphInfo &out);
  /// @brief シェルフ方式でアトラス上に矩形を確保する
  /// @param w    幅（ピクセル。パディング込み）
  /// @param h    高さ（ピクセル。パディング込み）
  /// @param outX 確保した矩形の左上 X
  /// @param outY 確保した矩形の左上 Y
  /// @return 確保できたら true（満杯なら false）
  bool AllocRect_(uint32_t w, uint32_t h, uint32_t &outX, uint32_t &outY);
  /// @brief 転送が必要な行範囲を広げる
  /// @param y0 先頭行
  /// @param y1 末尾行（この行を含む）
  void MarkDirty_(uint32_t y0, uint32_t y1);

private:
  // --- DirectWrite ---
  Microsoft::WRL::ComPtr<IDWriteFactory> factory_;   ///< DirectWrite ファクトリ
  Microsoft::WRL::ComPtr<IDWriteFactory2> factory2_; ///< グレースケール AA 用（無ければ nullptr）
  Microsoft::WRL::ComPtr<IDWriteFontFace> fontFace_; ///< 読み込んだフォントフェイス
  DWRITE_FONT_METRICS fontMetrics_{};                ///< フォントメトリクス（デザイン単位）

  std::string path_;         ///< フォントファイルパス
  float sizePx_ = 0.0f;      ///< フォントサイズ（ピクセル）
  float designScale_ = 0.0f; ///< デザイン単位 → ピクセル
  float ascent_ = 0.0f;      ///< ベースラインから上端までの高さ（ピクセル）
  float descent_ = 0.0f;     ///< ベースラインから下端までの高さ（ピクセル）
  float lineHeight_ = 0.0f;  ///< 行送り量（ピクセル）

  // --- CPU アトラス / パッキング ---
  uint32_t atlasSize_ = 0;        ///< アトラスの一辺（ピクセル）
  std::vector<uint8_t> cpuAtlas_; ///< R8、atlasSize_ * atlasSize_
  uint32_t shelfX_ = 0;           ///< 現在のシェルフの次の書き込み X
  uint32_t shelfY_ = 0;           ///< 現在のシェルフの上端 Y
  uint32_t shelfH_ = 0;           ///< 現在のシェルフの高さ（詰めた矩形の最大高さ）
  bool atlasFull_ = false;        ///< 満杯で登録に失敗したことがあるか
  bool warnedFull_ = false;       ///< 満杯の警告ログを出したか（1 回だけ出すため）

  std::unordered_map<uint32_t, GlyphInfo> glyphs_; ///< コードポイント → グリフ情報

  // --- 転送管理 ---
  uint32_t dirtyMinY_ = UINT32_MAX; ///< 未転送の先頭行（汚れが無ければ UINT32_MAX）
  uint32_t dirtyMaxY_ = 0;          ///< 未転送の末尾行（この行を含む）

  // --- GPU ---
  ID3D12Device *device_ = nullptr;                 ///< リソース作成に使うデバイス
  Microsoft::WRL::ComPtr<ID3D12Resource> texture_; ///< アトラステクスチャ（R8_UNORM）
  SRVManager::Handle srv_{};                       ///< アトラステクスチャの SRV

  /// @struct Staging
  /// @brief アップロード用ステージングバッファ 1 本分
  struct Staging {
    Microsoft::WRL::ComPtr<ID3D12Resource> res; ///< UPLOAD ヒープのバッファ
    uint8_t *mapped = nullptr;                  ///< Map 済みの先頭アドレス
  };
  Staging staging_[kStagingCount]{}; ///< フレームごとに使い回すステージングのリング
};
