#include "SkillshotRegistry.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>

#include <nlohmann/json.hpp>

#include "DebugLog.h"

void SkillshotRegistry::LoadSkillshots(const std::string &directoryPath) {
  if (!std::filesystem::exists(directoryPath)) {
    const std::string errStr =
        "Warning: Skillshot directory not found: " + directoryPath + "\n";
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

      SkillshotDef def;
      def.id = entry.path().stem().string();
      def.sprite = j.value("sprite", "");
      def.path = j.value("path", std::string("line"));
      def.speed = j.value("speed", 950.0f);
      def.width = j.value("width", 12.0f);
      def.length = j.value("length", 34.0f);

      if (def.sprite.empty()) {
        warn("Warning: Skillshot '" + def.id +
             "' declares no 'sprite', so it is ignored.");
        continue;
      }
      if (def.path != "line") {
        warn("Warning: Skillshot '" + def.id + "' uses path '" + def.path +
             "', which this build does not implement yet; falling back to a "
             "line.");
        def.path = "line";
      }
      if (def.speed <= 0.0f) {
        warn("Warning: Skillshot '" + def.id +
             "' has speed <= 0; using 950 instead.");
        def.speed = 950.0f;
      }
      if (def.width <= 0.0f) {
        warn("Warning: Skillshot '" + def.id +
             "' has width <= 0; using 12 instead.");
        def.width = 12.0f;
      }
      if (def.length <= 0.0f) {
        warn("Warning: Skillshot '" + def.id +
             "' has length <= 0; using 34 instead.");
        def.length = 34.0f;
      }

      skillshots[def.id] = std::move(def);
    } catch (const std::exception &e) {
      warn("Failed to parse skillshot " + entry.path().string() + ": " +
           std::string(e.what()));
    }
  }
}

const SkillshotDef *SkillshotRegistry::Get(const std::string &id) const {
  auto it = skillshots.find(id);
  return it == skillshots.end() ? nullptr : &it->second;
}

std::vector<std::string> SkillshotRegistry::GetIds() const {
  std::vector<std::string> ids;
  ids.reserve(skillshots.size());
  for (const auto &pair : skillshots) {
    ids.push_back(pair.first);
  }
  std::sort(ids.begin(), ids.end());
  return ids;
}

std::vector<const SkillshotDef *> SkillshotRegistry::GetAll() const {
  std::vector<const SkillshotDef *> defs;
  for (const std::string &id : GetIds()) {
    defs.push_back(Get(id));
  }
  return defs;
}
