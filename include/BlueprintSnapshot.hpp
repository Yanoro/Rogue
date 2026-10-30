#pragma once

#include <raylib.h>

#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

struct Blueprint;
class TileRegistry;
class ObjectFactory;

// A picture of a blueprint as the world would draw it, for the sprite designer to
// look at.
//
// Why a picture at all. The document turn is asked to draw one object out of a
// building it cannot see: the brief says "jail cell bars" and nothing says "between
// grey stone walls, on a dirt floor, in a place with a lot of one-cell detail".
// That context is what makes the document match its neighbours instead of being
// correct and foreign, and it is the one thing a text brief cannot carry --
// materials and tones can be named, but "how busy is this wall" only exists as
// pixels.
//
// Three things this deliberately is not:
//
//   * **Not a screenshot of the game.** It rasterises the expansion on the CPU
//     (LoadImage/ImageDraw/ExportImage), so it needs no window, no GL context and
//     no camera -- which is the same reason --sprite-compile and --gaps run before
//     InitWindow. A blueprint is map-independent and has no site, so there is no
//     "the game" to photograph: the design *is* the thing to draw.
//   * **Not the generated layer.** Every sprite here resolves through
//     SpriteCandidatePaths, which puts the art pack first, but the whole point of
//     the gap is that a neighbouring sprite is hand-curated. A crop that showed
//     previously generated sprites would feed generated output back into the style
//     signal, which is the one thing asset-generation-design.md section 3 forbids.
//     Callers that have a generated sprite on screen already have a style.
//   * **Not a design tool.** It draws the last thing the blueprint says, gaps and
//     all, and marks the gaps. It never fills one in, and it cannot fail a design.
namespace BlueprintSnapshot {

struct Options {
  // One cell in pixels. 32 is the sprite convention's own size, so a crop is what
  // the world looks like at native resolution rather than a resample of it.
  int cellPixels = 32;
  // Cells of context around the design's bounding box. Two is deliberate: one cell
  // shows the wall the object will touch, two show the motif the wall repeats.
  int marginCells = 2;
  // Whether to stamp the gap mark. On for the describer, off for a plain preview.
  bool markGaps = true;
  // Filled behind every cell. Opaque on purpose: a transparent PNG is ambiguous to
  // an image model (transparency reads as black, or as nothing at all), and the
  // question being asked is about art, not about compositing.
  Color background = {38, 38, 42, 255};
  // The gap mark's two colours. Amber against a dark field because it does not
  // occur in this pack's palette, so it cannot be read as part of the art.
  Color gapFill = {255, 196, 64, 255};
  Color gapInk = {32, 24, 8, 255};
};

// One material the design names and the world cannot supply, with the cells it
// occupies. Grouped by material like MissingMaterial, because that is how the
// caller asks for the assets: one document per name, not one per cell.
struct Gap {
  std::string material;  // exactly as the blueprint spells it
  std::string kind;      // "paint" (a ground tile) or "place" (an object)
  std::vector<std::pair<int, int>> cells;  // origin-relative
};

struct Snapshot {
  bool ok = false;
  // Blocking reason. Empty when ok.
  std::string error;
  // The rendered crop. Owned; released by the destructor.
  Image image = {};
  // The design cell that the canvas's top-left corner corresponds to. Negative when
  // the margin extends past the origin, which is normal: marginCells of context is
  // the point.
  int originX = 0;
  int originY = 0;
  int cellsWide = 0;
  int cellsTall = 0;
  // Every gap, deduplicated by (kind, material) and in a stable order.
  std::vector<Gap> gaps;
  // Non-blocking observations: a sprite that did not resolve and fell back to a
  // plate, a design with nothing to draw. Kept on success, because "the crop is
  // mostly flat plates" is exactly what would make a style brief wrong.
  std::vector<std::string> notes;
  size_t cellsDrawn = 0;

  Snapshot() = default;
  ~Snapshot();
  Snapshot(const Snapshot &) = delete;
  Snapshot &operator=(const Snapshot &) = delete;
  Snapshot(Snapshot &&other) noexcept;
  Snapshot &operator=(Snapshot &&other) noexcept;
};

// Renders `blueprint`. `missing` is the set of material names the vocabulary lacks
// -- normally `FindMissingVocabulary` reduced to names -- and those cells are marked
// rather than drawn as nothing.
//
// `tiles` and `objects` are const because nothing here changes the world: the crop
// is a view, and a view that could add a tile would be a second writable copy of the
// catalog.
Snapshot Render(const Blueprint &blueprint, const std::unordered_set<std::string> &missing,
                const TileRegistry &tiles, const ObjectFactory &objects,
                const Options &options = {});

// Writes the crop as a PNG. CPU-side, like PixelDSL::WritePng, so it works before a
// window exists. Returns false when the image is empty or the path is unwritable.
bool WritePng(const Image &image, const std::string &path);

// The PNG bytes and their base64 form, which is what a vision request carries.
// `base64` is empty when the image could not be encoded.
bool EncodePng(const Image &image, std::string *base64, std::vector<unsigned char> *bytes);

// The one-line description of the crop that goes with the image: what it is, how
// big one cell is, and which cells are holes. Kept beside the renderer so the
// prompt and the picture cannot disagree about what the marks mean.
std::string Describe(const Blueprint &blueprint, const Snapshot &snapshot);

}  // namespace BlueprintSnapshot
