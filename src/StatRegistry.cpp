#include "StatRegistry.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>

#include "DebugLog.h"
#include "EffectParsing.h"

void StatRegistry::LoadStats(const std::string& directoryPath) {
  if (!std::filesystem::exists(directoryPath)) {
    std::string errStr =
        "Warning: Stat directory not found: " + directoryPath + "\n";
    if (debugLog) debugLog->LogError(errStr);
    else std::cerr << errStr;
    return;
  }

  auto warn = [this](const std::string& message) {
    if (debugLog) debugLog->LogWarning(message);
    else std::cerr << message << std::endl;
  };

  for (const auto& entry : std::filesystem::directory_iterator(directoryPath)) {
    if (entry.path().extension() != ".json") continue;

    std::ifstream file(entry.path());
    if (!file.is_open()) continue;

    try {
      nlohmann::json j;
      file >> j;

      StatDef def;
      def.id = entry.path().stem().string();
      def.name = j.value("name", def.id);
      def.description = j.value("description", "");
      def.baseline = j.value("baseline", 10.0f);
      def.scale = j.value("scale", 1.0f);
      def.min = j.value("min", 1.0f);
      def.max = j.value("max", 20.0f);

      // Guards on the two values the fold divides and clamps by. A zero scale
      // would be a division by zero at resolution time, and an inverted range
      // would make every clamp produce the minimum, so both are corrected here
      // where the file name is still known to report.
      if (def.scale <= 0.0f) {
        warn("Warning: Stat '" + def.id +
             "' has scale <= 0, which would divide by zero when resolving; "
             "using 1 instead.");
        def.scale = 1.0f;
      }
      if (def.max <= def.min) {
        warn("Warning: Stat '" + def.id + "' has max (" +
             std::to_string(def.max) + ") <= min (" + std::to_string(def.min) +
             "); every resolved value would clamp to the minimum.");
      }
      if (def.baseline < def.min || def.baseline > def.max) {
        warn("Warning: Stat '" + def.id + "' has baseline " +
             std::to_string(def.baseline) + " outside its range [" +
             std::to_string(def.min) + ", " + std::to_string(def.max) +
             "], so the neutral value is not reachable.");
      }

      stats[def.id] = def;

      if (j.contains("effects")) {
        ParseEffectEntries(j["effects"], def.id, declared, warn);
      }
    } catch (const std::exception& e) {
      warn("Failed to parse stat " + entry.path().string() + ": " +
           std::string(e.what()));
    }
  }
}

const StatDef* StatRegistry::Get(const std::string& id) const {
  auto it = stats.find(id);
  if (it == stats.end()) return nullptr;
  return &it->second;
}

std::vector<std::string> StatRegistry::GetIds() const {
  std::vector<std::string> ids;
  ids.reserve(stats.size());
  for (const auto& pair : stats) {
    ids.push_back(pair.first);
  }
  std::sort(ids.begin(), ids.end());
  return ids;
}

std::vector<const StatDef*> StatRegistry::GetAll() const {
  std::vector<const StatDef*> defs;
  defs.reserve(stats.size());
  for (const std::string& id : GetIds()) {
    defs.push_back(Get(id));
  }
  return defs;
}
