#pragma once

#include "AsciiArt.hpp"
#include "AsciiArtFonts.hpp"
#include "Components.h"
#include "imgui.h"
#include "raylib.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <string>

// The two renderers for AsciiArt, kept in one header so they cannot drift apart.
//
// Both consume the same layout from AsciiArt.hpp, so a chest cannot be centred
// differently on the map than it is in the storage window. They differ in the
// font and the call they make per cell: raylib's DrawTextCodepoint for the world
// (through the art font ladder), an ImGui draw-list AddText for the UI (through
// ArtImGuiFont, because ImGui draws with its own font).

// The single-glyph fallback: one codepoint centred in the entity's box. This is
// the renderer exactly as it behaved before AsciiArt existed, so an object
// without "art" is pixel-for-pixel unchanged.
inline void DrawAsciiGlyphRaylib(Font font, const DrawAscii &ascii, float x,
                                 float y) {
  const char buf[2] = {ascii.ch, '\0'};
  const float fontSize =
      static_cast<float>(std::min(ascii.width, ascii.height));
  const Vector2 textSize = MeasureTextEx(font, buf, fontSize, 0.0f);
  const Vector2 textPos = {
      std::round(x + (static_cast<float>(ascii.width) - textSize.x) / 2.0f),
      std::round(y + (static_cast<float>(ascii.height) - textSize.y) / 2.0f)};

  DrawTextCodepoint(font, static_cast<int>(ascii.ch), textPos, fontSize,
                    ascii.characterColor);
}

// The multi-line art: one codepoint per non-blank cell, each centred in its cell.
//
// The font comes from the ladder rather than being the game's single 32px font.
// An art cell is the smallest text in the game, and a glyph rasterized at its own
// size is the difference between a letter and a smudge at 8px.
inline void DrawAsciiArtRaylib(const AsciiArtFonts &fonts, const AsciiArt &art,
                               Color fallback, float x, float y, float width,
                               float height) {
  const AsciiGrid grid = ComputeAsciiGrid(art, x, y, width, height);
  // Cell-sized rather than box-sized: a glyph must fit its cell or neighbouring
  // cells overlap.
  const int cellSize = std::min(grid.cellWidth, grid.cellHeight);
  const float fontSize = static_cast<float>(cellSize);
  const Font font = fonts.ForCellSize(cellSize);

  for (int row = 0; row < grid.rows; ++row) {
    for (int col = 0; col < grid.cols; ++col) {
      const int codepoint = AsciiArtCell(art, col, row);
      if (codepoint == ' ') {
        continue;
      }

      const float cellX =
          grid.originX + static_cast<float>(col * grid.cellWidth);
      const float cellY =
          grid.originY + static_cast<float>(row * grid.cellHeight);
      const float cellWidth = static_cast<float>(grid.cellWidth);
      const float cellHeight = static_cast<float>(grid.cellHeight);
      const Color color = AsciiArtColor(art, fallback, codepoint);

      // Blocks and shades are filled, not drawn: see AsciiBlockRect for why a
      // glyph cannot be trusted to cover its cell.
      AsciiBlockArea block;
      if (AsciiBlockRect(codepoint, block)) {
        DrawRectangleV({std::round(cellX + block.x * cellWidth),
                        std::round(cellY + block.y * cellHeight)},
                       {std::round(block.width * cellWidth),
                        std::round(block.height * cellHeight)},
                       color);
        continue;
      }
      const float shadeAlpha = AsciiShadeAlpha(codepoint);
      if (shadeAlpha > 0.0f) {
        DrawRectangleV({cellX, cellY}, {cellWidth, cellHeight},
                       Color{color.r, color.g, color.b,
                             static_cast<unsigned char>(color.a * shadeAlpha)});
        continue;
      }

      // Everything else is a glyph. Art is stored as codepoints, so it is
      // re-encoded only to measure; the draw call takes the codepoint directly.
      char buf[5];
      EncodeUtf8Codepoint(codepoint, buf);
      const Vector2 textSize = MeasureTextEx(font, buf, fontSize, 0.0f);
      const Vector2 textPos = {
          std::round(cellX + (cellWidth - textSize.x) / 2.0f),
          std::round(cellY + (cellHeight - textSize.y) / 2.0f)};

      DrawTextCodepoint(font, codepoint, textPos, fontSize, color);
    }
  }
}

// Dispatches between the two world renderers. `x` and `y` are the top-left of the
// entity's DrawAscii box; the caller owns the background rectangle, as before.
// `glyphFont` is the game's normal font, used only by the single-glyph fallback.
inline void DrawAsciiEntityRaylib(const AsciiArtFonts &fonts, Font glyphFont,
                                  const DrawAscii &ascii, const AsciiArt *art,
                                  float x, float y) {
  if (art != nullptr && !art->lines.empty()) {
    DrawAsciiArtRaylib(fonts, *art, ascii.characterColor, x, y,
                       static_cast<float>(ascii.width),
                       static_cast<float>(ascii.height));
    return;
  }
  DrawAsciiGlyphRaylib(glyphFont, ascii, x, y);
}

// The ImGui renderer. `topLeft`/`size` are the rectangle the art must fill --
// for a palette button or an inventory row that is the widget rect, not the
// DrawAscii box, so art is laid out to whatever cell the caller is drawing in.
inline void DrawAsciiArtImGui(const AsciiArt &art, Color fallback,
                              ImVec2 topLeft, ImVec2 size) {
  if (art.lines.empty()) {
    return;
  }

  ImDrawList *drawList = ImGui::GetWindowDrawList();
  // Only the glyph path needs a font, and ImGui's built-in one is ASCII-only, so
  // a box-drawing character would come out blank. ArtImGuiFont() is the font
  // Game::Init adds when it can; blocks and shades do not care either way,
  // because they are filled rather than drawn.
  ImFont *font = ArtImGuiFont() != nullptr ? ArtImGuiFont() : ImGui::GetFont();
  if (drawList == nullptr || font == nullptr) {
    return;
  }

  const AsciiGrid grid =
      ComputeAsciiGrid(art, topLeft.x, topLeft.y, size.x, size.y);
  const float fontSize =
      static_cast<float>(std::min(grid.cellWidth, grid.cellHeight));

  for (int row = 0; row < grid.rows; ++row) {
    for (int col = 0; col < grid.cols; ++col) {
      const int codepoint = AsciiArtCell(art, col, row);
      if (codepoint == ' ') {
        continue;
      }

      const float cellX =
          grid.originX + static_cast<float>(col * grid.cellWidth);
      const float cellY =
          grid.originY + static_cast<float>(row * grid.cellHeight);
      const float cellWidth = static_cast<float>(grid.cellWidth);
      const float cellHeight = static_cast<float>(grid.cellHeight);
      const Color color = AsciiArtColor(art, fallback, codepoint);

      // Same rule as the world renderer: blocks and shades are filled, so the
      // two cannot disagree about what an art block looks like. See
      // AsciiBlockRect.
      AsciiBlockArea block;
      if (AsciiBlockRect(codepoint, block)) {
        drawList->AddRectFilled(
            ImVec2(cellX + block.x * cellWidth, cellY + block.y * cellHeight),
            ImVec2(cellX + (block.x + block.width) * cellWidth,
                   cellY + (block.y + block.height) * cellHeight),
            IM_COL32(color.r, color.g, color.b, color.a));
        continue;
      }
      const float shadeAlpha = AsciiShadeAlpha(codepoint);
      if (shadeAlpha > 0.0f) {
        drawList->AddRectFilled(
            ImVec2(cellX, cellY), ImVec2(cellX + cellWidth, cellY + cellHeight),
            IM_COL32(color.r, color.g, color.b,
                     static_cast<int>(color.a * shadeAlpha)));
        continue;
      }

      // ImGui's text API takes UTF-8, so the codepoint is encoded back for it.
      char buf[5];
      EncodeUtf8Codepoint(codepoint, buf);
      const ImVec2 textSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, buf);

      drawList->AddText(
          font, fontSize,
          ImVec2(std::round(cellX + (cellWidth - textSize.x) / 2.0f),
                 std::round(cellY + (cellHeight - textSize.y) / 2.0f)),
          IM_COL32(color.r, color.g, color.b, color.a), buf);
    }
  }
}
