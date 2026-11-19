#pragma once
#include <memory>
#include <type_traits>
#include <utility>

/// @file StateMachine.h
/// @brief アプリ層で使う State パターンの共通基盤
/// @details 敵 AI・ウェーブ進行・タイトル演出などは、もともと enum + switch で状態を
///          切り替えていた。状態が増えるたびに switch が肥大化し、状態ごとの変数や
///          遷移条件が 1 つの関数に混ざって読みにくくなっていたため、
///          「状態 1 つ = クラス 1 つ」に分けられるようにこの基盤を用意した。
///
///          使い方:
///            1. 状態を持つ本体（Owner）ごとに IState<Owner> を継承した状態クラスを作る
///            2. 本体は StateMachine<Owner> をメンバに持ち、毎フレーム Update(*this, dt) を呼ぶ
///            3. 次の状態は「状態クラス自身」が machine.ChangeState<次の状態>() で決める
///
///          ChangeState は予約だけを行い、実際の切り替え（OnExit → OnEnter）は
///          現在の状態の OnUpdate を抜けたあとに行う。状態が自分自身を
///          OnUpdate の途中で破棄してしまう事故を防ぐため。

template <class Owner> class StateMachine;

/// @brief 状態クラスの基底インタフェース
/// @tparam Owner 状態を持つ本体の型（例: SharkEnemyScript）
template <class Owner>
class IState {
public:
  virtual ~IState() = default;

  /// @brief この状態に入った瞬間に 1 回だけ呼ばれる
  /// @param owner 状態を持つ本体
  /// @param machine この状態を管理しているステートマシン（遷移の予約に使う）
  virtual void OnEnter(Owner& owner, StateMachine<Owner>& machine) {
    (void)owner;
    (void)machine;
  }

  /// @brief この状態での毎フレームの処理。遷移条件を満たしたら machine.ChangeState で次を予約する
  /// @param owner 状態を持つ本体
  /// @param machine この状態を管理しているステートマシン
  /// @param deltaTime 前フレームからの経過秒数
  virtual void OnUpdate(Owner& owner, StateMachine<Owner>& machine, float deltaTime) = 0;

  /// @brief この状態から抜ける瞬間に 1 回だけ呼ばれる
  /// @param owner 状態を持つ本体
  virtual void OnExit(Owner& owner) { (void)owner; }

  /// @brief 状態名（インスペクタやログの表示用）
  /// @return 状態名の文字列リテラル
  virtual const char* Name() const = 0;
};

/// @brief 状態クラスを 1 つだけ保持し、遷移を管理するステートマシン
/// @tparam Owner 状態を持つ本体の型
template <class Owner>
class StateMachine {
public:
  using State = IState<Owner>;

  /// @brief 次の状態を予約する（実際の切り替えは Update の最後に行う）
  /// @tparam S 次の状態クラス（IState<Owner> の派生）
  /// @tparam Args S のコンストラクタ引数
  /// @param args S のコンストラクタへ転送する引数
  template <class S, class... Args>
  void ChangeState(Args&&... args) {
    static_assert(std::is_base_of_v<State, S>, "S は IState<Owner> を継承していること");
    pending_ = std::make_unique<S>(std::forward<Args>(args)...);
  }

  /// @brief 予約済みの遷移があれば今すぐ切り替える
  /// @param owner 状態を持つ本体
  /// @details 初期状態を OnCreate で入れた直後など、Update を待たずに
  ///          OnEnter を走らせたいときに呼ぶ。
  void ApplyPending(Owner& owner) {
    // OnEnter の中でさらに ChangeState されても追従できるよう、空になるまで繰り返す
    while (pending_) {
      if (current_) {
        current_->OnExit(owner);
      }
      current_ = std::move(pending_);
      current_->OnEnter(owner, *this);
    }
  }

  /// @brief 現在の状態を 1 フレーム進め、予約された遷移があれば切り替える
  /// @param owner 状態を持つ本体
  /// @param deltaTime 前フレームからの経過秒数
  void Update(Owner& owner, float deltaTime) {
    ApplyPending(owner);
    if (current_) {
      current_->OnUpdate(owner, *this, deltaTime);
    }
    ApplyPending(owner);
  }

  /// @brief 現在の状態が S かどうか
  /// @tparam S 調べる状態クラス
  /// @return 現在の状態が S（またはその派生）なら true
  template <class S>
  bool IsIn() const {
    return dynamic_cast<const S*>(current_.get()) != nullptr;
  }

  /// @brief 現在の状態を S として取得する
  /// @tparam S 取得したい状態クラス
  /// @return 現在の状態が S なら そのポインタ、違えば nullptr
  template <class S>
  S* As() const {
    return dynamic_cast<S*>(current_.get());
  }

  /// @brief 現在の状態名
  /// @return 状態名（状態が無ければ "None"）
  const char* CurrentName() const { return current_ ? current_->Name() : "None"; }

  /// @brief 状態を 1 つでも持っているか
  /// @return 現在の状態があれば true
  bool HasState() const { return current_ != nullptr; }

private:
  std::unique_ptr<State> current_; ///< 現在の状態
  std::unique_ptr<State> pending_; ///< 次に切り替える状態（予約）
};
