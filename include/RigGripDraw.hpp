#pragma once

#include "RigGrip.hpp"

#include "SpriteDraw.hpp"

#include "imgui.h"

#include <cmath>

namespace RigGrip {

// Draws a hand item's worn art into an ImGui cell with its grip at the hand. `angle` is the
// final screen rotation (WeaponAngle plus any per-view offset/snapping). The arm frame is
// expected to have been drawn into the same box already.
inline void DrawWeapon(ImDrawList *draw, const Texture2D &weapon, const ItemDef &item,
                       const ArmGrip &arm, ImVec2 boxTopLeft, float boxSize,
                       float angle) {
  if (!arm.ok || weapon.id == 0) return;
  const float scale = boxSize / 32.0f;
  const ImVec2 hand(boxTopLeft.x + arm.handX * scale,
                    boxTopLeft.y + arm.handY * scale);
  const float r = angle * 3.14159265358979f / 180.0f;
  const float c = std::cos(r);
  const float s = std::sin(r);
  const auto rotate = [&](float px, float py) {
    const float x = px - static_cast<float>(item.wornGripX);
    const float y = py - static_cast<float>(item.wornGripY);
    return ImVec2(hand.x + (x * c - y * s) * scale,
                  hand.y + (x * s + y * c) * scale);
  };
  const float w = static_cast<float>(weapon.width);
  const float h = static_cast<float>(weapon.height);
  const ImVec2 p0 = rotate(0.0f, 0.0f);
  const ImVec2 p1 = rotate(w, 0.0f);
  const ImVec2 p2 = rotate(w, h);
  const ImVec2 p3 = rotate(0.0f, h);
  const ImVec2 uv0(0.0f, 0.0f);
  const ImVec2 uv1(1.0f, 0.0f);
  const ImVec2 uv2(1.0f, 1.0f);
  const ImVec2 uv3(0.0f, 1.0f);
  SetTextureFilter(weapon, TEXTURE_FILTER_POINT);
  draw->AddImageQuad(
      static_cast<ImTextureID>(static_cast<intptr_t>(weapon.id)), p0, p1, p2, p3, uv0,
      uv1, uv2, uv3, IM_COL32_WHITE);
}

}  // namespace RigGrip
