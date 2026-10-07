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

/// @brief DirectX12の主要コンポーネント（デバイス、コマンド、スワップチェーン、記述子ヒープ等）を統括管理するクラス
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
    bool gpuValidation = false; ///< GPUバリデーションを有効にするか
    UINT srvHeapCapacity = 256; ///< SRV用ヒープの最大容量
    bool allowTearingIfSupported = true; ///< 可変リフレッシュレート(Tearing)を許可するか
  };

  /// @brief 初期化
  /// @param hwnd ウィンドウハンドル
  /// @param d 初期化設定
  void Init(HWND hwnd, const Desc &d);

  /// @brief 終了処理
  void Term();

  /// @brief ウィンドウリサイズ対応
  void Resize(UINT width, UINT height);

  /// @brief FPS固定機能の有効/無効切り替え
  /// @param enable 有効にするならtrue
  void EnableFixFps(bool enable = true);

  /// @brief 目標FPSを設定
  void SetTargetFps(float fps);

  /// @brief 現在の目標FPSを取得
  float GetTargetFps() const { return targetFps_; }

  /// @brief フレーム開始処理
  /// コマンドアロケータやリストのリセット、バックバッファの取得を行います。
  void BeginFrame();

  /// @brief フレーム終了処理
  /// コマンドのクローズ、実行、Present、およびGPU同期（フェンス発行）を行います。
  void EndFrame();

  /// @brief GPUの処理完了を待機する (Flush)
  void WaitForGPU();

  /// @brief 現在のバックバッファインデックスを取得
  /// @return 0 ~ (frameCount-1)
  UINT BackBufferIndex() const { return backIndex_; }

  /// @brief D3D12デバイスを取得
  /// @return ID3D12Deviceへのポインタ
  ID3D12Device *GetDevice() const { return device_.GetDevice(); }

  /// @brief 現在のフレームのグラフィックスコマンドリストを取得
  /// @return ID3D12GraphicsCommandListへのポインタ
  ID3D12GraphicsCommandList *CL() const { return cmd_.List(); }

  /// @brief グラフィックスコマンドキューを取得
  /// @return ID3D12CommandQueueへのポインタ
  ID3D12CommandQueue *Queue() const { return cmd_.Queue(); }

  /// @brief SRV用デスクリプタヒープを取得
  /// @return DescriptorHeapへの参照
  DescriptorHeap &SRV() { return srv_; }

  /// @brief RTV用デスクリプタヒープを取得
  /// @return DescriptorHeapへの参照
  DescriptorHeap &RTV() { return rtv_; }

  /// @brief DSV用デスクリプタヒープを取得
  /// @return DescriptorHeapへの参照
  DescriptorHeap &DSV() { return dsv_; }

  /// @brief 構造化バッファ管理クラスを取得
  /// @return StructuredBufferManagerへの参照
  StructuredBufferManager &StructuredBuffers() { return sbMgr_; }

  /// @brief 現在のレンダーターゲットのCPUハンドルを取得
  /// @return D3D12_CPU_DESCRIPTOR_HANDLE
  D3D12_CPU_DESCRIPTOR_HANDLE CurrentRTV() const {
    return swap_.RtvAt(backIndex_);
  }

  /// @brief 深度ステンシルバッファのCPUハンドルを取得
  /// @return D3D12_CPU_DESCRIPTOR_HANDLE
  D3D12_CPU_DESCRIPTOR_HANDLE Dsv() const { return depth_.Dsv(); }

  /// @brief 深度ステンシルリソースを取得
  /// @return ID3D12Resourceへのポインタ
  ID3D12Resource* GetDepthResource() const { return depth_.Resource(); }

  /// @brief SRV管理クラスを取得
  /// @return SRVManagerへの参照
  SRVManager &SRVMan() { return srvMgr_; }

  /// @brief SRV管理クラスを取得 (const)
  /// @return SRVManagerへのconst参照
  const SRVManager &SRVMan() const { return srvMgr_; }

  /// @brief 遅延解放キューを取得
  /// @return DeferredReleaseQueueへの参照
  DeferredReleaseQueue &DeferredRelease() { return deferredRelease_; }

  /// @brief 現在の完了済みフェンス値を取得
  /// @return 完了済みフェンス値
  uint64_t GetCompletedFenceValue() const { return cmd_.GetCompletedFenceValue(); }

  /// @brief 次にシグナルされるフェンス値を取得（遅延解放の登録用）
  /// @return 現在記録中のコマンドが完了したときに到達するフェンス値
  uint64_t GetNextFenceValue() const { return cmd_.GetNextFenceValue(); }

  /// @brief 画面をクリアする
  /// @param r 赤
  /// @param g 緑
  /// @param b 青
  /// @param a アルファ
  void Clear(float r = 0.1f, float g = 0.25f, float b = 0.5f, float a = 1.0f);

  /// @brief フレームバッファ数を取得
  /// @return バックバッファの数
  UINT FrameCount() const { return swap_.FrameCount(); }

  /// @brief スクリーンショット撮影をリクエストする
  void RequestScreenshot() { requestScreenshot_ = true; }

  /// @brief 最新のスクリーンショットのパスを取得
  const std::string &GetLatestScreenshotPath() const { return latestScreenshotPath_; }
  /// @brief スクリーンショットのパスをクリア
  void ClearLatestScreenshotPath() { latestScreenshotPath_.clear(); }

  /// @brief ビデオ録画を開始する
  void StartRecording();
  /// @brief ビデオ録画を停止する
  void StopRecording();
  /// @brief VideoRecorder のインスタンスを取得する
  VideoRecorder& GetVideoRecorder() { return videoRecorder_; }

  // ── フレーム時間の内訳（CPU と GPU のどちらが重いかを切り分ける用） ──

  /// @brief 前フレームの GPU 実行時間 (ms)。BeginFrame〜EndFrame のコマンドを GPU が処理した時間
  /// @details タイムスタンプクエリで計測。取得できない環境では 0。
  float GpuFrameMs() const { return gpuFrameMs_; }
  /// @brief 前フレームの CPU 時間 (ms)。前フレームの EndFrame 終了から今フレームの Present 直前まで
  /// @details Update・描画コマンドの記録を含み、GPU 待ち・VSync 待ち・録画は含まない。
  float CpuFrameMs() const { return cpuFrameMs_; }
  /// @brief 前フレームの録画処理（VideoRecorder::Update）にかかった時間 (ms)
  float CaptureMs() const { return captureMs_; }

  /// @brief ビューポートを設定
  /// @param vp ビューポート設定
  void SetViewport(const D3D12_VIEWPORT &vp) { viewport_ = vp; }

  /// @brief シザー矩形を設定
  /// @param rc 矩形設定
  void SetScissor(const D3D12_RECT &rc) { scissor_ = rc; }

  /// @brief 現在のビューポートを取得
  /// @return D3D12_VIEWPORTへの参照
  const D3D12_VIEWPORT &Viewport() const { return viewport_; }

  /// @brief 現在のシザー矩形を取得
  /// @return D3D12_RECTへの参照
  const D3D12_RECT &Scissor() const { return scissor_; }

  /// @brief ビューポートとシザーを現在のバックバッファサイズに合わせてリセットする
  /// @param width 幅
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
  SRVManager srvMgr_;        ///< SRV管理
  DepthStencil depth_;       ///< 深度ステンシルバッファ
  UINT backIndex_ = 0;       ///< 現在のバックバッファインデックス
  bool allowTearing_ = false; ///< テアリング許可フラグ
  D3D12_VIEWPORT viewport_{}; ///< 現在のビューポート
  D3D12_RECT scissor_{};      ///< 現在のシザー矩形
  std::unique_ptr<FixFps> fixFps_; ///< FPS固定管理
  bool fixFpsEnabled_ = true;      ///< FPS固定有効フラグ
  StructuredBufferManager sbMgr_;  ///< 構造化バッファ管理
  DeferredReleaseQueue deferredRelease_; ///< GPU リソース遅延解放キュー
  bool requestScreenshot_ = false; ///< スクリーンショット撮影要求フラグ
  std::string latestScreenshotPath_; ///< 最新のスクリーンショットパス
  VideoRecorder videoRecorder_;    ///< ビデオ録画管理
  float targetFps_ = 60.0f;        ///< 目標FPS

  // ── フレーム時間の計測 ──
  /// @brief タイムスタンプクエリを用意する（失敗しても動作は続ける）
  void InitFrameTimer_();
  Microsoft::WRL::ComPtr<ID3D12QueryHeap> timestampHeap_;  ///< [0]=フレーム先頭, [1]=フレーム末尾
  Microsoft::WRL::ComPtr<ID3D12Resource> timestampReadback_; ///< 解決先（READBACK ヒープ、uint64 × 2）
  uint64_t timestampFrequency_ = 0;  ///< GPU タイムスタンプの周波数 (Hz)
  bool timestampPending_ = false;    ///< 解決済みで読み出し待ちの値があるか
  float gpuFrameMs_ = 0.0f;          ///< 前フレームの GPU 時間
  float cpuFrameMs_ = 0.0f;          ///< 前フレームの CPU 時間
  float captureMs_ = 0.0f;           ///< 前フレームの録画処理時間
  std::chrono::steady_clock::time_point cpuFrameStart_{}; ///< CPU 時間の計測開始点
};
