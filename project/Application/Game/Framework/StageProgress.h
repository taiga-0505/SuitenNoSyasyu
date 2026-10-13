#pragma once
#include "Common/Log/Log.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <fstream>
#include <string>

/// @class StageProgress
/// @brief ステージ構成と進行状況（解放・クリア・ベスト記録）をシーンをまたいで保持する
/// @details
///   ステージ構成（ステージセレクトで順に進む 5 面）:
///     1. 水上        … 外洋。洞窟の入口（崖の口）へ向かう
///     2. 水上        … 外洋 → 洞窟（岩窟水道。崖の口をくぐって洞窟へ入っていく）
///     3. 水上 → 水中 … 洞窟（沈み洞。広間から水没した通路へもぐる）
///     4. 水中 → 水上 … 洞窟（地底湖。水没した通路から地底湖へ浮上する）
///     5. 水上        … 洞窟 → 外洋（黎明の海。洞窟を抜けて外洋へ出る最終面）
///   中間の 3 面（2〜4）が洞窟内（2 は入口、5 は出口をまたぐ）。
///
///   値の流れ:
///     ステージシーン OnEnter -> SetCurrentScene(sceneName)（RailShooterGameMode::OnSceneEnter）
///     クリア時               -> MarkCleared(index, score)（RailShooterGameMode::EvaluateOutcome）→ 次の面を解放して Save()
///     StageSelectScript      -> IsUnlocked / IsCleared / BestScore を読んで一覧を描く
///     ResultScreenScript     -> Current() / NextSceneName() で「次のステージへ」「もう一度」を決める
///
///   保存先は project/StageProgress.json（セーブデータなので Resources/Setting/ には置かない。.gitignore 済み）。
///   ファイルが無ければ「ステージ 1 だけ解放」で始まる。
class StageProgress {
public:
  /// @brief ステージ数
  static constexpr int kStageCount = 5;

  /// @brief ステージの静的な情報（表示名・シーン名・環境）
  struct StageInfo {
    const char *sceneName; ///< SceneManager に登録されているシーン名（Resources/Scenes/<name>.json）
    const char *title;     ///< セレクト画面に出す名前
    const char *subtitle;  ///< 欧文の添え書き
    bool underwater;       ///< 主に水中のステージか（セレクト画面で丸を水面の下に置く）
    bool cave;             ///< 洞窟内のステージか（セレクト画面で岩の囲みに入れる）
    const char *waterText; ///< 情報パネルの「水上／水中」タグ（例: "水上→水中"）
    const char *placeText; ///< 情報パネルの「外洋／洞窟」タグ（例: "外洋→洞窟"）
  };

  /// @brief ステージの静的情報を取得する（一覧の並び順＝進行順）
  /// @param index ステージ番号（0 始まり。範囲外は 0〜kStageCount-1 に丸める）
  /// @return ステージ情報
  static const StageInfo &Info(int index) {
    static const std::array<StageInfo, kStageCount> kStages = {{
        {"Stage1", "外洋", "OPEN SEA", false, false, "水上", "外洋"},
        {"Stage2", "岩窟水道", "CAVERN CHANNEL", false, true, "水上", "外洋→洞窟"},
        {"Stage3", "沈み洞", "FLOODED CAVE", true, true, "水上→水中", "洞窟"},
        {"Stage4", "地底湖", "UNDERGROUND LAKE", false, true, "水中→水上", "洞窟"},
        {"Stage5", "黎明の海", "DAWN SEA", false, false, "水上", "洞窟→外洋"},
    }};
    return kStages[static_cast<size_t>(std::clamp(index, 0, kStageCount - 1))];
  }

  /// @brief シーン名からステージ番号（0 始まり）を引く
  /// @param sceneName シーン名
  /// @return ステージ番号。ステージでなければ -1
  static int IndexOf(const std::string &sceneName) {
    for (int i = 0; i < kStageCount; ++i) {
      if (sceneName == Info(i).sceneName) return i;
    }
    return -1;
  }

  /// @brief プレイ対象のシーンか（ステージ 1〜5）
  /// @details RailShooterGameMode のクリア・死亡判定や GameSession::BeginRun をかける対象。
  /// @param sceneName シーン名
  /// @return ステージ 1〜5 のいずれかなら true
  static bool IsPlayScene(const std::string &sceneName) {
    return IndexOf(sceneName) >= 0;
  }

  /// @brief 唯一のインスタンスを取得する
  /// @return StageProgress のシングルトンインスタンス
  static StageProgress &Get() {
    static StageProgress instance;
    return instance;
  }

  // ------------------------------------------------------------------
  // 現在のプレイ
  // ------------------------------------------------------------------

  /// @brief いま遊んでいる（直前に遊んだ）ステージのシーン名を記録する
  /// @details ステージ以外（Game など）を渡したときは -1 になり、Result は従来どおり振る舞う。
  /// @param sceneName 入ったシーンの名前
  void SetCurrentScene(const std::string &sceneName) {
    current_ = IndexOf(sceneName);
    currentSceneName_ = sceneName;
  }
  /// @brief いまのステージ番号
  /// @return 0 始まりの番号。ステージ外なら -1
  int Current() const { return current_; }
  /// @brief 直前にプレイしたシーン名（リトライの遷移先）
  /// @return シーン名。未設定なら "Stage1"
  const std::string &CurrentSceneName() const {
    static const std::string kFallback = "Stage1";
    return currentSceneName_.empty() ? kFallback : currentSceneName_;
  }
  /// @brief 次のステージのシーン名
  /// @return シーン名。最終面・ステージ外なら空文字
  std::string NextSceneName() const {
    if (current_ < 0 || current_ + 1 >= kStageCount) return {};
    return Info(current_ + 1).sceneName;
  }
  /// @brief 最終面か
  /// @return 現在のステージが最終面なら true
  bool IsFinalStage() const { return current_ == kStageCount - 1; }

  /// @brief セレクト画面で最後にカーソルを置いていたステージ（戻ってきたときの初期位置）の取得・設定
  /// @param index 設定するステージ番号（0〜kStageCount-1 に丸める）
  /// @return 記録しているステージ番号
  int LastSelected() const { return std::clamp(lastSelected_, 0, kStageCount - 1); }
  void SetLastSelected(int index) { lastSelected_ = std::clamp(index, 0, kStageCount - 1); }

  // ------------------------------------------------------------------
  // 解放・クリア記録
  // ------------------------------------------------------------------

  /// @brief ステージが解放済みか（ステージ 1 は常に解放。未読込なら先にファイルを読む）
  /// @param index ステージ番号（0 始まり）
  /// @return 解放済みなら true。範囲外は false
  bool IsUnlocked(int index) const {
    EnsureLoaded();
    if (index < 0 || index >= kStageCount) return false;
    return index == 0 || unlockAll_ || index <= unlocked_;
  }
  /// @brief ステージをクリア済みか（未読込なら先にファイルを読む）
  /// @param index ステージ番号（0 始まり）
  /// @return クリア済みなら true。範囲外は false
  bool IsCleared(int index) const {
    EnsureLoaded();
    if (index < 0 || index >= kStageCount) return false;
    return cleared_[static_cast<size_t>(index)];
  }
  /// @brief ステージのベストスコア（未読込なら先にファイルを読む）
  /// @param index ステージ番号（0 始まり）
  /// @return ベストスコア。範囲外は 0
  int BestScore(int index) const {
    EnsureLoaded();
    if (index < 0 || index >= kStageCount) return 0;
    return bestScore_[static_cast<size_t>(index)];
  }
  /// @brief 解放済みのうち最も先のステージ番号
  /// @return ステージ番号（0 始まり。全解放中なら最終面）
  int HighestUnlocked() const {
    EnsureLoaded();
    return unlockAll_ ? kStageCount - 1 : std::clamp(unlocked_, 0, kStageCount - 1);
  }

  /// @brief クリアを記録し、次のステージを解放して保存する
  /// @param index クリアしたステージ番号（0 始まり。範囲外なら何もしない）
  /// @param score 今回のスコア（ベストを超えていれば更新する）
  /// @return 今回のクリアで新しく解放されたステージがあれば true
  bool MarkCleared(int index, int score) {
    EnsureLoaded();
    if (index < 0 || index >= kStageCount) return false;
    const size_t i = static_cast<size_t>(index);
    cleared_[i] = true;
    bestScore_[i] = (std::max)(bestScore_[i], score);
    // 次の面がまだ解放されていなければ解放し、セレクト画面の演出用に記録する
    bool newlyUnlocked = false;
    if (index + 1 < kStageCount && unlocked_ < index + 1) {
      unlocked_ = index + 1;
      newlyUnlocked = true;
    }
    justUnlocked_ = newlyUnlocked ? index + 1 : -1;
    Save();
    Log::Print("[StageProgress] cleared " + std::string(Info(index).sceneName) +
               (newlyUnlocked ? (" -> unlocked " + std::string(Info(index + 1).sceneName)) : ""));
    return newlyUnlocked;
  }

  /// @brief 直前のクリアで新しく解放されたステージ。セレクト画面の演出用
  /// @return ステージ番号（無ければ -1）
  int JustUnlocked() const { return justUnlocked_; }
  /// @brief 解放演出を見せ終えたので JustUnlocked を -1 に戻す
  void ConsumeJustUnlocked() { justUnlocked_ = -1; }

  /// @brief 進行をすべて消す（デバッグ用）
  void ResetAll() {
    unlocked_ = 0;
    cleared_.fill(false);
    bestScore_.fill(0);
    justUnlocked_ = -1;
    Save();
  }

  /// @brief 全ステージを解放済み扱いにするフラグの設定・取得（保存はしない。デバッグ用）
  /// @param v true で全解放扱い
  /// @return 全解放扱いなら true
  void SetUnlockAll(bool v) { unlockAll_ = v; }
  bool UnlockAll() const { return unlockAll_; }

  // ------------------------------------------------------------------
  // 保存
  // ------------------------------------------------------------------

  /// @brief セーブデータを読み込む（無ければ「ステージ 1 だけ解放」のまま）
  /// @return ファイルを開けたら true（パースに失敗して初期状態へ戻した場合も true）
  bool Load() {
    loaded_ = true;
    std::ifstream ifs(kPath);
    if (!ifs.is_open()) return false;
    // 項目ごとに型を確かめて読み、壊れたファイルなら初期状態から始める
    try {
      nlohmann::json j;
      ifs >> j;
      if (j.contains("unlocked") && j["unlocked"].is_number_integer()) {
        unlocked_ = std::clamp(j["unlocked"].get<int>(), 0, kStageCount - 1);
      }
      if (j.contains("stages") && j["stages"].is_array()) {
        const auto &arr = j["stages"];
        for (size_t i = 0; i < arr.size() && i < static_cast<size_t>(kStageCount); ++i) {
          const auto &s = arr[i];
          if (s.contains("cleared") && s["cleared"].is_boolean()) cleared_[i] = s["cleared"].get<bool>();
          if (s.contains("best") && s["best"].is_number_integer()) bestScore_[i] = s["best"].get<int>();
        }
      }
      Log::Print(std::string("[StageProgress] loaded ") + kPath);
    } catch (...) {
      Log::Print(std::string("[StageProgress] failed to parse ") + kPath + " (starting fresh)");
      unlocked_ = 0;
      cleared_.fill(false);
      bestScore_.fill(0);
    }
    return true;
  }

  /// @brief セーブデータを書き出す
  /// @return 書けたら true
  bool Save() const {
    std::ofstream ofs(kPath);
    if (!ofs.is_open()) {
      Log::Print(std::string("[StageProgress] failed to open for write: ") + kPath);
      return false;
    }
    nlohmann::json stages = nlohmann::json::array();
    for (int i = 0; i < kStageCount; ++i) {
      stages.push_back({{"scene", Info(i).sceneName},
                        {"cleared", cleared_[static_cast<size_t>(i)]},
                        {"best", bestScore_[static_cast<size_t>(i)]}});
    }
    ofs << nlohmann::json{{"unlocked", unlocked_}, {"stages", stages}}.dump(2);
    return true;
  }

private:
  StageProgress() = default;

  /// @brief まだ読み込んでいなければ Load する（const な参照系からも遅延読み込みできるよう const_cast する）
  void EnsureLoaded() const {
    if (!loaded_) const_cast<StageProgress *>(this)->Load();
  }

  static constexpr const char *kPath = "../project/StageProgress.json"; ///< セーブデータ（配布パッケージには含めない）

  bool loaded_ = false;    ///< Load 済みか
  bool unlockAll_ = false; ///< デバッグ用の全解放フラグ（保存しない）
  int unlocked_ = 0; ///< 解放済みの最大ステージ番号（0 始まり）
  std::array<bool, kStageCount> cleared_{};  ///< ステージごとのクリア済みフラグ
  std::array<int, kStageCount> bestScore_{}; ///< ステージごとのベストスコア
  int current_ = -1;             ///< いまのステージ番号（ステージ外なら -1）
  std::string currentSceneName_; ///< 直前にプレイしたシーン名
  int lastSelected_ = 0;         ///< セレクト画面で最後に選んでいたステージ
  int justUnlocked_ = -1;        ///< 直前のクリアで解放されたステージ（無ければ -1）
};
