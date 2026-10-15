#pragma once

#include <d3d12.h>
#include <memory>
#include <vector>

#include "../../Common/struct.h"

class Skydome;
class TextureManager;

namespace RC {

/// @class SkydomeManager
/// @brief スカイドーム（Skydome）を管理するマネージャクラス
/// @details 球体メッシュとパノラマテクスチャ（または背景テクスチャ）を使ったスカイドームを管理する。
class SkydomeManager {
public:
  /// @brief デバイスとテクスチャマネージャを受け取って初期化する
  /// @param device D3D12 デバイス
  /// @param texman テクスチャマネージャ（テクスチャハンドルの解決に使う）
  void Init(ID3D12Device *device, TextureManager *texman);

  /// @brief デストラクタ
  ~SkydomeManager();

  /// @brief 終了処理。管理しているすべてのスカイドームを解放する
  void Term();

  /// @brief 新規スカイドームを生成する
  /// @param textureHandle 使うテクスチャのハンドル（負ならテクスチャを設定しない）
  /// @param radius        球体の半径
  /// @param sliceCount    経度方向の分割数
  /// @param stackCount    緯度方向の分割数
  /// @return スカイドームハンドル（Init 前なら -1）
  int Create(int textureHandle, float radius = 100.0f, unsigned int sliceCount = 32,
             unsigned int stackCount = 32);

  /// @brief 指定したハンドルに対応するスカイドームを解放する
  /// @param handle スカイドームハンドル
  void Unload(int handle);

  /// @brief 有効なハンドルか確認する
  /// @param handle スカイドームハンドル
  /// @return 有効なら true
  bool IsValid(int handle) const;

  /// @brief ハンドルからスカイドームの実体を取得する
  /// @param handle スカイドームハンドル
  /// @return Skydome へのポインタ。無効なハンドルの場合は nullptr
  Skydome *Get(int handle);

  /// @brief ハンドルからスカイドームの実体を取得する（const 版）
  /// @param handle スカイドームハンドル
  /// @return Skydome への const ポインタ。無効なハンドルの場合は nullptr
  const Skydome *Get(int handle) const;

  /// @brief ハンドルからトランスフォーム情報へのポインタを取得する
  /// @param handle スカイドームハンドル
  /// @return Transform 構造体へのポインタ。無効なハンドルの場合は nullptr
  Transform *GetTransformPtr(int handle);

  /// @brief スカイドームの乗算カラーを設定する
  /// @param handle スカイドームハンドル
  /// @param color  設定する RGBA カラー
  void SetColor(int handle, const Vector4 &color);

  /// @brief スカイドームに使うテクスチャを上書き適用する
  /// @param handle            スカイドームハンドル
  /// @param overrideTexHandle 新しいテクスチャハンドル（負なら Create 時のテクスチャに戻す）
  void ApplyTexture(int handle, int overrideTexHandle);

private:
  /// @struct Slot
  /// @brief スカイドーム保持用スロット
  struct Slot {
    std::unique_ptr<Skydome> ptr; ///< スカイドーム実体
    bool inUse = false;          ///< 使用中フラグ
    int defaultTexHandle = -1;   ///< Create 時に指定されたテクスチャハンドル
  };

  /// @brief 未使用スロットを検索・確保する
  /// @return 確保したスロットのインデックス（空きが無ければ末尾に追加する）
  int AllocSlot_();

private:
  ID3D12Device *device_ = nullptr;     ///< デバイス
  TextureManager *texman_ = nullptr;   ///< テクスチャマネージャ
  std::vector<Slot> skydomes_;         ///< スロット配列
};

} // namespace RC
