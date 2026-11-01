#pragma once

#include <string>
#include <vector>

#include "SpriteKinds.hpp"

// The kinds of thing a designer agent can create: a building, a tile, an object.
//
// Why this exists beside SpriteKinds rather than inside it. `SpriteKinds::Kind` is a
// *sprite* kind -- it carries the art convention (`tile_<slug>` versus `<slug>`), the
// generated-overlay directory and the vocabulary heading, and it is branched on 78 times
// across the tree. A building has none of those: it is not drawn as a sprite, it has no
// overlay, and there is no such thing as a building-shaped set. Adding it to that enum
// would leave every one of those branches quietly treating a building as a tile, and none
// of them is a `switch`, so the compiler would say nothing.
//
// So this table is the roof: a row per domain, with the sprite rows *pointing at* the
// SpriteKinds row rather than restating it. Every fact has exactly one home -- the
// display name of a tile comes from SpriteKinds, the fact that a building is not a sprite
// comes from `sprite == nullptr`.
namespace DesignerDomains {

enum class Domain {
  Blueprint,
  Tile,
  Object,
  // A being's sprite, with no definition turn behind it. The actor already exists (or will
  // be authored by hand); this mode exists so "draw me a baker" produces baker.png rather
  // than a JSON document. It has no sprite descriptor row and no overlay because it is not
  // a definition kind -- Send routes to the sprite-document conversation instead.
  ActorSprite,
};

struct Descriptor {
  // What this is, for code that has to branch on it. Everything else here is data, so a
  // caller that can avoid looking at `domain` should.
  Domain domain;

  // The classifier's answer key and the spelling the CLI uses: `building`, `tile`,
  // `object`. `building` rather than `blueprint` because that is the word the rewrite
  // brief already uses (`--rewrite-brief building`) and the one a person types.
  std::string id;

  // What a window shows a person: `Building`, `Tile`, `Object`.
  std::string label;

  // The noun for prose and refusals: "a building", "a tile", "an object".
  std::string noun;

  // The instruction document for this domain, and the settings key that overrides it.
  // For a sprite these agree with the SpriteKinds row; they are repeated here so a caller
  // holding only a domain can name the file it is about to read.
  std::string promptFile;
  std::string promptSettingKey;

  // The sprite half, or null for a building. A pointer into SpriteKinds::All(), which is
  // stable for the run. Null is the whole point of this table: it is what a caller checks
  // before asking for an art convention, an overlay directory or a glyph.
  const SpriteKinds::Descriptor *sprite = nullptr;
};

// Every domain, in the order a dropdown should offer them.
//
// A function-local static rather than a namespace-scope one: the sprite rows hold pointers
// obtained from SpriteKinds::All(), and a namespace-scope table here would depend on the
// initialisation order between two translation units.
const std::vector<Descriptor> &All();

const Descriptor &For(Domain domain);

// The descriptor for an id ("building", "tile", "object"), or null when the id is
// unknown. Null rather than a default, because "the classifier said something we do not
// have" and "the classifier said tile" must not read the same.
const Descriptor *Parse(const std::string &id);

bool IsSprite(Domain domain);

// The sprite kind for a domain. Only valid when IsSprite(domain); returns
// SpriteKinds::Kind::Tile for a building so a caller that got here by mistake writes a
// tile rather than corrupting memory. Callers should test IsSprite first.
SpriteKinds::Kind SpriteOf(Domain domain);

}  // namespace DesignerDomains
