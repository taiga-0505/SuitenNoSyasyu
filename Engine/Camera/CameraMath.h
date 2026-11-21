#pragma once
#include "Math/MathTypes.h"
#include "Math/MathUtils.h" // SafeNormalize / Clamp など使う
#include "Math/Math.h"      // Matrix Multiply/Inverse
#include <algorithm>
#include <cmath>

/// @brief カメラに関連する数学計算関数をまとめた名前空間
namespace RC::CameraMath {

/// @brief 視点座標と注視点座標から、カメラの回転角（Yaw, Pitch）を計算する
/// @param eye 視点座標（カメラの位置）
/// @param target 注視点座標（カメラが向く位置）
/// @param fallbackForward ベクトルの長さが 0 だった場合に使用するデフォルトの前方方向
/// @return 回転角（x: Pitch, y: Yaw, z: 0）
inline Vector3 LookAtYawPitch(const Vector3 &eye, const Vector3 &target,
                              const Vector3 &fallbackForward = Vector3{0, 0,
                                                                       1}) {
  const Vector3 dir = RC::SafeNormalize(RC::Sub(target, eye), fallbackForward);

  const float yaw = std::atan2(dir.x, dir.z);
  const float pitch = std::asin(std::clamp(dir.y, -1.0f, 1.0f));

  return Vector3{pitch, yaw, 0.0f}; // roll は使わない想定
}

/// @brief Yaw 角と Pitch 角から前方ベクトルを計算する。DebugCamera の移動計算などで使用
/// @param yaw Yaw 角（ラジアン）
/// @param pitch Pitch 角（ラジアン）
/// @return 前方ベクトル
inline Vector3 ForwardFromYawPitch(float yaw, float pitch) {
  const float cy = std::cos(yaw);
  const float sy = std::sin(yaw);
  const float cp = std::cos(pitch);
  const float sp = std::sin(pitch);

  // 「+Z が前」系の forward
  return Vector3{sy * cp, sp, cy * cp};
}

/// @brief 回転角（Vector3）から前方ベクトルを計算する
/// @param rot 回転角（x: Pitch, y: Yaw, z: Roll）
/// @return 前方ベクトル
inline Vector3 ForwardFromRotation(const Vector3 &rot) {
  return ForwardFromYawPitch(rot.y, rot.x); // rot = {pitch, yaw, roll} の前提
}

/// @brief スクリーン座標からワールド空間のレイ（Ray）を生成する
/// @param mousePos マウスのスクリーン座標（左上が原点）
/// @param screenSize 画面サイズ（幅, 高さ）
/// @param viewMatrix カメラのビュー行列
/// @param projMatrix カメラのプロジェクション行列
/// @return スクリーン上をクリックした位置に向かうワールド空間のレイ
inline Ray ScreenPointToRay(const Vector2& mousePos, const Vector2& screenSize, const Matrix4x4& viewMatrix, const Matrix4x4& projMatrix) {
    float ndcX = (2.0f * mousePos.x) / screenSize.x - 1.0f;
    float ndcY = 1.0f - (2.0f * mousePos.y) / screenSize.y;

    Matrix4x4 viewProj = Multiply(viewMatrix, projMatrix);
    Matrix4x4 invViewProj = Inverse(viewProj);

    Vector3 nearPt = {ndcX, ndcY, 0.0f};
    Vector3 farPt  = {ndcX, ndcY, 1.0f};

    Vector3 worldNear = Vector3Transform(nearPt, invViewProj);
    Vector3 worldFar  = Vector3Transform(farPt, invViewProj);

    Ray ray;
    ray.origin = worldNear;
    ray.direction = SafeNormalize(Sub(worldFar, worldNear));
    return ray;
}

/// @brief ワールド空間の座標をスクリーン座標に変換する
/// @param worldPos ワールド空間の座標
/// @param screenSize 画面サイズ（幅, 高さ）
/// @param viewMatrix カメラのビュー行列
/// @param projMatrix カメラのプロジェクション行列
/// @return スクリーン座標（x, y）および NDC 空間での Z 値（z）
inline Vector3 WorldToScreenPoint(const Vector3& worldPos, const Vector2& screenSize, const Matrix4x4& viewMatrix, const Matrix4x4& projMatrix) {
    Matrix4x4 viewProj = Multiply(viewMatrix, projMatrix);
    Vector3 ndcPos = Vector3Transform(worldPos, viewProj);
    
    float screenX = (ndcPos.x + 1.0f) * 0.5f * screenSize.x;
    float screenY = (1.0f - ndcPos.y) * 0.5f * screenSize.y;
    
    return Vector3{screenX, screenY, ndcPos.z};
}

} // namespace RC::CameraMath
