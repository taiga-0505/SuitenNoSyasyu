#pragma once
#include <cstdint>
#include <d3d12.h>
#include <vector>
#include <wrl/client.h>
#include "SRVManager/SRVManager.h"

class SRVManager;

/// @class StructuredBufferManager
/// @brief StructuredBuffer（構造化バッファ）を管理するクラス
/// @details Upload ヒープ上へのリソース確保と、SRVManager を用いたディスクリプタ管理を統合する。
class StructuredBufferManager {
public:
  /// @brief バッファを識別するための ID 型
  using BufferID = int;

  /// @brief 初期化する
  /// @param srvMgr SRV 管理クラスへのポインタ
  void Init(SRVManager *srvMgr);

  /// @brief 終了処理。全バッファを破棄する
  void Term();

  /// @brief 新規バッファを作成する（Upload ヒープ上に確保し、SRV も作る）
  /// @param elementCount 要素数
  /// @param strideBytes  1 要素あたりのバイトサイズ
  /// @return 生成されたバッファの ID
  BufferID Create(UINT elementCount, UINT strideBytes);

  /// @brief CPU から書き込むためにバッファをマップする
  /// @param id 対象バッファの ID
  /// @return マップされたメモリへのポインタ（ID が不正なら nullptr）
  /// @note マップ済みなら同じポインタを返す。
  void *Map(BufferID id);

  /// @brief バッファをアンマップする
  /// @param id 対象バッファの ID
  void Unmap(BufferID id);

  /// @brief SRV の GPU ハンドルを取得する
  /// @param id 対象バッファの ID
  /// @return SRV の GPU ハンドル（ID が不正なら 0）
  D3D12_GPU_DESCRIPTOR_HANDLE GetSrv(BufferID id) const;

  /// @brief リソース本体を取得する
  /// @param id 対象バッファの ID
  /// @return ID3D12Resource へのポインタ（ID が不正なら nullptr）
  ID3D12Resource *GetResource(BufferID id) const;

  /// @brief 指定したバッファを破棄する
  /// @param id 対象バッファの ID
  /// @note SRV スロットを即座に返却するので、GPU が参照し終わってから呼ぶこと。ID は再利用されない。
  void Destroy(BufferID id);

private:
  /// @brief 管理用のエントリ構造体
  struct Entry {
    Microsoft::WRL::ComPtr<ID3D12Resource> resource; ///< バッファリソース本体
    void *mapped = nullptr;                         ///< マップされたアドレス

    // SRVManager が管理するスロット
    SRVManager::Handle srvHandle{};                 ///< SRVManager が管理するディスクリプタハンドル

    UINT stride = 0;                                ///< ストライドサイズ
    UINT count = 0;                                 ///< 要素数
  };

  SRVManager *srvMgr_ = nullptr;                    ///< SRV マネージャ（非所有）
  Microsoft::WRL::ComPtr<ID3D12Device> device_;     ///< デバイス（ComPtr で参照を保持）
  std::vector<Entry> entries_;                     ///< バッファエントリの配列
};
