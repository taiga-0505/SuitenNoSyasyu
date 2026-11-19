#pragma once
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ECS/Entity.h"
#include "Scene.h"

/// @file EntityPool.h
/// @brief 弾やエフェクトなど、頻繁に出し入れするエンティティの Object Pool
/// @details 以前は自機弾（RailShooterController）・敵弾（EnemyAI）・砲弾（ShipEnemyScript）が
///          それぞれ「シーンから非アクティブな同名エンティティを探して再利用し、
///          無ければ新規作成する」処理を個別に書いていた。同じ処理が 3 か所に重複しており、
///          片方だけ直すと挙動がずれるため、プールの取り出し規則をここ 1 か所にまとめた。
///
///          返却は「エンティティを非アクティブにする」だけでよい（WaterBullet などが寿命切れで行う）。
///          プール自体はシーンのエンティティを所有しない。シーンが破棄されれば中身も消えるので、
///          シーンをまたいでダングリング参照が残ることはない。

/// @brief 名前で束ねたエンティティの Object Pool
class EntityPool {
public:
  /// @brief 取り出した結果
  struct Lease {
    std::shared_ptr<Entity> entity; ///< 取り出したエンティティ（失敗時は nullptr）
    bool isNew = false;             ///< 新規作成したなら true（ランタイム初期化が必要）
  };

  /// @brief プールするエンティティ名を指定して作る
  /// @param entityName プール対象のエンティティ名（同名の非アクティブなエンティティを再利用する）
  explicit EntityPool(std::string entityName) : entityName_(std::move(entityName)) {}

  /// @brief 再利用できるエンティティを 1 つ取り出す（無ければ新規作成する）
  /// @param scene 取り出し先のシーン
  /// @return 取り出したエンティティと、新規作成かどうか
  /// @details 再利用したものはアクティブに戻し、"reused" タグを立てる
  ///          （WaterBullet などがこのタグを見て OnCreate 相当の初期化をやり直す）。
  Lease Acquire(Scene& scene) const {
    if (auto reused = FindInactive(scene)) {
      reused->SetActive(true);
      reused->SetTag("reused", 1);
      return {reused, false};
    }
    return {scene.CreateEntity(entityName_), true};
  }

  /// @brief 先に n 個まで、いま使える（非アクティブな）エンティティを集める
  /// @param scene 探すシーン
  /// @param maxCount 集める最大数
  /// @return 非アクティブなエンティティの一覧（最大 maxCount 個）
  /// @details 散弾のように 1 フレームで複数発を出すとき、同じ弾を二重に取らないよう
  ///          先にまとめて確保するために使う。
  std::vector<std::shared_ptr<Entity>> CollectInactive(Scene& scene, std::size_t maxCount) const {
    std::vector<std::shared_ptr<Entity>> result;
    for (const auto& e : scene.GetEntities()) {
      if (result.size() >= maxCount) break;
      if (IsReusable(e)) result.push_back(e);
    }
    return result;
  }

  /// @brief エンティティをプールへ返す（非アクティブにする）
  /// @param entity 返すエンティティ
  static void Release(Entity& entity) { entity.SetActive(false); }

  /// @brief プール対象のエンティティ名
  /// @return エンティティ名
  const std::string& EntityName() const { return entityName_; }

private:
  /// @brief 再利用できるエンティティか（同名・非アクティブ・破棄予約なし）
  /// @param e 調べるエンティティ
  /// @return 再利用できるなら true
  bool IsReusable(const std::shared_ptr<Entity>& e) const {
    return e && !e->IsActive() && !e->IsPendingDestroy() && e->GetName() == entityName_;
  }

  /// @brief 非アクティブな同名エンティティを探す
  /// @param scene 探すシーン
  /// @return 見つかったエンティティ（無ければ nullptr）
  std::shared_ptr<Entity> FindInactive(Scene& scene) const {
    for (const auto& e : scene.GetEntities()) {
      if (IsReusable(e)) return e;
    }
    return nullptr;
  }

  std::string entityName_; ///< プール対象のエンティティ名
};
