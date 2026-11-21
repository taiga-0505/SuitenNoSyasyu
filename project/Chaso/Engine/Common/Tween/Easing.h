#pragma once

// ============================================================================
// Easing
// ----------------------------------------------------------------------------
// 0〜1 の進み具合 t を、緩急をつけた 0〜1（Back / Elastic は少しはみ出す）に変換する関数群。
// 式は Robert Penner のイージング関数（easings.net と同じ形）。
//
//   float k = Chaso::Ease::Evaluate(Chaso::EaseType::OutBack, t);
//   pos = RC::Lerp(from, to, k);
//
// 名前の付け方:
//   In    … ゆっくり始まって速く終わる
//   Out   … 速く始まってゆっくり終わる（UI の出現・移動はまずこれ）
//   InOut … ゆっくり始まり、速くなり、ゆっくり終わる
// ============================================================================

#include <cmath>
#include <cstring>
#include <string>

namespace Chaso {

/// @enum EaseType
/// @brief イージングの種類
enum class EaseType {
  Linear,
  InSine, OutSine, InOutSine,
  InQuad, OutQuad, InOutQuad,
  InCubic, OutCubic, InOutCubic,
  InQuart, OutQuart, InOutQuart,
  InQuint, OutQuint, InOutQuint,
  InExpo, OutExpo, InOutExpo,
  InCirc, OutCirc, InOutCirc,
  InBack, OutBack, InOutBack,
  InElastic, OutElastic, InOutElastic,
  InBounce, OutBounce, InOutBounce,
  Count ///< 種類の数（番兵）
};

namespace Ease {

inline constexpr float kPi = 3.14159265358979323846f;

inline float OutBounce(float t) {
  constexpr float n1 = 7.5625f;
  constexpr float d1 = 2.75f;
  if (t < 1.0f / d1) return n1 * t * t;
  if (t < 2.0f / d1) { t -= 1.5f / d1; return n1 * t * t + 0.75f; }
  if (t < 2.5f / d1) { t -= 2.25f / d1; return n1 * t * t + 0.9375f; }
  t -= 2.625f / d1;
  return n1 * t * t + 0.984375f;
}

/// @brief イージングを計算する
/// @param type 種類
/// @param t    進み具合（0〜1 の外は 0〜1 に丸める）
/// @return 変換後の値（t = 0 で 0、t = 1 で 1）
inline float Evaluate(EaseType type, float t) {
  if (!(t > 0.0f)) return 0.0f; // NaN も 0 扱い
  if (t >= 1.0f) return 1.0f;

  constexpr float c1 = 1.70158f;
  constexpr float c2 = c1 * 1.525f;
  constexpr float c3 = c1 + 1.0f;
  constexpr float c4 = (2.0f * kPi) / 3.0f;
  constexpr float c5 = (2.0f * kPi) / 4.5f;

  switch (type) {
  case EaseType::Linear: return t;

  case EaseType::InSine: return 1.0f - std::cos(t * kPi * 0.5f);
  case EaseType::OutSine: return std::sin(t * kPi * 0.5f);
  case EaseType::InOutSine: return -(std::cos(kPi * t) - 1.0f) * 0.5f;

  case EaseType::InQuad: return t * t;
  case EaseType::OutQuad: return 1.0f - (1.0f - t) * (1.0f - t);
  case EaseType::InOutQuad:
    return t < 0.5f ? 2.0f * t * t : 1.0f - std::pow(-2.0f * t + 2.0f, 2.0f) * 0.5f;

  case EaseType::InCubic: return t * t * t;
  case EaseType::OutCubic: return 1.0f - std::pow(1.0f - t, 3.0f);
  case EaseType::InOutCubic:
    return t < 0.5f ? 4.0f * t * t * t : 1.0f - std::pow(-2.0f * t + 2.0f, 3.0f) * 0.5f;

  case EaseType::InQuart: return t * t * t * t;
  case EaseType::OutQuart: return 1.0f - std::pow(1.0f - t, 4.0f);
  case EaseType::InOutQuart:
    return t < 0.5f ? 8.0f * t * t * t * t : 1.0f - std::pow(-2.0f * t + 2.0f, 4.0f) * 0.5f;

  case EaseType::InQuint: return t * t * t * t * t;
  case EaseType::OutQuint: return 1.0f - std::pow(1.0f - t, 5.0f);
  case EaseType::InOutQuint:
    return t < 0.5f ? 16.0f * t * t * t * t * t : 1.0f - std::pow(-2.0f * t + 2.0f, 5.0f) * 0.5f;

  case EaseType::InExpo: return std::pow(2.0f, 10.0f * t - 10.0f);
  case EaseType::OutExpo: return 1.0f - std::pow(2.0f, -10.0f * t);
  case EaseType::InOutExpo:
    return t < 0.5f ? std::pow(2.0f, 20.0f * t - 10.0f) * 0.5f
                    : (2.0f - std::pow(2.0f, -20.0f * t + 10.0f)) * 0.5f;

  case EaseType::InCirc: return 1.0f - std::sqrt(1.0f - t * t);
  case EaseType::OutCirc: return std::sqrt(1.0f - (t - 1.0f) * (t - 1.0f));
  case EaseType::InOutCirc:
    return t < 0.5f ? (1.0f - std::sqrt(1.0f - 4.0f * t * t)) * 0.5f
                    : (std::sqrt(1.0f - std::pow(-2.0f * t + 2.0f, 2.0f)) + 1.0f) * 0.5f;

  case EaseType::InBack: return c3 * t * t * t - c1 * t * t;
  case EaseType::OutBack: return 1.0f + c3 * std::pow(t - 1.0f, 3.0f) + c1 * std::pow(t - 1.0f, 2.0f);
  case EaseType::InOutBack:
    return t < 0.5f
               ? (std::pow(2.0f * t, 2.0f) * ((c2 + 1.0f) * 2.0f * t - c2)) * 0.5f
               : (std::pow(2.0f * t - 2.0f, 2.0f) * ((c2 + 1.0f) * (t * 2.0f - 2.0f) + c2) + 2.0f) * 0.5f;

  case EaseType::InElastic:
    return -std::pow(2.0f, 10.0f * t - 10.0f) * std::sin((t * 10.0f - 10.75f) * c4);
  case EaseType::OutElastic:
    return std::pow(2.0f, -10.0f * t) * std::sin((t * 10.0f - 0.75f) * c4) + 1.0f;
  case EaseType::InOutElastic:
    return t < 0.5f
               ? -(std::pow(2.0f, 20.0f * t - 10.0f) * std::sin((20.0f * t - 11.125f) * c5)) * 0.5f
               : (std::pow(2.0f, -20.0f * t + 10.0f) * std::sin((20.0f * t - 11.125f) * c5)) * 0.5f + 1.0f;

  case EaseType::InBounce: return 1.0f - OutBounce(1.0f - t);
  case EaseType::OutBounce: return OutBounce(t);
  case EaseType::InOutBounce:
    return t < 0.5f ? (1.0f - OutBounce(1.0f - 2.0f * t)) * 0.5f
                    : (1.0f + OutBounce(2.0f * t - 1.0f)) * 0.5f;

  default: return t;
  }
}

/// @brief 種類の名前（エディタ表示・JSON 保存用）
/// @param type 種類
/// @return "OutQuad" などの名前
inline const char *Name(EaseType type) {
  static const char *kNames[] = {
      "Linear",
      "InSine", "OutSine", "InOutSine",
      "InQuad", "OutQuad", "InOutQuad",
      "InCubic", "OutCubic", "InOutCubic",
      "InQuart", "OutQuart", "InOutQuart",
      "InQuint", "OutQuint", "InOutQuint",
      "InExpo", "OutExpo", "InOutExpo",
      "InCirc", "OutCirc", "InOutCirc",
      "InBack", "OutBack", "InOutBack",
      "InElastic", "OutElastic", "InOutElastic",
      "InBounce", "OutBounce", "InOutBounce",
  };
  static_assert(sizeof(kNames) / sizeof(kNames[0]) == static_cast<size_t>(EaseType::Count),
                "EaseType と名前の表の数がずれている");
  const int i = static_cast<int>(type);
  return (i >= 0 && i < static_cast<int>(EaseType::Count)) ? kNames[i] : "Linear";
}

/// @brief 名前から種類を引く（見つからなければ fallback）
/// @param name     "OutQuad" など
/// @param fallback 見つからなかったときの値
inline EaseType FromName(const std::string &name, EaseType fallback = EaseType::Linear) {
  for (int i = 0; i < static_cast<int>(EaseType::Count); ++i) {
    if (name == Name(static_cast<EaseType>(i))) return static_cast<EaseType>(i);
  }
  return fallback;
}

} // namespace Ease
} // namespace Chaso
