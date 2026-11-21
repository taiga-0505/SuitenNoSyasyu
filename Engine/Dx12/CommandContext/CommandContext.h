#pragma once
#include <array>
#include <cassert>
#include <cstdint>
#include <d3d12.h>
#include <wrl/client.h>

/// @class CommandContext
/// @brief DirectX12 のコマンド発行と同期を管理するクラス
/// @details コマンドキュー、リスト、アロケータ、およびフェンスによる GPU 同期をラップする。
class CommandContext {
public:
  /// @brief 最大フレーム数（ダブル／トリプルバッファリング用）
  static constexpr uint32_t kMaxFrames = 3;

  /// @brief デフォルトコンストラクタ
  CommandContext() = default;
  /// @brief デストラクタ（Term を呼んで解放する）
  ~CommandContext() { Term(); }

  /// @brief キュー・アロケータ・リスト・フェンスを作成する
  /// @param device     DirectX12 デバイス
  /// @param type       コマンドリストのタイプ（DIRECT, COMPUTE 等）
  /// @param frameCount フレームバッファ数（1〜kMaxFrames 以外なら 2 になる）
  void Init(ID3D12Device *device,
            D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_DIRECT,
            uint32_t frameCount = 2);

  /// @brief 作成したオブジェクトをすべて解放する
  void Term();

  /// @brief フレーム開始処理。同じフレームの前回分の完了を待ち、アロケータのリセットとリストの記録開始を行う
  /// @param frameIndex 現在のバックバッファインデックス
  void BeginFrame(uint32_t frameIndex);

  /// @brief フレーム終了処理。コマンドを実行し、完了待ちフェンスをシグナルする
  void EndFrame();

  /// @brief 指定したフレームの GPU 処理完了を待機する
  /// @param frameIndex 待機対象のフレームインデックス
  /// @note まだ一度もコマンドを投げていないフレームなら何もしない。
  void WaitForFrame(uint32_t frameIndex);

  /// @brief GPU の全命令完了を待機する（Flush）
  void FlushGPU();

  /// @brief フェンスの現在の完了値を取得する
  /// @return 完了済みのフェンス値
  uint64_t GetCompletedFenceValue() const;

  /// @brief 次にシグナルされるフェンス値を取得する
  /// @return 現在記録中のコマンドが完了したときに到達するフェンス値
  /// @note 「今フレームまでに参照したリソース」を DeferredReleaseQueue に
  ///       登録する際のフェンス値として使う（UINT64_MAX のように終了時まで
  ///       溜め込まず、GPU が使い終わった時点で解放される）。
  uint64_t GetNextFenceValue() const { return globalFenceValue_ + 1; }

  /// @brief 現在のコマンドを実行し、リストをリセットして次の記録を即座に開始する
  /// @return 実行完了時に期待されるフェンス値
  /// @note 実行完了まで待ってから 0 番のアロケータでリセットする（同期実行）。
  uint64_t ExecuteAndReset();

  /// @brief リソースのバリア（トランジション）を発行する
  /// @param res    対象リソース
  /// @param before 遷移前の状態
  /// @param after  遷移後の状態
  void Transition(ID3D12Resource *res, D3D12_RESOURCE_STATES before,
                  D3D12_RESOURCE_STATES after);

  /// @brief コマンドキューを取得する
  /// @return ID3D12CommandQueue へのポインタ
  ID3D12CommandQueue *Queue() const { return queue_.Get(); }

  /// @brief コマンドリストを取得する
  /// @return ID3D12GraphicsCommandList へのポインタ
  ID3D12GraphicsCommandList *List() const { return list_.Get(); }

  /// @brief 指定インデックスのコマンドアロケータを取得する
  /// @param index フレームインデックス
  /// @return ID3D12CommandAllocator へのポインタ
  ID3D12CommandAllocator *GetAllocator(uint32_t index) const { return alloc_[index].Get(); }

  /// @brief フレームバッファ数を取得する
  /// @return フレーム数
  uint32_t FrameCount() const { return frameCount_; }

private:
  Microsoft::WRL::ComPtr<ID3D12Device> device_;       ///< デバイス
  D3D12_COMMAND_LIST_TYPE type_ = D3D12_COMMAND_LIST_TYPE_DIRECT; ///< リストタイプ

  Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;  ///< コマンドキュー
  std::array<Microsoft::WRL::ComPtr<ID3D12CommandAllocator>, kMaxFrames> alloc_{}; ///< アロケータ配列
  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list_; ///< コマンドリスト

  Microsoft::WRL::ComPtr<ID3D12Fence> fence_;  ///< フェンス
  HANDLE fenceEvent_ = nullptr;                ///< フェンス用イベント
  uint64_t globalFenceValue_ = 0;              ///< 次にシグナルするフェンス値
  std::array<uint64_t, kMaxFrames> frameFenceValue_{}; ///< フレームごとの完了フェンス値（0 なら未発行）

  uint32_t frameCount_ = 2;       ///< フレームバッファ数
  uint32_t currentFrameIndex_ = 0; ///< 現在の記録フレームインデックス
};
