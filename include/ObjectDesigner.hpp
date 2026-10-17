#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "ObjectFactory.h"

class DebugLog;
class TileRegistry;

// The object designer: the half of the sprite designer that turns a model's reply into
// an object the engine can actually spawn, with no window and no ImGui in the way.
//
// It is the object counterpart of TileDesigner, and it exists in its own file for the
// same reason: this repo has no test framework and cannot run the game headless, so
// anything reachable only through an ImGui Draw() is unreachable to verification. The
// window is the least testable part and the validator is the part that matters, so the
// grammar, the extraction and the verdict live here, and both `AIRogue --sprite-design
// object <file>` and the window call this one implementation. There is exactly one
// object validator.
//
// What it deliberately does NOT do, exactly like TileDesigner: generate art. A created
// object is a glyph, an optional ASCII art block and a set of capabilities for now; a
// sprite is hot-swapped in later. Nothing here produces an image.
//
// The validation itself is not in this file. It is `ObjectFactory::AddRuntime`, because
// the only honest way to check a definition is to run the setters that will load it
// (see the comment there). What lives here is the reply handling, the wiring to the
// overlay, and the verdict a window can show.
namespace ObjectDesigner {

// What one reply produced. `ok` is the verdict; everything else is why, or what to say
// about an object that was accepted anyway.
//
// Deliberately the same shape as TileDesigner::ApplyResult, field for field, including
// the fields a tile does not currently use. The window drives both kinds through one
// code path, and two result structs that differ only in spelling would force it to
// branch on kind for every field it reads.
struct ApplyResult {
  bool ok = false;
  // Blocking reason. Empty when ok. Worded to be fed straight back to the model.
  std::string error;
  // Non-blocking observations: a cross-kind name collision, an art block the parser
  // had to trim. Kept even on success.
  std::vector<std::string> warnings;
  // The accepted definition, so a preview can render it and the caller can write it.
  // Owned by value rather than pointing into a registry: an ObjectFactory owns its
  // templates by value, and a pointer into that map would be invalidated by the next
  // runtime add. TileDesigner can hand out a pointer because TileRegistry stores
  // unique_ptrs; this cannot, and that difference is the whole reason this is a copy.
  nlohmann::json definition;
  // The template key, which is also the sprite convention id (`wooden_crate` ->
  // wooden_crate.png). Empty on a failure that happened before the key was derived.
  std::string key;
  // The display name, copied out for the same reason as the key.
  std::string name;
  // Where the definition was written, when `overlayDirectory` was given. Empty when it
  // was only accepted in memory or when writing failed.
  std::string savedPath;
  // True when the write failed but the object was accepted. The caller decides whether
  // that is a refusal; this reports it rather than hiding it.
  bool saveFailed = false;
  // The model's own critique, pulled out of the reply and stripped before the
  // definition is written. Empty for ApplyJson, which has no reply to read it from.
  std::string notes;
  // True when the key already existed before this call.
  bool nameTaken = false;
  // True when this replaced an earlier *runtime* definition rather than creating a new
  // one. A revision is a normal thing to want -- "same crate, taller" -- and it is
  // reported rather than silently presented as a creation.
  bool revised = false;
  // A name used by the *other* kind -- a tile with this name, or an object with the
  // name of a tile. Not an error: the two registries are separate and the world has
  // shipped an 'anvil' that is both. Recorded because "which anvil do you mean" is a
  // real question an agent reading the world will ask.
  bool crossKindNameCollision = false;
};

// Pulls the object out of a reply: the fenced ```json block when there is one,
// otherwise the whole string.
//
// Shares TileDesigner::ExtractJsonBlock for the fence, because the contract is one
// contract -- the same reply shape is required for both kinds, so a second fence parser
// would be a second thing able to disagree about what a reply is.
std::string ExtractObjectJson(const std::string &text, bool *fromBlock = nullptr);

// Parses `json`, validates it through the real component setters, and adds it to
// `objects`. Writes it under `overlayDirectory` as <key>.json when that is non-empty.
//
// `tiles` is optional and used only to report a cross-kind name collision. Pass nullptr
// when there is no tile catalog to compare against.
//
// Not thread safe, and not meant to be: called on the main thread after a stream
// finishes, exactly like TileDesigner::ApplyReply.
ObjectDesigner::ApplyResult ApplyObject(const nlohmann::json &json,
                                        ObjectFactory &objects,
                                        const std::string &overlayDirectory,
                                        const TileRegistry *tiles = nullptr);

// Reads `path` and applies its contents, accepting either form ExtractObjectJson
// accepts. This is what `--sprite-design object` calls, so the mode cannot drift from
// the window about what a reply is.
ApplyResult ApplyFile(const std::string &path, ObjectFactory &objects,
                      const std::string &overlayDirectory,
                      const TileRegistry *tiles = nullptr);

// The same thing for a model *reply*: strips `<think>` reasoning, pulls the fenced json
// block, parses it, and applies it. This is what the sprite designer window calls once a
// stream finishes -- the parallel of TileDesigner::ApplyReply, and for the same reason:
// the window and the headless mode have to agree about what a reply is.
//
// An empty or unparseable reply is reported as a *refusal* with a reason worded to be sent
// back to the model, not as a transport error, because that is how the conversation
// retries.
ApplyResult ApplyReply(const std::string &reply, ObjectFactory &objects,
                       const std::string &overlayDirectory,
                       const TileRegistry *tiles = nullptr);

// A reply that may carry one object or a set of them. The object counterpart of
// TileDesigner::SetApplyResult, field for field and for the same reasons -- see the comment
// there. Mirrored rather than shared because the two carry their own ApplyResult, and
// unifying them would mean a variant whose only purpose is to make a template parameter out
// of something the window already branches on.
struct SetApplyResult {
  bool ok = false;
  std::string error;
  std::string setName;
  std::vector<ApplyResult> sprites;
  size_t accepted = 0;
  size_t refused = 0;
};

// Validates every entry on its own terms, keeps the ones that pass, and reports the index of
// each that did not. A refused entry does not roll back an accepted one.
SetApplyResult ApplySetReply(const std::string &reply, ObjectFactory &objects,
                             const std::string &overlayDirectory,
                             const TileRegistry *tiles = nullptr);

// The object definition as it is written to the overlay: designer-only keys stripped,
// the rest in a stable order, with a trailing newline. Field names must stay in sync
// with ObjectFactory's setters; it is the one thing both ends have to agree on.
nlohmann::json WriteObjectDef(const nlohmann::json &definition);

// Loads every *.json in `overlayDirectory` through the factory's runtime-add path with
// the "at least one component" rule relaxed -- the overlay is content, not a design in
// progress. Returns how many were added; malformed files are reported through `log` (or
// stderr) and skipped. A missing directory is not an error: most runs have no generated
// objects.
size_t LoadOverlayDirectory(const std::string &overlayDirectory,
                            ObjectFactory &objects, DebugLog *log);

// The object vocabulary as the prompt's `%VOCAB%` body: one component per line with its
// parameters, sorted. Generated from the factory rather than written into a document,
// because a hand-written list drifts the moment a component is added and the failure is
// quiet -- the designer writes a key the engine ignores.
std::string ComponentVocabulary(const ObjectFactory &objects);

}  // namespace ObjectDesigner
