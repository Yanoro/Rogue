#include "BlueprintExpansion.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

namespace BlueprintExpansion {
namespace {

constexpr int kRegionDepthCap = 8;

void InsertRect(CellSet &cells, int ax, int ay, int bx, int by) {
  for (int x = std::min(ax, bx); x <= std::max(ax, bx); ++x) {
    for (int y = std::min(ay, by); y <= std::max(ay, by); ++y) {
      cells.insert({x, y});
    }
  }
}

// Bresenham. Note this steps diagonally, so a stepped line is *not* guaranteed
// 4-connected: two diagonal runs can touch only at a corner. That matters for
// sealing a building (see docs/blueprint-design.md, wall sealing rule) and is
// why rect_outline, not a four-line wall, is the wall primitive.
void InsertLine(CellSet &cells, int x0, int y0, int x1, int y1) {
  const int dx = std::abs(x1 - x0);
  const int dy = -std::abs(y1 - y0);
  const int sx = x0 < x1 ? 1 : -1;
  const int sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  int x = x0;
  int y = y0;
  for (;;) {
    cells.insert({x, y});
    if (x == x1 && y == y1) break;
    const int e2 = 2 * err;
    if (e2 >= dy) {
      err += dy;
      x += sx;
    }
    if (e2 <= dx) {
      err += dx;
      y += sy;
    }
  }
}

void InsertDisk(CellSet &cells, int cx, int cy, int radius) {
  const int r2 = radius * radius;
  for (int x = cx - radius; x <= cx + radius; ++x) {
    for (int y = cy - radius; y <= cy + radius; ++y) {
      const int dx = x - cx;
      const int dy = y - cy;
      if (dx * dx + dy * dy <= r2) {
        cells.insert({x, y});
      }
    }
  }
}

// Even-odd ray cast in cell space: a cell is inside when a ray from it crosses
// the polygon an odd number of times. Cell centres are sampled at x+0.5 so the
// test is not ambiguous about which cell an edge on an integer coordinate
// belongs to.
bool PointInPolygon(const std::vector<std::pair<int, int>> &points, double px,
                    double py) {
  bool inside = false;
  const size_t n = points.size();
  for (size_t i = 0, j = n - 1; i < n; j = i++) {
    const double xi = points[i].first;
    const double yi = points[i].second;
    const double xj = points[j].first;
    const double yj = points[j].second;
    const bool crosses = ((yi > py) != (yj > py)) &&
                         (px < (xj - xi) * (py - yi) / (yj - yi) + xi);
    if (crosses) inside = !inside;
  }
  return inside;
}

void InsertPolygon(CellSet &cells, const std::vector<std::pair<int, int>> &points) {
  if (points.size() < 3) {
    // A degenerate polygon has no interior. Keep its vertices so an author sees
    // something rather than nothing.
    for (const auto &p : points) cells.insert(p);
    return;
  }

  int minX = points[0].first;
  int maxX = minX;
  int minY = points[0].second;
  int maxY = minY;
  for (const auto &p : points) {
    minX = std::min(minX, p.first);
    maxX = std::max(maxX, p.first);
    minY = std::min(minY, p.second);
    maxY = std::max(maxY, p.second);
  }

  for (int x = minX; x <= maxX; ++x) {
    for (int y = minY; y <= maxY; ++y) {
      if (PointInPolygon(points, x + 0.5, y + 0.5)) {
        cells.insert({x, y});
      }
    }
  }
}

// The one-cell border of an already-filled set: every cell with at least one of
// its four orthogonal neighbours outside the set. Deriving the border from the
// fill means fill and outline can never disagree about what a shape covers.
//
// The obvious cheaper implementation -- keep the top and bottom cell of each
// occupied column -- is wrong, and wrong in a way that matters here: for a solid
// rectangle it puts a border cell at the top and bottom of every *interior*
// column too, turning a 7x6 rectangle's 20-cell border into 24 and punching the
// top and bottom rows through the middle. Since rect_outline is the wall
// primitive, that would have made every generated building's walls wrong.
//
// A one-cell-thick shape (a 1-wide rectangle, a single row) keeps all of its
// cells, because each has a neighbour outside: an erosion-based border would
// erase it entirely instead.
CellSet OutlineOf(const CellSet &filled) {
  static const int dx[4] = {1, -1, 0, 0};
  static const int dy[4] = {0, 0, 1, -1};

  CellSet outline;
  for (const auto &cell : filled) {
    for (int i = 0; i < 4; ++i) {
      const Cell neighbour{cell.first + dx[i], cell.second + dy[i]};
      if (filled.count(neighbour) == 0) {
        outline.insert(cell);
        break;
      }
    }
  }
  return outline;
}

bool EndsWith(const std::string &value, const std::string &suffix) {
  return value.size() >= suffix.size() &&
         value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

void SetError(std::string *error, const std::string &message) {
  if (error != nullptr && error->empty()) {
    *error = message;
  }
}

// The filled form of an area primitive, whatever spelling it was authored in.
CellSet RasterizeFilled(const BlueprintShape &shape,
                        const std::unordered_map<std::string, BlueprintShape> &regions,
                        int depth, std::string *error) {
  CellSet cells;
  const std::string &kind = shape.kind;

  if (kind == "point") {
    cells.insert({shape.atX, shape.atY});
  } else if (kind == "line") {
    InsertLine(cells, shape.atX, shape.atY, shape.toX, shape.toY);
  } else if (kind == "rect_fill" || kind == "rect_outline") {
    if (shape.width != 0 || shape.height != 0) {
      InsertRect(cells, shape.atX, shape.atY, shape.atX + shape.width - 1,
                 shape.atY + shape.height - 1);
    } else {
      InsertRect(cells, shape.atX, shape.atY, shape.toX, shape.toY);
    }
  } else if (kind == "disk_fill" || kind == "disk_outline") {
    InsertDisk(cells, shape.atX, shape.atY, shape.radius);
  } else if (kind == "polygon_fill" || kind == "polygon_outline") {
    InsertPolygon(cells, shape.points);
  } else {
    SetError(error, "unknown shape primitive '" + kind + "'");
  }

  if (EndsWith(kind, "_outline")) {
    return OutlineOf(cells);
  }
  return cells;
}

}  // namespace

CellSet RasterizeShape(const BlueprintShape &shape,
                       const std::unordered_map<std::string, BlueprintShape> &regions,
                       int depth, std::string *error) {
  if (depth > kRegionDepthCap) {
    SetError(error, "region references nest deeper than " +
                        std::to_string(kRegionDepthCap) +
                        "; a region probably refers to itself");
    return {};
  }

  const std::string &kind = shape.kind;

  if (kind == "region") {
    const auto it = regions.find(shape.ref);
    if (it == regions.end()) {
      SetError(error, "region '" + shape.ref + "' is not defined");
      return {};
    }
    return RasterizeShape(it->second, regions, depth + 1, error);
  }

  if (kind == "union" || kind == "intersect" || kind == "difference") {
    if (shape.of.empty()) {
      SetError(error, "'" + kind + "' has no shapes in 'of'");
      return {};
    }
    CellSet result = RasterizeShape(shape.of.front(), regions, depth + 1, error);
    for (size_t i = 1; i < shape.of.size(); ++i) {
      const CellSet next = RasterizeShape(shape.of[i], regions, depth + 1, error);
      if (kind == "union") {
        result.insert(next.begin(), next.end());
      } else if (kind == "intersect") {
        CellSet kept;
        for (const auto &cell : result) {
          if (next.count(cell) != 0) kept.insert(cell);
        }
        result = std::move(kept);
      } else {
        for (const auto &cell : next) result.erase(cell);
      }
    }
    return result;
  }

  return RasterizeFilled(shape, regions, depth, error);
}

CellSet RasterizePart(const BlueprintPart &part,
                      const std::unordered_map<std::string, BlueprintShape> &regions,
                      std::string *error) {
  if (part.shape.isRegionRef) {
    const auto it = regions.find(part.shape.regionName);
    if (it == regions.end()) {
      SetError(error, "part '" + part.id + "' refers to undefined region '" +
                          part.shape.regionName + "'");
      return {};
    }
    return RasterizeShape(it->second, regions, 1, error);
  }
  return RasterizeShape(part.shape.shape, regions, 0, error);
}

bool Interior(const CellSet &region, CellSet &out) {
  out.clear();
  if (region.empty()) return false;

  int minX = std::numeric_limits<int>::max();
  int maxX = std::numeric_limits<int>::min();
  int minY = std::numeric_limits<int>::max();
  int maxY = std::numeric_limits<int>::min();
  for (const auto &cell : region) {
    minX = std::min(minX, cell.first);
    maxX = std::max(maxX, cell.first);
    minY = std::min(minY, cell.second);
    maxY = std::max(maxY, cell.second);
  }

  // Flood the outside one ring beyond the region, so the search can get around a
  // border that touches the bounding box on all four sides.
  const int ox = minX - 1;
  const int oy = minY - 1;
  const int ex = maxX + 1;
  const int ey = maxY + 1;

  CellSet visited;
  std::vector<Cell> stack;
  stack.push_back({ox, oy});
  visited.insert({ox, oy});

  const int dx[4] = {1, -1, 0, 0};
  const int dy[4] = {0, 0, 1, -1};

  while (!stack.empty()) {
    const Cell current = stack.back();
    stack.pop_back();
    for (int i = 0; i < 4; ++i) {
      const int nx = current.first + dx[i];
      const int ny = current.second + dy[i];
      if (nx < ox || nx > ex || ny < oy || ny > ey) continue;
      const Cell next{nx, ny};
      if (region.count(next) != 0) continue;      // the border itself
      if (visited.count(next) != 0) continue;
      visited.insert(next);
      stack.push_back(next);
    }
  }

  for (int x = minX; x <= maxX; ++x) {
    for (int y = minY; y <= maxY; ++y) {
      const Cell cell{x, y};
      if (region.count(cell) != 0) continue;
      if (visited.count(cell) != 0) continue;  // reached from outside: a leak
      out.insert(cell);
    }
  }
  return !out.empty();
}

void ComputeBounds(const Blueprint &blueprint, int &minX, int &minY, int &maxX,
                   int &maxY) {
  minX = std::numeric_limits<int>::max();
  minY = std::numeric_limits<int>::max();
  maxX = std::numeric_limits<int>::min();
  maxY = std::numeric_limits<int>::min();

  bool any = false;
  for (const BlueprintPart &part : blueprint.parts) {
    // Only level 0: the map is single-layer, and a preview of a floor the world
    // cannot show would be misleading rather than informative.
    if (part.level != 0) continue;
    const CellSet cells = RasterizePart(part, blueprint.regions);
    for (const auto &cell : cells) {
      minX = std::min(minX, cell.first);
      maxX = std::max(maxX, cell.first);
      minY = std::min(minY, cell.second);
      maxY = std::max(maxY, cell.second);
      any = true;
    }
  }

  if (!any) {
    minX = minY = maxX = maxY = 0;
  }
}

Expansion Expand(const Blueprint &blueprint) {
  Expansion result;

  // Cells are accumulated per part and merged in order, so a later part
  // overwrites an earlier one on the same cell -- which is how a carve opens a
  // wall and how a floor can be laid before the walls go up.
  std::map<Cell, ExpandedCell> merged;

  for (const BlueprintPart &part : blueprint.parts) {
    if (part.level != 0) continue;  // single-layer world

    std::string error;
    CellSet cells = RasterizePart(part, blueprint.regions, &error);
    if (!error.empty()) {
      result.errors.push_back("part '" + part.id + "': " + error);
      continue;
    }
    if (cells.empty()) {
      result.errors.push_back("part '" + part.id + "' produces no cells");
      continue;
    }

    if (part.op.kind == "fill") {
      CellSet interior;
      if (!Interior(cells, interior)) {
        result.errors.push_back(
            "part '" + part.id +
            "': 'fill' found no interior -- the shape is solid or its border "
            "leaks, so there is nothing enclosed to fill");
        continue;
      }
      cells = std::move(interior);
    }

    for (const auto &cell : cells) {
      ExpandedCell &out = merged[cell];
      out.x = cell.first;
      out.y = cell.second;
      if (part.op.kind == "carve") {
        out.carve = true;
        out.tile.clear();
        out.object.clear();
      } else if (part.op.kind == "paint") {
        out.carve = false;
        out.tile = part.op.material;
        out.object.clear();
      } else if (part.op.kind == "place") {
        out.carve = false;
        out.tile.clear();
        out.object = part.op.material;
      } else if (part.op.kind == "fill") {
        out.carve = false;
        if (part.op.fillKind == "place") {
          out.tile.clear();
          out.object = part.op.fillMaterial;
        } else {
          out.tile = part.op.fillMaterial;
          out.object.clear();
        }
      } else {
        result.errors.push_back("part '" + part.id + "': unknown op '" +
                                part.op.kind + "'");
      }
    }
  }

  result.cells.reserve(merged.size());
  for (const auto &entry : merged) {
    result.cells.push_back(entry.second);
  }
  return result;
}

}  // namespace BlueprintExpansion
