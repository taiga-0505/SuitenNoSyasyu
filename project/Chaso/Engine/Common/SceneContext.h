#pragma once

// ============================================================================
// SceneContext
// ----------------------------------------------------------------------------
// シーンとエンジンの間で受け渡す共有情報。
// エンジン側（描画など）はこのヘッダだけを参照し、Application 側の Scene.h には依存しない。
// ============================================================================

#include <d3d12.h>
#include <functional>
#include <string>

class Dx12Core;
class Input;
class ImGuiManager;
class PipelineManager;
class PostProcess;
struct AppConfig;
namespace RC { class CameraController; }

/// @enum PlayState
/// @brief ゲームの再生状態を表す列挙型
enum class PlayState {
  Stopped, // 停止中（編集モード）
  Playing, // 再生中
  Paused   // 一時停止中
};

/// @enum SceneTransition
/// @brief シーン切り替えに使う演出の種類
/// @details SceneManager::RequestChange に渡す。演出ごとに「何色へ抜けるか」
///          「切り替え後に GrayscaleIntro を挟むか」が変わる。
enum class SceneTransition {
  None,     ///< 演出なし（起動直後の ChangeImmediately やエディタ操作）
  Dissolve, ///< 既定。黒へノイズディゾルブ → 切り替え → 白黒から色が戻る
  Dive,     ///< 水面へ飛び込む遷移。暗い深海色へ短く抜け、切り替え後は演出を挟まず
            ///< すぐにシーンを動かす（Game 側の「深海から浮上」がそのまま続く）
};

/// @brief スクリプトから文字列で遷移演出を指定するときの名前
/// @details ScriptableEntity::RequestSceneChange(name, transition) はエンジン層の
///          ヘッダなので SceneTransition を型として持てない。文字列で受けて
///          ParseSceneTransition で変換する。
namespace SceneTransitions {
inline constexpr const char *kDissolve = "dissolve";
inline constexpr const char *kDive = "dive";
} // namespace SceneTransitions

/// @brief 遷移演出の名前を SceneTransition へ変換する（不明な名前は Dissolve）
inline SceneTransition ParseSceneTransition(const std::string &name) {
  if (name == SceneTransitions::kDive) return SceneTransition::Dive;
  return SceneTransition::Dissolve;
}

/// @struct SceneContext
/// @brief シーン間で共有されるエンジンコンポーネントへの参照を保持する構造体
/// @details 各シーンの Update/Render に渡され、グラフィックスデバイス、入力、デバッグツールなどへのアクセスを提供します。
///          オーディオはシングルトンの AudioEngine と、エンティティの AudioSourceComponent で扱うためここには含めない。
struct SceneContext {
  Dx12Core *core = nullptr;             ///< DirectX12 コアシステム
  Input *input = nullptr;               ///< 入力システム（キーボード、マウス、コントローラー）
  AppConfig *app = nullptr;             ///< アプリケーション設定
  ImGuiManager *imgui = nullptr;         ///< ImGui 管理
  PipelineManager *pipelineManager = nullptr; ///< パイプライン管理
  PostProcess *postProcess = nullptr;     ///< ポストプロセス管理
  RC::CameraController *camera = nullptr; ///< エディタカメラ（SceneManager が所有）
  float deltaTime = 1.0f / 60.0f;        ///< 前フレームからの経過時間 (秒)

  /// @brief シーン遷移を要求するコールバック（SceneManager::Init で結線される）
  /// @details 引数は遷移先のシーン名と遷移演出、戻り値は要求が受理されたか。
  ///          SceneManager は Scene の入れ子クラスのため型として前方宣言できない。
  ///          ここを関数オブジェクトにしておくことで、SceneContext から
  ///          SceneManager への型依存を持たずに遷移要求だけを公開できる。
  ///          スクリプトからは ScriptableEntity::RequestSceneChange() 経由で使う。
  std::function<bool(const std::string &, SceneTransition)> requestSceneChange;

  /// @brief いま表示しているシーンへ入ったときの遷移演出
  /// @details SceneManager が ChangeImmediately の直前（OnEnter より前）に書く。
  ///          起動直後やエディタからの直接切り替えは None。
  ///          「タイトルから飛び込んで来たときだけ浮上演出を再生する」といった、
  ///          入り方で分岐したいスクリプトが OnCreate で読む。
  SceneTransition lastTransition = SceneTransition::None;

  PlayState playState = PlayState::Playing; ///< 現在の再生状態

  /// @brief ゲーム内ポーズ（ESC メニュー）中か
  /// @details playState はエディタの再生／停止ボタンの状態で、エディタが毎フレーム同期して
  ///          上書きするため、ゲーム側の都合で書き換えられない。ゲームが自分で止まる
  ///          「ポーズ」は別のフラグで持つ。true のあいだ DataDrivenScene はスクリプト・
  ///          アニメーション・GameMode の時間を進めない（deltaTime = 0 で回す）。
  ///          ポーズメニューのスクリプト自身は deltaTime ではなく ctx.deltaTime を読んで動く。
  ///          立てたスクリプトが OnDestroy で必ず下ろすこと（シーンをまたいで残さない）。
  bool gamePaused = false;

  D3D12_CPU_DESCRIPTOR_HANDLE currentRTV{}; ///< 現在の描画先RTV
  D3D12_CPU_DESCRIPTOR_HANDLE currentDSV{}; ///< 現在の描画先DSV
  /// @brief currentRTV の実体（RENDER_TARGET 状態）。水面の屈折で「水を描く前の画面」をコピーする元にする。
  ///        nullptr なら屈折は無効（従来の αブレンドで描く）
  ID3D12Resource *currentColorResource = nullptr;

  /// @brief 再生中かどうか判定する（エディタの再生状態。ゲーム内ポーズは見ない）
  bool isPlaying() const {
    return playState == PlayState::Playing;
  }

  /// @brief ゲームの時間が進んでいるか（再生中 かつ ゲーム内ポーズでない）
  /// @details 敵・弾・レール・アニメーションなど「ゲームの進行」はこちらで判定する。
  bool isSimulating() const {
    return isPlaying() && !gamePaused;
  }
};
