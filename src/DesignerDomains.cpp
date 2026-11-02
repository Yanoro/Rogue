#include "DesignerDomains.h"

namespace DesignerDomains {

const std::vector<Descriptor> &All() {
  // Built on first use, not at namespace scope: the sprite rows call SpriteKinds::For(),
  // and the order two translation units' globals are initialised in is unspecified. A
  // function-local static is initialised on the first call, which is always after both
  // tables are readable.
  static const std::vector<Descriptor> kAll = {
      {
          // The building. No sprite row, because a blueprint is not drawn as a sprite:
          // its cells are painted with *tiles* and filled with *objects*, which is why a
          // building's design decides the other two domains rather than competing with
          // them.
          Domain::Blueprint,
          "building",
          "Building",
          "building",
          "data/blueprints/DESIGNER.md",
          "designer.promptFile",
          nullptr,
      },
      {
          Domain::Tile,
          "tile",
          "Tile",
          "tile",
          "data/tiles/TILEDESIGNER.md",
          "content.tilePromptFile",
          &SpriteKinds::For(SpriteKinds::Kind::Tile),
      },
      {
          Domain::Object,
          "object",
          "Object",
          "object",
          "data/objects/OBJECTDESIGNER.md",
          "content.objectPromptFile",
          &SpriteKinds::For(SpriteKinds::Kind::Object),
      },
      {
          // A map. No sprite row: a map is drawn from its tiles, which is why its
          // design chooses them instead of competing with them.
          Domain::Map,
          "map",
          "Map",
          "a map",
          "maps/MAPDESIGNER.md",
          "content.mapPromptFile",
          nullptr,
      },
      {
          Domain::ActorSprite,
          "actor_sprite",
          "Actor sprite",
          "an actor sprite",
          // No definition prompt file and no sprite row: this mode does not produce a
          // definition. Send routes it to the sprite-document conversation, which is
          // configured by content.spriteDSLPromptFile like every other sprite document.
          "",
          "",
          nullptr,
      },
  };
  return kAll;
}

const Descriptor &For(Domain domain) {
  for (const Descriptor &descriptor : All()) {
    if (descriptor.domain == domain) return descriptor;
  }
  // Unreachable while `Domain` and the table agree. Returning the first row rather than
  // throwing keeps a missing row from taking the game down; a caller that needs to notice
  // compares `descriptor.domain` with what it asked for.
  return All().front();
}

const Descriptor *Parse(const std::string &id) {
  for (const Descriptor &descriptor : All()) {
    if (descriptor.id == id) return &descriptor;
  }
  return nullptr;
}

bool IsSprite(Domain domain) { return For(domain).sprite != nullptr; }

SpriteKinds::Kind SpriteOf(Domain domain) {
  const Descriptor &descriptor = For(domain);
  if (descriptor.sprite == nullptr) {
    // A building has no sprite kind. Answering Tile rather than dereferencing a null keeps
    // the failure in the output (a tile-shaped prompt for a building request) instead of in
    // the process; every caller is expected to have tested IsSprite first.
    return SpriteKinds::Kind::Tile;
  }
  return descriptor.sprite->kind;
}

}  // namespace DesignerDomains
