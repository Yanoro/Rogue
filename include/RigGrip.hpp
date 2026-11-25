#pragma once

#include "ItemDef.hpp"

#include "raylib.h"

#include <algorithm>
#include <cmath>

// The grip path: where a hand item sits on a rigged body, read from the arm's animation
// frame rather than authored per frame.
//
// The hand is the bottom of the arm frame; the arm direction is shoulder-to-hand. The item
// is placed with its grip point at the hand and rotated by the arm direction plus the
// item's rest angle. Deliberately free of ImGui: the world renderer and the windows share
// these numbers, and the drawing helpers live in RigGripDraw.hpp.
namespace RigGrip {

struct ArmGrip {
  bool ok = false;
  float handX = 0.0f;
  float handY = 0.0f;
  // Direction from the shoulder to the hand, in screen degrees (down = 90).
  float angle = 90.0f;
};

inline ArmGrip AnalyzeArm(const Texture2D &texture) {
  ArmGrip grip;
  Image image = LoadImageFromTexture(texture);
  if (image.data == nullptr) return grip;
  int minY = image.height;
  int maxY = -1;
  for (int y = 0; y < image.height; ++y) {
    for (int x = 0; x < image.width; ++x) {
      if (GetImageColor(image, x, y).a > 0) {
        minY = std::min(minY, y);
        maxY = std::max(maxY, y);
      }
    }
  }
  if (maxY < minY) {
    UnloadImage(image);
    return grip;
  }
  const int band = std::max(1, (maxY - minY + 1) / 4);
  const auto centroid = [&](int y0, int y1, float &cx, float &cy) {
    float sx = 0.0f;
    float sy = 0.0f;
    int count = 0;
    for (int y = y0; y <= y1; ++y) {
      for (int x = 0; x < image.width; ++x) {
        if (GetImageColor(image, x, y).a > 0) {
          sx += static_cast<float>(x);
          sy += static_cast<float>(y);
          ++count;
        }
      }
    }
    if (count == 0) return false;
    cx = sx / static_cast<float>(count);
    cy = sy / static_cast<float>(count);
    return true;
  };
  float shoulderX = 0.0f;
  float shoulderY = 0.0f;
  if (!centroid(minY, std::min(maxY, minY + band), shoulderX, shoulderY)) {
    UnloadImage(image);
    return grip;
  }
  if (!centroid(std::max(minY, maxY - band), maxY, grip.handX, grip.handY)) {
    UnloadImage(image);
    return grip;
  }
  UnloadImage(image);
  grip.angle = std::atan2(grip.handY - shoulderY, grip.handX - shoulderX) * 57.29578f;
  grip.ok = true;
  return grip;
}

// The weapon's screen rotation: the arm points down at 0 when the hand is below the
// shoulder, and the item's art points up at angle 0.
inline float WeaponAngle(const ArmGrip &arm, const ItemDef &item) {
  return arm.angle - 90.0f + item.wornGripAngle;
}

}  // namespace RigGrip
