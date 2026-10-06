#pragma once

#include "raylib.h"

#include <unordered_map>
#include <vector>

// The AsciiArt component's payload, kept in its own leaf header on purpose.
//
// It lives here rather than in Components.h because things that are not ECS
// components need it: RaceDef carries a race's look (data/races/<id>.json), and
// equipment/race headers are included *by* Components.h, so they cannot include
// it back. Components.h includes this header, so every existing user of AsciiArt
// keeps working unchanged.
//
// Multi-line ASCII art for an entity, drawn inside that entity's DrawAscii box.
//
// Cells are stored as CODEPOINTS, not bytes. The art is authored in JSON, which
// is UTF-8, and the interesting characters are multi-byte: U+2584 is three bytes.
// Storing a line as std::string and indexing it would silently turn one block
// character into three cells of mojibake, so the parser decodes once (see
// ParseAsciiArt) and everything downstream works in codepoints.
//
// One art block is a small grid of codepoints. The grid is fitted into the box by
// integer division; on a 32px tile a 4x4 grid is exactly 8px cells, which is the
// smallest the bitmap font still reads at. That is also the ceiling on how fine
// an art block can be: it is drawn inside one entity box, so a grid much bigger
// than 4x4 gets cells too small to read rather than more detail.
//
// `palette` gives a colour per distinct codepoint. A codepoint with no entry
// falls back to DrawAscii::characterColor -- which is how the player's tunic and
// an NPC's tunic can be different colours while sharing one art block -- and a
// space is never drawn, so the tile behind shows through.
//
// Authored by an object template's or a race's "art" and "palette" keys; the
// parser is ParseAsciiArt in AsciiArt.hpp and the renderers are in
// AsciiArtDraw.hpp. An entity with no AsciiArt keeps the single-glyph DrawAscii
// as its whole visual, which is exactly the behaviour that existed before this
// component.
struct AsciiArt {
  std::vector<std::vector<int>> lines;
  std::unordered_map<int, Color> palette;
};

// Box-drawing codepoints: single lines, then double lines. These join across
// cells, so they are what lets art draw a frame or a fence without gaps.
inline const std::vector<int> &ArtBoxDrawingCodepoints() {
  static const std::vector<int> codepoints = {
      0x2500, 0x2502, 0x250C, 0x2510, 0x2514, 0x2518, 0x251C, 0x2524,
      0x252C, 0x2534, 0x253C, 0x2550, 0x2551, 0x2554, 0x2557, 0x255A,
      0x255D, 0x2560, 0x2563, 0x2566, 0x2569, 0x256C};
  return codepoints;
}

// The printable Latin-1 Supplement, U+00A1 to U+00FF: the accented letters and
// punctuation DOS bitmap fonts carry above ASCII.
//
// These are drawn as glyphs, and the game's font (the "Plus" build of the
// oldschool PC pack, see DEFAULT_FONT_PATH) covers the whole range -- the face of
// the humanoid figure is `õ`, U+00F5 -- so the art vocabulary follows the font
// rather than stopping at ASCII. U+00A0 is left out because it is a no-break
// space: it is invisible, and a literal ' ' already means "draw nothing here".
inline const std::vector<int> &ArtLatin1Codepoints() {
  static const std::vector<int> codepoints = [] {
    std::vector<int> list;
    for (int c = 0xA1; c <= 0xFF; ++c) {
      list.push_back(c);
    }
    return list;
  }();
  return codepoints;
}

// Block elements: full, halves, eighths, and the three shades. These are the
// sub-cell resolution -- `\u2580`/`\u2584` make one cell two pixels tall,
// `\u2591`-`\u2593` three tones -- and they are filled as rectangles rather than
// drawn as glyphs, so they need no font support at all. See AsciiBlockRect.
inline const std::vector<int> &ArtBlockCodepoints() {
  static const std::vector<int> codepoints = {
      0x2580, 0x2584, 0x2588, 0x258C, 0x2590, 0x2591, 0x2592, 0x2593,
      0x2594, 0x2581, 0x2582, 0x2583, 0x2585, 0x2586, 0x2587, 0x2589,
      0x258A, 0x258B, 0x258D, 0x258E, 0x258F};
  return codepoints;
}

// Every codepoint an art block is allowed to use: printable ASCII, box drawing,
// the Latin-1 letters, and the block/shade set.
inline const std::vector<int> &ArtCodepoints() {
  static const std::vector<int> codepoints = [] {
    std::vector<int> list;
    for (int c = 0x20; c <= 0x7E; ++c) {
      list.push_back(c);
    }
    for (const int c : ArtBoxDrawingCodepoints()) {
      list.push_back(c);
    }
    for (const int c : ArtLatin1Codepoints()) {
      list.push_back(c);
    }
    for (const int c : ArtBlockCodepoints()) {
      list.push_back(c);
    }
    return list;
  }();
  return codepoints;
}

// The codepoints actually rasterized from the font: ASCII, box drawing and the
// Latin-1 letters. Blocks and shades are deliberately absent because they are
// filled as rectangles -- which means the characters that carry the detail cannot
// fail to load, and the font only has to cover the line-drawing and letter set.
inline const std::vector<int> &ArtGlyphCodepoints() {
  static const std::vector<int> codepoints = [] {
    std::vector<int> list;
    for (int c = 0x20; c <= 0x7E; ++c) {
      list.push_back(c);
    }
    for (const int c : ArtBoxDrawingCodepoints()) {
      list.push_back(c);
    }
    for (const int c : ArtLatin1Codepoints()) {
      list.push_back(c);
    }
    return list;
  }();
  return codepoints;
}

// Whether art may use this codepoint at all. Anything outside the list would be a
// character nothing is prepared to draw.
inline bool IsArtCodepointAllowed(int codepoint) {
  if (codepoint == ' ') {
    return true;
  }
  for (const int allowed : ArtCodepoints()) {
    if (allowed == codepoint) {
      return true;
    }
  }
  return false;
}
