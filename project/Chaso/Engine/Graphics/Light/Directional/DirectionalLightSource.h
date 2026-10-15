#pragma once
#include "Math/MathTypes.h"
#include "struct.h"

namespace RC {

/// @class DirectionalLightSource
/// @brief 平行光源（Directional Light）を制御するクラス
/// @details 光の方向、色、輝度、およびライティングモード（Lambert / HalfLambert）を管理する。
class DirectionalLightSource {
public:
  /// @brief コンストラクタ（既定値を設定する）
  DirectionalLightSource();

  /// @brief ライトの生データ（構造体）を取得する
  /// @return DirectionalLight 構造体への参照
  DirectionalLight &Data() { return data_; }

  /// @brief ライトの生データ（構造体）を取得する（const 版）
  /// @return DirectionalLight 構造体への const 参照
  const DirectionalLight &Data() const { return data_; }

  /// @brief 有効状態を取得する
  /// @return 有効なら true
  bool IsEnabled() const { return enabled_; }

  /// @brief 有効状態を設定する
  /// @param enabled 有効にするなら true
  void SetEnabled(bool enabled) { enabled_ = enabled; }

  /// @brief GPU 転送用のデータを取得する
  /// @details 有効フラグが false の場合は、強度を 0 にしたコピーを返す。
  /// @return GPU 転送用データ
  DirectionalLight DataForGPU() const;

  /// @brief 光の方向を設定する
  /// @param dir       方向ベクトル
  /// @param normalize ベクトルを正規化するかどうか
  void SetDirection(const Vector3 &dir, bool normalize = true);

  /// @brief ライトの色を設定する（RGB）
  /// @param rgb   色成分
  /// @param alpha 透明度
  void SetColor(const Vector3 &rgb, float alpha = 1.0f);

  /// @brief ライトの色を設定する（RGBA）
  /// @param rgba 4 成分の色
  void SetColor(const Vector4 &rgba);

  /// @brief 輝度を設定する
  /// @param intensity 輝度値
  void SetIntensity(float intensity);

  /// @brief 環境光を設定する（ライトが当たっていない面にも base * rgb * intensity が足される）
  /// @param rgb       環境光の色
  /// @param intensity 環境光の強さ（0 で環境光なし）
  void SetAmbient(const Vector3 &rgb, float intensity);

  /// @brief ライティングモードを取得する
  /// @return モジュール内部の定義値（0: None, 1: Lambert, 2: HalfLambert）
  int GetLightingMode() const { return lightingMode_; }

  /// @brief ライティングモードを設定する
  /// @param m モード指定（0: None, 1: Lambert, 2: HalfLambert）
  void SetLightingMode(int m) { lightingMode_ = m; }

  /// @brief 光沢度（Shininess）を設定する
  /// @details スペキュラ計算に使用する。
  /// @param s 光沢度
  void SetShininess(float s) { shininess_ = s; }

  /// @brief 光沢度（Shininess）を取得する
  /// @return 光沢度
  float GetShininess() const { return shininess_; }

private:
  DirectionalLight data_{}; ///< ライトパラメータ実体
  bool enabled_ = true;      ///< 有効フラグ
  int lightingMode_ = 2;    ///< ライティングモード（既定: HalfLambert）
  float shininess_ = 32.0f; ///< スペキュラ用光沢度
};

} // namespace RC
