#pragma once

#include <string>
#include <vector>

#include "SourceScaling.hpp"

// Definition of one improvable skill, loaded from data/skills/<id>.json.
//
// A skill is a source exactly like a stat -- its level is a magnitude that links
// can scale an outcome by -- and what separates the two is only HOW the magnitude
// moves. A stat changes on level-up or a special event; a skill changes by being
// used (docs/stat-effects-design.md section 1).
//
// Stages are an OPEN set: any number, any names, declared in data. The engine
// only ever compares an integer stage index, so a future race or class can have a
// skill with three stages and no engine code changes at all.
//
// LEVEL CONVENTION. `level` runs 0..MaxLevel(), where 0 is untrained and
// MaxLevel() is the final stage at its final level -- 45 for five stages of nine.
// Stage and position within the stage are DERIVED, never stored, so the two can
// never disagree:
//
//   level  1 -> stage 0, within-stage 1   (Initiate 1/9)
//   level  9 -> stage 0, within-stage 9   (Initiate 9/9)
//   level 10 -> stage 1, within-stage 1   (Journeyman 1/9)
//   level 45 -> stage 4, within-stage 9   (Grandmaster 9/9)
//
// So "finishing the 9th level" is exactly the step that moves you up a stage.
struct SkillDef {
  std::string id;
  std::string name;
  // Flavour for [SKILLS], never a mechanical claim: the "affects harvesting"
  // clause is generated from the registered links (section 9.1).
  std::string description;

  // Stage names, lowest first. The size is the number of stages.
  std::vector<std::string> stages;
  int levelsPerStage = 9;

  // Which activities may train this skill at all: "harvest", "craft", or "*".
  //
  // A GUARD, not a selector. It never decides who trains what -- the subject
  // names the skill and the amount -- it only lets the loader report a mismatch
  // at startup. Without it, a recipe declaring `"trains": {"farming": 8}` would
  // be a silent no-op on every craft, which is precisely the class of bug the
  // startup hook check exists to catch.
  std::vector<std::string> trainedBy;

  // XP needed for one level while in a stage, indexed by stage. The last entry
  // repeats for any stage past the end of the list, and an empty list means a
  // flat default.
  //
  // Cost is constant within a stage on purpose: the stage is the meaningful unit
  // of the design, and a within-stage ramp can be added later without changing
  // how anything else reads.
  std::vector<int> xpPerStage;

  // Same meaning as StatDef::baseline and StatDef::scale, so one contribution
  // builder can serve both kinds of source. A skill's neutral is untrained, so
  // baseline is normally 0; `scale` is what keeps a coefficient readable across
  // 45 levels -- set it to levelsPerStage and a coefficient reads as "per stage".
  float baseline = 0.0f;
  float scale = 1.0f;

  static constexpr int kDefaultXpPerLevel = 100;

  // How this definition turns a level into an effect, on the same terms a stat
  // does. The range is the level range, so BaselineMode::Min measures from
  // untrained.
  SourceScaling Scaling() const {
    SourceScaling scaling;
    scaling.baseline = baseline;
    scaling.scale = scale;
    scaling.min = 0.0f;
    scaling.max = static_cast<float>(MaxLevel());
    return scaling;
  }

  int StageCount() const { return static_cast<int>(stages.size()); }

  // Total levels reachable: 45 for five stages of nine.
  int MaxLevel() const { return StageCount() * levelsPerStage; }

  // Stage a level falls in, clamped to the last stage. Untrained is stage 0.
  int StageForLevel(int level) const {
    if (level <= 0 || stages.empty() || levelsPerStage <= 0) {
      return 0;
    }
    const int stage = (level - 1) / levelsPerStage;
    return stage >= StageCount() ? StageCount() - 1 : stage;
  }

  // Position within the current stage: 0 when untrained, otherwise 1..levelsPerStage.
  int LevelWithinStage(int level) const {
    if (level <= 0 || levelsPerStage <= 0) {
      return 0;
    }
    return ((level - 1) % levelsPerStage) + 1;
  }

  // Empty string only for a skill that declares no stages at all.
  const std::string &StageName(int stage) const {
    static const std::string unknown = "";
    if (stages.empty()) {
      return unknown;
    }
    const int last = StageCount() - 1;
    const int clamped = stage < 0 ? 0 : (stage > last ? last : stage);
    return stages[static_cast<size_t>(clamped)];
  }

  // XP to go from `level` to `level + 1`. 0 means already at the cap.
  //
  // The cost comes from the stage the character is currently in: advancing out
  // of Initiate is Initiate work. Where a stage costs nothing, that would be an
  // infinite loop, so a non-positive entry is treated as the default.
  int XpForNextLevel(int level) const {
    if (level >= MaxLevel()) {
      return 0;
    }
    if (xpPerStage.empty()) {
      return kDefaultXpPerLevel;
    }
    const size_t index = static_cast<size_t>(StageForLevel(level));
    const int cost = index < xpPerStage.size() ? xpPerStage[index]
                                               : xpPerStage.back();
    return cost > 0 ? cost : kDefaultXpPerLevel;
  }
};
