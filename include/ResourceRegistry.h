#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

class DebugLog;

// One character resource definition. Vitality is NOT a property of the resource
// globally: a ghost's Mana is vital while a human's is not. That is decided by
// the character's ResourceSpec (race/template).
struct ResourceDef {
  std::string id;
  std::string name;
  std::string description;
  std::string icon;
  std::string color = "#cccccc";
  float baseMax = 100.0f;
  bool canBeVital = true;
  bool canBeSpendable = true;
  // A shield resource is still a separate pool; damage consumes it before the
  // character's primary vital resource.
  bool shield = false;
  bool hidden = false;
  int displayOrder = 0;
  float regenPerSecond = 0.0f;
};

// One pool a character/race/template grants. `current < 0` means "spawn full".
struct ResourceSpec {
  std::string id;
  bool vital = false;
  bool primary = false;
  float current = -1.0f;
  float maxOverride = -1.0f;
};

// Shared parser for race/NPC/character JSON:
//   "resources": ["health", {"id":"mana","vital":true,"primary":true}]
bool ParseResourceSpecs(const nlohmann::json &value,
                        std::vector<ResourceSpec> &out,
                        const std::string &owner,
                        std::vector<std::string> *warnings);
nlohmann::json ResourceSpecsJson(const std::vector<ResourceSpec> &specs);

class ResourceRegistry {
public:
  void Load(const std::string &directoryPath);

  const ResourceDef *Get(const std::string &id) const;
  std::vector<const ResourceDef *> GetAll() const;
  size_t Count() const { return resources.size(); }

  nlohmann::json ToJson(const ResourceDef &definition) const;
  bool AddOrReplace(const nlohmann::json &document, const std::string &source,
                    std::string *error, std::vector<std::string> *warnings);
  static std::string IdFor(const nlohmann::json &document);

  void SetDebugLog(DebugLog *log) { debugLog = log; }

private:
  std::unordered_map<std::string, ResourceDef> resources;
  DebugLog *debugLog = nullptr;
};
