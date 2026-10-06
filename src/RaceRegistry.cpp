#include "RaceRegistry.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>

#include "DebugLog.h"
#include "AsciiArt.hpp"

void RaceRegistry::LoadRaces(const std::string& directoryPath) {
  if (!std::filesystem::exists(directoryPath)) {
    const std::string errStr =
        "Warning: Race directory not found: " + directoryPath + "\n";
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

      RaceDef def;
      def.id = entry.path().stem().string();
      def.name = j.value("name", def.id);

      // The race's raster look. Optional, and independent of "art": a race may
      // declare either, both or neither, and a missing file is not an error here
      // -- AttachSprite decides that at spawn, where the fallback to art lives.
      if (j.contains("sprite")) {
        if (j["sprite"].is_string()) {
          def.sprite = j["sprite"].get<std::string>();
        } else {
          warn("Warning: Race '" + def.id +
               "' has a non-string 'sprite'; ignoring it.");
        }
      }

      if (j.contains("slots")) {
        if (!j["slots"].is_array()) {
          warn("Warning: Race '" + def.id +
               "' has a non-array 'slots'; ignoring it.");
        } else {
          for (const auto& slotJson : j["slots"]) {
            SlotSpec slot;
            if (slotJson.is_string()) {
              // Shorthand for a slot whose kind is its own name, which keeps the
              // common body readable: "head" rather than {"id":"head",
              // "accepts":"head"}.
              slot.id = slotJson.get<std::string>();
              slot.accepts = slot.id;
            } else if (slotJson.is_object()) {
              slot.id = slotJson.value("id", "");
              slot.accepts = slotJson.value("accepts", slot.id);
            } else {
              warn("Warning: Race '" + def.id +
                   "' has a slot that is neither a string nor an object; "
                   "skipping it.");
              continue;
            }
            if (slot.id.empty()) {
              warn("Warning: Race '" + def.id +
                   "' has a slot with no id; skipping it.");
              continue;
            }
            if (slot.accepts.empty()) {
              slot.accepts = slot.id;
            }
            def.slots.push_back(std::move(slot));
          }
        }
      }

      if (def.slots.empty()) {
        warn("Warning: Race '" + def.id +
             "' declares no slots, so its characters can wear nothing.");
      }

      // The race's look, authored with the same "art"/"palette" keys an object
      // template uses. Unusable art is a warning and leaves the race's characters
      // on the code-level humanoid look (see ActorArt.hpp); it never fails a load,
      // exactly like a bad art block on an object.
      {
        AsciiArt art;
        std::string artWarning;
        if (ParseAsciiArt(j, art, artWarning)) {
          def.art = std::move(art);
        }
        if (!artWarning.empty()) {
          warn("Warning: Race '" + def.id + "': " + artWarning + ".");
        }
      }

      races[def.id] = std::move(def);
    } catch (const std::exception& e) {
      warn("Failed to parse race " + entry.path().string() + ": " +
           std::string(e.what()));
    }
  }
}

const RaceDef* RaceRegistry::Get(const std::string& id) const {
  auto it = races.find(id);
  if (it == races.end()) return nullptr;
  return &it->second;
}

std::vector<std::string> RaceRegistry::GetIds() const {
  std::vector<std::string> ids;
  ids.reserve(races.size());
  for (const auto& pair : races) {
    ids.push_back(pair.first);
  }
  std::sort(ids.begin(), ids.end());
  return ids;
}

std::vector<const RaceDef*> RaceRegistry::GetAll() const {
  std::vector<const RaceDef*> defs;
  defs.reserve(races.size());
  for (const std::string& id : GetIds()) {
    defs.push_back(Get(id));
  }
  return defs;
}
