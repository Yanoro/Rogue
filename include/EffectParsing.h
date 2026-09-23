#pragma once

#include <algorithm>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "StatLinks.hpp"

// The effects a kind of source declares, with the empty-on-miss accessors every
// caller wants. Both the stat and the skill registry hold one of these, so the
// lookup rules exist in exactly one place.
struct DeclaredEffects {
  std::unordered_map<std::string, std::vector<StatLink>> links;
  std::unordered_map<std::string, std::vector<std::string>> hooks;

  const std::vector<StatLink> &LinksFor(const std::string &id) const {
    static const std::vector<StatLink> none;
    auto it = links.find(id);
    return it == links.end() ? none : it->second;
  }

  const std::vector<std::string> &HooksFor(const std::string &id) const {
    static const std::vector<std::string> none;
    auto it = hooks.find(id);
    return it == hooks.end() ? none : it->second;
  }

  // Sorted and de-duplicated, for the startup report of ids nothing handles.
  std::vector<std::string> AllHookIds() const {
    std::vector<std::string> ids;
    for (const auto &pair : hooks) {
      ids.insert(ids.end(), pair.second.begin(), pair.second.end());
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
  }
};

// Parses the "effects" array shared by stat and skill definitions: a string
// entry names a C++ hook, an object entry is a declarative link.
//
// One implementation for both, because to the effect system a stat and a skill
// are the same thing -- a source with a magnitude -- and two copies of this would
// drift the moment a field is added.
//
// Malformed entries are reported through `warn` and skipped rather than aborting
// the load: one bad effect must not take out a whole file.
void ParseEffectEntries(const nlohmann::json &effects,
                        const std::string &ownerId, DeclaredEffects &out,
                        const std::function<void(const std::string &)> &warn);
