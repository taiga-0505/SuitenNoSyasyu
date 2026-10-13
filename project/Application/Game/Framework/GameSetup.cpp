// ============================================================================
// GameSetup — エンジンへゲーム固有の設定を差し込む場所
// ----------------------------------------------------------------------------
// エンジン（project/Chaso）はゲームのことを知らない。ゲームごとに違う部分は
// エンジンが用意した「登録口」へ、Application 側からここで流し込む。
//
// 静的初期化（main より前）で実行する。スクリプトの REGISTER_SCRIPT と同じ仕組み。
// エンジン側の登録口はすべて関数内 static に保存しているので、初期化順の問題は起きない。
// ============================================================================

#include "Game/Framework/GameModeBase.h"
#include "Game/Framework/RailShooterGameMode.h"
#include "Game/Framework/StageProgress.h"
#include "Game/Framework/UnderwaterLook.h"
#include "SceneFlow.h"
#include "SceneManager.h"

#include <memory>
#include <string>

namespace {

/// @brief 静的初期化のタイミングでゲーム固有の設定をエンジンの登録口へ流し込む
/// @details グローバル変数 g_gameSetup のコンストラクタとして main より前に一度だけ実行される。
struct GameSetup {
  /// @brief GameMode のファクトリ・Dive 遷移の画面色・遷移表の変数をエンジンへ登録する
  GameSetup() {
    // すべてのシーンを『水天の射手』のルールで動かす
    GameModeBase::SetFactory([](const std::string & /*sceneName*/) {
      return std::make_unique<RailShooterGameMode>();
    });

    // Dive 遷移（Title の潜水 → Game の浮上）は深海の画面色へ抜ける。
    // Title はこの色まで暗くしてから遷移を要求し、Game 側はこの色から浮上を始めるので継ぎ目が見えない。
    Scene::SceneManager::SetDiveScreenColor(UnderwaterLook::kAbyssScreenColor);

    // 遷移表（Resources/SceneFlow.json）の遷移先に書ける変数
    auto &flow = SceneFlow::Get();
    flow.RegisterVariable("$lastPlay", "直前に遊んだステージ（Result の「もう一度」）", []() {
      return StageProgress::Get().CurrentSceneName();
    });
    flow.RegisterVariable("$nextStage", "次のステージ（最終面・ステージ外なら行き先なし）", []() {
      return StageProgress::Get().NextSceneName();
    });
  }
};

GameSetup g_gameSetup;

} // namespace
