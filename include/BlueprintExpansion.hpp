#pragma once

#include <set>
#include <string>
#include <utility>
#include <vector>

#include "Blueprint.hpp"

// Turning a parsed blueprint into cells, and cells into world coordinates.
//
// Kept out of BlueprintRegistry so the two halves can be reasoned about
// separately: the registry answers "what does this JSON mean", this file answers
// "which cells does that produce". Nothing here mutates the world.
namespace BlueprintExpansion {

// A cell of a cell set on one level. Ordered by (y, x) so iteration and any
// reported output are deterministic regardless of how the shape was authored.
using Cell = std::pair<int, int>;
using CellSet = std::set<Cell>;

// Rasterises one shape, resolving "region" references against `regions`.
//
// `outline` selects the border form of the *area* primitives. The grammar spells
// that in the primitive name (rect_fill vs rect_outline), so callers pass it
// derived from the kind rather than choosing it; it is a parameter only because
// region-of-region expansion needs to carry it down.
//
// Depth guards region recursion: a region that refers to itself, directly or
// through others, must terminate rather than hang an agent's turn. On exceeding
// it the set comes back empty and `error` says why.
CellSet RasterizeShape(const BlueprintShape &shape,
                       const std::unordered_map<std::string, BlueprintShape> &regions,
                       int depth = 0, std::string *error = nullptr);

// The set of cells a part's shape produces, following a "region" reference when
// the part used one.
CellSet RasterizePart(const BlueprintPart &part,
                      const std::unordered_map<std::string, BlueprintShape> &regions,
                      std::string *error = nullptr);

// The interior of a region: free cells reachable from inside but not from
// outside the region's bounding box. This is what the "fill" op fills, and it is
// the same flood fill the design note's enclosure check needs, so it lives here
// rather than being reimplemented per caller.
//
// Returns false when the region has no interior at all -- a solid shape, or a
// border that leaks -- which is a content problem worth reporting rather than
// silently producing zero cells.
bool Interior(const CellSet &region, CellSet &out);

// The blueprint's bounding box over every part on level 0, relative to the
// design's own origin. Written into Blueprint by the registry.
void ComputeBounds(const Blueprint &blueprint, int &minX, int &minY, int &maxX,
                   int &maxY);

// One cell of a fully expanded blueprint: the tile to paint, the object to
// spawn, or a carve.
struct ExpandedCell {
  int x = 0;
  int y = 0;
  std::string tile;    // tile name to paint; empty unless the cell paints
  std::string object;  // object template id to spawn; empty unless it places
  bool carve = false;  // clear this cell to void
};

// A blueprint resolved into cells, in part order so later parts overwrite
// earlier ones exactly as authored. `errors` carries any shape that could not be
// rasterised, so a broken design reports instead of quietly shrinking.
struct Expansion {
  std::vector<ExpandedCell> cells;
  std::vector<std::string> errors;
  bool ok() const { return errors.empty(); }
};

Expansion Expand(const Blueprint &blueprint);

// Cells a part covered and left with no ground under them.
//
// The rule: every cell a part touches must end up with a tile. A `place` on a cell
// nobody painted leaves an object standing on nothing, and a `carve` on an unpainted cell
// leaves a hole -- and a hole is not cosmetic: the placed building shows the map's own
// ground in the middle of a room, so the room reads as a patchwork of two different
// floors.
//
// Reported rather than thrown so the loader can refuse a design on disk and the designer
// can hand the same sentence back to the model, which is the only way it learns.
struct VoidReport {
  size_t count = 0;
  // One sentence per offending cell, up to the limit the caller asked for.
  std::vector<std::string> messages;

  bool ok() const { return count == 0; }

  // The cells named, the rule, and how to fix it -- one sentence, ready to log or to use
  // as a refusal reason. Kept here rather than spelled out at each call site so the
  // loader and the designer cannot tell a model two different things.
  std::string Summary() const;
};

VoidReport FindVoids(const Expansion &expansion, size_t limit = 8);

}  // namespace BlueprintExpansion
