#pragma once
#include "Render/FrameResource.h" // RC::DynamicCB

#include "Light/Directional/DirectionalLightSource.h" // RC::DirectionalLightSource / DirectionalLight

#include <d3d12.h>
#include <vector>
#include <wrl/client.h>

namespace RC {

/// @class DirectionalLightManager
/// @brief 平行光源（Directional Light）を管理するマネージャクラス
/// @details ライトをハンドル制で管理し、各ライト用の定数バッファ（DynamicCB）の用意と同期を担当する。
///          ハンドル 0 は既定のライトとして予約され、Destroy では破棄できない。
class DirectionalLightManager {
public:
  /// @brief デフォルトコンストラクタ
  DirectionalLightManager() = default;

  /// @brief デストラクタ。内部リソースを解放する
  ~DirectionalLightManager() { Term(); }

  // コピー禁止
  DirectionalLightManager(const DirectionalLightManager &) = delete;
  DirectionalLightManager &operator=(const DirectionalLightManager &) = delete;

  /// @brief 既定のライト（ハンドル 0）と定数バッファを用意する
  /// @param device D3D12 デバイス（定数バッファの作成に使う）
  /// @note 呼ぶたびに一度 Term してから作り直す。
  void Init(ID3D12Device *device);

  /// @brief 終了処理。ライトと定数バッファなどのリソースをすべて解放する
  void Term();

  /// @brief 新規ライトを生成する
  /// @return 生成されたライトのハンドル（失敗時は -1。ハンドル 0 は既定のライト用に予約されている）
  /// @note 明示的なアクティブが未設定なら、生成したライトを自動でアクティブにする。
  int Create();

  /// @brief 指定したハンドルに対応するライトを破棄する
  /// @param handle ライトハンドル（0 は破棄できない）
  /// @note 破棄したライトがアクティブだった場合は、明示的なアクティブなしに戻る。
  void Destroy(int handle);

  /// @brief シーン内で使う「アクティブなライト」を設定する
  /// @param handle ライトハンドル。負の値を指定すると明示的なアクティブなし（既定のライトを使う）になる。無効なハンドルなら何もしない
  void SetActive(int handle);

  /// @brief 現在設定されている「明示的なアクティブ」ハンドルを取得する
  /// @return ライトハンドル。未設定なら -1
  int GetActiveHandle() const { return activeHandle_; }

  /// @brief ハンドルからライトの実体を取得する
  /// @param handle ライトハンドル
  /// @return ライトソースへのポインタ。無効なハンドルの場合は nullptr
  DirectionalLightSource *Get(int handle);

  /// @brief ハンドルからライトの実体を取得する（const 版）
  /// @param handle ライトハンドル
  /// @return ライトソースへの const ポインタ。無効なハンドルの場合は nullptr
  const DirectionalLightSource *Get(int handle) const;

  /// @brief 現在のアクティブなライトの実体を取得する
  /// @details 明示的なアクティブが設定されていない場合は、既定のライト（ハンドル 0）を返す。
  /// @return アクティブなライトソースへのポインタ（未初期化なら nullptr）
  DirectionalLightSource *GetActive();

  /// @brief 現在のアクティブなライトの実体を取得する（const 版）
  /// @return アクティブなライトソースへの const ポインタ（未初期化なら nullptr）
  const DirectionalLightSource *GetActive() const;

  /// @brief アクティブなライトの定数バッファ（CB）の GPU 仮想アドレスを取得する
  /// @return GPU 仮想アドレス。準備できていない場合は 0
  D3D12_GPU_VIRTUAL_ADDRESS GetActiveCBAddress();

  /// @brief 指定したハンドルのライトの定数バッファ（CB）の GPU 仮想アドレスを取得する
  /// @param handle ライトハンドル
  /// @return GPU 仮想アドレス。準備できていない場合は 0
  /// @note 呼ぶたびにライトの最新値を CB へ書き、今フレームの領域へ送る。
  D3D12_GPU_VIRTUAL_ADDRESS GetCBAddress(int handle);

  /// @brief 明示的なアクティブライトが設定されているか確認する
  /// @return 設定されていれば true（handle >= 0）
  bool HasExplicitActive() const { return activeHandle_ >= 0; }

  /// @brief 既定のライトのハンドルを取得する
  /// @return 常に 0
  int DefaultHandle() const { return 0; }

private:
  /// @struct Slot
  /// @brief ライト管理用のスロット構造体
  struct Slot {
    DirectionalLightSource light; ///< ライトソース実体
    /// @brief 定数バッファ。値は CPU 側に持ち、アドレスを求められたときに今フレームの領域へ送る
    /// @details 以前は Map しっぱなしの固定 CB だった（CPU と GPU を並行させると上書き競合が起きる）。
    DynamicCB<DirectionalLight> dyn;
    bool hasCB = false;                       ///< CB を使える状態か（デバイス初期化後）
    bool inUse = false;                       ///< 使用中フラグ
  };

  /// @brief 未使用スロットを検索・確保する
  /// @return 確保したスロットのインデックス（0 は既定のライト用なので 1 以降から探し、無ければ末尾に追加する）
  int AllocSlot_();

  /// @brief ハンドルの有効性をチェックする
  /// @param handle ライトハンドル
  /// @return 初期化済みで、範囲内かつ使用中のスロットなら true
  bool IsValid_(int handle) const;

  /// @brief 最終的なアクティブハンドルを解決する
  /// @return 明示的なアクティブが有効ならそのハンドル、未設定なら 0、未初期化なら -1
  int ResolveActiveHandle_() const;

  /// @brief スロットの定数バッファを使えない状態に戻す
  /// @param s 対象のスロット
  /// @note DynamicCB は毎フレームの一時領域を使うので、解放する GPU リソースは無い。
  void ReleaseSlot_(Slot &s);

  /// @brief スロット用の定数バッファを使える状態にし、現在のライトの値を書く
  /// @param s 対象のスロット
  void EnsureCB_(Slot &s);

  /// @brief CPU 側のライトの値を定数バッファへ書き込む
  /// @param s 対象のスロット
  void SyncCB_(Slot &s);

private:
  Microsoft::WRL::ComPtr<ID3D12Device> device_; ///< デバイス保持
  std::vector<Slot> slots_;                    ///< ライトスロット配列

  int activeHandle_ = -1; ///< 現在のアクティブハンドル
  bool initialized_ = false; ///< 初期化済みフラグ
};

} // namespace RC
