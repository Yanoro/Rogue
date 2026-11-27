#pragma once

#include "imgui.h"
#include "raylib.h"

// The plate behind one inventory or container cell.
//
// Both windows draw the same asset, so one visual-asset theme (`inventory_slot`) changes
// every slot at once. `plate` is the resolved `inventory_slot` texture and may be null --
// the asset may not have art yet -- in which case the flat rectangle both windows used
// before is drawn, so a missing picture degrades to the old look instead of to a hole.
inline void DrawInventorySlotPlate(ImDrawList *draw, const ImVec2 &min, float size,
                                   const Texture2D *plate, bool available) {
  const ImVec2 max(min.x + size, min.y + size);
  if (plate != nullptr) {
    draw->AddImage(static_cast<ImTextureID>(static_cast<intptr_t>(plate->id)), min,
                   max, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
                   available ? IM_COL32_WHITE : IM_COL32(255, 255, 255, 105));
    return;
  }
  draw->AddRectFilled(min, max,
                      available ? IM_COL32(30, 32, 36, 150)
                                : IM_COL32(18, 19, 22, 180));
  draw->AddRect(min, max,
                available ? IM_COL32(120, 125, 135, 220)
                          : IM_COL32(70, 74, 82, 190),
                2.0f);
}
