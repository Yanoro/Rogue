#pragma once

#include <string>

// One possible drop: an item type and the probability it appears. Reused by
// harvest loot tables and by crafting outputs, so a byproduct can carry a chance
// the same way a harvest drop does.
//
// Kept out of Components.h, like ItemStack.hpp and StatDef.hpp, so the loot
// resolution code and its tests do not have to pull in flecs and raylib to name
// an item.
struct LootDrop {
  std::string itemType;
  float chance;
};
