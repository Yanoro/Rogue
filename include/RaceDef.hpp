#pragma once

#include <string>
#include <vector>

#include "AsciiArtTypes.hpp"
#include "EquipmentTypes.h"

// Definition of one people, loaded from data/races/<id>.json.
//
// For now a race does almost nothing on purpose: it supplies the body slots a
// character is born with, which is what makes "an NPC's slots, or its race's
// slots" a data question rather than an engine one. A race with tentacles is a
// file listing tentacle instances that accept "grasp"; no code compares slot
// names.
//
// `art` is the race's look, authored with the same "art"/"palette" keys an object
// template uses, so what a people looks like is content too. It is independent of
// `slots`: a race may supply either, both or neither, and a character whose race
// declares no art falls back to DefaultHumanoidArt() in ActorArt.hpp.
//
// `sprite` is the race's raster look, from a "sprite" key. When it resolves it is
// drawn instead of `art`, so the two are alternatives rather than layers: the art
// stays as the fallback for every character whose race has no sprite (see
// SpritePaths.hpp). A race with no "sprite" key still gets one for free if
// data/sprites/<id>.png exists, since the race id is also the convention id.
//
// Stats and named effects will attach here later -- the design record already
// reserves Race as a source kind -- but slots come first because they are what
// equipment needs, and adding them later costs nothing.
struct RaceDef {
  std::string id;
  std::string name;
  std::vector<SlotSpec> slots;
  AsciiArt art;
  std::string sprite;
};
