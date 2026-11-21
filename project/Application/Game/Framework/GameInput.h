#pragma once
#include "Common/ResourcePath.h"
#include "Input/Input.h"
#include "Input/InputActions.h"
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

/// @brief 『水天の射手』の入力アクション（論理名）と既定の割り当て
/// @details スクリプトはキーコードを直接読まず、ここで決めたアクション名で Chaso::InputActions を読む。
///          既定の割り当ては GameSetup.cpp が起動時に DefineDefaults() で登録し、
///          プレイヤーが変えた割り当ては Resources/Setting/InputActions.json に保存される
///          （App が起動時に読み込み、Define より優先される）。
///
///          キーコンフィグで変えられるのは Rebindable() の操作だけ。メニューの上下左右や移動は
///          複数のキー（矢印と WASD）を同時に持つので、1 キーへ置き換えると不便になるため固定にしている。
///          マウスの移動量・カーソル位置（メニューのホバー）、右スティックの視点操作は
///          アクションにせず、今までどおり Input から直接読む（割り当てを変える物ではないため）。
namespace GameInput {

// ---- アクション名 ----
inline constexpr const char *kConfirm = "Confirm";       ///< 決定
inline constexpr const char *kBack = "Back";             ///< 戻る
inline constexpr const char *kPause = "Pause";           ///< ポーズメニューの開閉
inline constexpr const char *kMenuUp = "MenuUp";         ///< メニューの上
inline constexpr const char *kMenuDown = "MenuDown";     ///< メニューの下
inline constexpr const char *kMenuLeft = "MenuLeft";     ///< メニューの左（値を減らす）
inline constexpr const char *kMenuRight = "MenuRight";   ///< メニューの右（値を増やす）
inline constexpr const char *kMoveX = "MoveX";           ///< 移動（左右。-1〜1）
inline constexpr const char *kMoveY = "MoveY";           ///< 移動（前後。-1〜1）
inline constexpr const char *kFire = "Fire";             ///< 射撃
inline constexpr const char *kWeaponPrev = "WeaponPrev"; ///< 前の武器
inline constexpr const char *kWeaponNext = "WeaponNext"; ///< 次の武器

/// @brief キーコンフィグ画面に並べるアクション
struct RebindableAction {
  const char *action; ///< アクション名
  const char *label;  ///< 画面に出す名前
  bool allowMouse;    ///< マウスボタン・ホイールを割り当ててよいか（ポーズ・決定・戻るを左クリックにすると撃つたびに反応するため不可）
};

/// @brief キーコンフィグで変えられるアクション（画面の並び順）
inline const std::vector<RebindableAction> &Rebindable() {
  static const std::vector<RebindableAction> kList = {
      {kFire, "射撃", true},
      {kWeaponPrev, "前の武器", true},
      {kWeaponNext, "次の武器", true},
      {kPause, "ポーズ", false},
      {kConfirm, "決定", false},
      {kBack, "戻る", false},
  };
  return kList;
}

/// @brief 既定の割り当てを登録する（GameSetup.cpp の静的初期化から 1 回だけ呼ぶ）
inline void DefineDefaults() {
  auto &in = Chaso::InputActions::Get();
  in.Define(kConfirm, {"Key:SPACE", "Key:ENTER", "Pad:A"});
  in.Define(kBack, {"Key:ESCAPE", "Pad:B"});
  in.Define(kPause, {"Key:ESCAPE", "Pad:START"});
  // スティックは片側だけ（LY+ など）を割り当てると、倒した瞬間が IsTriggered になる
  in.Define(kMenuUp, {"Key:UP", "Key:W", "Pad:UP", "Pad:LY+"});
  in.Define(kMenuDown, {"Key:DOWN", "Key:S", "Pad:DOWN", "Pad:LY-"});
  in.Define(kMenuLeft, {"Key:LEFT", "Key:A", "Pad:LEFT", "Pad:LX-"});
  in.Define(kMenuRight, {"Key:RIGHT", "Key:D", "Pad:RIGHT", "Pad:LX+"});
  in.Define(kMoveX, {"Key:D", "-Key:A", "Key:RIGHT", "-Key:LEFT", "Pad:RIGHT", "-Pad:LEFT", "Pad:LX"});
  in.Define(kMoveY, {"Key:W", "-Key:S", "Key:UP", "-Key:DOWN", "Pad:UP", "-Pad:DOWN", "Pad:LY"});
  in.Define(kFire, {"Mouse:Left", "Pad:RT"});
  in.Define(kWeaponPrev, {"Mouse:WheelUp", "Pad:LB"});
  in.Define(kWeaponNext, {"Mouse:WheelDown", "Pad:RB"});
}

/// @brief アクションを読む窓口（Chaso::InputActions::Get() の短縮）
inline Chaso::InputActions &Actions() { return Chaso::InputActions::Get(); }

/// @brief 押した瞬間か
inline bool Triggered(const char *action) { return Actions().IsTriggered(action); }

/// @brief 押しているか
inline bool Pressed(const char *action) { return Actions().IsPressed(action); }

/// @brief 上下（または左右）の 2 アクションを -1 / 0 / +1 にまとめる（押した瞬間だけ）
/// @param negative -1 側のアクション（上・左）
/// @param positive +1 側のアクション（下・右）
/// @return このフレームに押された向き
inline int TriggeredAxis(const char *negative, const char *positive) {
  return (Triggered(positive) ? 1 : 0) - (Triggered(negative) ? 1 : 0);
}

/// @brief 上下（または左右）の 2 アクションを -1 / 0 / +1 にまとめる（押している間）
inline int PressedAxis(const char *negative, const char *positive) {
  return (Pressed(positive) ? 1 : 0) - (Pressed(negative) ? 1 : 0);
}

/// @brief 割り当てがマウス（ボタン・ホイール）か
inline bool IsMouse(const Chaso::InputBinding &b) {
  return b.device == Chaso::InputBinding::Device::Mouse || b.device == Chaso::InputBinding::Device::MouseWheel;
}

/// @brief 割り当てがキーボード・マウス側か（パッド側でないか）
inline bool IsKeyboardMouse(const Chaso::InputBinding &b) {
  return b.device == Chaso::InputBinding::Device::Key || IsMouse(b);
}

/// @brief 条件に合う割り当てだけで「押しているか」を調べる
/// @details InputActions と同じ規則（和を -1〜1 に丸め、|値| >= 0.5 で押している。
///          スティック・トリガーはデッドゾーンを掛ける）で、accept が true の割り当てだけを数える。
/// @param action アクション名
/// @param accept 数える割り当てを選ぶ関数（bool(const Chaso::InputBinding&)）
/// @return 選んだ割り当てで押していれば true
template <class Pred> bool PressedIf(const char *action, Pred accept) {
  const Input *input = Input::GetInstance();
  if (!input) return false;
  const auto &actions = Actions();
  float sum = 0.0f;
  for (const auto &b : actions.GetBindings(action)) {
    if (!accept(b)) continue;
    float v = 0.0f;
    switch (b.device) {
    case Chaso::InputBinding::Device::Mouse:
      v = input->IsMousePressed(b.code) ? 1.0f : 0.0f;
      break;
    case Chaso::InputBinding::Device::MouseWheel: {
      const long z = static_cast<long>(input->GetMouseZ());
      v = ((b.code > 0 && z > 0) || (b.code < 0 && z < 0)) ? 1.0f : 0.0f;
      break;
    }
    case Chaso::InputBinding::Device::Key:
      v = input->IsKeyPressed(static_cast<uint8_t>(b.code)) ? 1.0f : 0.0f;
      break;
    case Chaso::InputBinding::Device::PadButton:
      v = input->IsXInputButtonPressed(static_cast<WORD>(b.code)) ? 1.0f : 0.0f;
      break;
    case Chaso::InputBinding::Device::PadAxis: {
      const auto stick = [](SHORT s) { return (std::max)(-1.0f, static_cast<float>(s) / 32767.0f); };
      switch (b.code) {
      case Chaso::InputBinding::LX: v = stick(input->GetXInputThumbLX()); break;
      case Chaso::InputBinding::LY: v = stick(input->GetXInputThumbLY()); break;
      case Chaso::InputBinding::RX: v = stick(input->GetXInputThumbRX()); break;
      case Chaso::InputBinding::RY: v = stick(input->GetXInputThumbRY()); break;
      case Chaso::InputBinding::LT: v = static_cast<float>(input->GetXInputLeftTrigger()) / 255.0f; break;
      case Chaso::InputBinding::RT: v = static_cast<float>(input->GetXInputRightTrigger()) / 255.0f; break;
      default: break;
      }
      v = Chaso::InputActions::ApplyDeadZone(v, actions.DeadZone());
      if (b.half > 0) v = (std::max)(v, 0.0f);
      if (b.half < 0) v = (std::max)(-v, 0.0f);
      break;
    }
    }
    sum += v * b.scale;
  }
  return std::abs(std::clamp(sum, -1.0f, 1.0f)) >= 0.5f;
}

/// @brief マウス（ボタン・ホイール）の割り当てを除いて押しているか
/// @details 射撃のように、マウスはエディタ操作中・ポーズ明けのクリック残りでは無視したいが、
///          キーやパッドの割り当ては効かせたいアクション用。
inline bool PressedWithoutMouse(const char *action) {
  return PressedIf(action, [](const Chaso::InputBinding &b) { return !IsMouse(b); });
}

/// @brief マウス（ボタン・ホイール）の割り当てで押しているか
inline bool PressedByMouse(const char *action) {
  return PressedIf(action, [](const Chaso::InputBinding &b) { return IsMouse(b); });
}

/// @brief パッドの割り当てで押しているか
inline bool PressedOnPad(const char *action) {
  return PressedIf(action, [](const Chaso::InputBinding &b) { return !IsKeyboardMouse(b); });
}

/// @brief 割り当てを画面に出す短い名前にする（"Key:SPACE" → "SPACE"、"Mouse:Left" → "左クリック"）
inline std::string DisplayName(const Chaso::InputBinding &b) {
  using Device = Chaso::InputBinding::Device;
  if (b.device == Device::MouseWheel) return (b.code > 0) ? "ホイール奥" : "ホイール手前";
  if (b.device == Device::Mouse) {
    switch (b.code) {
    case 0: return "左クリック";
    case 1: return "右クリック";
    case 2: return "中クリック";
    default: break;
    }
  }
  std::string s = b.ToString();
  if (const size_t colon = s.find(':'); colon != std::string::npos) {
    const bool negative = !s.empty() && s[0] == '-';
    s = (negative ? "-" : "") + s.substr(colon + 1);
  }
  return s;
}

/// @brief アクションの割り当てのうち、キーボード・マウス側（またはパッド側）だけを並べた文字列
/// @param action        アクション名
/// @param keyboardMouse true ならキーボード・マウス側、false ならパッド側
/// @return "SPACE / ENTER" のような文字列（無ければ "-"）
inline std::string BindingsText(const char *action, bool keyboardMouse) {
  std::string out;
  for (const auto &b : Actions().GetBindings(action)) {
    if (IsKeyboardMouse(b) != keyboardMouse) continue;
    if (!out.empty()) out += " / ";
    out += DisplayName(b);
  }
  return out.empty() ? "-" : out;
}

/// @brief アクションのキーボード・マウス側（またはパッド側）の割り当てを 1 つに置き換える
/// @details 反対側（パッド ⇔ キーボード・マウス）の割り当てはそのまま残す。
/// @param action  アクション名
/// @param binding 新しい割り当て
inline void Rebind(const char *action, const Chaso::InputBinding &binding) {
  auto &actions = Actions();
  std::vector<Chaso::InputBinding> next;
  const bool kbm = IsKeyboardMouse(binding);
  for (const auto &b : actions.GetBindings(action)) {
    if (IsKeyboardMouse(b) != kbm) next.push_back(b);
  }
  next.push_back(binding);
  actions.SetBindings(action, next);
}

/// @brief 今の割り当てを Resources/Setting/InputActions.json へ保存する
/// @return 書けたら true
inline bool Save() { return Actions().Save(Chaso::PrepareSettingPath(Chaso::kInputActionsPath)); }

} // namespace GameInput
