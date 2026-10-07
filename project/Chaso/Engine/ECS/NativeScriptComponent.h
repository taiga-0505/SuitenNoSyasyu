#pragma once
#include "IComponent.h"
#include "ScriptableEntity.h"
#include "ScriptRegistry.h"
#include "Common/FrameProfiler.h"
#include <functional>
#include <string>
#include <vector>

/// @brief Component that manages and executes native scripts (ScriptableEntity).
class NativeScriptComponent : public IComponent {
public:
  struct ScriptEntry {
    ScriptableEntity* instance = nullptr;
    std::string scriptTypeName;
    std::function<ScriptableEntity*()> InstantiateScript;
    std::function<void(ScriptEntry&)> DestroyScript;
    nlohmann::json pendingData;
    const char* profileName = nullptr; ///< FrameProfiler の区間名（Intern 済み。ランタイム専用）
  };

  std::vector<ScriptEntry> scripts;

  /// @brief エンティティ破棄の通知
  /// @details スクリプトの OnDestroy() は GetComponent() で同じエンティティの
  ///          他コンポーネントを参照する。デストラクタまで待つと components_ の
  ///          破棄が始まっており、破棄途中の unordered_map を検索して落ちるため、
  ///          全コンポーネントが有効なこの時点で片付ける。
  void OnEntityDestroy() override { DestroyAllScripts(); }

  ~NativeScriptComponent() override {
    // OnEntityDestroy() が呼ばれずに破棄された場合（Entity から切り離して
    // 単体で持っているケース）の保険。DestroyAllScripts は再入しても安全。
    DestroyAllScripts();
  }

  /// @brief 保持しているスクリプトすべてに OnDestroy() を通知して破棄する
  /// @note instance を nullptr に戻すので二度呼んでも二重解放にならない
  void DestroyAllScripts() {
    for (auto& entry : scripts) {
      if (entry.instance && entry.DestroyScript) {
        entry.instance->OnDestroy();
        entry.DestroyScript(entry);
      }
    }
    scripts.clear();
  }

  /// @brief Add a script type to this component
  /// @tparam T Script type (must inherit from ScriptableEntity)
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

  /// @brief Add a script by its registered string name
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

  /// @brief Remove a script by its index in the vector
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

  /// @brief Set the scene reference for the scripts
  void SetScene(class Scene* scene) { scene_ = scene; }

  /// @brief Set the scene context reference for the scripts
  void SetSceneContext(SceneContext* ctx) { sceneContext_ = ctx; }

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
          // Fallback: script not found, prevent further instantiate attempts
          printf("ERROR: Failed to instantiate script: %s\n", entry.scriptTypeName.c_str());
          entry.InstantiateScript = nullptr;
        }
      }
      if (entry.instance) {
        // Update context each frame (may change between frames)
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

  const char* TypeName() const override { return "NativeScriptComponent"; }

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
    
    // Backward compatibility property
    if (!names.empty()) {
      j["scriptTypeName"] = names[0];
    }
    return j;
  }

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
  class Scene* scene_ = nullptr;
  SceneContext* sceneContext_ = nullptr;
};
