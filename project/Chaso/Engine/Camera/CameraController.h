#pragma once
#include "DebugCamera/DebugCamera.h"
#include "Input/Input.h"
#include "MainCamera/MainCamera.h"
#include "Math/Math.h"

namespace RC {

/// @class CameraController
/// @brief メインカメラとデバッグカメラを切り替えて管理するコントローラ
class CameraController {
public:
  /// @brief 両カメラを初期化する
  /// @param input 入力管理クラスへのポインタ
  /// @param mainPos メインカメラの初期座標
  /// @param mainRot メインカメラの初期回転
  /// @param fovY 垂直画角（ラジアン）
  /// @param aspect アスペクト比（width / height）
  /// @param nearZ ニアクリップ距離
  /// @param farZ ファークリップ距離
  /// @note 画角・アスペクト比・クリップ距離は両カメラ共通。
  void Initialize(Input *input, const Vector3 &mainPos, const Vector3 &mainRot,
                  float fovY, float aspect, float nearZ, float farZ);

  /// @brief 有効なカメラを更新する（切り替えは SetUseDebug。F1 は DataDrivenScene が扱う）
  /// @param dt 前フレームからの経過時間（秒）。補間計算に使用する
  void Update(float dt = 1.0f / 60.0f);

  /// @brief 現在有効なカメラのビュー行列を取得する
  /// @return ビュー行列への参照
  const Matrix4x4 &GetView() const {
    return useDebug_ ? debug_.GetView() : main_.GetView();
  }

  /// @brief 現在有効なカメラの射影行列を取得する
  /// @return 射影行列への参照
  const Matrix4x4 &GetProjection() const {
    return useDebug_ ? debug_.GetProjection() : main_.GetProjection();
  }

  /// @brief ImGui によるカメラ情報の描画とパラメータ調整
  void DrawImGui();

  /// @brief デバッグカメラの使用状態を設定する
  /// @param v true でデバッグカメラを使用
  void SetUseDebug(bool v) { useDebug_ = v; }

  /// @brief デバッグカメラの使用中かどうかを取得する
  /// @return true ならデバッグカメラを使用中
  bool IsUsingDebug() const { return useDebug_; }

  /// @brief メインカメラの位置を直接設定する
  /// @param pos 設定する座標
  void SetMainPosition(const Vector3 &pos);

  /// @brief メインカメラの回転を直接設定する
  /// @param rot 設定する回転角（ラジアン）
  void SetMainRotation(const Vector3 &rot);

  /// @brief 現在のカメラのワールド座標を取得する
  /// @return ワールド座標
  RC::Vector3 GetWorldPos() const { return worldPos_; }

  /// @brief メインカメラの射影パラメータを実行時に変更する
  /// @param fovY 垂直画角（ラジアン）
  /// @param aspect アスペクト比
  /// @param nearZ ニアクリップ距離
  /// @param farZ ファークリップ距離
  /// @note デバッグカメラの射影は変わらない。
  void SetProjection(float fovY, float aspect, float nearZ, float farZ) {
    main_.SetProjection(fovY, aspect, nearZ, farZ);
  }

private:
  Input *input_ = nullptr; ///< 入力管理クラスへのポインタ
  DebugCamera debug_;      ///< デバッグ用カメラ
  MainCamera main_;        ///< メインカメラ
  bool useDebug_ = false;  ///< デバッグモードフラグ
  bool showGuide_ = false; ///< ガイドライン表示フラグ
  bool isDraggingCamera_ = false; ///< カメラのドラッグ操作中フラグ

  RC::Vector3 worldPos_{0, 0, 0}; ///< 現在のワールド座標
};

} // namespace RC
