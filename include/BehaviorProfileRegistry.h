#pragma once

#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

class DebugLog;

// One profile binds roles (neutral, aggressive, ...) to atomic behaviors.
// Behaviors stay reusable; an NPC references one profile instead of one field
// per role.
struct BehaviorProfileDef {
  std::string id;
  std::string name;
  std::string description;
  // Role used when the NPC is created / returns to calm.
  std::string initial = "neutral";
  // role -> BehaviorRegistry id.
  std::map<std::string, std::string> behaviors;
  // Transition metadata is preserved and exposed for the editor. The runtime
  // currently performs the same transitions through its existing triggers.
  nlohmann::json transitions = nlohmann::json::array();
  nlohmann::json document;
};

class BehaviorProfileRegistry {
public:
  void Load(const std::string &directoryPath);

  const BehaviorProfileDef *Get(const std::string &id) const;
  std::vector<const BehaviorProfileDef *> GetAll() const;
  size_t Count() const { return profiles.size(); }

  nlohmann::json ToJson(const std::string &id) const;
  bool AddOrReplace(const nlohmann::json &document, const std::string &source,
                    std::string *error, std::vector<std::string> *warnings);
  static std::string IdFor(const nlohmann::json &document);

  void SetDebugLog(DebugLog *log) { debugLog = log; }

private:
  std::map<std::string, BehaviorProfileDef> profiles;
  DebugLog *debugLog = nullptr;
};
