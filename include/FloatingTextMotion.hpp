#pragma once

#include <algorithm>

// The pure geometry of one floating change number: how far it has risen, how
// far sideways it sits, and how opaque it is. Kept free of raylib and flecs so
// the curve can be pinned by a test that links neither -- the render system only
// translates this into a screen position and a colour.
struct FloatingTextMotion {
  float rise = 0.0f;  // pixels travelled upward since spawn
  float fan = 0.0f;   // horizontal offset in pixels, from the stack index
  float alpha = 1.0f; // 0..1 opacity, 1 is fully opaque
};

// Horizontal offset that separates numbers sharing one target: 0, +1, -1, +2,
// -2 ... in units of `spacing`. The first number sits centred and later ones
// alternate outward, so a batch stays symmetric instead of marching off to one
// side.
inline float FloatingTextFanOffset(int stackIndex, float spacing) {
  const int steps =
      (stackIndex % 2 == 1) ? (stackIndex + 1) / 2 : -(stackIndex / 2);
  return static_cast<float>(steps) * spacing;
}

// Evaluates the motion at `elapsed` seconds into a `lifetime`-second life.
//
// The rise eases out (fast off the entity, slowing as it fades), which reads as
// a pop rather than a linear slide. `fadeStart` and `fadeEnd` are fractions of
// the lifetime, so the default 0.66 keeps the number fully opaque for the first
// two thirds and fades it over the last third.
inline FloatingTextMotion EvaluateFloatingTextMotion(
    float elapsed, float lifetime, float riseDistance, int stackIndex,
    float fanSpacing, float fadeStart = 0.66f, float fadeEnd = 1.0f) {
  FloatingTextMotion motion;
  motion.fan = FloatingTextFanOffset(stackIndex, fanSpacing);

  if (lifetime <= 0.0f) {
    motion.alpha = 0.0f;
    return motion;
  }

  const float progress = std::clamp(elapsed / lifetime, 0.0f, 1.0f);
  const float eased = 1.0f - (1.0f - progress) * (1.0f - progress);
  motion.rise = riseDistance * eased;

  if (progress <= fadeStart) {
    motion.alpha = 1.0f;
  } else if (progress >= fadeEnd || fadeEnd <= fadeStart) {
    motion.alpha = 0.0f;
  } else {
    motion.alpha = 1.0f - (progress - fadeStart) / (fadeEnd - fadeStart);
  }

  return motion;
}
