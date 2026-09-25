#include "ItemRegistry.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>

#include "DebugLog.h"

void ItemRegistry::LoadItems(const std::string& directoryPath) {
  if (!std::filesystem::exists(directoryPath)) {
    const std::string errStr =
        "Warning: Item directory not found: " + directoryPath + "\n";
    if (debugLog) debugLog->LogError(errStr);
    else std::cerr << errStr;
    return;
  }

  auto warn = [this](const std::string& message) {
    if (debugLog) debugLog->LogWarning(message);
    else std::cerr << message << std::endl;
  };

  for (const auto& entry : std::filesystem::directory_iterator(directoryPath)) {
    if (entry.path().extension() != ".json") continue;

    std::ifstream file(entry.path());
    if (!file.is_open()) continue;

    try {
      nlohmann::json j;
      file >> j;

      ItemDef def;
      def.id = entry.path().stem().string();
      def.name = j.value("name", "");
      def.slot = j.value("slot", "");
      def.occupies = j.value("occupies", 1);
      def.tier = j.value("tier", 1.0f);
      def.scale = j.value("scale", 1.0f);
      def.baseline = j.value("baseline", 0.0f);

      // An item definition exists to be worn, so a missing slot is a content
      // error rather than a valid "carried only" item. Reported here where the
      // file name is still known; the item simply cannot be equipped.
      if (def.slot.empty()) {
        warn("Warning: Item '" + def.id +
             "' declares no 'slot', so it can never be equipped.");
      }
      if (def.occupies < 1) {
        warn("Warning: Item '" + def.id +
             "' has occupies < 1; using 1 instead.");
        def.occupies = 1;
      }
      if (def.tier <= 0.0f) {
        warn("Warning: Item '" + def.id +
             "' has tier <= 0, which would make every effect zero or negative; "
             "using 1 instead.");
        def.tier = 1.0f;
      }
      if (def.scale <= 0.0f) {
        warn("Warning: Item '" + def.id +
             "' has scale <= 0, which would divide by zero when resolving; "
             "using 1 instead.");
        def.scale = 1.0f;
      }

      // Capability tags a requirement can name. A non-string entry is reported
      // and skipped rather than aborting the file, the same contract the effects
      // parser has.
      if (j.contains("tags")) {
        if (!j["tags"].is_array()) {
          warn("Warning: Item '" + def.id +
               "' has a non-array 'tags'; ignoring it.");
        } else {
          for (const auto& tagJson : j["tags"]) {
            if (!tagJson.is_string() || tagJson.get<std::string>().empty()) {
              warn("Warning: Item '" + def.id +
                   "' has a tag that is not a non-empty string; skipping it.");
              continue;
            }
            def.tags.push_back(tagJson.get<std::string>());
          }
        }
      }

      // Durability is opt-in: 0 (or an absent field) means the item never wears
      // out, which is the ordinary case for anything that is not a tool.
      if (j.contains("durability")) {
        if (!j["durability"].is_number_integer()) {
          warn("Warning: Item '" + def.id +
               "' has a non-integer 'durability'; treating it as indestructible.");
        } else if (j["durability"].get<int>() < 0) {
          warn("Warning: Item '" + def.id +
               "' has a negative 'durability'; treating it as indestructible.");
        } else {
          def.durability = j["durability"].get<int>();
        }
      }

      // The activities that wear this item out. Durability without a
      // "consumedBy" would mean an item that can break but never does, which is
      // almost certainly a content mistake, so it is reported.
      if (j.contains("consumedBy")) {
        if (!j["consumedBy"].is_array()) {
          warn("Warning: Item '" + def.id +
               "' has a non-array 'consumedBy'; ignoring it.");
        } else {
          for (const auto& activityJson : j["consumedBy"]) {
            if (!activityJson.is_string() ||
                activityJson.get<std::string>().empty()) {
              warn("Warning: Item '" + def.id +
                   "' has a 'consumedBy' entry that is not a non-empty string; "
                   "skipping it.");
              continue;
            }
            def.consumedBy.push_back(activityJson.get<std::string>());
          }
        }
      }
      if (def.durability > 0 && def.consumedBy.empty()) {
        warn("Warning: Item '" + def.id +
             "' has a durability but no 'consumedBy', so nothing will ever wear "
             "it out.");
      }

      items[def.id] = def;

      if (j.contains("effects")) {
        ParseEffectEntries(j["effects"], def.id, declared, warn);
      }
    } catch (const std::exception& e) {
      warn("Failed to parse item " + entry.path().string() + ": " +
           std::string(e.what()));
    }
  }
}

const ItemDef* ItemRegistry::Get(const std::string& id) const {
  auto it = items.find(id);
  if (it == items.end()) return nullptr;
  return &it->second;
}

std::vector<std::string> ItemRegistry::GetIds() const {
  std::vector<std::string> ids;
  ids.reserve(items.size());
  for (const auto& pair : items) {
    ids.push_back(pair.first);
  }
  std::sort(ids.begin(), ids.end());
  return ids;
}

std::vector<const ItemDef*> ItemRegistry::GetAll() const {
  std::vector<const ItemDef*> defs;
  defs.reserve(items.size());
  for (const std::string& id : GetIds()) {
    defs.push_back(Get(id));
  }
  return defs;
}
