#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "Components.h"
#include "SpriteDraw.hpp"
#include "SpritePaths.hpp"
#include "TileRegistry.h"
#include "imgui.h"
#include "raylib.h"

// A map file read and drawn without loading it into the world.
//
// The Map Reload window needs to show a map before it is chosen, and loading it
// through Map would build tile entities the world does not want yet. This peeks at
// the same JSON Map reads and resolves the palette the same way -- so what the
// preview shows is what the map will look like, not an approximation of it.
//
// Drawing follows BlueprintPreview exactly: a background plate from the tile's own
// colour, then the sprite through ResolveSpriteTexture and SpriteFitRect, then the
// glyph when neither is available. It is deliberately a sibling of that header
// rather than a generalisation of it, because a blueprint has no positions array
// and a map has no object list to expand; what they share is the cell drawing, and
// a shared helper for that would be larger than these two loops.
namespace MapPreview {

struct Info {
  bool valid = false;
  std::string error;
  // The file this came from, and the name shown for it: "name" from the map when
  // authored, the file stem otherwise.
  std::string path;
  std::string name;
  std::string description;

  int width = 0;
  int height = 0;
  int tileWidth = 0;
  int tileHeight = 0;

  // Palette entries with the map's overrides already applied to a copy of the
  // catalog definition, so a cell only has to index this.
  std::vector<TileDef> palette;
  // width*height, column-major (index = x*height + y) exactly as "positions"
  // stores it. -1 is the editor's spelling for an erased cell.
  std::vector<int> cells;

  int objectCount = 0;
  int npcCount = 0;
  int placeCount = 0;
  bool hasPlayerStart = false;
  GamePosition playerStart{0, 0};
};

// "characterColor": [r, g, b, a], the same shape Map's own from_json reads. Parsed
// by hand rather than through a JSON conversion so this header does not define a
// global overload that Map.cpp also defines.
inline Color ParseColor(const nlohmann::json &json, Color fallback) {
  if (!json.is_array() || json.size() < 3) return fallback;
  const auto channel = [&json](size_t index) {
    const int value = json[index].is_number() ? json[index].get<int>() : 0;
    return static_cast<unsigned char>(std::clamp(value, 0, 255));
  };
  const unsigned char alpha =
      json.size() >= 4 ? channel(3) : static_cast<unsigned char>(255);
  return Color{channel(0), channel(1), channel(2), alpha};
}

// Inherit the catalog definition, then let the palette object override the fields a
// preview can draw: glyph, colours and sprite. Everything else in an override (place
// categories, hitbox) changes behaviour, not appearance.
inline TileDef ResolveEntry(const nlohmann::json &entry, const TileRegistry &tiles) {
  nlohmann::json object;
  if (entry.is_string()) {
    object["name"] = entry.get<std::string>();
  } else if (entry.is_object()) {
    object = entry;
  } else {
    object["name"] = "Unknown name";
  }

  const std::string name = object.value("name", std::string("Unknown name"));
  TileDef definition;
  if (const TileDef *catalog = tiles.Get(name)) {
    definition = *catalog;
  } else {
    // Map does this too: an unresolvable name draws as '?' rather than as nothing,
    // because an invisible tile is worse than a visibly wrong one.
    definition.name = name;
    definition.character = '?';
    definition.hasCharacter = true;
  }

  if (object.contains("character")) {
    const std::string glyph = object["character"].get<std::string>();
    definition.character = glyph.empty() ? '?' : glyph.at(0);
    definition.hasCharacter = true;
  }
  if (object.contains("characterColor")) {
    definition.characterColor =
        ParseColor(object["characterColor"], definition.characterColor);
    definition.hasCharacterColor = true;
  }
  if (object.contains("backgroundColor")) {
    definition.backgroundColor =
        ParseColor(object["backgroundColor"], definition.backgroundColor);
    definition.hasBackgroundColor = true;
  }
  if (object.contains("sprite")) {
    definition.sprite = object.value("sprite", std::string());
  }
  if (object.contains("blocksTile")) {
    definition.blocksTile = object.value("blocksTile", definition.blocksTile);
  }
  return definition;
}

// Reads the file. False (with a reason in `error`) for a missing file, malformed
// JSON or missing dimensions; the caller shows the reason instead of a preview.
inline bool Load(const std::string &path, const TileRegistry &tiles, Info &out,
                 std::string *error) {
  out = Info{};
  out.path = path;

  std::ifstream file(path);
  if (!file.is_open()) {
    if (error != nullptr) *error = "Could not open " + path;
    return false;
  }
  nlohmann::json json;
  try {
    file >> json;
  } catch (const std::exception &parseError) {
    if (error != nullptr) {
      *error = std::string("Not valid JSON: ") + parseError.what();
    }
    return false;
  }
  if (!json.is_object()) {
    if (error != nullptr) *error = "The file is not a JSON object";
    return false;
  }

  out.width = json.value("mapWidth", 0);
  out.height = json.value("mapHeight", 0);
  out.tileWidth = json.value("tileWidth", 32);
  out.tileHeight = json.value("tileHeight", 32);
  if (out.width <= 0 || out.height <= 0) {
    if (error != nullptr) *error = "The file has no positive mapWidth/mapHeight";
    return false;
  }

  const std::string stem =
      std::filesystem::path(path).stem().string();
  out.name = json.value("name", std::string());
  if (out.name.empty()) out.name = stem;
  out.description = json.value("description", std::string());

  // "tilePalette" is the current spelling; "tileInfo" is the older positional one.
  // Both are read here for the same reason Map reads both: a map authored before
  // the rename must still preview.
  const nlohmann::json palette = json.contains("tilePalette")
                                     ? json["tilePalette"]
                                     : json.value("tileInfo", nlohmann::json::array());
  if (palette.is_array()) {
    out.palette.reserve(palette.size());
    for (const auto &entry : palette) {
      out.palette.push_back(ResolveEntry(entry, tiles));
    }
  }

  const auto positions = json.value("positions", nlohmann::json::array());
  out.cells.assign(static_cast<size_t>(out.width) * out.height, -1);
  if (positions.is_array()) {
    // Column-major, matching Map's cursor: y advances, then x.
    size_t index = 0;
    int x = 0;
    int y = 0;
    for (const auto &entry : positions) {
      if (index >= out.cells.size()) break;
      if (entry.is_number_integer() && entry.get<int>() >= 0 &&
          static_cast<size_t>(entry.get<int>()) < out.palette.size()) {
        out.cells[index] = entry.get<int>();
      }
      ++index;
      if (++y == out.height) {
        y = 0;
        ++x;
      }
    }
  }

  out.objectCount = json.contains("objects") && json["objects"].is_array()
                        ? static_cast<int>(json["objects"].size())
                        : 0;
  out.npcCount = json.contains("npcs") && json["npcs"].is_array()
                     ? static_cast<int>(json["npcs"].size())
                     : 0;
  out.placeCount = json.contains("placeSeeds") && json["placeSeeds"].is_array()
                       ? static_cast<int>(json["placeSeeds"].size())
                       : 0;
  if (json.contains("playerStart") && json["playerStart"].is_array() &&
      json["playerStart"].size() >= 2) {
    out.hasPlayerStart = true;
    out.playerStart = {json["playerStart"][0].get<int>(),
                       json["playerStart"][1].get<int>()};
  }

  out.valid = true;
  return true;
}

// A comma-separated count, for the info panel.
inline std::string Count(int value, const char *singular, const char *plural) {
  return std::to_string(value) + " " + (value == 1 ? singular : plural);
}

// Draws the map fitted into `maxWidth` x `maxHeight`, in the current window. One
// cell is as large as fits, so a 20x20 map and a 64x64 map both fill the pane
// without a zoom control. Hovering a cell names it.
inline void Draw(const Info &info, ResourceManager &resources, float maxWidth,
                 float maxHeight) {
  if (!info.valid || info.width <= 0 || info.height <= 0) {
    ImGui::TextDisabled("%s", info.error.empty() ? "No preview available."
                                                 : info.error.c_str());
    return;
  }
  if (maxWidth <= 0.0f || maxHeight <= 0.0f) return;

  const float cell = std::max(
      1.0f, std::floor(std::min(maxWidth / info.width, maxHeight / info.height)));
  const ImVec2 canvasSize(info.width * cell, info.height * cell);

  // One child for the canvas so the draw list is clipped to it and the hover test
  // stays inside the map. Everything is drawn before the Dummy: the Dummy only
  // reserves the layout box, and screen-space coordinates do not depend on it.
  ImGui::BeginChild("##map-canvas", ImVec2(0.0f, canvasSize.y + 4.0f),
                    ImGuiChildFlags_Borders);
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const ImVec2 bottomRight(origin.x + canvasSize.x, origin.y + canvasSize.y);
  ImDrawList *draw = ImGui::GetWindowDrawList();
  draw->AddRectFilled(origin, bottomRight, IM_COL32(18, 18, 20, 255));

  // Resolve each palette entry's sprite once rather than once per cell: a 64x64 map
  // is 4096 cells and most of them share a handful of textures.
  std::vector<const Texture2D *> textures(info.palette.size(), nullptr);
  for (size_t i = 0; i < info.palette.size(); ++i) {
    const TileDef &definition = info.palette[i];
    textures[i] = ResolveSpriteTexture(definition.sprite,
                                       TileSpriteId(definition.name), resources,
                                       nullptr);
  }

  const bool drawGlyphs = cell >= 10.0f;
  for (int x = 0; x < info.width; ++x) {
    for (int y = 0; y < info.height; ++y) {
      const size_t index = static_cast<size_t>(x) * info.height + y;
      if (index >= info.cells.size()) continue;
      const int entry = info.cells[index];
      const ImVec2 topLeft(origin.x + x * cell, origin.y + y * cell);
      const ImVec2 cellBottomRight(topLeft.x + cell, topLeft.y + cell);

      if (entry < 0 || static_cast<size_t>(entry) >= info.palette.size()) {
        draw->AddRectFilled(topLeft, cellBottomRight, IM_COL32(40, 40, 44, 255));
        continue;
      }
      const TileDef &definition = info.palette[static_cast<size_t>(entry)];

      const Color plate =
          definition.hasBackgroundColor && definition.backgroundColor.a > 0
              ? definition.backgroundColor
              : Color{40, 40, 44, 255};
      draw->AddRectFilled(topLeft, cellBottomRight,
                          ImGui::ColorConvertFloat4ToU32(
                              ImVec4(plate.r / 255.0f, plate.g / 255.0f,
                                     plate.b / 255.0f, plate.a / 255.0f)));

      bool drewSprite = false;
      if (const Texture2D *texture = textures[static_cast<size_t>(entry)]) {
        const SpriteRect fit =
            SpriteFitRect(*texture, topLeft.x, topLeft.y, cell, cell);
        if (fit.width > 0.0f && fit.height > 0.0f) {
          draw->AddImage(
              static_cast<ImTextureID>(static_cast<std::intptr_t>(texture->id)),
              ImVec2(fit.x, fit.y), ImVec2(fit.x + fit.width, fit.y + fit.height));
          drewSprite = true;
        }
      }

      if (!drewSprite && drawGlyphs && definition.hasCharacter) {
        const char glyph[2] = {definition.character, '\0'};
        const ImVec2 size = ImGui::CalcTextSize(glyph);
        const ImVec2 textPos(
            std::round(topLeft.x + (cell - size.x) * 0.5f),
            std::round(topLeft.y + (cell - size.y) * 0.5f));
        const Color ink = definition.hasCharacterColor
                              ? definition.characterColor
                              : Color{255, 255, 255, 255};
        draw->AddText(textPos,
                      ImGui::ColorConvertFloat4ToU32(
                          ImVec4(ink.r / 255.0f, ink.g / 255.0f, ink.b / 255.0f,
                                 ink.a / 255.0f)),
                      glyph);
      }
    }
  }
  draw->AddRect(origin, bottomRight, IM_COL32(90, 90, 96, 255));

  // Hover: name the cell under the cursor. A map is a picture of its tiles, and
  // "which tile is that" is otherwise unanswerable at four pixels per cell.
  const ImVec2 mouse = ImGui::GetIO().MousePos;
  if (ImGui::IsWindowHovered() && mouse.x >= origin.x && mouse.y >= origin.y &&
      mouse.x < bottomRight.x && mouse.y < bottomRight.y) {
    const int x = static_cast<int>((mouse.x - origin.x) / cell);
    const int y = static_cast<int>((mouse.y - origin.y) / cell);
    const size_t index = static_cast<size_t>(x) * info.height + y;
    std::string label = "(erased)";
    if (index < info.cells.size()) {
      const int entry = info.cells[index];
      if (entry >= 0 && static_cast<size_t>(entry) < info.palette.size()) {
        label = info.palette[static_cast<size_t>(entry)].name;
      }
    }
    ImGui::SetTooltip("%s\n(%d, %d)", label.c_str(), x, y);
  }

  ImGui::Dummy(canvasSize);
  ImGui::EndChild();
}

}  // namespace MapPreview
