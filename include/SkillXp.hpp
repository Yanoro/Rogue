#pragma once

#include <string>

// How much of one skill a subject teaches: "crafting this grants 8 blacksmithing".
//
// It lives on the SUBJECT -- a recipe, a harvestable object -- rather than on the
// skill, because the amount is a fact about the work and belongs next to the
// thing being made. That placement is also what makes the blacksmithing/bread
// problem impossible: bread.json names cooking and never mentions blacksmithing,
// so there is no matching step to get wrong.
//
// The skill's own `trainedBy` list is a GUARD rather than a selector. It does not
// decide who trains what; it lets the loader report "this recipe claims to train
// something that crafting cannot train" at startup, instead of silently granting
// nothing on every single craft.
//
// Dependency-free, like LootDrop.hpp and ItemStack.hpp, so recipe and template
// parsing and their tests do not need the rendering stack.
struct SkillXp {
  std::string skillId;
  int xp = 0;
};
