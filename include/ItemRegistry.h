#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "EffectParsing.h"
#include "ItemDef.hpp"

class DebugLog;

// Content database for equippable items, the exact counterpart of StatRegistry
// and SkillRegistry.
//
// Kept separate from ObjectFactory's templates rather than folded into them
// because the two answer different questions: a template says what an object
// looks like and does in the world (its draw character, its drops, whether it is
// a storage container), while this says what it does when worn. What it shares
// with the other two registries is the part the effect system cares about -- a
// source id with an effects list -- which is why it holds the same
// DeclaredEffects and reuses the same parser.
class ItemRegistry {
public:
  // Reads every .json in `directoryPath`. Malformed files are reported through
  // the injected DebugLog (or stderr) rather than aborting the load.
  void LoadItems(const std::string& directoryPath);

  // nullptr for an unknown id. The pointer is stable for the registry's lifetime.
  const ItemDef* Get(const std::string& id) const;

  // Sorted, so prompts and debug UIs are stable.
  std::vector<std::string> GetIds() const;

  // Every definition, sorted by id. Used by the startup slot check, which needs
  // to know every kind of slot the content asks for.
  std::vector<const ItemDef*> GetAll() const;

  // Effects declared for an item. Empty for an unknown id and for an item that
  // declares none, so callers never need a null check.
  const std::vector<StatLink>& GetEffects(const std::string& itemId) const {
    return declared.LinksFor(itemId);
  }

  // Named C++ hooks this item activates. Empty when it has none.
  const std::vector<std::string>& GetHookIds(const std::string& itemId) const {
    return declared.HooksFor(itemId);
  }

  // Every named hook id any item declares, sorted and de-duplicated. Merged into
  // the startup report of ids no handler was registered for.
  std::vector<std::string> GetAllHookIds() const {
    return declared.AllHookIds();
  }

  size_t Count() const { return items.size(); }

  void SetDebugLog(DebugLog* log) { debugLog = log; }

private:
  std::unordered_map<std::string, ItemDef> items;
  DeclaredEffects declared;
  DebugLog* debugLog = nullptr;
};
