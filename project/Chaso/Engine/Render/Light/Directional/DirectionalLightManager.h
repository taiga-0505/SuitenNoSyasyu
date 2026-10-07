#pragma once
#include "Render/FrameResource.h" // RC::DynamicCB

#include "Light/Directional/DirectionalLightSource.h" // RC::DirectionalLightSource / DirectionalLight

#include <d3d12.h>
#include <vector>
#include <wrl/client.h>

namespace RC {

/// @brief 平行光源(Directional Light)を管理するマネージャクラス
/// ライトをハンドル制で管理し、各ライト用の定数バッファ(Upload CB)の寿命管理と同期を担当します。
class DirectionalLightManager {
public:
  /// @brief デフォルトコンストラクタ
  DirectionalLightManager() = default;
  
  /// @brief デストラクタ。内部リソースを解放します。
  ~DirectionalLightManager() { Term(); }

  // コピー禁止
  DirectionalLightManager(const DirectionalLightManager &) = delete;
  DirectionalLightManager &operator=(const DirectionalLightManager &) = delete;

  /// @brief 初期化処理
  /// @param device DirectX12デバイス。定数バッファの作成に使用します。
  void Init(ID3D12Device *device);

  /// @brief 終了処理。確保した定数バッファなどのリソースをすべて解放します。
  void Term();

  /// @brief 新規ライトを生成する
  /// @return 生成されたライトのハンドル（失敗時は -1）。ハンドル 0 はデフォルトスロットとして予約されます。
  int Create();

  /// @brief 指定したハンドルに対応するライトを破棄する
  /// @param handle ライトハンドル（0 は破棄できません）
  void Destroy(int handle);

  /// @brief シーン内で使用する「アクティブなライト」を設定する
  /// @param handle ライトハンドル。-1 を指定すると明示的なアクティブなし（デフォルトスロットを使用）になります。
  void SetActive(int handle);

  /// @brief 現在設定されている「明示的なアクティブ」ハンドルの取得
  /// @return ライトハンドル。未設定なら -1
  int GetActiveHandle() const { return activeHandle_; }

  /// @brief ハンドルからライトの実体を取得
  /// @param handle ライトハンドル
  /// @return ライトソースへのポインタ。無効なハンドルの場合は nullptr
  DirectionalLightSource *Get(int handle);

  /// @brief ハンドルからライトの実体を取得 (const)
  /// @param handle ライトハンドル
  /// @return ライトソースへのconstポインタ。無効なハンドルの場合は nullptr
  const DirectionalLightSource *Get(int handle) const;

  /// @brief 現在のアクティブなライトの実体を取得
  /// 明示的なアクティブが設定されていない場合は、デフォルトスロット(0)のライトを返します。
  /// @return アクティブなライトソースへのポインタ
  DirectionalLightSource *GetActive();

  /// @brief 現在のアクティブなライトの実体を取得 (const)
  /// @return アクティブなライトソースへのconstポインタ
  const DirectionalLightSource *GetActive() const;

  /// @brief アクティブなライトの定数バッファ(CB)のGPU仮想アドレスを取得
  /// @return GPU上の仮想アドレス。準備できていない場合は 0
  D3D12_GPU_VIRTUAL_ADDRESS GetActiveCBAddress();

  /// @brief 指定したハンドルのライトの定数バッファ(CB)のGPU仮想アドレスを取得
  /// @param handle ライトハンドル
  /// @return GPU上の仮想アドレス。準備できていない場合は 0
  D3D12_GPU_VIRTUAL_ADDRESS GetCBAddress(int handle);

  /// @brief 明示的なアクティブライトが設定されているか確認
  /// @return 設定されていれば true (handle >= 0)
  bool HasExplicitActive() const { return activeHandle_ >= 0; }

  /// @brief デフォルトライトのハンドルを取得
  /// @return 常に 0
  int DefaultHandle() const { return 0; }

private:
  /// @brief ライト管理用のスロット構造体
  struct Slot {
    DirectionalLightSource light; ///< ライトソース実体
    /// @brief 定数バッファ。値は CPU 側に持ち、アドレスを求められたときに今フレームの領域へ送る
    /// @details 以前は Map しっぱなしの固定 CB だった（CPU と GPU を並行させると上書き競合が起きる）
    DynamicCB<DirectionalLight> dyn;
    bool hasCB = false;                       ///< CB を使える状態か（デバイス初期化後）
    bool inUse = false;                       ///< 使用中フラグ
  };

  /// @brief 未使用スロットを検索・確保する
  int AllocSlot_();
  
  /// @brief ハンドルの有効性をチェックする
  bool IsValid_(int handle) const;
  
  /// @brief 最終的なアクティブハンドルを解決する（未設定なら0を返す）
  int ResolveActiveHandle_() const;

  /// @brief スロットのリソースを解放する
  void ReleaseSlot_(Slot &s);
  
  /// @brief スロット用の定数バッファを確保する
  void EnsureCB_(Slot &s);
  
  /// @brief CPU側のデータをGPU定数バッファに同期（コピー）する
  void SyncCB_(Slot &s);

private:
  Microsoft::WRL::ComPtr<ID3D12Device> device_; ///< デバイス保持
  std::vector<Slot> slots_;                    ///< ライトスロット配列

  int activeHandle_ = -1; ///< 現在のアクティブハンドル
  bool initialized_ = false; ///< 初期化済みフラグ
};

} // namespace RC
