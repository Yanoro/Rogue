#pragma once

// The designer's request for an asset the world does not have.
//
// Why this exists rather than relying on the vocabulary check. A design that names a
// material the catalog lacks is a *symptom*: the designer has already been told to build
// with what it has, so a missing name usually means a mistake, not an intention. The
// intention is what this carries -- "to build this prison properly I need jail cell bars,
// and here is what they are" -- and it is the thing the sprite designer can actually act
// on. The vocabulary check still runs, because a hand-written design can name a material
// without asking for it, and both routes end at the same offer.
//
// `needs` is not part of the blueprint schema. It is stripped before the design is saved,
// exactly like `designerNotes`: a saved blueprint is content, and a request is a message.
//
// The name rule is not cosmetic. An object's key is its name slugified
// (ObjectFactory::KeyFor), and a blueprint's `place` names that *key*, so a requested
// object whose name is not already a key would be created under a key the design does not
// use -- and the design would still be missing it afterwards. So an object need's name is
// required to be key-shaped, and a tile need's name is free-form because a tile is
// addressed by its display name ("wooden floor").
#include <algorithm>
#include <cctype>
#include <regex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

struct BlueprintNeed {
  // The exact string the design uses: the template key for an object, the tile's name
  // for a tile.
  std::string name;
  // "object" or "tile", in the sprite designer's own vocabulary.
  std::string kind;
  // What it is and how it behaves, in the designer's words. This is the brief the sprite
  // designer works from, so it is required rather than optional.
  std::string description;
};

namespace BlueprintNeeds {

inline constexpr int kMaxNeeds = 8;
inline constexpr size_t kMaxNameLength = 48;
inline constexpr size_t kMaxDescriptionLength = 400;

struct ParseResult {
  bool ok = true;
  // Blocking reason, worded to be sent back to the model on the next attempt.
  std::string error;
  std::vector<BlueprintNeed> needs;
};

inline std::string Trim(std::string text) {
  const auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
  text.erase(text.begin(), std::find_if_not(text.begin(), text.end(), isSpace));
  text.erase(std::find_if_not(text.rbegin(), text.rend(), isSpace).base(), text.end());
  return text;
}

// Reads `needs` out of a parsed reply. Absent is not an error -- most designs need
// nothing -- but a malformed one is, because a need that silently disappears is a
// building with a hole and no explanation.
inline ParseResult Parse(const nlohmann::json &reply) {
  ParseResult result;
  const auto entry = reply.find("needs");
  if (entry == reply.end() || entry->is_null()) {
    return result;
  }
  if (!entry->is_array()) {
    result.ok = false;
    result.error =
        "\"needs\" must be an array of { name, kind, description } objects.";
    return result;
  }
  if (entry->size() > static_cast<size_t>(kMaxNeeds)) {
    result.ok = false;
    result.error = "\"needs\" has " + std::to_string(entry->size()) +
                   " entries; at most " + std::to_string(kMaxNeeds) +
                   " can be requested for one design. Ask for the ones the design "
                   "cannot do without.";
    return result;
  }

  // Object keys are lowercase letters, digits and underscores. A tile is addressed by its
  // display name, which is why the two kinds are checked differently.
  static const std::regex keyShape("^[a-z0-9_]+$");

  for (const nlohmann::json &item : *entry) {
    if (!item.is_object()) {
      result.ok = false;
      result.error = "every \"needs\" entry must be an object.";
      return result;
    }
    BlueprintNeed need;
    need.name = Trim(item.value("name", std::string()));
    need.kind = Trim(item.value("kind", std::string()));
    need.description = Trim(item.value("description", std::string()));

    if (need.name.empty()) {
      result.ok = false;
      result.error = "a \"needs\" entry has no \"name\".";
      return result;
    }
    if (need.name.size() > kMaxNameLength) {
      result.ok = false;
      result.error = "the need \"" + need.name + "\" has a name over " +
                     std::to_string(kMaxNameLength) + " characters.";
      return result;
    }
    if (need.kind != "object" && need.kind != "tile") {
      result.ok = false;
      result.error = "the need \"" + need.name +
                     "\" must say \"kind\": \"object\" or \"kind\": \"tile\".";
      return result;
    }
    if (need.kind == "object" && !std::regex_match(need.name, keyShape)) {
      result.ok = false;
      result.error =
          "the object need \"" + need.name +
          "\" is not a template key. An object is placed by key: lowercase letters, "
          "digits and underscores, no spaces -- for example \"jail_cell_bars\". The "
          "description carries the readable name.";
      return result;
    }
    if (need.description.empty()) {
      result.ok = false;
      result.error = "the need \"" + need.name +
                     "\" has no \"description\". Say what it is and how it behaves; "
                     "that is what it will be drawn from.";
      return result;
    }
    if (need.description.size() > kMaxDescriptionLength) {
      need.description.resize(kMaxDescriptionLength);
    }

    // Duplicated requests are one request: two entries with the same name would offer the
    // same asset twice and the second would find the first already created.
    const auto same = [&need](const BlueprintNeed &other) {
      return other.kind == need.kind && other.name == need.name;
    };
    if (std::none_of(result.needs.begin(), result.needs.end(), same)) {
      result.needs.push_back(std::move(need));
    }
  }

  return result;
}

}  // namespace BlueprintNeeds
