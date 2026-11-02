#include "MapDesigner.hpp"

#include "DesignerSession.h"
#include "ObjectFactory.h"
#include "TileRegistry.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>

namespace {

std::string Slugify(const std::string &text) {
  std::string out;
  bool lastUnderscore = false;
  for (const char c : text) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (std::isalnum(u) != 0) {
      out.push_back(static_cast<char>(std::tolower(u)));
      lastUnderscore = false;
    } else if (!out.empty() && !lastUnderscore) {
      out.push_back('_');
      lastUnderscore = true;
    }
  }
  while (!out.empty() && out.back() == '_') out.pop_back();
  return out;
}

// A design is ASCII by construction: one character is one cell, and a multi-byte
// glyph would make the width check lie about the number of cells.
bool SingleCharacter(const std::string &key) {
  return key.size() == 1 &&
         static_cast<unsigned char>(key[0]) < 128;
}

}  // namespace

namespace MapDesigner {

ApplyResult ApplyReply(const std::string &text, const TileRegistry &tiles,
                       const ObjectFactory &objects) {
  ApplyResult result;
  auto fail = [&result](const std::string &reason) {
    result.ok = false;
    result.error = reason;
    return result;
  };

  // The same two-step the other designers use: reasoning out, fenced block or the
  // whole text, then parse. The window hands over the session's raw answer, so this
  // is the only place that knows a reply is text rather than a document.
  const std::string cleaned = DesignerReply::StripReasoning(text);
  std::string block = DesignerReply::ExtractJsonBlock(cleaned);
  const bool fromBlock = !block.empty();
  if (!fromBlock) {
    // A model that answered with nothing but the object is accepted: the contract
    // asks for a fence, but the object is what matters.
    block = cleaned;
  }
  if (block.find_first_not_of(" \t\r\n") == std::string::npos) {
    return fail("The reply was empty. Send the map as one ```json block, as "
                "maps/MAPDESIGNER.md requires.");
  }

  nlohmann::json reply;
  try {
    reply = nlohmann::json::parse(block);
  } catch (const std::exception &parseError) {
    // A reply that stopped mid-object is a budget problem, not a syntax one, and
    // saying so is what stops the model from retrying at the same size: the two
    // things it can change are the map's size and its own reasoning.
    const size_t fence = cleaned.find("```json");
    const bool closed =
        fence != std::string::npos &&
        cleaned.find("```", fence + 7) != std::string::npos;
    if (fromBlock && !closed) {
      return fail("The reply looks truncated -- the JSON block was never closed. "
                  "Send a smaller map (prefer at most 32x24) with no more than "
                  "about 40 objects.");
    }
    return fail(std::string("The map is not valid JSON: ") + parseError.what());
  }
  if (!reply.is_object()) {
    return fail("The json block must be a JSON object describing one map.");
  }

  Draft draft;
  draft.name = reply.value("name", std::string());
  if (draft.name.empty()) {
    return fail("The reply has no \"name\"; a map needs a name to be saved by.");
  }
  draft.description = reply.value("description", std::string());
  result.notes = reply.value("designerNotes", std::string());
  if (result.notes.empty()) {
    return fail("The reply has no \"designerNotes\"; say what you built and why.");
  }

  if (!reply.contains("width") || !reply.contains("height") ||
      !reply["width"].is_number_integer() ||
      !reply["height"].is_number_integer()) {
    return fail("The reply needs integer \"width\" and \"height\".");
  }
  draft.width = reply["width"].get<int>();
  draft.height = reply["height"].get<int>();
  if (draft.width < kMinSide || draft.width > kMaxSide || draft.height < kMinSide ||
      draft.height > kMaxSide) {
    return fail("The map is " + std::to_string(draft.width) + "x" +
                std::to_string(draft.height) + "; width and height must be between " +
                std::to_string(kMinSide) + " and " + std::to_string(kMaxSide) + ".");
  }
  draft.tileWidth = reply.value("tileWidth", 32);
  draft.tileHeight = reply.value("tileHeight", 32);

  // --- legend ---------------------------------------------------------------
  if (!reply.contains("legend") || !reply["legend"].is_object() ||
      reply["legend"].empty()) {
    return fail("The reply needs a non-empty \"legend\" mapping one character to a tile name.");
  }
  // Resolve the legend up front: a row that uses a character is only meaningful if
  // that character names a tile this world has, so the error can name both.
  std::vector<std::string> paletteTiles;  // first appearance, for stable positions
  std::vector<int> legendIndex(256, -1);
  for (auto it = reply["legend"].begin(); it != reply["legend"].end(); ++it) {
    if (!SingleCharacter(it.key())) {
      return fail("Legend key \"" + it.key() +
                  "\" is not a single ASCII character.");
    }
    if (!it.value().is_string()) {
      return fail("Legend entry \"" + it.key() + "\" is not a tile name.");
    }
    const std::string tileName = it.value().get<std::string>();
    if (tiles.Get(tileName) == nullptr) {
      return fail("Legend entry \"" + it.key() + "\" names tile '" + tileName +
                  "', which is not in the tile catalog.");
    }
    const unsigned char key = static_cast<unsigned char>(it.key()[0]);
    legendIndex[key] = static_cast<int>(paletteTiles.size());
    paletteTiles.push_back(tileName);
  }

  // --- rows -----------------------------------------------------------------
  if (!reply.contains("rows") || !reply["rows"].is_array()) {
    return fail("The reply needs \"rows\": one string per map row.");
  }
  if (static_cast<int>(reply["rows"].size()) != draft.height) {
    return fail("The reply has " + std::to_string(reply["rows"].size()) +
                " rows but height is " + std::to_string(draft.height) + ".");
  }
  std::vector<std::string> rows;
  rows.reserve(reply["rows"].size());
  for (size_t i = 0; i < reply["rows"].size(); ++i) {
    if (!reply["rows"][i].is_string()) {
      return fail("Row " + std::to_string(i) + " is not a string.");
    }
    const std::string row = reply["rows"][i].get<std::string>();
    if (static_cast<int>(row.size()) != draft.width) {
      return fail("Row " + std::to_string(i) + " has " + std::to_string(row.size()) +
                  " characters but width is " + std::to_string(draft.width) + ".");
    }
    for (size_t column = 0; column < row.size(); ++column) {
      const unsigned char key = static_cast<unsigned char>(row[column]);
      if (legendIndex[key] < 0) {
        return fail(std::string("Row ") + std::to_string(i) + " uses '" +
                    row[column] + "', which is not in the legend.");
      }
    }
    rows.push_back(row);
  }

  // --- objects --------------------------------------------------------------
  nlohmann::json objectArray = nlohmann::json::array();
  if (reply.contains("objects")) {
    if (!reply["objects"].is_array()) {
      return fail("\"objects\" is not an array.");
    }
    for (const auto &entry : reply["objects"]) {
      if (!entry.is_object()) {
        return fail("An \"objects\" entry is not an object.");
      }
      const std::string type = entry.value("type", std::string());
      if (type.empty() || objects.GetTemplates().count(type) == 0) {
        return fail("Object type '" + type + "' is not in the object catalog.");
      }
      const int x = entry.value("x", -1);
      const int y = entry.value("y", -1);
      if (x < 0 || y < 0 || x >= draft.width || y >= draft.height) {
        return fail("Object '" + type + "' is at (" + std::to_string(x) + ", " +
                    std::to_string(y) + "), outside the map.");
      }
      objectArray.push_back({{"type", type}, {"x", x}, {"y", y}});
    }
  }
  draft.objectCount = static_cast<int>(objectArray.size());

  bool hasPlayerStart = false;
  int startX = 0;
  int startY = 0;
  if (reply.contains("playerStart")) {
    if (!reply["playerStart"].is_array() || reply["playerStart"].size() < 2) {
      return fail("\"playerStart\" must be [x, y].");
    }
    startX = reply["playerStart"][0].get<int>();
    startY = reply["playerStart"][1].get<int>();
    if (startX < 0 || startY < 0 || startX >= draft.width ||
        startY >= draft.height) {
      return fail("playerStart is outside the map.");
    }
    hasPlayerStart = true;
  }

  // --- expand ---------------------------------------------------------------
  //
  // position index = x * height + y, the order Map's cursor walks: y advances, then
  // x. Getting this wrong transposes the map, so it is written once, here.
  nlohmann::json positions = nlohmann::json::array();
  for (int x = 0; x < draft.width; ++x) {
    for (int y = 0; y < draft.height; ++y) {
      const unsigned char key = static_cast<unsigned char>(rows[y][x]);
      positions.push_back(legendIndex[key]);
    }
  }

  nlohmann::json paletteArray = nlohmann::json::array();
  for (const std::string &tileName : paletteTiles) {
    paletteArray.push_back(tileName);
  }

  draft.document = nlohmann::json{
      {"name", draft.name},
      {"description", draft.description},
      {"mapWidth", draft.width},
      {"mapHeight", draft.height},
      {"tileWidth", draft.tileWidth},
      {"tileHeight", draft.tileHeight},
      {"tilePalette", std::move(paletteArray)},
      {"positions", std::move(positions)},
      {"objects", std::move(objectArray)},
  };
  if (hasPlayerStart) {
    draft.document["playerStart"] = {startX, startY};
  }

  // Not fatal, but worth saying: objects on blocking terrain will be relocated at
  // spawn, which would quietly move what the design placed.
  for (const auto &entry : draft.document["objects"]) {
    const int x = entry["x"].get<int>();
    const int y = entry["y"].get<int>();
    const unsigned char key = static_cast<unsigned char>(rows[y][x]);
    const std::string &tileName =
        paletteTiles[static_cast<size_t>(legendIndex[key])];
    const TileDef *definition = tiles.Get(tileName);
    if (definition != nullptr && definition->blocksTile) {
      draft.warnings.push_back("Object '" + entry["type"].get<std::string>() +
                              "' is on blocking tile '" + tileName + "'.");
    }
  }

  result.ok = true;
  result.draft = std::move(draft);
  return result;
}

std::string BuildSystemPrompt(const std::string &promptFile,
                              const TileRegistry &tiles,
                              const ObjectFactory &objects) {
  std::string prompt;
  if (!promptFile.empty()) {
    std::ifstream file(promptFile);
    if (file.is_open()) {
      prompt.assign(std::istreambuf_iterator<char>(file),
                    std::istreambuf_iterator<char>());
    }
  }
  if (prompt.empty()) {
    // The file is the contract; without it there is nothing worth sending. Saying
    // which file is missing is more useful to a person reading the transcript than
    // an empty system prompt, which reads as a model failure.
    return "ERROR: the map designer's instruction file '" + promptFile +
           "' could not be read.";
  }

  std::vector<std::string> tileNames;
  for (const TileDef *definition : tiles.GetAll()) {
    tileNames.push_back(definition->name);
  }
  std::sort(tileNames.begin(), tileNames.end());
  std::vector<std::string> objectNames;
  for (const auto &pair : objects.GetTemplates()) {
    objectNames.push_back(pair.first);
  }
  std::sort(objectNames.begin(), objectNames.end());

  const auto joined = [](const std::vector<std::string> &items) {
    std::string out;
    for (const std::string &item : items) {
      if (!out.empty()) out += ", ";
      out += item;
    }
    return out;
  };

  const auto replaceAll = [](std::string &text, const std::string &needle,
                             const std::string &value) {
    size_t at = 0;
    while ((at = text.find(needle, at)) != std::string::npos) {
      text.replace(at, needle.size(), value);
      at += value.size();
    }
  };
  replaceAll(prompt, "%TILES%", joined(tileNames));
  replaceAll(prompt, "%OBJECTS%", joined(objectNames));
  return prompt;
}

bool Save(const Draft &draft, const std::string &mapsDirectory,
          std::string *savedPath, std::string *error) {
  if (savedPath != nullptr) savedPath->clear();
  const std::string slug = Slugify(draft.name);
  if (slug.empty()) {
    if (error != nullptr) *error = "The map name has no letters or digits to save under.";
    return false;
  }
  if (mapsDirectory.empty()) {
    if (error != nullptr) *error = "No maps directory is configured.";
    return false;
  }

  std::error_code ec;
  std::filesystem::create_directories(mapsDirectory, ec);
  const std::filesystem::path path =
      std::filesystem::path(mapsDirectory) / (slug + ".json");
  if (std::filesystem::exists(path, ec)) {
    if (error != nullptr) {
      *error = "A map file already exists at " + path.string() +
               "; rename the design rather than overwrite it.";
    }
    return false;
  }

  std::ofstream file(path);
  if (!file.is_open()) {
    if (error != nullptr) *error = "Could not write " + path.string() + ".";
    return false;
  }
  file << draft.document.dump(2) << "\n";
  if (!file.good()) {
    if (error != nullptr) *error = "Failed while writing " + path.string() + ".";
    return false;
  }
  if (savedPath != nullptr) *savedPath = path.string();
  return true;
}

}  // namespace MapDesigner
