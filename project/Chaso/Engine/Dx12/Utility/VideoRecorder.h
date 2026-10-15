#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <string>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <vector>

/// @class VideoRecorder
/// @brief Media Foundation を利用した画面録画クラス
/// @details バックバッファを読み戻し用バッファへコピーし、H.264（MP4）として ../media/video/ に書き出す。
class VideoRecorder {
public:
    /// @brief コンストラクタ（録画の準備は Start で行う）
    VideoRecorder();
    /// @brief デストラクタ（録画中なら停止して保存する）
    ~VideoRecorder();

    /// @brief 録画を開始する
    /// @param device D3D12 デバイス
    /// @param queue  コマンドキュー
    /// @param width  解像度幅
    /// @param height 解像度高さ
    /// @param fps    録画フレームレート
    /// @param format バックバッファのフォーマット
    /// @return 成功したら true（録画中なら false）
    bool Start(ID3D12Device* device, ID3D12CommandQueue* queue, UINT width, UINT height, UINT fps, DXGI_FORMAT format);

    /// @brief 録画を停止し、ファイルを確定する（録画中でなければ何もしない）
    void Stop();

    /// @brief 毎フレームのキャプチャを更新・実行する
    /// @param backBuffer 現在のバックバッファ（PRESENT 状態であること）
    /// @note 読み戻し用バッファが使用中なら、その完了を待ってから書き出す。
    void Update(ID3D12Resource* backBuffer);

    /// @brief 現在録画中かどうか
    /// @return 録画中なら true
    bool IsRecording() const { return isRecording_; }

private:
    /// @brief Media Foundation を初期化する
    /// @return 成功したら true
    bool InitializeMF();
    /// @brief Media Foundation を終了する
    void TerminateMF();
    /// @brief 出力ファイル用の SinkWriter を作り、入出力のメディアタイプを設定する
    /// @param path   出力ファイルのパス
    /// @param width  解像度幅
    /// @param height 解像度高さ
    /// @param fps    フレームレート
    /// @return 成功したら true
    bool SetupSinkWriter(const std::wstring& path, UINT width, UINT height, UINT fps);
    /// @brief 使用中の読み戻し用バッファをすべて書き出す（完了を待つ）
    void FlushBuffers();

    bool isRecording_ = false; ///< 録画中か
    std::string latestPath_; ///< 録画中ファイルの保存先（停止時にログへ出す）

    Microsoft::WRL::ComPtr<ID3D12Device> device_;      ///< デバイス
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_; ///< コピーコマンドを投げるキュー

    // Media Foundation 関連
    Microsoft::WRL::ComPtr<IMFSinkWriter> sinkWriter_; ///< 動画ファイルの書き出し先
    DWORD streamIndex_ = 0;  ///< 映像ストリームのインデックス
    UINT64 frameCount_ = 0;  ///< 書き出したフレーム数
    UINT videoFps_ = 60;     ///< 録画フレームレート
    UINT videoWidth_ = 0;    ///< 録画の幅
    UINT videoHeight_ = 0;   ///< 録画の高さ
    DXGI_FORMAT videoFormat_ = DXGI_FORMAT_UNKNOWN; ///< バックバッファのフォーマット

    // 読み戻し（Readback）バッファ（GPU のコピー完了を待たずに次のフレームへ進むため複数持つ）
    static constexpr UINT kBufferCount = 3; ///< 読み戻し用バッファの数
    /// @brief 読み戻し用バッファ 1 枚分の資源
    struct ReadbackBuffer {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource; ///< READBACK ヒープ上のバッファ
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;       ///< コピー完了を知るフェンス
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator; ///< コマンドアロケータ（バッファごとに専用。GPU 実行中の Reset を防ぐ）
        HANDLE fenceEvent = nullptr; ///< フェンス待ち用イベント
        UINT64 fenceValue = 0;       ///< 最後にシグナルしたフェンス値
        bool inUse = false;          ///< コピーを投げて、まだ書き出していないか
    };
    std::vector<ReadbackBuffer> readbackBuffers_; ///< 読み戻し用バッファ
    UINT currentBufferIndex_ = 0; ///< 次に使う読み戻し用バッファの番号

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint_{}; ///< バックバッファをコピーするときのレイアウト
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList_; ///< コピー用コマンドリスト

    /// @brief 読み戻し用バッファのコピー完了を待ち、内容を 1 フレームとして書き出す
    /// @param buffer 対象のバッファ
    void ProcessReadbackBuffer(ReadbackBuffer& buffer);
    /// @brief ピクセルデータを 1 フレーム分のサンプルとして SinkWriter へ書き出す
    /// @param data     ピクセルデータの先頭
    /// @param rowPitch 1 行あたりのバイト数
    /// @note R8G8B8A8 の場合は ARGB32（BGRA の並び）へ並べ替えてから書き出す。
    void WriteFrame(const void* data, UINT rowPitch);
};
