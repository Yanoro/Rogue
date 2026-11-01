#include "PathFinding.h"
#include "Map.h"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <queue>
#include <vector>

namespace {

// Costs are the usual "octile" ones: a diagonal step is 14 rather than 20 (the
// price of two orthogonal steps), because it covers the same Euclidean distance
// in one move. Integers keep the search exact and free of float comparisons.
constexpr int ORTHOGONAL_COST = 10;
constexpr int DIAGONAL_COST = 14;

// No route found, or no route computed yet.
constexpr int UNREACHABLE = std::numeric_limits<int>::max();

struct Step {
  int dx;
  int dy;
  int cost;
};

// All eight moves, in a fixed order so a map whose costs tie always yields the
// same path. That matters more than it sounds: two paths of equal cost that
// alternate between runs make an NPC look indecisive for no visible reason.
constexpr Step STEPS[8] = {
    {-1, -1, DIAGONAL_COST}, {0, -1, ORTHOGONAL_COST}, {1, -1, DIAGONAL_COST},
    {-1, 0, ORTHOGONAL_COST},                          {1, 0, ORTHOGONAL_COST},
    {-1, 1, DIAGONAL_COST},  {0, 1, ORTHOGONAL_COST},  {1, 1, DIAGONAL_COST},
};

// Straight-line-ish lower bound on the cost between two cells: walk the
// diagonal part of the rectangle, then the leftover straight part. It never
// overestimates, because a diagonal (14) is cheaper than two orthogonal steps
// (20). A* needs that to be admissible -- an overestimating heuristic returns a
// path that exists but is not the shortest.
int OctileDistance(const GamePosition &from, const GamePosition &to) {
  const int dx = std::abs(from.x - to.x);
  const int dy = std::abs(from.y - to.y);
  return ORTHOGONAL_COST * std::abs(dx - dy) + DIAGONAL_COST * std::min(dx, dy);
}

// Whether a character standing on `from` may step in direction (dx, dy).
//
// Every cell is treated as fully solid: a character occupies its whole cell, so
// a diagonal step passes the corner shared by the two cells beside it. If either
// of those is blocked, the diagonal is a squeeze that is not physically
// possible -- an actor does not fit through a gap that has no width. Requiring
// both is the strict rule; allowing one would let a character slide past a wall
// it is walking along, and would still be wrong for the corner between two
// objects that touch only at a corner.
//
// The check belongs here rather than at the call sites so that every route the
// game walks -- including the ones a moving target re-runs each frame -- obeys
// the same rule.
bool CornerIsClear(const Map &map, const GamePosition &from, int dx, int dy) {
  if (dx == 0 || dy == 0) {
    return true;  // orthogonal steps have no corner to pass
  }
  return !map.IsBlocked(from.x + dx, from.y) &&
         !map.IsBlocked(from.x, from.y + dy);
}

}  // namespace

std::vector<GamePosition> AStar(Map *map, const GamePosition &startPos,
                                const GamePosition &endPos) {
  std::vector<GamePosition> path;

  // Nothing to find, or nowhere to look.
  if (map == nullptr || startPos == endPos) {
    return path;
  }

  const int width = map->GetWidth();
  const int height = map->GetHeight();
  if (width <= 0 || height <= 0) {
    return path;
  }

  // The destination has to be walkable: a route is a route into a cell, and a
  // blocked cell is not one an actor can stand on. Callers that want to end up
  // *next to* something ask for a free cell beside it.
  if (!map->IsInBounds(startPos.x, startPos.y) ||
      !map->IsInBounds(endPos.x, endPos.y) ||
      map->IsBlocked(endPos.x, endPos.y)) {
    return path;
  }

  const size_t cellCount =
      static_cast<size_t>(width) * static_cast<size_t>(height);
  const auto cellIndex = [width](const GamePosition &p) {
    return static_cast<size_t>(p.y) * static_cast<size_t>(width) +
           static_cast<size_t>(p.x);
  };
  const auto cellAt = [width](size_t index) {
    return GamePosition{static_cast<int>(index % static_cast<size_t>(width)),
                        static_cast<int>(index / static_cast<size_t>(width))};
  };

  // Flat arrays indexed like Map::tileMap instead of two searched node lists:
  // the old nodeHasPosition() scan made every neighbour lookup linear in the
  // number of nodes already generated, which is the hot loop of the whole
  // search. Parents are stored as indices, so no allocation or cleanup is
  // needed per node and there is no list to leak.
  std::vector<int> gScore(cellCount, UNREACHABLE);
  std::vector<int> parent(cellCount, -1);
  std::vector<char> closed(cellCount, 0);

  struct Frontier {
    int fCost;
    int gCost;
    size_t index;
  };
  // std::priority_queue pops the "largest" element, so the comparator says
  // which node is *worse*: the one with the bigger f. Ties prefer the deeper
  // node (larger g), which is the usual way to keep A* from spreading a wide
  // plateau of equal-cost cells before it commits to one direction.
  struct CheapestFirst {
    bool operator()(const Frontier &a, const Frontier &b) const {
      if (a.fCost != b.fCost) {
        return a.fCost > b.fCost;
      }
      return a.gCost < b.gCost;
    }
  };
  std::priority_queue<Frontier, std::vector<Frontier>, CheapestFirst> open;

  const size_t startIndex = cellIndex(startPos);
  const size_t goalIndex = cellIndex(endPos);

  gScore[startIndex] = 0;
  open.push({OctileDistance(startPos, endPos), 0, startIndex});

  while (!open.empty()) {
    const Frontier current = open.top();
    open.pop();

    // Entries are never removed or updated in place; a cell that is improved
    // gets a second entry instead. The older, more expensive one is dropped here
    // by seeing that the cell is already settled, or that a cheaper route to it
    // has since been found.
    if (closed[current.index] || current.gCost != gScore[current.index]) {
      continue;
    }
    closed[current.index] = 1;

    if (current.index == goalIndex) {
      for (int at = static_cast<int>(goalIndex);
           at != -1 && static_cast<size_t>(at) != startIndex;
           at = parent[at]) {
        path.push_back(cellAt(static_cast<size_t>(at)));
      }
      std::reverse(path.begin(), path.end());
      return path;
    }

    const GamePosition from = cellAt(current.index);
    for (const Step &step : STEPS) {
      const GamePosition to{from.x + step.dx, from.y + step.dy};
      if (!map->IsInBounds(to.x, to.y)) {
        continue;
      }
      const size_t toIndex = cellIndex(to);
      if (closed[toIndex] || map->IsBlocked(to.x, to.y) ||
          !CornerIsClear(*map, from, step.dx, step.dy)) {
        continue;
      }

      const int candidate = gScore[current.index] + step.cost;
      if (candidate >= gScore[toIndex]) {
        continue;
      }

      gScore[toIndex] = candidate;
      parent[toIndex] = static_cast<int>(current.index);
      open.push({candidate + OctileDistance(to, endPos), candidate, toIndex});
    }
  }

  // The open set drained without reaching the goal: no walkable route exists
  // under the corner rule. Empty means exactly that to every caller.
  return path;
}
