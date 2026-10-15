#pragma once
#include "Math/MathTypes.h"
#include "struct.h"
#include <cstdint>

namespace RC {

/// @class SpotLightSource
/// @brief スポットライト（Spot Light）を制御するクラス
/// @details 位置、方向、色、輝度、および照射角度（カットオフ）を管理する。
class SpotLightSource {
public:
  /// @brief コンストラクタ（既定値を設定する）
  SpotLightSource();

  /// @brief ライトの生データ（構造体）を取得する
  /// @return SpotLight 構造体への参照
  ::SpotLight &Data() { return data_; }

  /// @brief ライトの生データ（構造体）を取得する（const 版）
  /// @return SpotLight 構造体への const 参照
  const ::SpotLight &Data() const { return data_; }

  /// @brief ライトの位置を設定する
  /// @param pos 配置座標
  void SetPosition(const Vector3 &pos) { data_.position = pos; }

  /// @brief ライトの照射方向を設定する
  /// @param dir 方向ベクトル
  void SetDirection(const Vector3 &dir) { data_.direction = dir; }

  /// @brief ライトの色を設定する（RGB）
  /// @param rgb   色成分
  /// @param alpha 透明度
  void SetColor(const Vector3 &rgb, float alpha = 1.0f) {
    data_.color = {rgb.x, rgb.y, rgb.z, alpha};
  }

  /// @brief ライトの色を設定する（RGBA）
  /// @param rgba 4 成分の色
  void SetColor(const Vector4 &rgba) { data_.color = rgba; }

  /// @brief 輝度を設定する
  /// @param v 輝度値
  void SetIntensity(float v) { data_.intensity = v; }

  /// @brief 届く距離（影響範囲）を設定する
  /// @param d 距離（0 以下で無効）
  void SetDistance(float d) { data_.distance = d; }

  /// @brief 距離減衰の指数を設定する
  /// @param d 減衰率（0 に近いほど緩やかに減衰）
  void SetDecay(float d) { data_.decay = d; }

  /// @brief 照射角のコサイン値を設定する
  /// @details 外側のカットオフ角度として扱われる。
  /// @param c コサイン値（例: cos(30度)）
  void SetCosAngle(float c) { data_.cosAngle = c; }

  /// @brief 角度（度数法）から照射角を設定する
  /// @param deg 角度（度。0〜89.9 に丸める）
  void SetAngleDeg(float deg);

  /// @brief 角度（弧度法）から照射角を設定する
  /// @param rad 角度（ラジアン。0〜ほぼ π/2 に丸める）
  void SetAngleRad(float rad);

  /// @brief 影アトラスのタイル番号を設定する
  /// @param index 0 以上でそのタイルの深度マップで遮蔽判定される。-1 で影なし（既定）
  void SetShadowIndex(int index) { data_.shadowIndex = index; }

  /// @brief 影アトラスのタイル番号を取得する
  /// @return タイル番号（-1 なら影なし）
  int GetShadowIndex() const { return data_.shadowIndex; }

  /// @brief 影を落とす（壁などに遮られる）かを設定する
  /// @details true のライトだけがシーン側で影アトラスのタイルを割り当てられる。
  ///          同時に影を落とせる数には上限（kMaxSpotShadows）があり、
  ///          あふれた分はカメラから遠い順に影なしになる。
  /// @param enabled true で影を落とす
  void SetCastShadow(bool enabled) { castShadow_ = enabled; }

  /// @brief 影を落とすか
  /// @return 影を落とすなら true
  bool IsCastShadow() const { return castShadow_; }

  /// @brief 影の near クリップ（ライトからこの距離より近い物は遮蔽物にならない）を設定する
  /// @details ライトを持つ本体が自分の光を遮ってしまう場合に大きくする。
  /// @param nearZ near クリップ距離
  void SetShadowNear(float nearZ) { shadowNear_ = nearZ; }

  /// @brief 影の near クリップを取得する
  /// @return near クリップ距離
  float GetShadowNear() const { return shadowNear_; }

  /// @brief 影の遮蔽物から外すオーナーの識別子を設定する（0 で無効）
  /// @details エンジンは中身を解釈しない不透明な値として扱い、シーン側が
  ///          「この識別子のエンティティはこのライトの影を落とさない」判定に使う。
  ///          ライトを持っている本体（プレイヤーの体など）が自分の光を遮るのを防ぐ用途。
  /// @param ownerId オーナーの識別子（0 で無効）
  void SetShadowExcludeOwnerId(uint32_t ownerId) { shadowExcludeOwnerId_ = ownerId; }

  /// @brief 影の遮蔽物から外すオーナーの識別子を取得する
  /// @return オーナーの識別子（0 で無効）
  uint32_t GetShadowExcludeOwnerId() const { return shadowExcludeOwnerId_; }

  /// @brief 影タイルの優先度を設定する（小さいほど優先。既定 0）
  /// @details 同時に影を落とせる灯数には上限があり、あふれた分は影なしになる。
  ///          敵が大量に湧いてもプレイヤー周りのライトが押し出されないよう、
  ///          重要度の低いライトに大きい値を入れておく。同じ優先度どうしは
  ///          カメラに近い順で選ばれる。
  /// @param priority 優先度（小さいほど優先）
  void SetShadowPriority(int priority) { shadowPriority_ = priority; }

  /// @brief 影タイルの優先度を取得する
  /// @return 優先度（小さいほど優先）
  int GetShadowPriority() const { return shadowPriority_; }

  /// @brief 有効状態を設定する
  /// @param enabled 有効にするなら true
  void SetEnabled(bool enabled) { enabled_ = enabled; }

  /// @brief 有効状態を取得する
  /// @return 有効なら true
  bool IsEnabled() const { return enabled_; }

  /// @brief 有効・無効を反転させる
  void ToggleEnabled() { enabled_ = !enabled_; }

  /// @brief GPU 転送用のデータを取得する
  /// @details 有効フラグが false の場合は、強度を 0 にしたコピーを返す。
  /// @return GPU 転送用データ
  ::SpotLight DataForGPU() const;

private:
  bool enabled_ = true;      ///< 有効フラグ
  bool castShadow_ = true;   ///< 影を落とすか（シーン側が影タイルを割り当てる対象になる）
  float shadowNear_ = 0.3f;  ///< 影の near クリップ
  uint32_t shadowExcludeOwnerId_ = 0; ///< このライトの影を落とさないエンティティの識別子（0 で無効）
  int shadowPriority_ = 0;   ///< 影タイルの優先度（小さいほど優先）
  ::SpotLight data_{};       ///< ライトパラメータ実体
};

} // namespace RC
