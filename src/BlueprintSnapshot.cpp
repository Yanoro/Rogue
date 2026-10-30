#include "BlueprintSnapshot.hpp"

#include <raylib.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "Blueprint.hpp"
#include "BlueprintExpansion.hpp"
#include "ImageEncode.h"
#include "ObjectFactory.h"
#include "SpritePaths.hpp"
#include "TileRegistry.h"

namespace BlueprintSnapshot {

namespace {

// The question mark the gap is stamped with. A 5x7 bitmap rather than a font for a
// reason that is not laziness: raylib's default font has to be loaded onto the GPU,
// which needs a window, and this whole module exists to run before one. Seven rows of
// five bits is cheaper than the alternative and cannot fail to load.
constexpr const char *kQuestionMark[7] = {
    "01110",
    "10001",
    "00010",
    "00100",
    "00100",
    "00000",
    "00100",
};

// The file a sprite actually lives in, or empty. Explicit path first, then the same
// convention roots ResolveSpriteTexture tries -- in the same order, because a preview
// that disagreed with the renderer about which file wins would be a preview of a
// different sprite.
std::string FirstExistingSprite(const std::string &explicitPath,
                                const std::string &conventionId) {
  std::error_code ec;
  // The generated layer is skipped, not merely deprioritised. A style signal that
  // contained previously generated art would feed a generator its own output, which is
  // the loop asset-generation-design.md section 3 exists to prevent: the signal dilutes
  // with each generation until everything converges on the mean of the last batch.
  //
  // An *explicit* path into the generated layer is still honoured -- a template that
  // names its sprite is telling us what this thing already looks like, which is not a
  // fallback search. Only the convention search is restricted.
  const std::string generatedRoot = CurrentSpriteRoots().generatedSprites;
  const auto inGeneratedLayer = [&generatedRoot](const std::string &path) {
    return !generatedRoot.empty() &&
           path.compare(0, generatedRoot.size(), generatedRoot) == 0;
  };
  if (!explicitPath.empty() && std::filesystem::exists(explicitPath, ec)) {
    return explicitPath;
  }
  if (conventionId.empty()) return {};
  for (const std::string &candidate : SpriteCandidatePaths(conventionId)) {
    if (inGeneratedLayer(candidate)) continue;
    if (std::filesystem::exists(candidate, ec)) {
      return candidate;
    }
  }
  return {};
}

std::string TileSpritePath(const TileRegistry &tiles, const std::string &name) {
  const TileDef *definition = tiles.Get(name);
  if (definition == nullptr) return {};
  return FirstExistingSprite(definition->sprite, TileSpriteId(name));
}

std::string ObjectSpritePath(const ObjectFactory &objects, const std::string &key) {
  const auto &templates = objects.GetTemplates();
  const auto found = templates.find(key);
  if (found == templates.end()) return {};
  const std::string explicitPath =
      found->second.is_object() ? found->second.value("sprite", std::string())
                                : std::string();
  return FirstExistingSprite(explicitPath, key);
}

// Blits one sprite into the cell at (x, y). Scaling is ImageDraw's job: passing a
// source rectangle of the whole sprite and a destination rectangle of one cell
// resamples exactly once, so a 16px sprite and a 32px one land the same size.
void DrawSprite(Image &canvas, const std::string &path, int x, int y, int cell) {
  Image sprite = LoadImage(path.c_str());
  if (sprite.data == nullptr) return;
  const Rectangle source{0.0f, 0.0f, static_cast<float>(sprite.width),
                         static_cast<float>(sprite.height)};
  const Rectangle destination{static_cast<float>(x), static_cast<float>(y),
                              static_cast<float>(cell), static_cast<float>(cell)};
  ImageDraw(&canvas, sprite, source, destination, WHITE);
  UnloadImage(sprite);
}

// Fills the cell and draws a question mark centred in it. Solid rather than
// translucent: an image model asked to "use the ramp entries, not invent a colour"
// will not read a 60%-alpha blob as a hole in the building.
void StampGap(Image &canvas, int x, int y, int cell, Color fill, Color ink) {
  ImageDrawRectangle(&canvas, x, y, cell, cell, fill);
  const int block = std::max(1, cell / 10);
  const int glyphWidth = 5 * block;
  const int glyphHeight = 7 * block;
  const int left = x + (cell - glyphWidth) / 2;
  const int top = y + (cell - glyphHeight) / 2;
  for (int row = 0; row < 7; ++row) {
    for (int column = 0; column < 5; ++column) {
      if (kQuestionMark[row][column] != '1') continue;
      ImageDrawRectangle(&canvas, left + column * block, top + row * block, block,
                         block, ink);
    }
  }
}

}  // namespace

Snapshot::~Snapshot() {
  if (image.data != nullptr) {
    UnloadImage(image);
    image = Image{};
  }
}

Snapshot::Snapshot(Snapshot &&other) noexcept { *this = std::move(other); }

Snapshot &Snapshot::operator=(Snapshot &&other) noexcept {
  if (this == &other) return *this;
  if (image.data != nullptr) {
    UnloadImage(image);
  }
  ok = other.ok;
  error = std::move(other.error);
  image = other.image;
  originX = other.originX;
  originY = other.originY;
  cellsWide = other.cellsWide;
  cellsTall = other.cellsTall;
  gaps = std::move(other.gaps);
  notes = std::move(other.notes);
  cellsDrawn = other.cellsDrawn;
  other.image = Image{};
  other.ok = false;
  return *this;
}

Snapshot Render(const Blueprint &blueprint, const std::unordered_set<std::string> &missing,
                const TileRegistry &tiles, const ObjectFactory &objects,
                const Options &options) {
  Snapshot out;

  // The expansion, not the parts: later parts overwrite earlier ones there, so the
  // crop is what would actually be built rather than the sum of every attempt at it.
  const BlueprintExpansion::Expansion expansion = BlueprintExpansion::Expand(blueprint);
  if (expansion.cells.empty()) {
    out.error = expansion.errors.empty()
                    ? "the design expands to no cells"
                    : "the design does not expand: " + expansion.errors.front();
    return out;
  }

  // Bounds from the cells rather than Blueprint::minX/maxX: those are filled by the
  // registry's parser, and a Blueprint built anywhere else (the designer's reply path
  // builds one before saving) would otherwise be cropped to a 1x1 canvas.
  int minX = expansion.cells.front().x;
  int maxX = minX;
  int minY = expansion.cells.front().y;
  int maxY = minY;
  for (const BlueprintExpansion::ExpandedCell &cell : expansion.cells) {
    minX = std::min(minX, cell.x);
    maxX = std::max(maxX, cell.x);
    minY = std::min(minY, cell.y);
    maxY = std::max(maxY, cell.y);
  }

  const int margin = std::max(0, options.marginCells);
  const int cellPixels = std::max(4, options.cellPixels);
  out.originX = minX - margin;
  out.originY = minY - margin;
  out.cellsWide = (maxX - minX + 1) + margin * 2;
  out.cellsTall = (maxY - minY + 1) + margin * 2;

  out.image = GenImageColor(out.cellsWide * cellPixels, out.cellsTall * cellPixels,
                            options.background);
  if (out.image.data == nullptr) {
    out.error = "could not allocate a " + std::to_string(out.cellsWide * cellPixels) +
                "x" + std::to_string(out.cellsTall * cellPixels) + " image";
    return out;
  }

  // Grouped by (kind, material) so the caller asks for one document per name rather
  // than one per cell, and keyed by a string so the order is stable from run to run --
  // a crop whose gap list shuffled would make two runs of the same design incomparable.
  std::map<std::string, size_t> gapIndex;
  std::set<std::string> noteSet;
  const auto recordGap = [&](const std::string &kind, const std::string &material,
                             int cellX, int cellY) {
    const std::string key = kind + "|" + material;
    auto found = gapIndex.find(key);
    size_t at = 0;
    if (found == gapIndex.end()) {
      at = out.gaps.size();
      gapIndex.emplace(key, at);
      Gap gap;
      gap.material = material;
      gap.kind = kind;
      out.gaps.push_back(std::move(gap));
    } else {
      at = found->second;
    }
    out.gaps[at].cells.emplace_back(cellX, cellY);
  };

  const int canvasWidth = out.image.width;
  const int canvasHeight = out.image.height;

  for (const BlueprintExpansion::ExpandedCell &cell : expansion.cells) {
    const int x = (cell.x - out.originX) * cellPixels;
    const int y = (cell.y - out.originY) * cellPixels;
    if (x < 0 || y < 0 || x + cellPixels > canvasWidth ||
        y + cellPixels > canvasHeight) {
      continue;
    }

    if (!cell.tile.empty()) {
      if (options.markGaps && missing.count(cell.tile) != 0) {
        StampGap(out.image, x, y, cellPixels, options.gapFill, options.gapInk);
        recordGap("paint", cell.tile, cell.x, cell.y);
      } else {
        const std::string path = TileSpritePath(tiles, cell.tile);
        if (!path.empty()) {
          DrawSprite(out.image, path, x, y, cellPixels);
        } else {
          // A plate in the tile's own background colour, because "this floor has no
          // art" is still information about the floor's tone, and drawing nothing
          // would put the dark canvas behind every cell of a missing tileset.
          const TileDef *definition = tiles.Get(cell.tile);
          Color plate = {70, 70, 74, 255};
          if (definition != nullptr && definition->hasBackgroundColor) {
            plate = definition->backgroundColor;
            plate.a = 255;
          }
          ImageDrawRectangle(&out.image, x, y, cellPixels, cellPixels, plate);
          if (definition == nullptr) {
            noteSet.insert("paints '" + cell.tile + "', which is not in the tile catalog");
          } else {
            noteSet.insert("no sprite resolves for tile '" + cell.tile +
                           "'; drawn as a flat plate");
          }
        }
      }
      ++out.cellsDrawn;
    }

    // Carve clears the object and keeps the ground, so there is nothing to draw.
    if (!cell.object.empty() && !cell.carve) {
      if (options.markGaps && missing.count(cell.object) != 0) {
        StampGap(out.image, x, y, cellPixels, options.gapFill, options.gapInk);
        recordGap("place", cell.object, cell.x, cell.y);
      } else {
        const std::string path = ObjectSpritePath(objects, cell.object);
        if (path.empty()) {
          noteSet.insert("no sprite resolves for object '" + cell.object + "'");
        } else {
          DrawSprite(out.image, path, x, y, cellPixels);
        }
      }
    }
  }

  out.notes.assign(noteSet.begin(), noteSet.end());
  out.ok = true;
  return out;
}

bool WritePng(const Image &image, const std::string &path) {
  if (image.data == nullptr || image.width <= 0 || image.height <= 0) return false;
  // ExportImage takes the Image by value and does not modify it; the copy is how the
  // const is shed without const_casting a struct raylib may touch.
  Image copy = image;
  return ExportImage(copy, path.c_str());
}

bool EncodePng(const Image &image, std::string *base64,
               std::vector<unsigned char> *bytes) {
  if (base64 == nullptr) return false;
  base64->clear();
  if (bytes != nullptr) bytes->clear();
  if (image.data == nullptr || image.width <= 0 || image.height <= 0) return false;

  Image copy = image;
  int size = 0;
  unsigned char *png = ExportImageToMemory(copy, ".png", &size);
  if (png == nullptr || size <= 0) {
    if (png != nullptr) MemFree(png);
    return false;
  }
  if (bytes != nullptr) {
    bytes->assign(png, png + size);
  }
  *base64 = EncodeBase64(png, static_cast<std::size_t>(size));
  // ExportImageToMemory allocates with MemAlloc and raylib does not own the buffer.
  MemFree(png);
  return !base64->empty();
}

std::string Describe(const Blueprint &blueprint, const Snapshot &snapshot) {
  std::ostringstream out;
  out << "A crop of the building \"" << blueprint.name << "\"";
  if (!blueprint.id.empty()) {
    out << " (" << blueprint.id << ")";
  }
  out << ", " << snapshot.cellsWide << "x" << snapshot.cellsTall << " cells";
  if (snapshot.gaps.empty()) {
    out << ", with no gaps.";
    return out.str();
  }
  out << ", with " << snapshot.gaps.size() << " gap(s) marked in amber with a \"?\":";
  for (const Gap &gap : snapshot.gaps) {
    out << "\n  - " << (gap.kind == "paint" ? "the ground tile" : "the object") << " \""
        << gap.material << "\" (" << gap.cells.size() << " cell(s))";
  }
  out << "\nEvery mark is something the world does not have yet. The art around them is "
         "what the new sprite has to live with.";
  return out.str();
}

}  // namespace BlueprintSnapshot
