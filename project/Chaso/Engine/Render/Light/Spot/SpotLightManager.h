#pragma once
#include "Render/FrameResource.h" // RC::DynamicCB
#include "Light/Spot/SpotLightSource.h"
#include "struct.h"
#include <array>
#include <d3d12.h>
#include <vector>
#include <wrl/client.h>

namespace RC {

/// @class SpotLightManager
/// @brief スポットライト（Spot Light）を管理するマネージャクラス
/// @details 最大 kMaxActive 個のアクティブなスポットライトを管理し、まとめて GPU（b4 スロット）へ転送する。
///          ハンドル 0 は既定のライト（強さ 0）で、Destroy では破棄できない。
class SpotLightManager {
public:
  /// @brief 同時に有効化できるスポットライトの最大数
  /// @details struct.h の kMaxSpotLights（= HLSL の MAX_SPOT_LIGHTS）と同じ値にする。
  static constexpr int kMaxActive = static_cast<int>(kMaxSpotLights);

  /// @brief 既定のライト（ハンドル 0）と定数バッファを用意する
  /// @param device D3D12 デバイス（定数バッファの作成に使う）
  /// @note 呼ぶたびに一度 Term してから作り直す。
  void Init(ID3D12Device *device);

  /// @brief 終了処理。ライトと定数バッファなどのリソースを解放する
  void Term();

  /// @brief 新規ライトを生成する
  /// @return 生成されたライトのハンドル（失敗時は -1）
  /// @note アクティブなライトが 1 つも無いときは、生成したライトを自動でアクティブにする。
  int Create();

  /// @brief 指定したハンドルに対応するライトを破棄する
  /// @param handle ライトハンドル
  /// @note ハンドル 0（既定のライト）は破棄しない。
  void Destroy(int handle);

  /// @brief 単一のライトのみをアクティブにする
  /// @details 現在のアクティブリストをクリアしてから、指定したライトを追加する。
  /// @param handle アクティブにするライトのハンドル（負ならクリアだけ行う）
  void SetActive(int handle);

  /// @brief 先頭のアクティブライトのハンドルを取得する
  /// @return ライトハンドル。アクティブが無い場合は -1
  int GetActiveHandle() const { return (activeCount_ > 0) ? active_[0] : -1; }

  /// @brief アクティブリストをすべてクリアする
  void ClearActive();

  /// @brief アクティブリストにライトを追加する
  /// @param handle 追加するライトのハンドル
  /// @return 追加できた（既に入っていた場合を含む）なら true、無効なハンドルか上限に達していれば false
  bool AddActive(int handle);

  /// @brief アクティブリストから特定のライトを除外する
  /// @param handle 除外するライトのハンドル
  void RemoveActive(int handle);

  /// @brief 現在アクティブなライトの数を取得する
  /// @return アクティブ数（0〜kMaxActive）
  int GetActiveCount() const { return activeCount_; }

  /// @brief アクティブリスト内の指定インデックスのハンドルを取得する
  /// @param index リスト内インデックス
  /// @return ライトハンドル（範囲外なら -1）
  int GetActiveHandleAt(int index) const;

  /// @brief ハンドルからライトの実体を取得する
  /// @param handle ライトハンドル
  /// @return ライトソースへのポインタ。無効なハンドルの場合は nullptr
  SpotLightSource *Get(int handle);

  /// @brief ハンドルからライトの実体を取得する（const 版）
  /// @param handle ライトハンドル
  /// @return ライトソースへの const ポインタ。無効なハンドルの場合は nullptr
  const SpotLightSource *Get(int handle) const;

  /// @brief アクティブリストの先頭にあるライトの実体を取得する
  /// @details アクティブが無い場合は既定のライト（ハンドル 0）を返す。
  /// @return アクティブなライトソースへのポインタ（未初期化なら nullptr）
  SpotLightSource *GetActive();

  /// @brief アクティブリストの先頭にあるライトの実体を取得する（const 版）
  /// @return アクティブなライトソースへの const ポインタ（未初期化なら nullptr）
  const SpotLightSource *GetActive() const;

  /// @brief GPU 転送用定数バッファ（SpotLightsCB / b4）の GPU 仮想アドレスを取得する
  /// @details 同期（SyncCB）は行わない。描画パスごとに RenderContext が SyncCB() を 1 回呼ぶ。
  /// @return GPU 仮想アドレス（未初期化なら 0）
  D3D12_GPU_VIRTUAL_ADDRESS GetCBAddress();

  /// @brief CPU 側のライト状態を GPU 定数バッファへ転送する（RenderContext が描画パスごとに 1 回呼ぶ）
  void SyncCB();

private:
  /// @struct Slot
  /// @brief ライト管理用のスロット構造体
  struct Slot {
    bool inUse = false;           ///< 使用中フラグ
    SpotLightSource light{};      ///< ライトソース実体
  };

  /// @brief ハンドルの有効性をチェックする
  /// @param handle ライトハンドル
  /// @return 初期化済みで、範囲内かつ使用中のスロットなら true
  bool IsValid_(int handle) const;

  /// @brief 未使用スロットを検索・確保する
  /// @return 確保したスロットのインデックス（0 は既定のライト用なので 1 以降から探し、無ければ末尾に追加する）
  int AllocSlot_();

  /// @brief アクティブが無い場合の代替ハンドルを解決する
  /// @return 代替ハンドル（常に既定のライトの 0）
  int ResolveFallbackHandle_() const;

  /// @brief 定数バッファ（DynamicCB）を使える状態にする
  void EnsureCB_();

  /// @brief CPU 側のデータを定数バッファへ書き込む
  /// @details アクティブリストに含まれる有効なライトだけを詰めて書く。
  void SyncCB_();

private:
  Microsoft::WRL::ComPtr<ID3D12Device> device_; ///< デバイス保持
  bool initialized_ = false;                    ///< 初期化済みフラグ

  std::vector<Slot> slots_;                     ///< ライトスロット配列

  std::array<int, kMaxActive> active_{};        ///< アクティブハンドル配列
  int activeCount_ = 0;                         ///< 現在のアクティブ数

  /// @brief 定数バッファ。値は CPU 側（mapped_ が指す）に持ち、アドレスを求められたときに今フレームの領域へ送る
  /// @details 以前は Map しっぱなしの固定 CB だった（CPU と GPU を並行させると上書き競合が起きる）。
  DynamicCB<::SpotLightsCB> dyn_;
  bool hasCB_ = false;                          ///< CB を使える状態か（デバイス初期化後）
  ::SpotLightsCB *mapped_ = nullptr;                       ///< 書き込み先（= dyn_.Ptr()）
};

} // namespace RC
