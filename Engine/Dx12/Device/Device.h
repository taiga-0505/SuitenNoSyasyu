#pragma once
#include <cassert>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <string>
#include <wrl/client.h>

/// @class Device
/// @brief DirectX12 デバイスと DXGI 関連リソースを管理するクラス
/// @details グラフィックスカードの列挙、デバイスの生成、デバッグレイヤーの設定などを行う。
class Device {
public:
  /// @brief ファクトリ・アダプタ・デバイスを生成する
  /// @param enableDebug   D3D12 デバッグレイヤーを有効にするか（Debug ビルドのみ有効）
  /// @param gpuValidation GPU バリデーション（実行時負荷が高い）を有効にするか
  /// @note ハードウェアアダプタが見つからなければ WARP アダプタを使う。
  ///       機能レベルは 12.2 → 12.1 → 12.0 の順に試す。
  void Init(bool enableDebug = true, bool gpuValidation = false);

  /// @brief デバイス・アダプタ・ファクトリを解放する
  void Term();

  /// @brief DXGI ファクトリを取得する
  /// @return IDXGIFactory6 へのポインタ
  IDXGIFactory6 *Factory() const { return factory_.Get(); }

  /// @brief DXGI アダプタを取得する
  /// @return IDXGIAdapter4 へのポインタ
  IDXGIAdapter4 *Adapter() const { return adapter_.Get(); }

  /// @brief D3D12 デバイスを取得する
  /// @return ID3D12Device へのポインタ
  ID3D12Device *GetDevice() const { return device_.Get(); }

  /// @brief 機能レベル（Feature Level）を取得する
  /// @return 決定された機能レベル
  D3D_FEATURE_LEVEL FeatureLevel() const { return featureLevel_; }

  /// @brief 機能レベルを文字列として取得する（例: "12.1"）
  /// @return 機能レベルの文字列（不明なら "Unknown"）
  const char *FeatureLevelString() const;

  /// @brief テアリング（可変リフレッシュレート）がサポートされているか確認する
  /// @return サポートされていれば true
  bool IsTearingSupported() const;

  /// @brief InfoQueue を設定する（Debug ビルド時のみ有効）
  /// @param breakOnError エラー発生時にプログラムを中断するか
  /// @param muteInfo     情報ログを抑制するか
  /// @note D3D12 のメッセージは Log::Print にも転送する（表示するレベルは Device.cpp の定数で切り替える）。
  void SetupInfoQueue(bool breakOnError = true,
                      bool muteInfo = true);

private:
  /// @brief デバッグレイヤーと GPU バリデーションを有効化する（Debug ビルドのみ）
  /// @param gpuValidation GPU バリデーションも有効にするか
  void enableDebug_(bool gpuValidation);
  /// @brief 高性能なハードウェアアダプタを選択する
  /// @return 機能レベル 12.0 以上でデバイスを作れるアダプタ（見つからなければ nullptr）
  IDXGIAdapter4 *pickHardwareAdapter_();
  /// @brief WARP アダプタ（ソフトウェアレンダリング）を取得する
  /// @return WARP アダプタ（取得できなければ nullptr）
  IDXGIAdapter4 *getWarpAdapter_();

private:
  Microsoft::WRL::ComPtr<IDXGIFactory6> factory_; ///< DXGI ファクトリ
  Microsoft::WRL::ComPtr<IDXGIAdapter4> adapter_; ///< DXGI アダプタ
  Microsoft::WRL::ComPtr<ID3D12Device> device_;   ///< D3D12 デバイス
  D3D_FEATURE_LEVEL featureLevel_ = (D3D_FEATURE_LEVEL)0; ///< 決定された機能レベル
};
