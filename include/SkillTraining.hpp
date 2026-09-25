#pragma once

#include <string>
#include <utility>
#include <vector>

#include "SkillDef.hpp"
#include "SkillProgression.hpp"
#include "SkillRegistry.h"
#include "SkillTypes.h"
#include "SkillXp.hpp"

// One skill that actually moved, and by how much.
//
// `def` is kept so a caller can name the stage without looking the skill up
// again. It points into the registry, which is stable for its lifetime.
struct SkillGain {
  std::string skillId;
  const SkillDef *def = nullptr;
  SkillProgress progress;
};

// True when this skill may be trained by this activity.
//
// "*" is the deliberate wildcard, matching the effect links: a skill that any
// kind of work trains has to say so rather than leaving `trainedBy` empty, so an
// omission cannot silently make a skill untrainable.
inline bool AcceptsActivity(const SkillDef &def, const std::string &activity) {
  for (const std::string &allowed : def.trainedBy) {
    if (allowed == activity || allowed == "*") {
      return true;
    }
  }
  return false;
}

// True when this grant would actually reach `block`: the skill exists, it
// accepts this activity, and the character has it. The three cases are skipped
// rather than reported (see ApplyTraining), and this predicate is shared with
// the reporting path so "what was awarded" and "what was shown" cannot drift --
// a number must never appear over a character for XP that was silently dropped.
inline bool GrantApplies(const SkillBlock &block, const SkillDef *def,
                         const std::string &activity) {
  if (!def || !AcceptsActivity(*def, activity)) {
    return false;
  }
  for (const SkillValue &value : block.values) {
    if (value.id == def->id) {
      return true;
    }
  }
  return false;
}

// Applies a subject's training grants to a character's SkillBlock, in place.
//
// Returns ONLY the skills that moved, so a caller can report those and ignore
// the rest. PURE: it touches a SkillBlock and a registry and nothing else -- no
// world, no clock -- which is what keeps the whole progression model testable
// without flecs.
//
// Three cases are skipped rather than reported:
//   * an unknown skill id, or one that does not accept this activity. Both are
//     content errors, and the loader reports them once at startup rather than on
//     every harvest.
//   * a skill the character does not have. That is deliberate and load-bearing:
//     a character with no `blacksmithing` entry cannot be trained in it, which is
//     the same "absent means absent" rule a race with a different skill set
//     relies on.
inline std::vector<SkillGain> ApplyTraining(SkillBlock &block,
                                            const std::vector<SkillXp> &grants,
                                            const SkillRegistry &registry,
                                            const std::string &activity) {
  std::vector<SkillGain> gains;

  for (const SkillXp &grant : grants) {
    if (grant.xp <= 0) {
      continue;
    }
    const SkillDef *def = registry.Get(grant.skillId);
    if (!GrantApplies(block, def, activity)) {
      continue;
    }

    for (SkillValue &value : block.values) {
      if (value.id != grant.skillId) {
        continue;
      }

      const SkillProgress progress =
          GrantSkillXp(*def, value.level, value.xp, grant.xp);
      value.level = progress.level;
      value.xp = progress.xp;

      if (progress.levelsGained > 0) {
        SkillGain gain;
        gain.skillId = def->id;
        gain.def = def;
        gain.progress = progress;
        gains.push_back(std::move(gain));
      }
      break;
    }
  }

  return gains;
}

// The lines appended to the result of the action that earned them.
//
// A stage change is announced as a stage, a mere level as progress within the
// current one, because the stage is the part that carries capability.
//
// These ride the action's own message rather than being sent separately, and that
// is deliberate: the agent is already being woken for the harvest or the craft, so
// reporting the progression costs no extra request. That is what makes it safe to
// report every level instead of only stage changes (section 9.4) -- the rule there
// was about not waking the model, not about staying silent.
inline std::string FormatTrainingGains(const std::vector<SkillGain> &gains) {
  std::string text;

  for (const SkillGain &gain : gains) {
    if (!gain.def) {
      continue;
    }
    const int stage = gain.def->StageForLevel(gain.progress.level);
    const int within = gain.def->LevelWithinStage(gain.progress.level);

    if (gain.progress.stagesGained > 0) {
      text += "System: Your " + gain.def->name + " skill advanced to " +
              gain.def->StageName(stage) + " (" + std::to_string(within) + "/" +
              std::to_string(gain.def->levelsPerStage) + ").\n";
    } else {
      text += "System: Your " + gain.def->name + " skill improved to " +
              gain.def->StageName(stage) + " " + std::to_string(within) + "/" +
              std::to_string(gain.def->levelsPerStage) + ".\n";
    }
  }

  return text;
}
