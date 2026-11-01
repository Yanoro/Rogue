#pragma once

#include <string>
#include <vector>

// One skill a character has: how far along it is and the progress toward the
// next level.
//
// `level` is the ONLY progression fact stored. The stage and the position within
// it are derived from it (see SkillDef), so there is no second field to keep in
// sync and no way to be "level 12 but still listed as Initiate".
//
// Kept free of flecs and raylib, like StatTypes.h and ItemStack.hpp, so the
// progression rules and their test stay independent of the game's rendering
// stack.
struct SkillValue {
  std::string id;
  // 0 is untrained, up to SkillDef::MaxLevel().
  int level = 0;
  // Progress toward level + 1. Always 0 at the cap: there is nothing left to
  // progress toward.
  int xp = 0;
};

// The skills a character has. Same shape as StatBlock, and a vector for the same
// reason: one reflection helper already exists for it.
//
// A character with no entry for a skill is untrained in it, which is neutral in
// exactly the way a missing stat is (section 3.4).
struct SkillBlock {
  std::vector<SkillValue> values;
};

// The toggle skills currently switched on, by definition id.
//
// Kept beside SkillBlock rather than inside SkillValue because it is not
// progression state: a toggle's level is a magnitude like any other skill's,
// while whether it is on is a switch the character can flip at any time. A
// toggle that is off contributes no source at all (see GatherSources), which is
// exactly the difference between a stance and a passive.
struct ActiveToggles {
  std::vector<std::string> ids;

  bool Has(const std::string &id) const {
    for (const std::string &entry : ids) {
      if (entry == id) {
        return true;
      }
    }
    return false;
  }
};
