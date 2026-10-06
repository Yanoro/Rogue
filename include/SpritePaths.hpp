#pragma once

#include <flecs.h>
#include <raylib.h>

#include <string>

class ResourceManager;
class DebugLog;

// Where a sprite is looked for when nothing points at one explicitly.
inline constexpr const char *SPRITE_DIRECTORY = "data/sprites/";

// The convention id for a map tile: its name lowercased and slugified, prefixed
// with "tile_". So the tile named "tilled soil" looks for
// data/sprites/tile_tilled_soil.png.
//
// The prefix is not decoration. Tile names are ordinary words -- "floor", "path",
// "water" -- and an object template could take any of them later; a bare
// data/sprites/water.png would then be ambiguous between the two systems.
std::string TileSpriteId(const std::string &tileName);

// Resolves a sprite to a loaded texture, or nullptr when there is none.
//
// Precedence:
//   1. `explicitPath` -- the "sprite" key authored on an object template, a race
//      file or a map NPC entry. Something meant this, so a path that is missing
//      or unloadable warns; it then falls through rather than failing the caller,
//      matching how a bad art block is treated.
//   2. `data/sprites/<conventionId>.png`, so new content gains a look by dropping
//      a file in without touching any JSON. Absence here is silent on purpose --
//      most content has no sprite, and a warning per id would bury the log.
//   3. nothing, leaving the ASCII art as the whole visual.
//
// `conventionId` is an object template key or a race id. Pass an empty string for
// content with no sensible id of its own -- a map NPC, for instance, whose race
// supplies the convention instead.
//
// Exposed rather than kept private to AttachSprite because the UI surfaces need
// the same answer: the map editor's template palette draws a cell per template
// and the inventory draws a row per item, and neither may show a look the map
// would not. Same reason both art renderers live in AsciiArtDraw.hpp.
//
// `resolvedPath` is filled in with whichever path won, for callers that need to
// remember it rather than just draw it.
const Texture2D *ResolveSpriteTexture(const std::string &explicitPath,
                                      const std::string &conventionId,
                                      ResourceManager &resources,
                                      DebugLog *debugLog,
                                      std::string *resolvedPath = nullptr);

// Gives `entity` a Sprite when an image resolves, and leaves it untouched
// otherwise. The untouched case is the common one, and it is what keeps a
// sprite-less entity on its ASCII art: nothing here clears AsciiArt or DrawAscii,
// so the renderer's existing art-then-glyph fallback simply still applies.
bool AttachSprite(flecs::entity entity, const std::string &explicitPath,
                  const std::string &conventionId, ResourceManager &resources,
                  DebugLog *debugLog);
