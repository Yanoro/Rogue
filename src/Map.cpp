#include "Map.h"
#include "Game.h"
#include <fstream>
#include <iostream>

Map::Map(std::string jsonPath, ResourceManager &rm, flecs::world &ecs)
    : resourceManager(rm), ecs(ecs) {
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
    width = json["width"];
    height = json["height"];
    tileWidth = json["tilewidth"];
    tileHeight = json["tileheight"];

    for (const auto &layer : json["layers"]) {
      if (layer["type"] != "tilelayer") {
        continue;
      }

      int x = 0;
      int y = 0;
      for (const unsigned int &tileGID : layer["data"]) {
        for (const auto &tileSetJson : json["tilesets"]) {
          unsigned int firstGID = tileSetJson["firstgid"];
          unsigned int tileCount = tileSetJson["tilecount"];

          if (tileGID < firstGID || tileGID >= (firstGID + tileCount)) {
            continue;
          }

          raylib::Texture *texture =
              resourceManager.GetTexture(tileSetJson["image"]);
          int columns = tileSetJson["columns"];
          int localID = tileGID - firstGID;
          int srcX = (localID % columns) * tileWidth;
          int srcY = (localID / columns) * tileHeight;

          raylib::Rectangle srcRect(srcX, srcY, tileWidth, tileHeight);
          raylib::Vector2 pos(x * tileWidth, y * tileHeight);

          auto entity = ecs.entity()
                            .add<Tile>()
                            .set<Drawable>({texture, srcRect})
                            .set<ScreenPosition>({pos.x, pos.y})
                            .set<GamePosition>({x, y});

          if (tileSetJson.contains("tiles")) {
            for (const auto &tileDef : tileSetJson["tiles"]) {
              if (tileDef.contains("id") && tileDef["id"] == localID) {
                if (tileDef.contains("properties")) {
                  for (const auto &prop : tileDef["properties"]) {
                    if (prop.contains("name") && prop["name"] == "BlocksTile" &&
                        prop.contains("value") && prop["value"] == true) {
                      entity.add<BlocksTile>();
                      break;
                    }
                  }
                }
                break; // Found the specific tile definition, no need to keep
                       // searching tiles
              }
            }
          }

          break;
        }

        x++;
        if (x >= width) {
          x = 0;
          y++;
        }
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

ScreenPosition Map::GameCoordsToScreenCoords(float x, float y) const {
  return ScreenPosition(static_cast<int>(x * tileWidth),
                        static_cast<int>(y * tileHeight));
}

GamePosition Map::ScreenCoordsToGameCoords(float x, float y) const {
  return GamePosition(std::floor(x / tileWidth),
                      std::floor(y / tileHeight));
}

