#pragma once

#include "AsciiArtTypes.hpp"

#include <vector>

// The neutral humanoid look: what a character is drawn with when its race does
// not say. Mirrors DefaultHumanoidSlots() in EquipmentTypes.h -- the code-level
// neutral body, which any race may override from data/races/<id>.json.
//
// The figure is 5 wide by 5 tall: a head, arms either side of a spine, then legs
// splaying out to two feet. That is a deliberate compression of a larger 12x8
// version of the same pose. An actor is drawn inside one 32px tile, and 32/12 is
// 2px a cell -- too small for the art fonts to draw as anything but specks -- so
// the figure has to fit a grid that leaves cells the font can actually render.
// Five is the widest grid that still fits a tile at the 6px the ladder
// rasterizes for, and it is the narrowest grid with a centre column, which a
// symmetric figure needs.
//
// The limbs and head outline are deliberately absent from the palette, so every
// one of them falls back to the entity's DrawAscii::characterColor. That one
// detail is what keeps the player (purple) visually distinct from every NPC (dark
// red) while all of them share a single art block: the whole figure takes the
// entity colour, leaving only the eyes and mouth as fixed skin.
//
// The head is centred over the body: the mouth '_' sits on the same column as the
// spine '|' above it, and the arms and legs splay symmetrically either side of
// that column.
inline AsciiArt DefaultHumanoidArt() {
  AsciiArt art;
  // Rows are codepoint lists rather than strings: each eye is U+00F5, which is
  // two bytes in UTF-8, and a std::string row would have counted those bytes as
  // two separate cells instead of one character. U+00F5 is part of the Latin-1
  // stretch of the art vocabulary (AsciiArtTypes.hpp), which the game's font
  // covers, so it is drawn as a glyph like any other.
  art.lines = {
      {'(', 0x00F5, '_', 0x00F5, ')'},   // head
      {'/', ' ', '|', ' ', '\\'},        // arms and spine
      {'|', ' ', '|', ' ', '|'},         // forearms and spine
      {' ', '/', ' ', '\\', ' '},        // legs, splaying from the hips
      {'(', ' ', ' ', ' ', ')'},         // feet
  };
  art.palette = {
      {0x00F5, {222, 184, 135, 255}}, // eyes
      {'_', {222, 184, 135, 255}},    // mouth
  };
  return art;
}
