#pragma once
#include <nlohmann/json_fwd.hpp>

/// @class IComponent
/// @brief すべてのコンポーネントの基底インターフェース
/// @details Entity に付けるコンポーネントは必ずこのクラスを継承する。
class IComponent {
public:
  virtual ~IComponent() = default;

  /// @brief 毎フレームの更新
  /// @param deltaTime 前フレームからの経過時間（秒）
  virtual void Update(float deltaTime) { (void)deltaTime; }

  /// @brief 所有する Entity がコンポーネントの表を破棄する直前に呼ばれる
  /// @details エンティティが破棄されるとき、components_ の破棄が始まる前に全コンポーネントへ
  ///          通知される。他のコンポーネントを参照する後始末（スクリプトの OnDestroy など）は
  ///          デストラクタではなくここで行うこと。デストラクタから GetComponent() を呼ぶと、
  ///          破棄途中の unordered_map を検索することになり未定義動作になる。
  /// @note 同じインスタンスに対して複数回呼ばれても壊れないように実装すること。
  virtual void OnEntityDestroy() {}

  /// @brief コンポーネントの型名を返す（シリアライズ用）
  /// @return 型名の文字列（例: "TransformComponent"）
  virtual const char* TypeName() const = 0;

  /// @brief コンポーネントの状態を JSON に書き出す
  /// @return シリアライズした JSON オブジェクト
  virtual nlohmann::json Serialize() const = 0;

  /// @brief JSON からコンポーネントの状態を読み込む
  /// @param j 読み込み元の JSON オブジェクト
  virtual void Deserialize(const nlohmann::json& j) = 0;

  /// @brief 有効かどうかを返す
  /// @return 有効なら true
  bool IsEnabled() const { return enabled_; }

  /// @brief 有効・無効を切り替える
  /// @param enabled 設定する状態
  void SetEnabled(bool enabled) { enabled_ = enabled; }

  /// @brief このコンポーネントが付いている Entity を取得する
  /// @return 親 Entity へのポインタ
  class Entity* GetEntity() const { return entity_; }

protected:
  bool enabled_ = true; ///< 有効フラグ（false なら Update を飛ばす）
  class Entity* entity_ = nullptr; ///< 親 Entity へのポインタ
  friend class Entity;
};
