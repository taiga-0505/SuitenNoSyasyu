#pragma once

/// @class GameStateBase
/// @brief ゲームの進行状況、スコア、経過時間などを管理するクラス
/// @details Unreal Engine の AGameStateBase に相当。
class GameStateBase {
public:
    GameStateBase() = default;
    virtual ~GameStateBase() = default;

    /// @brief ゲーム開始時の初期化処理
    virtual void BeginPlay() {}

    /// @brief スコアと経過時間を初期状態へ戻す
    /// @details GameState は Scene のメンバであり、シーンを抜けても破棄されない。
    ///          リトライ時に前回の値が残り続けるのを防ぐため、シーン再入場時に呼ぶ。
    virtual void Reset() {
        elapsedTime_ = 0.0f;
        score_ = 0;
    }

    /// @brief 経過時間だけを 0 に戻す
    /// @details 開始演出（深海からの浮上）のあいだ Tick は回り続けるので、演出が終わって
    ///          実際に操作できるようになった時点で呼び、演出の時間をプレイ時間に含めない。
    void ResetElapsedTime() { elapsedTime_ = 0.0f; }

    /// @brief 毎フレームの更新処理
    /// @param deltaTime 前フレームからの経過時間（秒）
    virtual void Tick(float deltaTime) {
        elapsedTime_ += deltaTime;
    }

    /// @brief 経過時間を取得する
    /// @return BeginPlay（またはリセット）からの経過時間（秒）
    float GetElapsedTime() const { return elapsedTime_; }

    /// @brief スコアを加算する
    /// @param score 加算する値
    void AddScore(int score) { score_ += score; }

    /// @brief 現在のスコアを取得する
    /// @return 現在のスコア
    int GetScore() const { return score_; }

protected:
    float elapsedTime_ = 0.0f; ///< 経過時間（秒）
    int score_ = 0;            ///< スコア
};
