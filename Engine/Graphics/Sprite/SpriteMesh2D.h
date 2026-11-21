#pragma once
#include <cstdint>
#include <d3d12.h>
#include <wrl/client.h>

#include "Math/Math.h"
#include "function/function.h"

/// @struct SpriteVertex
/// @brief スプライト描画に使用する頂点データ構造体
struct SpriteVertex {
  RC::Vector4 position; ///< 座標（0〜1 の単位クアッド空間）
  RC::Vector2 texcoord; ///< テクスチャ座標（0〜1）
};

/// @class SpriteMesh2D
/// @brief 全てのスプライトで共有される四角形メッシュ（頂点／インデックスバッファ）を管理するクラス
/// @details 0.0 から 1.0 の範囲の単位矩形を 1 つだけ生成し、GPU リソースとして保持する。
///          各 Sprite2D はこのメッシュをバインドし、定数バッファによる座標変換で任意のサイズ・位置に描画する。
class SpriteMesh2D {
public:
  /// @brief コンストラクタ（GPU リソースは Initialize で作る）
  SpriteMesh2D() = default;
  /// @brief デストラクタ（GPU リソースを解放する）
  ~SpriteMesh2D();

  /// @brief メッシュリソースを生成・初期化する
  /// @param device D3D12 デバイス
  /// @return 成功すれば true
  bool Initialize(ID3D12Device *device);

  /// @brief リソースの準備が完了しているか
  /// @return 完了なら true
  bool Ready() const { return vb_ && ib_; }

  /// @brief 頂点バッファビューを取得する
  /// @return 頂点バッファビュー
  const D3D12_VERTEX_BUFFER_VIEW &VBV() const { return vbv_; }

  /// @brief インデックスバッファビューを取得する
  /// @return インデックスバッファビュー
  const D3D12_INDEX_BUFFER_VIEW &IBV() const { return ibv_; }

  /// @brief 描画に必要なインデックス数を取得する
  /// @return インデックス数（常に 6）
  uint32_t IndexCount() const { return 6; }

private:
  /// @brief リソースを解放する
  void Release_();

private:
  Microsoft::WRL::ComPtr<ID3D12Device> device_; ///< D3D12 デバイス

  Microsoft::WRL::ComPtr<ID3D12Resource> vb_; ///< 頂点バッファリソース
  Microsoft::WRL::ComPtr<ID3D12Resource> ib_; ///< インデックスバッファリソース

  D3D12_VERTEX_BUFFER_VIEW vbv_{}; ///< 頂点バッファビュー
  D3D12_INDEX_BUFFER_VIEW ibv_{};  ///< インデックスバッファビュー
};
