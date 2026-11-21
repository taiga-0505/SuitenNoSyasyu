#pragma once

// ============================================================================
// SaveData
// ----------------------------------------------------------------------------
// ゲームの進行状況・ハイスコア・解放済みステージなどを保存する仕組み。
// スロット（"slot1" / "autosave" など）ごとに 1 つの JSON ファイルになる。
//
// 使い方（スクリプトから）:
//   auto& save = Chaso::SaveData::Get();
//   save.Set("stage.cleared", 3);                // "." で階層を作れる
//   save.Set("best.score", std::max(score, save.Get("best.score", 0)));
//   save.Save();                                 // 今のスロットへ書き込む（既定は "default"）
//
//   save.Load("slot2");                          // スロットを切り替えて読み込む（無ければ空から）
//   int stage = save.Get("stage.cleared", 0);    // 無ければ第 2 引数の値
//   if (save.Has("tutorialDone")) { ... }
//
//   for (auto& s : save.ListSlots()) { s.name; s.savedAt; }  // セーブ画面の一覧
//
// 安全のための仕組み:
//   - 一時ファイルに書いてから置き換える（書き込み中に落ちても元のファイルは壊れない）
//   - 置き換える前のファイルを .bak として残し、本体が壊れていたら .bak から読む
//   - 中身のチェックサムを一緒に保存し、途中で切れたファイルを見分ける
//   - バージョン番号を保存し、SetVersion / SetMigration で古いセーブを新しい形に直せる
//
// 保存先は既定で実行ファイルの作業フォルダの Saves/（SetDirectory で変更可）。
// 設定（音量・感度）は Resources/Setting/GameSettings.json、キーコンフィグは InputActions.json のまま。
// ============================================================================

#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace Chaso {

/// @class SaveData
/// @brief スロット単位のセーブデータ（シングルトン）
class SaveData {
public:
  /// @struct SlotInfo
  /// @brief セーブ画面に並べるためのスロット情報
  struct SlotInfo {
    std::string name;    ///< スロット名
    std::string savedAt; ///< 保存日時（"2026-11-20 21:30:05"）
    int version = 0;     ///< 保存したときのデータのバージョン
    bool valid = false;  ///< 読める状態か（壊れていて .bak も無ければ false）
  };

  /// @brief インスタンスを取得する
  static SaveData &Get() {
    static SaveData instance;
    return instance;
  }

  // ------------------------------------------------------------
  // 設定
  // ------------------------------------------------------------

  /// @brief 保存先フォルダ（既定 "Saves"）
  void SetDirectory(const std::string &dir) { directory_ = dir; }
  const std::string &Directory() const { return directory_; }

  /// @brief 今のデータの形のバージョン（形を変えたら上げる）
  void SetVersion(int version) { version_ = version; }
  int Version() const { return version_; }

  /// @brief 古いバージョンのセーブを読んだときに呼ぶ変換処理
  /// @param fn void(int fromVersion, nlohmann::json& data)。data をその場で今の形に直す
  void SetMigration(std::function<void(int, nlohmann::json &)> fn) { migration_ = std::move(fn); }

  // ------------------------------------------------------------
  // 値の読み書き（今のスロットの中身）
  // ------------------------------------------------------------

  /// @brief 値を書く（"player.hp" のように "." で階層を作れる）
  /// @return 書けたら true（"a" が数値なのに "a.b" に書こうとした等、形が合わないときは false）
  template <class T> bool Set(const std::string &key, const T &value) {
    try {
      data_[Pointer_(key)] = value;
    } catch (...) {
      return false;
    }
    dirty_ = true;
    return true;
  }

  /// @brief 値を読む（無い・型が違うときは fallback）
  template <class T> T Get(const std::string &key, const T &fallback) const {
    try {
      const auto ptr = Pointer_(key);
      if (!data_.contains(ptr)) return fallback;
      return data_.at(ptr).get<T>();
    } catch (...) {
      return fallback;
    }
  }

  /// @brief 文字列リテラルを fallback に渡したとき用（Get("name", "名無し")）
  std::string Get(const std::string &key, const char *fallback) const {
    return Get<std::string>(key, std::string(fallback ? fallback : ""));
  }

  /// @brief 値があるか
  bool Has(const std::string &key) const {
    try {
      return data_.contains(Pointer_(key));
    } catch (...) {
      return false;
    }
  }

  /// @brief 値を消す
  void Remove(const std::string &key) {
    try {
      const auto ptr = Pointer_(key);
      if (!data_.contains(ptr)) return;
      auto &obj = data_.at(ptr.parent_pointer());
      if (obj.is_object()) obj.erase(ptr.back());
      dirty_ = true;
    } catch (...) {
    }
  }

  /// @brief 今のスロットの中身をすべて消す（ファイルは Save するまで変わらない）
  void Clear() {
    data_ = nlohmann::json::object();
    dirty_ = true;
  }

  /// @brief 中身を JSON として直接触る（まとめて書きたいとき）
  nlohmann::json &Data() {
    dirty_ = true;
    return data_;
  }
  const nlohmann::json &Data() const { return data_; }

  /// @brief Save していない変更があるか
  bool IsDirty() const { return dirty_; }

  /// @brief 今のスロット名
  const std::string &CurrentSlot() const { return slot_; }

  // ------------------------------------------------------------
  // ファイル
  // ------------------------------------------------------------

  /// @brief 今のスロットへ保存する
  /// @return 書けたら true
  bool Save() { return SaveAs(slot_); }

  /// @brief 別のスロットへ保存し、以後そのスロットを使う
  bool SaveAs(const std::string &slot) {
    if (!IsValidSlotName(slot)) return false;
    nlohmann::json envelope = {
        {"version", version_},
        {"savedAt", NowString_()},
        {"checksum", Checksum_(data_)},
        {"data", data_},
    };
    if (!WriteAtomic_(PathOf_(slot), envelope.dump(2, ' ', false, nlohmann::json::error_handler_t::replace))) {
      return false;
    }
    slot_ = slot;
    dirty_ = false;
    return true;
  }

  /// @brief スロットを読み込み、以後そのスロットを使う
  /// @param slot スロット名
  /// @return ファイルがあって読めたら true。無ければ false で中身は空（新規ゲーム）
  bool Load(const std::string &slot = "default") {
    if (!IsValidSlotName(slot)) return false;
    slot_ = slot;
    dirty_ = false;
    nlohmann::json data;
    int version = 0;
    if (ReadSlot_(slot, data, version, nullptr)) {
      if (version != version_ && migration_) migration_(version, data);
      data_ = data.is_object() ? std::move(data) : nlohmann::json::object();
      if (version != version_) dirty_ = true; // 直した形で次に保存されるように
      return true;
    }
    data_ = nlohmann::json::object();
    return false;
  }

  /// @brief スロットのファイルがあるか
  bool Exists(const std::string &slot) const {
    std::error_code ec;
    return IsValidSlotName(slot) && (std::filesystem::exists(PathOf_(slot), ec) ||
                                     std::filesystem::exists(PathOf_(slot) + ".bak", ec));
  }

  /// @brief スロットを消す（.bak も消す）
  bool Delete(const std::string &slot) {
    if (!IsValidSlotName(slot)) return false;
    std::error_code ec;
    const bool removed = std::filesystem::remove(PathOf_(slot), ec);
    std::filesystem::remove(PathOf_(slot) + ".bak", ec);
    if (slot == slot_) Clear();
    return removed;
  }

  /// @brief 保存されているスロットの一覧（名前順）
  std::vector<SlotInfo> ListSlots() const {
    std::vector<SlotInfo> out;
    std::error_code ec;
    if (!std::filesystem::is_directory(directory_, ec)) return out;
    for (const auto &entry : std::filesystem::directory_iterator(directory_, ec)) {
      const auto &p = entry.path();
      if (p.extension() != kExtension) continue;
      SlotInfo info;
      info.name = p.stem().string();
      nlohmann::json data;
      info.valid = ReadSlot_(info.name, data, info.version, &info.savedAt);
      out.push_back(std::move(info));
    }
    std::sort(out.begin(), out.end(), [](const SlotInfo &a, const SlotInfo &b) { return a.name < b.name; });
    return out;
  }

  /// @brief スロット名として使えるか（英数字・'_'・'-' のみ、1〜64 文字）
  static bool IsValidSlotName(const std::string &slot) {
    if (slot.empty() || slot.size() > 64) return false;
    for (char c : slot) {
      const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                      c == '_' || c == '-';
      if (!ok) return false;
    }
    return true;
  }

private:
  SaveData() = default;

  static constexpr const char *kExtension = ".json"; ///< スロット名.json

  std::string PathOf_(const std::string &slot) const {
    return (std::filesystem::path(directory_) / (slot + kExtension)).string();
  }

  /// @brief "a.b.c" → "/a/b/c"（"/" で始まるならそのまま JSON Pointer として使う）
  static nlohmann::json::json_pointer Pointer_(const std::string &key) {
    if (!key.empty() && key[0] == '/') return nlohmann::json::json_pointer(key);
    std::string p = "/";
    for (char c : key) {
      if (c == '.') p += '/';
      else if (c == '~') p += "~0"; // JSON Pointer の予約文字を逃がす
      else if (c == '/') p += "~1";
      else p += c;
    }
    return nlohmann::json::json_pointer(p);
  }

  /// @brief 中身のチェックサム（FNV-1a 64bit。改ざん防止ではなく、壊れたファイルを見分けるため）
  static std::string Checksum_(const nlohmann::json &data) {
    const std::string s = data.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) {
      h ^= c;
      h *= 1099511628211ull;
    }
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
    return buf;
  }

  static std::string NowString_() {
    const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
  }

  /// @brief 一時ファイルに書いてから置き換える。置き換え前のファイルは .bak に残す
  bool WriteAtomic_(const std::string &path, const std::string &text) const {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    const std::string tmp = path + ".tmp";
    {
      std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
      if (!ofs) return false;
      ofs << text;
      ofs.flush();
      if (!ofs) return false;
    }
    if (fs::exists(path, ec)) {
      fs::copy_file(path, path + ".bak", fs::copy_options::overwrite_existing, ec);
    }
    fs::rename(tmp, path, ec); // 同じフォルダ内なので置き換えになる
    if (ec) {
      fs::remove(tmp, ec);
      return false;
    }
    return true;
  }

  /// @brief 1 ファイルを読んでチェックサムを確かめる
  static bool ReadFile_(const std::string &path, nlohmann::json &data, int &version, std::string *savedAt) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return false;
    const nlohmann::json j = nlohmann::json::parse(ifs, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object() || !j.contains("data")) return false;
    if (j.contains("checksum") && j["checksum"].is_string() &&
        j["checksum"].get<std::string>() != Checksum_(j["data"])) {
      return false; // 途中で切れた・壊れた
    }
    data = j["data"];
    version = j.value("version", 0);
    if (savedAt) *savedAt = j.value("savedAt", std::string());
    return true;
  }

  /// @brief スロットを読む（本体が駄目なら .bak）
  bool ReadSlot_(const std::string &slot, nlohmann::json &data, int &version, std::string *savedAt) const {
    const std::string path = PathOf_(slot);
    if (ReadFile_(path, data, version, savedAt)) return true;
    return ReadFile_(path + ".bak", data, version, savedAt);
  }

  std::string directory_ = "Saves";      ///< 保存先
  std::string slot_ = "default";         ///< 今のスロット
  nlohmann::json data_ = nlohmann::json::object(); ///< 今のスロットの中身
  int version_ = 1;                      ///< データの形のバージョン
  std::function<void(int, nlohmann::json &)> migration_; ///< 古い形を直す処理
  bool dirty_ = false;                   ///< 未保存の変更があるか
};

} // namespace Chaso
