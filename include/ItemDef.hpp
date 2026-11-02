#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "SourceScaling.hpp"

// Definition of one equippable item, loaded from data/items/<id>.json.
//
// The file stem is the id, exactly like StatRegistry/SkillRegistry and
// ObjectFactory templates, and it is the SAME id as the object template the item
// spawns from: data/items/iron_scythe.json describes the thing
// data/objects/iron_scythe.json spawns. The object template says what it looks
// like in the world; this file says what it does when worn.
//
// An item is a source like a stat or a skill, and its magnitude is its `tier`:
// at tier 1 with baseline 0 and scale 1 the deviation is 1, so a link's
// `perPoint` reads as the literal contribution ("flat -1.0" is one second off)
// and a tier-2 item doubles every link it declares. That is the whole reason
// item effects need no new machinery in the resolvers.
//
// `slot` is the KIND the item needs, never an instance: a "hand" item fits
// whichever hand slot the character actually has, which is what lets a race with
// tentacles use a completely different body with no change here.
struct ItemDef {
  std::string id;
  // Optional. Empty means fall back to the object template's display name, so a
  // definition that only adds mechanics does not repeat the name.
  std::string name;
  // Required slot kind, e.g. "hand", "head", "grasp". Empty is reported at load.
  std::string slot;
  // How many instances of that kind the item takes. 2 is a two-handed weapon.
  int occupies = 1;
  // Free-form capability tags ("scythe", "pickaxe", "light"). An object can
  // require one (see Components.h Requires), which is how "you need a scythe to
  // cut wheat" stays content rather than a hardcoded item id: any future item
  // that carries the tag satisfies it.
  std::vector<std::string> tags;
  // Maximum durability, and 0 meaning the item never wears out. Durability is
  // opt-in: an item definition without it lasts forever.
  int durability = 0;
  // Activities that consume this item's durability, e.g. ["harvest"]. Empty
  // means the item is never consumed by being used, however long it is worn.
  //
  // This is what "the equipment you used" resolves to: an item is a tool for an
  // activity because its data says so, not because of which slot it sits in or
  // which effects happened to fire.
  std::vector<std::string> consumedBy;
  // The magnitude the item's links scale by. See the class comment.
  float tier = 1.0f;
  // Effective units per tier, so a coefficient stays readable if a future item
  // kind wants a different range. 1 means "per tier" plainly.
  float scale = 1.0f;
  float baseline = 0.0f;

  // What this item hits for as a weapon, and how long one swing takes.
  //
  // Deliberately plain fields rather than an effect link: base damage is the
  // weapon's own number, not a contribution to a fold. Stats, skills and the
  // weapon's tier still reach the result through the ordinary damage fold, so
  // this only has to say what the object is worth before anything modifies it.
  // An item with damage 0 is simply not a weapon.
  float damage = 0.0f;
  // Seconds between swings. The duration fold still applies on top (haste), so
  // a data value of 1.8 is the weapon's unhasted tempo.
  float swingTime = 1.8f;

  // Flat damage reduction per hit while worn. 0 means not armour. Summed with
  // any innate Armor the character or object carries, then subtracted from each
  // hit and floored at one point -- see ArmorOf.
  float armor = 0.0f;

  // How the item looks worn by a *rigged* actor: a sprite convention id drawn over
  // the rig, its draw order among the worn layers, and the rig parts it covers
  // (`hides`, e.g. a helmet hiding the hair). Empty means the item has no worn art,
  // which is the normal state for everything that is not worn on a body.
  //
  // Separate from the object template's sprite on purpose: a sword lying on the
  // ground and a sword in a hand are different pictures.
  std::string wornSprite;
  // How the item looks worn, as authored. Kept so a later regeneration has the design
  // intent to redraw from, rather than only a name.
  std::string wornBrief;
  int wornZ = 0;
  std::vector<std::string> wornHides;

  // The "effects" array exactly as authored. The typed definition does not model it --
  // ParseEffectEntries folds it into the registry's DeclaredEffects -- so this is kept for
  // the Content Editor, which must write back an item it did not change without silently
  // dropping the effects it does not understand.
  nlohmann::json effectsJson;

  // The same shape a StatDef and a SkillDef provide, so one contribution builder
  // serves all three.
  SourceScaling Scaling() const {
    SourceScaling scaling;
    scaling.baseline = baseline;
    scaling.scale = scale;
    scaling.min = 0.0f;
    // Items have no natural cap; the field exists because SourceScaling is the
    // shared shape the contribution math takes (only BaselineMode::Min reads
    // min, and no threshold is meaningful for a worn object).
    scaling.max = 1.0e9f;
    return scaling;
  }
};
