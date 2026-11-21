#pragma once
#include "Math/MathTypes.h"
#include "Math/Math.h"
#include "IComponent.h"
#include <nlohmann/json.hpp>

/// @brief ローカル空間のオフセットを、エンティティの回転で回してからワールド座標に変換する
/// @param basePos エンティティの位置（TransformComponent::position）
/// @param baseRot エンティティの回転（オイラー角、ラジアン）
/// @param offset  ローカル空間でのオフセット量
/// @return オフセット適用後のワールド座標
/// @note スケールは意図的に無視する（敵のモデルを拡大しても視界の起点はずらさないため）。
inline RC::Vector3 ApplyLocalOffset(const RC::Vector3& basePos,
                                    const RC::Vector3& baseRot,
                                    const RC::Vector3& offset) {
  if (offset.x == 0.0f && offset.y == 0.0f && offset.z == 0.0f) return basePos;
  return Vector3Transform(offset, MakeAffineMatrix({1.0f, 1.0f, 1.0f}, baseRot, basePos));
}

/// @struct DirectionalLightComponent
/// @brief 平行光源のコンポーネント
struct DirectionalLightComponent : public IComponent {
    int lightHandle = -1;                            ///< ライトのハンドル（未作成なら -1。保存しない）
    bool visible = true;                             ///< 有効か
    RC::Vector4 color = { 1.0f, 1.0f, 1.0f, 1.0f };  ///< 光の色
    RC::Vector3 direction = { 0.0f, -1.0f, 0.0f };   ///< 光の向き
    float intensity = 1.0f;                          ///< 光の強さ
    /// @brief 環境光の色。ライトが当たっていない面にも base * ambientColor * ambientIntensity が足される
    RC::Vector3 ambientColor = { 1.0f, 1.0f, 1.0f };
    /// @brief 環境光の強さ。0 でライトの当たった所以外は真っ暗（旧シェーダーの固定値は 0.2 相当）
    float ambientIntensity = 0.0f;

    /// @brief コンポーネントの型名を返す
    /// @return "DirectionalLightComponent"
    const char* TypeName() const override { return "DirectionalLightComponent"; }
    /// @brief ライトの設定を JSON に書き出す
    /// @return lightHandle 以外の設定値を格納した JSON
    nlohmann::json Serialize() const override {
      return {
        {"visible", visible},
        {"color", {color.x, color.y, color.z, color.w}},
        {"direction", {direction.x, direction.y, direction.z}},
        {"intensity", intensity},
        {"ambientColor", {ambientColor.x, ambientColor.y, ambientColor.z}},
        {"ambientIntensity", ambientIntensity}
      };
    }
    /// @brief JSON からライトの設定を読み込む
    /// @param j 読み込み元の JSON（含まれていないキーは現在の値のまま）
    void Deserialize(const nlohmann::json& j) override {
      if (j.contains("visible")) visible = j["visible"].get<bool>();
      if (j.contains("color")) { auto& c = j["color"]; color = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>(), c[3].get<float>()}; }
      if (j.contains("direction")) { auto& d = j["direction"]; direction = {d[0].get<float>(), d[1].get<float>(), d[2].get<float>()}; }
      if (j.contains("intensity")) intensity = j["intensity"].get<float>();
      if (j.contains("ambientColor")) { auto& a = j["ambientColor"]; ambientColor = {a[0].get<float>(), a[1].get<float>(), a[2].get<float>()}; }
      if (j.contains("ambientIntensity")) ambientIntensity = j["ambientIntensity"].get<float>();
    }
};

/// @struct PointLightComponent
/// @brief 点光源のコンポーネント
struct PointLightComponent : public IComponent {
    int lightHandle = -1;                            ///< ライトのハンドル（未作成なら -1。保存しない）
    bool visible = true;                             ///< 有効か
    RC::Vector4 color = { 1.0f, 1.0f, 1.0f, 1.0f };  ///< 光の色
    float intensity = 1.0f;                          ///< 光の強さ
    float radius = 10.0f;                            ///< 光が届く半径
    float decay = 1.0f;                              ///< 減衰率
    /// @brief 影を落とす（壁などに遮られる）か。
    /// @details 全方位に光る光源なので、キューブマップの代わりに「ライト位置から真下向きの
    ///          広角シャドウ」1 枚で遮蔽を判定する（壁が垂直である前提。詳細はシェーダの
    ///          SampleOmniShadowDown を参照）。同時に影を落とせるのは kMaxSpotShadows 灯まで
    ///          （超えた分はカメラに近い順に優先され、残りは影なしで照らす）。
    bool castShadow = true;
    /// @brief 影の near クリップ（ライトからこの距離より近い物は遮蔽物にならない）。
    /// @details 真下向きシャドウでは「ライトから水平距離 shadowNear / tan(照射半角) より近い壁」が
    ///          判定から外れてしまうので小さめにしておく。
    float shadowNear = 0.05f;
    /// @brief true なら、このライトを持つエンティティ自身は遮蔽物にしない。
    bool shadowExcludeSelf = false;
    /// @brief 影タイルの優先度（小さいほど優先。既定 0）
    /// @details 同時に影を落とせる灯数には上限があるので、あふれさせたくないライトほど
    ///          小さい値にする。同じ優先度どうしはカメラに近い順で選ばれる。
    int shadowPriority = 0;

    /// @brief コンポーネントの型名を返す
    /// @return "PointLightComponent"
    const char* TypeName() const override { return "PointLightComponent"; }
    /// @brief ライトの設定を JSON に書き出す
    /// @return lightHandle 以外の設定値を格納した JSON
    nlohmann::json Serialize() const override {
      return {
        {"visible", visible},
        {"color", {color.x, color.y, color.z, color.w}},
        {"intensity", intensity}, {"radius", radius}, {"decay", decay},
        {"castShadow", castShadow}, {"shadowNear", shadowNear}, {"shadowExcludeSelf", shadowExcludeSelf},
        {"shadowPriority", shadowPriority}
      };
    }
    /// @brief JSON からライトの設定を読み込む
    /// @param j 読み込み元の JSON（含まれていないキーは現在の値のまま）
    void Deserialize(const nlohmann::json& j) override {
      if (j.contains("visible")) visible = j["visible"].get<bool>();
      if (j.contains("color")) { auto& c = j["color"]; color = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>(), c[3].get<float>()}; }
      if (j.contains("intensity")) intensity = j["intensity"].get<float>();
      if (j.contains("radius")) radius = j["radius"].get<float>();
      if (j.contains("decay")) decay = j["decay"].get<float>();
      // これらが無い既存シーンは既定値（影あり）になる
      if (j.contains("castShadow")) castShadow = j["castShadow"].get<bool>();
      if (j.contains("shadowNear")) shadowNear = j["shadowNear"].get<float>();
      if (j.contains("shadowExcludeSelf")) shadowExcludeSelf = j["shadowExcludeSelf"].get<bool>();
      if (j.contains("shadowPriority")) shadowPriority = j["shadowPriority"].get<int>();
    }
};

/// @struct SpotLightComponent
/// @brief スポットライトのコンポーネント
struct SpotLightComponent : public IComponent {
    int lightHandle = -1;                            ///< ライトのハンドル（未作成なら -1。保存しない）
    bool visible = true;                             ///< 有効か
    RC::Vector4 color = { 1.0f, 1.0f, 1.0f, 1.0f };  ///< 光の色
    RC::Vector3 direction = { 0.0f, -1.0f, 0.0f };   ///< 光の向き
    /// @brief エンティティ中心からのローカル空間オフセット。エンティティの回転に追従する
    /// @details 例: 足元に置くなら {0, -0.9f, 0}、顔の少し前なら {0, 1.5f, 0.3f}。
    RC::Vector3 offset = { 0.0f, 0.0f, 0.0f };
    float intensity = 1.0f;   ///< 光の強さ
    float distance = 20.0f;   ///< 光が届く距離
    float decay = 1.0f;       ///< 減衰率
    float cosAngle = 0.866f; ///< 照射範囲の半角の cos（既定値は約 cos(30 度)）
    /// @brief 影を落とす（壁などに遮られる）か。true なら専用のシャドウマップで遮蔽判定され、
    ///        壁を挟んだ向こう側へ光が漏れない。同時に影を落とせるのは kMaxSpotShadows 灯まで
    ///        （超えた分はカメラに近いライトが優先され、残りは影なしで照らす）。
    bool castShadow = true;
    /// @brief 影の near クリップ（ライト位置からこの距離より近い物は遮蔽物にならない）。
    ///        ライトを持つ本体（顔の中など）が自分の光を遮らないよう、少し離しておく。
    float shadowNear = 0.3f;
    /// @brief true なら、このライトを持つエンティティ自身は遮蔽物にしない
    ///        （頭上ライトで自分の影が足元に落ちるのが邪魔な場合などに使う）。
    bool shadowExcludeSelf = false;
    /// @brief 影タイルの優先度（小さいほど優先。既定 0）
    /// @details 同時に影を落とせる灯数には上限があるので、あふれさせたくないライトほど
    ///          小さい値にする。同じ優先度どうしはカメラに近い順で選ばれる。
    int shadowPriority = 0;

    /// @brief オフセットを適用した最終的なライト位置を返す
    /// @param basePos エンティティの位置
    /// @param baseRot エンティティの回転（オイラー角、ラジアン）
    /// @return offset をエンティティの回転で回して加えたワールド座標
    RC::Vector3 ResolvePosition(const RC::Vector3& basePos, const RC::Vector3& baseRot) const {
      return ApplyLocalOffset(basePos, baseRot, offset);
    }

    /// @brief コンポーネントの型名を返す
    /// @return "SpotLightComponent"
    const char* TypeName() const override { return "SpotLightComponent"; }
    /// @brief ライトの設定を JSON に書き出す
    /// @return lightHandle 以外の設定値を格納した JSON
    nlohmann::json Serialize() const override {
      return {
        {"visible", visible},
        {"color", {color.x, color.y, color.z, color.w}},
        {"direction", {direction.x, direction.y, direction.z}},
        {"offset", {offset.x, offset.y, offset.z}},
        {"intensity", intensity}, {"distance", distance}, {"decay", decay}, {"cosAngle", cosAngle},
        {"castShadow", castShadow}, {"shadowNear", shadowNear}, {"shadowExcludeSelf", shadowExcludeSelf},
        {"shadowPriority", shadowPriority}
      };
    }
    /// @brief JSON からライトの設定を読み込む
    /// @param j 読み込み元の JSON（含まれていないキーは現在の値のまま）
    void Deserialize(const nlohmann::json& j) override {
      if (j.contains("visible")) visible = j["visible"].get<bool>();
      if (j.contains("color")) { auto& c = j["color"]; color = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>(), c[3].get<float>()}; }
      if (j.contains("direction")) { auto& d = j["direction"]; direction = {d[0].get<float>(), d[1].get<float>(), d[2].get<float>()}; }
      // offset が無い既存シーンは {0,0,0} のまま = 従来通りエンティティ中心に配置される
      if (j.contains("offset")) { auto& o = j["offset"]; offset = {o[0].get<float>(), o[1].get<float>(), o[2].get<float>()}; }
      if (j.contains("intensity")) intensity = j["intensity"].get<float>();
      if (j.contains("distance")) distance = j["distance"].get<float>();
      if (j.contains("decay")) decay = j["decay"].get<float>();
      if (j.contains("cosAngle")) cosAngle = j["cosAngle"].get<float>();
      // castShadow が無い既存シーンは true（影あり）
      if (j.contains("castShadow")) castShadow = j["castShadow"].get<bool>();
      if (j.contains("shadowNear")) shadowNear = j["shadowNear"].get<float>();
      if (j.contains("shadowExcludeSelf")) shadowExcludeSelf = j["shadowExcludeSelf"].get<bool>();
      if (j.contains("shadowPriority")) shadowPriority = j["shadowPriority"].get<int>();
    }
};

/// @struct AreaLightComponent
/// @brief 面光源（halfHeight が 0 以下なら線光源）のコンポーネント
struct AreaLightComponent : public IComponent {
    int lightHandle = -1;                            ///< ライトのハンドル（未作成なら -1。保存しない）
    bool visible = true;                             ///< 有効か
    RC::Vector4 color = { 1.0f, 1.0f, 1.0f, 1.0f };  ///< 光の色
    float intensity = 1.0f;                          ///< 光の強さ
    float range = 10.0f;                             ///< 光が届く距離
    float decay = 1.0f;                              ///< 減衰率
    float halfWidth = 1.0f;                          ///< 面の幅の半分
    /// @brief 面の高さの半分。0 以下にすると「right 方向へ ±halfWidth 伸びる線光源（Tube）」として扱われ、
    ///        線分全体から全方位に光る（紐・ネオン管など）。
    float halfHeight = 1.0f;
    bool twoSided = false; ///< 両面に光るか
    /// @brief 面の右方向（幅方向）。Tube ライトではこの方向に線分が伸びる。
    RC::Vector3 right = { 1.0f, 0.0f, 0.0f };
    /// @brief 面の上方向（高さ方向）。right と直交させること。
    RC::Vector3 up = { 0.0f, 1.0f, 0.0f };
    /// @brief 影を落とす（壁などに遮られる）か。
    /// @details 全方位に光る光源なので、キューブマップの代わりに「ライト位置から真下向きの
    ///          広角シャドウ」1 枚で遮蔽を判定する（壁が垂直である前提。詳細はシェーダの
    ///          SampleOmniShadowDown を参照）。同時に影を落とせるのは kMaxSpotShadows 灯まで
    ///          （超えた分はカメラに近い順に優先され、残りは影なしで照らす）。
    bool castShadow = true;
    /// @brief 影の near クリップ（ライトからこの距離より近い物は遮蔽物にならない）。
    /// @details 真下向きシャドウでは「ライトから水平距離 shadowNear / tan(照射半角) より近い壁」が
    ///          判定から外れてしまうので小さめにしておく。
    float shadowNear = 0.05f;
    /// @brief true なら、このライトを持つエンティティ自身は遮蔽物にしない。
    bool shadowExcludeSelf = false;
    /// @brief 影タイルの優先度（小さいほど優先。既定 0）
    /// @details 同時に影を落とせる灯数には上限があるので、あふれさせたくないライトほど
    ///          小さい値にする。同じ優先度どうしはカメラに近い順で選ばれる。
    int shadowPriority = 0;

    /// @brief コンポーネントの型名を返す
    /// @return "AreaLightComponent"
    const char* TypeName() const override { return "AreaLightComponent"; }
    /// @brief ライトの設定を JSON に書き出す
    /// @return lightHandle 以外の設定値を格納した JSON
    nlohmann::json Serialize() const override {
      return {
        {"visible", visible},
        {"color", {color.x, color.y, color.z, color.w}},
        {"intensity", intensity}, {"range", range}, {"decay", decay},
        {"halfWidth", halfWidth}, {"halfHeight", halfHeight}, {"twoSided", twoSided},
        {"right", {right.x, right.y, right.z}},
        {"up", {up.x, up.y, up.z}},
        {"castShadow", castShadow}, {"shadowNear", shadowNear}, {"shadowExcludeSelf", shadowExcludeSelf},
        {"shadowPriority", shadowPriority}
      };
    }
    /// @brief JSON からライトの設定を読み込む
    /// @param j 読み込み元の JSON（含まれていないキーは現在の値のまま）
    void Deserialize(const nlohmann::json& j) override {
      if (j.contains("visible")) visible = j["visible"].get<bool>();
      if (j.contains("color")) { auto& c = j["color"]; color = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>(), c[3].get<float>()}; }
      if (j.contains("intensity")) intensity = j["intensity"].get<float>();
      if (j.contains("range")) range = j["range"].get<float>();
      if (j.contains("decay")) decay = j["decay"].get<float>();
      if (j.contains("halfWidth")) halfWidth = j["halfWidth"].get<float>();
      if (j.contains("halfHeight")) halfHeight = j["halfHeight"].get<float>();
      if (j.contains("twoSided")) twoSided = j["twoSided"].get<bool>();
      // right / up が無い既存シーンは既定値（X 右・Y 上）のまま = 従来通り
      if (j.contains("right")) { auto& r = j["right"]; right = {r[0].get<float>(), r[1].get<float>(), r[2].get<float>()}; }
      if (j.contains("up")) { auto& u = j["up"]; up = {u[0].get<float>(), u[1].get<float>(), u[2].get<float>()}; }
      // これらが無い既存シーンは既定値（影あり）になる
      if (j.contains("castShadow")) castShadow = j["castShadow"].get<bool>();
      if (j.contains("shadowNear")) shadowNear = j["shadowNear"].get<float>();
      if (j.contains("shadowExcludeSelf")) shadowExcludeSelf = j["shadowExcludeSelf"].get<bool>();
      if (j.contains("shadowPriority")) shadowPriority = j["shadowPriority"].get<int>();
    }
};
