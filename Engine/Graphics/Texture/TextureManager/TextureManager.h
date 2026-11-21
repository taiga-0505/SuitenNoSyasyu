#pragma once
#include "Texture/Texture2D/Texture2D.h"
#include "Dx12/CommandContext/CommandContext.h"
#include <d3d12.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <mutex>
#include <future>
#include <wrl/client.h>

class DescriptorHeap;

/// @class TextureManager
/// @brief テクスチャリソースのロード・キャッシュ管理を一括して行うマネージャークラス
/// @details 同じパスのテクスチャを二重にロードしないようにキャッシュを保持し、GPU 記述子ハンドルや ID によるアクセスを提供する。
///          ロード時の GPU 転送タスクを追跡し、各操作は再帰ミューテックスでスレッドセーフにしている。
class TextureManager {
public:
  using TextureID = int; ///< テクスチャを一意に識別する ID 型

  /// @struct UploadTask
  /// @brief GPU へのアップロードタスクを管理する構造体
  struct UploadTask {
    Microsoft::WRL::ComPtr<ID3D12Resource> uploadRes; ///< アップロード用の中間リソース
    uint64_t fenceValue;                             ///< 完了待ち用のフェンス値
  };

  /// @brief テクスチャ転送用のコマンドコンテキストを作成する
  /// @param srv SRV 管理クラスへのポインタ
  void Init(SRVManager *srv);

  /// @brief 残っている転送の完了を待ち、全テクスチャと SRV を解放する
  void Term();

  /// @brief テクスチャを同期ロードし、GPU 記述子ハンドルを返す
  /// @param path ファイルパス（同じパスはキャッシュを返す）
  /// @param srgb sRGB フォーマットとして扱うか
  /// @return GPU 上の SRV ハンドル
  D3D12_GPU_DESCRIPTOR_HANDLE Load(const std::string &path, bool srgb = true);

  /// @brief パス指定で直接 Texture2D オブジェクトを取得する
  /// @param path ファイルパス
  /// @return Texture2D ポインタ（見つからなければ nullptr）
  Texture2D *Get(const std::string &path);

  /// @brief テクスチャの非同期ロードを開始し、管理用 ID を取得する
  /// @param path ファイルパス
  /// @param srgb sRGB フォーマットとして扱うか
  /// @return テクスチャ ID（同じパスが既に登録済みなら同じ ID を返し、ロードは追加しない）
  /// @note ID はすぐに返るが、ロード完了は IsLoaded で確認する。
  TextureID LoadID(const std::string &path, bool srgb = true);

  /// @brief ID 指定でテクスチャを解放する
  /// @param id 解放するテクスチャ ID
  /// @note テクスチャ本体と SRV の返却は、GPU が使い終わってから遅延して行う。
  void Unload(TextureID id);

  /// @brief ID から GPU 上の SRV ハンドルを取得する
  /// @param id テクスチャ ID
  /// @return GPU 記述子ハンドル（未ロード・不明な ID なら白 1x1 の SRV。それも無ければ {0}）
  D3D12_GPU_DESCRIPTOR_HANDLE GetSrv(TextureID id) const;

  /// @brief ID のテクスチャが GPU へのロードまで完了しているか
  /// @details GetSrv はロード前でも代替（白）を返すため、「本物の SRV が取れたか」を知りたいときに使う。
  /// @param id テクスチャ ID
  /// @return ロード完了なら true
  bool IsLoaded(TextureID id) const;

  /// @brief ID からテクスチャのメタデータを取得する
  /// @param id テクスチャ ID
  /// @return TexMetadata へのポインタ（未ロード・不明な ID なら nullptr）
  const DirectX::TexMetadata *GetMeta(TextureID id) const;

  /// @brief ID から Texture2D オブジェクトを取得する
  /// @param id テクスチャ ID
  /// @return Texture2D ポインタ（不明な ID なら nullptr）
  Texture2D *GetTexture(TextureID id);

  /// @brief SRV（GPU ハンドル）からロード元のパスを逆引きする
  /// @param srv 検索対象の GPU 記述子ハンドル
  /// @return ロード元のファイルパス（見つからなければ空文字）
  std::string GetPathBySrv(D3D12_GPU_DESCRIPTOR_HANDLE srv) const;

  /// @brief このシーンでのログ出力履歴をクリアする（次のシーンでキャッシュ利用ログを再び出すため）
  void ClearLogHistory() { loggedPaths_.clear(); }

private:
  /// @brief GPU 側のコピーが完了したアップロード用中間リソースを解放する
  void ReleasePendingUploads();

private:
  SRVManager *srv_ = nullptr;        ///< SRV 管理クラス
  CommandContext loadCmd_;          ///< ロード・転送用コマンドコンテキスト
  std::vector<UploadTask> pendingUploads_; ///< 完了待ちのアップロードタスクリスト
  std::unordered_map<std::string, Texture2D> cache_; ///< ファイルパスをキーとしたテクスチャキャッシュ

  int nextId_ = 1; ///< 次に割り当てる ID
  std::unordered_map<std::string, TextureID> pathToId_; ///< パスから ID へのマップ
  std::unordered_map<TextureID, std::string> idToPath_; ///< ID からパスへのマップ

  std::unordered_set<std::string> loggedPaths_; ///< ログ出力済みのパス履歴（重複防止用）

  mutable std::recursive_mutex mtx_; ///< スレッドセーフ確保用の再帰ミューテックス
};
