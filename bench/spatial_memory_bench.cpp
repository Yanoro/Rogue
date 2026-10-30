// Standalone reproduction of the once-per-sample spatial memory derivation.
//
// It loads a real map and the real content registries, builds the cell and
// object record the vision writer would have produced after exploring that map,
// then runs `Memory::DerivePlaces` / `Memory::DeriveRegions` along a walk path
// and reports the per-sample cost. The point is to iterate on the derivation
// without launching the game: same data, same code, no window.
//
// Usage: SpatialMemoryBench [map.json] [samples] [active]
//   active=1 (default): force a changed observation each sample, so the heavy
//   path is measured. active=0: re-observe identical ground, which both passes
//   may skip.
//
// Not part of the game. Built as its own target so the derivation stays free of
// benchmark-only code.

#include "SpatialMemory.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {

using nlohmann::json;

struct TileDefLite {
  std::string name;
  bool blocksTile = false;
  std::vector<SeedCategory> categories;
};

struct ObjectDefLite {
  std::string name;
  bool structural = false;
  bool portable = false;
  std::vector<SeedCategory> categories;
};

std::vector<SeedCategory> ParseCategories(const json &value) {
  std::vector<SeedCategory> categories;
  if (!value.is_object()) {
    return categories;
  }
  for (auto it = value.begin(); it != value.end(); ++it) {
    if (!it.value().is_number()) {
      continue;
    }
    SeedCategory category;
    category.category = it.key();
    category.weight = it.value().get<float>();
    categories.push_back(std::move(category));
  }
  return categories;
}

bool HasComponent(const json &templ, const std::string &component) {
  if (!templ.contains("components") || !templ["components"].is_array()) {
    return false;
  }
  for (const auto &entry : templ["components"]) {
    if (entry.is_string() && entry.get<std::string>() == component) {
      return true;
    }
  }
  return false;
}

std::map<std::string, TileDefLite> LoadTileCatalog() {
  std::map<std::string, TileDefLite> tiles;
  const std::filesystem::path directory("data/tiles");
  if (!std::filesystem::exists(directory)) {
    std::cerr << "missing " << directory << "\n";
    return tiles;
  }
  for (const auto &entry : std::filesystem::directory_iterator(directory)) {
    if (entry.path().extension() != ".json") {
      continue;
    }
    std::ifstream file(entry.path());
    if (!file.is_open()) {
      continue;
    }
    json root;
    try {
      file >> root;
    } catch (const std::exception &error) {
      std::cerr << "cannot parse " << entry.path() << ": " << error.what()
                << "\n";
      continue;
    }
    TileDefLite tile;
    tile.name = root.value("name", entry.path().stem().string());
    tile.blocksTile = root.value("blocksTile", false);
    tile.categories = ParseCategories(root.value("placeCategories", json()));
    tiles[tile.name] = std::move(tile);
  }
  return tiles;
}

std::map<std::string, ObjectDefLite> LoadObjectCatalog() {
  std::map<std::string, ObjectDefLite> objects;
  const std::filesystem::path directory("data/objects");
  if (!std::filesystem::exists(directory)) {
    std::cerr << "missing " << directory << "\n";
    return objects;
  }
  for (const auto &entry : std::filesystem::directory_iterator(directory)) {
    if (entry.path().extension() != ".json") {
      continue;
    }
    std::ifstream file(entry.path());
    if (!file.is_open()) {
      continue;
    }
    json root;
    try {
      file >> root;
    } catch (const std::exception &error) {
      std::cerr << "cannot parse " << entry.path() << ": " << error.what()
                << "\n";
      continue;
    }
    ObjectDefLite object;
    object.name = root.value("name", entry.path().stem().string());
    object.structural = HasComponent(root, "Structural");
    object.portable = HasComponent(root, "Portable");
    object.categories = ParseCategories(root.value("placeCategories", json()));
    objects[entry.path().stem().string()] = std::move(object);
  }
  return objects;
}

// The observation record a character that had walked the whole map would hold:
// every authored cell, every authored object. Deliberately the worst case the
// cell and object caps allow -- a real walk is a subset of this.
void FillMemory(const json &map, const std::map<std::string, TileDefLite> &tiles,
                const std::map<std::string, ObjectDefLite> &objectDefs,
                SpatialMemory &memory) {
  const int width = map.value("mapWidth", 0);
  const int height = map.value("mapHeight", 0);

  std::vector<TileDefLite> palette;
  const json &paletteJson = map.contains("tilePalette")
                                ? map["tilePalette"]
                                : map.value("tileInfo", json::array());
  for (const auto &entry : paletteJson) {
    std::string name;
    TileDefLite tile;
    if (entry.is_string()) {
      name = entry.get<std::string>();
      const auto found = tiles.find(name);
      if (found != tiles.end()) {
        tile = found->second;
      } else {
        tile.name = name;
      }
    } else {
      name = entry.value("name", std::string());
      const auto found = tiles.find(name);
      if (found != tiles.end()) {
        tile = found->second;
      } else {
        tile.name = name;
      }
      if (entry.contains("blocksTile")) {
        tile.blocksTile = entry.value("blocksTile", tile.blocksTile);
      }
      if (entry.contains("placeCategories")) {
        tile.categories = ParseCategories(entry["placeCategories"]);
      }
    }
    palette.push_back(std::move(tile));
  }

  const json &positions = map.value("positions", json::array());
  int index = 0;
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x, ++index) {
      if (index >= static_cast<int>(positions.size())) {
        break;
      }
      const int pos = positions[index];
      if (pos < 0 || pos >= static_cast<int>(palette.size())) {
        continue;
      }
      const TileDefLite &tile = palette[pos];
      MemoryCell cell;
      cell.pos = {x, y};
      cell.surface = tile.name;
      cell.walkable = !tile.blocksTile;
      cell.categories = tile.categories;
      cell.observedTick = 1;
      cell.visitCount = 1;
      memory.cells.emplace(VisionQuery::CellKey(cell.pos), std::move(cell));
      ++memory.discoveredCells;
    }
  }

  flecs::entity_t nextId = 1;
  const json &objects = map.value("objects", json::array());
  for (const auto &entry : objects) {
    const std::string type = entry.value("type", std::string());
    const auto found = objectDefs.find(type);
    if (found == objectDefs.end()) {
      continue;
    }
    SeenObject object;
    object.templateId = type;
    object.name = found->second.name;
    object.pos = {entry.value("x", 0), entry.value("y", 0)};
    object.structural = found->second.structural;
    object.portable = found->second.portable;
    object.categories = found->second.categories;
    object.firstSeenTick = 1;
    object.lastSeenTick = 1;
    memory.objects.emplace(nextId++, std::move(object));
  }
}

std::vector<GamePosition> WalkPath(const json &map, int samples) {
  // A one-cell-per-sample loop around the authored village. The engine samples
  // vision every 30 ticks and a walking character covers a cell in that time, so
  // a sample that teleports half the map overstates how much a region's band
  // membership changes between passes.
  std::vector<GamePosition> path;
  int minX = map.value("mapWidth", 64);
  int minY = map.value("mapHeight", 64);
  int maxX = 0;
  int maxY = 0;
  for (const auto &entry : map.value("objects", json::array())) {
    minX = std::min(minX, entry.value("x", 0));
    minY = std::min(minY, entry.value("y", 0));
    maxX = std::max(maxX, entry.value("x", 0));
    maxY = std::max(maxY, entry.value("y", 0));
  }
  const int midX = (minX + maxX) / 2;
  const int midY = (minY + maxY) / 2;
  const int half = 6;
  GamePosition cell{midX - half, midY - half};
  int dx = 1;
  int dy = 0;
  int leg = 0;
  for (int i = 0; i < samples; ++i) {
    path.push_back(cell);
    cell.x += dx;
    cell.y += dy;
    ++leg;
    if (leg >= 2 * half) {
      leg = 0;
      const int nextDx = -dy;
      const int nextDy = dx;
      dx = nextDx;
      dy = nextDy;
    }
  }
  return path;
}

double Milliseconds(std::chrono::steady_clock::time_point a,
                    std::chrono::steady_clock::time_point b) {
  return std::chrono::duration<double, std::milli>(b - a).count();
}

}  // namespace

int main(int argc, char **argv) {
  const std::string mapPath =
      argc > 1 ? argv[1] : "maps/Map_Village_Quest.json";
  const int samples = argc > 2 ? std::max(1, std::atoi(argv[2])) : 40;
  // 1 = stand in for a sample that observed new ground, so both passes always
  // run. 0 = re-observe identical ground, which both passes may skip entirely.
  const bool active = argc > 3 ? std::atoi(argv[3]) != 0 : true;

  std::ifstream mapFile(mapPath);
  if (!mapFile.is_open()) {
    std::cerr << "cannot open " << mapPath << "\n";
    return 1;
  }
  json map;
  mapFile >> map;

  const auto tiles = LoadTileCatalog();
  const auto objectDefs = LoadObjectCatalog();

  PlaceCategoryRegistry registry;
  registry.Load("data/places.json");

  SpatialMemory memory;
  FillMemory(map, tiles, objectDefs, memory);
  // The writer bumps this as it records cells; FillMemory writes the records
  // directly, so stand in for the observation that created them.
  memory.contentRevision = 1;
  const std::vector<GamePosition> path = WalkPath(map, samples);

  std::vector<double> placesMs;
  std::vector<double> regionsMs;
  std::vector<Memory::DerivationProfile> profiles;
  placesMs.reserve(samples);
  regionsMs.reserve(samples);
  profiles.reserve(samples);

  int tick = 1;
  for (const GamePosition here : path) {
    ++tick;
    if (active) {
      // A new observation would have bumped the revision; this keeps the heavy
      // path being measured instead of both passes short-circuiting.
      ++memory.contentRevision;
    }
    Memory::DerivationProfile profile;
    const auto beforePlaces = std::chrono::steady_clock::now();
    Memory::DerivePlaces(memory, registry, tick, &profile);
    const auto afterPlaces = std::chrono::steady_clock::now();
    Memory::DeriveRegions(memory, registry, here, tick, 8, 12, 16, 24, 32, 48,
                          64, &profile);
    const auto afterRegions = std::chrono::steady_clock::now();
    placesMs.push_back(Milliseconds(beforePlaces, afterPlaces));
    regionsMs.push_back(Milliseconds(afterPlaces, afterRegions));
    profiles.push_back(profile);
  }

  const auto median = [](std::vector<double> values) {
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
  };
  const auto total = [](const std::vector<double> &values) {
    double sum = 0.0;
    for (const double value : values) {
      sum += value;
    }
    return sum;
  };
  const auto maximum = [](const std::vector<double> &values) {
    return *std::max_element(values.begin(), values.end());
  };

  std::cout << "map=" << mapPath << " samples=" << samples
            << " active=" << (active ? 1 : 0)
            << " cells=" << memory.cells.size()
            << " objects=" << memory.objects.size()
            << " places=" << memory.places.size()
            << " regions=" << memory.regions.size() << "\n";
  std::cout << "derivePlaces  median " << median(placesMs) << " ms  mean "
            << total(placesMs) / placesMs.size() << " ms  max "
            << maximum(placesMs) << " ms\n";
  std::cout << "deriveRegions median " << median(regionsMs) << " ms  mean "
            << total(regionsMs) / regionsMs.size() << " ms  max "
            << maximum(regionsMs) << " ms\n";

  const auto profileMedian = [&profiles](
                                 double Memory::DerivationProfile::*field) {
    std::vector<double> values;
    values.reserve(profiles.size());
    for (const Memory::DerivationProfile &profile : profiles) {
      values.push_back(profile.*field);
    }
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
  };
  const Memory::DerivationProfile &last = profiles.back();
  std::cout << "  places seeds=" << last.placesSeeds
            << " rebuilds=" << last.placesAspectRebuilds
            << " seed=" << profileMedian(&Memory::DerivationProfile::placesSeedMs)
            << " cluster="
            << profileMedian(&Memory::DerivationProfile::placesClusterMs)
            << " (union "
            << profileMedian(&Memory::DerivationProfile::placesUnionMs)
            << " flood "
            << profileMedian(&Memory::DerivationProfile::placesFloodMs)
            << " split "
            << profileMedian(&Memory::DerivationProfile::placesSplitMs)
            << ")"
            << " build=" << profileMedian(&Memory::DerivationProfile::placesBuildMs)
            << " (aspects "
            << profileMedian(&Memory::DerivationProfile::placesAspectsMs)
            << ") facts="
            << profileMedian(&Memory::DerivationProfile::placesFactsMs) << "\n";
  std::cout << "  regions refreshed=" << last.regionsRefreshed
            << " retained=" << last.regionsRetained
            << " rebuilds=" << last.regionsAspectRebuilds
            << " cells="
            << profileMedian(&Memory::DerivationProfile::regionsCellsMs)
            << " setup="
            << profileMedian(&Memory::DerivationProfile::regionsSetupMs)
            << " membership="
            << profileMedian(&Memory::DerivationProfile::regionsMembershipMs)
            << " signature="
            << profileMedian(&Memory::DerivationProfile::regionsSignatureMs)
            << " labels="
            << profileMedian(&Memory::DerivationProfile::regionsLabelsMs)
            << " build="
            << profileMedian(&Memory::DerivationProfile::regionsBuildMs)
            << " (aspects "
            << profileMedian(&Memory::DerivationProfile::regionsAspectsMs)
            << ") facts="
            << profileMedian(&Memory::DerivationProfile::regionsFactsMs)
            << " evict="
            << profileMedian(&Memory::DerivationProfile::regionsEvictMs) << "\n";
  std::cout << "  aspects calls=" << last.aspectsCalls
            << " object="
            << profileMedian(&Memory::DerivationProfile::aspectsObjectMs)
            << " tile="
            << profileMedian(&Memory::DerivationProfile::aspectsTileMs)
            << " surface="
            << profileMedian(&Memory::DerivationProfile::aspectsSurfaceMs)
            << " wall="
            << profileMedian(&Memory::DerivationProfile::aspectsWallMs)
            << " mean="
            << profileMedian(&Memory::DerivationProfile::aspectsMeanMs) << "\n";
  return 0;
}
