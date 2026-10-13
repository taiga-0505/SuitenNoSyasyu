#pragma once
#include "Game/Framework/GameModeBase.h"

/// @class RailShooterGameMode
/// @brief 『水天の射手』のルール（1 プレイの開始・死亡／クリア判定・結果の記録）
/// @details 以前は DataDrivenScene（エンジン側）に直書きされていた判定をここへ移した。
///          GameSetup.cpp で GameModeBase::SetFactory に登録しているので、
///          すべてのシーンがこの GameMode で動く。プレイシーン以外では何もしない。
class RailShooterGameMode : public GameModeBase {
public:
    /// @brief プレイシーン（Stage1〜5 / テスト用 Game）に入った瞬間を「1 プレイの開始」とする
    /// @details GameSession をリセットし、StageProgress に遊んでいるステージを記録する。
    /// @param scene 入ったシーン
    /// @param ctx   シーンコンテキスト（未使用）
    void OnSceneEnter(Scene& scene, SceneContext& ctx) override;

    /// @brief 自機の死亡 → "gameover"、レール終点到達（レールが無ければ敵全滅）→ "cleared"
    /// @details 行き先（Result）と演出は遷移表（Resources/SceneFlow.json）で決まる
    /// @param scene      判定するシーン
    /// @param ctx        シーンコンテキスト（未使用）
    /// @param outTrigger 決着した場合に遷移表のきっかけ名（kTriggerCleared / kTriggerGameOver）を受け取る
    /// @return 決着がついたら true（プレイシーン以外では常に false）
    bool EvaluateOutcome(Scene& scene, SceneContext& ctx, std::string& outTrigger) override;

    /// @brief レベルデータの敵に AI スクリプトを載せる（"Shark" だけ専用スクリプト）
    /// @param enemy     生成された敵エンティティ（NativeScriptComponent が無ければ追加する）
    /// @param spawnName レベルデータ上の生成名（"Shark" なら SharkEnemyScript、それ以外は EnemyAI）
    void OnLevelEnemySpawned(Entity& enemy, const std::string& spawnName) override;

    /// @brief 遷移表のきっかけ名
    static constexpr const char* kTriggerCleared = "cleared";   ///< クリア
    static constexpr const char* kTriggerGameOver = "gameover"; ///< ゲームオーバー
};
