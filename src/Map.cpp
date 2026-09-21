#include "Map.h"
#include "Defaults.h"
#include "Game.h"
#include "raylib.h"
#include "ObjectFactory.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <memory>

inline void from_json(const nlohmann::json &j, Color &c) {
  if (j.is_array() && j.size() >= 3) {
    c.r = j[0];
    c.g = j[1];
    c.b = j[2];
    c.a = j.size() > 3 ? j[3].get<unsigned char>() : (unsigned char)255;
  }
}

Map::Map(flecs::entity mapEntity, std::string jsonPath, DebugLog* debugLog) 
  : ecs(mapEntity.world()), debugLog(debugLog) {

  std::ifstream jsonFile;
  jsonFile.exceptions(std::ifstream::failbit | std::ifstream::badbit);
  try {
    jsonFile.open(jsonPath);
  } catch (const std::ifstream::failure &e) {
    std::string errStr = "Exception opening/reading file: " + std::string(e.what()) + "\n";
    if (debugLog) debugLog->LogError(errStr);
    else std::cerr << errStr;
    throw;
  }


  try {
    auto json = nlohmann::json::parse(jsonFile);
    width = json["mapWidth"];
    height = json["mapHeight"];
    tileWidth = json["tileWidth"];
    tileHeight = json["tileHeight"];

    // Kept for saving: everything the editor does not own is written back from
    // here, so hand-written keys survive. See Map::BuildMapJson.
    rawMapJson = json;

    tileMap.resize(width * height);

    // This implicitily forces the tileInfo given to be given with contiguous
    // IDS

    for (const auto &tileInfo : json["tileInfo"]) {
      auto newTile = std::make_unique<Tile>();
      // TODO: Memory leak here
      DrawAscii *newAscii = new DrawAscii();
      newAscii->ch = tileInfo.value("character", "?").at(0);
      newAscii->characterColor = tileInfo["characterColor"];
      newAscii->backgroundColor = tileInfo["backgroundColor"];
      newAscii->width = tileWidth;
      newAscii->height = tileHeight;

      newTile->name = tileInfo.value("name", "Unknown name");
      newTile->blocksTile = tileInfo.value("blocksTile", false);
      newTile->ascii = newAscii;
      newTile->hitbox.width = tileInfo.value("hitboxWidth", tileWidth);
      newTile->hitbox.height = tileInfo.value("hitboxHeight", tileHeight);

      uniqueTiles.push_back(std::move(newTile));
    }


    for (const auto &currLoc : json["locations"]) {
      auto location = std::make_unique<Location>();
      location->name = currLoc.value("name", "Unknown location name");
      location->description =
          currLoc.value("description", "Unknown location Description");
      auto locPos = currLoc["position"];
      location->pos = {locPos[0], locPos[1]};
      location->width = currLoc.value("hitboxWidth", currLoc.value("width", width));
      location->height = currLoc.value("hitboxHeight", currLoc.value("height", height));

      mapLocations.push_back(std::move(location));
    }

    if (json.contains("npcs")) {
      for (const auto &npcJson : json["npcs"]) {
        NPCData data;
        data.name = npcJson.value("name", "NPC");
        auto pos = npcJson["position"];
        data.position = {pos[0], pos[1]};
        data.background = npcJson.value("background", "");

        if (npcJson.contains("inventory")) {
          for (const auto &itemJson : npcJson["inventory"]) {
            ItemStack stack;
            stack.item = itemJson.value("item", "");
            stack.count = itemJson.value("count", 1);
            if (stack.item.empty() || stack.count <= 0) {
              std::string warnStr =
                  "Warning: NPC '" + data.name +
                  "' has an invalid inventory entry (empty item or non-positive "
                  "count); skipping it.\n";
              if (debugLog) debugLog->LogWarning(warnStr);
              else std::cerr << warnStr;
              continue;
            }
            data.inventory.push_back(stack);
          }
        }

        npcs.push_back(data);
      }
    }

    GamePosition currPos = {0, 0};

    for (const int pos : json["positions"]) {
      Tile *currentTile = uniqueTiles[pos].get();
      addTileToMap(currentTile, currPos.x, currPos.y);

      ecs.entity()
          .child_of(mapEntity)
          .set<DrawAscii>(*currentTile->ascii)
          .set<ScreenPosition>(GameCoordsToScreenCoords(currPos.x, currPos.y));

      if (++currPos.y == height) {
        currPos.y = 0;
        currPos.x++;
      }
    }

    if (json.contains("objects")) {
      auto factoryRes = ecs.get<ObjectFactoryResource>();
      if (factoryRes && factoryRes->factory) {
        for (const auto &objData : json["objects"]) {
          std::string type = objData.value("type", "");
          int x = objData.value("x", 0);
          int y = objData.value("y", 0);
          if (!type.empty()) {
            // authored = true: these are the map's own objects, so the map
            // writer must write them back.
            factoryRes->factory->SpawnObject(ecs, mapEntity, this, type,
                                             GamePosition{x, y}, true);
          }
        }
      }
    }

  } catch (const std::exception &e) {
    std::string errStr = "JSON Parse Error: " + std::string(e.what()) + " jsonPath: " + jsonPath + "\n";
    if (debugLog) debugLog->LogError(errStr);
    else std::cerr << errStr;
    throw;
  }
}

nlohmann::json Map::BuildMapJson() const {
  // Start from the file as loaded so section keys the editor does not manage -
  // tileInfo above all - and any hand-written extras are carried through.
  nlohmann::json mapJson = rawMapJson.is_object() ? rawMapJson
                                                  : nlohmann::json::object();

  mapJson["mapWidth"] = width;
  mapJson["mapHeight"] = height;
  mapJson["tileWidth"] = tileWidth;
  mapJson["tileHeight"] = tileHeight;

  // Tile layout. tileMap is indexed y*width+x but the file stores it
  // column-major, which is the order Map::Map consumes; see the header note.
  nlohmann::json positions = nlohmann::json::array();
  for (int x = 0; x < width; ++x) {
    for (int y = 0; y < height; ++y) {
      const Tile *tile = tileMap[GetIndex(x, y)];
      int tileId = 0;
      for (size_t i = 0; i < uniqueTiles.size(); ++i) {
        if (uniqueTiles[i].get() == tile) {
          tileId = static_cast<int>(i);
          break;
        }
      }
      positions.push_back(tileId);
    }
  }
  mapJson["positions"] = std::move(positions);

  mapJson["locations"] = nlohmann::json::array();
  for (const auto &loc : mapLocations) {
    mapJson["locations"].push_back({
        {"name", loc->name},
        {"description", loc->description},
        {"position", {loc->pos.x, loc->pos.y}},
        {"hitboxWidth", loc->width},
        {"hitboxHeight", loc->height},
    });
  }

  // Only MapAuthored entities: entities the world spawned at runtime (harvest
  // drops, crafted items, NPC starting inventory) carry no marker and are left
  // out, so simulation state never leaks into the file. Positions are sorted so
  // the output depends on map content, not on ECS allocation order.
  struct AuthoredObject {
    std::string type;
    GamePosition pos;
  };
  std::vector<AuthoredObject> authoredObjects;
  ecs.each([&authoredObjects](flecs::entity, MapAuthored &authored,
                              const GamePosition &pos) {
    authoredObjects.push_back({authored.templateKey, pos});
  });
  std::sort(authoredObjects.begin(), authoredObjects.end(),
            [](const AuthoredObject &a, const AuthoredObject &b) {
              if (a.pos.y != b.pos.y) return a.pos.y < b.pos.y;
              if (a.pos.x != b.pos.x) return a.pos.x < b.pos.x;
              return a.type < b.type;
            });

  mapJson["objects"] = nlohmann::json::array();
  for (const AuthoredObject &obj : authoredObjects) {
    mapJson["objects"].push_back(
        {{"type", obj.type}, {"x", obj.pos.x}, {"y", obj.pos.y}});
  }

  // NPCs: the raw entries are already in the base document, so they are only
  // rebuilt when the loaded list no longer matches. Rebuilding unconditionally
  // from NPCData would drop "inventory", which NPCData does not model.
  if (!mapJson.contains("npcs") || !mapJson["npcs"].is_array() ||
      mapJson["npcs"].size() != npcs.size()) {
    mapJson["npcs"] = nlohmann::json::array();
    for (const NPCData &npc : npcs) {
      mapJson["npcs"].push_back({
          {"name", npc.name},
          {"position", {npc.position.x, npc.position.y}},
          {"background", npc.background},
      });
    }
  }

  return mapJson;
}

bool Map::SaveToFile(const std::string &path) {
  try {
    nlohmann::json mapJson = BuildMapJson();

    std::ofstream outFile(path);
    if (!outFile.is_open()) {
      std::string errStr = "Cannot open map file for writing: " + path + "\n";
      if (debugLog) debugLog->LogError(errStr);
      else std::cerr << errStr;
      return false;
    }

    outFile << mapJson.dump(2) << std::endl;
    outFile.close();

    if (debugLog) {
      debugLog->LogInfo("Map editor: wrote map to " + path + " (" +
                        std::to_string(width) + "x" + std::to_string(height) +
                        " tiles, " + std::to_string(mapLocations.size()) +
                        " locations, " +
                        std::to_string(mapJson["objects"].size()) + " objects)");
    }
    return true;
  } catch (const std::exception &e) {
    std::string errStr =
        "Failed to write map file " + path + ": " + std::string(e.what()) + "\n";
    if (debugLog) debugLog->LogError(errStr);
    else std::cerr << errStr;
    return false;
  }
}

void Map::addTileToMap(Tile *newTile, int x, int y) {
  tileMap[x + (y * width)] = newTile;
}

Location *Map::AddLocation(const std::string &name,
                           const std::string &description,
                           GamePosition topLeft, int width, int height) {
  auto location = std::make_unique<Location>();
  location->name = name;
  location->description = description;
  location->pos = topLeft;
  location->width = width;
  location->height = height;

  mapLocations.push_back(std::move(location));
  return mapLocations.back().get();
}

std::string Map::RemoveLocationAt(GamePosition pos) {
  for (auto it = mapLocations.begin(); it != mapLocations.end(); ++it) {
    const Location &loc = **it;
    // Same bounds test GetLocation uses, so removal hits exactly what a lookup
    // at this position would return.
    if (pos.x >= loc.pos.x && pos.x <= loc.pos.x + loc.width &&
        pos.y >= loc.pos.y && loc.height + loc.pos.y >= pos.y) {
      std::string removedName = loc.name;
      mapLocations.erase(it);
      return removedName;
    }
  }
  return "";
}

std::vector<GamePosition> Map::GetTilePositionsInRect(GamePosition topLeft,
                                                      int rectWidth,
                                                      int rectHeight) const {
  std::vector<GamePosition> tiles;
  for (int x = topLeft.x; x < topLeft.x + rectWidth; ++x) {
    for (int y = topLeft.y; y < topLeft.y + rectHeight; ++y) {
      if (IsInBounds(static_cast<float>(x), static_cast<float>(y))) {
        tiles.push_back({x, y});
      }
    }
  }
  return tiles;
}

Location *Map::GetLocation(GamePosition pos) {
  for (const auto &currLoc : mapLocations) {
    if (pos.x >= currLoc->pos.x && pos.x <= currLoc->pos.x + currLoc->width &&
        pos.y >= currLoc->pos.y && currLoc->height + currLoc->pos.y >= pos.y) {
      return currLoc.get();
    }
  }
  return nullptr;
}

Location *Map::GetLocation(const std::string& name) {
  for (const auto &currLoc : mapLocations) {
    if (currLoc->name == name) {
      return currLoc.get();
    }
  }
  return nullptr;
}

std::vector<std::string> Map::GetAllLocationNames() {
  std::vector<std::string> locations;
  for (const auto &currLoc : mapLocations) {
    locations.push_back(currLoc.get()->name);
  }
  return locations;
}

Tile *Map::GetTile(int x, int y) {
  if (!IsInBounds(x, y)) {
    return nullptr;
  }
  return tileMap[GetIndex(x, y)];
}
int Map::GetIndex(int x, int y) const { return y * width + x; }

std::vector<Tile *> Map::GetNeighbours(GamePosition p) {
  static const int dx[] = {-1, 0, 1, -1, 1, -1, 0, 1};
  static const int dy[] = {-1, -1, -1, 0, 0, 1, 1, 1};

  std::vector<Tile *> neighbors;
  for (int i = 0; i < 8; ++i) {
    neighbors.push_back(GetTile(p.x + dx[i], p.y + dy[i]));
  }
  return neighbors;
}

bool Map::AreNeighbours(GamePosition p1, GamePosition p2) {
  int dx = p1.x - p2.x;
  if (dx < 0) dx = -dx;
  
  int dy = p1.y - p2.y;
  if (dy < 0) dy = -dy;
  
  return (dx <= 1 && dy <= 1) && (dx == 1 || dy == 1);
}

bool Map::IsInBounds(float x, float y) const {
  return (x >= 0 && x < width && y >= 0 && y < height);
}

bool Map::InsideScreenMap(Rectangle rect) const {
  return (rect.x >= 0) && (rect.y >= 0) &&
         ((rect.x + rect.width) <= GetMapWidthPx()) &&
         ((rect.y + rect.height) <= GetMapHeightPx());
}

// TODO: I have no idea why both functions take floats
ScreenPosition Map::GameCoordsToScreenCoords(float x, float y) const {
  return ScreenPosition(std::floor(static_cast<int>(x * tileWidth)),
                        std::floor(static_cast<int>(y * tileHeight)));
}

GamePosition Map::ScreenCoordsToGameCoords(float x, float y) const {
  return GamePosition(std::floor(x / tileWidth), std::floor(y / tileHeight));
}


