#pragma once

#include <string>
#include <vector>

#include "EquipmentTypes.h"

// The small components that tie an object to an inventory and to equipment.
//
// This header is deliberately free of flecs and raylib, the same way
// StatTypes.h, SkillTypes.h and ItemStack.hpp are: the source gatherer reads
// these from an entity, and the gatherer is included by the resolution tests,
// which link flecs but not the rendering stack.

// Player-facing label, e.g. "Iron Scythe".
struct DisplayName {
  std::string name;
};

// Stable content identity of a spawned object: the ObjectFactory template key it
// was spawned from (e.g. "iron_scythe", "flour"). Recipes and equipment match on
// this, never on DisplayName, because the display name is player-facing text and
// is safe to rename. Stamped in ObjectFactory::ApplyTemplate.
struct ItemType {
  std::string id;
};

// Inventory relationship: holder.add<Holds>(item). An equipped item is still a
// Holds child of its character, which is what keeps [INVENTORY], [TAKE],
// [STORE] and trade seeing it with no special casing.
struct Holds {};
