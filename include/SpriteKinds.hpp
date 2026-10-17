#pragma once

#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

// The kinds of sprite a designer agent can create, and everything about a kind that
// is not the validation itself.
//
// Why a table. Two kinds are being built now -- a tile and an object -- and more are
// plausible later (an item, a piece of decor). The failure this exists to prevent is
// a boolean `isObject` threaded through the window, the prompt builder, the validator
// and the art queue, which is how a system ends up with four places that disagree
// about what an object is. With a descriptor, a new kind is a row and a prompt file.
//
// What a descriptor deliberately does NOT hold is a validator. Validation differs per
// kind in a way a function pointer would hide badly: a tile is refused for a
// two-character glyph, an object for a component the engine cannot spawn, and both
// have to name *which* field failed in a sentence fed straight back to the model. So
// each kind keeps its own validator in its own file and the headless mode dispatches
// to it by kind. The descriptor holds the prose, the paths and the names, which is the
// part that is genuinely shared.
//
// This is phase 1 of docs/sprite-designer-design.md: the prompt side and the registry.
// The object validator, the runtime object registration and the classifier are later
// phases, so `Kind::Object` is currently buildable as a prompt and not yet acceptable
// as a definition.
namespace SpriteKinds {

enum class Kind {
  Tile,
  Object,
};

struct Descriptor {
  // What this kind is, for code that has to branch on it. Everything else here is
  // data, so a caller that can avoid looking at `kind` should.
  Kind kind;

  // The classifier's answer key, and the spelling the CLI uses: `tile`, `object`. It
  // is separate from `label` because one is matched and the other is read aloud.
  std::string id;

  // What a window shows a person: `Tile`, `Object`.
  std::string label;

  // The noun for prose and refusals: "a tile", "an object". Kept as the bare noun so
  // a sentence can put its own article in front of it.
  std::string noun;

  // The instruction document for this kind. It contains the shared reply contract by
  // reference (a `%CONTRACT%` placeholder) plus the sections specific to the kind.
  std::string promptFile;

  // The settings key that holds `promptFile`, so a caller can report *which* setting
  // is missing a file rather than just a path. Empty for a kind with no override.
  std::string promptSettingKey;

  // The generated-overlay directory an accepted definition is written to and read
  // from, and the settings key that holds it. The key is carried so the window can
  // say "nothing will be saved, `content.generatedObjects` is empty" instead of
  // silently accepting a definition that vanishes on restart.
  std::string overlayDirectory;
  std::string overlaySettingKey;

  // The heading over the vocabulary appended to the prompt when it has no
  // `%VOCAB%` placeholder, and the note used when the vocabulary is empty. Both are
  // per kind because a tile list and a component list are not the same kind of thing
  // to read even though they arrive through the same placeholder.
  std::string vocabularyHeading;
  std::string emptyVocabularyNote;
};

// Every kind, in the order a dropdown should offer them.
const std::vector<Descriptor> &All();

// The descriptor for a kind. Returns a reference into `All()`, so it is stable for
// the run.
const Descriptor &For(Kind kind);

// The descriptor for an id ("tile", "object"), or null when the id is unknown. Null
// rather than a default, because "the classifier said something we do not have" and
// "the classifier said tile" must not read the same.
const Descriptor *Parse(const std::string &id);

// The id as it is spelled in the table, for logging. Empty for an unknown kind.
std::string IdOf(Kind kind);

// The sprite convention id for a name of this kind: the name the *sprite file* is called,
// which is not the same as the name the definition is called for tiles only.
//
//   tile   "tilled soil"  ->  "tile_tilled_soil"   (TileSpriteId)
//   object "wooden crate" ->  "wooden_crate"       (the object template key)
//
// This is the one place the per-kind naming rule lives, and it exists because the rule
// genuinely differs. `TileSpriteId` prefixes "tile_" for a reason its own comment gives:
// tile names are ordinary words -- "floor", "path", "water" -- and an object template can
// take any of them, so a bare `water.png` would be ambiguous between the two systems. An
// object therefore needs the *bare* key and a tile needs the prefix, and a caller that
// hardcodes either one will write a file the other system never looks for.
//
// A tile's id comes from `TileSpriteId`; an object's from the template key, which its
// registry derives from the name. Both are called here rather than reimplemented.
std::string ConventionIdFor(Kind kind, const std::string &name);

}  // namespace SpriteKinds
