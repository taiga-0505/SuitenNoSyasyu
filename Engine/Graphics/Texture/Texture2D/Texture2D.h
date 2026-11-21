#pragma once
#include "DirectXTex/DirectXTex.h"
#include "SRVManager/SRVManager.h"
#include <cassert>
#include <d3d12.h>
#include <string>
#include <wrl/client.h>

class SRVManager;
class CommandContext;

/// @class Texture2D
/// @brief 2D テクスチャリソースのロードと管理を行うクラス
/// @details DirectXTex を使用して画像ファイルを読み込み、D3D12 リソースとして GPU へ転送・管理する。
///          同期ロードに加え、CPU での読み込みと GPU 転送を分けた非同期ロードにも対応している。
class Texture2D {
public:
  /// @brief コンストラクタ（ロードは LoadFromFile / LoadCPU で行う）
  Texture2D() = default;
  /// @brief デストラクタ（Term を呼ぶ。SRV は先に Term(&srvMgr) で解放しておくこと）
  ~Texture2D() { Term(); }

  /// @brief ファイルからテクスチャをロードして GPU に転送する（同期）
  /// @param srv  SRV 管理クラス
  /// @param cmd  コマンドコンテキスト（転送に使用）
  /// @param path ファイルパス
  /// @param srgb sRGB フォーマットとして扱うか
  /// @return アップロード用の中間バッファ（コピーが GPU で終わるまで呼び出し側で保持する）
  Microsoft::WRL::ComPtr<ID3D12Resource> LoadFromFile(SRVManager &srv, CommandContext &cmd, const std::string &path, bool srgb = true);

  /// @brief ファイルからテクスチャデータを CPU メモリにロードする
  /// @param path ファイルパス
  /// @param srgb sRGB フォーマットとして扱うか
  /// @return 現状は常に true（読み込みに失敗した場合も白 1x1 で代替し、IsFallback() が true になる）
  bool LoadCPU(const std::string &path, bool srgb = true);

  /// @brief CPU にロード済みのテクスチャデータを GPU に転送し、SRV を作成する
  /// @param srv SRV 管理クラス
  /// @param cmd コマンドコンテキスト（転送に使用）
  /// @return アップロード用の中間バッファ（コピーが GPU で終わるまで呼び出し側で保持する）。失敗時は nullptr
  Microsoft::WRL::ComPtr<ID3D12Resource> Upload(SRVManager &srv, CommandContext &cmd);

  /// @brief リソースの解放
  /// @param srv SRV 管理クラス（指定すると SRV 記述子も解放される）
  /// @note SRV 記述子の解放は、GPU が参照し終わったタイミングで呼ぶ前提。
  void Term(SRVManager *srv = nullptr);

  // コピー禁止
  Texture2D(const Texture2D &) = delete;
  Texture2D &operator=(const Texture2D &) = delete;

  /// @brief ムーブコンストラクタ
  /// @param o ムーブ元
  Texture2D(Texture2D &&o) noexcept { *this = std::move(o); }

  /// @brief ムーブ代入演算子
  /// @param o ムーブ元（中身は空になる）
  /// @return 自身への参照
  /// @note 代入先が空でない状態で呼ぶのは運用ミス（Debug ビルドでは assert で止まる）。
  Texture2D &operator=(Texture2D &&o) noexcept {
    if (this == &o)
      return *this;

#if _DEBUG
    // ここが非空でムーブ代入されるのは運用ミス
    assert(resource_ == nullptr);
    assert(!srv_.IsValid());
#endif
    path_ = std::move(o.path_);
    isFallback_ = o.isFallback_;
    o.isFallback_ = false;
    resource_ = o.resource_;
    o.resource_ = nullptr;
    srv_ = o.srv_;
    o.srv_ = {};
    mipImages_ = std::move(o.mipImages_);
    metadata_ = o.metadata_;
    o.metadata_ = {};
    return *this;
  }

  // 取得系

  /// @brief ID3D12Resource へのポインタを取得する
  /// @return ID3D12Resource ポインタ
  ID3D12Resource *Resource() const { return resource_.Get(); }

  /// @brief SRV ハンドル（内部管理用）を取得する
  /// @return SRV ハンドル
  SRVManager::Handle SrvHandle() const { return srv_; }

  /// @brief GPU 上の SRV ハンドルを取得する
  /// @return GPU 記述子ハンドル
  D3D12_GPU_DESCRIPTOR_HANDLE GpuSrv() const { return srv_.gpu; }

  /// @brief CPU 上の SRV ハンドルを取得する
  /// @return CPU 記述子ハンドル
  D3D12_CPU_DESCRIPTOR_HANDLE CpuSrv() const { return srv_.cpu; }

  /// @brief テクスチャのメタデータを取得する
  /// @return TexMetadata への参照
  const DirectX::TexMetadata &Metadata() const { return metadata_; }

  /// @brief ロードに使用したファイルパスを取得する
  /// @return ファイルパス文字列
  const std::string &Path() const { return path_; }

  /// @brief ロードが完了しているか確認する
  /// @return 完了なら true
  bool IsLoaded() const { return resource_ != nullptr; }

  /// @brief 実ファイルの読み込みに失敗し、白 1x1 で代替されているか
  /// @return 代替されていれば true
  /// @note true でも IsLoaded() は true になる（描画は継続するため）。
  bool IsFallback() const { return isFallback_; }

private:
  std::string path_;                             ///< ファイルパス
  bool isFallback_ = false;                      ///< 白 1x1 へフォールバックしたか
  Microsoft::WRL::ComPtr<ID3D12Resource> resource_; ///< D3D12 リソース
  SRVManager::Handle srv_{};                     ///< SRV ハンドル
  DirectX::ScratchImage mipImages_;               ///< ロード中の一時イメージ（ミップマップ含む）
  DirectX::TexMetadata metadata_{};               ///< テクスチャ情報
};
