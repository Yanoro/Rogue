#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "EffectParsing.h"
#include "StatDef.hpp"
#include "StatLinks.hpp"

class DebugLog;

// Content database for stats, mirroring RecipeRegistry: load once from a data
// directory, then look definitions up by id. Components store ids and values,
// never definitions, so one definition serves every character and can be
// enumerated for prompts and debug UIs.
//
// Also owns the effects each stat declares (section 4.4): the declarative links
// are the common case, and named hook ids are the escape hatch for effects that
// need real logic. Both live here rather than on the entity, so "what does
// dexterity do" is answered in one place instead of being spread over every
// character that has dexterity.
class StatRegistry {
public:
  // Reads every .json in `directoryPath`. Malformed files and invalid values are
  // reported through the injected DebugLog (or stderr) rather than aborting the
  // load, so one bad file cannot take out every stat.
  void LoadStats(const std::string& directoryPath);

  // Returns nullptr for an unknown id. The pointer is stable for the registry's
  // lifetime (unordered_map does not invalidate references on rehash); do not
  // cache it across a reload.
  const StatDef* Get(const std::string& id) const;

  // Sorted, so prompts and debug UIs list stats in a stable order. Ids are what
  // content references.
  std::vector<std::string> GetIds() const;

  // Every definition, sorted by id. Used to seed a fresh character's StatBlock
  // with the standard set.
  std::vector<const StatDef*> GetAll() const;

  // Effects declared for a stat. Empty for an unknown id and for a stat that
  // declares none, so callers never need a null check.
  const std::vector<StatLink>& GetEffects(const std::string& statId) const {
    return declared.LinksFor(statId);
  }

  // Named C++ hooks this stat activates. Empty when it has none.
  const std::vector<std::string>& GetHookIds(const std::string& statId) const {
    return declared.HooksFor(statId);
  }

  // Every named hook id any stat declares, sorted and de-duplicated. Used once
  // at startup to report ids no handler was registered for, which would
  // otherwise be a silent no-op.
  std::vector<std::string> GetAllHookIds() const {
    return declared.AllHookIds();
  }

  size_t Count() const { return stats.size(); }

  void SetDebugLog(DebugLog* log) { debugLog = log; }

private:
  std::unordered_map<std::string, StatDef> stats;
  DeclaredEffects declared;
  DebugLog* debugLog = nullptr;
};
