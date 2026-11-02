#include "CombatStrategyRegistry.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>

#include <nlohmann/json.hpp>

#include "DebugLog.h"

void CombatStrategyRegistry::Load(const std::string &directoryPath) {
  if (!std::filesystem::exists(directoryPath)) {
    const std::string errStr =
        "Warning: Combat strategy directory not found: " + directoryPath + "\n";
    if (debugLog) debugLog->LogError(errStr);
    else std::cerr << errStr;
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

    try {
      nlohmann::json j;
      file >> j;

      CombatStrategyDef def;
      def.id = entry.path().stem().string();
      def.name = j.value("name", def.id);
      def.description = j.value("description", std::string());
      def.behavior = j.value("behavior", std::string("chase"));
      def.preferredRange = j.value("preferredRange", 1.0f);
      def.repathSeconds = j.value("repathSeconds", 0.5f);

      if (def.behavior != "chase") {
        warn("Warning: Combat strategy '" + def.id + "' uses behavior '" +
             def.behavior +
             "', which this build does not implement yet; falling back to chase.");
        def.behavior = "chase";
      }
      if (def.preferredRange <= 0.0f) {
        warn("Warning: Combat strategy '" + def.id +
             "' has non-positive preferredRange; using 1 tile.");
        def.preferredRange = 1.0f;
      }
      if (def.repathSeconds <= 0.0f) {
        warn("Warning: Combat strategy '" + def.id +
             "' has non-positive repathSeconds; using 0.5.");
        def.repathSeconds = 0.5f;
      }

      strategies[def.id] = std::move(def);
    } catch (const std::exception &e) {
      warn("Failed to parse combat strategy " + entry.path().string() + ": " +
           std::string(e.what()));
    }
  }
}

const CombatStrategyDef *CombatStrategyRegistry::Get(
    const std::string &id) const {
  auto it = strategies.find(id);
  return it == strategies.end() ? nullptr : &it->second;
}

std::vector<const CombatStrategyDef *> CombatStrategyRegistry::GetAll() const {
  std::vector<const CombatStrategyDef *> defs;
  defs.reserve(strategies.size());
  for (const auto &pair : strategies) {
    defs.push_back(&pair.second);
  }
  std::sort(defs.begin(), defs.end(),
            [](const CombatStrategyDef *a, const CombatStrategyDef *b) {
              return a->name < b->name;
            });
  return defs;
}
