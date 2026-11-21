#include "ScriptRegistry.h"

std::unordered_map<std::string, ScriptRegistry::InstantiateFn>& ScriptRegistry::GetRegistry() {
    static std::unordered_map<std::string, InstantiateFn> registry;
    return registry;
}

std::vector<std::string>& ScriptRegistry::GetNames() {
    static std::vector<std::string> names;
    return names;
}

void ScriptRegistry::Register(const std::string& name, InstantiateFn func) {
    auto& registry = GetRegistry();
    if (registry.find(name) == registry.end()) {
        registry[name] = func;
        GetNames().push_back(name);
    }
}

ScriptableEntity* ScriptRegistry::Instantiate(const std::string& name) {
    auto& registry = GetRegistry();
    auto it = registry.find(name);
    if (it != registry.end()) {
        return it->second();
    }
    return nullptr;
}

const std::vector<std::string>& ScriptRegistry::GetScriptNames() {
    return GetNames();
}
