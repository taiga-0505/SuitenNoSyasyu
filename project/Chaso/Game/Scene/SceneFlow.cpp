#include "SceneFlow.h"

#include "Common/Log/Log.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

SceneFlow &SceneFlow::Get() {
  static SceneFlow instance;
  return instance;
}

// ============================================================================
// 読み書き
// ============================================================================

bool SceneFlow::Load(const std::string &path) {
  path_ = path;
  rules_.clear();
  dirty_ = false;

  if (!std::filesystem::exists(path)) {
    Log::Print("[SceneFlow] " + path + " が無いため空の遷移表で始めます");
    return false;
  }

  std::ifstream ifs(path);
  if (!ifs.is_open()) {
    Log::Print("[SceneFlow] 開けませんでした: " + path);
    return false;
  }

  nlohmann::json root;
  try {
    ifs >> root;
  } catch (const nlohmann::json::exception &e) {
    Log::Print("[SceneFlow] JSON の読み込みに失敗: " + std::string(e.what()));
    return false;
  }

  if (root.contains("rules") && root["rules"].is_array()) {
    for (const auto &j : root["rules"]) {
      if (!j.is_object()) continue;
      SceneFlowRule r;
      r.from = j.value("from", std::string());
      r.trigger = j.value("trigger", std::string());
      r.to = j.value("to", std::string());
      r.transition = j.value("transition", std::string(SceneTransitions::kDissolve));
      r.note = j.value("note", std::string());
      rules_.push_back(std::move(r));
    }
  }

  Log::Print("[SceneFlow] 読み込み: " + path + " (" + std::to_string(rules_.size()) + " 行)");
  return true;
}

bool SceneFlow::Save() {
  nlohmann::json rules = nlohmann::json::array();
  for (const auto &r : rules_) {
    nlohmann::json j = {
        {"from", r.from},
        {"trigger", r.trigger},
        {"to", r.to},
        {"transition", r.transition},
    };
    if (!r.note.empty()) j["note"] = r.note;
    rules.push_back(std::move(j));
  }
  nlohmann::json root = {{"version", 1}, {"rules", std::move(rules)}};

  const std::filesystem::path p(path_);
  if (p.has_parent_path()) {
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
  }
  std::ofstream ofs(path_);
  if (!ofs.is_open()) {
    Log::Print("[SceneFlow] 保存できませんでした: " + path_);
    return false;
  }
  ofs << root.dump(2);
  dirty_ = false;
  Log::Print("[SceneFlow] 保存: " + path_);
  return true;
}

// ============================================================================
// 解決
// ============================================================================

bool SceneFlow::MatchFrom(const std::string &pattern, const std::string &scene, int *score) {
  int s = 0;
  if (pattern == "*") {
    s = 1;
  } else if (!pattern.empty() && pattern.back() == '*') {
    const std::string prefix = pattern.substr(0, pattern.size() - 1);
    if (scene.compare(0, prefix.size(), prefix) != 0) return false;
    s = 2 + static_cast<int>(prefix.size()); // 長い前方一致ほど優先
  } else if (pattern == scene) {
    s = 100000; // 完全一致が最優先
  } else {
    return false;
  }
  if (score) *score = s;
  return true;
}

const SceneFlowRule *SceneFlow::FindRule(const std::string &fromScene,
                                         const std::string &trigger) const {
  const SceneFlowRule *best = nullptr;
  int bestScore = -1;
  for (const auto &r : rules_) {
    if (r.trigger != trigger) continue;
    int score = 0;
    if (!MatchFrom(r.from, fromScene, &score)) continue;
    // 同じ優先度なら上の行を採る
    if (score > bestScore) {
      best = &r;
      bestScore = score;
    }
  }
  return best;
}

std::string SceneFlow::ResolveTarget(const std::string &to, const std::string &fromScene,
                                     const std::string &arg) const {
  if (to.empty() || to[0] != '$') return to;
  if (to == kVarCurrent) return fromScene;
  if (to == kVarArg) return arg;
  for (const auto &v : variables_) {
    if (v.name == to) return v.fn ? v.fn() : std::string();
  }
  Log::Print("[SceneFlow] 未登録の変数です: " + to);
  return {};
}

bool SceneFlow::Resolve(const std::string &fromScene, const std::string &trigger,
                        const std::string &arg, SceneFlowResult &out) {
  out = SceneFlowResult{};
  const SceneFlowRule *rule = FindRule(fromScene, trigger);
  if (!rule) {
    // エディタの「未登録のきっかけ」に出して、ワンクリックで行を足せるようにする
    auto it = std::find_if(missing_.begin(), missing_.end(), [&](const MissingTrigger &m) {
      return m.from == fromScene && m.trigger == trigger;
    });
    if (it == missing_.end()) {
      missing_.push_back({fromScene, trigger, 1});
      Log::Print("[SceneFlow] 遷移表に行がありません: " + fromScene + " / " + trigger +
                 "（Window > Scene Flow で追加できます）");
    } else {
      ++it->count;
    }
    return false;
  }
  out.rule = rule;
  out.transition = rule->transition.empty() ? std::string(SceneTransitions::kDissolve)
                                            : rule->transition;
  out.target = ResolveTarget(rule->to, fromScene, arg);
  return true;
}

std::string SceneFlow::Describe(const std::string &fromScene, const std::string &trigger) const {
  const SceneFlowRule *rule = FindRule(fromScene, trigger);
  if (!rule) return trigger + " -> (未登録。Window > Scene Flow で追加)";
  std::string s = trigger + " -> " + (rule->to.empty() ? std::string("(空)") : rule->to);
  if (!rule->to.empty() && rule->to[0] == '$') {
    const std::string resolved = ResolveTarget(rule->to, fromScene, "");
    if (rule->to != kVarArg) s += " = " + (resolved.empty() ? std::string("(なし)") : resolved);
  }
  s += " (" + rule->transition + ")";
  return s;
}

const std::vector<std::string> &SceneFlow::TransitionNames() {
  static const std::vector<std::string> names = {SceneTransitions::kDissolve,
                                                 SceneTransitions::kDive,
                                                 SceneTransitions::kAscend};
  return names;
}

// ============================================================================
// 変数
// ============================================================================

void SceneFlow::RegisterVariable(const std::string &name, const std::string &description,
                                 std::function<std::string()> fn) {
  for (auto &v : variables_) {
    if (v.name == name) { // 同名は上書き
      v.description = description;
      v.fn = std::move(fn);
      return;
    }
  }
  variables_.push_back({name, description, std::move(fn)});
}

std::vector<SceneFlow::VariableInfo> SceneFlow::Variables() const {
  std::vector<VariableInfo> list;
  list.push_back({kVarCurrent, "いまのシーン（やり直し）", true});
  list.push_back({kVarArg, "スクリプトが RequestTransition の第2引数で渡した行き先", true});
  for (const auto &v : variables_) list.push_back({v.name, v.description, false});
  return list;
}

bool SceneFlow::HasVariable(const std::string &name) const {
  if (name == kVarCurrent || name == kVarArg) return true;
  for (const auto &v : variables_) {
    if (v.name == name) return true;
  }
  return false;
}

void SceneFlow::RemoveMissing(const std::string &from, const std::string &trigger) {
  missing_.erase(std::remove_if(missing_.begin(), missing_.end(),
                                [&](const MissingTrigger &m) {
                                  return m.from == from && m.trigger == trigger;
                                }),
                 missing_.end());
}

// ============================================================================
// エディタ向け
// ============================================================================

std::vector<std::string> SceneFlow::SceneNames() const {
  std::vector<std::string> names = sceneNames_ ? sceneNames_() : std::vector<std::string>{};
  std::sort(names.begin(), names.end());
  return names;
}

bool SceneFlow::IsKnownScene(const std::string &name) const {
  if (!sceneNames_) return true; // 結線前は判定しない
  const auto names = sceneNames_();
  return std::find(names.begin(), names.end(), name) != names.end();
}

bool SceneFlow::Request(const std::string &target, const std::string &transition) {
  if (!request_ || target.empty()) return false;
  return request_(target, ParseSceneTransition(transition));
}
