#pragma once

#include <string>
#include <vector>

#include "EquipmentTypes.h"

// Definition of one people, loaded from data/races/<id>.json.
//
// For now a race does almost nothing on purpose: it supplies the body slots a
// character is born with, which is what makes "an NPC's slots, or its race's
// slots" a data question rather than an engine one. A race with tentacles is a
// file listing tentacle instances that accept "grasp"; no code compares slot
// names.
//
// Stats and named effects will attach here later -- the design record already
// reserves Race as a source kind -- but slots come first because they are what
// equipment needs, and adding them later costs nothing.
struct RaceDef {
  std::string id;
  std::string name;
  std::vector<SlotSpec> slots;
};
