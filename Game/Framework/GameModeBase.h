#pragma once
#include <functional>
#include <memory>
#include <string>
#include "GameStateBase.h"

// 前方宣言
struct SceneContext;
class Scene;
class Entity;

/// @class GameModeBase
/// @brief ゲームのルール、勝利条件、進行を管理するクラス
/// @details Unreal Engine の AGameModeBase に相当。
///          エンジンは「いつ呼ぶか」だけを持ち、「何を判定するか」は
///          Application 側の派生クラスが決める。派生クラスは SetFactory で登録すると、
///          DataDrivenScene がシーンを読み込むたびにそのファクトリで GameMode を作る。
class GameModeBase {
public:
    /// @brief 既定の GameStateBase を生成して保持する
    GameModeBase();
    virtual ~GameModeBase() = default;

    /// @brief ゲーム開始時の初期化処理（既定では GameState の BeginPlay を呼ぶ）
    /// @param ctx シーンコンテキスト
    virtual void BeginPlay(SceneContext& ctx);

    /// @brief 毎フレームの更新処理（既定では GameState の経過時間を進める）
    /// @param ctx シーンコンテキスト
    virtual void Tick(SceneContext& ctx);

    // ------------------------------------------------------------------
    // シーンから呼ばれるフック（既定では何もしない）
    // ------------------------------------------------------------------

    /// @brief シーンに入った直後に呼ばれる（エンティティ読み込み後、ランタイム初期化前）
    /// @param scene 入ったシーン（scene.Name() でシーン名が取れる）
    /// @param ctx シーンコンテキスト
    /// @details 「1 プレイの開始」をシーン名で判定して結果保持を初期化する、などに使う。
    virtual void OnSceneEnter(Scene& scene, SceneContext& ctx) {}

    /// @brief 決着判定。プレイ中、決着がつくまで毎フレーム呼ばれる
    /// @param scene 判定対象のシーン
    /// @param ctx シーンコンテキスト
    /// @param outTrigger 決着したときの遷移のきっかけ名（例: "cleared" / "gameover"。空なら遷移しない）
    /// @return 決着したら true。true を返した後は同じシーンでは二度と呼ばれない
    /// @details true を返すと、シーンは一定時間の余韻を置いてから、遷移表（SceneFlow）で
    ///          「このシーン × outTrigger」に登録された行き先・演出で遷移する。
    ///          結果の記録（スコア保存など）は true を返す前にここで済ませること。
    virtual bool EvaluateOutcome(Scene& scene, SceneContext& ctx, std::string& outTrigger) {
        return false;
    }

    /// @brief レベルデータの敵スポーン地点からエンティティを生成した直後に呼ばれる
    /// @param enemy 生成したエンティティ（Transform と NativeScriptComponent は付与済み）
    /// @param spawnName レベルデータ側の名前（fileName。空なら "Enemy"）
    /// @details どのスクリプトを載せるかはゲームごとに違うので、ここで AddScript する。
    virtual void OnLevelEnemySpawned(Entity& enemy, const std::string& spawnName) {}

    // ------------------------------------------------------------------

    /// @brief GameState を取得する
    /// @return 保持している GameState（未設定なら nullptr）
    GameStateBase* GetGameState() const { return gameState_.get(); }

    /// @brief BeginPlay 済みかどうか
    /// @return BeginPlay 済みなら true
    bool HasBegunPlay() const { return hasBegunPlay_; }
    /// @brief BeginPlay 済みとして記録する（シーンが BeginPlay を呼んだ直後に呼ぶ）
    void MarkBegunPlay() { hasBegunPlay_ = true; }

    /// @brief シーンを再入場したときに BeginPlay からやり直せる状態へ戻す
    /// @details hasBegunPlay_ は一度立つと二度と下りないため、GameMode を作り直さずに
    ///          使い回すと、リトライしても BeginPlay が呼ばれずスコアと経過時間が
    ///          前回のまま積み上がってしまう。シーンの OnEnter から呼ぶこと。
    virtual void ResetForRestart() {
        hasBegunPlay_ = false;
        if (gameState_) {
            gameState_->Reset();
        }
    }

    /// @brief カスタムの GameState を生成して差し替える（派生クラスのコンストラクタ等で利用）
    /// @tparam T GameStateBase の派生型
    template <typename T>
    void SetGameState() {
        gameState_ = std::make_unique<T>();
    }

    // ------------------------------------------------------------------
    // ファクトリ（Application 側の GameMode を差し込む口）
    // ------------------------------------------------------------------

    /// @brief シーン名から GameMode を作る関数
    using Factory = std::function<std::unique_ptr<GameModeBase>(const std::string& sceneName)>;

    /// @brief GameMode のファクトリを登録する
    /// @param factory 登録するファクトリ（既存の登録は上書きされる）
    /// @details Application 側の静的初期化から呼んでよい（保存先は関数内 static）。
    ///          未登録なら GameModeBase そのものが使われる。
    static void SetFactory(Factory factory);

    /// @brief 登録済みのファクトリで GameMode を作る
    /// @param sceneName 作る対象のシーン名
    /// @return 必ず非 null（ファクトリが null を返したら GameModeBase で埋める）
    static std::unique_ptr<GameModeBase> Create(const std::string& sceneName);

protected:
    std::unique_ptr<GameStateBase> gameState_; ///< ゲームの進行状況
    bool hasBegunPlay_ = false;                ///< BeginPlay 済みか

private:
    /// @brief 登録済みファクトリの保存先を取得する
    /// @return ファクトリの参照
    /// @note 関数内 static にして、静的初期化順序に依存せず SetFactory を呼べるようにしている
    static Factory& FactoryStorage();
};
