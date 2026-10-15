#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

// The blueprint grammar: a structure written down as data, independent of any
// map. See docs/blueprint-design.md, which this implements the parsing half of.
//
// Shape and material are separate throughout. A part is *a shape filled with a
// thing*: the shape does not know what it is made of and the material does not
// know its layout, which is why a small grammar covers a whole village -- a
// church is a solid rectangle, two outlines, a carve, a fill and some fixtures,
// not two thousand tile decisions.
//
// Filled or outline is always in the primitive's name (rect_fill vs rect_outline)
// and there is no "filled" boolean and no "thickness" anywhere. One spelling per
// cell set is what lets the validator's overlap and sealing checks be
// unambiguous.
//
// This header is the *data* only. Parsing lives in BlueprintRegistry.cpp and
// turning a shape into cells lives in BlueprintExpansion.cpp, so the two can be
// reasoned about (and tested) separately.

// One shape primitive. Absolute world coordinates never appear: every coordinate
// is relative to the blueprint's own origin, and the site is chosen separately.
struct BlueprintShape {
  // point    at
  // line     at -> to, inclusive, axis-aligned or stepped (Bresenham)
  // rect_*   at,to or at,width,height; `at`/`to` are normalised, so an author
  //          may give either corner first
  // disk_*   at, radius
  // polygon_* points, rasterised by the even-odd rule
  // region   a reference to a named shape in the blueprint's "regions"
  // union / intersect / difference  set algebra over the shapes in "of"
  std::string kind;
  int atX = 0;
  int atY = 0;
  int toX = 0;
  int toY = 0;
  int width = 0;
  int height = 0;
  int radius = 0;
  std::string ref;
  std::vector<std::pair<int, int>> points;
  std::vector<BlueprintShape> of;
};

// A shape, or a reference to a named one. A part names either a "shape" or a
// "region"; both resolve to the same thing.
struct BlueprintShapeRef {
  bool isRegionRef = false;
  std::string regionName;
  BlueprintShape shape;
};

// What a part writes into its shape's cells. "paint" writes a tile, "place"
// spawns an object template, "carve" clears the cell (a doorway or window
// opening), and "fill" fills the *enclosed interior* of the part's shape.
//
// carve is separate from paint rather than a "door: true" flag so that a window,
// a gate and a hole in a roof all express without new vocabulary.
struct BlueprintOp {
  std::string kind;      // paint | place | carve | fill
  std::string material;  // tile name or object template id; empty for carve
  std::string fillKind;  // for "fill": paint | place
  std::string fillMaterial;
};

struct BlueprintPart {
  std::string id;
  BlueprintShapeRef shape;
  BlueprintOp op;
  // structural | entrance | interior_door | fixture | (empty). Advisory for now:
  // the validator is not written yet, so nothing enforces these. They are parsed
  // and kept because the design note's topological checks are defined in terms of
  // them and re-authoring three blueprints later would be worse than carrying
  // the field now.
  std::string role;
  // Multi-storey designs. The current Map is single-layer, so only level 0 is
  // placeable; parsed and preserved so designs do not have to be re-authored.
  int level = 0;
  int levelTo = 0;
};

struct Blueprint {
  std::string id;  // the file stem, matching ItemType::id and recipe ids
  std::string name;
  std::string owner;
  std::string purpose;
  int levels = 1;
  std::unordered_map<std::string, BlueprintShape> regions;
  std::vector<BlueprintPart> parts;
  // The file this came from, for reporting. Empty for a runtime-authored design.
  std::string sourcePath;

  // Bounding box of every part on every level, origin-relative. Set by the
  // registry after parsing; used for both the ghost preview and placement.
  int minX = 0;
  int minY = 0;
  int maxX = 0;
  int maxY = 0;

  int Width() const { return maxX - minX + 1; }
  int Height() const { return maxY - minY + 1; }
};
