#pragma once

#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "SkillDef.hpp"
#include "SkillTypes.h"
#include "StringUtils.hpp"

// Starting proficiency authored on an NPC in the map file:
//
//   "skills": { "farming": { "stage": "Master", "level": 1 } }
//
// `stage` names a stage from the skill's own data (data/skills/<id>.json) and
// `level` is the position WITHIN that stage, 1..levelsPerStage. That is how the
// game itself talks about rank -- the status window prints "Farming: Master
// 1/9" -- and keeping the pair rather than a raw number means an authored rank
// stays meaningful if a skill ever changes its levelsPerStage.
//
// The absolute level the engine stores is DERIVED from the two, never authored,
// for the same reason SkillDef derives a stage back out of a level: there is no
// second fact that can disagree with the first. "Master 1" is 3 * 9 + 1 = 28.
//
// This is the same map-JSON-to-createNPC stamping the design doc reserves for
// "race"/"class" keys (docs/stat-effects-design.md section 6.3), so an NPC's
// authored identity lives in one place: the map.
//
// Header-only on purpose. The resolve/apply half is pure (no flecs, no raylib,
// no filesystem), and keeping the parse beside it lets the skill test exercise
// both without a new translation unit in the build.
struct StartingSkill {
  // SkillDef id, e.g. "farming".
  std::string id;
  // Stage NAME from that skill's data, e.g. "Master". Empty means the first
  // stage, so a plain starting level can be written without naming a stage.
  std::string stage;
  // Position within the stage. Clamped into 1..levelsPerStage on resolve, so an
  // out-of-range value is forgiving rather than a load failure.
  int levelInStage = 1;
};

// Index of the stage whose name matches, or -1. Case-insensitive because stage
// names are content, and a map author should not have to match their
// capitalisation exactly.
inline int StartingStageIndex(const SkillDef &def, const std::string &name) {
  for (int stage = 0; stage < def.StageCount(); ++stage) {
    if (StringUtils::EqualsIgnoreCase(def.StageName(stage), name)) {
      return stage;
    }
  }
  return -1;
}

// The absolute level a spec means, or -1 when this skill cannot hold one: it
// declares no stages, or the spec names a stage it does not have.
inline int StartingLevelFor(const SkillDef &def, const StartingSkill &spec) {
  if (def.StageCount() <= 0 || def.levelsPerStage <= 0) {
    return -1;
  }

  int stage = 0;
  if (!spec.stage.empty()) {
    stage = StartingStageIndex(def, spec.stage);
    if (stage < 0) {
      return -1;
    }
  }

  int within = spec.levelInStage;
  if (within < 1) {
    within = 1;
  } else if (within > def.levelsPerStage) {
    within = def.levelsPerStage;
  }

  return stage * def.levelsPerStage + within;
}

// Sets the character's level for one skill, clearing any part-progress: a
// starting rank is a clean level, not a level plus unexplained XP.
//
// Returns false and fills `reason` when the spec cannot be honoured -- the
// character's block lacks the skill, or the stage name is not one this skill
// declares. Nothing is written in that case.
inline bool ApplyStartingSkill(SkillBlock &block, const SkillDef &def,
                               const StartingSkill &spec,
                               std::string *reason = nullptr) {
  SkillValue *value = nullptr;
  for (SkillValue &candidate : block.values) {
    if (candidate.id == spec.id) {
      value = &candidate;
      break;
    }
  }
  if (value == nullptr) {
    if (reason) {
      *reason = "the character's skill block has no '" + spec.id + "'";
    }
    return false;
  }

  const int level = StartingLevelFor(def, spec);
  if (level < 0) {
    if (reason) {
      *reason = spec.stage.empty()
                    ? "skill '" + def.id + "' declares no stages"
                    : "skill '" + def.id + "' has no stage named '" +
                          spec.stage + "'";
    }
    return false;
  }

  value->level = level;
  value->xp = 0;
  return true;
}

// Parses a map NPC's "skills" object and appends what it declares to `out`.
//
// Malformed entries are reported through `warn` and skipped rather than
// aborting the map load, the same contract ParseTrainingGrants has. `ownerLabel`
// names the NPC in those warnings.
inline void ParseStartingSkills(
    const nlohmann::json &skills, const std::string &ownerLabel,
    std::vector<StartingSkill> &out,
    const std::function<void(const std::string &)> &warn) {
  if (skills.is_null()) {
    return;
  }
  if (!skills.is_object()) {
    warn("Warning: " + ownerLabel + " has a non-object 'skills'; ignoring it.");
    return;
  }

  for (auto it = skills.begin(); it != skills.end(); ++it) {
    StartingSkill spec;
    spec.id = it.key();

    if (!it.value().is_object()) {
      warn("Warning: " + ownerLabel + " declares skill '" + spec.id +
           "' with a non-object value; skipping it. Expected {\"stage\": "
           "\"Master\", \"level\": 1}.");
      continue;
    }
    const nlohmann::json &entry = it.value();

    if (entry.contains("stage")) {
      if (!entry["stage"].is_string()) {
        warn("Warning: " + ownerLabel + " skill '" + spec.id +
             "' has a non-string 'stage'; skipping it.");
        continue;
      }
      spec.stage = entry["stage"].get<std::string>();
    }

    if (entry.contains("level")) {
      if (!entry["level"].is_number_integer()) {
        warn("Warning: " + ownerLabel + " skill '" + spec.id +
             "' has a non-integer 'level'; skipping it.");
        continue;
      }
      spec.levelInStage = entry["level"].get<int>();
    }

    out.push_back(std::move(spec));
  }
}
