#pragma once
#include <d3d12.h>
#include <memory>
#include <vector>
#include "struct.h"

class PrimitiveMesh;
class TextureManager;

namespace RC {

/// @class PrimitiveMeshManager
/// @brief 動的に生成されたプリミティブメッシュを管理するクラス
/// @details ハンドル制でメッシュオブジェクトを保持し、テクスチャの適用やトランスフォームの管理を行う。
class PrimitiveMeshManager {
public:
  /// @brief デバイスとテクスチャマネージャを受け取り、テクスチャ未指定時用の白テクスチャを読み込む
  /// @param device D3D12 デバイス
  /// @param texman テクスチャマネージャ（テクスチャハンドルの解決に使う）
  void Init(ID3D12Device *device, TextureManager *texman);

  /// @brief デストラクタ
  ~PrimitiveMeshManager();

  /// @brief 終了処理。管理しているすべてのプリミティブメッシュを解放する
  void Term();

  /// @brief 新規プリミティブメッシュを生成する
  /// @param data          モデルデータ（頂点・インデックス配列など）
  /// @param textureHandle 使うテクスチャのハンドル（-1 なら白テクスチャ）
  /// @param typeName      オブジェクトの識別名（デバッグ・UI 表示用）
  /// @return メッシュハンドル（Init 前なら -1）
  int Create(const ModelData &data, int textureHandle = -1, const std::string &typeName = "Unknown");

  /// @brief 指定したハンドルに対応するプリミティブメッシュを解放する
  /// @param handle メッシュハンドル
  void Unload(int handle);

  /// @brief 有効なハンドルか確認する
  /// @param handle メッシュハンドル
  /// @return 有効なら true
  bool IsValid(int handle) const;

  /// @brief ハンドルからプリミティブメッシュの実体を取得する
  /// @param handle メッシュハンドル
  /// @return PrimitiveMesh へのポインタ。無効なハンドルの場合は nullptr
  PrimitiveMesh *Get(int handle);

  /// @brief ハンドルからプリミティブメッシュの実体を取得する（const 版）
  /// @param handle メッシュハンドル
  /// @return PrimitiveMesh への const ポインタ。無効なハンドルの場合は nullptr
  const PrimitiveMesh *Get(int handle) const;

  /// @brief ハンドルからトランスフォーム情報へのポインタを取得する
  /// @param handle メッシュハンドル
  /// @return Transform 構造体へのポインタ。無効なハンドルの場合は nullptr
  Transform *GetTransformPtr(int handle);

  /// @brief メッシュの乗算カラーを設定する
  /// @param handle メッシュハンドル
  /// @param color  設定する RGBA カラー
  void SetColor(int handle, const Vector4 &color);

  /// @brief メッシュに使うテクスチャを上書き適用する
  /// @param handle            メッシュハンドル
  /// @param overrideTexHandle 新しいテクスチャハンドル（負なら Create 時のテクスチャに戻す）
  void ApplyTexture(int handle, int overrideTexHandle);

private:
  /// @struct Slot
  /// @brief メッシュオブジェクト保持用スロット
  struct Slot {
    std::unique_ptr<PrimitiveMesh> ptr; ///< メッシュ実体
    bool inUse = false;                ///< 使用中フラグ
    int defaultTexHandle = -1;         ///< Create 時に指定されたテクスチャハンドル（-1 なら白テクスチャ）
    std::string typeName;              ///< 型名・識別名
  };

  /// @brief 未使用スロットを検索・確保する
  /// @return 確保したスロットのインデックス（空きが無ければ末尾に追加する）
  int AllocSlot_();

private:
  ID3D12Device *device_ = nullptr;     ///< デバイス
  TextureManager *texman_ = nullptr;   ///< テクスチャマネージャ
  D3D12_GPU_DESCRIPTOR_HANDLE whiteSrv_{}; ///< テクスチャ未指定時の白テクスチャ用 SRV
  std::vector<Slot> primitives_;       ///< スロット配列
};

} // namespace RC
