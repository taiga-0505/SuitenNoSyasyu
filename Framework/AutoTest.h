#pragma once

// ============================================================================
// AutoTest — 起動引数 --autotest でシーンを自動で一周して終了する仕組み
// ----------------------------------------------------------------------------
// 決めた順路（例: Title → Stage1 → Result → Title）のシーンへ順に遷移し、
// 各シーンに着いて一定時間待ったらゲーム画面を PNG に保存する。
// 最後まで回ったらログに結果をまとめ、終了コード（0 = 成功 / 1 = 失敗）で終了する。
//
// 起動引数:
//   --autotest                 有効化
//   --autotest-route=A,B,C     順路（省略時は SetDefaultRoute の値、それも無ければ起動シーンだけ）
//   --autotest-wait=<秒>       到着から撮影までの待ち時間（既定 2.0）
//   --autotest-out=<フォルダ>  保存先（既定 ../media/autotest/<日時>）
//
// エンジンはゲームのシーン名を知らないので、既定の順路は Application 側が
// GameSetup.cpp などの静的初期化で SetDefaultRoute に登録する。
// ============================================================================

#include <chrono>
#include <string>
#include <vector>

class Game;

/// @class AutoTest
/// @brief シーンを順に巡ってスクリーンショットを撮り、終了するまでを進める
class AutoTest {
public:
  /// @brief 既定の順路を登録する（Application 側が静的初期化で呼ぶ）
  /// @param route 巡るシーン名の並び
  static void SetDefaultRoute(std::vector<std::string> route);

  /// @brief 起動引数を読んで、有効なら設定を取り込む
  /// @param bootScene 起動シーン名（順路が無いときの代わり）
  /// @return --autotest が指定されていれば true
  bool ParseCommandLine(const std::string &bootScene);

  /// @brief 有効か
  /// @return --autotest が指定されていれば true
  bool IsEnabled() const { return enabled_; }

  /// @brief 1 フレーム分進める（App::Update の後、描画の前に呼ぶ）
  /// @param game 遷移の要求先
  /// @details 撮影が必要なフレームでは TakeCaptureRequest が保存先を返す。
  ///          全部回り終えたら PostQuitMessage で終了コードを送る。
  void Update(Game &game);

  /// @brief このフレームで撮影すべき保存先を受け取る（1 回取ったら空に戻る）
  /// @return 保存先のパス。撮影しないフレームは空
  std::string TakeCaptureRequest();

private:
  /// @brief 進行段階
  enum class Phase {
    Arriving,  ///< 遷移先に着く（遷移演出が終わる）のを待つ
    Settling,  ///< 着いてから撮影まで待つ
    Capturing, ///< 撮影を要求した（次のフレームで保存結果を確かめる）
    Finished,  ///< 終了を送った
  };

  /// @brief 次の段へ進み、遷移を要求する（最後なら終了する）
  /// @param game 遷移の要求先
  void advance_(Game &game);

  /// @brief 現在の段を失敗として記録し、テストを打ち切る
  /// @param reason ログに出す理由
  void fail_(const std::string &reason);

  /// @brief 結果をログにまとめ、終了コードを送る
  void finish_();

  using Clock = std::chrono::steady_clock;

  bool enabled_ = false;          ///< --autotest が指定されたか
  std::vector<std::string> route_; ///< 巡るシーン名
  float waitSeconds_ = 2.0f;      ///< 到着から撮影までの待ち（秒）
  float stepTimeoutSeconds_ = 30.0f; ///< 1 段で到着を待つ上限（秒）
  std::string outDir_;            ///< スクリーンショットの保存先フォルダ

  Phase phase_ = Phase::Arriving; ///< 現在の段階
  size_t step_ = 0;               ///< route_ の何番目に向かっているか
  bool changeRequested_ = false;  ///< 現在の段の遷移要求が受理されたか
  Clock::time_point stepStart_;   ///< 現在の段を始めた時刻
  Clock::time_point arrivedAt_;   ///< 遷移先に着いた時刻
  std::string pendingCapture_;    ///< このフレームで撮る保存先
  std::string lastCapture_;       ///< 直前に撮影を要求した保存先
  std::vector<std::string> results_; ///< 段ごとの結果（ログ用）
  bool failed_ = false;           ///< 失敗があったか
};
