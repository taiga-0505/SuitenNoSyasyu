#pragma once

/// @namespace ShoreVisit
/// @brief ステージセレクトから浜辺シーン（Shore：設定・操作説明）へ行って戻ってきたことを覚えておく受け渡し口
/// @details StageSelectScript が浜辺へ遷移するときに MarkLeaving() し、戻ってきた Select の
///          StageSelectScript が OnCreate で ConsumeReturning() する。true ならカメラを砂浜の上に置いた
///          状態で始め、浜辺を選んだまま波打ち際の上へ滑って戻る（Shore シーンのカメラと同じ位置なので継ぎ目なくつながる）。
namespace ShoreVisit {

/// @brief フラグ本体
/// @return 浜辺へ出かけていれば true になっている参照
inline bool &Flag() {
  static bool visiting = false;
  return visiting;
}

/// @brief 浜辺へ出かけることを記録する
inline void MarkLeaving() { Flag() = true; }

/// @brief 浜辺から戻ってきたかを調べ、記録を消す
/// @return 浜辺から戻ってきたなら true
inline bool ConsumeReturning() {
  if (!Flag()) return false;
  Flag() = false;
  return true;
}

} // namespace ShoreVisit
