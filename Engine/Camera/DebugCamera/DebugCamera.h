#pragma once
#include "Input/Input.h"
#include "struct.h"

/// @class DebugCamera
/// @brief デバッグ用の自由移動カメラクラス
/// @details Blender に近い操作（中ドラッグで回転、Shift + 中ドラッグでパン、WASD / E / Q で移動）で動かす。
class DebugCamera {
public:
  /// @brief 入力と射影パラメータを設定して初期化する
  /// @param input キー入力インスタンスへのポインタ
  /// @param fovY 垂直画角（ラジアン）
  /// @param aspect アスペクト比（width / height）
  /// @param nearZ ニアクリップ距離
  /// @param farZ ファークリップ距離
  /// @note 射影行列はここで一度だけ作る。
  void Initialize(Input *input, float fovY, float aspect, float nearZ,
                  float farZ);

  /// @brief 入力に基づいてカメラの移動・回転・行列計算を行う
  /// @param dt フレーム経過時間（秒。0 以下なら前回の値を使う）
  void Update(float dt = 1.0f / 60.0f);

  /// @brief 位置と回転を初期状態にリセットする
  /// @note ビュー行列に反映されるのは次の Update から。
  void Reset();

  /// @brief ビュー行列を取得する
  /// @return ビュー行列への参照
  const RC::Matrix4x4 &GetView() const { return view_; }

  /// @brief 射影行列を取得する
  /// @return 射影行列への参照
  const RC::Matrix4x4 &GetProjection() const { return proj_; }

  /// @brief カメラの座標を取得する
  /// @return カメラ座標（Vector3）
  RC::Vector3 GetPosition() const { return translation_; }

  /// @brief カメラの位置を設定する（ビュー行列も即座に作り直す）
  /// @param pos 設定する座標
  void SetPosition(const RC::Vector3 &pos);

  /// @brief カメラの回転角を設定する（ビュー行列も即座に作り直す）
  /// @param rot 設定する回転角（ラジアン）
  void SetRotation(const RC::Vector3 &rot);

  /// @brief カメラの位置と回転を同時に設定する（ビュー行列も即座に作り直す）
  /// @param pos 設定する座標
  /// @param rot 設定する回転角（ラジアン）
  void SetTransform(const RC::Vector3 &pos, const RC::Vector3 &rot);

  /// @brief 移動速度を設定する
  /// @param speed 基本移動速度（m/s）
  void SetMoveSpeed(float speed) { moveSpeed_ = speed; }
  /// @brief 移動速度を取得する
  /// @return 基本移動速度（m/s）
  float GetMoveSpeed() const { return moveSpeed_; }

  /// @brief Shift 加速倍率を設定する
  /// @param mult Shift 押下時に移動速度へ掛ける倍率
  void SetBoostMultiplier(float mult) { boostMultiplier_ = mult; }
  /// @brief Shift 加速倍率を取得する
  /// @return Shift 押下時に移動速度へ掛ける倍率
  float GetBoostMultiplier() const { return boostMultiplier_; }

private:
  /// @brief 位置と回転からビュー行列を計算し直す
  void RebuildView_();

  Input *input_ = nullptr;               ///< キー入力インスタンスへのポインタ
  RC::Vector3 rotation_ = {0, 0, 0};     ///< 各軸の回転角（ラジアン）
  RC::Vector3 translation_ = {0, 0, -8}; ///< カメラの位置（並進座標）
  RC::Matrix4x4 view_;                   ///< ビュー行列
  RC::Matrix4x4 proj_;                   ///< 射影行列

  float deltaTime_ = 1.0f / 60.0f;       ///< 1 フレームあたりの経過時間（秒）

  float moveSpeed_ = 5.0f;               ///< カメラの基本移動速度（m/s）
  float boostMultiplier_ = 3.5f;         ///< Shift 押下時の加速倍率
  float rotateSpeed_ = 1.5f;             ///< キー入力による回転速度（ラジアン/秒）
  float mouseRotateSpeed_ = 0.003f;      ///< マウスドラッグ回転感度
  float mousePanSpeed_ = 0.008f;         ///< マウスドラッグ平行移動感度
  float mouseZoomSpeed_ = 0.015f;        ///< マウスドラッグズーム感度
  float mouseWheelSpeed_ = 0.003f;       ///< マウスホイール感度
};
