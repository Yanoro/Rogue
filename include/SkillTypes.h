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
