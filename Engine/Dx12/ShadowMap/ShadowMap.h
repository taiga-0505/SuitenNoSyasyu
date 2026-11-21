#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include "../DescriptorHeap/DescriptorHeap.h"

class Dx12Core;

/// @class ShadowMap
/// @brief シャドウマップ（深度テクスチャ）を生成・管理するクラス
class ShadowMap {
public:
  /// @brief コンストラクタ（リソースの生成は Create で行う）
  ShadowMap();
  /// @brief デストラクタ（Term を呼んで解放する）
  ~ShadowMap();

  /// @brief シャドウマップを初期化する（深度テクスチャ・専用の DSV ヒープ・SRV を作る）
  /// @param core   Dx12Core のインスタンス
  /// @param width  テクスチャ幅
  /// @param height テクスチャ高さ
  /// @note リソースの生成に失敗した場合は std::runtime_error を投げる。
  void Create(Dx12Core* core, uint32_t width, uint32_t height);

  /// @brief リソースを深度書き込み状態へ遷移し、DSV をセットしてクリアする（ビューポート・シザーも設定する）
  /// @param cmd コマンドリスト
  void BindAndClear(ID3D12GraphicsCommandList* cmd);

  /// @brief シャドウマップ描画後に、リソースを SRV 状態へ遷移させる
  /// @param cmd コマンドリスト
  void TransitionToSRV(ID3D12GraphicsCommandList* cmd);

  /// @brief リソースを破棄し、確保した SRV を返却する
  void Term();

  /// @brief テクスチャリソースを取得する
  /// @return 深度テクスチャのリソース
  ID3D12Resource* GetResource() const { return resource_.Get(); }

  /// @brief SRV ヒープ上でのインデックスを取得する
  /// @return SRV のインデックス
  uint32_t GetSrvIndex() const { return srvIndex_; }

  /// @brief テクスチャ幅を取得する
  /// @return 幅
  uint32_t GetWidth() const { return width_; }
  /// @brief テクスチャ高さを取得する
  /// @return 高さ
  uint32_t GetHeight() const { return height_; }

private:
  Microsoft::WRL::ComPtr<ID3D12Resource> resource_; ///< 深度テクスチャ（R32_TYPELESS）
  DescriptorHeap dsvHeap_;    ///< シャドウマップ専用の DSV ヒープ（1 枠）
  uint32_t srvIndex_ = 0;     ///< SRVManager 上の SRV インデックス（0 なら未確保扱い）
  Dx12Core* core_ = nullptr;  ///< 生成に使った Dx12Core（非所有）
  uint32_t width_ = 0;        ///< テクスチャ幅
  uint32_t height_ = 0;       ///< テクスチャ高さ
};
