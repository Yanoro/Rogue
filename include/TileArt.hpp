#pragma once

#include <functional>
#include <string>
#include <vector>

// Wiring generated art into a tile, and proving the two contracts that govern it.
//
// The chain this is the last three links of (docs/tile-art-wiring-design.md §1):
//
//   agent's tile accepted  ->  generate  ->  palette lock  ->  cut  ->  [this]
//
// `[this]` is: put the sprite in the generated layer under the tile's convention
// name, invalidate the one cache entry that says it is absent, and give the
// entities already drawing that tile a `Sprite`. All three, or the swap does not
// happen -- see §3 and the three checks recorded in
// docs/tile-art-wiring-design.md.
//
// **This module does not generate anything.** It takes a finished PNG. Generation
// arrives behind the seam in step 2 and must be no more than "produce a file, then
// call SwapIn".
//
// The two rules it exists to enforce rather than trust (§1.2):
//
//   1. art goes to the generated layer, never the art pack;
//   2. a tile sprite is `tile_<slug>.png` via TileSpriteId, never `<name>.png`.
//
// Neither is self-enforcing: the slicers write `<key>.png` unprefixed (§2), and
// the art pack's directory is an equally valid place to put a PNG. So the copy
// refuses to overwrite anything in the pack, and the check reports where the file
// that actually wins came from.
namespace TileArt {

// Where the generated sprite for a tile will be written, and whether the art pack
// already supplies one.
struct Target {
  std::string tileName;
  // The convention id: `tile_<slug>`, from TileSpriteId. This is the name, not a
  // path, and it is the one place the prefix rule is applied.
  std::string conventionId;
  // The path in the generated layer. Empty when no generated layer is configured,
  // which is a supported state and not an error: with no art pack there is no
  // "outside the pack" to write to (see GameSettings::ArtSection).
  std::string generatedPath;
  // The path the tile's sprite would resolve from *today*, empty when it resolves
  // from nothing. When this is non-empty and equals `packPath`, the art pack
  // already has art for this tile.
  std::string resolvedPath;
  // The art pack's path for the same id, empty when no art pack is configured.
  std::string packPath;
  // True when the art pack already ships art for this tile. The generated layer is
  // additive, so in that case a swap is written (it is recoverable and reviewable)
  // but is *not* what the renderer will draw -- and saying so is the difference
  // between a working pipeline and a mysterious one.
  bool packWins = false;
};

// Resolves the naming and the roots for `tileName` without touching the
// filesystem. This is the function both the swap and `--tile-art` ask, so the
// headless report cannot disagree with what the game would do.
Target PlanFor(const std::string &tileName);

// Moves/copies `sourcePng` into the generated layer at the tile's convention name.
//
// Refuses outright to write into the art pack, and refuses to overwrite an
// existing file there -- the art pack is hand-curated and pristine by decision
// (docs/asset-generation-design.md §1.2). Creating the generated directory is
// fine; writing into the pack is not, and a check that only warned would be the
// convention this is supposed to enforce in code.
struct CopyResult {
  bool ok = false;
  std::string error;
  // The path written to. Empty on failure.
  std::string path;
  // True when a file was already there and was replaced. The generated layer is
  // ours, so replacing is allowed; it is reported so an overwrite is never silent.
  bool replaced = false;
  // True when `sourcePng` and the destination were the same file, so nothing was
  // copied. Not a failure -- it is the normal case for a re-run.
  bool alreadyInPlace = false;
};

CopyResult PlaceSprite(const Target &target, const std::string &sourcePng);

// One of the two contracts in §7, checked rather than trusted.
struct PaletteCheck {
  // True when the check ran and passed, or when it could not run at all.
  bool ok = true;
  // Whether it actually ran. False when there is no palette file, or no
  // `tools/palette.py` to run it with -- reported so a pass is never claimed on
  // the strength of a check that did not happen.
  bool ran = false;
  // The tool's own output, verbatim, so the number is not re-derived here.
  std::string output;
  // The recolour share as the tool reported it, or -1 when it did not say. This is
  // rounded to one decimal by the tool, so it is for the report only -- `ok` is
  // decided by `outsidePixels`, which is exact. One stray pixel in a 1024-pixel
  // sprite prints as "0.0%" beside a count of "1".
  double outsidePercent = -1.0;
  // The exact number of opaque pixels not in the palette, or -1 when unreported.
  int outsidePixels = -1;
  // How many opaque pixels were measured.
  int opaquePixels = -1;
  std::string error;
};

// Runs `tools/palette.py report --palette <paletteFile> --sprites <dir>` over the
// directory holding `spritePath`, and reports whether every opaque pixel is in the
// palette.
//
// Shells out rather than reimplementing the measurement, deliberately: §7.3
// records that this metric had a bug (it divided by all pixels instead of opaque
// ones) and that the number matters more than the code. A second implementation
// here would be a second number, and the two would eventually disagree. The rule
// this enforces is the §7.3 one: the lock is applied *before* the downscale, so a
// sprite in the generated layer should already be 0% outside.
PaletteCheck CheckPaletteLocked(const std::string &spritePath,
                                const std::string &paletteFile,
                                const std::string &paletteTool = "tools/palette.py");

// The whole tail of the chain, in the order it has to happen:
//
//   1. place the PNG in the generated layer under `tile_<slug>.png`;
//   2. verify the palette contract (when a palette is configured);
//   3. hand off to `swap`, which the caller supplies because invalidating the
//      texture cache and re-attaching Sprites is the world's business, not this
//      module's.
//
// `swap` is called exactly once, on the main thread, only after a successful
// place. It returns the number of map cells whose visual changed, for reporting.
//
// Returning a struct rather than throwing keeps the failure modes explicit: a
// refused copy, a violated palette contract and a successful swap are three
// different things and the caller has to tell them apart.
struct SwapResult {
  bool ok = false;
  // The copied file, and whether it replaced one.
  CopyResult copy;
  // The palette contract. A *failure* here does not undo the placement -- the file
  // is in our own layer and is recoverable -- but it does make `ok` false, because
  // an unlocked sprite is exactly the "looks imported" failure §7.3 exists to
  // prevent.
  PaletteCheck palette;
  // Cells whose visual was refreshed. 0 when the tile is not on the map, which is
  // normal: a tile can be created and never painted.
  size_t refreshedCells = 0;
  std::string error;
};

SwapResult SwapIn(const std::string &tileName, const std::string &sourcePng,
                  const std::string &paletteFile,
                  const std::function<size_t(const std::string &)> &swap);

}  // namespace TileArt
