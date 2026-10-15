#pragma once
#include "Math/MathTypes.h"
#include "struct.h"
#include <cstdint>

namespace RC {

/// @class AreaLightSource
/// @brief 矩形エリアライト（Rect AreaLight）の操作用クラス
/// @details CPU 側でパラメータを保持し、GPU へ送るためのデータ構造体（AreaLight）を管理する。
class AreaLightSource {
public:
  /// @brief コンストラクタ（既定値を設定する）
  AreaLightSource();

  /// @brief ライトの生データ（構造体）を取得する
  /// @return AreaLight 構造体への参照
  ::AreaLight &Data() { return data_; }

  /// @brief ライトの生データ（構造体）を取得する（const 版）
  /// @return AreaLight 構造体への const 参照
  const ::AreaLight &Data() const { return data_; }

  /// @brief ライトの位置を設定する
  /// @param pos 配置座標
  void SetPosition(const Vector3 &pos) { data_.position = pos; }

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

  /// @brief ライトの方向軸（基底）を設定する
  /// @details 面の向きを決める 2 軸を指定する。
  /// @param right     右方向ベクトル
  /// @param up        上方向ベクトル
  /// @param normalize ベクトルを正規化するかどうか
  void SetBasis(const Vector3 &right, const Vector3 &up, bool normalize = true);

  /// @brief エリアライトのサイズを設定する
  /// @param width  幅（フルサイズ）
  /// @param height 高さ（フルサイズ）
  void SetSize(float width, float height);

  /// @brief エリアライトの半分サイズを設定する
  /// @param halfWidth  半幅
  /// @param halfHeight 半高さ
  void SetHalfSize(float halfWidth, float halfHeight) {
    data_.halfWidth = halfWidth;
    data_.halfHeight = halfHeight;
  }

  /// @brief 影響範囲を設定する
  /// @param r 距離
  void SetRange(float r) { data_.range = r; }

  /// @brief 減衰パラメータを設定する
  /// @param d 減衰率
  void SetDecay(float d) { data_.decay = d; }

  /// @brief 両面発光にするかを設定する
  /// @param b true で両面を発光させる
  void SetTwoSided(bool b) { data_.twoSided = b ? 1u : 0u; }

  /// @brief 両面発光かを取得する
  /// @return true なら両面
  bool IsTwoSided() const { return data_.twoSided != 0; }

  // --------------------------------------------------------------------------
  // 影（全方位に光る光源なので、真下向きの広角シャドウ 1 枚で遮蔽を判定する。
  //   シェーダの SampleOmniShadowDown を参照）
  // --------------------------------------------------------------------------

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
  /// @param nearZ near クリップ距離
  void SetShadowNear(float nearZ) { shadowNear_ = nearZ; }

  /// @brief 影の near クリップを取得する
  /// @return near クリップ距離
  float GetShadowNear() const { return shadowNear_; }

  /// @brief 影の遮蔽物から外すオーナーの識別子を設定する（0 で無効）
  /// @details エンジンは中身を解釈しない不透明な値として扱い、シーン側が
  ///          「この識別子のエンティティはこのライトの影を落とさない」判定に使う。
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

  /// @brief 有効・無効を切り替える
  /// @param enabled true で有効
  void SetEnabled(bool enabled) { enabled_ = enabled; }

  /// @brief 有効状態を取得する
  /// @return 有効なら true
  bool IsEnabled() const { return enabled_; }

  /// @brief 有効・無効を反転させる
  void ToggleEnabled() { enabled_ = !enabled_; }

  /// @brief GPU 転送用のデータを取得する
  /// @details enabled フラグの状態を反映（false なら輝度を 0 にする）したコピーを返す。
  /// @return GPU 転送用データ
  ::AreaLight DataForGPU() const;

private:
  bool enabled_ = true; ///< 有効フラグ
  bool castShadow_ = true;   ///< 影を落とすか（シーン側が影タイルを割り当てる対象になる）
  float shadowNear_ = 0.05f; ///< 影の near クリップ（真下向きシャドウなので小さめ）
  uint32_t shadowExcludeOwnerId_ = 0; ///< このライトの影を落とさないエンティティの識別子（0 で無効）
  int shadowPriority_ = 0;   ///< 影タイルの優先度（小さいほど優先）
  ::AreaLight data_{};  ///< ライトパラメータ実体
};

} // namespace RC
