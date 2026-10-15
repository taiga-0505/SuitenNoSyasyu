#pragma once
#include "IComponent.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <random>
#include <nlohmann/json.hpp>
#include <any>

/// @class Entity
/// @brief 軽量なエンティティクラス。コンポーネントを保持するだけの最小限の入れ物
/// @details テンプレートの AddComponent / GetComponent で型安全にコンポーネントを管理する。
class Entity {
public:
  /// @brief 一意な GUID を生成する
  /// @return 1〜UINT64_MAX の乱数（0 は「未設定」の意味で使うため出さない）
  static uint64_t GenerateGUID() {
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    static std::uniform_int_distribution<uint64_t> dis(1, UINT64_MAX);
    return dis(gen);
  }

  /// @brief コンストラクタ。内部 ID と GUID は自動で割り当てる
  Entity() : id_(NextId()), guid_(GenerateGUID()) {}

  /// @brief 名前を指定するコンストラクタ
  /// @param name エンティティ名
  explicit Entity(const std::string &name) : id_(NextId()), name_(name), guid_(GenerateGUID()) {}

  /// @brief デストラクタ
  /// @details components_ の破棄が始まる前に全コンポーネントへ OnEntityDestroy() を通知する。
  ///          スクリプトの OnDestroy() は GetComponent() で他のコンポーネントを触るため、
  ///          unordered_map の破棄中に呼ぶとノードが解放済みのマップを検索してしまい
  ///          アクセス違反で落ちる。通知 → clear() の順にすることで、
  ///          後始末の間はすべてのコンポーネントが有効であることを保証する。
  ~Entity() {
    for (auto &[type, comp] : components_) {
      if (comp) comp->OnEntityDestroy();
    }
    components_.clear();
  }

  /// @brief コンポーネントを追加する
  /// @tparam T    コンポーネントの型（IComponent の派生であること）
  /// @tparam Args コンストラクタ引数の型
  /// @param args T のコンストラクタに渡す引数
  /// @return 追加したコンポーネントへの参照（同じ型が既にあれば置き換える）
  template <typename T, typename... Args> T &AddComponent(Args &&...args) {
    static_assert(std::is_base_of_v<IComponent, T>,
                  "T must derive from IComponent");
    auto ptr = std::make_unique<T>(std::forward<Args>(args)...);
    ptr->entity_ = this;
    T &ref = *ptr;
    components_[std::type_index(typeid(T))] = std::move(ptr);
    return ref;
  }

  /// @brief コンポーネントを取得する
  /// @tparam T 取得するコンポーネントの型
  /// @return コンポーネントへのポインタ（無ければ nullptr）
  template <typename T> T *GetComponent() {
    auto it = components_.find(std::type_index(typeid(T)));
    if (it != components_.end()) {
      return static_cast<T *>(it->second.get());
    }
    return nullptr;
  }

  /// @brief コンポーネントを取得する（const 版）
  /// @tparam T 取得するコンポーネントの型
  /// @return コンポーネントへのポインタ（無ければ nullptr）
  template <typename T> const T *GetComponent() const {
    auto it = components_.find(std::type_index(typeid(T)));
    if (it != components_.end()) {
      return static_cast<const T *>(it->second.get());
    }
    return nullptr;
  }

  /// @brief 指定した型のコンポーネントを持っているかを返す
  /// @tparam T 調べるコンポーネントの型
  /// @return 持っていれば true
  template <typename T> bool HasComponent() const {
    return components_.count(std::type_index(typeid(T))) > 0;
  }

  /// @brief 指定した型のコンポーネントを取り除く
  /// @tparam T 取り除くコンポーネントの型
  /// @note OnEntityDestroy() は呼ばずにその場で破棄する。
  template <typename T> void RemoveComponent() {
    components_.erase(std::type_index(typeid(T)));
  }

  /// @brief 有効なコンポーネントをすべて更新する
  /// @param deltaTime 前フレームからの経過時間（秒）
  void UpdateAll(float deltaTime) {
    for (auto &[type, comp] : components_) {
      if (comp && comp->IsEnabled()) {
        comp->Update(deltaTime);
      }
    }
  }

  /// @brief アクティブかどうかを返す
  /// @return アクティブなら true
  bool IsActive() const { return active_; }

  /// @brief アクティブ状態を設定する
  /// @param active 設定する状態
  void SetActive(bool active) { active_ = active; }

  /// @brief 遅延破棄の印を付ける
  /// @details その場では削除しない。Scene::RemoveDeadEntities() でまとめて回収される。
  void Destroy() { pendingDestroy_ = true; }

  /// @brief 破棄待ちかどうかを返す
  /// @return Destroy() 済みなら true
  bool IsPendingDestroy() const { return pendingDestroy_; }

  /// @brief 一意な ID を取得する
  /// @return エンティティ ID（実行中のみ有効。保存しない）
  uint32_t Id() const { return id_; }

  /// @brief エンティティ名を取得する
  /// @return 名前の文字列
  const std::string &Name() const { return name_; }

  /// @brief エンティティ名を設定する
  /// @param name 設定する名前
  void SetName(const std::string &name) { name_ = name; }

  /// @brief 表示するかどうかを返す
  /// @return 表示するなら true
  bool IsVisible() const { return visible_; }
  /// @brief 表示・非表示を設定する
  /// @param v 表示するなら true
  void SetVisible(bool v) { visible_ = v; }

  /// @brief ロック状態を返す
  /// @return ロック中なら true
  bool IsLocked() const { return locked_; }
  /// @brief ロック状態を設定する
  /// @param l ロックするなら true
  void SetLocked(bool l) { locked_ = l; }

  /// @brief GUID を取得する
  /// @return GUID
  uint64_t Guid() const { return guid_; }
  /// @brief GUID を設定する（主にデシリアライズ時に使う）
  /// @param guid 設定する GUID
  void SetGuid(uint64_t guid) { guid_ = guid; }

  /// @brief 親の GUID を取得する
  /// @return 親の GUID（0 なら親なし）
  uint64_t ParentGuid() const { return parentGuid_; }
  /// @brief 親の GUID を設定する
  /// @param parentGuid 親の GUID（0 で親なし）
  void SetParentGuid(uint64_t parentGuid) { parentGuid_ = parentGuid; }

  /// @brief UI 上のフォルダとして扱うエンティティかを返す
  /// @return フォルダなら true
  bool IsFolder() const { return isFolder_; }
  /// @brief フォルダかどうかを設定する
  /// @param isFolder フォルダにするなら true
  void SetIsFolder(bool isFolder) { isFolder_ = isFolder; }

  /// @brief エンティティ名を取得する（Name() の別名）
  /// @return 名前の文字列
  const std::string& GetName() const { return name_; }

  /// @brief エンティティ ID を取得する（Id() の別名）
  /// @return エンティティ ID
  uint32_t GetId() const { return id_; }

  // ================================================================
  // 軽量タグ（スクリプト間のやり取り用）
  // ================================================================

  /// @brief 整数タグを設定する
  /// @param key   タグ名
  /// @param value 設定する値
  void SetTag(const std::string& key, int value) { intTags_[key] = value; }

  /// @brief 整数タグを取得する
  /// @param key          タグ名
  /// @param defaultValue タグが無いときに返す値
  /// @return タグの値（無ければ defaultValue）
  int GetTagInt(const std::string& key, int defaultValue = 0) const {
      auto it = intTags_.find(key);
      return (it != intTags_.end()) ? it->second : defaultValue;
  }

  /// @brief タグを消す
  /// @param key タグ名
  void ClearTag(const std::string& key) { intTags_.erase(key); }

  /// @brief タグがあるかを返す
  /// @param key タグ名
  /// @return あれば true
  bool HasTag(const std::string& key) const { return intTags_.count(key) > 0; }

  /// @brief すべてのタグを取得する
  /// @return タグ名 → 値の表
  const std::unordered_map<std::string, int>& GetTags() const { return intTags_; }
  /// @brief すべてのタグを書き換え可能な参照で取得する
  /// @return タグ名 → 値の表
  std::unordered_map<std::string, int>& GetTagsRef() { return intTags_; }

  /// @brief エンティティの状態を JSON に書き出す
  /// @return 名前・各フラグ・GUID・コンポーネント（型名・データ・有効状態）・タグを格納した JSON
  nlohmann::json Serialize() const {
    nlohmann::json j;
    j["name"] = name_;
    j["active"] = active_;
    j["visible"] = visible_;
    j["locked"] = locked_;
    j["guid"] = guid_;
    j["parentGuid"] = parentGuid_;
    j["isFolder"] = isFolder_;
    nlohmann::json comps = nlohmann::json::array();
    for (auto& [type, comp] : components_) {
      if (comp) {
        nlohmann::json c;
        c["type"] = comp->TypeName();
        c["data"] = comp->Serialize();
        c["enabled"] = comp->IsEnabled();
        comps.push_back(c);
      }
    }
    j["components"] = comps;
    j["tags"] = intTags_;
    return j;
  }

  /// @brief JSON からエンティティの状態を読み込む
  /// @param j 読み込み元の JSON オブジェクト
  /// @note コンポーネントは ComponentFactory で型名から生成する。未登録の型名は読み飛ばす。
  void Deserialize(const nlohmann::json& j) {
    if (j.contains("name")) name_ = j["name"].get<std::string>();
    if (j.contains("active")) active_ = j["active"].get<bool>();
    if (j.contains("visible")) visible_ = j["visible"].get<bool>();
    if (j.contains("locked")) locked_ = j["locked"].get<bool>();
    if (j.contains("guid")) guid_ = j["guid"].get<uint64_t>();
    if (j.contains("parentGuid")) parentGuid_ = j["parentGuid"].get<uint64_t>();
    if (j.contains("isFolder")) isFolder_ = j["isFolder"].get<bool>();
    if (j.contains("components")) {
      for (auto& cj : j["components"]) {
        std::string typeName = cj["type"].get<std::string>();
        auto comp = ComponentFactory::Create(typeName);
        if (comp) {
          comp->entity_ = this;
          comp->Deserialize(cj["data"]);
          if (cj.contains("enabled")) comp->SetEnabled(cj["enabled"].get<bool>());
          components_[std::type_index(typeid(*comp))] = std::move(comp);
        }
      }
    }
    if (j.contains("tags")) {
      intTags_ = j["tags"].get<std::unordered_map<std::string, int>>();
    }
  }

  // =================================================================
  // コンポーネントファクトリ（型名の文字列 → 生成関数の表）
  // =================================================================
  /// @class ComponentFactory
  /// @brief 型名の文字列からコンポーネントを生成するためのファクトリ
  class ComponentFactory {
  public:
    using Creator = std::function<std::unique_ptr<IComponent>()>; ///< コンポーネントの生成関数

    /// @brief コンポーネントの生成関数を登録する
    /// @tparam T 登録するコンポーネントの型
    /// @param name 型名（TypeName() と同じ文字列にする）
    template <typename T>
    static void Register(const std::string& name) {
      Registry()[name] = []() { return std::make_unique<T>(); };
    }

    /// @brief 型名からコンポーネントを生成する
    /// @param name 型名
    /// @return 生成したコンポーネント（未登録なら nullptr）
    static std::unique_ptr<IComponent> Create(const std::string& name) {
      auto& reg = Registry();
      auto it = reg.find(name);
      if (it != reg.end()) return it->second();
      return nullptr;
    }

  private:
    /// @brief 型名 → 生成関数の表を取得する
    /// @return 関数内 static の表（静的初期化順の問題を避けるため関数内に置く）
    static std::unordered_map<std::string, Creator>& Registry() {
      static std::unordered_map<std::string, Creator> registry;
      return registry;
    }
  };

private:
  uint32_t id_;                 ///< 実行中の一意 ID（保存しない）
  std::string name_;            ///< エンティティ名
  bool active_ = true;          ///< アクティブか
  bool visible_ = true;         ///< 表示するか
  bool locked_ = false;         ///< ロック中か
  bool pendingDestroy_ = false; ///< 破棄待ちか
  uint64_t guid_;               ///< 保存される一意 ID
  uint64_t parentGuid_ = 0;     ///< 親の GUID（0 なら親なし）
  bool isFolder_ = false;       ///< UI 上のフォルダか

  std::unordered_map<std::type_index, std::unique_ptr<IComponent>> components_; ///< 型 → コンポーネント（1 型につき 1 つ）
  std::unordered_map<std::string, int> intTags_; ///< スクリプト間のやり取り用の軽量タグ

  /// @brief 次の一意 ID を発行する
  /// @return 発行した ID（1 から連番）
  static uint32_t NextId() {
    static uint32_t counter = 0;
    return ++counter;
  }
};
