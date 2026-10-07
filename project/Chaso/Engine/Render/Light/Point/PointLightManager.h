#pragma once
#include "Render/FrameResource.h" // RC::DynamicCB
#include "Light/Point/PointLightSource.h"
#include "struct.h"
#include <array>
#include <d3d12.h>
#include <vector>
#include <wrl/client.h>

namespace RC {

/// @brief 点光源(Point Light)を管理するマネージャクラス
/// 最大 kMaxPointLights 個のアクティブな点光源を管理し、一括して GPU (b3スロット)へ転送します。
class PointLightManager {
public:
  /// @brief 同時に有効化可能な点光源の最大数
  /// @details struct.h の kMaxPointLights（= HLSL の MAX_POINT_LIGHTS）と同じ値にする
  static constexpr int kMaxActive = static_cast<int>(kMaxPointLights);

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
  PointLightSource *Get(int handle);

  /// @brief ハンドルからライトの実体を取得 (const)
  /// @param handle ライトハンドル
  /// @return ライトソースへのconstポインタ。無効なハンドルの場合は nullptr
  const PointLightSource *Get(int handle) const;

  /// @brief アクティブリストの先頭にあるライトの実体を取得
  /// アクティブがない場合はデフォルト（ハンドル0）のライトを返しようと試みます。
  /// @return アクティブなライトソースへのポインタ
  PointLightSource *GetActive();

  /// @brief アクティブリストの先頭にあるライトの実体を取得 (const)
  /// @return アクティブなライトソースへのconstポインタ
  const PointLightSource *GetActive() const;

  /// @brief GPU転送用定数バッファ(PointLightsCB / b3)のGPU仮想アドレスを取得
  /// @details 同期（SyncCB）は行わない。描画パスごとに RenderContext が SyncCB() を
  ///          1 回呼ぶので、ドローごとにこの関数を呼んでもコストは掛からない。
  /// @return GPU上の仮想アドレス
  D3D12_GPU_VIRTUAL_ADDRESS GetCBAddress();

  /// @brief CPU側のライト状態を GPU 定数バッファへ転送する
  /// @details 以前はドローごとに GetCBAddress() 内で全灯分（約12KB）を毎回書き直していた。
  ///          今は RenderContext が Execute3DCommands の先頭で 1 回だけ呼ぶ
  ///          （GPU が読むのはコマンドリスト実行時＝最後に書いた内容なので結果は同じ）。
  void SyncCB();

private:
  /// @brief ライト管理用のスロット構造体
  struct Slot {
    bool inUse = false;            ///< 使用中フラグ
    PointLightSource light{};      ///< ライトソース実体
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
  /// アクティブリストに含まれるライトのみをパッキングしてGPUへ送ります。
  void SyncCB_();

private:
  Microsoft::WRL::ComPtr<ID3D12Device> device_; ///< デバイス保持
  bool initialized_ = false;                    ///< 初期化済みフラグ

  std::vector<Slot> slots_;                     ///< ライトスロット配列

  std::array<int, kMaxActive> active_{};        ///< アクティブハンドル配列
  int activeCount_ = 0;                         ///< 現在のアクティブ数

  /// @brief 定数バッファ。値は CPU 側（mapped_ が指す）に持ち、アドレスを求められたときに今フレームの領域へ送る
  /// @details 以前は Map しっぱなしの固定 CB だった（CPU と GPU を並行させると上書き競合が起きる）
  DynamicCB<::PointLightsCB> dyn_;
  bool hasCB_ = false;                          ///< CB を使える状態か（デバイス初期化後）
  ::PointLightsCB *mapped_ = nullptr;                       ///< 書き込み先（= dyn_.Ptr()）
};

} // namespace RC
