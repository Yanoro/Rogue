#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace SpriteReply {

// One reply may define one sprite, or a set of them. This is the small piece that tells the
// two apart and takes them apart, shared by both kinds.
//
// The contract (data/prompts/SPRITE_CONTRACT.md) is a bare definition object for one sprite
// and an envelope with a `sprites` array for a set:
//
//   { "name": "clay brick floor", "character": "b", "designerNotes": "..." }
//
//   { "set": "prison", "sprites": [ { ... }, { ... } ] }
//
// Why an envelope rather than "a bare array or an object": a model given two acceptable
// shapes picks the wrong one about half the time, and an envelope has a named field to look
// for, so a reply that is neither is distinguishable from one that is merely malformed. It
// also gives the set a name, which the window shows and the log records.
//
// Backwards compatibility is the point of it *not* being an array at the top level: every
// existing reply, every file on disk, and every `--tile-design` fixture is a bare object and
// keeps working untouched.
struct Parsed {
  // False when `error` says why. A parse failure is a refusal fed back to the model, not an
  // exception: the conversation retries on refusals.
  bool ok = false;
  std::string error;

  // True when the reply used the envelope. False for a bare definition, which is the
  // single-sprite case and still the common one.
  bool isSet = false;
  // The group's name from the envelope's `set` field. Empty for a single, and empty for a set
  // that did not name itself -- which is allowed, because the name is cosmetic.
  std::string setName;

  // One entry per definition, in the order the reply gave them. Exactly one when `isSet` is
  // false. Each entry still has to be validated by its own kind's validator: this function
  // only decides *how many* there are and whether the envelope itself is well formed.
  std::vector<nlohmann::json> definitions;
};

// Splits one already-parsed json document into its definitions.
//
// `json` is the document, not the reply text: the caller has already stripped any reasoning
// and pulled the fenced block, because both kinds do that identically and this function has
// no business knowing about fences.
//
// Refuses -- rather than silently taking the first -- a `sprites` value that is not an array,
// an empty `sprites` array, and an envelope whose entries are not objects. Every one of those
// is a reply the model can fix on the next attempt if it is told what was wrong.
Parsed Split(const nlohmann::json &json);

// The same thing from raw text, for a caller that only has a string.
Parsed SplitText(const std::string &jsonText);

}  // namespace SpriteReply
