#include "Map.h"

Map::Map(std::string jsonPath, ResourceManager &rm) : resourceManager(rm) {
  std::ifstream jsonFile;
  jsonFile.exceptions(std::ifstream::failbit | std::ifstream::badbit);

  try {
    jsonFile.open(jsonPath);
  }
  catch (const std::ifstream::failure &e) {
    std::cerr << "Exception opening/reading file: " << e.what() << std::endl;
    throw;
  }

  try {
    auto json = nlohmann::json::parse(jsonFile);
    width = json["width"];
    height = json["height"];
    tileWidth = json["tilewidth"];
    tileHeight = json["tileheight"];
 
    tiles.resize(width * height);
    tileSets.reserve(json["tilesets"].size());
    for (const auto &tileSetJson : json["tilesets"]) {
      tileSets.emplace_back(
        tileSetJson["name"],
        resourceManager.GetTexture(tileSetJson["image"]),
        tileSetJson["firstgid"],
        tileSetJson["tilecount"],
        tileSetJson["columns"]
      );
    }

    for (const auto &layer: json["layers"]) {
      if (layer["type"] == "tilelayer") {
        mapData.reserve(layer["data"].size());
        for (const auto &tileGID : layer["data"]) {
          mapData.push_back(tileGID);
        }
      }
    } 
  } 
  catch (const std::exception& e) {
    std::cerr << "JSON Parse Error: " << e.what() << " jsonPath: " << jsonPath << std::endl;
    throw;
  }
}

TileSet *Map::gidToTileSet(unsigned int gid) {
  auto texture = gidToTileSetMap.find(gid);
  if (texture != gidToTileSetMap.end()) {
    return texture->second;
  }
  for (auto &tileSet : tileSets) {
    if (gid >= tileSet.firstGID and gid < (tileSet.firstGID + tileSet.tileCount)) {
      gidToTileSetMap[gid] = &tileSet;
      return gidToTileSetMap[gid];
    }
  }

  return nullptr;
}

int Map::GetIndex(int x, int y) const {
  return y * width + x;
}

bool Map::IsInBounds(int x, int y) const {
  return (x >= 0 && x < width && y >= 0 && y < height);
}

ScreenCoords Map::MapCoordsToScreenCoords(int x, int y) const {
  return std::make_pair(x * tileWidth, y * tileHeight);    
}

// Tile& Map::GetTile(int x, int y) {
//   if (!IsInBounds(x, y)) {
//     static Tile voidTile = Tile::Void();
//     return voidTile;
//   }
//   return tiles[GetIndex(x, y)];
// }

void Map::SetTile(int x, int y, Tile tile) {
  if (IsInBounds(x, y)) {
    tiles[GetIndex(x, y)] = tile;
  }
}

void Map::Draw() {
  int x, y;
  x = 0;
  y = 0;
  for (auto &tileGID : mapData) {
    TileSet *tileSet = gidToTileSet(tileGID);
    if (tileSet == nullptr) {
      std::cerr << "Map::Draw() could not find tileSet for gid: " << tileGID << std::endl;
      throw;
    }

    int localID, srcX, srcY;
    localID = tileGID - tileSet->firstGID;
    srcX = (localID % tileSet->columns) * tileWidth;
    srcY = (localID / tileSet->columns) * tileHeight;

    raylib::Texture *texture = tileSet->texture;
    raylib::Rectangle srcRect(srcX, srcY, tileWidth, tileHeight);
    raylib::Vector2 pos(x * tileWidth, y * tileWidth);

    texture->Draw(srcRect, pos);

    x++;
    if (x >= width) {
      x = 0;
      y++;
    }
  }
}
