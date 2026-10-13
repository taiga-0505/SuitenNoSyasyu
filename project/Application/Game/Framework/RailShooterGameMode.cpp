#include "RailShooterGameMode.h"

#include "Game/Framework/GameSession.h"
#include "Game/Framework/StageProgress.h"
#include "Scene.h"
#include "ECS/Entity.h"
#include "ECS/NativeScriptComponent.h"

void RailShooterGameMode::OnSceneEnter(Scene& scene, SceneContext& ctx) {
    (void)ctx;
    const std::string sceneName = scene.Name();

    // プレイシーンに入った瞬間が「1 プレイの開始」。前回の結果を捨てる。
    // どのステージを遊んでいるかも記録しておく（Result の「次へ／もう一度」が使う）。
    if (StageProgress::IsPlayScene(sceneName)) {
        GameSession::Get().BeginRun();
        StageProgress::Get().SetCurrentScene(sceneName);
    }
}

bool RailShooterGameMode::EvaluateOutcome(Scene& scene, SceneContext& ctx,
                                          std::string& outTrigger) {
    (void)ctx;
    const std::string sceneName = scene.Name();

    // スペースキーで次へ進むのはセレクト〜リザルトの導線シーンだけに限定する。
    // 導線外のシーン（Sample など）では Space をゲーム操作に使う可能性があるため、
    // 名前が一致しないシーンをまとめて Title へ送らないこと。
    // Title は TitleScreenScript がメニュー（スタート／ゲーム終了）で、
    // Result は ResultScreenScript が「もう一度／タイトルへ」で
    // 自前に遷移を扱うため、ここでは判定しない（二重判定になる）。
    // Select は StageSelectScript が自前でステージを選んで遷移するので、ここでは判定しない。
    if (!StageProgress::IsPlayScene(sceneName)) {
        return false;
    }

    const auto& entities = scene.GetEntities();
    auto& session = GameSession::Get();

    bool decided = false;
    const char* trigger = kTriggerCleared;

    // --- プレイヤー死亡チェック ---
    // レールシューターの自機はカメラに乗っていて名前が "player" ではないため、
    // 名前だけでなく is_player タグ（RailShooterController が OnCreate で立てる）でも拾う。
    for (const auto& e : entities) {
        if (!e || !e->HasTag("game_over")) continue;
        if (e->GetName() != "player" && !e->HasTag("is_player")) continue;
        session.Finish(GameSession::Outcome::GameOver);
        trigger = kTriggerGameOver;
        decided = true;
        break;
    }

    // --- クリアチェック（プレイヤーが生きている場合のみ） ---
    //
    // レールが敷かれているシーンでは「終点に到達＝クリア」とする。
    // A-03 のウェーブ戦闘を入れると、ウェーブとウェーブの間に敵が
    // 0 体になる瞬間が必ず生まれるため、「敵が全員撃破された＝クリア」
    // のままだと最初のウェーブを倒した時点で Result へ飛んでしまう。
    // 撃破後の敵は 2 秒で実体ごと消えるので、抑止タグを足すだけでは塞げない。
    //
    // レールが無いシーン（単体テスト用など）は従来どおり全滅で判定する。
    if (!decided) {
        // has_rail を持つエンティティがあればレールシーン。そのどれかに rail_finished が立てば終点到達
        bool hasRail = false;
        bool railFinished = false;
        for (const auto& e : entities) {
            if (!e || !e->HasTag("has_rail")) continue;
            hasRail = true;
            if (e->HasTag("rail_finished")) {
                railFinished = true;
                break;
            }
        }

        bool cleared = false;
        if (hasRail) {
            cleared = railFinished;
        } else {
            // 名前（Enemy / Shark）か is_enemy タグで敵を拾い、1 体以上いて全員 enemy_defeated なら全滅
            bool hasEnemy = false;
            bool allDefeated = true;
            for (const auto& e : entities) {
                if (e && (e->GetName() == "Enemy" || e->GetName() == "Shark" || e->HasTag("is_enemy"))) {
                    hasEnemy = true;
                    if (!e->HasTag("enemy_defeated")) {
                        allDefeated = false;
                        break;
                    }
                }
            }
            cleared = hasEnemy && allDefeated;
        }

        if (cleared) {
            session.Finish(GameSession::Outcome::Cleared);
            // ステージのクリアを記録し、次の面を解放して保存する（Game シーンは対象外）
            const int stageIndex = StageProgress::IndexOf(sceneName);
            if (stageIndex >= 0) {
                StageProgress::Get().MarkCleared(stageIndex, session.Score());
            }
            decided = true;
        }
    }

    if (!decided) {
        return false;
    }

    // 決着がついた時点の経過時間を確定させ、Result へ引き渡す。
    // true を返した後は同じシーンでは呼ばれないので、ここで 1 回だけ書けばよい
    // （決着後もリザルト遷移待ちのあいだ GameState は Tick し続けるため、
    //   毎フレーム上書きすると表示される時間が伸びてしまう）。
    if (gameState_) {
        session.SetElapsedTime(gameState_->GetElapsedTime());
    }

    outTrigger = trigger;
    return true;
}

void RailShooterGameMode::OnLevelEnemySpawned(Entity& enemy, const std::string& spawnName) {
    auto* nsc = enemy.GetComponent<NativeScriptComponent>();
    if (!nsc) {
        nsc = &enemy.AddComponent<NativeScriptComponent>();
    }
    if (spawnName == "Shark") {
        nsc->AddScript("SharkEnemyScript");
    } else {
        nsc->AddScript("EnemyAI");
    }
}
