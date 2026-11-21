#pragma once
#include <format>

#include "ECS/ScriptableEntity.h"
#include "ECS/ModelRendererComponent.h"
#include "ECS/PrimitiveMeshComponent.h"
#include "RenderCommon.h"
#include "Common/Log/Log.h"
#include "Common/GameTime.h"

#if RC_ENABLE_IMGUI
#include "imgui/imgui.h"
#endif

/// @brief 弾を当てて壊せるもの（敵・宝箱など）の共通基底クラス
/// @details 以前は EnemyBaseScript と TreasureChestScript がそれぞれ
///          「pending_damage タグの取り込み → 被弾フラッシュ → HP が 0 で撃破／開封 → 一定時間後に消滅」
///          を個別に実装していた。流れは同じで、違うのは「撃破されたら何をするか」と
///          「生存中・撃破後にどう動くか」だけなので、流れをこのクラスに固定し（Template Method）、
///          違う部分だけを派生クラスが仮想関数で差し替える（ポリモーフィズム）形にした。
///
///          毎フレームの流れ（OnUpdate、派生クラスでは上書きできない）:
///            1. 他スクリプトが "pending_damage" タグに積んだダメージを TakeDamage へ渡す
///            2. 被弾フラッシュの時間を進め、終わったら RestoreColor() で色を戻す
///            3. 生存中なら OnUpdateAlive()、撃破後なら OnUpdateDefeated() を呼ぶ
///            4. 撃破後 GetDefeatDuration() 秒経ったらエンティティを破棄する
///            5. UI 用に HP をタグ（current_hp / max_hp）で公開する
class DamageableScript : public ScriptableEntity {
public:
  /// @brief ダメージを受ける。HP が 0 以下になったら撃破処理（OnDefeated）を行う
  /// @param damage 受けるダメージ量
  /// @note 撃破済みなら何もしない。被弾フラッシュの色と長さは派生クラスが決める。
  void TakeDamage(int damage) {
    if (defeated_) return;
    hp_ -= damage;

    if (Entity* self = GetEntity()) {
      Log::Print(std::format("[{}] Took {} damage! HP left: {}/{}", self->GetName(), damage, hp_, maxHp_));
    }

    // 被弾フラッシュ
    SaveOriginalColor();
    ApplyColor(DamageFlashColor());
    flashTimer_ = DamageFlashDuration();

    if (hp_ <= 0) {
      hp_ = 0;
      defeated_ = true;
      defeatTimer_ = 0.0f;
      OnDefeated();
      ApplyTimeFx(DefeatTimeFx());
    } else {
      ApplyTimeFx(DamageTimeFx());
    }
    OnDamaged(damage);
  }

  /// @brief 被弾・撃破の瞬間に掛ける時間演出（ヒットストップ・スロー）
  /// @details 秒数が 0 の項目は使わない。ヒットストップは実行中なら長い方が残るので、連射で積み上がることはない。
  ///          倍率を 0（完全停止）にしないのは、deltaTime = 0 のフレームを「ポーズ中」とみなす
  ///          スクリプト（RailShooterController のカーソルロック解除など）があるため。
  struct TimeFx {
    float hitStop = 0.0f;       ///< ヒットストップの長さ（実時間の秒）
    float hitStopScale = 0.02f; ///< ヒットストップ中の倍率（見た目はほぼ停止。0 にはしないこと）
    float slowScale = 1.0f;    ///< スロー倍率（1 ならスローしない）
    float slowDuration = 0.0f; ///< スローの長さ（実時間の秒）
    float slowRecover = 0.0f;  ///< 終わりの何秒で等速へ戻すか
  };

  /// @brief 現在の HP
  /// @return 現在の HP
  int GetHp() const { return hp_; }

  /// @brief 最大 HP
  /// @return 最大 HP
  int GetMaxHp() const { return maxHp_; }

  /// @brief 撃破済み（宝箱なら開封済み）か
  /// @return 撃破済みなら true
  bool IsDefeated() const { return defeated_; }

  /// @brief インスペクタに HP の編集欄を表示する
  void OnImGui() override {
#if RC_ENABLE_IMGUI
    ImGui::DragInt("HP##Damageable", &hp_, 1, 0, maxHp_);
    ImGui::DragInt("Max HP##Damageable", &maxHp_, 1, 1, 1000);
    ImGui::Text("Defeated: %s", defeated_ ? "yes" : "no");
#endif
  }

protected:
  // ------------------------------------------------------------------
  // Template Method（流れは固定。派生クラスは下の仮想関数だけを差し替える）
  // ------------------------------------------------------------------

  /// @brief 被弾・フラッシュ・生存中／撃破後の更新・消滅・HP 公開を行う
  /// @param deltaTime 前フレームからの経過秒数
  void OnUpdate(float deltaTime) final {
    Entity* self = GetEntity();
    if (!self) return;

    // 1. 他スクリプトが積んだダメージを取り出して反映する
    const int dmg = self->GetTagInt("pending_damage", 0);
    if (dmg > 0) {
      self->ClearTag("pending_damage");
      TakeDamage(dmg);
    }

    // 2. 被弾フラッシュの時間を進め、終わったら元の色に戻す
    if (flashTimer_ > 0.0f) {
      flashTimer_ -= deltaTime;
      if (flashTimer_ <= 0.0f) {
        RestoreColor();
      }
    }

    // 3. 生存中／撃破後の振る舞いは派生クラスに任せる
    if (!defeated_) {
      OnUpdateAlive(deltaTime);
    } else {
      defeatTimer_ += deltaTime;
      OnUpdateDefeated(deltaTime);
      // 4. 撃破演出が終わったら実体を消す
      if (defeatTimer_ >= defeatDuration_) {
        self->Destroy();
      }
    }

    // 5. HP 情報をタグで公開（UI 用）
    if (!defeated_ || PublishesHpAfterDefeat()) {
      self->SetTag("current_hp", hp_);
      self->SetTag("max_hp", maxHp_);
    }
  }

  /// @brief 生存中の毎フレーム処理
  /// @param deltaTime 前フレームからの経過秒数
  virtual void OnUpdateAlive(float deltaTime) { (void)deltaTime; }

  /// @brief 撃破後（消滅するまで）の毎フレーム処理。沈む・崩れる・開くなどの演出に使う
  /// @param deltaTime 前フレームからの経過秒数
  virtual void OnUpdateDefeated(float deltaTime) { (void)deltaTime; }

  /// @brief HP が 0 になった瞬間に 1 回だけ呼ばれる。撃破時に何をするかは派生クラスが必ず決める
  virtual void OnDefeated() = 0;

  /// @brief ダメージを受けるたびに呼ばれる（HP の更新・撃破処理のあと）
  /// @param damage 受けたダメージ量
  virtual void OnDamaged(int damage) { (void)damage; }

  /// @brief 被弾フラッシュの色
  /// @return フラッシュ色（既定は赤）
  virtual RC::Vector4 DamageFlashColor() const { return {1.0f, 0.2f, 0.2f, 1.0f}; }

  /// @brief 被弾フラッシュの長さ
  /// @return 秒数（既定 0.2 秒）
  virtual float DamageFlashDuration() const { return 0.2f; }

  /// @brief 撃破されずに被弾したときの時間演出
  /// @return 既定は何もしない（連射の弾が当たるたびに止まると操作感が悪くなるため）
  virtual TimeFx DamageTimeFx() const { return {}; }

  /// @brief 撃破（宝箱なら開封）した瞬間の時間演出
  /// @return 既定は何もしない。派生クラスが手応えに合わせて決める
  virtual TimeFx DefeatTimeFx() const { return {}; }

  /// @brief 撃破後も HP タグ（current_hp / max_hp）を公開し続けるか
  /// @return 公開し続けるなら true（既定 true）
  virtual bool PublishesHpAfterDefeat() const { return true; }

  // ------------------------------------------------------------------
  // 色（被弾フラッシュ用）
  // ------------------------------------------------------------------

  /// @brief 被弾フラッシュ前の元の色を保存する（初回のみ）
  /// @note ModelRenderer を優先し、無ければ PrimitiveMesh のマテリアル色を使う。
  void SaveOriginalColor() {
    if (hasSavedColor_) return;
    Entity* self = GetEntity();
    if (!self) return;
    if (auto* mr = self->GetComponent<ModelRendererComponent>()) {
      originalColor_ = mr->color;
      hasSavedColor_ = true;
    } else if (auto* pm = self->GetComponent<PrimitiveMeshComponent>()) {
      if (pm->meshHandle >= 0) {
        if (auto* mat = RC::GetPrimitiveMeshMaterialPtr(pm->meshHandle)) {
          originalColor_ = mat->color;
          hasSavedColor_ = true;
        }
      }
    }
  }

  /// @brief 自身の描画色を変更する。部位を持つ派生クラスは上書きして部位にも反映する
  /// @param color 設定する色（RGBA）
  virtual void ApplyColor(const RC::Vector4& color) {
    Entity* self = GetEntity();
    if (!self) return;
    if (auto* mr = self->GetComponent<ModelRendererComponent>()) {
      mr->color = color;
    } else if (auto* pm = self->GetComponent<PrimitiveMeshComponent>()) {
      if (pm->meshHandle >= 0) {
        if (auto* mat = RC::GetPrimitiveMeshMaterialPtr(pm->meshHandle)) {
          mat->color = color;
        }
      }
    }
  }

  /// @brief 保存しておいた元の色に戻す（未保存なら何もしない）
  virtual void RestoreColor() {
    if (hasSavedColor_) {
      ApplyColor(originalColor_);
    }
  }

  /// @brief 保存済みの元の色
  /// @return 元の色（未保存なら白）
  const RC::Vector4& GetOriginalColor() const { return originalColor_; }

  /// @brief 被弾フラッシュの残り秒数
  /// @return 残り秒数（0 以下ならフラッシュしていない）
  float GetFlashTimer() const { return flashTimer_; }

  // ------------------------------------------------------------------
  // HP・撃破演出時間の設定
  // ------------------------------------------------------------------

  /// @brief データ（JSON / スポナー）で HP が指定されていなければ既定の HP を入れる
  /// @param defaultHp 派生クラスごとの既定 HP
  /// @details 派生クラスは OnCreate で既定 HP を入れることが多いが、無条件に代入すると
  ///          データ側の指定を潰してしまうため、指定が無いときだけ反映する。
  void SetDefaultHp(int defaultHp) {
    if (hpFromData_) return;
    hp_ = defaultHp;
    maxHp_ = defaultHp;
  }

  /// @brief HP と最大 HP を直接設定する（値の補正用）
  /// @param hp 現在の HP
  /// @param maxHp 最大 HP
  void SetHpValues(int hp, int maxHp) {
    hp_ = hp;
    maxHp_ = maxHp;
  }

  /// @brief HP がデータから与えられたか
  /// @return JSON / スポナーで hp か maxHp が指定されていれば true
  bool IsHpFromData() const { return hpFromData_; }

  /// @brief 撃破から消滅までの秒数を設定する
  /// @param seconds 秒数
  void SetDefeatDuration(float seconds) { defeatDuration_ = seconds; }

  /// @brief 撃破から消滅までの秒数
  /// @return 秒数
  float GetDefeatDuration() const { return defeatDuration_; }

  /// @brief 撃破されてからの経過秒数
  /// @return 経過秒数（未撃破なら 0）
  float GetDefeatTimer() const { return defeatTimer_; }

  /// @brief 撃破演出の進み具合
  /// @return 0（撃破直後）〜 1（消滅直前）
  float GetDefeatProgress() const {
    if (defeatDuration_ <= 0.0f) return 1.0f;
    const float t = defeatTimer_ / defeatDuration_;
    return (t < 0.0f) ? 0.0f : (t > 1.0f ? 1.0f : t);
  }

  /// @brief 撃破状態を初期化する（再利用時など）
  void ResetDefeatState() {
    defeated_ = false;
    defeatTimer_ = 0.0f;
  }

  // ------------------------------------------------------------------
  // シリアライズ
  // ------------------------------------------------------------------

  /// @brief HP を JSON へ書き出す
  /// @return hp / maxHp を格納した json（派生クラスはこれに自分の項目を足す）
  nlohmann::json Serialize() override {
    nlohmann::json j;
    j["hp"] = hp_;
    j["maxHp"] = maxHp_;
    return j;
  }

  /// @brief JSON から HP を読み込む
  /// @param j 読み込み元の json（hp / maxHp は任意）
  /// @note maxHp だけ指定された場合は hp も満タンとして揃える（ウェーブ定義に "maxHp" とだけ書けるように）。
  void Deserialize(const nlohmann::json& j) override {
    if (j.contains("maxHp")) {
      maxHp_ = j["maxHp"].get<int>();
      hp_ = maxHp_;
      hpFromData_ = true;
    }
    if (j.contains("hp")) {
      hp_ = j["hp"].get<int>();
      if (!j.contains("maxHp")) maxHp_ = hp_;
      hpFromData_ = true;
    }
  }

private:
  /// @brief 時間演出を Chaso::Time へ渡す
  /// @param fx 掛ける演出（秒数が 0 の項目は無視する）
  static void ApplyTimeFx(const TimeFx& fx) {
    auto& time = Chaso::Time::Get();
    if (fx.slowDuration > 0.0f && fx.slowScale < 1.0f) {
      time.SlowMotion(fx.slowScale, fx.slowDuration, fx.slowRecover);
    }
    // 止まっている最中（自機の被弾・撃破など）は上書きしない（HitStop は新しい倍率で上書きするため、弱い止めで緩むのを防ぐ）
    if (fx.hitStop > 0.0f && !time.IsHitStopping()) time.HitStop(fx.hitStop, fx.hitStopScale);
  }

  int hp_ = 10;                 ///< 現在の HP
  int maxHp_ = 10;              ///< 最大 HP
  bool hpFromData_ = false;     ///< HP が JSON / スポナーから与えられたか
  bool defeated_ = false;       ///< 撃破済みか
  float defeatTimer_ = 0.0f;    ///< 撃破されてからの経過秒数
  float defeatDuration_ = 2.0f; ///< 撃破から消滅までの秒数
  float flashTimer_ = 0.0f;     ///< 被弾フラッシュの残り秒数
  RC::Vector4 originalColor_ = {1.0f, 1.0f, 1.0f, 1.0f}; ///< フラッシュ前の元の色
  bool hasSavedColor_ = false;  ///< originalColor_ を保存済みか
};
