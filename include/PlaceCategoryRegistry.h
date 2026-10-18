#pragma once

#include <string>
#include <unordered_map>
#include <vector>

class DebugLog;

// The vocabulary of place categories, and the only place a genre adds one
// (docs/place-derivation-design.md section 4).
//
// A category is the engine's statement of what it can cluster seeds on, not a
// constraint on what the world may contain: an unknown category contributes
// nothing and is warned about once at load, so a misspelt "orcards" is loud
// instead of silently producing a category nobody can configure.
//
// The distinction that matters is `formsPlace`. A category that forms places is a
// *kind* (market, orchard, road, water). One that does not is a *quality* (poor,
// dirty, shady) and only ever contributes to the place it is inside or borders --
// which is why a wall's condition is a property of the courtyard rather than a
// chain of "poor places" across the village.
struct PlaceCategory {
  std::string id;

  // False for qualities. A category that cannot form a place is not a failure of
  // configuration; it is the normal case for adjectives.
  bool formsPlace = false;

  // How many seeds a component needs before it is a place. One tree is an orchard;
  // one stall is not yet a market.
  int minSeeds = 1;

  // Chebyshev distance between seeds that belong to the same component, and the
  // outer bound on how far a place's extent may reach from its seeds.
  int radius = 4;

  // How a connected component is cut into places. Empty means "not cut".
  // "junctions" is for networks like roads (section 4.2).
  std::string split;

  // Whether seeds of this category may join seeds of *other* categories in one
  // place. False for linear terrain: a road or a lake is its own region, and
  // without this one path across the village chains every object within the link
  // radius into a single place (section 5.1).
  bool linksAcross = true;
};

class PlaceCategoryRegistry {
public:
  void SetDebugLog(DebugLog *log) { debugLog = log; }

  // Reads {"market": {"formsPlace": true, "minSeeds": 3, "radius": 5}, ...}.
  //
  // A missing file is a warning and leaves the registry empty, exactly like the
  // other registries: the game still runs, places simply never form, and the
  // warning names the path that was looked for.
  void Load(const std::string &filePath);

  const PlaceCategory *Get(const std::string &id) const;
  bool IsKnown(const std::string &id) const { return Get(id) != nullptr; }
  const std::vector<PlaceCategory> &All() const { return categories; }
  std::size_t Size() const { return categories.size(); }

private:
  void Warn(const std::string &message) const;

  DebugLog *debugLog = nullptr;
  std::vector<PlaceCategory> categories;
  std::unordered_map<std::string, std::size_t> byId;
};

// Registered as a resource component, alongside SkillRegistryResource and the
// rest, and populated once at init before any map is loaded.
struct PlaceCategoryRegistryResource {
  PlaceCategoryRegistry *registry;
};
