#include "Map.h"
#include "Defaults.h"
#include "Game.h"
#include "raylib.h"
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

Map::Map(std::string jsonPath, flecs::world &ecs) : ecs(ecs) {
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

    tileMap.resize(width * height);

    // This implicitily forces the tileInfo given to be given with contiguous
    // IDS,
    std::vector<Tile *> tiles;

    for (const auto &tileInfo : json["tileInfo"]) {
      Tile *newTile = new Tile();
      DrawAscii *newAscii = new DrawAscii();
      newAscii->ch = tileInfo.value("character", "?").at(0);
      newAscii->characterColor = tileInfo["characterColor"];
      newAscii->backgroundColor = tileInfo["backgroundColor"];
      newAscii->width = tileWidth;
      newAscii->height = tileHeight;

      newTile->name = tileInfo.value("name", "Unknown name");
      newTile->blocksTile = tileInfo.value("blocksTile", false);
      newTile->ascii = newAscii;
      tiles.push_back(newTile);
    }

    GamePosition currPos = {0, 0};

    for (const int pos : json["positions"]) {
      addTileToMap(tiles[pos], currPos.x, currPos.y);
      ScreenPosition screenPos = GameCoordsToScreenCoords(currPos.x, currPos.y);

      ecs.entity()
          .set<DrawAscii>(*tiles[pos]->ascii)
          .set<ScreenPosition>(GameCoordsToScreenCoords(currPos.x, currPos.y));

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

void Map::addTileToMap(Tile *newTile, int x, int y) {
  tileMap[x + (y * width)] = newTile;
}
                              
Tile *Map::GetTile(int x, int y) {
  if (!IsInBounds(x, y)) {
    return nullptr;
  }
  return tileMap[GetIndex(x, y)];
}
int Map::GetIndex(int x, int y) const { return y * width + x; }
 
std::vector<Tile*> Map::GetNeighbours(GamePosition p) {
  static const int dx[] = {-1,  0,  1, -1, 1, -1, 0, 1};
  static const int dy[] = {-1, -1, -1,  0, 0,  1, 1, 1};

  std::vector<Tile*> neighbors;
  for(int i = 0; i < 8; ++i) {
    neighbors.push_back(GetTile(p.x + dx[i], p.y + dy[i]));
  }
  return neighbors;
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

