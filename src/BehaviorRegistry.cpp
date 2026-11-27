#include "BehaviorRegistry.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>

#include "DebugLog.h"

namespace {

std::string Slugify(const std::string &text) {
  std::string out;
  bool lastUnderscore = false;
  for (unsigned char c : text) {
    if (std::isalnum(c)) {
      out.push_back(static_cast<char>(std::tolower(c)));
      lastUnderscore = false;
    } else if (!out.empty() && !lastUnderscore) {
      out.push_back('_');
      lastUnderscore = true;
    }
  }
  while (!out.empty() && out.back() == '_') out.pop_back();
  return out;
}

void Warn(const std::string &message, std::vector<std::string> *warnings) {
  if (warnings != nullptr) warnings->push_back(message);
}

bool IsKnownMovementMode(const std::string &mode) {
  return mode == "chase" || mode == "kite" || mode == "flee" || mode == "hold";
}

bool ParsePolicy(const nlohmann::json &json, MovementPolicy &out,
                 const std::string &owner,
                 std::vector<std::string> *warnings) {
  if (!json.is_object()) {
    Warn(owner + ": movement policy is not an object; using chase", warnings);
    return false;
  }
  out.mode = json.value("mode", std::string("chase"));
  if (!IsKnownMovementMode(out.mode)) {
    Warn(owner + ": unknown movement mode '" + out.mode +
             "'; using chase", warnings);
    out.mode = "chase";
  }
  out.desiredRange = json.value("desiredRange", 1.0f);
  out.tolerance = std::max(0.0f, json.value("tolerance", 0.0f));
  out.allowActions = json.value("allowActions", true);
  if (out.desiredRange < 0.0f) {
    Warn(owner + ": negative desiredRange; using 0", warnings);
    out.desiredRange = 0.0f;
  }
  return true;
}

bool ParseCondition(const nlohmann::json &json, BehaviorCondition &out,
                    const std::string &owner,
                    std::vector<std::string> *warnings);

bool ParseConditionList(const nlohmann::json &json, const char *key,
                        BehaviorCondition &out, const std::string &owner,
                        std::vector<std::string> *warnings) {
  if (!json.is_array()) {
    Warn(owner + ": '" + key + "' is not an array", warnings);
    return false;
  }
  out.kind = std::string(key) == "any" ? BehaviorCondition::Kind::Any
                                       : BehaviorCondition::Kind::All;
  for (const auto &childJson : json) {
    BehaviorCondition child;
    if (ParseCondition(childJson, child, owner, warnings)) {
      out.children.push_back(std::move(child));
    }
  }
  return !out.children.empty();
}

bool ParseCondition(const nlohmann::json &json, BehaviorCondition &out,
                    const std::string &owner,
                    std::vector<std::string> *warnings) {
  out = BehaviorCondition{};
  if (json.is_null()) {
    out.kind = BehaviorCondition::Kind::Always;
    return true;
  }
  if (!json.is_object()) {
    Warn(owner + ": condition is not an object", warnings);
    return false;
  }
  if (json.contains("all")) {
    return ParseConditionList(json["all"], "all", out, owner, warnings);
  }
  if (json.contains("any")) {
    return ParseConditionList(json["any"], "any", out, owner, warnings);
  }
  if (json.contains("not")) {
    out.kind = BehaviorCondition::Kind::Not;
    BehaviorCondition child;
    if (!ParseCondition(json["not"], child, owner, warnings)) return false;
    out.children.push_back(std::move(child));
    return true;
  }

  std::vector<BehaviorCondition> leaves;
  for (auto it = json.begin(); it != json.end(); ++it) {
    const std::string key = it.key();
    const nlohmann::json &value = it.value();
    BehaviorCondition leaf;
    bool recognized = true;

    if (key == "self.health.pctBelow" || key == "self.health.pctAbove") {
      leaf.kind = key == "self.health.pctBelow"
                      ? BehaviorCondition::Kind::SelfHealthPctBelow
                      : BehaviorCondition::Kind::SelfHealthPctAbove;
      leaf.value = value.get<float>();
    } else if (key.rfind("self.resource.", 0) == 0) {
      const std::string rest = key.substr(std::string("self.resource.").size());
      const std::string suffix = ".pctBelow";
      const std::string suffixAbove = ".pctAbove";
      if (rest.size() > suffix.size() &&
          rest.compare(rest.size() - suffix.size(), suffix.size(), suffix) == 0) {
        leaf.kind = BehaviorCondition::Kind::SelfResourcePctBelow;
        leaf.id = rest.substr(0, rest.size() - suffix.size());
      } else if (rest.size() > suffixAbove.size() &&
                 rest.compare(rest.size() - suffixAbove.size(),
                              suffixAbove.size(), suffixAbove) == 0) {
        leaf.kind = BehaviorCondition::Kind::SelfResourcePctAbove;
        leaf.id = rest.substr(0, rest.size() - suffixAbove.size());
      } else {
        recognized = false;
      }
      if (recognized) leaf.value = value.get<float>();
    } else if (key == "target.distanceAtLeast" ||
               key == "target.distanceAtMost") {
      leaf.kind = key == "target.distanceAtLeast"
                      ? BehaviorCondition::Kind::TargetDistanceAtLeast
                      : BehaviorCondition::Kind::TargetDistanceAtMost;
      leaf.value = value.get<float>();
    } else if (key == "target.hasStatus" || key == "target.lacksStatus") {
      leaf.kind = key == "target.hasStatus"
                      ? BehaviorCondition::Kind::TargetHasStatus
                      : BehaviorCondition::Kind::TargetLacksStatus;
      leaf.id = value.get<std::string>();
    } else if (key == "self.hasStatus") {
      leaf.kind = BehaviorCondition::Kind::SelfHasStatus;
      leaf.id = value.get<std::string>();
    } else if (key == "alliesWithin" || key == "enemiesWithin") {
      leaf.kind = key == "alliesWithin"
                      ? BehaviorCondition::Kind::AlliesWithin
                      : BehaviorCondition::Kind::EnemiesWithin;
      leaf.range = value.value("range", 4.0f);
      leaf.count = value.value("countAtLeast", 1);
    } else {
      recognized = false;
    }

    if (!recognized) {
      Warn(owner + ": unknown condition '" + key + "'; ignoring it", warnings);
      continue;
    }
    leaves.push_back(std::move(leaf));
  }

  if (leaves.empty()) {
    out.kind = BehaviorCondition::Kind::Always;
    return true;
  }
  if (leaves.size() == 1) {
    out = std::move(leaves.front());
    return true;
  }
  out.kind = BehaviorCondition::Kind::All;
  out.children = std::move(leaves);
  return true;
}

bool ParseAction(const nlohmann::json &json, BehaviorAction &out,
                 const std::string &owner,
                 std::vector<std::string> *warnings) {
  if (!json.is_object()) {
    Warn(owner + ": action is not an object", warnings);
    return false;
  }
  out.skillId = json.value("skill", std::string());
  if (out.skillId.empty()) {
    Warn(owner + ": action has no 'skill'; skipping it", warnings);
    return false;
  }
  out.priority = json.value("priority", 0);
  out.minRange = std::max(0.0f, json.value("minRange", 0.0f));
  if (json.contains("when") &&
      !ParseCondition(json["when"], out.when, owner + " action '" + out.skillId + "'",
                      warnings)) {
    return false;
  }
  return true;
}

bool ParseBehavior(const nlohmann::json &document, BehaviorDef &out,
                   std::string *error, std::vector<std::string> *warnings) {
  if (!document.is_object()) {
    if (error) *error = "the behavior is not an object";
    return false;
  }
  out.document = document;
  out.name = document.value("name", std::string());
  out.id = Slugify(out.name);
  if (out.id.empty()) {
    if (error) *error = "the behavior has no \"name\", so it has no id";
    return false;
  }
  out.description = document.value("description", std::string());
  out.type = document.value("type", std::string("aggressive"));
  out.kind = document.value("kind", std::string("combat"));
  std::transform(out.type.begin(), out.type.end(), out.type.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (out.type != "neutral" && out.type != "aggressive") {
    Warn("Behavior '" + out.name + "' has unknown type '" + out.type +
             "'; using aggressive", warnings);
    out.type = "aggressive";
  }
  std::transform(out.kind.begin(), out.kind.end(), out.kind.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (out.kind != "combat" && out.kind != "wander" && out.kind != "patrol") {
    Warn("Behavior '" + out.name + "' has unknown kind '" + out.kind +
             "'; using combat", warnings);
    out.kind = "combat";
  }
  out.maxTargetDistance = document.value("maxTargetDistance", 12.0f);
  out.repathSeconds = document.value("repathSeconds", 0.5f);
  if (out.maxTargetDistance <= 0.0f) {
    Warn("Behavior '" + out.name + "' has non-positive maxTargetDistance; using 12",
         warnings);
    out.maxTargetDistance = 12.0f;
  }
  if (out.repathSeconds <= 0.0f) {
    Warn("Behavior '" + out.name + "' has non-positive repathSeconds; using 0.5",
         warnings);
    out.repathSeconds = 0.5f;
  }

  const std::string owner = "Behavior '" + out.name + "'";
  if (document.contains("movement")) {
    const auto &movement = document["movement"];
    if (movement.is_object() && movement.contains("default")) {
      ParsePolicy(movement["default"], out.defaultMovement,
                  owner + " default movement", warnings);
    }
    if (movement.is_object() && movement.contains("rules")) {
      if (!movement["rules"].is_array()) {
        Warn(owner + ": movement.rules is not an array", warnings);
      } else {
        for (const auto &ruleJson : movement["rules"]) {
          BehaviorMovementRule rule;
          rule.id = ruleJson.value("id", std::string());
          if (rule.id.empty()) {
            Warn(owner + ": movement rule has no id; skipping it", warnings);
            continue;
          }
          rule.priority = ruleJson.value("priority", 0);
          rule.minDuration = std::max(0.0f, ruleJson.value("minDuration", 0.0f));
          rule.cooldown = std::max(0.0f, ruleJson.value("cooldown", 0.0f));
          if (ruleJson.contains("enterWhen")) {
            if (!ParseCondition(ruleJson["enterWhen"], rule.enterWhen,
                                owner + " rule '" + rule.id + "'", warnings)) {
              continue;
            }
          }
          if (ruleJson.contains("exitWhen")) {
            if (!ParseCondition(ruleJson["exitWhen"], rule.exitWhen,
                                owner + " rule '" + rule.id + "'", warnings)) {
              continue;
            }
            rule.hasExit = true;
          }
          if (ruleJson.contains("policy")) {
            ParsePolicy(ruleJson["policy"], rule.policy,
                        owner + " rule '" + rule.id + "'", warnings);
          } else {
            Warn(owner + ": rule '" + rule.id +
                     "' has no policy; using chase", warnings);
          }
          out.rules.push_back(std::move(rule));
        }
        std::stable_sort(out.rules.begin(), out.rules.end(),
                         [](const BehaviorMovementRule &a,
                            const BehaviorMovementRule &b) {
                           return a.priority > b.priority;
                         });
      }
    }
  }

  if (document.contains("actions")) {
    if (!document["actions"].is_array()) {
      Warn(owner + ": actions is not an array", warnings);
    } else {
      for (const auto &actionJson : document["actions"]) {
        BehaviorAction action;
        if (ParseAction(actionJson, action, owner, warnings)) {
          out.actions.push_back(std::move(action));
        }
      }
      std::stable_sort(out.actions.begin(), out.actions.end(),
                       [](const BehaviorAction &a, const BehaviorAction &b) {
                         return a.priority > b.priority;
                       });
    }
  }

  if (out.actions.empty()) {
    Warn(owner + ": has no valid actions", warnings);
  }
  return true;
}

}  // namespace

void BehaviorRegistry::Load(const std::string &directoryPath) {
  if (!std::filesystem::exists(directoryPath)) {
    const std::string err = "Warning: Behavior directory not found: " +
                            directoryPath + "\n";
    if (debugLog) debugLog->LogError(err);
    else std::cerr << err;
    return;
  }
  auto warn = [this](const std::string &message) {
    if (debugLog) debugLog->LogWarning(message);
    else std::cerr << message << std::endl;
  };
  for (const auto &entry : std::filesystem::directory_iterator(directoryPath)) {
    if (entry.path().extension() != ".json") continue;
    std::ifstream file(entry.path());
    if (!file.is_open()) continue;
    nlohmann::json document;
    try {
      file >> document;
    } catch (const std::exception &e) {
      warn("Failed to parse behavior " + entry.path().string() + ": " + e.what());
      continue;
    }
    std::string error;
    std::vector<std::string> warnings;
    if (!AddOrReplace(document, entry.path().string(), &error, &warnings)) {
      warn("Behavior " + entry.path().string() + " refused: " + error);
      continue;
    }
    for (const std::string &warning : warnings) warn(warning);
  }
}

bool BehaviorRegistry::AddOrReplace(const nlohmann::json &document,
                                    const std::string &source, std::string *error,
                                    std::vector<std::string> *warnings) {
  (void)source;
  BehaviorDef parsed;
  if (!ParseBehavior(document, parsed, error, warnings)) return false;
  behaviors[parsed.id] = std::move(parsed);
  return true;
}

const BehaviorDef *BehaviorRegistry::Get(const std::string &id) const {
  const auto found = behaviors.find(id);
  return found == behaviors.end() ? nullptr : &found->second;
}

std::vector<const BehaviorDef *> BehaviorRegistry::GetAll() const {
  std::vector<const BehaviorDef *> out;
  out.reserve(behaviors.size());
  for (const auto &[id, behavior] : behaviors) {
    (void)id;
    out.push_back(&behavior);
  }
  return out;
}

nlohmann::json BehaviorRegistry::ToJson(const std::string &id) const {
  const BehaviorDef *definition = Get(id);
  return definition ? definition->document : nlohmann::json::object();
}

std::string BehaviorRegistry::IdFor(const nlohmann::json &document) {
  return Slugify(document.value("name", std::string()));
}
