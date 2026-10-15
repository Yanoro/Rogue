#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "Components.h"

class DebugLog;

// Content database for tile *definitions* -- what a tile named "wooden wall"
// is, independently of any map that uses it.
//
// Why this exists. Tile definitions used to live only inside each map's
// "tileInfo" array, and cells are indices into that array (Map::uniqueTiles).
// That made a tile a property of one map rather than of the world, with three
// consequences that only became visible once something wanted to *reference* a
// tile instead of owning it:
//
//   1. Every map re-declared the same tiles, and they could disagree. In this
//      repo they already did: "path" is '.' on Map_Farm and ':' on Map_Smithy,
//      and "wooden wall" uses characterColor 255,255,255 on the A* maps and
//      200,200,200 on the village maps.
//   2. A blueprint or an agent that is not inside a map has no vocabulary to
//      name a surface with. The designer agent is global by decision, so it
//      could not answer "what can I build with?" at all.
//   3. A genre pack cannot ship a tile vocabulary, because there was no
//      cross-map place for one to live.
//
// So the catalog is the world's vocabulary and a map's "tileInfo" is demoted to
// per-map *overrides*. A map that wants the smithy's ':' path still says so;
// it just no longer has to restate the colors, and every map that says nothing
// now agrees. Map::BuildMapJson writes only the overrides back, so a save does
// not re-duplicate the catalog into the map file.
//
// Same shape as StatRegistry / SkillRegistry: a plain content database loaded
// from a directory of .json files, injected where needed, reachable through a
// resource component (TileRegistryResource).
class TileRegistry {
public:
  // Reads every .json in `directoryPath`. Malformed files are reported through
  // the injected DebugLog (or stderr) rather than aborting the load. A missing
  // directory is a warning, not an error: a map that declares every tile it
  // uses inline still works, which is how every map predates this class.
  void LoadTiles(const std::string& directoryPath);

  // nullptr for an unknown name. The pointer is stable for the registry's
  // lifetime.
  const TileDef* Get(const std::string& name) const;

  // Sorted, so a material palette can be built deterministically for prompts
  // and debug UIs.
  std::vector<std::string> GetNames() const;

  // Every definition, sorted by name.
  std::vector<const TileDef*> GetAll() const;

  size_t Count() const { return tiles.size(); }

  void SetDebugLog(DebugLog* log) { debugLog = log; }

private:
  std::unordered_map<std::string, TileDef> tiles;
  DebugLog* debugLog = nullptr;
};
