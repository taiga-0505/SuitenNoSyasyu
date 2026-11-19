#pragma once
#include "Audio/AudioEngine.h"
#include "Common/Log/Log.h"
#include "Common/ResourcePath.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <fstream>
#include <string>

/// @class GameSettings
/// @brief プレイヤーが変えられる設定（操作感度・音量）をシーンをまたいで保持する
/// @details 感度は RailShooterController が毎フレーム読み、音量は Apply() で AudioEngine に流す。
///          ポーズメニュー（PauseMenuScript）の設定画面がここを書き換えて Save() する。
///          値はスクリプトのメンバに持たせない。スクリプトはシーンごとに作り直されるので、
///          Result → Game と回った瞬間に既定値へ戻ってしまうため。
///
///          保存先は AppConfig.json / EditorConfig.json と同じ Resources/Setting/（作業ディレクトリ = project/ 基準）。
///          ファイルが無ければ既定値のまま起動する。
///
///          Singleton にしている理由: 設定ファイルは 1 つで、どのシーンのどのスクリプトから読んでも
///          同じ値でなければならない（ポーズで変えた感度が次のシーンでも効く）ため、
///          インスタンスを 1 つに限定してアクセス点を Get() にまとめている。
///          値はすべて private にし、読み書きは下の Getter / Setter を通す（カプセル化）。
class GameSettings {
public:
  /// @brief 唯一のインスタンスを取得する
  /// @return GameSettings のシングルトンインスタンス
  static GameSettings &Get() {
    static GameSettings instance;
    return instance;
  }

  // Singleton なのでコピー・代入はさせない
  GameSettings(const GameSettings &) = delete;
  GameSettings &operator=(const GameSettings &) = delete;

  static constexpr float kSensitivityMin = 0.1f;
  static constexpr float kSensitivityMax = 3.0f;
  static constexpr float kSensitivityStep = 0.1f; ///< 設定画面で左右キー 1 回ぶん
  static constexpr float kVolumeStep = 0.05f;     ///< 設定画面で左右キー 1 回ぶん
  static constexpr float kFovMin = 20.0f;         ///< 視野角の下限（度）
  static constexpr float kFovMax = 60.0f;         ///< 視野角の上限（度）。垂直 60 度 ≒ 水平 94 度
  static constexpr float kFovStep = 1.0f;         ///< 設定画面で左右キー 1 回ぶん（度）

  // ------------------------------------------------------------------
  // 読み取り
  // ------------------------------------------------------------------

  /// @brief マウス感度の倍率を取得する
  /// @return 倍率（kSensitivityMin〜kSensitivityMax）
  float MouseSensitivity() const { return mouseSensitivity_; }
  /// @brief 右スティック感度の倍率を取得する
  /// @return 倍率（kSensitivityMin〜kSensitivityMax）
  float ControllerSensitivity() const { return controllerSensitivity_; }
  /// @brief 上下反転が有効か
  /// @return 反転するなら true
  bool InvertY() const { return invertY_; }
  /// @brief 垂直方向の視野角（度）を取得する
  /// @return 視野角（度）。CameraComponent::fovY はラジアンなので FovRadians() を使う
  float FovDeg() const { return fovDeg_; }
  /// @brief 全体音量を取得する
  /// @return 0.0〜1.0
  float MasterVolume() const { return masterVolume_; }
  /// @brief BGM バスの音量を取得する
  /// @return 0.0〜1.0
  float BgmVolume() const { return bgmVolume_; }
  /// @brief SE バスの音量を取得する
  /// @return 0.0〜1.0
  float SeVolume() const { return seVolume_; }

  /// @brief 視野角をラジアンで返す（CameraComponent::fovY に入れる値）
  /// @return fovDeg をラジアンに変換した値
  float FovRadians() const { return fovDeg_ * (3.14159265358979f / 180.0f); }

  // ------------------------------------------------------------------
  // 書き込み（範囲への丸めは Clamp() / Apply() でまとめて行う）
  // ------------------------------------------------------------------

  /// @brief マウス感度の倍率を設定する
  /// @param v 倍率
  void SetMouseSensitivity(float v) { mouseSensitivity_ = v; }
  /// @brief 右スティック感度の倍率を設定する
  /// @param v 倍率
  void SetControllerSensitivity(float v) { controllerSensitivity_ = v; }
  /// @brief 上下反転を設定する
  /// @param v 反転するなら true
  void SetInvertY(bool v) { invertY_ = v; }
  /// @brief 垂直方向の視野角（度）を設定する
  /// @param v 視野角（度）
  void SetFovDeg(float v) { fovDeg_ = v; }
  /// @brief 全体音量を設定する（AudioEngine へ流すには Apply() を呼ぶ）
  /// @param v 0.0〜1.0
  void SetMasterVolume(float v) { masterVolume_ = v; }
  /// @brief BGM バスの音量を設定する（AudioEngine へ流すには Apply() を呼ぶ）
  /// @param v 0.0〜1.0
  void SetBgmVolume(float v) { bgmVolume_ = v; }
  /// @brief SE バスの音量を設定する（AudioEngine へ流すには Apply() を呼ぶ）
  /// @param v 0.0〜1.0
  void SetSeVolume(float v) { seVolume_ = v; }

  /// @brief 値を有効範囲へ丸める
  void Clamp() {
    mouseSensitivity_ = std::clamp(mouseSensitivity_, kSensitivityMin, kSensitivityMax);
    controllerSensitivity_ = std::clamp(controllerSensitivity_, kSensitivityMin, kSensitivityMax);
    fovDeg_ = std::clamp(fovDeg_, kFovMin, kFovMax);
    masterVolume_ = std::clamp(masterVolume_, 0.0f, 1.0f);
    bgmVolume_ = std::clamp(bgmVolume_, 0.0f, 1.0f);
    seVolume_ = std::clamp(seVolume_, 0.0f, 1.0f);
  }

  /// @brief 音量を AudioEngine へ反映する
  /// @details 感度は使う側（RailShooterController）が毎フレーム読むので流し込む先が無い。
  ///          音量だけはエンジン側に状態があるので、変更のたびに呼ぶ。
  void Apply() {
    Clamp();
    auto &audio = AudioEngine::Get();
    audio.SetMasterVolume(masterVolume_);
    audio.SetBusVolume(AudioBus::BGM, bgmVolume_);
    audio.SetBusVolume(AudioBus::SE, seVolume_);
  }

  /// @brief 既定値に戻す（保存はしない）
  void ResetToDefault() {
    mouseSensitivity_ = 1.0f;
    controllerSensitivity_ = 1.0f;
    invertY_ = false;
    fovDeg_ = 26.0f;
    masterVolume_ = 1.0f;
    bgmVolume_ = 1.0f;
    seVolume_ = 1.0f;
  }

  /// @brief ファイルから読み込む（無ければ既定値のまま）。起動時に一度呼べばよい
  /// @return ファイルを開けたら true（パースに失敗して既定値へ戻した場合も true）
  bool Load() {
    std::ifstream ifs(Chaso::PrepareSettingPath(kPath));
    // ファイルが無いのは初回起動の正常ケース。既定値のまま読み込み済み扱いにする
    if (!ifs.is_open()) {
      loaded_ = true;
      return false;
    }
    // 壊れた JSON でも起動を止めないよう、パース失敗時は既定値へ戻す
    try {
      nlohmann::json j;
      ifs >> j;
      FromJson(j);
      Clamp();
      Log::Print(std::string("[GameSettings] loaded ") + kPath);
    } catch (...) {
      Log::Print(std::string("[GameSettings] failed to parse ") + kPath + " (using defaults)");
      ResetToDefault();
    }
    loaded_ = true;
    return true;
  }

  /// @brief まだ一度も Load していなければ読み込む（どこから最初に触られても同じ値になるように）
  void EnsureLoaded() {
    if (!loaded_) {
      Load();
      Apply();
    }
  }

  /// @brief ファイルへ保存する
  /// @return 書けたら true
  bool Save() const {
    std::ofstream ofs(Chaso::PrepareSettingPath(kPath));
    if (!ofs.is_open()) {
      Log::Print(std::string("[GameSettings] failed to open for write: ") + kPath);
      return false;
    }
    ofs << ToJson().dump(2);
    return true;
  }

  /// @brief 設定値を JSON に変換する
  /// @return 全設定項目を持つ JSON オブジェクト
  nlohmann::json ToJson() const {
    return {
        {"mouseSensitivity", mouseSensitivity_},
        {"controllerSensitivity", controllerSensitivity_},
        {"invertY", invertY_},
        {"fovDeg", fovDeg_},
        {"masterVolume", masterVolume_},
        {"bgmVolume", bgmVolume_},
        {"seVolume", seVolume_},
    };
  }

  /// @brief JSON から設定値を読み込む
  /// @param j 読み込む JSON（含まれていないキーは現在の値のまま）
  /// @note 範囲チェックはしないので、呼んだ後に Clamp() すること。
  void FromJson(const nlohmann::json &j) {
    if (j.contains("mouseSensitivity")) mouseSensitivity_ = j["mouseSensitivity"].get<float>();
    if (j.contains("controllerSensitivity")) controllerSensitivity_ = j["controllerSensitivity"].get<float>();
    if (j.contains("invertY")) invertY_ = j["invertY"].get<bool>();
    if (j.contains("fovDeg")) fovDeg_ = j["fovDeg"].get<float>();
    if (j.contains("masterVolume")) masterVolume_ = j["masterVolume"].get<float>();
    if (j.contains("bgmVolume")) bgmVolume_ = j["bgmVolume"].get<float>();
    if (j.contains("seVolume")) seVolume_ = j["seVolume"].get<float>();
  }

private:
  GameSettings() = default;

  static constexpr const char *kPath = Chaso::kGameSettingsPath; ///< Resources/Setting/GameSettings.json
  bool loaded_ = false;

  // --- 操作 ---
  float mouseSensitivity_ = 1.0f;      ///< マウス感度の倍率（kSensitivityMin〜kSensitivityMax）
  float controllerSensitivity_ = 1.0f; ///< 右スティック感度の倍率
  bool invertY_ = false;               ///< 上下反転

  // --- 視野角 ---
  /// @brief 垂直方向の視野角（度）。CameraComponent::fovY はラジアンなので使う側で変換する
  /// @details 既定の 26 度は Game.json のカメラに入っている 0.45 rad とほぼ同じ
  ///          （＝設定を触らなければ今までと同じ見え方）。16:9 では水平 約 44 度。
  float fovDeg_ = 26.0f;

  // --- 音量（0.0〜1.0） ---
  float masterVolume_ = 1.0f; ///< 全体音量
  float bgmVolume_ = 1.0f;    ///< BGM バスの音量
  float seVolume_ = 1.0f;     ///< SE バスの音量
};
