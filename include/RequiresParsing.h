#pragma once

#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

// Parses a subject template's "requires" capability gate -- `{"equipped":
// "scythe"}`, or an array of such tags -- into the list of equipped tags it
// demands.
//
// One implementation for both the object spawner and the guide, for the same
// reason ParseTrainingGrants is shared between recipes and templates: the two
// describe the same gate, and two copies would drift the moment the shape
// changes. The guide has to report exactly the gate the engine will enforce,
// otherwise it promises a harvest that then fails with "you need a scythe".
//
// Malformed entries are reported through `warn` and skipped rather than aborting
// the load, matching the loaders. Returns an empty list when the template
// declares no gate.
//
// Header-only and dependency-light on purpose: it is a pure function over JSON,
// with no state and no link dependency on the ECS.
inline std::vector<std::string> ParseRequiredTags(
    const nlohmann::json &tmpl, const std::string &ownerId,
    const std::function<void(const std::string &)> &warn) {
  std::vector<std::string> tags;

  if (!tmpl.contains("requires")) {
    warn("Warning: object template '" + ownerId +
         "' lists the Requires component but has no 'requires' block; it is "
         "ungated.\n");
    return tags;
  }

  const auto &requiresJson = tmpl["requires"];
  if (!requiresJson.is_object()) {
    warn("Warning: object template '" + ownerId +
         "' has a non-object 'requires'; it is ungated.\n");
    return tags;
  }

  if (requiresJson.contains("equipped")) {
    const auto &equippedJson = requiresJson["equipped"];
    if (equippedJson.is_string()) {
      tags.push_back(equippedJson.get<std::string>());
    } else if (equippedJson.is_array()) {
      for (const auto &tagJson : equippedJson) {
        if (!tagJson.is_string() || tagJson.get<std::string>().empty()) {
          warn("Warning: object template '" + ownerId +
               "' has a 'requires.equipped' entry that is not a non-empty "
               "string; skipping it.\n");
          continue;
        }
        tags.push_back(tagJson.get<std::string>());
      }
    } else {
      warn("Warning: object template '" + ownerId +
           "' has a 'requires.equipped' that is neither a string nor an "
           "array; ignoring it.\n");
    }
  }

  if (tags.empty()) {
    warn("Warning: object template '" + ownerId +
         "' lists the Requires component but requires nothing; it is "
         "ungated.\n");
  }
  return tags;
}
