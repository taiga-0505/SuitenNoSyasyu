#pragma once
#include "Math/Math.h"
#include "Math/MathTypes.h"
#include "struct.h"

/// @class MainCamera
/// @brief ゲーム内で使用するメインカメラクラス
class MainCamera {
public:
  /// @brief 位置・回転・射影パラメータを設定して初期化する
  /// @param pos カメラのワールド座標
  /// @param rot カメラの回転角（ラジアン）
  /// @param fovY 垂直画角（ラジアン）
  /// @param aspect 画面アスペクト比（width / height）
  /// @param nearZ ニアクリップ距離
  /// @param farZ ファークリップ距離
  /// @note 射影行列はここで一度だけ作る。後から変えるときは SetProjection を使う。
  void Initialize(const RC::Vector3 &pos, const RC::Vector3 &rot, float fovY,
                  float aspect, float nearZ, float farZ);

  /// @brief 位置と向きからビュー行列を計算し直す
  void Update();

  /// @brief ビュー行列を取得する
  /// @return ビュー行列への参照
  const RC::Matrix4x4 &GetView() const { return view_; }

  /// @brief 射影行列を取得する
  /// @return 射影行列への参照
  const RC::Matrix4x4 &GetProjection() const { return proj_; }

  /// @brief カメラの座標を取得する
  /// @return カメラ座標（Vector3）
  RC::Vector3 GetPosition() const { return transform_.translation; }

  /// @brief カメラの位置を設定する
  /// @param pos 設定する座標
  /// @note ビュー行列に反映されるのは次の Update から。
  void SetPosition(const RC::Vector3 &pos) { transform_.translation = pos; }

  /// @brief カメラの回転角を設定する
  /// @param rot 設定する回転角（ラジアン）
  /// @note ビュー行列に反映されるのは次の Update から。
  void SetRotation(const RC::Vector3 &rot) { transform_.rotation = rot; }

  /// @brief カメラの回転角を取得する
  /// @return 回転角（ラジアン）
  RC::Vector3 GetRotation() const { return transform_.rotation; }

  /// @brief 射影パラメータを実行時に変更する
  /// @param fovY 垂直画角（ラジアン）
  /// @param aspect アスペクト比
  /// @param nearZ ニアクリップ距離
  /// @param farZ ファークリップ距離
  void SetProjection(float fovY, float aspect, float nearZ, float farZ);

  /// @brief 垂直画角を取得する
  /// @return 垂直画角（ラジアン）
  float GetFovY() const { return fovY_; }
  /// @brief ニアクリップ距離を取得する
  /// @return ニアクリップ距離
  float GetNearZ() const { return nearZ_; }
  /// @brief ファークリップ距離を取得する
  /// @return ファークリップ距離
  float GetFarZ() const { return farZ_; }

private:
  Transform transform_; ///< カメラのトランスフォーム情報（座標、回転、スケール）
  RC::Matrix4x4 view_;   ///< ビュー行列
  RC::Matrix4x4 proj_;   ///< 射影行列
  float fovY_ = 0.45f;   ///< 垂直画角（ラジアン）
  float nearZ_ = 0.1f;   ///< ニアクリップ
  float farZ_ = 100.0f;  ///< ファークリップ
};
