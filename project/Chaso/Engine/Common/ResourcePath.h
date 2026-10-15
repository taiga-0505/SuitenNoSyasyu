#pragma once
#include <filesystem>
#include <string>

/// @file ResourcePath.h
/// @brief リソースパスの解決（ゲーム側 → エンジン側の順に探す）
/// @details エンジン本体は `project/Chaso/` 配下（ChasoEngine リポジトリの subtree）にあり、
///          エンジンが必要とするリソース（シェーダー・アイコン・フォントなど）は
///          `Chaso/Resources/` に置かれている。一方、コード中のパスは従来どおり
///          `"Resources/..."`（作業ディレクトリ = project/ 基準）で書かれているため、
///          ゲーム側の `Resources/` に無ければ `Chaso/Resources/` を探す。
///
///          ゲーム側で同名ファイルを置けばエンジン側を上書きできる（ゲーム側が優先）。
namespace Chaso {

/// @brief エンジンのルートディレクトリの保存先（作業ディレクトリ基準。既定は "Chaso"）
/// @return ルートディレクトリ文字列への参照
inline std::string &EngineRootRef_() {
  static std::string root = "Chaso";
  return root;
}

/// @brief エンジンのルートディレクトリを変更する（通常は変更不要）
/// @param root 新しいルートディレクトリ（作業ディレクトリ基準）
inline void SetEngineRoot(const std::string &root) { EngineRootRef_() = root; }

/// @brief エンジンのルートディレクトリを取得する
/// @return ルートディレクトリ（作業ディレクトリ基準）
inline const std::string &EngineRoot() { return EngineRootRef_(); }

/// @brief パスを解決する
/// @param path "Resources/..." 形式の相対パス（絶対パスならそのまま返す）
/// @return 実在するパス。ゲーム側 → エンジン側の順で探し、どちらにも無ければ入力をそのまま返す
inline std::filesystem::path ResolvePath(const std::filesystem::path &path) {
  namespace fs = std::filesystem;
  if (path.empty() || path.is_absolute()) {
    return path;
  }
  std::error_code ec;
  if (fs::exists(path, ec)) {
    return path;
  }
  const fs::path engine = fs::path(EngineRoot()) / path;
  if (fs::exists(engine, ec)) {
    return engine;
  }
  return path;
}

/// @brief パスを解決する（std::string 版）
/// @param path "Resources/..." 形式の相対パス
/// @return 解決したパス（区切り文字は '/'）
inline std::string ResolvePath(const std::string &path) {
  return ResolvePath(std::filesystem::path(path)).generic_string();
}

/// @brief パスを解決する（const char* 版）
/// @param path "Resources/..." 形式の相対パス（nullptr は空文字として扱う）
/// @return 解決したパス（区切り文字は '/'）
inline std::string ResolvePath(const char *path) {
  return ResolvePath(std::string(path ? path : ""));
}

/// @brief パスを解決する（std::wstring 版。DXC など wchar_t API 向け）
/// @param path "Resources/..." 形式の相対パス
/// @return 解決したパス（区切り文字は '/'）
inline std::wstring ResolvePath(const std::wstring &path) {
  return ResolvePath(std::filesystem::path(path)).generic_wstring();
}

// ============================================================
// 設定ファイル（ゲーム側 Resources/Setting/ に置く）
// ============================================================

/// @brief 設定ファイルの置き場所（作業ディレクトリ = project/ 基準）
inline constexpr const char *kSettingDir = "Resources/Setting";
inline constexpr const char *kAppConfigPath = "Resources/Setting/AppConfig.json";       ///< 解像度・タイトル・起動シーン
inline constexpr const char *kEditorConfigPath = "Resources/Setting/EditorConfig.json"; ///< エディタのウィンドウ表示状態
inline constexpr const char *kGameSettingsPath = "Resources/Setting/GameSettings.json"; ///< プレイヤー設定（感度・音量）

/// @brief 設定ファイルのパスを用意する（読み書きの直前に呼ぶ）
/// @details - Resources/Setting/ が無ければ作る（保存時に ofstream が失敗しないように）。
///          - 旧配置（project/ 直下の同名ファイル）だけがある場合は Resources/Setting/ へ移動する。
/// @param path kAppConfigPath などの設定ファイルパス
/// @return そのまま path を返す（ifstream / ofstream にそのまま渡せる）
inline const char *PrepareSettingPath(const char *path) {
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path p(path);
  fs::create_directories(p.parent_path(), ec);
  if (!fs::exists(p, ec)) {
    const fs::path legacy = p.filename(); // 旧: project/<name>.json
    if (fs::exists(legacy, ec)) {
      fs::rename(legacy, p, ec);
    }
  }
  return path;
}

} // namespace Chaso
