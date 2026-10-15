#pragma once
#include "Common/SceneContext.h"

#include <functional>
#include <string>
#include <vector>

// ============================================================================
// SceneFlow — シーン遷移表
// ----------------------------------------------------------------------------
// 「どのシーンで・何が起きたら・どこへ・どの演出で」遷移するかを 1 か所に集めた表。
// 保存先は Resources/SceneFlow.json。エディタの Window > Scene Flow で編集できる。
//
// スクリプトやゲームルールはシーン名を直接書かず、「きっかけ（trigger）」の名前だけで遷移する。
//
//   RequestTransition("start");          // Title の「スタート」
//   RequestTransition("stage", "Stage3"); // 行き先をスクリプトが決める（遷移先が $arg の行）
//
// 行の書き方
//   from       : 遷移元。シーン名 / "Stage*"（前方一致）/ "*"（全シーン）
//                複数の行が当てはまるときは「完全一致 → 長い前方一致 → *」の順で優先する
//   trigger    : きっかけの名前（スクリプト・GameMode が渡す文字列）
//   to         : 遷移先。シーン名、または $ で始まる変数
//                  $current … いまのシーン（やり直し）
//                  $arg     … スクリプトが RequestTransition の第 2 引数で渡した名前
//                  その他   … Application が RegisterVariable で登録したもの（例: $nextStage）
//   transition : 演出名（"dissolve" / "dive" / "ascend"）
//   note       : メモ（エディタに表示するだけ）
// ============================================================================

/// @brief 遷移表の 1 行（遷移元・きっかけ・遷移先・演出の組）を保持する
struct SceneFlowRule {
  std::string from;                       ///< 遷移元（シーン名 / "Prefix*" / "*"）
  std::string trigger;                    ///< きっかけ
  std::string to;                         ///< 遷移先（シーン名 / $変数）
  std::string transition = "dissolve";    ///< 演出名
  std::string note;                       ///< メモ
};

/// @brief 遷移表を引いた結果（変数解決後の遷移先と演出）を保持する
struct SceneFlowResult {
  std::string target;                ///< 変数を解決した遷移先シーン名（空なら「行き先なし」）
  std::string transition;            ///< 演出名
  const SceneFlowRule *rule = nullptr; ///< 当てはまった行
};

/// @class SceneFlow
/// @brief シーン遷移表を読み書きし、「遷移元＋きっかけ」から遷移先と演出を引く（シングルトン）
class SceneFlow {
public:
  /// @brief 既定の保存先（作業ディレクトリ = project/ 基準）
  static constexpr const char *kDefaultPath = "Resources/SceneFlow.json";

  /// @brief 組み込み変数: いまのシーン
  static constexpr const char *kVarCurrent = "$current";
  /// @brief 組み込み変数: スクリプトが RequestTransition の第 2 引数で渡した名前
  static constexpr const char *kVarArg = "$arg";

  /// @brief 唯一のインスタンスを取得する
  /// @return SceneFlow のインスタンス（関数内 static）
  static SceneFlow &Get();

  // ------------------------------------------------------------------
  // 読み書き
  // ------------------------------------------------------------------

  /// @brief JSON から読み込む（ファイルが無ければ空の表で始める）
  /// @param path 読み込む JSON のパス。以後の Save の保存先にもなる
  /// @return 読み込めたら true。ファイルが無い・開けない・JSON が壊れているなら false（表は空になる）
  bool Load(const std::string &path = kDefaultPath);

  /// @brief 読み込んだ場所へ保存する（親ディレクトリが無ければ作る）
  /// @return 保存できたら true
  bool Save();

  /// @brief 保存先パスを取得する
  /// @return 保存先パス
  const std::string &Path() const { return path_; }

  /// @brief 保存していない変更があるか
  /// @return 未保存の変更があれば true
  bool IsDirty() const { return dirty_; }

  /// @brief 変更ありにする（エディタが行を書き換えたときに呼ぶ）
  void MarkDirty() { dirty_ = true; }

  /// @brief 行の一覧を取得する（エディタから直接編集してよい。編集したら MarkDirty）
  /// @return 遷移表の行の配列
  std::vector<SceneFlowRule> &Rules() { return rules_; }
  const std::vector<SceneFlowRule> &Rules() const { return rules_; }

  // ------------------------------------------------------------------
  // 解決
  // ------------------------------------------------------------------

  /// @brief from のシーンで trigger に当てはまる行を探す
  /// @param fromScene 遷移元のシーン名
  /// @param trigger きっかけの名前
  /// @return 最も優先度の高い行（同じ優先度なら上の行）。無ければ nullptr
  const SceneFlowRule *FindRule(const std::string &fromScene, const std::string &trigger) const;

  /// @brief 遷移先の変数を解決する（変数でなければそのまま返す）
  /// @param to 行の遷移先
  /// @param fromScene いまのシーン名（$current 用）
  /// @param arg スクリプトが渡した名前（$arg 用）
  /// @return 解決したシーン名。解決できなければ空
  std::string ResolveTarget(const std::string &to, const std::string &fromScene,
                            const std::string &arg) const;

  /// @brief from のシーンで trigger を引き、遷移先と演出を返す
  /// @param fromScene 遷移元のシーン名
  /// @param trigger きっかけの名前
  /// @param arg スクリプトが渡した名前（$arg の解決に使う）
  /// @param out 結果の受け取り先（最初に初期化される）
  /// @return 行が見つかれば true（遷移先が空のこともある）。見つからなければ未登録として記録する
  bool Resolve(const std::string &fromScene, const std::string &trigger, const std::string &arg,
               SceneFlowResult &out);

  /// @brief エディタ表示用に「trigger → 行き先 (演出)」の文字列を作る
  /// @param fromScene 遷移元のシーン名
  /// @param trigger きっかけの名前
  /// @return 表示用の文字列
  /// @details 未登録なら「(未登録)」。Resolve と違って未登録の記録はしない（毎フレーム呼んでよい）
  std::string Describe(const std::string &fromScene, const std::string &trigger) const;

  /// @brief from の書き方が scene に当てはまるか
  /// @param pattern 行の from（シーン名 / "Prefix*" / "*"）
  /// @param scene 判定するシーン名
  /// @param score 当てはまったときの優先度（大きいほど優先）。不要なら nullptr
  /// @return 当てはまれば true
  static bool MatchFrom(const std::string &pattern, const std::string &scene, int *score = nullptr);

  /// @brief 使える演出名の一覧を取得する
  /// @return 演出名の配列（dissolve / dive / ascend）
  static const std::vector<std::string> &TransitionNames();

  // ------------------------------------------------------------------
  // 変数（Application 側が登録する）
  // ------------------------------------------------------------------

  /// @brief 遷移先の変数を登録する
  /// @param name "$" で始まる名前（例: "$nextStage"）
  /// @param description エディタに出す説明
  /// @param fn 解決関数。空文字を返すと「行き先なし」
  /// @details 静的初期化から呼んでよい。Load しても消えない。同名で登録し直すと上書きする。
  void RegisterVariable(const std::string &name, const std::string &description,
                        std::function<std::string()> fn);

  /// @brief エディタ表示用に変数の名前・説明・組み込みかどうかを保持する
  struct VariableInfo {
    std::string name;        ///< 変数名（"$" 始まり）
    std::string description; ///< エディタに出す説明
    bool builtin = false;    ///< 組み込み変数（$current / $arg）なら true
  };

  /// @brief 使える変数の一覧を取得する
  /// @return 変数情報の配列（組み込み → 登録順）
  std::vector<VariableInfo> Variables() const;

  /// @brief その名前の変数があるか
  /// @param name 調べる変数名
  /// @return 組み込みまたは登録済みなら true
  bool HasVariable(const std::string &name) const;

  // ------------------------------------------------------------------
  // 未登録のきっかけ（エディタに「追加」ボタン付きで出す）
  // ------------------------------------------------------------------

  /// @brief Resolve で行が見つからなかった「遷移元＋きっかけ」と発生回数を保持する
  struct MissingTrigger {
    std::string from;    ///< 遷移元のシーン名
    std::string trigger; ///< きっかけの名前
    int count = 0;       ///< Resolve で見つからなかった回数
  };

  /// @brief 未登録のきっかけ一覧を取得する
  /// @return 未登録のきっかけの配列（発生順）
  const std::vector<MissingTrigger> &Missing() const { return missing_; }

  /// @brief 未登録のきっかけを一覧から取り除く（エディタで行を追加したときなどに使う）
  /// @param from 遷移元のシーン名
  /// @param trigger きっかけの名前
  void RemoveMissing(const std::string &from, const std::string &trigger);

  // ------------------------------------------------------------------
  // エディタ向けの結線（SceneManager::Init が差す）
  // ------------------------------------------------------------------

  /// @brief 登録済みシーン名の取得関数を差す
  /// @param fn 登録済みシーン名を返す関数（nullptr で結線を外す）
  void SetSceneNameProvider(std::function<std::vector<std::string>()> fn) { sceneNames_ = std::move(fn); }

  /// @brief 登録済みシーン名を取得する
  /// @return ソート済みのシーン名一覧（結線前は空）
  std::vector<std::string> SceneNames() const;

  /// @brief シーンが登録済みか
  /// @param name 調べるシーン名
  /// @return 登録済みなら true。結線前は判定できないので常に true
  bool IsKnownScene(const std::string &name) const;

  /// @brief いまのシーン名の取得関数を差す
  /// @param fn いまのシーン名を返す関数（nullptr で結線を外す）
  void SetCurrentSceneProvider(std::function<std::string()> fn) { currentScene_ = std::move(fn); }

  /// @brief いまのシーン名を取得する
  /// @return いまのシーン名（結線前は空）
  std::string CurrentScene() const { return currentScene_ ? currentScene_() : std::string(); }

  /// @brief 遷移要求の関数を差す（エディタの「試す」ボタンが使う）
  /// @param fn 遷移要求を受け付ける関数（nullptr で結線を外す）
  void SetRequestFn(std::function<bool(const std::string &, SceneTransition)> fn) { request_ = std::move(fn); }

  /// @brief 遷移を要求する（エディタ用。スクリプトは ScriptableEntity::RequestTransition を使う）
  /// @param target 遷移先のシーン名
  /// @param transition 演出名（ParseSceneTransition で型に変換する）
  /// @return 要求が受理されたら true。未結線または target が空なら false
  bool Request(const std::string &target, const std::string &transition);

private:
  SceneFlow() = default;

  /// @brief Application が登録した遷移先変数（名前・説明・解決関数）を保持する
  struct Variable {
    std::string name;                ///< 変数名（"$" 始まり）
    std::string description;         ///< エディタに出す説明
    std::function<std::string()> fn; ///< 遷移先シーン名を返す解決関数
  };

  std::string path_ = kDefaultPath;     ///< 読み込み元・保存先のパス
  bool dirty_ = false;                  ///< 未保存の変更があるか
  std::vector<SceneFlowRule> rules_;    ///< 遷移表の行
  std::vector<Variable> variables_;     ///< 登録済みの変数（組み込み変数は含まない）
  std::vector<MissingTrigger> missing_; ///< 未登録のきっかけ

  std::function<std::vector<std::string>()> sceneNames_;              ///< 登録済みシーン名の取得関数
  std::function<std::string()> currentScene_;                         ///< いまのシーン名の取得関数
  std::function<bool(const std::string &, SceneTransition)> request_; ///< 遷移要求の関数
};
