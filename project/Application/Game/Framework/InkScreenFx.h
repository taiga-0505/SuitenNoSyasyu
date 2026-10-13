#pragma once
#include "Engine/Graphics/PostProcess/PostProcess.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cmath>

#if RC_ENABLE_IMGUI
#include "imgui/imgui.h"
#endif

/// @brief タコの墨がレンズに貼り付いた状態（InkOverlay ポストエフェクト）を管理する
/// @details 使い方：
///   - 墨を当てる側（InkBlobScript）はプレイヤー（メインカメラ）に
///     `pending_ink` タグ（強さ×100 の整数）を積むだけ。
///   - プレイヤー側（RailShooterController）が毎フレームそれを拾って AddHit() し、Update() を呼ぶ。
///
///   1 発ごとに墨を 1 個貼る。貼った直後は holdDuration 秒まったく薄れず、
///   その後 fadeDuration 秒かけて縁から崩れて消える。
///   続けて当たると別の位置に重なって貼られるので、連続で食らうほど視界が塞がる。
///   同時に貼れる数（PostProcess::kMaxInkSplats）を超えたら、いちばん弱っているものを置き換える。
struct InkScreenFx {
  // ---- 調整値（JSON キー "ink"） ----
  float holdDuration = 1.6f;   ///< 貼り付いてから薄れ始めるまで（秒）
  float fadeDuration = 2.6f;   ///< 薄れ始めてから消えきるまで（秒）
  float underwaterFadeScale = 1.35f; ///< 水中では墨が水に溶けて早く消える（倍率）
  float radiusMin = 0.26f;     ///< 墨の半径（画面の高さ=1）
  float radiusMax = 0.40f;     ///< 墨の半径の上限（画面の高さ=1）
  float centerSpread = 0.22f;  ///< 画面中央からどれだけずらして貼るか（UV）
  float murkPerSplat = 0.22f;  ///< 墨 1 個あたりの画面全体の濁り
  float murkMax = 0.55f;       ///< 画面全体の濁りの上限
  float inkColor[3] = {0.03f, 0.02f, 0.05f}; ///< 墨の色（RGB）
  float inkOpacity = 0.96f;    ///< 墨の不透明度

  /// @brief 墨を 1 個貼る
  /// @param amount 強さ 0..1（1 でフルサイズ）。小さい墨ほど小さく貼る
  void AddHit(float amount) {
    amount = std::clamp(amount, 0.1f, 1.0f);

    // 空きが無ければ、いちばん弱っている墨を置き換える
    int slot = count_;
    if (count_ >= PostProcess::kMaxInkSplats) {
      slot = 0;
      for (int i = 1; i < count_; ++i) {
        if (Remaining(i) < Remaining(slot)) slot = i;
      }
    } else {
      ++count_;
    }

    // 位置と形は黄金比の低食い違い列で決める（乱数だと同じ場所に重なりやすい）
    seedCursor_ += 0.6180339887f;
    seedCursor_ -= std::floor(seedCursor_);
    const float a = seedCursor_ * 6.2831853f;
    // 1 発目はほぼ中央（いちばん視界を奪う場所）、重なるほど外へ散らす
    const float spread = centerSpread * (count_ <= 1 ? 0.35f : 1.0f);
    const float rr = spread * std::sqrt(Frac(seedCursor_ * 7.13f));

    PostProcess::InkSplat &s = splats_[slot];
    s.centerX = 0.5f + std::cos(a) * rr * 0.8f;
    s.centerY = 0.46f + std::sin(a) * rr * 0.6f; // 少し上寄り（垂れ筋が下へ伸びる余白）
    s.radius = (radiusMin + (radiusMax - radiusMin) * Frac(seedCursor_ * 3.71f)) *
               (0.55f + 0.45f * amount);
    s.strength = 1.0f;
    s.age = 0.0f;
    s.seed = Frac(seedCursor_ * 11.3f + 0.17f);

    kick_ = 1.0f;
  }

  /// @brief 毎フレーム呼ぶ。寿命を進めて PostProcess に流し込む
  /// @param pp         反映先の PostProcess（nullptr なら寿命だけ進める）
  /// @param dt         経過時間（秒）
  /// @param underwater 水中にいるか（true なら underwaterFadeScale 倍の速さで薄れる）
  void Update(PostProcess *pp, float dt, bool underwater) {
    // 各墨の寿命を進める。holdDuration を過ぎたものだけ薄れさせ、消えたものは末尾と入れ替えて詰める
    const float fadeMul = underwater ? underwaterFadeScale : 1.0f;
    for (int i = 0; i < count_;) {
      PostProcess::InkSplat &s = splats_[i];
      s.age += dt;
      if (s.age > holdDuration) {
        s.strength -= dt * fadeMul / (std::max)(fadeDuration, 0.01f);
      }
      if (s.strength <= 0.0f) {
        splats_[i] = splats_[count_ - 1];
        --count_;
        continue;
      }
      ++i;
    }

    // 画面全体の濁り：墨の強さの合計に比例。貼られた瞬間だけ少し強く
    float sum = 0.0f;
    for (int i = 0; i < count_; ++i) sum += splats_[i].strength;
    kick_ = (std::max)(0.0f, kick_ - dt * 2.5f);
    const float murk = (std::min)(murkMax, sum * murkPerSplat + kick_ * 0.25f);

    // 墨か濁りが残っている間は InkOverlay を有効にして値を渡し、無くなったら外す
    if (!pp) return;
    if (count_ > 0 || murk > 0.001f) {
      // 墨はレンズの表面に付いているので、必ず最後に掛ける。
      // 途中にあると、あとから積まれる Underwater（歪み・青み）や Caustics（光の網目）が
      // 墨の上にまで描かれてしまう。水中の出入りでそれらが積み直されるたびに末尾へ戻す。
      const auto &effects = pp->GetEffects();
      if (effects.empty() || effects.back() != PostEffectType::InkOverlay) {
        pp->RemoveEffect(PostEffectType::InkOverlay);
        pp->AddEffect(PostEffectType::InkOverlay);
      }
      pp->SetInkOverlayColor(inkColor[0], inkColor[1], inkColor[2], inkOpacity);
      pp->SetInkOverlaySplats(splats_.data(), count_);
      pp->SetInkOverlayMurk(murk);
      active_ = true;
    } else if (active_) {
      pp->SetInkOverlaySplats(nullptr, 0);
      pp->SetInkOverlayMurk(0.0f);
      pp->RemoveEffect(PostEffectType::InkOverlay);
      active_ = false;
    }
  }

  /// @brief 全部消す（シーンを抜けるとき・リトライ時）
  /// @param pp 反映先の PostProcess（nullptr なら内部状態だけ消す）
  void Clear(PostProcess *pp) {
    count_ = 0;
    kick_ = 0.0f;
    if (pp) {
      pp->SetInkOverlaySplats(nullptr, 0);
      pp->SetInkOverlayMurk(0.0f);
      pp->RemoveEffect(PostEffectType::InkOverlay);
    }
    active_ = false;
  }

  /// @brief 現在貼られている墨の数
  /// @return 墨の個数（0〜PostProcess::kMaxInkSplats）
  int Count() const { return count_; }

  /// @brief 調整値を JSON に変換する
  /// @return 調整値を持つ JSON オブジェクト
  nlohmann::json ToJson() const {
    return {
        {"holdDuration", holdDuration},
        {"fadeDuration", fadeDuration},
        {"underwaterFadeScale", underwaterFadeScale},
        {"radiusMin", radiusMin},
        {"radiusMax", radiusMax},
        {"centerSpread", centerSpread},
        {"murkPerSplat", murkPerSplat},
        {"murkMax", murkMax},
        {"inkColor", {inkColor[0], inkColor[1], inkColor[2]}},
        {"inkOpacity", inkOpacity},
    };
  }

  /// @brief JSON から調整値を読み込む
  /// @param j 読み込む JSON（無いキー・型が違うキーは現在の値のまま）
  void FromJson(const nlohmann::json &j) {
    auto f = [&](const char *k, float &o) { if (j.contains(k) && j[k].is_number()) o = j[k].get<float>(); };
    f("holdDuration", holdDuration);
    f("fadeDuration", fadeDuration);
    f("underwaterFadeScale", underwaterFadeScale);
    f("radiusMin", radiusMin);
    f("radiusMax", radiusMax);
    f("centerSpread", centerSpread);
    f("murkPerSplat", murkPerSplat);
    f("murkMax", murkMax);
    f("inkOpacity", inkOpacity);
    if (j.contains("inkColor") && j["inkColor"].is_array() && j["inkColor"].size() == 3) {
      for (int i = 0; i < 3; ++i) inkColor[i] = j["inkColor"][i].get<float>();
    }
  }

#if RC_ENABLE_IMGUI
  /// @brief 調整値の編集 UI とテスト用の被弾ボタンを ImGui に描画する
  void DrawImGui() {
    ImGui::Text("Ink splats: %d / %d", count_, PostProcess::kMaxInkSplats);
    ImGui::DragFloat("Ink Hold (s)", &holdDuration, 0.05f, 0.0f, 10.0f);
    ImGui::DragFloat("Ink Fade (s)", &fadeDuration, 0.05f, 0.1f, 10.0f);
    ImGui::DragFloat("Ink Underwater Fade x", &underwaterFadeScale, 0.01f, 0.1f, 5.0f);
    ImGui::DragFloat("Ink Radius Min", &radiusMin, 0.005f, 0.05f, 1.0f);
    ImGui::DragFloat("Ink Radius Max", &radiusMax, 0.005f, 0.05f, 1.0f);
    ImGui::DragFloat("Ink Center Spread", &centerSpread, 0.005f, 0.0f, 0.5f);
    ImGui::DragFloat("Ink Murk / Splat", &murkPerSplat, 0.01f, 0.0f, 1.0f);
    ImGui::DragFloat("Ink Murk Max", &murkMax, 0.01f, 0.0f, 1.0f);
    ImGui::ColorEdit3("Ink Color", inkColor);
    ImGui::SliderFloat("Ink Opacity", &inkOpacity, 0.0f, 1.0f);
    if (ImGui::Button("Test Ink Hit (墨を1発くらう)")) AddHit(1.0f);
  }
#endif

private:
  /// @brief 小数部を返す
  /// @param x 入力値
  /// @return x - floor(x)（0 以上 1 未満）
  static float Frac(float x) { return x - std::floor(x); }
  /// @brief 墨の残り寿命（フェード時間を 1 とした単位）。貼りたてほど大きい
  /// @param i 墨のスロット番号
  /// @return 残り寿命
  float Remaining(int i) const {
    const auto &s = splats_[i];
    return s.strength + (std::max)(0.0f, holdDuration - s.age) / (std::max)(fadeDuration, 0.01f);
  }

  std::array<PostProcess::InkSplat, PostProcess::kMaxInkSplats> splats_{};
  int count_ = 0;
  float seedCursor_ = 0.0f;
  float kick_ = 0.0f;
  bool active_ = false;
};
