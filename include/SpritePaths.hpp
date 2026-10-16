#pragma once

#include <flecs.h>
#include <raylib.h>

#include <string>
#include <vector>

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

// Every path a convention id is looked for at, most authoritative first. Exposed
// as paths rather than textures for two reasons:
//
//   * the gap tooling has to ask "would this resolve?" without loading anything,
//     and it must use the same rules in the same order as the renderer or its
//     report is a lie;
//   * it is the single place the layering is expressed -- art pack first, then
//     the generated layer, then data/sprites/ when no art pack is configured.
//
// Empty for an empty `conventionId`. See GameSettings::ArtSection for the roots.
std::vector<std::string> SpriteCandidatePaths(const std::string &conventionId);

// Whether a sprite *would* resolve, in exactly the order ResolveSpriteTexture
// tries -- without loading anything.
//
// This exists so a report cannot disagree with the renderer. Asking the texture
// cache would load every image just to answer a question about paths, and asking
// the filesystem with separately written rules would eventually lie. One lookup
// order, two consumers.
//
// `resolvedPath`, when given, receives the path that answered.
bool SpriteResolves(const std::string &explicitPath,
                    const std::string &conventionId,
                    std::string *resolvedPath = nullptr);

// The art roots in use, for tooling that has to name them -- the gap report says
// where it looked and decides where to write itself. Exposed rather than
// re-derived, so the derivation (which is not obvious: the generated layer's
// location is inferred from the pack's) exists in exactly one place.
struct SpriteRootsInfo {
  std::string artPackSprites;    // empty when no art pack is configured
  std::string generatedSprites;  // empty when not applicable
};

SpriteRootsInfo CurrentSpriteRoots();

// The path a tile's sprite is looked for in the *generated layer*, whether or not
// the file exists there yet -- empty when no generated layer is configured.
//
// This is the write target for newly generated tile art, and it needs to be a
// separate question from "where does this tile's sprite resolve from". They differ
// in exactly the case that matters: the art pack already has a `tile_x.png`, so the
// tile resolves from the pack while the generated layer is still where new art for
// it has to go -- where it will be *ignored* (the pack wins) rather than shadowing
// hand-authored art. Asking `SpriteCandidatePaths(...).back()` would be wrong when
// the pack is configured but the generated directory is not, and asking
// `SpriteResolves` would answer with the pack's path and quietly overwrite the art
// pack -- which is the one thing §1.2 forbids.
std::string GeneratedSpritePath(const std::string &conventionId);
