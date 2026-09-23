#include "SkillRegistry.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>

#include "DebugLog.h"
#include "EffectParsing.h"

void SkillRegistry::LoadSkills(const std::string& directoryPath) {
  if (!std::filesystem::exists(directoryPath)) {
    const std::string errStr =
        "Warning: Skill directory not found: " + directoryPath + "\n";
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

      SkillDef def;
      def.id = entry.path().stem().string();
      def.name = j.value("name", def.id);
      def.description = j.value("description", "");
      def.levelsPerStage = j.value("levelsPerStage", 9);
      // Untrained is neutral for a skill, so the baseline is 0 rather than the
      // average-human 10 a stat uses (section 3.4).
      def.baseline = j.value("baseline", 0.0f);
      def.scale = j.value("scale", 1.0f);

      if (j.contains("stages")) {
        for (const auto& stage : j["stages"]) {
          def.stages.push_back(stage.get<std::string>());
        }
      }
      if (j.contains("xpPerStage")) {
        for (const auto& cost : j["xpPerStage"]) {
          def.xpPerStage.push_back(cost.get<int>());
        }
      }
      if (j.contains("trainedBy")) {
        for (const auto& activity : j["trainedBy"]) {
          def.trainedBy.push_back(activity.get<std::string>());
        }
      }

      // A skill nothing can progress through is content error, not a runtime
      // state: report it here rather than letting it sit inert forever.
      if (def.stages.empty()) {
        warn("Warning: Skill '" + def.id +
             "' declares no stages, so it can never progress.");
      }
      if (def.levelsPerStage <= 0) {
        warn("Warning: Skill '" + def.id + "' has levelsPerStage <= 0; using 9.");
        def.levelsPerStage = 9;
      }
      if (def.scale <= 0.0f) {
        warn("Warning: Skill '" + def.id +
             "' has scale <= 0, which would divide by zero when resolving; "
             "using 1 instead.");
        def.scale = 1.0f;
      }
      if (def.xpPerStage.empty()) {
        warn("Warning: Skill '" + def.id +
             "' declares no xpPerStage, so every level costs the default " +
             std::to_string(SkillDef::kDefaultXpPerLevel) + " XP.");
      }
      for (size_t i = 0; i < def.xpPerStage.size(); ++i) {
        if (def.xpPerStage[i] <= 0) {
          warn("Warning: Skill '" + def.id + "' has a non-positive XP cost for " +
               def.StageName(static_cast<int>(i)) +
               "; that level will cost the default instead.");
        }
      }

      skills[def.id] = def;

      if (j.contains("effects")) {
        ParseEffectEntries(j["effects"], def.id, declared, warn);
      }
    } catch (const std::exception& e) {
      warn("Failed to parse skill " + entry.path().string() + ": " +
           std::string(e.what()));
    }
  }
}

const SkillDef* SkillRegistry::Get(const std::string& id) const {
  auto it = skills.find(id);
  if (it == skills.end()) return nullptr;
  return &it->second;
}

std::vector<std::string> SkillRegistry::GetIds() const {
  std::vector<std::string> ids;
  ids.reserve(skills.size());
  for (const auto& pair : skills) {
    ids.push_back(pair.first);
  }
  std::sort(ids.begin(), ids.end());
  return ids;
}

std::vector<const SkillDef*> SkillRegistry::GetAll() const {
  std::vector<const SkillDef*> defs;
  defs.reserve(skills.size());
  for (const std::string& id : GetIds()) {
    defs.push_back(Get(id));
  }
  return defs;
}
