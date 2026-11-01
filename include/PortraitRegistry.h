#pragma once

#include <string>
#include <unordered_map>
#include <vector>

class DebugLog;

// One character's portrait: a square image that stands for them in the HUD.
//
// Deliberately thin. A portrait is content, not a mechanic, so the definition
// holds only the image reference; everything about where it is drawn belongs to
// the HUD, and everything about who has it belongs to the character.
//
// `sprite` is either an explicit path ("data/sprites/pietro.png") or a sprite
// convention id ("portrait_pietro"). The convention form is the one content should
// prefer, because it resolves through the same art-pack -> generated -> data/sprites
// order as every other sprite, so an art pack can restyle portraits without
// touching this registry.
struct PortraitDef {
  std::string id;
  std::string sprite;
};

// Content database for portraits, the counterpart of StatRegistry/SkillRegistry.
//
// A character names a portrait id in its `Portrait` component; this registry says
// what image that id refers to. Separation matters: map files and race files name
// an id, and a pack can replace the image behind that id without editing every
// character that uses it.
class PortraitRegistry {
public:
  // Reads every .json in `directoryPath`. Malformed files are reported through
  // the injected DebugLog (or stderr) rather than aborting the load.
  void LoadPortraits(const std::string &directoryPath);

  // nullptr for an unknown id. The pointer is stable for the registry's lifetime.
  const PortraitDef *Get(const std::string &id) const;

  // Sorted, so a picker or a report is stable.
  std::vector<std::string> GetIds() const;
  std::vector<const PortraitDef *> GetAll() const;

  size_t Count() const { return portraits.size(); }

  void SetDebugLog(DebugLog *log) { debugLog = log; }

private:
  std::unordered_map<std::string, PortraitDef> portraits;
  DebugLog *debugLog = nullptr;
};
