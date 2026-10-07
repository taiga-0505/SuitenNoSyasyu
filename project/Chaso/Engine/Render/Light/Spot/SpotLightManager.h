#pragma once
#include "Render/FrameResource.h" // RC::DynamicCB
#include "Light/Spot/SpotLightSource.h"
#include "struct.h"
#include <array>
#include <d3d12.h>
#include <vector>
#include <wrl/client.h>

namespace RC {

/// @brief スポットライト(Spot Light)を管理するマネージャクラス
/// 最大 kMaxSpotLights 個のアクティブなスポットライトを管理し、一括して GPU (b4スロット)へ転送します。
class SpotLightManager {
public:
  /// @brief 同時に有効化可能なスポットライトの最大数
  /// @details struct.h の kMaxSpotLights（= HLSL の MAX_SPOT_LIGHTS）と同じ値にする
  static constexpr int kMaxActive = static_cast<int>(kMaxSpotLights);

  /// @brief 初期化処理
  /// @param device DirectX12デバイス。定数バッファの作成に使用します。
  void Init(ID3D12Device *device);

  /// @brief 終了処理。確保した定数バッファなどのリソースを解放します。
  void Term();

  /// @brief 新規ライトを生成する
  /// @return 生成されたライトのハンドル（失敗時は -1）
  int Create();

  /// @brief 指定したハンドルに対応するライトを破棄する
  /// @param handle ライトハンドル
  void Destroy(int handle);

  /// @brief 単一のライトのみをアクティブにする
  /// 内部で現在のアクティブリストをクリアした後、指定したライトを追加します。
  /// @param handle アクティブにするライトのハンドル
  void SetActive(int handle);

  /// @brief 先頭のアクティブライトのハンドルを取得する
  /// @return ライトハンドル。アクティブがない場合は -1
  int GetActiveHandle() const { return (activeCount_ > 0) ? active_[0] : -1; }

  /// @brief アクティブリストをすべてクリアする
  void ClearActive();

  /// @brief アクティブリストにライトを追加する
  /// @param handle 追加するライトのハンドル
  /// @return 追加に成功（最大数未満）した場合は true
  bool AddActive(int handle);

  /// @brief アクティブリストから特定のライトを除外する
  /// @param handle 除外するライトのハンドル
  void RemoveActive(int handle);

  /// @brief 現在アクティブなライトの数を取得
  /// @return アクティブ数 (0 ～ kMaxActive)
  int GetActiveCount() const { return activeCount_; }

  /// @brief アクティブリスト内の指定インデックスのハンドルを取得
  /// @param index リスト内インデックス
  /// @return ライトハンドル
  int GetActiveHandleAt(int index) const;

  /// @brief ハンドルからライトの実体を取得
  /// @param handle ライトハンドル
  /// @return ライトソースへのポインタ。無効なハンドルの場合は nullptr
  SpotLightSource *Get(int handle);

  /// @brief ハンドルからライトの実体を取得 (const)
  /// @param handle ライトハンドル
  /// @return ライトソースへのconstポインタ。無効なハンドルの場合は nullptr
  const SpotLightSource *Get(int handle) const;

  /// @brief アクティブリストの先頭にあるライトの実体を取得
  /// アクティブがない場合はデフォルト（ハンドル0）のライトを返しようと試みます。
  /// @return アクティブなライトソースへのポインタ
  SpotLightSource *GetActive();

  /// @brief アクティブリストの先頭にあるライトの実体を取得 (const)
  /// @return アクティブなライトソースへのconstポインタ
  const SpotLightSource *GetActive() const;

  /// @brief GPU転送用定数バッファ(SpotLightsCB / b4)のGPU仮想アドレスを取得
  /// @details 同期（SyncCB）は行わない。描画パスごとに RenderContext が SyncCB() を 1 回呼ぶ。
  /// @return GPU上の仮想アドレス
  D3D12_GPU_VIRTUAL_ADDRESS GetCBAddress();

  /// @brief CPU側のライト状態を GPU 定数バッファへ転送する（RenderContext が描画パスごとに 1 回呼ぶ）
  void SyncCB();

private:
  /// @brief ライト管理用のスロット構造体
  struct Slot {
    bool inUse = false;           ///< 使用中フラグ
    SpotLightSource light{};      ///< ライトソース実体
  };

  /// @brief ハンドルの有効性をチェックする
  bool IsValid_(int handle) const;

  /// @brief 未使用スロットを検索・確保する
  int AllocSlot_();

  /// @brief アクティブがない場合の代替ハンドルを解決する
  int ResolveFallbackHandle_() const;

  /// @brief 定数バッファを確保する
  void EnsureCB_();

  /// @brief CPU側のデータをGPU定数バッファに同期（コピー）する
  void SyncCB_();

private:
  Microsoft::WRL::ComPtr<ID3D12Device> device_; ///< デバイス保持
  bool initialized_ = false;                    ///< 初期化済みフラグ

  std::vector<Slot> slots_;                     ///< ライトスロット配列

  std::array<int, kMaxActive> active_{};        ///< アクティブハンドル配列
  int activeCount_ = 0;                         ///< 現在のアクティブ数

  /// @brief 定数バッファ。値は CPU 側（mapped_ が指す）に持ち、アドレスを求められたときに今フレームの領域へ送る
  /// @details 以前は Map しっぱなしの固定 CB だった（CPU と GPU を並行させると上書き競合が起きる）
  DynamicCB<::SpotLightsCB> dyn_;
  bool hasCB_ = false;                          ///< CB を使える状態か（デバイス初期化後）
  ::SpotLightsCB *mapped_ = nullptr;                       ///< 書き込み先（= dyn_.Ptr()）
};

} // namespace RC
