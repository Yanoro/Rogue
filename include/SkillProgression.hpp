#pragma once

#include "SkillDef.hpp"

// What happened when XP was granted.
//
// `levelsGained` and `stagesGained` are separate on purpose. A stage change is
// the event worth telling the agent about -- it is where new capabilities appear
// -- while a level is a progress tick whose numeric gain is far too small for an
// LLM to notice. One award can cross both, so the caller needs to know which.
struct SkillProgress {
  int level = 0;
  int xp = 0;
  int levelsGained = 0;
  int stagesGained = 0;
  // XP arrived while already at the maximum level, so it was discarded rather
  // than banked toward a level that does not exist.
  bool capped = false;
};

// Grants `amount` XP to a skill at `level` with `xp` progress already banked, and
// resolves every level -- and therefore every stage -- that the award crosses.
//
// A single award can cross several levels and even a stage boundary, which is why
// this loops instead of adding once and comparing: a caller doing that by hand
// gets the boundary wrong, and the boundary is the entire point of the design.
//
// Pure: no registry, no world, no clock. The whole progression model is testable
// without flecs.
inline SkillProgress GrantSkillXp(const SkillDef &def, int level, int xp,
                                  int amount) {
  SkillProgress result;

  const int maxLevel = def.MaxLevel();
  if (maxLevel <= 0) {
    // A skill with no stages cannot progress; treat it as unusable rather than
    // letting callers accumulate XP against nothing.
    result.capped = true;
    return result;
  }

  result.level = level < 0 ? 0 : (level > maxLevel ? maxLevel : level);
  result.xp = xp < 0 ? 0 : xp;

  if (result.level >= maxLevel) {
    result.level = maxLevel;
    result.xp = 0;
    result.capped = true;
    return result;
  }

  if (amount <= 0) {
    return result;
  }

  result.xp += amount;

  while (result.level < maxLevel) {
    const int needed = def.XpForNextLevel(result.level);
    if (needed <= 0 || result.xp < needed) {
      break;
    }
    result.xp -= needed;

    const int stageBefore = def.StageForLevel(result.level);
    ++result.level;
    ++result.levelsGained;
    if (def.StageForLevel(result.level) != stageBefore) {
      ++result.stagesGained;
    }
  }

  if (result.level >= maxLevel) {
    // At the cap there is nothing left to progress toward, so XP stops
    // accumulating rather than banking toward a level that cannot be reached.
    result.level = maxLevel;
    result.capped = true;
    result.xp = 0;
  }

  return result;
}
