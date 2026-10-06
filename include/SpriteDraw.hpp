#pragma once

#include "imgui.h"
#include "raylib.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

// The one place a Sprite becomes pixels, for the same reason AsciiArtDraw.hpp
// holds both art renderers: the world, the map editor's template palette and the
// inventory rows all draw sprites, and they must not be able to lay one out
// differently from each other.
//
// The image is fitted inside the caller's box preserving aspect, then centred on
// whole pixels -- the discipline ComputeAsciiGrid already applies to art cells,
// and for the same reason: a point-filtered texture landing on a half-pixel shows
// uneven pixels.
//
// Scale is snapped to a whole number whenever it is at least 1, so a 16px sprite
// in a 32px tile lands at exactly 2x rather than 1.97x of uneven pixels. A sprite
// larger than its box is downscaled by a fraction instead, since no whole-number
// choice exists and shrinking beats overflowing the box.
struct SpriteRect {
  float x = 0.0f;
  float y = 0.0f;
  float width = 0.0f;
  float height = 0.0f;
};

// Where the sprite lands inside the box. A zero width means the texture is
// unusable, which both renderers treat as "draw nothing" rather than as an error.
inline SpriteRect SpriteFitRect(const Texture2D &texture, float x, float y,
                                float width, float height) {
  if (texture.id == 0 || texture.width <= 0 || texture.height <= 0) {
    return {};
  }

  const float byWidth = width / static_cast<float>(texture.width);
  const float byHeight = height / static_cast<float>(texture.height);
  float scale = std::min(byWidth, byHeight);
  if (scale >= 1.0f) {
    scale = std::floor(scale);
  }

  const float drawnWidth = static_cast<float>(texture.width) * scale;
  const float drawnHeight = static_cast<float>(texture.height) * scale;
  return {std::round(x + (width - drawnWidth) / 2.0f),
          std::round(y + (height - drawnHeight) / 2.0f), std::round(drawnWidth),
          std::round(drawnHeight)};
}

// The world renderer. Returns the rectangle it drew, so a caller that also needs
// it (the debug overlay that outlines the sprite) does not repeat the arithmetic.
inline Rectangle DrawSpriteFitted(const Texture2D &texture, float x, float y,
                                  float width, float height) {
  const SpriteRect fit = SpriteFitRect(texture, x, y, width, height);
  if (fit.width <= 0.0f || fit.height <= 0.0f) {
    return {0.0f, 0.0f, 0.0f, 0.0f};
  }

  const Rectangle source = {0.0f, 0.0f, static_cast<float>(texture.width),
                            static_cast<float>(texture.height)};
  const Rectangle destination = {fit.x, fit.y, fit.width, fit.height};

  // White tint: the sprite carries its own colours, unlike an art block whose
  // palette falls back to DrawAscii::characterColor.
  DrawTexturePro(texture, source, destination, {0.0f, 0.0f}, 0.0f, WHITE);
  return destination;
}

// The ImGui renderer, mirroring DrawAsciiArtImGui. `topLeft`/`size` are the
// rectangle the sprite must fill -- for a palette button or an inventory row that
// is the widget rect, not the entity's DrawAscii box, so it is laid out to
// whatever cell the caller is drawing in.
//
// Drawn on the window's draw list rather than with ImGui::Image because these
// callers draw the sprite *over* an already-created widget (the palette button
// carries the click target and the tooltip), which is exactly how the art
// preview does it.
inline void DrawSpriteImGui(const Texture2D &texture, ImVec2 topLeft,
                            ImVec2 size) {
  const SpriteRect fit =
      SpriteFitRect(texture, topLeft.x, topLeft.y, size.x, size.y);
  if (fit.width <= 0.0f || fit.height <= 0.0f) {
    return;
  }
  ImDrawList *drawList = ImGui::GetWindowDrawList();
  if (drawList == nullptr) {
    return;
  }
  drawList->AddImage(static_cast<ImTextureID>(
                         static_cast<std::intptr_t>(texture.id)),
                     ImVec2(fit.x, fit.y),
                     ImVec2(fit.x + fit.width, fit.y + fit.height));
}
