#pragma once

#include "AsciiArtTypes.hpp"
#include "raylib.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

// Art-block parsing and layout shared by every consumer of AsciiArt.
//
// This header deliberately contains no drawing code: the data layer (ObjectFactory
// and RaceRegistry) parses templates and race files here, and the two renderers
// live in AsciiArtDraw.hpp. Both the raylib world renderer and the ImGui renderers
// use the layout functions here, so a chest cannot be centred differently on the
// map than it is in the storage window.

// Decodes one UTF-8 codepoint starting at `index` and advances `index` past it.
// Returns -1 for a malformed sequence, advancing one byte so a bad byte can never
// spin the caller.
//
// Hand-rolled rather than pulled from raylib because art is parsed in the data
// layer, before any font exists, and raylib's text helpers all want a Font.
inline int DecodeUtf8Codepoint(const std::string &text, size_t &index) {
  if (index >= text.size()) {
    return -1;
  }

  const unsigned char first = static_cast<unsigned char>(text[index]);
  if (first < 0x80) {
    index += 1;
    return first;
  }

  int extra = 0;
  int codepoint = 0;
  if ((first & 0xE0) == 0xC0) {
    codepoint = first & 0x1F;
    extra = 1;
  } else if ((first & 0xF0) == 0xE0) {
    codepoint = first & 0x0F;
    extra = 2;
  } else if ((first & 0xF8) == 0xF0) {
    codepoint = first & 0x07;
    extra = 3;
  } else {
    index += 1;
    return -1;
  }

  if (index + static_cast<size_t>(extra) >= text.size()) {
    index += 1;
    return -1;
  }
  for (int i = 1; i <= extra; ++i) {
    const unsigned char next = static_cast<unsigned char>(text[index + i]);
    if ((next & 0xC0) != 0x80) {
      index += 1;
      return -1;
    }
    codepoint = (codepoint << 6) | (next & 0x3F);
  }

  index += static_cast<size_t>(extra) + 1;
  return codepoint;
}

// Encodes `codepoint` as UTF-8 into `out`, which must have room for 5 bytes, and
// returns the byte count. Needed because ImGui's text API takes UTF-8 while art
// is stored as codepoints.
inline int EncodeUtf8Codepoint(int codepoint, char out[5]) {
  if (codepoint < 0x80) {
    out[0] = static_cast<char>(codepoint);
    out[1] = '\0';
    return 1;
  }
  if (codepoint < 0x800) {
    out[0] = static_cast<char>(0xC0 | (codepoint >> 6));
    out[1] = static_cast<char>(0x80 | (codepoint & 0x3F));
    out[2] = '\0';
    return 2;
  }
  if (codepoint < 0x10000) {
    out[0] = static_cast<char>(0xE0 | (codepoint >> 12));
    out[1] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
    out[2] = static_cast<char>(0x80 | (codepoint & 0x3F));
    out[3] = '\0';
    return 3;
  }
  out[0] = static_cast<char>(0xF0 | (codepoint >> 18));
  out[1] = static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
  out[2] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
  out[3] = static_cast<char>(0x80 | (codepoint & 0x3F));
  out[4] = '\0';
  return 4;
}

// Parses the "characterColor"-style [r, g, b] / [r, g, b, a] array every object
// template already uses. Falls back unchanged when the value is not such an
// array, which is why callers pass their own default rather than getting white.
inline Color ParseAsciiColor(const nlohmann::json &json, Color fallback) {
  if (json.is_array() && json.size() >= 3) {
    return {json[0].get<unsigned char>(), json[1].get<unsigned char>(),
            json[2].get<unsigned char>(),
            json.size() > 3 ? json[3].get<unsigned char>()
                            : static_cast<unsigned char>(255)};
  }
  return fallback;
}

// Reads the "art" and "palette" keys of an object template or race file.
//
// Returns true when an art block was parsed. Returns false with `warning` filled
// in when the keys are present but unusable, and false with `warning` empty when
// the file simply has no "art" -- in both false cases the caller keeps the
// single-glyph DrawAscii, so a bad art block degrades to the old look rather than
// erasing the object.
//
// Nothing here throws and nothing here aborts a load: a malformed art block is
// content data, and every other content error in ObjectFactory is a warning.
// A palette entry that cannot be used is dropped on its own; a codepoint used in
// the art but missing from the palette is not an error, it just takes the
// fallback colour.
inline bool ParseAsciiArt(const nlohmann::json &tmpl, AsciiArt &out,
                          std::string &warning) {
  warning.clear();

  if (!tmpl.contains("art")) {
    return false;
  }

  const nlohmann::json &artJson = tmpl["art"];
  if (!artJson.is_array()) {
    warning = "'art' is not an array; using the single-glyph 'character' instead";
    return false;
  }

  std::vector<std::vector<int>> lines;
  for (const nlohmann::json &lineJson : artJson) {
    if (!lineJson.is_string()) {
      warning =
          "'art' contains a non-string line; using the single-glyph 'character' "
          "instead";
      return false;
    }

    // Decoded to codepoints here, once, so no later stage can mistake a
    // three-byte block character for three cells.
    const std::string text = lineJson.get<std::string>();
    std::vector<int> line;
    size_t index = 0;
    while (index < text.size()) {
      const int codepoint = DecodeUtf8Codepoint(text, index);
      if (codepoint < 0) {
        warning = "'art' has malformed UTF-8 at line " +
                  std::to_string(lines.size()) + ", byte " +
                  std::to_string(index) +
                  "; using the single-glyph 'character' instead";
        return false;
      }
      if (!IsArtCodepointAllowed(codepoint)) {
        char buffer[16];
        std::snprintf(buffer, sizeof(buffer), "U+%04X", codepoint);
        warning = std::string("'art' uses ") + buffer +
                  " at line " + std::to_string(lines.size()) + ", cell " +
                  std::to_string(line.size()) +
                  ", which the art fonts are never asked to load; using the "
                  "single-glyph 'character' instead";
        return false;
      }
      line.push_back(codepoint);
    }
    lines.push_back(std::move(line));
  }

  // An all-blank block is almost certainly a mistake, and rendering it would
  // silently make the object invisible.
  bool hasInk = false;
  for (const std::vector<int> &line : lines) {
    for (const int codepoint : line) {
      if (codepoint != ' ') {
        hasInk = true;
        break;
      }
    }
    if (hasInk) {
      break;
    }
  }
  if (!hasInk) {
    warning =
        "'art' is empty or entirely blank; using the single-glyph 'character' "
        "instead";
    return false;
  }

  AsciiArt art;
  art.lines = std::move(lines);

  if (tmpl.contains("palette")) {
    const nlohmann::json &paletteJson = tmpl["palette"];
    if (!paletteJson.is_object()) {
      warning = "'palette' is not an object; its entries were ignored";
    } else {
      for (auto entry = paletteJson.begin(); entry != paletteJson.end();
           ++entry) {
        // A key is a codepoint too, so it may be multi-byte -- "▄" rather than a
        // single char.
        const std::string key = entry.key();
        size_t index = 0;
        const int codepoint = DecodeUtf8Codepoint(key, index);
        if (codepoint < 0 || index != key.size()) {
          warning = "'palette' has a key that is not a single character ('" + key +
                    "'); that entry was ignored";
          continue;
        }
        art.palette[codepoint] =
            ParseAsciiColor(entry.value(), Color{255, 255, 255, 255});
      }
    }
  }

  out = std::move(art);
  return true;
}

// The drawn extent of an art block inside an entity's box, in pixels. The grid is
// fitted by integer division and centred, so a 3-wide art in a 32px box gets
// 10px cells and a 1px margin on each side instead of cells landing on
// fractional pixels -- the font texture is point-filtered, so half-pixel cells
// would show as uneven glyphs.
struct AsciiGrid {
  int cols = 1;
  int rows = 1;
  int cellWidth = 1;
  int cellHeight = 1;
  float originX = 0.0f;
  float originY = 0.0f;
};

inline int AsciiArtColumns(const AsciiArt &art) {
  size_t cols = 1;
  for (const std::vector<int> &line : art.lines) {
    cols = std::max(cols, line.size());
  }
  return static_cast<int>(cols);
}

inline int AsciiArtRows(const AsciiArt &art) {
  return std::max(1, static_cast<int>(art.lines.size()));
}

inline AsciiGrid ComputeAsciiGrid(const AsciiArt &art, float x, float y,
                                  float width, float height) {
  AsciiGrid grid;
  grid.cols = AsciiArtColumns(art);
  grid.rows = AsciiArtRows(art);
  grid.cellWidth = std::max(1, static_cast<int>(width) / grid.cols);
  grid.cellHeight = std::max(1, static_cast<int>(height) / grid.rows);
  grid.originX = std::floor(
      x + (width - static_cast<float>(grid.cellWidth * grid.cols)) / 2.0f);
  grid.originY = std::floor(
      y + (height - static_cast<float>(grid.cellHeight * grid.rows)) / 2.0f);
  return grid;
}

// Ragged art is allowed on purpose: a short row is simply blank past its end,
// which is easier to author than padding every line by hand.
inline int AsciiArtCell(const AsciiArt &art, int col, int row) {
  if (row < 0 || row >= static_cast<int>(art.lines.size())) {
    return ' ';
  }
  const std::vector<int> &line = art.lines[row];
  if (col < 0 || col >= static_cast<int>(line.size())) {
    return ' ';
  }
  return line[col];
}

inline Color AsciiArtColor(const AsciiArt &art, Color fallback, int codepoint) {
  const auto entry = art.palette.find(codepoint);
  return entry == art.palette.end() ? fallback : entry->second;
}

// The area a block codepoint fills inside one cell, as fractions of the cell.
//
// Named AsciiBlockArea rather than AsciiBlockRect because it shares its job with
// the function below, and a struct and a function cannot have the same name: the
// function hides the type, so `AsciiBlockRect block;` fails to parse.
struct AsciiBlockArea {
  float x = 0.0f;
  float y = 0.0f;
  float width = 0.0f;
  float height = 0.0f;
};

// Fills `out` and returns true when the codepoint is a geometric block.
//
// Why blocks are filled rather than drawn: a monospace glyph's advance is
// narrower than an art cell -- roughly half of it for a CP437-derived font at an
// 8px cell -- so a run of U+2588 drawn as glyphs comes out as stripes with seams
// instead of a solid mass, and U+2580 is not a true half-cell. Filling the cell is
// exact at any cell size, and it also means the characters carrying the detail do
// not depend on the font having them.
inline bool AsciiBlockRect(int codepoint, AsciiBlockArea &out) {
  switch (codepoint) {
    case 0x2588: // full block
      out = {0.0f, 0.0f, 1.0f, 1.0f};
      return true;
    case 0x2580: // upper half
      out = {0.0f, 0.0f, 1.0f, 0.5f};
      return true;
    case 0x2584: // lower half
      out = {0.0f, 0.5f, 1.0f, 0.5f};
      return true;
    case 0x2590: // right half
      out = {0.5f, 0.0f, 0.5f, 1.0f};
      return true;
    case 0x2594: // upper one eighth
      out = {0.0f, 0.0f, 1.0f, 0.125f};
      return true;
    default:
      break;
  }

  // Lower one-eighth blocks, U+2581 (one eighth) to U+2587 (seven eighths).
  if (codepoint >= 0x2581 && codepoint <= 0x2587) {
    const float height = static_cast<float>(codepoint - 0x2580) / 8.0f;
    out = {0.0f, 1.0f - height, 1.0f, height};
    return true;
  }

  // Left one-eighth blocks, U+2589 (seven eighths) to U+258F (one eighth). This
  // range also covers U+258C, the left half.
  if (codepoint >= 0x2589 && codepoint <= 0x258F) {
    out = {0.0f, 0.0f, static_cast<float>(0x2590 - codepoint) / 8.0f, 1.0f};
    return true;
  }

  return false;
}

// The opacity a shade codepoint is drawn with, or 0 when it is not a shade.
// Approximated as a translucent wash over the whole cell rather than the font's
// dither pattern: that pattern is only a few pixels wide at these cell sizes and
// would show the same cell-seam problem, while a wash over the tile reads the
// same at a distance and costs one rectangle.
inline float AsciiShadeAlpha(int codepoint) {
  switch (codepoint) {
    case 0x2591: // light shade
      return 0.25f;
    case 0x2592: // medium shade
      return 0.5f;
    case 0x2593: // dark shade
      return 0.75f;
    default:
      return 0.0f;
  }
}
