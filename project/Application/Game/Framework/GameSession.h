#pragma once

/// @class GameSession
/// @brief シーンをまたいで 1 プレイ分の結果を保持する
/// @details Scene が持つ GameMode / GameState はシーンごとのインスタンスなので、
///          Game シーンで積んだスコアを Result シーンから読むことができない。
///          そこでプレイ結果だけをここに集約し、Result から参照する。
///
///          値の流れ:
///            Game シーン OnEnter    -> BeginRun()   （前回のプレイ結果を捨てる）
///            プレイ中               -> RailShooterController が score / hp / 被ダメージを書き込む
///                                      RailMovementScript がウェイポイントの通過数を書き込む
///                                      撃破数は Observer 経由で数える: EnemyBaseScript が撃破時に
///                                      GameEvents::EnemyDefeated() を発行し、それを購読している
///                                      RailShooterGameMode が AddEnemyDefeated() を呼ぶ
///                                      （敵は GameSession を知らない）
///            クリア or 死亡         -> Finish(Outcome) をシーン側が呼ぶ
///            Result                 -> Score() 等を読んで表示する（クリア／ゲームオーバー共通）
///
///          Singleton にしている理由: シーンをまたいで残る「今回のプレイ結果」は常に 1 つだけで、
///          Game シーンと Result シーンという寿命の異なる場所から同じ値を読み書きする必要があるため。
///          値はすべて private にし、書き込みはプレイ中の Set / Add 系、読み取りは const の Getter に限定している。
class GameSession {
public:
  /// @brief 1 プレイの決着のつき方
  enum class Outcome {
    InProgress, ///< プレイ中（まだ決着していない）
    Cleared,    ///< 敵を全滅させてクリアした
    GameOver    ///< プレイヤーが力尽きた
  };

  /// @brief 唯一のインスタンスを取得する
  /// @return GameSession のシングルトンインスタンス
  static GameSession &Get() {
    static GameSession instance;
    return instance;
  }

  // Singleton なのでコピー・代入はさせない
  GameSession(const GameSession &) = delete;
  GameSession &operator=(const GameSession &) = delete;

  /// @brief 新しいプレイを開始する（Game シーンの OnEnter で呼ぶ）
  /// @details リトライのたびにスコアと経過時間をゼロへ戻す。
  ///          これを呼ばないと 2 周目以降にスコアが加算され続ける。
  void BeginRun() {
    score_ = 0;
    playerHp_ = 0;
    playerMaxHp_ = 0;
    elapsedTime_ = 0.0f;
    outcome_ = Outcome::InProgress;
    waypointsReached_ = 0;
    waypointsTotal_ = 0;
    enemiesDefeated_ = 0;
    damageTaken_ = 0;
    chestsCollected_ = 0;
    ++playCount_;
  }

  /// @brief プレイの決着を記録する
  /// @param outcome 決着の種類（Cleared / GameOver）
  void Finish(Outcome outcome) { outcome_ = outcome; }

  // --- プレイ中に書き込む値 ---
  /// @brief 現在のスコアを書き込む
  /// @param score スコア
  void SetScore(int score) { score_ = score; }
  /// @brief プレイヤーの HP を書き込む
  /// @param hp    現在の HP
  /// @param maxHp 最大 HP
  void SetPlayerHp(int hp, int maxHp) {
    playerHp_ = hp;
    playerMaxHp_ = maxHp;
  }
  /// @brief プレイ開始からの経過時間を書き込む
  /// @param seconds 経過時間（秒）
  void SetElapsedTime(float seconds) { elapsedTime_ = seconds; }

  /// @brief ウェイポイントの進捗を書き込む（RailMovementScript が毎フレーム呼ぶ）
  /// @param reached このプレイで到達したウェイポイントの数
  /// @param total   到達済み ＋ 終点までに残っている数（＝このルートで通る予定の総数）
  /// @details total は「配列の長さ」ではない。分岐で通らないウェイポイントを
  ///          分母に入れると、本線を走りきっても 100% にならないため、
  ///          レール側が「いまのルートで終点までに通る数」を都度数え直して渡す。
  void SetWaypointProgress(int reached, int total) {
    waypointsReached_ = (reached < 0) ? 0 : reached;
    waypointsTotal_ = (total < waypointsReached_) ? waypointsReached_ : total;
  }

  /// @brief 敵を撃破した
  /// @details RailShooterGameMode が GameEvents::EnemyDefeated() を購読し、通知のたびに呼ぶ（Observer）。
  ///          敵側（EnemyBaseScript）からは直接呼ばない。
  /// @param count 加算する撃破数（既定 1）
  void AddEnemyDefeated(int count = 1) { enemiesDefeated_ += count; }

  /// @brief プレイヤーが被弾した（RailShooterController::TakeDamage が呼ぶ）
  /// @param amount 実際に減った HP の量（無敵中に弾かれたぶんは含めない）
  void AddDamageTaken(int amount) {
    if (amount > 0) damageTaken_ += amount;
  }

  /// @brief 宝箱を開けた（TreasureChestScript が HP 0 になった瞬間に呼ぶ）
  /// @details 得点そのものは score_add 経由でスコアに合算されるので、ここでは個数だけ数える。
  /// @param count 加算する個数（既定 1）
  void AddChestCollected(int count = 1) { chestsCollected_ += count; }

  /// @brief 撃破数を直接書く（撮影モードの仕込み用。プレイ中は AddEnemyDefeated を使うこと）
  /// @param count 撃破数（負なら 0 に丸める）
  void SetEnemiesDefeated(int count) { enemiesDefeated_ = (count < 0) ? 0 : count; }
  /// @brief 被ダメージ合計を直接書く（撮影モードの仕込み用。プレイ中は AddDamageTaken を使うこと）
  /// @param amount 被ダメージ合計（負なら 0 に丸める）
  void SetDamageTaken(int amount) { damageTaken_ = (amount < 0) ? 0 : amount; }
  /// @brief 宝箱取得数を直接書く（撮影モードの仕込み用。プレイ中は AddChestCollected を使うこと）
  /// @param count 宝箱取得数（負なら 0 に丸める）
  void SetChestsCollected(int count) { chestsCollected_ = (count < 0) ? 0 : count; }

  // --- Result から読む値 ---
  /// @brief 記録したスコア
  /// @return スコア
  int Score() const { return score_; }
  /// @brief 記録したプレイヤーの HP
  /// @return HP
  int PlayerHp() const { return playerHp_; }
  /// @brief 記録したプレイヤーの最大 HP
  /// @return 最大 HP
  int PlayerMaxHp() const { return playerMaxHp_; }
  /// @brief 記録したプレイ開始からの経過時間
  /// @return 経過時間（秒）
  float ElapsedTime() const { return elapsedTime_; }
  /// @brief プレイの決着
  /// @return 決着の種類（InProgress / Cleared / GameOver）
  Outcome GetOutcome() const { return outcome_; }
  /// @brief クリアで決着したかどうか
  /// @return Outcome::Cleared なら true
  bool IsCleared() const { return outcome_ == Outcome::Cleared; }

  /// @brief 到達したウェイポイントの数
  /// @return 到達数
  int WaypointsReached() const { return waypointsReached_; }
  /// @brief このルートで通る予定だったウェイポイントの総数（0 ならレールが無いシーン）
  /// @return 総数
  int WaypointsTotal() const { return waypointsTotal_; }
  /// @brief ウェイポイント達成率（0.0〜1.0）
  /// @details レールの無いシーンやデータが無い場合はクリアなら 1.0、それ以外 0.0 を返す。
  /// @return 到達数 / 総数 を 0.0〜1.0 に丸めた値
  float WaypointRate() const {
    if (waypointsTotal_ <= 0) return IsCleared() ? 1.0f : 0.0f;
    float rate = static_cast<float>(waypointsReached_) / static_cast<float>(waypointsTotal_);
    if (rate < 0.0f) rate = 0.0f;
    if (rate > 1.0f) rate = 1.0f;
    return rate;
  }
  /// @brief 撃破した敵の数
  /// @return 撃破数
  int EnemiesDefeated() const { return enemiesDefeated_; }
  /// @brief 被弾した合計ダメージ
  /// @return 被ダメージ合計
  int DamageTaken() const { return damageTaken_; }
  /// @brief 開けた宝箱の数
  /// @return 宝箱取得数
  int ChestsCollected() const { return chestsCollected_; }

  /// @brief 何回プレイしたか（1 周目なら 1）
  /// @return BeginRun が呼ばれた回数
  int PlayCount() const { return playCount_; }

  /// @brief クリア時の評価ランク（'S' / 'A' / 'B' / 'C'）
  /// @details 残 HP の割合から算出する簡易評価（現状スコアは見ていない）。
  ///          ゲームオーバー時は常に 'C' を返す。
  /// @return 評価ランクの文字
  char Rank() const {
    if (outcome_ != Outcome::Cleared) return 'C';
    const float hpRate =
        (playerMaxHp_ > 0) ? static_cast<float>(playerHp_) / playerMaxHp_ : 0.0f;
    // 残 HP 率で段階分け（99% 以上 S / 60% 以上 A / 30% 以上 B / それ未満 C）
    if (hpRate >= 0.99f) return 'S';
    if (hpRate >= 0.6f) return 'A';
    if (hpRate >= 0.3f) return 'B';
    return 'C';
  }

private:
  GameSession() = default;

  int score_ = 0;                         ///< スコア
  int playerHp_ = 0;                      ///< プレイヤーの HP
  int playerMaxHp_ = 0;                   ///< プレイヤーの最大 HP
  float elapsedTime_ = 0.0f;              ///< プレイ開始からの経過時間（秒）
  Outcome outcome_ = Outcome::InProgress; ///< 決着
  int playCount_ = 0;                     ///< BeginRun が呼ばれた回数
  int waypointsReached_ = 0;              ///< 到達したウェイポイント数
  int waypointsTotal_ = 0;                ///< このルートで通る予定のウェイポイント総数
  int enemiesDefeated_ = 0;               ///< 撃破数
  int damageTaken_ = 0;                   ///< 被ダメージ合計
  int chestsCollected_ = 0;               ///< 宝箱取得数
};
