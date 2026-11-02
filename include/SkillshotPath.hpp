#pragma once

// The path layer of the skillshot system.
//
// A skillshot's runtime is deliberately a polyline: a list of world-space points.
// Line, crescent, arc and multi-pellet shapes all reduce to a polyline, and once
// the path exists the travel, swept collision and rendering are shared. Adding a
// shape is therefore a branch in BuildSkillshotPath and nothing else -- notably
// not a second collision routine or a second projectile component.
//
// The same builder feeds the aiming indicator, so the preview cannot disagree
// with the projectile that is actually launched.

#include <algorithm>
#include <cmath>
#include <vector>

#include <raylib.h>

#include "SkillshotRegistry.h"

inline Vector2 SkillshotNormalize(Vector2 v) {
  const float length = std::sqrt(v.x * v.x + v.y * v.y);
  if (length <= 0.0001f) {
    return {1.0f, 0.0f};
  }
  return {v.x / length, v.y / length};
}

// Samples the path the skill will travel. `rangePixels` is the skill's configured
// range converted to world pixels; the skill, not the visual, owns that number.
inline std::vector<Vector2> BuildSkillshotPath(const SkillshotDef &def,
                                               Vector2 origin, Vector2 direction,
                                               float rangePixels) {
  const Vector2 dir = SkillshotNormalize(direction);
  const float range = std::max(0.0f, rangePixels);

  if (def.path == "line") {
    return {origin,
            {origin.x + dir.x * range, origin.y + dir.y * range}};
  }

  // Unknown or not-yet-implemented shapes still launch as a line. The registry
  // reports the bad id at load time; falling back here keeps a content typo from
  // producing an invisible spell.
  return {origin, {origin.x + dir.x * range, origin.y + dir.y * range}};
}

inline float SkillshotPathLength(const std::vector<Vector2> &path) {
  float total = 0.0f;
  for (size_t i = 1; i < path.size(); ++i) {
    const float dx = path[i].x - path[i - 1].x;
    const float dy = path[i].y - path[i - 1].y;
    total += std::sqrt(dx * dx + dy * dy);
  }
  return total;
}

// Position along the polyline at `distance` world pixels from the start.
inline Vector2 SkillshotPointAt(const std::vector<Vector2> &path,
                                float distance) {
  if (path.empty()) {
    return {0.0f, 0.0f};
  }
  if (path.size() == 1 || distance <= 0.0f) {
    return path.front();
  }

  float remaining = distance;
  for (size_t i = 1; i < path.size(); ++i) {
    const Vector2 a = path[i - 1];
    const Vector2 b = path[i];
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    const float length = std::sqrt(dx * dx + dy * dy);
    if (length <= 0.0001f) {
      continue;
    }
    if (remaining <= length) {
      const float t = remaining / length;
      return {a.x + dx * t, a.y + dy * t};
    }
    remaining -= length;
  }
  return path.back();
}

// Heading of the segment at `distance`, used to rotate the sprite.
inline Vector2 SkillshotDirectionAt(const std::vector<Vector2> &path,
                                    float distance) {
  if (path.size() < 2) {
    return {1.0f, 0.0f};
  }

  float remaining = std::max(0.0f, distance);
  for (size_t i = 1; i < path.size(); ++i) {
    const Vector2 delta = {path[i].x - path[i - 1].x,
                           path[i].y - path[i - 1].y};
    const float length = std::sqrt(delta.x * delta.x + delta.y * delta.y);
    if (length <= 0.0001f) {
      continue;
    }
    if (remaining <= length) {
      return {delta.x / length, delta.y / length};
    }
    remaining -= length;
  }
  const Vector2 last = {path.back().x - path[path.size() - 2].x,
                        path.back().y - path[path.size() - 2].y};
  return SkillshotNormalize(last);
}

// Distance from a point to a segment, plus where along it the closest point lies
// (0 at `a`, 1 at `b`). The swept collision uses this to ask "did the lance pass
// within a body's radius during this frame?".
inline float SkillshotPointSegmentDistance(Vector2 point, Vector2 a, Vector2 b,
                                           float *t) {
  const Vector2 ab = {b.x - a.x, b.y - a.y};
  const float lengthSquared = ab.x * ab.x + ab.y * ab.y;
  float u = 0.0f;
  if (lengthSquared > 0.0001f) {
    u = ((point.x - a.x) * ab.x + (point.y - a.y) * ab.y) / lengthSquared;
    u = std::clamp(u, 0.0f, 1.0f);
  }
  const Vector2 closest = {a.x + ab.x * u, a.y + ab.y * u};
  const float dx = point.x - closest.x;
  const float dy = point.y - closest.y;
  if (t != nullptr) {
    *t = u;
  }
  return std::sqrt(dx * dx + dy * dy);
}
