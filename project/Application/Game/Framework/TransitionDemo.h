#pragma once

/// @brief 見せ用の自動デモ（一時的なもの）
/// @details kEnabled が true のあいだ、人の操作なしで流れをループする。kMode で 2 種類から選ぶ。
///
///   Mode::Clear（クリアを見せる）:
///     Select（kClearStage の面へカーソルを移して kSelectHold 秒後に自動出航）
///       → ステージ: 開始演出 → kClearPlayTime 秒ふつうに進む（近くの敵へ自動で照準を合わせて撃つ）
///         → 画面を暗転し、そのあいだにレールの終点の手前（kFinalRunDistance m）へ移る（ウェーブは打ち切り）
///         → 明けると前方にサメが現れる（ゲームのウェーブと同じ SharkEnemyScript。潜って近づき、
///            浮上しながら詰めてくる）→ 水面に出てきたところへ照準を合わせて撃ち倒す
///         → レールが終点に着いてクリア演出（ClearAscendScript の海図）→ "cleared"（Result へ ascend）
///       → Result（kResultHold 秒後に「セレクトへ」を選び、kResultConfirmDelay 秒後に決定）
///       → Select → …（以下くり返し）
///     デモ中は自機が被弾しない（demo_invincible）。クリアは通常どおり記録・保存される。
///     ステージ側の制御は ClearRunDemo.h（DeathSinkScript から呼ぶ）。
///
///   Mode::GameOver（力尽きるのを見せる）:
///     Title（kTitleAutoStart のとき kTitleHold 秒後に自動で「スタート」）
///       → Select（kSelectHold 秒後に選択中の面で自動出航）
///       → ステージ: 開始演出 → レールが最初のウェーブ地点に着いて WAVE 1 が始まる
///         → kFrenzyDelay 秒後、サメの群れ（SharkFrenzyDemo）が一斉に襲いかかり、一気に食われて力尽きる
///         → 通常の力尽きた演出（DeathSinkScript：沈む）→ 遷移表のきっかけ "gameover"（Result へ dive）
///       → Result → Select → …（Clear と同じ）
///
///   人が操作した場合はそちらが優先される（自動決定までの待ち時間中に通常どおり操作できる）。
///   見せ終わったら kEnabled を false に戻すだけで元の挙動になる。
namespace TransitionDemo {
inline constexpr bool kEnabled = true; ///< デモを有効にするか（見せ終わったら false に戻す）

/// @brief デモの種類
enum class Mode {
  Clear,    ///< 終盤のサメを倒してクリア → Result → Select をくり返す
  GameOver, ///< サメの群れに食われて力尽きる → Result → Select をくり返す
};
inline constexpr Mode kMode = Mode::Clear; ///< どちらのデモを流すか

// ---- ステージ（Mode::Clear）：暗転で終盤へ飛び、サメを倒してクリア ----
inline constexpr int kClearStage = 0;               ///< 毎回遊ぶ面（0 始まり。0 = 1 面）
inline constexpr float kClearPlayTime = 2.5f;       ///< 開始演出が終わってから暗転を始めるまで（秒）
inline constexpr float kSkipFadeOut = 0.5f;         ///< 暗転にかける時間（秒）
inline constexpr float kSkipBlackHold = 0.35f;      ///< 暗転しきったまま待つ時間（秒。この間にレールを移す）
inline constexpr float kSkipFadeIn = 0.6f;          ///< 明けるまでの時間（秒）
inline constexpr float kFinalRunDistance = 10.0f;   ///< 終点の何 m 手前へ移るか
inline constexpr float kSharkStartDistance = 35.0f; ///< サメが現れる距離（カメラの前方。m。1 面のウェーブ 3 の aheadDistance と同じ）
inline constexpr float kSharkStartSide = 4.0f;      ///< サメが現れる横のずれ（m。正で右）
inline constexpr int kSharkHp = 3;                  ///< サメの体力（1 面のウェーブのサメと同じ maxHp）
inline constexpr float kSharkReadyDepth = 2.0f;     ///< サメがこの深さ（水面から。m）より浮いてきたら撃ち始める
inline constexpr float kFightTimeout = 6.0f;        ///< サメが出てから倒せなかったら強制的に倒すまで（秒。保険）
inline constexpr float kAfterKillDelay = 0.8f;      ///< 倒してからレールを再び進めるまで（秒）
inline constexpr float kAimTolerance = 0.045f;      ///< 照準のずれがこの角度（rad）以内なら撃つ
inline constexpr float kAimRange = 45.0f;           ///< 序盤に自動で狙う敵の最大距離（m）

// ---- ステージ（Mode::GameOver）：サメに一気に食われる ----
inline constexpr float kFrenzyDelay = 0.9f;       ///< 最初のウェーブが始まってから群れが動き出すまで（秒。WAVE 表示がせり出しきる頃）
inline constexpr float kWaveFallbackTime = 25.0f; ///< 開始演出後、ウェーブが始まらないまま経ったらこの秒数で群れを出す（保険）
inline constexpr int kSharkCount = 6;             ///< 襲いかかるサメの数（最大 8）
inline constexpr float kLungeTime = 1.4f;         ///< 1 匹が出現位置から噛みつくまで（秒）
inline constexpr float kBiteDistance = 3.4f;      ///< 噛みついたときのカメラからサメの原点までの距離（m）
inline constexpr float kSharkScale = 6.0f;        ///< 1 面のウェーブの enemyScale と同じ（クリアのデモのサメも同じ大きさ）
inline constexpr int kLethalDamage = 999;         ///< 噛みついたときに与えるダメージ（HP に関係なく力尽きる）

// ---- Result / Select / Title の自動操作 ----
inline constexpr float kResultHold = 3.0f;         ///< Result の成績が出そろってから「セレクトへ」を選ぶまで（秒）
inline constexpr float kResultConfirmDelay = 0.8f; ///< 「セレクトへ」を選んでから決定するまで（秒）
inline constexpr float kSelectHold = 2.5f;         ///< Select の UI が出てから出航するまで（秒）
inline constexpr float kSelectMoveAt = 1.2f;       ///< Mode::Clear で kClearStage へカーソルを動かすまで（秒）
inline constexpr bool kTitleAutoStart = true;      ///< Title でも自動で「スタート」を押すか
inline constexpr float kTitleHold = 3.0f;          ///< Title のメニューが操作できるようになってから「スタート」まで（秒）

/// @brief クリアのデモを流しているか
/// @return kEnabled かつ kMode が Clear なら true
inline constexpr bool IsClearDemo() { return kEnabled && kMode == Mode::Clear; }
/// @brief 力尽きるデモを流しているか
/// @return kEnabled かつ kMode が GameOver なら true
inline constexpr bool IsGameOverDemo() { return kEnabled && kMode == Mode::GameOver; }
} // namespace TransitionDemo
