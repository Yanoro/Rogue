#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

// Turning a designer reply into worn equipment.
//
// Scope, and why it is narrow: this generates the *item definition* and its **worn** art --
// how it looks on a rigged body. The world object template (what it looks like lying in a
// room, and the components that let it spawn) belongs to the object designer's format and
// is not written here. A generated item can be worn by a body in a preview; it cannot be
// placed in a room until that half exists.
//
// One reply may define one item or a themed set of them (an armour and its weapon, a whole
// outfit). The set form is an `items` envelope, the same convention the sprite designer
// uses for a sheet, and each entry is validated on its own.
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
  // For a hand item: where the hand holds the worn art (in the 32x32 frame) and the
  // item's rest orientation relative to the arm. The grip path uses these.
  int gripX = 24;
  int gripY = 22;
  float gripAngle = 0.0f;

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

// One reply's items. A reply may define one item or a themed set of them; both are kept in
// this one shape so the window's pane, art queue and save path have one thing to handle.
struct SetDraft {
  // The envelope's `set` name. Empty for a single-item reply, and allowed to be empty for a
  // set: the name is cosmetic and is what the pane shows.
  std::string name;
  std::vector<Draft> items;
};

// The many-item form, which is also the single-item form: a bare item object is accepted
// and comes back as a one-entry set, so callers do not branch on the shape of the reply.
struct SetApplyResult {
  // False only when the reply itself could not be read (no fence, bad JSON, a malformed
  // envelope). A readable reply whose entries were all refused has `ok == true` and a
  // failure per entry, because that is a retryable attempt, not an unreadable one.
  bool ok = false;
  std::string error;

  // True when the reply used the `{ "set": ..., "items": [...] }` envelope.
  bool isSet = false;
  std::string setName;

  // One result per entry, in the order the reply gave them. Each is validated on its own:
  // a refused entry does not roll back an accepted one, so a set of five with one bad slot
  // still leaves four usable drafts and names the entry to fix.
  std::vector<ApplyResult> items;
  // Counters, so the window and the log do not recount and risk disagreeing.
  size_t accepted = 0;
  size_t refused = 0;
};

// Takes the raw reply text, validates it and builds the item documents. Never writes.
//
// Accepts both reply shapes: a single item object (every existing reply and file), and the
// set envelope whose `items` array holds a small, bounded number of complete item objects.
// The envelope's `set` name is added as a tag to every accepted item, which is what lets a
// set be found again later.
SetApplyResult ApplySetReply(const std::string &reply);

// The instruction file for the definition turn. `set` selects the single-item or the
// themed-set section of the document, the same shape-fence convention the sprite prompts
// use; a document with no fences is passed through unchanged.
std::string BuildSystemPrompt(const std::string &promptFile, bool set = false);

// The worn-art request text for this draft: the shared rig frame plus this item's brief,
// hides and slot-specific placement.
std::string WornArtRequirement(const Draft &draft);

std::string SlugFor(const std::string &name);

// Writes <directory>/<slug>.json, replacing a generated file of the same name. The
// hand-authored data/items catalog is never the target.
bool Save(const Draft &draft, const std::string &directory, std::string *savedPath,
          std::string *error);

}  // namespace EquipmentDesigner
