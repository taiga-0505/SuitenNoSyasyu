#pragma once

// ============================================================================
// Prefab
// ----------------------------------------------------------------------------
// エンティティ（子孫込み）を JSON ファイルに保存し、実行時に何度でも生成するための仕組み。
//
//   保存: エディタの Hierarchy で右クリック →「Save as Prefab」
//         → Resources/Prefabs/<名前>.prefab.json
//   生成: スクリプトから  InstantiatePrefab("Prefabs/Bullet", muzzlePos);
//         シーンから      scene->InstantiatePrefab("Prefabs/Bullet", pos);
//         エディタでは Content Browser から .prefab.json を Viewport へドラッグ＆ドロップ
//
// ファイル形式:
//   { "version": 1, "name": "Bullet", "entities": [ <ルート>, <子>, <孫>, ... ] }
//   entities の中身は Entity::Serialize() と同じ。先頭がルート。
//   生成のたびに GUID を振り直し、親子関係（parentGuid）はプレハブ内で付け替える。
//
// パスの書き方（NormalizePath）:
//   "Bullet"                               → Resources/Prefabs/Bullet.prefab.json
//   "Prefabs/Enemy/Goblin"                 → Resources/Prefabs/Enemy/Goblin.prefab.json
//   "Resources/Prefabs/Bullet.prefab.json" → そのまま
// ============================================================================

#include "ECS/Entity.h"
#include "Common/Log/Log.h"
#include "Common/ResourcePath.h"
#include <nlohmann/json.hpp>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace Prefab {

inline constexpr const char *kDirectory = "Resources/Prefabs"; ///< 既定の保存先
inline constexpr const char *kExtension = ".prefab.json";     ///< プレハブの拡張子
inline constexpr int kFormatVersion = 1;                       ///< ファイル形式のバージョン

/// @brief プレハブのファイルかどうか（拡張子で判定）
/// @param path 調べるパス
/// @return .prefab.json で終わっていれば true（大文字小文字は区別しない）
inline bool IsPrefabPath(const std::string &path) {
  const std::string ext = kExtension;
  if (path.size() < ext.size()) return false;
  for (size_t i = 0; i < ext.size(); ++i) {
    const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(path[path.size() - ext.size() + i])));
    if (a != ext[i]) return false;
  }
  return true;
}

/// @brief 省略した書き方のパスを、実際のファイルパスにそろえる
/// @param path "Bullet" / "Prefabs/Bullet" / "Resources/Prefabs/Bullet.prefab.json" など
/// @return Resources/ から始まり .prefab.json で終わるパス（絶対パスはそのまま）
inline std::string NormalizePath(const std::string &path) {
  std::string s = std::filesystem::path(path).generic_string();
  if (s.empty()) return s;
  if (!IsPrefabPath(s)) s += kExtension;
  if (std::filesystem::path(s).is_absolute()) return s;
  if (s.rfind("Resources/", 0) == 0) return s;
  if (s.rfind("Prefabs/", 0) == 0) return "Resources/" + s;
  return std::string(kDirectory) + "/" + s;
}

/// @brief エンティティ名をファイル名に使える文字列にする
/// @param name エンティティ名
/// @return Windows のファイル名に使えない文字を '_' に置き換えた名前（空なら "Prefab"）
inline std::string SanitizeFileName(const std::string &name) {
  std::string out = name;
  for (char &c : out) {
    switch (c) {
    case '\\': case '/': case ':': case '*': case '?': case '"': case '<': case '>': case '|':
      c = '_';
      break;
    default:
      if (static_cast<unsigned char>(c) < 0x20) c = '_';
      break;
    }
  }
  // 末尾のドット・空白は Windows で問題になるので落とす
  while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();
  return out.empty() ? std::string("Prefab") : out;
}

/// @brief root とその子孫をプレハブの JSON にする
/// @param entities シーンのエンティティ一覧（Scene::GetEntities()）
/// @param root     プレハブにするエンティティ
/// @return プレハブの JSON（root が無ければ null）
/// @details 子孫は parentGuid を幅優先でたどって集める（先頭がルート）。
///          破棄予定のエンティティは含めない。GUID が循環していても止まる。
inline nlohmann::json Capture(const std::vector<std::shared_ptr<Entity>> &entities,
                              const std::shared_ptr<Entity> &root) {
  if (!root) return nullptr;
  std::vector<Entity *> subtree;
  std::set<uint64_t> visited;
  subtree.push_back(root.get());
  visited.insert(root->Guid());
  for (size_t i = 0; i < subtree.size(); ++i) {
    const uint64_t parentGuid = subtree[i]->Guid();
    for (const auto &e : entities) {
      if (!e || e->IsPendingDestroy()) continue;
      if (e->ParentGuid() != parentGuid) continue;
      if (!visited.insert(e->Guid()).second) continue;
      subtree.push_back(e.get());
    }
  }

  nlohmann::json arr = nlohmann::json::array();
  for (Entity *e : subtree) arr.push_back(e->Serialize());
  // ルートの親はプレハブの外なので切り離して保存する
  arr[0]["parentGuid"] = 0;

  return {{"version", kFormatVersion}, {"name", root->Name()}, {"entities", std::move(arr)}};
}

namespace detail {
/// @brief 読み込んだプレハブのキャッシュ（正規化したパス → JSON。読めなかったものは null）
inline std::unordered_map<std::string, nlohmann::json> &Cache() {
  static std::unordered_map<std::string, nlohmann::json> cache;
  return cache;
}
} // namespace detail

/// @brief プレハブの JSON をファイルへ保存する（キャッシュも更新する）
/// @param prefab Capture() の戻り値
/// @param path   保存先（NormalizePath で正規化する）
/// @return 保存できたら true
inline bool Save(const nlohmann::json &prefab, const std::string &path) {
  if (!prefab.is_object() || !prefab.contains("entities")) return false;
  const std::string file = NormalizePath(path);
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(file).parent_path(), ec);
  std::ofstream ofs(file);
  if (!ofs) {
    Log::Print("[Prefab] 保存できませんでした: " + file);
    return false;
  }
  // 名前などに不正な UTF-8 が混ざっていても例外を投げないようにする
  ofs << prefab.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
  detail::Cache()[file] = prefab;
  Log::Print("[Prefab] 保存しました: " + file);
  return true;
}

/// @brief プレハブを読み込む（2 回目以降はキャッシュを返す）
/// @param path プレハブのパス（NormalizePath で正規化する）
/// @return プレハブの JSON（読めなければ nullptr）。ClearCache までポインタは有効
/// @note 読めなかったパスも覚えておき、毎フレーム同じエラーを出さないようにする。
///       ファイルを直したら ClearCache() を呼ぶ（エディタの保存では自動で更新される）。
inline const nlohmann::json *Load(const std::string &path) {
  const std::string file = NormalizePath(path);
  auto &cache = detail::Cache();
  if (auto it = cache.find(file); it != cache.end()) {
    return it->second.is_null() ? nullptr : &it->second;
  }

  const std::string resolved = Chaso::ResolvePath(file);
  std::ifstream ifs(resolved);
  nlohmann::json j;
  if (ifs) {
    j = nlohmann::json::parse(ifs, nullptr, /*allow_exceptions=*/false);
  }
  const bool ok = j.is_object() && j.contains("entities") && j["entities"].is_array() &&
                  !j["entities"].empty();
  if (!ok) {
    Log::Print("[Prefab] 読み込めませんでした: " + file);
    cache[file] = nullptr;
    return nullptr;
  }
  auto [it, inserted] = cache.insert_or_assign(file, std::move(j));
  return &it->second;
}

/// @brief 読み込み済みのプレハブを忘れる（次の Load でファイルを読み直す）
inline void ClearCache() { detail::Cache().clear(); }

} // namespace Prefab
