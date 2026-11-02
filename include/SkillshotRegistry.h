#pragma once

#include <string>
#include <unordered_map>
#include <vector>

class DebugLog;

// One aimed delivery's presentation and motion: the sprite, how fast it crosses
// the world, how wide it is, and which path generator draws it.
//
// The crucial field is `path`. A SkillshotRuntime holds a sampled polyline, and
// travel, collision and rendering all walk that polyline without knowing which
// shape produced it. "line" is the only generator today; an arc/crescent (Diana
// Q) or a spread of lines (Ashe W) is a new branch in BuildSkillshotPath, not a
// new runtime, collision test or renderer.
//
// Range is deliberately NOT here. The skill's activation owns range, because the
// same ice-lance visual should be usable by a short stab and a long throw.
struct SkillshotDef {
  std::string id;
  std::string sprite;
  // "line" today. Unknown values fall back to a line with a warning.
  std::string path = "line";
  // World pixels per second. One tile is 32 world pixels at the maps this game
  // ships, so 950 is roughly thirty tiles a second -- a lance, not a fireball.
  float speed = 950.0f;
  // Collision width and the sprite's drawn breadth, in world pixels.
  float width = 12.0f;
  // The sprite's drawn length along its direction, in world pixels. Generated
  // art points right; the renderer rotates it to the path's heading.
  float length = 34.0f;
};

// Content database for aimed deliveries, the counterpart of ProjectileRegistry.
class SkillshotRegistry {
public:
  void LoadSkillshots(const std::string &directoryPath);

  const SkillshotDef *Get(const std::string &id) const;
  std::vector<std::string> GetIds() const;
  std::vector<const SkillshotDef *> GetAll() const;

  size_t Count() const { return skillshots.size(); }

  void SetDebugLog(DebugLog *log) { debugLog = log; }

private:
  std::unordered_map<std::string, SkillshotDef> skillshots;
  DebugLog *debugLog = nullptr;
};
