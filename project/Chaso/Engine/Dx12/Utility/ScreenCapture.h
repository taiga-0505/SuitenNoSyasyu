#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <string>

/// @class ScreenCapture
/// @brief スクリーンショット撮影機能を提供するユーティリティクラス
class ScreenCapture {
public:
    /// @brief バックバッファをキャプチャしてファイルに保存する
    /// @param device     D3D12 デバイス
    /// @param queue      コマンドキュー（GPU との同期用）
    /// @param backBuffer キャプチャ対象のバックバッファリソース（PRESENT 状態であること）
    /// @return 保存されたファイルのパス（失敗した場合は空文字列）
    /// @details ../media/screenshots/ に日時名の PNG で保存し、同じ画像をクリップボードにも DIB でコピーする。
    /// @note コピー完了まで GPU を待つため、呼び出し元はその分だけ止まる。
    static std::string SaveScreenshot(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* backBuffer);

    /// @brief 通知 UI の表示時間（秒）
    static constexpr float kNotifyDisplayTime = 3.0f;

    /// @brief 撮影通知 UI（サムネイル付き）を描画する
    /// @param deltaTime 経過時間（秒）
    /// @param core      撮影結果のパスを持つ Dx12Core（nullptr なら何もしない）
    /// @note RC_ENABLE_IMGUI が無効なビルドでは何もしない。
    static void DrawImGui(float deltaTime, class Dx12Core* core);

private:
    /// @brief 撮影通知の表示状態
    struct NotifyData {
        std::string path;             ///< 表示中のスクリーンショットのパス
        int texHandle = -1;           ///< サムネイル用テクスチャのハンドル（未ロードなら -1）
        float timer = 0.0f;           ///< 表示の残り時間（秒）
        bool active = false;          ///< 表示中か
        int pendingUnloadHandle = -1; ///< 次フレームで解放するテクスチャのハンドル（無ければ -1）
    };
    static NotifyData notify_; ///< 撮影通知の表示状態

    /// @brief スクリーンショット保存用のフォルダを作成する
    static void CreateScreenshotDirectory();

    /// @brief 現在の時刻からファイル名を生成する
    /// @return 「YYYYMMDD_hhmmss」形式のファイル名（拡張子なし）
    static std::string GenerateTimestampFileName();
};
