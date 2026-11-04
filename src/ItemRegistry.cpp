#include "ItemRegistry.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>

#include "DebugLog.h"

bool ItemRegistry::ParseAndStore(const nlohmann::json& j, const std::string& id,
                                 std::string* error,
                                 std::vector<std::string>* warnings) {
  if (!j.is_object()) {
    if (error != nullptr) *error = "the document is not a JSON object";
    return false;
  }
  const auto warn = [this, warnings](const std::string& message) {
    if (warnings != nullptr) {
      warnings->push_back(message);
    } else if (debugLog) {
      debugLog->LogWarning(message);
    } else {
      std::cerr << message << std::endl;
    }
  };

  ItemDef def;
  def.id = id;
  def.name = j.value("name", "");
  def.slot = j.value("slot", "");
  def.occupies = j.value("occupies", 1);
  def.style = j.value("style", std::string());
  def.stackable = j.value("stackable", false);
  def.maxStack = std::max(1, j.value("maxStack", 1));
  def.tier = j.value("tier", 1.0f);
  def.scale = j.value("scale", 1.0f);
  def.baseline = j.value("baseline", 0.0f);
  // A weapon's own numbers. Damage 0 is the default and simply means "not a
  // weapon": the basic attack then falls back to unarmed damage. Swing time
  // is the unhasted tempo; the duration fold applies haste on top.
  def.damage = j.value("damage", 0.0f);
  def.swingTime = j.value("swingTime", 1.8f);
  if (def.damage < 0.0f) {
    warn("Warning: Item '" + def.id +
         "' has negative damage; using 0 (not a weapon).");
    def.damage = 0.0f;
  }
  if (def.swingTime <= 0.0f) {
    warn("Warning: Item '" + def.id +
         "' has a swingTime <= 0, which would let it swing without a "
         "cooldown; using 1.8 instead.");
    def.swingTime = 1.8f;
  }
  def.armor = j.value("armor", 0.0f);
  if (def.armor < 0.0f) {
    warn("Warning: Item '" + def.id +
         "' has negative armor; using 0 (no protection).");
    def.armor = 0.0f;
  }
  // Worn art, for rigged actors. Absent for most items and absent for every item
  // until a worn sprite exists, so this is optional rather than required: an item
  // still equips, it simply draws nothing on the rig yet.
  if (j.contains("worn")) {
    if (!j["worn"].is_object()) {
      warn("Warning: Item '" + def.id +
           "' has a non-object 'worn'; ignoring it.");
    } else {
      const nlohmann::json& worn = j["worn"];
      def.wornSprite = worn.value("sprite", std::string());
      def.wornBrief = worn.value("brief", std::string());
      def.wornZ = worn.value("z", 0);
      if (worn.contains("hides") && worn["hides"].is_array()) {
        for (const auto& hide : worn["hides"]) {
          if (hide.is_string()) {
            def.wornHides.push_back(hide.get<std::string>());
          }
        }
      }
      if (def.wornSprite.empty()) {
        warn("Warning: Item '" + def.id +
             "' has a 'worn' block with no sprite; ignoring it.");
      }
    }
  }
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
  // A replace must not leave the previous definition's links behind: they are keyed by
  // id, and an item that lost an effect would otherwise keep firing it.
  declared.links.erase(def.id);
  declared.hooks.erase(def.id);

  if (j.contains("effects")) {
    def.effectsJson = j["effects"];
  }
  items[def.id] = def;

  if (j.contains("effects")) {
    ParseEffectEntries(j["effects"], def.id, declared, warn);
  }
  return true;
}

void ItemRegistry::LoadItems(const std::string& directoryPath) {
  if (!std::filesystem::exists(directoryPath)) {
    const std::string errStr =
        "Warning: Item directory not found: " + directoryPath + "\n";
    if (debugLog) debugLog->LogError(errStr);
    else std::cerr << errStr;
    return;
  }

  for (const auto& entry : std::filesystem::directory_iterator(directoryPath)) {
    if (entry.path().extension() != ".json") continue;

    std::ifstream file(entry.path());
    if (!file.is_open()) continue;

    nlohmann::json j;
    try {
      file >> j;
    } catch (const std::exception& e) {
      const std::string message = "Failed to parse item " + entry.path().string() +
                                  ": " + std::string(e.what());
      if (debugLog) debugLog->LogWarning(message);
      else std::cerr << message << std::endl;
      continue;
    }

    std::string error;
    std::vector<std::string> warnings;
    if (!ParseAndStore(j, entry.path().stem().string(), &error, &warnings)) {
      const std::string message = "Item " + entry.path().string() +
                                  " was refused: " + error;
      if (debugLog) debugLog->LogWarning(message);
      else std::cerr << message << std::endl;
      continue;
    }
    for (const std::string& warning : warnings) {
      if (debugLog) debugLog->LogWarning(warning);
      else std::cerr << warning << std::endl;
    }
  }
}

bool ItemRegistry::AddOrReplace(const nlohmann::json& document,
                                const std::string& source, std::string* error,
                                std::vector<std::string>* warnings) {
  std::string id = document.is_object() ? document.value("id", std::string())
                                        : std::string();
  if (id.empty() && !source.empty()) {
    const std::filesystem::path sourcePath(source);
    if (sourcePath.extension() == ".json") {
      id = sourcePath.stem().string();
    }
  }
  if (id.empty()) {
    if (error != nullptr) {
      *error = "the document has no \"id\" and no file name to take one from";
    }
    return false;
  }
  return ParseAndStore(document, id, error, warnings);
}

nlohmann::json ItemRegistry::ToJson(const ItemDef& definition) const {
  nlohmann::json document = nlohmann::json::object();
  if (!definition.name.empty()) document["name"] = definition.name;
  document["slot"] = definition.slot;
  // Always present so the Content Editor can turn stacking on without adding
  // a raw key by hand.
  document["stackable"] = definition.stackable;
  document["maxStack"] = definition.maxStack;
  document["style"] = definition.style;
  if (definition.occupies != 1) document["occupies"] = definition.occupies;
  document["tier"] = definition.tier;
  if (definition.scale != 1.0f) document["scale"] = definition.scale;
  if (definition.baseline != 0.0f) document["baseline"] = definition.baseline;
  if (!definition.tags.empty()) document["tags"] = definition.tags;
  if (definition.durability > 0) document["durability"] = definition.durability;
  if (!definition.consumedBy.empty()) document["consumedBy"] = definition.consumedBy;
  if (definition.damage > 0.0f) {
    document["damage"] = definition.damage;
    document["swingTime"] = definition.swingTime;
  }
  if (definition.armor > 0.0f) document["armor"] = definition.armor;
  if (!definition.wornSprite.empty()) {
    nlohmann::json worn = {{"sprite", definition.wornSprite},
                           {"z", definition.wornZ}};
    if (!definition.wornBrief.empty()) worn["brief"] = definition.wornBrief;
    if (!definition.wornHides.empty()) worn["hides"] = definition.wornHides;
    document["worn"] = std::move(worn);
  }
  if (!definition.effectsJson.is_null()) {
    document["effects"] = definition.effectsJson;
  }
  return document;
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
