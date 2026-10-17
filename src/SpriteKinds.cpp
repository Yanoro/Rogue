#include "SpriteKinds.hpp"

#include "ObjectFactory.h"
#include "SpritePaths.hpp"

namespace SpriteKinds {

namespace {

// The table. One row per kind, and everything a new kind needs except its validator
// and its prompt document.
//
// The paths here are the *defaults*; both are overridable through the settings keys
// named beside them, which is why the key is carried alongside the value. A caller
// that needs the live value reads the setting and falls back to this.
const std::vector<Descriptor> kAll = {
    {
        Kind::Tile,
        "tile",
        "Tile",
        "tile",
        "data/tiles/TILEDESIGNER.md",
        "content.tileDesignerPromptFile",
        // Deliberately a sibling of data/tiles rather than a subdirectory of it:
        // TileRegistry::LoadTiles reads every .json in one directory and is not
        // recursive, so a nested overlay would either be picked up as catalog (which
        // would defeat the split) or never be read at all.
        "data/generated/tiles",
        "content.generatedTiles",
        "## Tiles that already exist",
        "  (none: no tile catalog is loaded)",
    },
    {
        Kind::Object,
        "object",
        "Object",
        "object",
        "data/objects/OBJECTDESIGNER.md",
        "content.objectDesignerPromptFile",
        // The same sibling-of-the-catalog arrangement as tiles, for the same reason:
        // ObjectFactory::LoadTemplates reads one directory and is not recursive.
        "data/generated/objects",
        "content.generatedObjects",
        "## Object components the engine can spawn",
        "  (none: the object factory registered no components)",
    },
};

}  // namespace

const std::vector<Descriptor> &All() { return kAll; }

const Descriptor &For(Kind kind) {
  for (const Descriptor &descriptor : kAll) {
    if (descriptor.kind == kind) return descriptor;
  }
  // Unreachable while `Kind` and the table agree, which is checked by
  // --sprite-design over every kind. Returning the first row rather than throwing
  // keeps a missing row from taking the game down; the id check below is what makes
  // the mismatch loud in the place it can be observed.
  return kAll.front();
}

const Descriptor *Parse(const std::string &id) {
  for (const Descriptor &descriptor : kAll) {
    if (descriptor.id == id) return &descriptor;
  }
  return nullptr;
}

std::string IdOf(Kind kind) { return For(kind).id; }

std::string ConventionIdFor(Kind kind, const std::string &name) {
  if (name.empty()) return {};
  // The object key is derived from a definition, so it is wrapped and unwrapped here
  // rather than duplicating the slug rule. One extra JSON object per call, on a path that
  // runs once per accepted definition -- not per frame.
  if (kind == Kind::Object) {
    nlohmann::json wrapper = nlohmann::json::object();
    wrapper["name"] = name;
    return ObjectFactory::KeyFor(wrapper);
  }
  return TileSpriteId(name);
}

}  // namespace SpriteKinds
