#include "TileDesigner.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "DebugLog.h"

namespace TileDesigner {

namespace {

// The language tag that opens the one fenced block a reply must carry. The same
// string DesignerChatWindow matches on; the two windows are answering different
// questions but the contract is deliberately identical, so a model that learned
// one has learned the other.
constexpr const char *kJsonFence = "```json";

constexpr const char *kGlyphPlaceholder = "%GLYPHS%";
constexpr const char *kTilePlaceholder = "%TILES%";

// The tile vocabulary, in the same shape BuildDesignerSystemPrompt builds its
// material palette. One line per tile, marked when it blocks movement, because
// that is the one mechanic a tile carries and the one a designer has to reason
// about.
std::string TileList(const TileRegistry &tiles) {
  if (tiles.Count() == 0) {
    return "  (none: no tile catalog is loaded)\n";
  }
  std::string out;
  for (const TileDef *tile : tiles.GetAll()) {
    out += "  - " + tile->name;
    if (tile->blocksTile) {
      out += "  [blocks movement]";
    }
    out += "\n";
  }
  return out;
}

// Appends `section` when the placeholder is absent, so a prompt file that forgets
// it still carries the vocabulary rather than silently losing it.
void SubstituteOrAppend(std::string &prompt, const std::string &placeholder,
                        const std::string &heading, const std::string &body) {
  const size_t at = prompt.find(placeholder);
  if (at != std::string::npos) {
    prompt.replace(at, placeholder.size(), body);
    return;
  }
  if (prompt.empty()) return;
  prompt += "\n\n" + heading + "\n\n" + body;
}

// The overlay file is read back by TileRegistry, which keys on the `name` field,
// so the stem is only a filename. Still slugified rather than written verbatim:
// a model may name a tile "Oak Bed (large)" and a filename with parentheses,
// spaces and slashes in it is a bug waiting for a second platform.
std::string SanitizeStem(const std::string &name) {
  std::string out;
  out.reserve(name.size());
  for (const char raw : name) {
    const unsigned char c = static_cast<unsigned char>(raw);
    if (std::isalnum(c)) {
      out += static_cast<char>(std::tolower(c));
    } else if (!out.empty() && out.back() != '_') {
      out += '_';
    }
  }
  while (!out.empty() && out.back() == '_') {
    out.pop_back();
  }
  if (out.empty()) {
    out = "tile";
  }
  return out;
}

// Writes `def` as <overlayDirectory>/<stem>.json, creating the directory if
// needed. Returns the path on success and an empty string on failure, with the
// reason in `error`.
std::string WriteDefinition(const TileDef &def, const std::string &overlayDirectory,
                            std::string &error) {
  std::error_code ec;
  std::filesystem::create_directories(overlayDirectory, ec);
  if (ec) {
    error = "could not create " + overlayDirectory + ": " + ec.message();
    return {};
  }

  // The write is to a temporary path and then renamed, so a crash or a full disk
  // cannot leave a half-written definition that the next load reports as
  // malformed. The overlay is reloaded on every launch, so a truncated file is
  // not a one-off annoyance, it is a broken tile forever.
  const std::filesystem::path finalPath =
      std::filesystem::path(overlayDirectory) / (SlugifyFileName(def.name) + ".json");
  const std::filesystem::path tempPath = finalPath.string() + ".tmp";

  {
    std::ofstream out(tempPath);
    if (!out.is_open()) {
      error = "could not open " + tempPath.string() + " for writing";
      return {};
    }
    out << WriteTileDef(def).dump(2) << "\n";
    if (!out.good()) {
      error = "writing " + tempPath.string() + " failed";
      std::error_code removeEc;
      std::filesystem::remove(tempPath, removeEc);
      return {};
    }
  }

  std::filesystem::rename(tempPath, finalPath, ec);
  if (ec) {
    error = "could not move " + tempPath.string() + " to " + finalPath.string() +
            ": " + ec.message();
    std::error_code removeEc;
    std::filesystem::remove(tempPath, removeEc);
    return {};
  }
  return finalPath.string();
}

}  // namespace

std::string ExtractJsonBlock(const std::string &text) {
  // Deliberately tolerant about what surrounds the fence and strict about the
  // fence itself, because that is the one thing the prompt is unambiguous about.
  const size_t fence = text.find(kJsonFence);
  if (fence == std::string::npos) {
    return {};
  }
  const size_t bodyStart = text.find('\n', fence);
  if (bodyStart == std::string::npos) {
    return {};
  }
  const size_t close = text.find("```", bodyStart);
  if (close == std::string::npos) {
    // An unterminated fence: take the rest. A model that forgot to close it has
    // usually still produced the object, and "no JSON block" would be a less
    // useful error than whatever the parser says about the text.
    return text.substr(bodyStart + 1);
  }
  return text.substr(bodyStart + 1, close - bodyStart - 1);
}

std::string ExtractTileJson(const std::string &text, bool *fromBlock) {
  const std::string block = ExtractJsonBlock(text);
  if (!block.empty()) {
    if (fromBlock != nullptr) *fromBlock = true;
    return block;
  }
  if (fromBlock != nullptr) *fromBlock = false;
  return text;
}

std::string StripReasoning(const std::string &text) {
  static constexpr const char *kOpen = "<think>";
  static constexpr const char *kClose = "</think>";
  static constexpr size_t kOpenLength = 7;
  static constexpr size_t kCloseLength = 8;

  std::string out;
  out.reserve(text.size());

  size_t i = 0;
  while (i < text.size()) {
    if (text.compare(i, kOpenLength, kOpen) == 0) {
      const size_t close = text.find(kClose, i + kOpenLength);
      if (close == std::string::npos) {
        break;  // an unterminated block swallows the rest
      }
      i = close + kCloseLength;
      continue;
    }
    out += text[i++];
  }
  return out;
}

std::vector<std::string> GlyphLegend(const TileRegistry &tiles) {
  // Grouped by glyph, because the glyph is what an agent perceives (§2.1) and a
  // collision is a fact about the glyph, not about either tile that uses it.
  //
  // Ordered by first appearance in the sorted name list rather than by glyph, so
  // the output is stable across runs: a legend that reshuffles every launch would
  // make two prompt logs impossible to diff.
  std::vector<std::pair<char, std::vector<std::string>>> rows;
  for (const TileDef *tile : tiles.GetAll()) {
    if (!tile->hasCharacter) continue;
    auto it = std::find_if(rows.begin(), rows.end(), [&](const auto &row) {
      return row.first == tile->character;
    });
    if (it == rows.end()) {
      rows.emplace_back(tile->character, std::vector<std::string>{tile->name});
    } else {
      it->second.push_back(tile->name);
    }
  }

  std::vector<std::string> lines;
  lines.reserve(rows.size());
  for (const auto &row : rows) {
    std::string line = "  '";
    line += row.first;
    line += "'  ";
    for (size_t i = 0; i < row.second.size(); ++i) {
      if (i > 0) line += ", ";
      line += row.second[i];
    }
    if (row.second.size() > 1) {
      line += "   [SHARED: " + std::to_string(row.second.size()) +
              " tiles draw this glyph]";
    }
    lines.push_back(std::move(line));
  }
  return lines;
}

std::string BuildSystemPrompt(const std::string &promptFile,
                              const TileRegistry &tiles) {
  std::string prompt;
  if (!promptFile.empty()) {
    std::ifstream file(promptFile);
    if (file.is_open()) {
      std::stringstream buffer;
      buffer << file.rdbuf();
      prompt = buffer.str();
    } else {
      // Same treatment the blueprint designer gives a missing prompt file: the
      // generated vocabulary still goes out, so the model is under-informed
      // rather than unconstrained.
      std::cerr << "TileDesigner: could not read the prompt file " << promptFile
                << "; using the generated vocabulary only." << std::endl;
    }
  }

  std::string glyphs;
  const std::vector<std::string> legend = GlyphLegend(tiles);
  if (legend.empty()) {
    glyphs = "  (the catalog is empty, so every printable character is free)\n";
  } else {
    for (const std::string &line : legend) {
      glyphs += line + "\n";
    }
  }

  SubstituteOrAppend(prompt, kGlyphPlaceholder,
                     "## Glyphs already in use (generated from this world)",
                     glyphs);
  SubstituteOrAppend(prompt, kTilePlaceholder,
                     "## Tiles that already exist (generated from this world)",
                     TileList(tiles));
  return prompt;
}

bool NameTaken(const TileRegistry &tiles, const std::string &name) {
  return tiles.Get(name) != nullptr;
}

nlohmann::json WriteTileDef(const TileDef &def) {
  nlohmann::json out;
  out["name"] = def.name;
  if (def.hasCharacter) {
    // A one-character JSON string. Stored from the raw char, so a glyph that is
    // not printable ASCII round-trips as the byte it was.
    out["character"] = std::string(1, def.character);
  }
  if (def.hasCharacterColor) {
    // Explicitly int: a Color's channels are unsigned char, and nlohmann treats
    // an unsigned char as a character, so the braced list would serialise
    // [255, 1, 2, 255] as [255, '\u0001', '\u0002', 255] rather than as the
    // numbers the schema and TileRegistry's parser expect.
    out["characterColor"] = {static_cast<int>(def.characterColor.r),
                             static_cast<int>(def.characterColor.g),
                             static_cast<int>(def.characterColor.b),
                             static_cast<int>(def.characterColor.a)};
  }
  if (def.hasBackgroundColor) {
    out["backgroundColor"] = {static_cast<int>(def.backgroundColor.r),
                              static_cast<int>(def.backgroundColor.g),
                              static_cast<int>(def.backgroundColor.b),
                              static_cast<int>(def.backgroundColor.a)};
  }
  out["blocksTile"] = def.blocksTile;
  if (!def.sprite.empty()) {
    out["sprite"] = def.sprite;
  }
  if (def.hasHitboxSize) {
    out["hitboxSize"] = def.hitboxSize;
  }
  return out;
}

std::string SlugifyFileName(const std::string &name) { return SanitizeStem(name); }

ApplyResult ApplyJson(const nlohmann::json &tileJson, TileRegistry &tiles,
                      const std::string &overlayDirectory) {
  ApplyResult result;

  if (tiles.Get(tileJson.value("name", std::string())) != nullptr) {
    result.nameTaken = true;
  }

  // A hand-authored name is refused before the write path is reached, and the
  // refusal is the registry's, not this function's: one validator, so the window,
  // the headless mode and the overlay loader cannot disagree about what is legal.
  TileRegistry::AddResult added = tiles.AddRuntime(tileJson, "(tile designer)");
  result.warnings = added.warnings;
  if (!added.ok) {
    result.error = added.error;
    result.name = tileJson.value("name", std::string());
    return result;
  }

  result.ok = true;
  result.definition = added.definition;
  result.name = added.definition->name;

  if (!overlayDirectory.empty()) {
    std::string writeError;
    result.savedPath =
        WriteDefinition(*added.definition, overlayDirectory, writeError);
    if (result.savedPath.empty()) {
      // The tile is accepted and live; only persisting it failed. Reported rather
      // than silently downgraded to a refusal, because the tile *does* exist now
      // and saying otherwise would be a lie the caller acts on.
      result.saveFailed = true;
      result.warnings.push_back("the tile was accepted but not saved: " +
                                writeError);
    }
  }
  return result;
}

ApplyResult ApplyReply(const std::string &reply, TileRegistry &tiles,
                       const std::string &overlayDirectory) {
  ApplyResult result;

  const std::string cleaned = StripReasoning(reply);
  bool fromBlock = false;
  const std::string block = ExtractTileJson(cleaned, &fromBlock);
  if (block.empty()) {
    result.error =
        "the reply was empty. Send the tile as one ```json block, as "
        "TILEDESIGNER.md requires.";
    return result;
  }
  // A bare object is accepted (see ExtractTileJson), but the contract asks for a
  // fence, and a reply that ignored that is worth saying so about -- once, on a
  // successful attempt, without refusing it. Refusing a valid tile over its
  // packaging would teach the model nothing about tiles.
  const bool unfenced = !fromBlock;

  nlohmann::json parsed;
  try {
    parsed = nlohmann::json::parse(block);
  } catch (const std::exception &e) {
    result.error = std::string("the tile is not valid JSON: ") + e.what();
    return result;
  }
  if (!parsed.is_object()) {
    result.error = "the json block must be a JSON object describing one tile.";
    return result;
  }

  // Required by the contract, and the reason it is required is the whole point of
  // the window: the model's own judgement of what it invented. Stripped before
  // storage because it is not part of the tile schema, exactly as the blueprint
  // designer strips its own notes.
  if (!parsed.contains("designerNotes") || !parsed["designerNotes"].is_string() ||
      parsed["designerNotes"].get<std::string>().empty()) {
    result.error =
        "the tile has no non-empty \"designerNotes\" string. Every tile must "
        "carry your own thoughts on it.";
    return result;
  }
  const std::string notes = parsed["designerNotes"].get<std::string>();
  parsed.erase("designerNotes");

  ApplyResult applied = ApplyJson(parsed, tiles, overlayDirectory);
  applied.notes = notes;
  if (unfenced) {
    applied.warnings.push_back(
        "the tile was not inside a ```json block; it was accepted anyway, but a "
        "reply should fence it");
  }
  return applied;
}

ApplyResult ApplyFile(const std::string &path, TileRegistry &tiles,
                      const std::string &overlayDirectory) {
  ApplyResult result;
  std::ifstream file(path);
  if (!file.is_open()) {
    result.error = "could not read " + path;
    return result;
  }
  std::stringstream buffer;
  buffer << file.rdbuf();
  return ApplyReply(buffer.str(), tiles, overlayDirectory);
}

size_t LoadOverlayDirectory(const std::string &overlayDirectory,
                            TileRegistry &tiles, DebugLog *log) {
  auto warn = [log](const std::string &message) {
    // Always to stderr as well as the log: DebugLog keeps entries in memory and
    // never prints, so in the headless modes -- the ones that exist to make this
    // path verifiable -- routing through it alone would make a broken overlay
    // tile completely silent. Being told twice in a game run is the cheaper
    // mistake.
    std::cerr << message << std::endl;
    if (log) {
      log->LogWarning(message);
    }
  };

  std::error_code ec;
  if (!std::filesystem::exists(overlayDirectory, ec)) {
    // Not an error, and deliberately not a warning either: a run that has never
    // created a tile is the normal case, and warning about it on every launch
    // would train people to ignore the warning that matters.
    return 0;
  }

  size_t added = 0;
  for (const auto &entry :
       std::filesystem::directory_iterator(overlayDirectory, ec)) {
    if (ec) {
      warn("Tile overlay: could not list " + overlayDirectory + ": " +
           ec.message());
      return added;
    }
    if (entry.path().extension() != ".json") continue;
    // The only files written here are .json and, briefly, .json.tmp -- the
    // extension test above already excludes the temporary one, so nothing extra
    // is needed; a crash mid-write cannot surface as a malformed definition.

    std::ifstream file(entry.path());
    if (!file.is_open()) continue;

    try {
      nlohmann::json j;
      file >> j;
      // Runtime additions may replace other runtime additions -- that is what
      // makes the overlay reloadable -- but never a hand-authored catalog tile,
      // which AddRuntime enforces itself.
      TileRegistry::AddResult result = tiles.AddRuntime(
          j, entry.path().string(), /*replaceExisting=*/true);
      if (!result.ok) {
        warn("Tile overlay: " + entry.path().string() + " was refused: " +
             result.error);
        continue;
      }
      ++added;
    } catch (const std::exception &e) {
      warn("Tile overlay: failed to parse " + entry.path().string() + ": " +
           e.what());
    }
  }
  return added;
}

}  // namespace TileDesigner
