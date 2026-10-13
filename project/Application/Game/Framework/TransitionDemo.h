#pragma once

/// @brief シーン遷移の見せ用デモ（一時的なもの）
/// @details kEnabled が true のあいだ、ステージは
///            浮上の開始演出が終わって GAME START が出る → kDelayAfterStart 秒 →
///            力尽きたときと同じ「沈んでいく」演出（DeathSinkScript）→ 遷移表のきっかけ "demo_end"（Title へ dive）
///          と流れ、遊ばずに遷移だけを一周見せられる。HP やゲームオーバーの判定には触らない。
///          見せ終わったら kEnabled を false に戻すだけで元の挙動になる
///          （遷移表の "demo_end" の行は残っていても使われない）。
namespace TransitionDemo {
inline constexpr bool kEnabled = true;
inline constexpr float kDelayAfterStart = 3.0f; ///< 開始演出が終わってから沈み始めるまで（秒）
inline constexpr const char *kTrigger = "demo_end"; ///< 遷移表のきっかけ名
} // namespace TransitionDemo
