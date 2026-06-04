#include "Map.h"
#include "Game.h"
#include <fstream>
#include <iostream>

inline void from_json(const nlohmann::json &j, Color &c) {
  if (j.is_array() && j.size() >= 3) {
    c.r = j[0];
    c.g = j[1];
    c.b = j[2];
    c.a = j.size() > 3 ? j[3].get<unsigned char>() : (unsigned char)255;
  }
}

Map::Map(std::string jsonPath, flecs::world &ecs)
    : ecs(ecs) {
  std::ifstream jsonFile;
  jsonFile.exceptions(std::ifstream::failbit | std::ifstream::badbit);

  try {
    jsonFile.open(jsonPath);
  } catch (const std::ifstream::failure &e) {
    std::cerr << "Exception opening/reading file: " << e.what() << std::endl;
    throw;
  }

  try {
    auto json = nlohmann::json::parse(jsonFile);
    width = json["mapWidth"];
    height = json["mapHeight"];
    tileWidth = json["tileWidth"];
    tileHeight = json["tileHeight"];


    // This implicitily forces the tileInfo given to be given with contiguous
    // IDS,
    std::vector<nlohmann::json> tiles = json["tileInfo"];
    GamePosition currPos = {0, 0};
    
    for (const int pos : json["positions"]) {
      Tile newTile;
      ScreenPosition newScreenPosition =
          GameCoordsToScreenCoords(currPos.x, currPos.y);
      int tileId = pos;
      auto &currTileInfo = tiles[tileId];

      newTile.name = currTileInfo.value("name", "Unknown name");
      newTile.ch = currTileInfo.value("character", "?").at(0);
      newTile.characterColor = currTileInfo["characterColor"];
      newTile.backgroundColor = currTileInfo["backgroundColor"];
      
      auto blocksTile = (currTileInfo.value("blocksTile", false));

      auto entity = ecs.entity()
          .set<Tile>(newTile)
          .set<GamePosition>(currPos)
          .set<ScreenPosition>(newScreenPosition);

      if (blocksTile) {
        entity.add<BlocksTile>();
      }

      if (++currPos.y == height) {
        currPos.y = 0;
        currPos.x++;
      }
    }

  } catch (const std::exception &e) {
    std::cerr << "JSON Parse Error: " << e.what() << " jsonPath: " << jsonPath
              << std::endl;
    throw;
  }
}

int Map::GetIndex(int x, int y) const { return y * width + x; }

bool Map::IsInBounds(float x, float y) const {
  return (x >= 0 && x < width && y >= 0 && y < height);
}

//TODO: I have no idea why both functions take floats
ScreenPosition Map::GameCoordsToScreenCoords(float x, float y) const {
  return ScreenPosition(static_cast<int>(x * tileWidth),
                        static_cast<int>(y * tileHeight));
}

GamePosition Map::ScreenCoordsToGameCoords(float x, float y) const {
  return GamePosition(std::floor(x / tileWidth), std::floor(y / tileHeight));
}
