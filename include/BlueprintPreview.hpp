#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <utility>

#include "BlueprintExpansion.hpp"
#include "Components.h"
#include "SpriteDraw.hpp"
#include "SpritePaths.hpp"
#include "TileRegistry.h"
#include "imgui.h"
#include "raylib.h"

// Rendering a blueprint into an ImGui pane, shared by the two windows that show
// one: the Designer's preview of the latest reply, and the Blueprints window's
// preview of the design under the cursor.
//
// A header of inline functions for the same reason SpriteDraw.hpp and
// AsciiArtDraw.hpp are: the two windows must not be able to lay a design out
// differently from each other, and from the world. Everything here reuses the
// world's own pieces -- SpriteFitRect for sprite layout, ResolveSpriteTexture for
// resolution order, the catalog's colours and glyphs -- so the preview cannot
// drift from what the map draws.
//
// What it is NOT: pixel-identical to the screen. The game renders into a 1920x1080
// texture that the camera then scales, so matching the screen exactly would mean
// rendering through the world camera. At kDefaultZoom (64px, the native tile size)
// the tile *content* is identical and sprites are crisp; only the raster differs.
namespace BlueprintPreview {

// Cell sizes, in pixels. kDefaultZoom is the game's native tile size and the only
// value at which the preview is exactly what the map shows; the range exists for
// inspecting a design too large to fit, not as the normal way to view one.
inline constexpr float kMinZoom = 16.0f;
inline constexpr float kMaxZoom = 128.0f;
inline constexpr float kDefaultZoom = 64.0f;

// The zoom slider plus its reset. `idSuffix` keeps the ImGui ids distinct when
// two windows are open at once, which they can be.
inline void DrawZoomControl(const char *idSuffix, float &zoom) {
  zoom = std::clamp(zoom, kMinZoom, kMaxZoom);

  ImGui::SetNextItemWidth(150.0f);
  ImGui::SliderFloat(idSuffix, &zoom, kMinZoom, kMaxZoom, "%.0f px/cell",
                     ImGuiSliderFlags_AlwaysClamp);
  ImGui::SameLine();
  const bool reset = ImGui::SmallButton((std::string("native##") + idSuffix).c_str());
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip(
        "Reset to %d px per cell, the size the game draws a tile at.\nThis is the "
        "only zoom at which the preview is exactly what the map shows.",
        static_cast<int>(kDefaultZoom));
  }
  if (reset) {
    zoom = kDefaultZoom;
  }
}

// The design itself: every cell drawn the way the world draws it, scrolling when
// it does not fit. `height` is the canvas height in pixels.
inline void DrawCanvas(const char *childId, const Blueprint &blueprint,
                       const BlueprintExpansion::Expansion &expansion, float zoom,
                       const TileRegistry &tiles, ResourceManager &resources,
                       float height) {
  const float cell = zoom;
  const int minX = blueprint.minX;
  const int minY = blueprint.minY;
  const int width = blueprint.Width();
  const int heightCells = blueprint.Height();

  ImGui::BeginChild(childId, ImVec2(0, height), true,
                    ImGuiWindowFlags_HorizontalScrollbar);
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  ImDrawList *draw = ImGui::GetWindowDrawList();

  // Indexed by cell so a lookup per drawn cell is a hash hit rather than a scan.
  std::map<std::pair<int, int>, const BlueprintExpansion::ExpandedCell *> at;
  for (const auto &c : expansion.cells) {
    at[{c.x, c.y}] = &c;
  }

  // Only the cells on screen are submitted. At 128 px/cell a large design is
  // thousands of pixels wide, and drawing every cell of it every frame -- each one
  // a rectangle plus a texture or a glyph -- is work done only to have ImGui clip
  // it away.
  const ImVec2 clipMin = ImGui::GetWindowPos();
  const ImVec2 clipMax(clipMin.x + ImGui::GetWindowSize().x,
                       clipMin.y + ImGui::GetWindowSize().y);

  for (int y = 0; y < heightCells; ++y) {
    for (int x = 0; x < width; ++x) {
      const ImVec2 topLeft(origin.x + static_cast<float>(x) * cell,
                           origin.y + static_cast<float>(y) * cell);
      if (topLeft.x > clipMax.x || topLeft.y > clipMax.y ||
          topLeft.x + cell < clipMin.x || topLeft.y + cell < clipMin.y) {
        continue;
      }
      const ImVec2 bottomRight(topLeft.x + cell, topLeft.y + cell);

      const auto found = at.find({minX + x, minY + y});
      if (found == at.end()) {
        // A cell the design does not cover: faint, so the footprint is readable
        // without pretending it is part of the building.
        draw->AddRectFilled(topLeft, bottomRight, IM_COL32(0, 0, 0, 40));
        continue;
      }

      const BlueprintExpansion::ExpandedCell &c = *found->second;
      if (c.carve) {
        // An opening reads as an opening: no art, just a marked cell.
        draw->AddRectFilled(topLeft, bottomRight, IM_COL32(20, 20, 20, 255));
        draw->AddRect(topLeft, bottomRight, IM_COL32(120, 120, 120, 255));
        continue;
      }
      if (!c.object.empty()) {
        // Objects are not resolved here: their art lives in a template the world
        // applies at spawn, and this pane has no instance of it. The cell is marked
        // and named so a fixture is still identifiable.
        draw->AddRectFilled(topLeft, bottomRight, IM_COL32(60, 45, 30, 255));
        draw->AddRect(topLeft, bottomRight, IM_COL32(200, 160, 90, 255));
        draw->AddText(ImVec2(topLeft.x + 2.0f, topLeft.y + 2.0f),
                      IM_COL32(220, 190, 140, 255), c.object.c_str());
        continue;
      }
      if (c.tile.empty()) {
        continue;
      }

      const TileDef *definition = tiles.Get(c.tile);

      // The plate, exactly as the world draws it: the tile's own background inside
      // the cell. Drawn even when a sprite follows, because a sprite need not be
      // full-bleed.
      const Color plate =
          definition != nullptr ? definition->backgroundColor : Color{40, 40, 40, 255};
      draw->AddRectFilled(topLeft, bottomRight,
                          ImGui::ColorConvertFloat4ToU32(
                              ImVec4(plate.r / 255.0f, plate.g / 255.0f,
                                     plate.b / 255.0f, plate.a / 255.0f)));

      // The sprite, through the world's own resolver and fitting function, so
      // scale, snapping and centring cannot drift from what the map shows.
      bool drewSprite = false;
      const Texture2D *texture = ResolveSpriteTexture(
          definition != nullptr ? definition->sprite : std::string(),
          TileSpriteId(c.tile), resources, nullptr);
      if (texture != nullptr) {
        const SpriteRect fit =
            SpriteFitRect(*texture, topLeft.x, topLeft.y, cell, cell);
        if (fit.width > 0.0f && fit.height > 0.0f) {
          draw->AddImage(
              static_cast<ImTextureID>(static_cast<std::intptr_t>(texture->id)),
              ImVec2(fit.x, fit.y), ImVec2(fit.x + fit.width, fit.y + fit.height));
          drewSprite = true;
        }
      }

      // The glyph, the world's fallback. Its centring is the same maths as
      // DrawAsciiGlyphRaylib but measured with the ImGui font, because that is what
      // the UI draws with -- the one place a preview cannot be pixel-identical to
      // the map.
      if (!drewSprite && definition != nullptr && definition->hasCharacter) {
        const char glyph[2] = {definition->character, '\0'};
        const ImVec2 size = ImGui::CalcTextSize(glyph);
        const ImVec2 textPos(std::round(topLeft.x + (cell - size.x) * 0.5f),
                             std::round(topLeft.y + (cell - size.y) * 0.5f));
        const Color ink = definition->characterColor;
        draw->AddText(textPos,
                      ImGui::ColorConvertFloat4ToU32(
                          ImVec4(ink.r / 255.0f, ink.g / 255.0f, ink.b / 255.0f,
                                 ink.a / 255.0f)),
                      glyph);
      }
    }
  }

  // Reserve the canvas rectangle in the layout so the child scrolls to it.
  ImGui::Dummy(
      ImVec2(static_cast<float>(width) * cell, static_cast<float>(heightCells) * cell));
  ImGui::EndChild();
}

// One swatch and name per distinct material, which is what explains the canvas.
inline void DrawLegend(const BlueprintExpansion::Expansion &expansion,
                       const TileRegistry &tiles) {
  std::map<std::string, ImU32> colors;
  for (const auto &c : expansion.cells) {
    if (c.tile.empty()) continue;
    const TileDef *definition = tiles.Get(c.tile);
    const Color plate =
        definition != nullptr ? definition->backgroundColor : Color{40, 40, 40, 255};
    colors.emplace(c.tile, ImGui::ColorConvertFloat4ToU32(
                               ImVec4(plate.r / 255.0f, plate.g / 255.0f,
                                      plate.b / 255.0f, plate.a / 255.0f)));
  }

  if (!ImGui::TreeNodeEx("Legend", ImGuiTreeNodeFlags_DefaultOpen)) {
    return;
  }
  for (const auto &entry : colors) {
    const ImVec2 swatchTopLeft = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRectFilled(
        swatchTopLeft, ImVec2(swatchTopLeft.x + 14.0f, swatchTopLeft.y + 14.0f),
        entry.second);
    ImGui::GetWindowDrawList()->AddRect(
        swatchTopLeft, ImVec2(swatchTopLeft.x + 14.0f, swatchTopLeft.y + 14.0f),
        IM_COL32(200, 200, 200, 180));
    ImGui::Dummy(ImVec2(18.0f, 14.0f));
    ImGui::SameLine();
    ImGui::Text("%s", entry.first.c_str());
    if (tiles.Get(entry.first) == nullptr) {
      ImGui::SameLine();
      ImGui::TextDisabled("(not in the tile catalog)");
    }
  }
  ImGui::TreePop();
}

}  // namespace BlueprintPreview
