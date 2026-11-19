#pragma once
#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

/// @file GameEvents.h
/// @brief ゲーム内イベントの通知（Observer パターン）
/// @details 以前は敵が撃破されたとき、EnemyBaseScript が GameSession（結果の集計）を
///          直接呼んでいた。敵が「誰が撃破数を数えているか」を知っている必要があり、
///          集計先を増やす（実績・SE・演出など）たびに敵側を書き換えることになる。
///          そこで敵は「撃破された」というイベントを発行するだけにし、
///          関心のある側（RailShooterGameMode など）が購読する形に分けた。

/// @brief 購読を解除するためのハンドル（RAII）
/// @details 破棄されると自動で購読を解除する。購読者がイベントより先に消えても
///          ダングリング参照にならないよう、購読者側のメンバとして持たせる。
class EventConnection {
public:
  EventConnection() = default;

  /// @brief 解除処理を受け取って接続を作る
  /// @param disconnect 解除時に呼ぶ関数
  explicit EventConnection(std::function<void()> disconnect)
      : disconnect_(std::move(disconnect)) {}

  ~EventConnection() { Disconnect(); }

  EventConnection(const EventConnection&) = delete;
  EventConnection& operator=(const EventConnection&) = delete;

  /// @brief ムーブで所有権を移す（移動元は空になる）
  /// @param other 移動元
  EventConnection(EventConnection&& other) noexcept
      : disconnect_(std::exchange(other.disconnect_, nullptr)) {}

  /// @brief ムーブ代入。既に持っている購読は先に解除する
  /// @param other 移動元
  /// @return *this
  EventConnection& operator=(EventConnection&& other) noexcept {
    if (this != &other) {
      Disconnect();
      disconnect_ = std::exchange(other.disconnect_, nullptr);
    }
    return *this;
  }

  /// @brief 購読を解除する（何度呼んでもよい）
  void Disconnect() {
    if (disconnect_) {
      disconnect_();
      disconnect_ = nullptr;
    }
  }

  /// @brief 購読中か
  /// @return 解除されていなければ true
  bool IsConnected() const { return static_cast<bool>(disconnect_); }

private:
  std::function<void()> disconnect_; ///< 解除処理（空なら未接続）
};

/// @brief 1 種類のイベントを発行する Subject
/// @tparam Args 通知に載せる引数の型
template <class... Args>
class GameEvent {
public:
  using Handler = std::function<void(Args...)>;

  GameEvent() : listeners_(std::make_shared<ListenerList>()) {}

  GameEvent(const GameEvent&) = delete;
  GameEvent& operator=(const GameEvent&) = delete;

  /// @brief イベントを購読する
  /// @param handler 通知時に呼ばれる関数
  /// @return 購読ハンドル（破棄すると購読解除）
  [[nodiscard]] EventConnection Subscribe(Handler handler) {
    const std::uint32_t id = nextId_++;
    listeners_->push_back({id, std::move(handler)});
    // イベント本体が先に破棄されても安全なよう、リストは weak_ptr で参照する
    std::weak_ptr<ListenerList> weak = listeners_;
    return EventConnection([weak, id]() {
      if (auto list = weak.lock()) {
        list->erase(std::remove_if(list->begin(), list->end(),
                                   [id](const Listener& l) { return l.id == id; }),
                    list->end());
      }
    });
  }

  /// @brief 購読者全員に通知する
  /// @param args 通知に載せる値
  void Emit(Args... args) const {
    // 通知中に購読・解除されても壊れないよう、コピーに対して回す
    const ListenerList snapshot = *listeners_;
    for (const auto& l : snapshot) {
      if (l.handler) {
        l.handler(args...);
      }
    }
  }

  /// @brief 購読者の数
  /// @return 現在の購読者数
  std::size_t ListenerCount() const { return listeners_->size(); }

private:
  /// @brief 購読者 1 件
  struct Listener {
    std::uint32_t id; ///< 解除用の識別子
    Handler handler;  ///< 通知先
  };
  using ListenerList = std::vector<Listener>;

  std::shared_ptr<ListenerList> listeners_; ///< 購読者の一覧
  std::uint32_t nextId_ = 1;                ///< 次に払い出す識別子
};

/// @brief 敵が撃破されたときの情報
struct EnemyDefeatedInfo {
  std::string enemyName; ///< 撃破された敵のエンティティ名
  int maxHp = 0;         ///< 撃破された敵の最大体力
};

/// @brief ゲーム全体で共有するイベントの置き場
/// @details イベントの種類ごとに Subject を 1 つずつ持つ。
///          発行側（敵など）と購読側（GameMode など）は互いを知らずに済む。
namespace GameEvents {

/// @brief 敵が撃破されたときに発行されるイベント
/// @return イベントの Subject
inline GameEvent<const EnemyDefeatedInfo&>& EnemyDefeated() {
  static GameEvent<const EnemyDefeatedInfo&> event;
  return event;
}

} // namespace GameEvents
