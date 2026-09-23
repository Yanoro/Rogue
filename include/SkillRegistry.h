#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "EffectParsing.h"
#include "SkillDef.hpp"

class DebugLog;

// Content database for skills, the exact counterpart of StatRegistry.
//
// Kept separate from StatRegistry rather than folded into it because the two
// answer different questions -- a stat is a fixed attribute, a skill is a
// progression track -- and because the state they drive lives in different
// components. What they share is the part that matters to the effect system: a
// source with a magnitude and a set of declared effects, which is why both hold
// the same DeclaredEffects and share one parser.
class SkillRegistry {
public:
  // Reads every .json in `directoryPath`. Malformed files are reported through
  // the injected DebugLog (or stderr) rather than aborting the load.
  void LoadSkills(const std::string& directoryPath);

  // nullptr for an unknown id. The pointer is stable for the registry's lifetime.
  const SkillDef* Get(const std::string& id) const;

  // Sorted, so prompts and debug UIs are stable.
  std::vector<std::string> GetIds() const;

  // Every definition, sorted by id. Used to seed a fresh character's SkillBlock.
  std::vector<const SkillDef*> GetAll() const;

  const std::vector<StatLink>& GetEffects(const std::string& skillId) const {
    return declared.LinksFor(skillId);
  }

  const std::vector<std::string>& GetHookIds(const std::string& skillId) const {
    return declared.HooksFor(skillId);
  }

  std::vector<std::string> GetAllHookIds() const {
    return declared.AllHookIds();
  }

  size_t Count() const { return skills.size(); }

  void SetDebugLog(DebugLog* log) { debugLog = log; }

private:
  std::unordered_map<std::string, SkillDef> skills;
  DeclaredEffects declared;
  DebugLog* debugLog = nullptr;
};
