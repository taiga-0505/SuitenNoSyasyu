#pragma once
#include "CommandContext/CommandContext.h"
#include "DeferredReleaseQueue/DeferredReleaseQueue.h"
#include "DepthStencil/DepthStencil.h"
#include "DescriptorHeap/DescriptorHeap.h"
#include "Device/Device.h"
#include "FixFps/FixFps.h"
#include "GraphicsPipeline/GraphicsPipeline.h"
#include "SRVManager/SRVManager.h"
#include "StructuredBufferManager/StructuredBufferManager.h"
#include "SwapChain/SwapChain.h"
#include "Utility/VideoRecorder.h"
#include <chrono>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <memory>
#include <wrl/client.h>

/// @class Dx12Core
/// @brief DirectX12 の主要コンポーネント（デバイス、コマンド、スワップチェーン、ディスクリプタヒープ等）を統括管理するクラス
class Dx12Core {
public:
  /// @brief 初期化用設定構造体
  struct Desc {
    UINT width = 1280;          ///< 解像度（幅）
    UINT height = 720;          ///< 解像度（高さ）
    DXGI_FORMAT rtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; ///< レンダーターゲットフォーマット
    DXGI_FORMAT dsvFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;   ///< 深度ステンシルフォーマット
    UINT frameCount = 2;        ///< スワップチェーンのバッファ数
    bool debug = true;          ///< デバッグレイヤーを有効にするか
    bool gpuValidation = false; ///< GPU バリデーションを有効にするか
    UINT srvHeapCapacity = 256; ///< SRV 用ヒープの最大容量
    bool allowTearingIfSupported = true; ///< 可変リフレッシュレート（テアリング）を許可するか
  };

  /// @brief デバイス・コマンド・ヒープ・スワップチェーン・深度バッファなどを初期化する
  /// @param hwnd ウィンドウハンドル
  /// @param d    初期化設定
  void Init(HWND hwnd, const Desc &d);

  /// @brief GPU の完了を待ってから、すべてのリソースを解放する
  void Term();

  /// @brief ウィンドウサイズの変更に合わせてバックバッファと深度バッファを作り直す
  /// @param width  新しい幅
  /// @param height 新しい高さ
  /// @note サイズが変わっていなければ何もしない。作り直す前に GPU の完了を待つ。
  void Resize(UINT width, UINT height);

  /// @brief FPS 固定機能の有効／無効を切り替える
  /// @param enable 有効にするなら true
  void EnableFixFps(bool enable = true);

  /// @brief 目標 FPS を設定する
  /// @param fps 目標 FPS
  void SetTargetFps(float fps);

  /// @brief 現在の目標 FPS を取得する
  /// @return 目標 FPS
  float GetTargetFps() const { return targetFps_; }

  /// @brief フレーム開始処理
  /// @details 遅延解放の実行、2 フレーム前の GPU 完了待ち、コマンドのリセット、
  ///          バックバッファの遷移とバインド、画面クリアを行う。
  void BeginFrame();

  /// @brief フレーム終了処理
  /// @details コマンドのクローズと実行、スクリーンショット・録画、Present、FPS 固定の待ちを行う。
  ///          今送ったフレームの GPU 完了は待たない（次の BeginFrame で待つ）。
  void EndFrame();

  /// @brief GPU の処理完了を待機する（Flush）
  void WaitForGPU();

  /// @brief 現在のバックバッファインデックスを取得する
  /// @return 0〜(frameCount - 1)
  UINT BackBufferIndex() const { return backIndex_; }

  /// @brief D3D12 デバイスを取得する
  /// @return ID3D12Device へのポインタ
  ID3D12Device *GetDevice() const { return device_.GetDevice(); }

  /// @brief 現在のフレームのグラフィックスコマンドリストを取得する
  /// @return ID3D12GraphicsCommandList へのポインタ
  ID3D12GraphicsCommandList *CL() const { return cmd_.List(); }

  /// @brief グラフィックスコマンドキューを取得する
  /// @return ID3D12CommandQueue へのポインタ
  ID3D12CommandQueue *Queue() const { return cmd_.Queue(); }

  /// @brief SRV 用ディスクリプタヒープを取得する
  /// @return DescriptorHeap への参照
  DescriptorHeap &SRV() { return srv_; }

  /// @brief RTV 用ディスクリプタヒープを取得する
  /// @return DescriptorHeap への参照
  DescriptorHeap &RTV() { return rtv_; }

  /// @brief DSV 用ディスクリプタヒープを取得する
  /// @return DescriptorHeap への参照
  DescriptorHeap &DSV() { return dsv_; }

  /// @brief 構造化バッファ管理クラスを取得する
  /// @return StructuredBufferManager への参照
  StructuredBufferManager &StructuredBuffers() { return sbMgr_; }

  /// @brief 現在のレンダーターゲットの CPU ハンドルを取得する
  /// @return 現在のバックバッファの RTV ハンドル
  D3D12_CPU_DESCRIPTOR_HANDLE CurrentRTV() const {
    return swap_.RtvAt(backIndex_);
  }

  /// @brief 深度ステンシルバッファの CPU ハンドルを取得する
  /// @return DSV ハンドル
  D3D12_CPU_DESCRIPTOR_HANDLE Dsv() const { return depth_.Dsv(); }

  /// @brief 深度ステンシルリソースを取得する
  /// @return ID3D12Resource へのポインタ
  ID3D12Resource* GetDepthResource() const { return depth_.Resource(); }

  /// @brief SRV 管理クラスを取得する
  /// @return SRVManager への参照
  SRVManager &SRVMan() { return srvMgr_; }

  /// @brief SRV 管理クラスを取得する（const）
  /// @return SRVManager への const 参照
  const SRVManager &SRVMan() const { return srvMgr_; }

  /// @brief 遅延解放キューを取得する
  /// @return DeferredReleaseQueue への参照
  DeferredReleaseQueue &DeferredRelease() { return deferredRelease_; }

  /// @brief 現在の完了済みフェンス値を取得する
  /// @return 完了済みフェンス値
  uint64_t GetCompletedFenceValue() const { return cmd_.GetCompletedFenceValue(); }

  /// @brief 次にシグナルされるフェンス値を取得する（遅延解放の登録用）
  /// @return 現在記録中のコマンドが完了したときに到達するフェンス値
  uint64_t GetNextFenceValue() const { return cmd_.GetNextFenceValue(); }

  /// @brief 現在のレンダーターゲットと深度ステンシルをクリアする
  /// @param r 赤
  /// @param g 緑
  /// @param b 青
  /// @param a アルファ
  void Clear(float r = 0.1f, float g = 0.25f, float b = 0.5f, float a = 1.0f);

  /// @brief フレームバッファ数を取得する
  /// @return バックバッファの数
  UINT FrameCount() const { return swap_.FrameCount(); }

  /// @brief スクリーンショット撮影をリクエストする
  /// @note 次の EndFrame で撮影する。
  void RequestScreenshot() { requestScreenshot_ = true; }

  /// @brief 最新のスクリーンショットのパスを取得する
  /// @return 保存先のパス（未撮影・クリア後は空）
  const std::string &GetLatestScreenshotPath() const { return latestScreenshotPath_; }
  /// @brief スクリーンショットのパスをクリアする
  void ClearLatestScreenshotPath() { latestScreenshotPath_.clear(); }

  /// @brief 指定テクスチャを指定パスへ PNG 保存する撮影をリクエストする
  /// @param path     保存先のパス（親フォルダが無ければ作る）
  /// @param resource 撮影対象（nullptr なら表示中のバックバッファ）
  /// @param state    フレーム終了時点での resource の状態（バックバッファなら PRESENT）
  /// @note 次の EndFrame（Present の直前）で撮影する。クリップボードへはコピーしない。
  void RequestCapture(const std::string &path, ID3D12Resource *resource = nullptr,
                      D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_PRESENT) {
    captureRequest_ = {path, resource, state};
  }

  /// @brief ビデオ録画を開始する（録画中なら何もしない）
  void StartRecording();
  /// @brief ビデオ録画を停止する（録画中でなければ何もしない）
  void StopRecording();
  /// @brief VideoRecorder のインスタンスを取得する
  /// @return VideoRecorder への参照
  VideoRecorder& GetVideoRecorder() { return videoRecorder_; }

  // ── フレーム時間の内訳（CPU と GPU のどちらが重いかを切り分ける用） ──

  /// @brief 前フレームの GPU 実行時間（ms）。BeginFrame〜EndFrame のコマンドを GPU が処理した時間
  /// @details タイムスタンプクエリで計測。取得できない環境では 0。
  /// @return GPU 実行時間（ms）
  float GpuFrameMs() const { return gpuFrameMs_; }
  /// @brief 前フレームの CPU 時間（ms）。前フレームの EndFrame 終了から今フレームの Present 直前まで
  /// @details Update・描画コマンドの記録を含み、GPU 待ち・VSync 待ち・録画は含まない。
  /// @return CPU 時間（ms）
  float CpuFrameMs() const { return cpuFrameMs_; }
  /// @brief 前フレームの録画処理（VideoRecorder::Update）にかかった時間（ms）
  /// @return 録画処理時間（ms）
  float CaptureMs() const { return captureMs_; }
  /// @brief BeginFrame で GPU の完了を待った時間（ms）。大きいほど GPU 側がボトルネック
  /// @return GPU 待ち時間（ms）
  float GpuWaitMs() const { return gpuWaitMs_; }

  /// @brief ビューポートを設定する
  /// @param vp ビューポート設定
  void SetViewport(const D3D12_VIEWPORT &vp) { viewport_ = vp; }

  /// @brief シザー矩形を設定する
  /// @param rc 矩形設定
  void SetScissor(const D3D12_RECT &rc) { scissor_ = rc; }

  /// @brief 現在のビューポートを取得する
  /// @return D3D12_VIEWPORT への参照
  const D3D12_VIEWPORT &Viewport() const { return viewport_; }

  /// @brief 現在のシザー矩形を取得する
  /// @return D3D12_RECT への参照
  const D3D12_RECT &Scissor() const { return scissor_; }

  /// @brief ビューポートとシザーを現在のバックバッファサイズに合わせてリセットする
  /// @param width  幅
  /// @param height 高さ
  void ResetViewportScissorToBackbuffer(UINT width, UINT height) {
    viewport_.TopLeftX = 0.0f;
    viewport_.TopLeftY = 0.0f;
    viewport_.Width = static_cast<float>(width);
    viewport_.Height = static_cast<float>(height);
    viewport_.MinDepth = 0.0f;
    viewport_.MaxDepth = 1.0f;

    scissor_.left = 0;
    scissor_.top = 0;
    scissor_.right = static_cast<LONG>(width);
    scissor_.bottom = static_cast<LONG>(height);
  }

private:
  Desc desc_{};              ///< 初期化設定の保持
  Device device_;            ///< デバイス管理
  CommandContext cmd_;       ///< コマンド管理
  SwapChain swap_;           ///< スワップチェーン管理
  DescriptorHeap rtv_, dsv_, srv_; ///< 各種デスクリプタヒープ
  SRVManager srvMgr_;        ///< SRV 管理
  DepthStencil depth_;       ///< 深度ステンシルバッファ
  UINT backIndex_ = 0;       ///< 現在のバックバッファインデックス
  bool allowTearing_ = false; ///< テアリング許可フラグ
  D3D12_VIEWPORT viewport_{}; ///< 現在のビューポート
  D3D12_RECT scissor_{};      ///< 現在のシザー矩形
  std::unique_ptr<FixFps> fixFps_; ///< FPS 固定管理
  bool fixFpsEnabled_ = true;      ///< FPS 固定有効フラグ
  StructuredBufferManager sbMgr_;  ///< 構造化バッファ管理
  DeferredReleaseQueue deferredRelease_; ///< GPU リソース遅延解放キュー
  bool requestScreenshot_ = false; ///< スクリーンショット撮影要求フラグ
  std::string latestScreenshotPath_; ///< 最新のスクリーンショットパス
  /// @brief RequestCapture で受けた撮影要求（path が空なら要求なし）
  struct CaptureRequest {
    std::string path;                  ///< 保存先
    ID3D12Resource *resource = nullptr; ///< 撮影対象（nullptr ならバックバッファ）
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_PRESENT; ///< 撮影時点の状態
  } captureRequest_;
  VideoRecorder videoRecorder_;    ///< ビデオ録画管理
  float targetFps_ = 60.0f;        ///< 目標 FPS

  // ── フレーム時間の計測 ──
  /// @brief タイムスタンプクエリを用意する（失敗しても動作は続ける）
  void InitFrameTimer_();
  Microsoft::WRL::ComPtr<ID3D12QueryHeap> timestampHeap_;  ///< スロットごとに [先頭, 末尾] のタイムスタンプ
  Microsoft::WRL::ComPtr<ID3D12Resource> timestampReadback_; ///< 解決先（READBACK ヒープ、スロットごとに uint64 × 2）
  uint64_t timestampFrequency_ = 0;  ///< GPU タイムスタンプの周波数（Hz）
  /// @brief 計測スロット数。バックバッファ番号をそのまま使う（同じ番号は BeginFrame の待ちで完了済み）
  static constexpr uint32_t kTimestampSlots = CommandContext::kMaxFrames;
  bool timestampPending_[kTimestampSlots]{};     ///< スロットごとに、解決済みで読み出し待ちの値があるか
  float gpuFrameMs_ = 0.0f;          ///< 前フレームの GPU 時間（ms）
  float cpuFrameMs_ = 0.0f;          ///< 前フレームの CPU 時間（ms）
  float captureMs_ = 0.0f;           ///< 前フレームの録画処理時間（ms）
  float gpuWaitMs_ = 0.0f;           ///< BeginFrame で GPU 完了を待った時間（ms）
  std::chrono::steady_clock::time_point cpuFrameStart_{}; ///< CPU 時間の計測開始点
};
