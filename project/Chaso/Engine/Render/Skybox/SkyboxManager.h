#pragma once

#include <d3d12.h>
#include <memory>
#include <string>
#include <vector>

#include "../../Common/struct.h"

class Skybox;
class TextureManager;

namespace RC {

/// @class SkyboxManager
/// @brief スカイボックス（Skybox）を管理するマネージャクラス
/// @details キューブマップテクスチャを使ったスカイボックスのロード、描画、および環境マップ用の SRV 管理を行う。
class SkyboxManager {
public:
  /// @brief デバイスとテクスチャマネージャを受け取って初期化する
  /// @param device D3D12 デバイス
  /// @param texman テクスチャマネージャ（DDS キューブマップのロードに使う）
  void Init(ID3D12Device *device, TextureManager *texman);

  /// @brief デストラクタ
  ~SkyboxManager();

  /// @brief 終了処理。管理しているすべてのスカイボックスを解放する
  void Term();

  // ムーブ許可、コピー禁止
  SkyboxManager() = default;
  SkyboxManager(SkyboxManager &&) = default;
  SkyboxManager &operator=(SkyboxManager &&) = default;
  SkyboxManager(const SkyboxManager &) = delete;
  SkyboxManager &operator=(const SkyboxManager &) = delete;

  /// @brief DDS パスからテクスチャのロードと Skybox の生成をまとめて行う
  /// @param ddsPath DDS 形式のキューブマップテクスチャのパス
  /// @return スカイボックスハンドル（失敗時は -1）
  /// @note キューブマップとして読めない場合（ファイルが無い・2D テクスチャ等）は生成しない。
  int Create(const std::string &ddsPath);

  /// @brief 指定したハンドルに対応するスカイボックスを解放する
  /// @param handle スカイボックスハンドル
  void Unload(int handle);

  /// @brief 有効なハンドルか確認する
  /// @param handle スカイボックスハンドル
  /// @return 有効なら true
  bool IsValid(int handle) const;

  /// @brief ハンドルからスカイボックスの実体を取得する
  /// @param handle スカイボックスハンドル
  /// @return Skybox へのポインタ。無効なハンドルの場合は nullptr
  Skybox *Get(int handle);

  /// @brief ハンドルからスカイボックスの実体を取得する（const 版）
  /// @param handle スカイボックスハンドル
  /// @return Skybox への const ポインタ。無効なハンドルの場合は nullptr
  const Skybox *Get(int handle) const;

  /// @brief ハンドルからトランスフォーム情報へのポインタを取得する
  /// @param handle スカイボックスハンドル
  /// @return Transform 構造体へのポインタ。無効なハンドルの場合は nullptr
  Transform *GetTransformPtr(int handle);

  /// @brief スカイボックスの乗算カラー（明るさや色味）を設定する
  /// @param handle スカイボックスハンドル
  /// @param color  設定する RGBA カラー
  void SetColor(int handle, const Vector4 &color);

  /// @brief Create 時に読み込んだキューブマップをスカイボックスに設定し直す
  /// @param handle スカイボックスハンドル
  void ApplyTexture(int handle);

  /// @brief キューブマップの GPU ディスクリプタハンドルを取得する
  /// @details 環境マップやリフレクション用のテクスチャとして使うときに呼ぶ。
  /// @param handle スカイボックスハンドル
  /// @return GPU ディスクリプタハンドル（無効なハンドルなら ptr = 0）
  D3D12_GPU_DESCRIPTOR_HANDLE GetTextureSrv(int handle) const;

private:
  /// @struct Slot
  /// @brief スカイボックス保持用スロット
  struct Slot {
    std::unique_ptr<Skybox> ptr;           ///< スカイボックス実体
    bool inUse = false;                   ///< 使用中フラグ
    D3D12_GPU_DESCRIPTOR_HANDLE texSrv{}; ///< キューブマップの SRV
  };

  /// @brief 未使用スロットを検索・確保する
  /// @return 確保したスロットのインデックス（空きが無ければ末尾に追加する）
  int AllocSlot_();

private:
  ID3D12Device *device_ = nullptr;     ///< デバイス
  TextureManager *texman_ = nullptr;   ///< テクスチャマネージャ
  std::vector<Slot> skyboxes_;         ///< スロット配列
};

} // namespace RC
