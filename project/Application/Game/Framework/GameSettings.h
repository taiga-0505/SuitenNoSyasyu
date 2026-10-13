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
class GameSettings {
public:
  /// @brief 唯一のインスタンスを取得する
  /// @return GameSettings のシングルトンインスタンス
  static GameSettings &Get() {
    static GameSettings instance;
    return instance;
  }

  // --- 操作 ---
  float mouseSensitivity = 1.0f;      ///< マウス感度の倍率（kSensitivityMin〜kSensitivityMax）
  float controllerSensitivity = 1.0f; ///< 右スティック感度の倍率
  bool invertY = false;               ///< 上下反転

  // --- 視野角 ---
  /// @brief 垂直方向の視野角（度）。CameraComponent::fovY はラジアンなので使う側で変換する
  /// @details 既定の 26 度は Game.json のカメラに入っている 0.45 rad とほぼ同じ
  ///          （＝設定を触らなければ今までと同じ見え方）。16:9 では水平 約 44 度。
  float fovDeg = 26.0f;

  // --- 音量（0.0〜1.0） ---
  float masterVolume = 1.0f; ///< 全体音量
  float bgmVolume = 1.0f;    ///< BGM バスの音量
  float seVolume = 1.0f;     ///< SE バスの音量

  static constexpr float kSensitivityMin = 0.1f;
  static constexpr float kSensitivityMax = 3.0f;
  static constexpr float kSensitivityStep = 0.1f; ///< 設定画面で左右キー 1 回ぶん
  static constexpr float kVolumeStep = 0.05f;     ///< 設定画面で左右キー 1 回ぶん
  static constexpr float kFovMin = 20.0f;         ///< 視野角の下限（度）
  static constexpr float kFovMax = 60.0f;         ///< 視野角の上限（度）。垂直 60 度 ≒ 水平 94 度
  static constexpr float kFovStep = 1.0f;         ///< 設定画面で左右キー 1 回ぶん（度）

  /// @brief 視野角をラジアンで返す（CameraComponent::fovY に入れる値）
  /// @return fovDeg をラジアンに変換した値
  float FovRadians() const { return fovDeg * (3.14159265358979f / 180.0f); }

  /// @brief 値を有効範囲へ丸める
  void Clamp() {
    mouseSensitivity = std::clamp(mouseSensitivity, kSensitivityMin, kSensitivityMax);
    controllerSensitivity = std::clamp(controllerSensitivity, kSensitivityMin, kSensitivityMax);
    fovDeg = std::clamp(fovDeg, kFovMin, kFovMax);
    masterVolume = std::clamp(masterVolume, 0.0f, 1.0f);
    bgmVolume = std::clamp(bgmVolume, 0.0f, 1.0f);
    seVolume = std::clamp(seVolume, 0.0f, 1.0f);
  }

  /// @brief 音量を AudioEngine へ反映する
  /// @details 感度は使う側（RailShooterController）が毎フレーム読むので流し込む先が無い。
  ///          音量だけはエンジン側に状態があるので、変更のたびに呼ぶ。
  void Apply() {
    Clamp();
    auto &audio = AudioEngine::Get();
    audio.SetMasterVolume(masterVolume);
    audio.SetBusVolume(AudioBus::BGM, bgmVolume);
    audio.SetBusVolume(AudioBus::SE, seVolume);
  }

  /// @brief 既定値に戻す（保存はしない）
  void ResetToDefault() {
    mouseSensitivity = 1.0f;
    controllerSensitivity = 1.0f;
    invertY = false;
    fovDeg = 26.0f;
    masterVolume = 1.0f;
    bgmVolume = 1.0f;
    seVolume = 1.0f;
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
        {"mouseSensitivity", mouseSensitivity},
        {"controllerSensitivity", controllerSensitivity},
        {"invertY", invertY},
        {"fovDeg", fovDeg},
        {"masterVolume", masterVolume},
        {"bgmVolume", bgmVolume},
        {"seVolume", seVolume},
    };
  }

  /// @brief JSON から設定値を読み込む
  /// @param j 読み込む JSON（含まれていないキーは現在の値のまま）
  /// @note 範囲チェックはしないので、呼んだ後に Clamp() すること。
  void FromJson(const nlohmann::json &j) {
    if (j.contains("mouseSensitivity")) mouseSensitivity = j["mouseSensitivity"].get<float>();
    if (j.contains("controllerSensitivity")) controllerSensitivity = j["controllerSensitivity"].get<float>();
    if (j.contains("invertY")) invertY = j["invertY"].get<bool>();
    if (j.contains("fovDeg")) fovDeg = j["fovDeg"].get<float>();
    if (j.contains("masterVolume")) masterVolume = j["masterVolume"].get<float>();
    if (j.contains("bgmVolume")) bgmVolume = j["bgmVolume"].get<float>();
    if (j.contains("seVolume")) seVolume = j["seVolume"].get<float>();
  }

private:
  GameSettings() = default;

  static constexpr const char *kPath = Chaso::kGameSettingsPath; ///< Resources/Setting/GameSettings.json
  bool loaded_ = false;
};
