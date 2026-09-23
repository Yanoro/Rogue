#pragma once

#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "SkillXp.hpp"

// Parses a "trains" object -- `{"blacksmithing": 8}` -- shared by recipe files
// and object templates.
//
// One implementation for both because the two say exactly the same thing about
// their subject, and two copies would drift the moment the shape changes. The
// companion to ParseEffectEntries: that one reads what a *source* affects, this
// one reads what a *subject* teaches.
//
// Malformed entries are reported through `warn` and skipped rather than aborting
// the load.
void ParseTrainingGrants(const nlohmann::json &trains, const std::string &ownerId,
                         std::vector<SkillXp> &out,
                         const std::function<void(const std::string &)> &warn);
