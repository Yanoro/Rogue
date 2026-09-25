#pragma once

#include <string>
#include <vector>

// The equipment slot vocabulary.
//
// A slot has an INSTANCE id and a KIND it accepts: { "off_hand", "hand" }.
// An item declares only the kind it needs ("hand"), so one definition fits
// either hand, and a people with three tentacles simply declares three instances
// that all accept the kind "grasp". The engine never knows a slot name -- it
// compares strings, exactly the way activities and skill stages are open sets
// declared in data.
//
// Both fields are content-facing and snake_case, like every other id.
struct SlotSpec {
  std::string id;
  std::string accepts;
};

// The positions a character actually has. Stamped at spawn from an authored map
// "slots" list, the character's race, or the default humanoid set below. Absent
// on an entity that has no body slots at all.
struct EquipmentSlots {
  std::vector<SlotSpec> slots;
};

// Which slot instances an equipped item occupies, by SlotSpec::id. Stored on the
// ITEM entity, so an item that is destroyed or traded away cannot leave a
// dangling slot behind: the state travels with the item.
//
// A single-slot item has one entry; a two-handed weapon has one per hand it
// takes. Occupancy is therefore checkable by plain string comparison.
struct Equipped {
  std::vector<std::string> slots;
};

// The wear state of one item instance. Present ONLY on items whose definition
// declares a durability: its absence is what "this item never wears out" means,
// so an indestructible item carries no state and needs no special case.
//
// `max` is copied from the definition at spawn so a listing can print "87/100"
// without consulting the registry, and so a future per-instance maximum (a
// masterwork tool) has somewhere to live.
struct Durability {
  int current = 0;
  int max = 0;
};

// The neutral humanoid body: what a character gets when neither its race nor the
// map says otherwise. Deliberately expressed in the same open vocabulary as any
// other slot set, so a tentacle race is a data file rather than a special case.
//
// `accepts` deliberately differs from `id` for hands and rings: the kinds are
// "hand" and "ring" because one item definition must fit either instance.
inline std::vector<SlotSpec> DefaultHumanoidSlots() {
  return {
      {"head", "head"},       {"torso", "torso"},   {"legs", "legs"},
      {"feet", "feet"},       {"main_hand", "hand"}, {"off_hand", "hand"},
      {"ring_1", "ring"},     {"ring_2", "ring"},   {"neck", "neck"},
      {"back", "back"},
  };
}
