#pragma once

// ============================================================================
// CaptureMode — 撮影モード
// ----------------------------------------------------------------------------
// F11 を押すとエディタの他のパネルを全部隠し、ゲーム画面だけを全画面にする。もう一度 F11 で戻る。
// F10 で録画（VideoRecorder。MP4）の開始 / 停止。撮影モード中に録ればゲーム画面だけが写る。
// 保存先はログに出る（[VideoRecorder] Video saved: ...）。
// ============================================================================

#include <d3d12.h>

class Dx12Core;
class Scene;

/// @class CaptureMode
/// @brief 撮影モード（ゲーム画面の全画面表示）
/// @details 状態は静的に持つ。撮影モードは 1 つしか無いので実害はない。
class CaptureMode {
public:
  /// @brief 撮影モード中か
  /// @return 撮影モード中なら true
  static bool IsActive();

  /// @brief 撮影モードの ON / OFF を切り替える
  /// @param active true で撮影モードに入る、false で抜ける
  /// @note 同じ状態を指定したときは何もしない。
  static void SetActive(bool active);

  /// @brief ゲーム画面を全画面で敷く
  /// @param viewportSrv  ゲーム画面の SRV
  /// @param core         Dx12Core（アスペクト比の算出に使う）。nullptr 可
  /// @param currentScene 現在のシーン。nullptr 可
  /// @param deltaTime    前フレームからの経過時間（秒）
  /// @note 撮影モード中は EditorManager 側で他のパネルを描かないこと。
  static void Draw(D3D12_GPU_DESCRIPTOR_HANDLE viewportSrv, Dx12Core *core,
                   Scene *currentScene, float deltaTime);

  /// @brief F11（撮影モードの開始 / 終了）と F10（録画の開始 / 停止）を拾う
  /// @param core 録画に使う。nullptr なら F10 は無視する
  /// @details 撮影モードに入っていなくても毎フレーム呼ぶこと。
  /// @return 撮影モードの ON / OFF が切り替わったら true
  static bool HandleHotkeys(Dx12Core *core = nullptr);

  /// @brief 撮影モードに入るとき、シーンを再生状態にする必要があるか
  /// @details 撮るのはプレイ画面なので、EditorManager から PlayState を Playing にしてもらう。
  /// @return 再生状態にしてほしいなら true
  static bool WantsPlaying();

  /// @brief いまマウスがゲーム画面の上にあるか（撮影モード中は常に true）
  /// @details 通常時は Viewport パネルの `ImGui::IsWindowHovered()` が担っている判定。
  ///          撮影モードでは Viewport パネルを描かないので、代わりにこれを
  ///          `EditorManager::isViewportHovered_` へ渡す。
  ///          これが false のあいだは射撃が止まるので、落下高さのスライダを
  ///          ドラッグしているあいだに弾が出ることもない。
  /// @note 値は Draw() の中で更新される（Viewport パネルと同じく 1 フレーム遅れ）。
  /// @return マウスがゲーム画面の上にあるなら true
  static bool IsGameHovered();
};
