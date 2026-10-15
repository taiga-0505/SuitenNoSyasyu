#pragma once

#include "IComponent.h"
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

/// @struct AnimationClip
/// @brief 名前付きアニメーションクリップ
/// @details Inspector で "Idle" "Walk" "Attack" のように登録しておき、
///          スクリプトからは名前で再生を指示する。
struct AnimationClip {
  std::string name;       ///< クリップ名（スクリプトから指定する識別子）
  std::string path;       ///< アニメーションファイル（空 = ModelRenderer の modelPath）
  int index = 0;          ///< ファイル内のクリップ番号（glTF は複数持てる）
  bool loop = true;       ///< false なら 1 回再生して defaultClip へ戻る
  float speed = 1.0f;     ///< このクリップ固有の再生速度（AnimationComponent::speed に乗算）
};

/// @class AnimationComponent
/// @brief アニメーションを制御するコンポーネント
/// @details ModelRendererComponent と組み合わせて使う。
///          再生・停止、速度、クリップの選択、スケルトンのデバッグ表示を扱う。
///
///          再生の条件付けには 2 通りの使い方がある。
///          1. 再生/停止と速度だけ変えたい場合は playing / speed を直接触る。
///          2. クリップを切り替えたい場合は clips に登録して PlayClip() を呼ぶ。
///             実際の切り替え（クロスフェード）はシーンの更新側で行われる。
///
/// @code
/// void Player::OnUpdate(float dt) {
///   auto* anim = GetComponent<AnimationComponent>();
///   if (!anim) return;
///   if (attacking_)              anim->PlayClip("Attack");   // loop=false なら勝手に戻る
///   else if (Length(velocity_) > 0.01f) anim->PlayClip("Walk");
///   else                         anim->PlayClip("Idle");
/// }
/// @endcode
class AnimationComponent : public IComponent {
public:
  bool playing = true;         ///< 再生中か
  float speed = 1.0f;          ///< 再生速度（1.0 = 等速）
  bool showSkeleton = false;   ///< スケルトンをデバッグ表示するか
  std::string animationPath;   ///< 外部アニメーションファイルのパス（空ならモデル内蔵のものを使う）
  int animIndex = 0;           ///< ファイル内のクリップ番号（glTF は複数のアニメーションを持てる）

  std::vector<AnimationClip> clips; ///< 名前付きクリップ一覧（空ならクリップ機能は使わない）
  std::string defaultClip;          ///< 起動時に再生し、ワンショット終了後に戻るクリップ名

  // --- 実行時の状態（シリアライズしない） ---
  bool attached_ = false;      ///< アニメーションをアタッチ済みか（内部用）
  std::string appliedPath_;    ///< 実際にアタッチしたパス（内部用）
  int appliedIndex_ = -1;      ///< 実際にアタッチしたクリップ番号（内部用）
  int clipCount_ = -1;         ///< クリップ数のキャッシュ（-1 = 未取得。内部用）

  std::string currentClip;     ///< 現在再生中のクリップ名（内部用）
  std::string pendingClip_;    ///< 次の更新で切り替えるクリップ名（内部用）
  bool pendingRequested_ = false; ///< 切り替え要求があるか（内部用）
  float pendingBlend_ = 0.15f; ///< 切り替え時のブレンド秒数（内部用）
  float clipElapsed_ = 0.0f;   ///< 現在のクリップの経過時間。ワンショット判定用（内部用）

  /// @brief クリップを名前で再生する
  /// @param name clips に登録したクリップ名
  /// @param blendDuration クロスフェード秒数（0 で即切り替え）
  /// @param force 同じクリップを再生中でも頭から再生し直す（攻撃の連打など）
  /// @details 毎フレーム同じ名前で呼んでも構わない。切り替えが必要な時だけ処理される。
  void PlayClip(const std::string& name, float blendDuration = 0.15f, bool force = false) {
    if (!force && name == currentClip && !pendingRequested_) return;
    if (!force && pendingRequested_ && name == pendingClip_) return;
    pendingClip_ = name;
    pendingBlend_ = blendDuration;
    pendingRequested_ = true;
  }

  /// @brief 登録済みクリップを名前で探す
  /// @param name クリップ名
  /// @return 見つかればポインタ、無ければ nullptr
  const AnimationClip* FindClip(const std::string& name) const {
    if (name.empty()) return nullptr;
    for (const auto& c : clips) {
      if (c.name == name) return &c;
    }
    return nullptr;
  }

  /// @brief 現在の設定とアタッチ済みの内容がずれているか
  /// @return アタッチし直しが必要なら true
  bool NeedsReattach() const {
    return !attached_ || appliedPath_ != animationPath || appliedIndex_ != animIndex;
  }

  /// @brief アタッチ済みの内容を記録する（再アタッチ直後に呼ぶ）
  void MarkAttached() {
    attached_ = true;
    appliedPath_ = animationPath;
    appliedIndex_ = animIndex;
  }

  /// @brief 設定を変更した時に呼ぶ。次の更新で再アタッチされる
  void MarkDirty() {
    attached_ = false;
    clipCount_ = -1;
  }

  /// @brief コンポーネントの型名を返す
  /// @return "AnimationComponent"
  const char* TypeName() const override { return "AnimationComponent"; }

  /// @brief 再生設定とクリップ一覧を JSON に書き出す
  /// @return 再生設定・clips・defaultClip を格納した JSON（実行時の状態は含まない）
  nlohmann::json Serialize() const override {
    nlohmann::json clipArray = nlohmann::json::array();
    for (const auto& c : clips) {
      clipArray.push_back({
        {"name", c.name},
        {"path", c.path},
        {"index", c.index},
        {"loop", c.loop},
        {"speed", c.speed}
      });
    }
    return {
      {"playing", playing},
      {"speed", speed},
      {"showSkeleton", showSkeleton},
      {"animationPath", animationPath},
      {"animIndex", animIndex},
      {"clips", clipArray},
      {"defaultClip", defaultClip}
    };
  }

  /// @brief JSON から再生設定とクリップ一覧を読み込む
  /// @param j 読み込み元の JSON（含まれていないキーは現在の値のまま。clips は配列があれば丸ごと置き換える）
  /// @note 読み込み後は再生状態をリセットし、次の更新で再アタッチさせる。
  void Deserialize(const nlohmann::json& j) override {
    if (j.contains("playing")) playing = j["playing"].get<bool>();
    if (j.contains("speed")) speed = j["speed"].get<float>();
    if (j.contains("showSkeleton")) showSkeleton = j["showSkeleton"].get<bool>();
    if (j.contains("animationPath")) animationPath = j["animationPath"].get<std::string>();
    if (j.contains("animIndex")) animIndex = j["animIndex"].get<int>();
    if (j.contains("defaultClip")) defaultClip = j["defaultClip"].get<std::string>();
    if (j.contains("clips") && j["clips"].is_array()) {
      clips.clear();
      for (const auto& jc : j["clips"]) {
        AnimationClip c;
        if (jc.contains("name")) c.name = jc["name"].get<std::string>();
        if (jc.contains("path")) c.path = jc["path"].get<std::string>();
        if (jc.contains("index")) c.index = jc["index"].get<int>();
        if (jc.contains("loop")) c.loop = jc["loop"].get<bool>();
        if (jc.contains("speed")) c.speed = jc["speed"].get<float>();
        clips.push_back(c);
      }
    }
    // 読み込み直後は必ずアタッチし直す
    currentClip.clear();
    pendingRequested_ = false;
    clipElapsed_ = 0.0f;
    MarkDirty();
  }
};
