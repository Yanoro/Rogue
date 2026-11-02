#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

// Turning a designer reply into a piece of worn equipment.
//
// Scope, and why it is narrow: this generates the *item definition* and its **worn** art --
// how it looks on a rigged body. The world object template (what it looks like lying in a
// room, and the components that let it spawn) belongs to the object designer's format and
// is not written here. A generated item can be worn by a body in a preview; it cannot be
// placed in a room until that half exists.
//
// Slots are limited to the ones a rig can show -- head, torso, legs, feet, hand -- because
// an item whose slot has no worn layer would be a definition with no picture, and this
// pass is about the picture.
namespace EquipmentDesigner {

// The slots a rig can wear. A helmet, a cuirass, trousers, boots, and a held weapon.
inline const char *const kSlots[] = {"head", "torso", "legs", "feet", "hand"};

struct Draft {
  std::string name;
  std::string slug;
  std::string description;
  std::string designerNotes;

  std::string slot;
  float tier = 1.0f;
  float armor = 0.0f;
  std::vector<std::string> tags;

  // The worn appearance: a sprite id (worn_<slug>), the art brief, its draw order and
  // which rig parts it covers. "hair" under a helmet is the canonical case.
  std::string wornSprite;
  std::string wornBrief;
  int wornZ = 0;
  std::vector<std::string> hides;

  // The weapon half, meaningful only for a hand item. Zero damage means "not a weapon",
  // which a tool can legitimately be.
  float damage = 0.0f;
  float swingTime = 1.8f;
  int durability = 0;

  std::vector<std::string> warnings;
  nlohmann::json document;
};

struct ApplyResult {
  bool ok = false;
  // The sentence fed back to the model for a retry.
  std::string error;
  Draft draft;
  std::string notes;
};

// Takes the raw reply text, validates it and builds the item document. Never writes.
ApplyResult ApplyReply(const std::string &reply);

// The instruction file for the definition turn.
std::string BuildSystemPrompt(const std::string &promptFile);

// The worn-art request text for this draft: the shared rig frame plus this item's brief,
// hides and slot-specific placement.
std::string WornArtRequirement(const Draft &draft);

std::string SlugFor(const std::string &name);

// Writes <directory>/<slug>.json, replacing a generated file of the same name. The
// hand-authored data/items catalog is never the target.
bool Save(const Draft &draft, const std::string &directory, std::string *savedPath,
          std::string *error);

}  // namespace EquipmentDesigner
