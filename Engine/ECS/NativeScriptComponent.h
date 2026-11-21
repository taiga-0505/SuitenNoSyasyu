#pragma once
#include "IComponent.h"
#include "ScriptableEntity.h"
#include "ScriptRegistry.h"
#include "Common/FrameProfiler.h"
#include <functional>
#include <string>
#include <vector>

/// @class NativeScriptComponent
/// @brief ネイティブスクリプト（ScriptableEntity）を保持して実行するコンポーネント
class NativeScriptComponent : public IComponent {
public:
  /// @struct ScriptEntry
  /// @brief スクリプト 1 本分の登録情報
  struct ScriptEntry {
    ScriptableEntity* instance = nullptr;                  ///< 生成済みのインスタンス（未生成なら nullptr）
    std::string scriptTypeName;                            ///< スクリプトの型名（シリアライズ用）
    std::function<ScriptableEntity*()> InstantiateScript;  ///< インスタンスの生成関数（生成に失敗したら nullptr にする）
    std::function<void(ScriptEntry&)> DestroyScript;       ///< インスタンスの破棄関数
    nlohmann::json pendingData;                            ///< 生成時に Deserialize へ渡すデータ
    const char* profileName = nullptr; ///< FrameProfiler の区間名（Intern 済み。ランタイム専用）
  };

  std::vector<ScriptEntry> scripts; ///< 登録されたスクリプト（Update はこの順で呼ぶ）

  /// @brief エンティティ破棄の通知
  /// @details スクリプトの OnDestroy() は GetComponent() で同じエンティティの
  ///          他コンポーネントを参照する。デストラクタまで待つと components_ の
  ///          破棄が始まっており、破棄途中の unordered_map を検索して落ちるため、
  ///          全コンポーネントが有効なこの時点で片付ける。
  void OnEntityDestroy() override { DestroyAllScripts(); }

  /// @brief 残っているスクリプトを破棄する
  ~NativeScriptComponent() override {
    // OnEntityDestroy() が呼ばれずに破棄された場合（Entity から切り離して
    // 単体で持っているケース）の保険。DestroyAllScripts は再入しても安全。
    DestroyAllScripts();
  }

  /// @brief 保持しているスクリプトすべてに OnDestroy() を通知して破棄する
  /// @note instance を nullptr に戻すので二度呼んでも二重解放にならない。
  void DestroyAllScripts() {
    for (auto& entry : scripts) {
      if (entry.instance && entry.DestroyScript) {
        entry.instance->OnDestroy();
        entry.DestroyScript(entry);
      }
    }
    scripts.clear();
  }

  /// @brief スクリプトの型を指定して追加する
  /// @tparam T スクリプトの型（ScriptableEntity の派生であること）
  /// @note インスタンスは次の Update で生成される。
  template <typename T> void AddScript() {
    ScriptEntry entry;
    entry.InstantiateScript = []() { return static_cast<ScriptableEntity*>(new T()); };
    entry.DestroyScript = [](ScriptEntry& se) {
      delete se.instance;
      se.instance = nullptr;
    };
    entry.scriptTypeName = typeid(T).name();
    scripts.push_back(entry);
  }

  /// @brief ScriptRegistry に登録された名前でスクリプトを追加する
  /// @param name 登録名（空なら何もしない）
  /// @note インスタンスは次の Update で生成される。
  void AddScript(const std::string& name) {
    if (name.empty()) return;
    
    ScriptEntry entry;
    entry.scriptTypeName = name;
    entry.InstantiateScript = [name]() { return ScriptRegistry::Instantiate(name); };
    entry.DestroyScript = [](ScriptEntry& se) {
      delete se.instance;
      se.instance = nullptr;
    };
    scripts.push_back(entry);
  }

  /// @brief scripts の添え字を指定してスクリプトを取り除く
  /// @param index 取り除くスクリプトの添え字（範囲外なら何もしない）
  /// @note 生成済みなら OnDestroy() を呼んでから破棄する。
  void RemoveScriptAtIndex(size_t index) {
    if (index < scripts.size()) {
      auto& entry = scripts[index];
      if (entry.instance && entry.DestroyScript) {
        entry.instance->OnDestroy();
        entry.DestroyScript(entry);
      }
      scripts.erase(scripts.begin() + index);
    }
  }

  /// @brief スクリプトに渡すシーンを設定する
  /// @param scene 所属シーン
  void SetScene(class Scene* scene) { scene_ = scene; }

  /// @brief スクリプトに渡すシーンコンテキストを設定する
  /// @param ctx シーンコンテキスト
  void SetSceneContext(SceneContext* ctx) { sceneContext_ = ctx; }

  /// @brief 未生成のスクリプトを生成し、全スクリプトの OnUpdate を呼ぶ
  /// @param deltaTime 前フレームからの経過時間（秒）
  /// @details 生成時は pendingData があれば Deserialize してから OnCreate を呼ぶ。
  ///          生成に失敗したスクリプトは、以後生成を試みない。
  void Update(float deltaTime) override {
    for (auto& entry : scripts) {
      if (!entry.instance && entry.InstantiateScript) {
        entry.instance = entry.InstantiateScript();
        if (entry.instance) {
          entry.instance->entity_ = GetEntity();
          entry.instance->scene_ = scene_;
          entry.instance->sceneContext_ = sceneContext_;
          if (!entry.pendingData.is_null()) {
            entry.instance->Deserialize(entry.pendingData);
          }
          entry.instance->OnCreate();
        } else {
          // スクリプトが見つからなかったので、以後は生成を試みないようにする（毎フレーム失敗し続けないため）
          printf("ERROR: Failed to instantiate script: %s\n", entry.scriptTypeName.c_str());
          entry.InstantiateScript = nullptr;
        }
      }
      if (entry.instance) {
        // シーンやコンテキストはフレーム間で変わりうるので、毎フレーム渡し直す
        entry.instance->scene_ = scene_;
        entry.instance->sceneContext_ = sceneContext_;
#if RC_ENABLE_IMGUI
        // スクリプトの種類ごとに Update の時間を集計する（Performance パネルの内訳表示用）
        if (!entry.profileName && !entry.scriptTypeName.empty()) {
          entry.profileName = FrameProfiler::Get().Intern("Script: " + entry.scriptTypeName);
        }
        if (entry.profileName) {
          FrameProfileScope scope(entry.profileName);
          entry.instance->OnUpdate(deltaTime);
        } else {
          entry.instance->OnUpdate(deltaTime);
        }
#else
        entry.instance->OnUpdate(deltaTime);
#endif
      }
    }
  }

  /// @brief コンポーネントの型名を返す
  /// @return "NativeScriptComponent"
  const char* TypeName() const override { return "NativeScriptComponent"; }

  /// @brief スクリプトの型名と各スクリプトのデータを JSON に書き出す
  /// @return scriptTypeNames / scriptDataList（＋旧形式の scriptTypeName）を格納した JSON
  /// @note 未生成のスクリプトは pendingData をそのまま書き出す。
  nlohmann::json Serialize() const override {
    nlohmann::json j;
    std::vector<std::string> names;
    nlohmann::json dataList = nlohmann::json::array();
    for (const auto& entry : scripts) {
      if (!entry.scriptTypeName.empty()) {
        names.push_back(entry.scriptTypeName);
        if (entry.instance) {
          dataList.push_back(entry.instance->Serialize());
        } else {
          dataList.push_back(entry.pendingData);
        }
      }
    }
    j["scriptTypeNames"] = names;
    j["scriptDataList"] = dataList;
    
    // 旧形式（スクリプト 1 本のみ）との後方互換用
    if (!names.empty()) {
      j["scriptTypeName"] = names[0];
    }
    return j;
  }

  /// @brief JSON からスクリプト構成を読み込む
  /// @param j 読み込み元の JSON（新形式 scriptTypeNames、無ければ旧形式 scriptTypeName）
  /// @note 既存のスクリプトはすべて OnDestroy() してから破棄する。各スクリプトのデータは
  ///       pendingData に入れ、次の Update で生成するときに渡す。
  void Deserialize(const nlohmann::json& j) override {
    for (auto& entry : scripts) {
      if (entry.instance && entry.DestroyScript) {
        entry.instance->OnDestroy();
        entry.DestroyScript(entry);
      }
    }
    scripts.clear();

    if (j.contains("scriptTypeNames") && j["scriptTypeNames"].is_array()) {
      auto names = j["scriptTypeNames"].get<std::vector<std::string>>();
      nlohmann::json dataList = j.value("scriptDataList", nlohmann::json::array());
      for (size_t i = 0; i < names.size(); ++i) {
        AddScript(names[i]);
        if (i < dataList.size() && !scripts.empty()) {
          scripts.back().pendingData = dataList[i];
        }
      }
    } else if (j.contains("scriptTypeName")) {
      std::string name = j["scriptTypeName"].get<std::string>();
      if (!name.empty()) {
        AddScript(name);
      }
    }
  }

private:
  class Scene* scene_ = nullptr;         ///< スクリプトに渡すシーン
  SceneContext* sceneContext_ = nullptr; ///< スクリプトに渡すシーンコンテキスト
};
