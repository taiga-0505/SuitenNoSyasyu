#pragma once

/// @brief 見せ用の自動デモ（一時的なもの）
/// @details kEnabled が true のあいだ、人の操作なしで次の流れをループする:
///            Title（kTitleAutoStart のとき kTitleHold 秒後に自動で「スタート」）
///              → Select（kSelectHold 秒後に選択中の面で自動出航）
///              → ステージ: 開始演出 → レールが最初のウェーブ地点に着いて WAVE 1 が始まる
///                → kFrenzyDelay 秒後、サメの群れ（SharkFrenzyDemo）が一斉に襲いかかり、一気に食われて力尽きる
///                → 通常の力尽きた演出（DeathSinkScript：沈む）→ 遷移表のきっかけ "gameover"（Result へ dive）
///              → Result（kResultHold 秒後に「セレクトへ」を選び、kResultConfirmDelay 秒後に決定）
///              → Select → …（以下くり返し）
///          人が操作した場合はそちらが優先される（自動決定までの待ち時間中に通常どおり操作できる）。
///          見せ終わったら kEnabled を false に戻すだけで元の挙動になる。
namespace TransitionDemo {
inline constexpr bool kEnabled = true; ///< デモを有効にするか（見せ終わったら false に戻す）

// ---- ステージ：サメに一気に食われる ----
inline constexpr float kFrenzyDelay = 0.9f;       ///< 最初のウェーブが始まってから群れが動き出すまで（秒。WAVE 表示がせり出しきる頃）
inline constexpr float kWaveFallbackTime = 25.0f; ///< 開始演出後、ウェーブが始まらないまま経ったらこの秒数で群れを出す（保険）
inline constexpr int kSharkCount = 6;             ///< 襲いかかるサメの数（最大 8）
inline constexpr float kLungeTime = 1.4f;         ///< 1 匹が出現位置から噛みつくまで（秒）
inline constexpr float kBiteDistance = 3.4f;      ///< 噛みついたときのカメラからサメの原点までの距離（m）
inline constexpr float kSharkScale = 6.0f;        ///< WaveSpawner の enemyScale と同じ
inline constexpr int kLethalDamage = 999;         ///< 噛みついたときに与えるダメージ（HP に関係なく力尽きる）

// ---- Result / Select / Title の自動操作 ----
inline constexpr float kResultHold = 3.0f;         ///< Result の成績が出そろってから「セレクトへ」を選ぶまで（秒）
inline constexpr float kResultConfirmDelay = 0.8f; ///< 「セレクトへ」を選んでから決定するまで（秒）
inline constexpr float kSelectHold = 2.5f;         ///< Select の UI が出てから出航するまで（秒）
inline constexpr bool kTitleAutoStart = true;      ///< Title でも自動で「スタート」を押すか
inline constexpr float kTitleHold = 3.0f;          ///< Title のメニューが操作できるようになってから「スタート」まで（秒）
} // namespace TransitionDemo
