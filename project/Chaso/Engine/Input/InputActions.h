#pragma once

// ============================================================================
// InputActions
// ----------------------------------------------------------------------------
// 「Jump」「Fire」「MoveX」のような論理名（アクション）に、キーボード・マウス・パッドの
// 入力を複数まとめて割り当てる仕組み。スクリプトはキーコードを書かずにアクション名で読む。
//
// 使い方（スクリプトから）:
//   auto& in = Chaso::InputActions::Get();
//   if (in.IsTriggered("Jump")) { ... }                  // 押した瞬間
//   if (in.IsPressed("Fire"))   { ... }                  // 押している間
//   RC::Vector2 move = in.GetVector2("MoveX", "MoveY");  // スティックと WASD を同じように（長さは最大 1）
//
// 割り当て（ゲーム側の初期化で。GameSetup.cpp など）:
//   in.Define("Jump",  {"Key:SPACE", "Pad:A"});
//   in.Define("MoveX", {"Key:D", "-Key:A", "Key:RIGHT", "-Key:LEFT", "Pad:LX"});
//   in.Define("MoveY", {"Key:W", "-Key:S", "Pad:LY"});
//   in.Define("Fire",  {"Mouse:Left", "Pad:RT"});
//   Define は「既定の割り当て」。Resources/Setting/InputActions.json（キーコンフィグの保存先）に
//   同じ名前があればそちらが優先される（Define と Load の順番はどちらでもよい）。
//
// 割り当ての書き方: [-]デバイス:名前[*倍率]
//   Key:A / Key:SPACE / Key:LSHIFT / Key:F1 ...（KeyNames() の表）
//   Mouse:Left / Mouse:Right / Mouse:Middle
//   Mouse:WheelUp / Mouse:WheelDown … ホイールを回したフレームだけ押している扱い（回すたびに IsTriggered）
//   Pad:A / Pad:B / Pad:X / Pad:Y / Pad:LB / Pad:RB / Pad:START / Pad:BACK / Pad:UP ...
//   Pad:LX / Pad:LY / Pad:RX / Pad:RY（-1〜1）/ Pad:LT / Pad:RT（0〜1）
//   Pad:LY+ / Pad:LY-   … スティックの片側だけ（メニューの上下など。値は 0〜1）
//   先頭の "-" で向きを逆に、"*0.5" で倍率を掛ける
//
// 値の決まり方: 全割り当ての和を -1〜1 に丸めたもの。|値| >= 0.5 で「押している」扱い。
// スティックはデッドゾーン（既定 0.2）の内側を 0 にし、外側を 0〜1 に引き伸ばす。
// ============================================================================

#include <nlohmann/json.hpp>
#include "Math/MathTypes.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Chaso {

/// @struct InputSnapshot
/// @brief 1 フレーム分の生の入力（Input から詰める。OS に依存しない形にしてテストしやすくしている）
struct InputSnapshot {
  std::array<bool, 256> keys{};   ///< DirectInput のキー（DIK_*）ごとの押下
  std::array<bool, 8> mouse{};    ///< マウスボタン（0 = 左, 1 = 右, 2 = 中, 3〜 = 拡張）
  int mouseWheel = 0;             ///< このフレームのホイールの回転量（奥へ回すと正。0 なら回していない）
  uint16_t padButtons = 0;        ///< XInput のボタン（XINPUT_GAMEPAD_* のビット）
  std::array<float, 6> padAxes{}; ///< LX, LY, RX, RY（-1〜1）, LT, RT（0〜1）。デッドゾーン適用前
};

/// @struct InputBinding
/// @brief アクションへの割り当て 1 つ
struct InputBinding {
  enum class Device : uint8_t { Key, Mouse, PadButton, PadAxis, MouseWheel };
  enum Axis : int { LX = 0, LY, RX, RY, LT, RT };

  Device device = Device::Key; ///< 入力の種類
  int code = 0;                ///< キー: DIK、マウス: ボタン番号、パッド: ボタンのビット / 軸番号、ホイール: +1（奥）/ -1（手前）
  int half = 0;                ///< 軸の片側だけ使うとき +1 / -1（0 なら両側）
  float scale = 1.0f;          ///< 倍率（負なら向きを逆にする）

  bool operator==(const InputBinding &o) const {
    return device == o.device && code == o.code && half == o.half && scale == o.scale;
  }

  /// @brief キー名と DIK コードの表（DirectInput の値。dinput.h と同じ）
  static const std::vector<std::pair<const char *, int>> &KeyNames() {
    static const std::vector<std::pair<const char *, int>> kTable = {
        {"ESCAPE", 0x01}, {"1", 0x02}, {"2", 0x03}, {"3", 0x04}, {"4", 0x05}, {"5", 0x06},
        {"6", 0x07}, {"7", 0x08}, {"8", 0x09}, {"9", 0x0A}, {"0", 0x0B}, {"MINUS", 0x0C},
        {"EQUALS", 0x0D}, {"BACKSPACE", 0x0E}, {"TAB", 0x0F},
        {"Q", 0x10}, {"W", 0x11}, {"E", 0x12}, {"R", 0x13}, {"T", 0x14}, {"Y", 0x15},
        {"U", 0x16}, {"I", 0x17}, {"O", 0x18}, {"P", 0x19}, {"LBRACKET", 0x1A}, {"RBRACKET", 0x1B},
        {"ENTER", 0x1C}, {"LCONTROL", 0x1D},
        {"A", 0x1E}, {"S", 0x1F}, {"D", 0x20}, {"F", 0x21}, {"G", 0x22}, {"H", 0x23},
        {"J", 0x24}, {"K", 0x25}, {"L", 0x26}, {"SEMICOLON", 0x27}, {"APOSTROPHE", 0x28},
        {"GRAVE", 0x29}, {"LSHIFT", 0x2A}, {"BACKSLASH", 0x2B},
        {"Z", 0x2C}, {"X", 0x2D}, {"C", 0x2E}, {"V", 0x2F}, {"B", 0x30}, {"N", 0x31}, {"M", 0x32},
        {"COMMA", 0x33}, {"PERIOD", 0x34}, {"SLASH", 0x35}, {"RSHIFT", 0x36},
        {"NUMPADSTAR", 0x37}, {"LALT", 0x38}, {"SPACE", 0x39}, {"CAPSLOCK", 0x3A},
        {"F1", 0x3B}, {"F2", 0x3C}, {"F3", 0x3D}, {"F4", 0x3E}, {"F5", 0x3F}, {"F6", 0x40},
        {"F7", 0x41}, {"F8", 0x42}, {"F9", 0x43}, {"F10", 0x44}, {"NUMLOCK", 0x45}, {"SCROLL", 0x46},
        {"NUMPAD7", 0x47}, {"NUMPAD8", 0x48}, {"NUMPAD9", 0x49}, {"NUMPADMINUS", 0x4A},
        {"NUMPAD4", 0x4B}, {"NUMPAD5", 0x4C}, {"NUMPAD6", 0x4D}, {"NUMPADPLUS", 0x4E},
        {"NUMPAD1", 0x4F}, {"NUMPAD2", 0x50}, {"NUMPAD3", 0x51}, {"NUMPAD0", 0x52},
        {"NUMPADPERIOD", 0x53}, {"F11", 0x57}, {"F12", 0x58}, {"NUMPADENTER", 0x9C},
        {"RCONTROL", 0x9D}, {"NUMPADSLASH", 0xB5}, {"RALT", 0xB8},
        {"HOME", 0xC7}, {"UP", 0xC8}, {"PAGEUP", 0xC9}, {"LEFT", 0xCB}, {"RIGHT", 0xCD},
        {"END", 0xCF}, {"DOWN", 0xD0}, {"PAGEDOWN", 0xD1}, {"INSERT", 0xD2}, {"DELETE", 0xD3},
    };
    return kTable;
  }

  /// @brief パッドのボタン名とビットの表（XInput の値。Xinput.h と同じ）
  static const std::vector<std::pair<const char *, int>> &PadButtonNames() {
    static const std::vector<std::pair<const char *, int>> kTable = {
        {"UP", 0x0001}, {"DOWN", 0x0002}, {"LEFT", 0x0004}, {"RIGHT", 0x0008},
        {"START", 0x0010}, {"BACK", 0x0020}, {"LSTICK", 0x0040}, {"RSTICK", 0x0080},
        {"LB", 0x0100}, {"RB", 0x0200}, {"A", 0x1000}, {"B", 0x2000}, {"X", 0x4000}, {"Y", 0x8000},
    };
    return kTable;
  }

  /// @brief パッドの軸名
  static const char *AxisName(int axis) {
    static const char *kNames[] = {"LX", "LY", "RX", "RY", "LT", "RT"};
    return (axis >= 0 && axis < 6) ? kNames[axis] : "LX";
  }

  /// @brief マウスボタン名
  static const char *MouseName(int button) {
    switch (button) {
    case 0: return "Left";
    case 1: return "Right";
    case 2: return "Middle";
    default: return nullptr;
    }
  }

  /// @brief "Key:SPACE" などの文字列から作る
  /// @param text 割り当ての文字列
  /// @param out  結果
  /// @return 読めたら true
  static bool Parse(const std::string &text, InputBinding &out) {
    std::string s;
    for (char c : text) if (!std::isspace(static_cast<unsigned char>(c))) s.push_back(c);
    if (s.empty()) return false;

    InputBinding b;
    if (s[0] == '-') { b.scale = -1.0f; s.erase(0, 1); }
    if (const size_t star = s.find('*'); star != std::string::npos) {
      char *end = nullptr;
      const float k = std::strtof(s.c_str() + star + 1, &end);
      if (end == s.c_str() + star + 1) return false;
      b.scale *= k;
      s.resize(star);
    }
    const size_t colon = s.find(':');
    if (colon == std::string::npos) return false;
    const std::string dev = Upper_(s.substr(0, colon));
    std::string name = Upper_(s.substr(colon + 1));

    if (dev == "KEY") {
      b.device = Device::Key;
      if (name == "RETURN") name = "ENTER";
      if (name == "LCTRL") name = "LCONTROL";
      if (name == "RCTRL") name = "RCONTROL";
      if (name == "ESC") name = "ESCAPE";
      for (const auto &[n, code] : KeyNames()) {
        if (name == n) { b.code = code; out = b; return true; }
      }
      return false;
    }
    if (dev == "MOUSE") {
      if (name == "WHEELUP" || name == "WHEELDOWN") {
        b.device = Device::MouseWheel;
        b.code = (name == "WHEELUP") ? 1 : -1;
        out = b;
        return true;
      }
      b.device = Device::Mouse;
      for (int i = 0; i < 3; ++i) {
        if (name == Upper_(MouseName(i))) { b.code = i; out = b; return true; }
      }
      return false;
    }
    if (dev == "PAD") {
      // 軸（片側指定 + / - も）
      int half = 0;
      std::string axisName = name;
      if (!axisName.empty() && (axisName.back() == '+' || axisName.back() == '-')) {
        half = (axisName.back() == '+') ? 1 : -1;
        axisName.pop_back();
      }
      for (int a = 0; a < 6; ++a) {
        if (axisName == AxisName(a)) {
          b.device = Device::PadAxis;
          b.code = a;
          b.half = half;
          out = b;
          return true;
        }
      }
      b.device = Device::PadButton;
      if (name == "DPADUP") name = "UP";
      if (name == "DPADDOWN") name = "DOWN";
      if (name == "DPADLEFT") name = "LEFT";
      if (name == "DPADRIGHT") name = "RIGHT";
      for (const auto &[n, bit] : PadButtonNames()) {
        if (name == n) { b.code = bit; out = b; return true; }
      }
      return false;
    }
    return false;
  }

  /// @brief 文字列にする（Parse と往復できる形）
  std::string ToString() const {
    std::string name;
    switch (device) {
    case Device::Key:
      name = "Key:?";
      for (const auto &[n, c] : KeyNames()) if (c == code) { name = std::string("Key:") + n; break; }
      break;
    case Device::Mouse:
      name = std::string("Mouse:") + (MouseName(code) ? MouseName(code) : "Left");
      break;
    case Device::MouseWheel:
      name = (code > 0) ? "Mouse:WheelUp" : "Mouse:WheelDown";
      break;
    case Device::PadButton:
      name = "Pad:?";
      for (const auto &[n, c] : PadButtonNames()) if (c == code) { name = std::string("Pad:") + n; break; }
      break;
    case Device::PadAxis:
      name = std::string("Pad:") + AxisName(code) + (half > 0 ? "+" : half < 0 ? "-" : "");
      break;
    }
    std::string out = (scale < 0.0f) ? "-" + name : name;
    const float mag = std::abs(scale);
    if (mag != 1.0f) {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "*%g", mag);
      out += buf;
    }
    return out;
  }

private:
  static std::string Upper_(std::string s) {
    for (char &c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
  }
};

/// @class InputActions
/// @brief アクション名で入力を読む窓口（シングルトン）
class InputActions {
public:
  /// @brief インスタンスを取得する
  static InputActions &Get() {
    static InputActions instance;
    return instance;
  }

  // ------------------------------------------------------------
  // 毎フレームの更新（App が呼ぶ）
  // ------------------------------------------------------------

  /// @brief 入力を取り込んで全アクションの値を計算する
  /// @param snap このフレームの生の入力
  void Update(const InputSnapshot &snap) {
    prevSnap_ = snap_;
    snap_ = snap;
    for (auto &[name, st] : states_) {
      st.prevPressed = st.pressed;
      st.value = Evaluate_(name);
      st.pressed = std::abs(st.value) >= pressThreshold_;
      st.wheelPulse = st.pressed && WheelMoved_(name);
    }
  }

  // ------------------------------------------------------------
  // 読む
  // ------------------------------------------------------------

  /// @brief 押している間 true
  bool IsPressed(const std::string &action) const { return State_(action).pressed; }
  /// @brief 押した瞬間だけ true（ホイールの割り当ては、回したフレームごとに true）
  bool IsTriggered(const std::string &action) const {
    const auto &s = State_(action);
    return (s.pressed && !s.prevPressed) || s.wheelPulse;
  }
  /// @brief 離した瞬間だけ true
  bool IsReleased(const std::string &action) const {
    const auto &s = State_(action);
    return !s.pressed && s.prevPressed;
  }
  /// @brief 値（-1〜1。ボタンなら 0 か 1、スティックなら傾き）
  float GetValue(const std::string &action) const { return State_(action).value; }

  /// @brief 2 つのアクションを (x, y) にまとめる（移動入力用。長さは最大 1 に丸める）
  RC::Vector2 GetVector2(const std::string &xAction, const std::string &yAction) const {
    float x = GetValue(xAction);
    float y = GetValue(yAction);
    const float len = std::sqrt(x * x + y * y);
    if (len > 1.0f) { x /= len; y /= len; }
    return {x, y};
  }

  /// @brief アクションが登録されているか
  bool HasAction(const std::string &action) const { return current_.count(action) > 0; }

  // ------------------------------------------------------------
  // 割り当て
  // ------------------------------------------------------------

  /// @brief 既定の割り当てを登録する（ファイルで上書きされていなければ今の割り当てにもなる）
  /// @param action   アクション名
  /// @param bindings "Key:SPACE" などの文字列（読めない物はログを出さずに飛ばし、戻り値で知らせる）
  /// @return すべて読めたら true
  bool Define(const std::string &action, std::initializer_list<const char *> bindings) {
    std::vector<std::string> v;
    for (const char *b : bindings) v.emplace_back(b ? b : "");
    return Define(action, v);
  }

  /// @brief 既定の割り当てを登録する（std::vector 版）
  bool Define(const std::string &action, const std::vector<std::string> &bindings) {
    std::vector<InputBinding> parsed;
    const bool ok = ParseList_(bindings, parsed);
    defaults_[action] = parsed;
    if (!fromFile_.count(action)) current_[action] = std::move(parsed);
    states_[action]; // 状態の入れ物を作っておく
    return ok;
  }

  /// @brief 今の割り当てを置き換える（キーコンフィグ画面から）
  void SetBindings(const std::string &action, const std::vector<InputBinding> &bindings) {
    current_[action] = bindings;
    fromFile_.insert(action); // 保存対象にする
    states_[action];
  }

  /// @brief 今の割り当てを取得する（無ければ空）
  const std::vector<InputBinding> &GetBindings(const std::string &action) const {
    static const std::vector<InputBinding> kEmpty;
    auto it = current_.find(action);
    return (it != current_.end()) ? it->second : kEmpty;
  }

  /// @brief 1 つのアクションを既定の割り当てに戻す
  void ResetToDefault(const std::string &action) {
    fromFile_.erase(action);
    if (auto it = defaults_.find(action); it != defaults_.end()) current_[action] = it->second;
    else current_.erase(action);
  }

  /// @brief 全アクションを既定の割り当てに戻す
  void ResetAllToDefault() {
    fromFile_.clear();
    current_ = defaults_;
  }

  /// @brief 登録されているアクション名の一覧（名前順）
  std::vector<std::string> ActionNames() const {
    std::vector<std::string> names;
    for (const auto &[n, b] : current_) names.push_back(n);
    std::sort(names.begin(), names.end());
    return names;
  }

  /// @brief このフレームに新しく押された入力を 1 つ拾う（キーコンフィグの「キーを押してください」用）
  /// @param out          拾った割り当て
  /// @param includeAxes  スティック・トリガーの倒し込みも拾うか
  /// @return 拾えたら true
  bool CaptureNextBinding(InputBinding &out, bool includeAxes = true) const {
    for (int k = 0; k < 256; ++k) {
      if (snap_.keys[k] && !prevSnap_.keys[k]) {
        for (const auto &[n, c] : InputBinding::KeyNames()) {
          if (c == k) {
            out = {InputBinding::Device::Key, k, 0, 1.0f};
            return true;
          }
        }
      }
    }
    for (int m = 0; m < 3; ++m) {
      if (snap_.mouse[m] && !prevSnap_.mouse[m]) {
        out = {InputBinding::Device::Mouse, m, 0, 1.0f};
        return true;
      }
    }
    if (snap_.mouseWheel != 0) {
      out = {InputBinding::Device::MouseWheel, snap_.mouseWheel > 0 ? 1 : -1, 0, 1.0f};
      return true;
    }
    for (const auto &[n, bit] : InputBinding::PadButtonNames()) {
      if ((snap_.padButtons & bit) && !(prevSnap_.padButtons & bit)) {
        out = {InputBinding::Device::PadButton, bit, 0, 1.0f};
        return true;
      }
    }
    if (includeAxes) {
      constexpr float kCapture = 0.6f;
      for (int a = 0; a < 6; ++a) {
        const float v = snap_.padAxes[a];
        const float pv = prevSnap_.padAxes[a];
        if (std::abs(v) >= kCapture && std::abs(pv) < kCapture) {
          out = {InputBinding::Device::PadAxis, a, (a >= InputBinding::LT) ? 0 : (v > 0 ? 1 : -1), 1.0f};
          return true;
        }
      }
    }
    return false;
  }

  // ------------------------------------------------------------
  // 設定
  // ------------------------------------------------------------

  /// @brief スティックのデッドゾーン（0〜0.95）
  void SetDeadZone(float dz) { deadZone_ = std::clamp(dz, 0.0f, 0.95f); }
  float DeadZone() const { return deadZone_; }

  /// @brief 「押している」とみなす値（既定 0.5）
  void SetPressThreshold(float t) { pressThreshold_ = std::clamp(t, 0.01f, 1.0f); }

  // ------------------------------------------------------------
  // 保存・読み込み（Resources/Setting/InputActions.json）
  // ------------------------------------------------------------

  /// @brief ファイルから割り当てを読む（同じ名前の Define より優先される）
  /// @param path 読み込むファイル
  /// @return 読めたら true（ファイルが無いときは false で、既定の割り当てのまま）
  bool Load(const std::string &path) {
    std::ifstream ifs(path);
    if (!ifs) return false;
    const nlohmann::json j = nlohmann::json::parse(ifs, nullptr, /*allow_exceptions=*/false);
    return LoadJson(j);
  }

  /// @brief JSON から割り当てを読む
  bool LoadJson(const nlohmann::json &j) {
    if (!j.is_object()) return false;
    if (auto it = j.find("deadZone"); it != j.end() && it->is_number()) SetDeadZone(it->get<float>());
    if (auto it = j.find("pressThreshold"); it != j.end() && it->is_number()) {
      SetPressThreshold(it->get<float>());
    }
    auto acts = j.find("actions");
    if (acts == j.end() || !acts->is_object()) return true;
    for (auto it = acts->begin(); it != acts->end(); ++it) {
      if (!it.value().is_array()) continue;
      std::vector<std::string> strs;
      for (const auto &b : it.value()) if (b.is_string()) strs.push_back(b.get<std::string>());
      std::vector<InputBinding> parsed;
      ParseList_(strs, parsed);
      current_[it.key()] = std::move(parsed);
      fromFile_.insert(it.key());
      states_[it.key()];
    }
    return true;
  }

  /// @brief 今の割り当てを JSON にする
  nlohmann::json ToJson() const {
    nlohmann::json acts = nlohmann::json::object();
    for (const auto &name : ActionNames()) {
      nlohmann::json arr = nlohmann::json::array();
      for (const auto &b : GetBindings(name)) arr.push_back(b.ToString());
      acts[name] = arr;
    }
    return {{"deadZone", deadZone_}, {"pressThreshold", pressThreshold_}, {"actions", acts}};
  }

  /// @brief 今の割り当てをファイルへ保存する（キーコンフィグの確定時に）
  bool Save(const std::string &path) const {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    std::ofstream ofs(path);
    if (!ofs) return false;
    ofs << ToJson().dump(2);
    return true;
  }

  /// @brief すべて忘れる（テスト用）
  void Clear() {
    current_.clear();
    defaults_.clear();
    fromFile_.clear();
    states_.clear();
    snap_ = {};
    prevSnap_ = {};
  }

  /// @brief 軸の値にデッドゾーンを掛ける（内側は 0、外側は 0〜1 に引き伸ばす）
  static float ApplyDeadZone(float v, float dz) {
    const float a = std::abs(v);
    if (a <= dz) return 0.0f;
    const float k = (std::min)((a - dz) / (1.0f - dz), 1.0f);
    return (v < 0.0f) ? -k : k;
  }

private:
  InputActions() = default;

  struct State {
    float value = 0.0f;
    bool pressed = false;
    bool prevPressed = false;
    bool wheelPulse = false; ///< このフレームにホイールの割り当てで押された（連続で回しても毎フレーム IsTriggered にする）
  };

  const State &State_(const std::string &action) const {
    static const State kNone;
    auto it = states_.find(action);
    return (it != states_.end()) ? it->second : kNone;
  }

  static bool ParseList_(const std::vector<std::string> &strs, std::vector<InputBinding> &out) {
    bool ok = true;
    out.clear();
    for (const auto &s : strs) {
      InputBinding b;
      if (InputBinding::Parse(s, b)) out.push_back(b);
      else ok = false;
    }
    return ok;
  }

  float Contribution_(const InputBinding &b) const {
    float v = 0.0f;
    switch (b.device) {
    case InputBinding::Device::Key:
      v = (b.code >= 0 && b.code < 256 && snap_.keys[b.code]) ? 1.0f : 0.0f;
      break;
    case InputBinding::Device::Mouse:
      v = (b.code >= 0 && b.code < 8 && snap_.mouse[b.code]) ? 1.0f : 0.0f;
      break;
    case InputBinding::Device::PadButton:
      v = (snap_.padButtons & b.code) ? 1.0f : 0.0f;
      break;
    case InputBinding::Device::MouseWheel:
      v = ((b.code > 0 && snap_.mouseWheel > 0) || (b.code < 0 && snap_.mouseWheel < 0)) ? 1.0f : 0.0f;
      break;
    case InputBinding::Device::PadAxis: {
      if (b.code < 0 || b.code >= 6) break;
      v = ApplyDeadZone(snap_.padAxes[b.code], deadZone_);
      if (b.half > 0) v = (std::max)(v, 0.0f);
      if (b.half < 0) v = (std::max)(-v, 0.0f);
      break;
    }
    }
    return v * b.scale;
  }

  /// @brief アクションのホイールの割り当てが、このフレームに回した向きと合っているか
  bool WheelMoved_(const std::string &action) const {
    if (snap_.mouseWheel == 0) return false;
    auto it = current_.find(action);
    if (it == current_.end()) return false;
    for (const auto &b : it->second) {
      if (b.device != InputBinding::Device::MouseWheel) continue;
      if ((b.code > 0) == (snap_.mouseWheel > 0)) return true;
    }
    return false;
  }

  float Evaluate_(const std::string &action) const {
    auto it = current_.find(action);
    if (it == current_.end()) return 0.0f;
    float sum = 0.0f;
    for (const auto &b : it->second) sum += Contribution_(b);
    return std::clamp(sum, -1.0f, 1.0f);
  }

  std::unordered_map<std::string, std::vector<InputBinding>> current_;  ///< 今の割り当て
  std::unordered_map<std::string, std::vector<InputBinding>> defaults_; ///< Define した既定値
  std::set<std::string> fromFile_;                ///< ファイル・キーコンフィグで上書きされた名前
  std::unordered_map<std::string, State> states_; ///< アクションごとの状態
  InputSnapshot snap_;                            ///< 今フレームの入力
  InputSnapshot prevSnap_;                        ///< 前フレームの入力
  float deadZone_ = 0.2f;                         ///< スティックのデッドゾーン
  float pressThreshold_ = 0.5f;                   ///< 押しているとみなす値
};

} // namespace Chaso
