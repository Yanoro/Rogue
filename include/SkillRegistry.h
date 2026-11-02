#pragma once

#include <string>
#include <nlohmann/json.hpp>
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
  // The outcome of replacing one definition from the Content Editor or a
  // content.editor/skills override. Warnings are the same authoring warnings the
  // file loader reports; `error` means the document was rejected outright.
  struct AddResult {
    bool ok = false;
    std::string error;
    std::vector<std::string> warnings;
  };

  // Reads every .json in `directoryPath`. Malformed files are reported through
  // the injected DebugLog (or stderr) rather than aborting the load.
  void LoadSkills(const std::string& directoryPath);

  // Replaces one skill from a full document (the Content Editor's save path and
  // the content.editor/skills startup layer). The document's "id" is the key;
  // when absent, the source file's stem is used. Effects are replaced with the
  // document's, not merged, so deleting an effect in the editor deletes it live.
  AddResult OverrideDefinition(const nlohmann::json& document,
                               const std::string& sourcePath = std::string());

  // The complete normalized document for one skill, ready for the generic form:
  // every known key is present with its live value, and the fixed "id" is
  // omitted because the editor supplies it from the selected entry.
  nlohmann::json ToJson(const std::string& id) const;

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
  // The authored document, kept so the Content Editor can round-trip unknown
  // fields and the effects array instead of reconstructing a lossy subset.
  std::unordered_map<std::string, nlohmann::json> sources;
  DeclaredEffects declared;
  DebugLog* debugLog = nullptr;
};
