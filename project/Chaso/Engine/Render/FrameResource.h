#pragma once

// ============================================================================
// FrameResource
// ----------------------------------------------------------------------------
// フレームごとの動的アップロードバッファ管理（リニアアロケータ）。
// GPU がフレーム N を実行中に CPU がフレーム N+1 のデータを書き込んでも
// 競合しないように、フレームごとに別々のバッファ領域を使う。
//
// 使い方:
//   1. フレーム開始時に Reset() で書き込みカーソルを先頭に戻す
//   2. 描画中は AllocCB / AllocSRV で領域を確保→書き込み
//   3. GPU はフレーム完了後に Release される
//
// RenderContext が kFrameCount 個のインスタンスをラウンドロビンで使う。
// ============================================================================

#include <cstdint>
#include <cstring>
#include <d3d12.h>
#include <wrl/client.h>

namespace RC {

/// @class FrameResource
/// @brief フレームごとの動的アップロードバッファを管理するクラス（リニアアロケータ）
/// @details GPU がフレーム N を実行中に CPU がフレーム N+1 のデータを書き込んでも競合しないように、
/// フレームごとに独立したバッファ領域を提供します。
class FrameResource {
public:
  /// @brief トリプルバッファリングに対応する最大フレーム数
  static constexpr uint32_t kFrameCount = 3;

  FrameResource() = default;
  ~FrameResource() = default;

  /// @brief リソースの初期化
  /// @param device D3D12 デバイス
  /// @param frameIndex フレームインデックス（デバッグ名用）
  void Init(ID3D12Device *device, uint32_t frameIndex);

  /// @brief リソースの解放
  void Term();

  /// @brief フレーム開始時に呼び出し、書き込みカーソルを先頭にリセットする
  void Reset();

  /// @brief 定数バッファ (CB) 用のメモリを確保する
  /// @param sizeBytes 必要なバイト数
  /// @param outMapped [out] CPU側の書き込み先ポインタを受け取る変数
  /// @return GPU 仮想アドレス（SetGraphicsRootConstantBufferView 等に使用）
  /// @note 確保される領域は 256 バイトアライメントされます。
  D3D12_GPU_VIRTUAL_ADDRESS AllocCB(uint32_t sizeBytes, void **outMapped);

  /// @brief SRV / StructuredBuffer 用のメモリを確保する
  /// @param sizeBytes 必要なバイト数
  /// @param outMapped [out] CPU側の書き込み先ポインタを受け取る変数
  /// @return GPU 仮想アドレス（SetGraphicsRootShaderResourceView 等に使用）
  /// @note アライメント制限はありません。
  D3D12_GPU_VIRTUAL_ADDRESS AllocSRV(uint32_t sizeBytes, void **outMapped);

  /// @brief Init 済みか（GPU バッファを持っているか）
  bool IsInitialized() const { return cbMapped_ != nullptr && srvMapped_ != nullptr; }

  /// @brief 定数バッファ用の残り容量があるか確認する
  /// @param sizeBytes 確保したいサイズ
  /// @return 容量が足りていれば true
  bool HasCBSpace(uint32_t sizeBytes) const;

  /// @brief SRV用の残り容量があるか確認する
  /// @param sizeBytes 確保したいサイズ
  /// @return 容量が足りていれば true
  bool HasSRVSpace(uint32_t sizeBytes) const;

private:
  /// @brief CB 容量オーバー時の捨て場（末尾）。1 回の確保はこれ以下（最大の CB でも 64KB）
  static constexpr uint64_t kOverflowScratch = 64u * 1024u;
  bool overflowReported_ = false; ///< 容量オーバーを一度ログに出したか

  /// @brief 256 バイトアライメントに切り上げる
  static constexpr uint32_t Align256(uint32_t s) { return (s + 255u) & ~255u; }

  /// @brief CB ヒープの初期容量（8MB：256byte の Transform CB 約 3.2 万個分）
  /// @details 単体モデルの Draw は 1 回ごとに 256byte を確保する。スポットライト影で
  ///          同じモデルを最大 18 回描くようになったため 1MB → 4MB に拡張し、
  ///          影の上限が kMaxSpotShadows = 32 灯（影 32 + 平行光源影 + メイン ＝ 最大 34 パス）に
  ///          増えたのに合わせて 4MB → 8MB に再拡張。kMaxSpotShadows を増やすならここも比例で増やす。
  /// @details CPU と GPU を並行させるため、毎フレーム書き換える定数バッファ（マテリアル・行列・ライト等）を
  ///          すべてここから取るようにしたので 8MB → 16MB に拡張（DynamicCB 参照）。
  static constexpr uint64_t kDefaultCBSize = 16u * 1024 * 1024;
  Microsoft::WRL::ComPtr<ID3D12Resource> cbHeap_; ///< 定数バッファ用リソース
  uint8_t *cbMapped_ = nullptr;                  ///< マップされた先頭ポインタ
  uint64_t cbOffset_ = 0;                        ///< 現在の書き込みオフセット
  uint64_t cbCapacity_ = 0;                      ///< ヒープの総容量

  /// @brief SRV ヒープの初期容量（32MB：InstanceDataGPU 約 16 万個分）
  /// @details インスタンス描画は 1 回の Draw ごとに count × 208byte を確保する。
  ///          スポットライト影（最大 kMaxSpotShadows = 32 灯）＋平行光源影＋メインで
  ///          同じバッチを最大 34 回描くため、マップの床・壁（20×20 で約 500 個 ≒ 100KB/回）でも
  ///          余裕を持たせて 16MB → 32MB に拡張。kMaxSpotShadows を増やすならここも比例で増やす。
  static constexpr uint64_t kDefaultSRVSize = 32u * 1024 * 1024;
  Microsoft::WRL::ComPtr<ID3D12Resource> srvHeap_; ///< SRV/StructuredBuffer用リソース
  uint8_t *srvMapped_ = nullptr;                   ///< マップされた先頭ポインタ
  uint64_t srvOffset_ = 0;                         ///< 現在の書き込みオフセット
  uint64_t srvCapacity_ = 0;                       ///< ヒープの総容量

  Microsoft::WRL::ComPtr<ID3D12Device> device_;    ///< D3D12 デバイス
  uint32_t frameIndex_ = 0;                        ///< フレームインデックス
};

// ============================================================================
// 現在フレームのアロケータへの入口（RenderContext.cpp で定義）
// ============================================================================

/// @brief 今のフレームのアップロード用アロケータ
/// @details RenderContext::BeginFrame で 1 フレームに 1 回だけ次の 1 枚へ進む。
FrameResource &CurrentFrameResource();

/// @brief フレームの通し番号（RenderContext::BeginFrame のたびに +1）
uint64_t CurrentFrameSerial();

// ============================================================================
// DynamicCB
// ----------------------------------------------------------------------------
// 「CPU が毎フレーム（または時々）書き換え、GPU が読む」定数バッファ。
//
// 以前は Map しっぱなしの固定 CB に直接書いていた。これは CPU が GPU の完了を毎フレーム
// 待っていたから成り立っていた方式で、CPU と GPU を並行させると、GPU が前フレームを
// 描いている最中に CPU が次フレームの値で上書きしてしまう（ちらつき・ずれの原因）。
//
// ここでは値を CPU 側（通常のメモリ）に持ち、バインドする瞬間に今フレームの
// FrameResource へコピーしてそのアドレスを使う。
//   - 同じフレーム内で値が変わっていなければ前回コピーした場所を使い回す（コピーは 1 回）
//   - 値はいつでも Ptr() 経由で読み書きしてよい（アップロードヒープを読む遅さも無くなる）
// ============================================================================
template <class T> class DynamicCB {
public:
  /// @brief CPU 側の値（読み書き自由。バインド時に GPU へ送られる）
  T *Ptr() { return &cpu_; }
  const T *Ptr() const { return &cpu_; }
  T &Value() { return cpu_; }
  const T &Value() const { return cpu_; }

  /// @brief 今の値を今フレームの領域へ送り、その GPU アドレスを返す（コマンド記録中に呼ぶこと）
  /// @note const でも呼べる（送った場所の記録だけが変わる。値そのものは変えない）
  D3D12_GPU_VIRTUAL_ADDRESS Address() const {
    const uint64_t serial = CurrentFrameSerial();
    if (addr_ != 0 && serial_ == serial && std::memcmp(&uploaded_, &cpu_, sizeof(T)) == 0) {
      return addr_;
    }
    FrameResource &frame = CurrentFrameResource();
    if (!frame.IsInitialized()) {
      return 0; // 描画系の初期化前（呼ばれない想定だが、落ちないようにする）
    }
    void *dst = nullptr;
    addr_ = frame.AllocCB(static_cast<uint32_t>(sizeof(T)), &dst);
    if (dst) {
      std::memcpy(dst, &cpu_, sizeof(T));
    }
    std::memcpy(&uploaded_, &cpu_, sizeof(T)); // パディングも含めて写す（memcmp の誤判定を防ぐ）
    serial_ = serial;
    invalid_ = false;
    return addr_;
  }

  /// @brief 大きな CB 向け: 値を書き換えたあとに 1 回呼ぶ（前回送った値と違えば送り直し対象にする）
  void Refresh() {
    if (std::memcmp(&uploaded_, &cpu_, sizeof(T)) != 0) invalid_ = true;
  }

  /// @brief 大きな CB 向け: 比較をせずにアドレスを返す（Refresh 済み、またはフレームが変わったときだけ送る）
  /// @details ドローごとに何度も呼ばれる大きな CB（ライト配列など）で、毎回の比較を省くため。
  D3D12_GPU_VIRTUAL_ADDRESS AddressCached() const {
    const uint64_t serial = CurrentFrameSerial();
    if (addr_ != 0 && serial_ == serial && !invalid_) {
      return addr_;
    }
    return Address();
  }

private:
  T cpu_{};                                    ///< CPU 側の値（正）
  mutable T uploaded_{};                       ///< 最後に送った値（同じなら送り直さない）
  mutable D3D12_GPU_VIRTUAL_ADDRESS addr_ = 0; ///< 最後に送った場所（serial_ のフレームでのみ有効）
  mutable uint64_t serial_ = ~0ull;            ///< 最後に送ったフレームの通し番号
  mutable bool invalid_ = true;                ///< Refresh で変更を検出した（AddressCached で送り直す）
};

/// @brief 値を今フレームの CB 領域へコピーして GPU アドレスを返す（使い捨て）
template <class T> D3D12_GPU_VIRTUAL_ADDRESS UploadFrameCB(const T &value) {
  FrameResource &frame = CurrentFrameResource();
  if (!frame.IsInitialized()) {
    return 0;
  }
  void *dst = nullptr;
  const D3D12_GPU_VIRTUAL_ADDRESS addr = frame.AllocCB(static_cast<uint32_t>(sizeof(T)), &dst);
  if (dst) {
    std::memcpy(dst, &value, sizeof(T));
  }
  return addr;
}

} // namespace RC
