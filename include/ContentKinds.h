#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

class Game;
class TileRegistry;
class ObjectFactory;
class DebugLog;
class ActorSpriteRegistry;

// The kinds of content the Content Editor can open: one row per registry.
//
// Why a table rather than a window per registry. Everything the editor does is registry
// work -- list the entries, hand me one as JSON, validate this JSON, remember where its
// override lives -- and none of it is specific to tiles or objects. A new registry is
// therefore a row with four functions, not a new window with a copy of the form, the list
// and the save path. Tiles and objects are simply the first two rows.
//
// What a kind does NOT get is its own editing UI: the editor draws one generic form over
// whatever JSON the document happens to be. A kind that later needs a richer editor (the
// obvious one is a blueprint, where a sentence to a model is a better control than a list
// of parts) gets a `customPanel` hook and keeps the generic form as the fallback.
namespace ContentKinds {

// What a kind needs from the world: the two registries and the log.
//
// A struct rather than a `Game&` because the registries are private members of Game and
// these functions are not its friends -- and because a kind that only needs the tile
// registry should not be handed the whole game. The window, which is a friend, builds
// one of these and passes it down.
struct World {
  TileRegistry &tiles;
  ObjectFactory &objects;
  ActorSpriteRegistry &actorSprites;
  DebugLog *log = nullptr;
};

// One entry in a kind's list.
struct Entry {
  // What the registry keys it by: a tile's name, an object's template key. This is what
  // `read` takes back and what the override file is named after.
  std::string id;
  // What a person reads. Often the same as `id`; an object's display name differs.
  std::string label;
  // Where the live definition came from, for the list: the catalog file, or a generated
  // overlay, or the editor. Purely informational, and it is the fastest answer to "why
  // does this look different from the file I edited".
  std::string origin;
};

struct Kind {
  // Directory name and dropdown id: "tiles", "objects". Also the subdirectory of
  // `content.editor` this kind's overrides live in.
  std::string id;
  std::string label;

  // False for a kind with no sprite of its own (a blueprint), which decides whether the
  // window shows the art pane and the regenerate button at all.
  bool hasSprites = false;

  std::vector<Entry> (*list)(const World &) = nullptr;

  // The document as a whole, ready to edit. A full definition, not a patch: the editor
  // always saves what it holds, which is what makes one generic form enough.
  nlohmann::json (*read)(const World &, const std::string &id) = nullptr;

  // Validates and applies a document to the live registry -- no file write. The editor
  // calls this on save, after writing the file, so "what is on disk" and "what the world
  // is using" cannot disagree.
  bool (*apply)(const World &, const nlohmann::json &document, std::string *error,
                std::vector<std::string> *warnings) = nullptr;

  // The file stem an override is saved under, derived from the document rather than
  // from the selection: renaming an asset renames its override, and a file named after
  // the id the user opened would be a second definition under the old name.
  std::string (*fileName)(const nlohmann::json &document) = nullptr;

  // The sprite convention id for the preview: `tile_<slug>` for a tile, the template key
  // for an object. Empty when the kind has no sprite.
  std::string (*spriteId)(const std::string &id) = nullptr;
};

// Writes `document` as this kind's override under `root/<kind>/<file>.json`, without
// touching the live registry. The window calls this after the registry has accepted the
// document; the round-trip test calls it before Init so the loader can be the thing that
// reads it back. One writer either way, so the test proves the code the window runs.
bool WriteOverrideFile(const Kind &kind, const std::string &root,
                       const nlohmann::json &document, std::string *path,
                       std::string *error);

// Every kind the editor offers, in dropdown order.
const std::vector<Kind> &All();

// The kind for an id ("tiles"), or null.
const Kind *For(const std::string &id);

}  // namespace ContentKinds
