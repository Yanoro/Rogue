#include "NpcTemplateRegistry.h"

#include "DebugLog.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

namespace {

std::string Slugify(const std::string& text) {
  std::string out;
  bool lastUnderscore = false;
  for (const char c : text) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (std::isalnum(u) != 0) {
      out.push_back(static_cast<char>(std::tolower(u)));
      lastUnderscore = false;
    } else if (!out.empty() && !lastUnderscore) {
      out.push_back('_');
      lastUnderscore = true;
    }
  }
  while (!out.empty() && out.back() == '_') out.pop_back();
  return out;
}

void Warn(const std::string& message, std::vector<std::string>* warnings) {
  if (warnings != nullptr) warnings->push_back(message);
}

// Slots use the same two spellings race files and maps use: a bare name whose kind
// is itself ("head"), or {id, accepts} when the slot takes something other than
// its own kind.
void ParseSlots(const nlohmann::json& json, const std::string& owner,
                std::vector<SlotSpec>& out, std::vector<std::string>* warnings) {
  if (!json.is_array()) {
    Warn("Template '" + owner + "' has a non-array \"slots\"; ignoring it",
         warnings);
    return;
  }
  for (const auto& entry : json) {
    SlotSpec slot;
    if (entry.is_string()) {
      slot.id = entry.get<std::string>();
      slot.accepts = slot.id;
    } else if (entry.is_object()) {
      slot.id = entry.value("id", std::string());
      slot.accepts = entry.value("accepts", slot.id);
    } else {
      Warn("Template '" + owner + "' has a slot that is neither a string nor an "
           "object; skipping it", warnings);
      continue;
    }
    if (slot.id.empty()) {
      Warn("Template '" + owner + "' has a slot with no id; skipping it",
           warnings);
      continue;
    }
    out.push_back(std::move(slot));
  }
}

void ParseStats(const nlohmann::json& json, const std::string& owner,
                std::vector<StatValue>& out, std::vector<std::string>* warnings) {
  if (!json.is_object()) {
    Warn("Template '" + owner + "' has a non-object \"stats\"; ignoring it",
         warnings);
    return;
  }
  for (auto it = json.begin(); it != json.end(); ++it) {
    if (!it.value().is_number()) {
      Warn("Template '" + owner + "' stat '" + it.key() +
           "' is not a number; skipping it", warnings);
      continue;
    }
    out.push_back({it.key(), it.value().get<float>()});
  }
}

bool ParseTemplate(const nlohmann::json& document, NpcTemplateRegistry* /*registry*/,
                   NpcTemplate& out, std::string* error,
                   std::vector<std::string>* warnings) {
  if (!document.is_object()) {
    if (error != nullptr) *error = "the entry is not a JSON object";
    return false;
  }
  out.name = document.value("name", std::string());
  out.id = Slugify(out.name);
  if (out.id.empty()) {
    if (error != nullptr) *error = "the entry has no \"name\", so it has no id";
    return false;
  }
  out.description = document.value("description", std::string());
  out.sprite = document.value("sprite", std::string());
  out.portrait = document.value("portrait", std::string());
  out.race = document.value("race", std::string());
  out.background = document.value("background", std::string());
  out.brain = document.value("brain", std::string());
  out.disposition = document.value("disposition", std::string("neutral"));
  for (char &c : out.disposition) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  if (out.disposition != "neutral" && out.disposition != "aggressive") {
    Warn("Template '" + out.name + "' has disposition '" + out.disposition +
         "'; using neutral", warnings);
    out.disposition = "neutral";
  }
  out.retaliates = document.value("retaliates", false);
  out.attackDamage = document.value("attackDamage", 8.0f);
  out.attackInterval = document.value("attackInterval", 6.0f);
  out.combatStrategy = document.value("combatStrategy", std::string());
  for (char &c : out.combatStrategy) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  if (out.attackDamage <= 0.0f) {
    Warn("Template '" + out.name + "' has non-positive attackDamage; using 8",
         warnings);
    out.attackDamage = 8.0f;
  }
  if (out.attackInterval <= 0.0f) {
    Warn("Template '" + out.name + "' has non-positive attackInterval; using 6",
         warnings);
    out.attackInterval = 6.0f;
  }

  if (document.contains("slots")) {
    ParseSlots(document["slots"], out.name, out.slots, warnings);
  }
  if (document.contains("inventory")) {
    if (!document["inventory"].is_array()) {
      Warn("Template '" + out.name +
           "' has a non-array \"inventory\"; ignoring it", warnings);
    } else {
      for (const auto& entry : document["inventory"]) {
        ItemStack stack;
        stack.item = entry.value("item", std::string());
        stack.count = entry.value("count", 1);
        if (stack.item.empty() || stack.count <= 0) {
          Warn("Template '" + out.name + "' has an invalid inventory entry; "
               "skipping it", warnings);
          continue;
        }
        out.inventory.push_back(stack);
      }
    }
  }
  if (document.contains("skills")) {
    const std::function<void(const std::string&)> warn =
        [warnings](const std::string& message) { Warn(message, warnings); };
    ParseStartingSkills(document["skills"], "template '" + out.name + "'",
                        out.startingSkills, warn);
  }
  if (document.contains("stats")) {
    ParseStats(document["stats"], out.name, out.startingStats, warnings);
  }
  if (document.contains("equipped")) {
    if (!document["equipped"].is_array()) {
      Warn("Template '" + out.name +
           "' has a non-array \"equipped\"; ignoring it", warnings);
    } else {
      for (const auto& entry : document["equipped"]) {
        if (!entry.is_string()) {
          Warn("Template '" + out.name +
               "' has a non-string \"equipped\" entry; skipping it", warnings);
          continue;
        }
        out.equipped.push_back(entry.get<std::string>());
      }
    }
  }
  return true;
}

}  // namespace

std::string NpcTemplateRegistry::IdFor(const nlohmann::json& document) {
  return Slugify(document.value("name", std::string()));
}

void NpcTemplateRegistry::Load(const std::string& directoryPath) {
  directory = directoryPath;
  if (directoryPath.empty()) return;
  std::error_code ec;
  if (!std::filesystem::is_directory(directoryPath, ec)) return;  // normal

  std::vector<std::filesystem::path> files;
  for (const auto& entry : std::filesystem::directory_iterator(directoryPath, ec)) {
    if (entry.path().extension() == ".json") files.push_back(entry.path());
  }
  std::sort(files.begin(), files.end());

  for (const std::filesystem::path& path : files) {
    std::ifstream in(path);
    if (!in.is_open()) continue;
    nlohmann::json document;
    try {
      in >> document;
    } catch (const std::exception& parseError) {
      if (debugLog) {
        debugLog->LogError("NPC templates: " + path.string() + " is not JSON: " +
                           parseError.what());
      }
      continue;
    }
    std::string error;
    std::vector<std::string> warnings;
    if (!AddOrReplace(document, path.string(), &error, &warnings) && debugLog) {
      debugLog->LogError("NPC templates: " + path.string() + " refused: " + error);
    }
  }
  if (debugLog) {
    debugLog->LogInfo("NPC templates: " + std::to_string(templates.size()) +
                      " template(s) from " + directoryPath);
  }
}

bool NpcTemplateRegistry::AddOrReplace(const nlohmann::json& document,
                                       const std::string& source,
                                       std::string* error,
                                       std::vector<std::string>* warnings) {
  (void)source;
  NpcTemplate parsed;
  if (!ParseTemplate(document, this, parsed, error, warnings)) {
    return false;
  }
  templates[parsed.id] = std::move(parsed);
  return true;
}

const NpcTemplate* NpcTemplateRegistry::Get(const std::string& id) const {
  const auto found = templates.find(id);
  return found == templates.end() ? nullptr : &found->second;
}

std::vector<const NpcTemplate*> NpcTemplateRegistry::GetAll() const {
  std::vector<const NpcTemplate*> out;
  out.reserve(templates.size());
  for (const auto& [id, template_] : templates) {
    (void)id;
    out.push_back(&template_);
  }
  return out;
}

nlohmann::json NpcTemplateRegistry::ToJson(const NpcTemplate& definition) const {
  nlohmann::json document = nlohmann::json::object();
  document["name"] = definition.name;
  if (!definition.description.empty()) document["description"] = definition.description;
  document["sprite"] = definition.sprite;
  if (!definition.portrait.empty()) document["portrait"] = definition.portrait;
  if (!definition.race.empty()) document["race"] = definition.race;
  if (!definition.background.empty()) document["background"] = definition.background;
  if (!definition.brain.empty()) document["brain"] = definition.brain;
  document["disposition"] = definition.disposition;
  document["retaliates"] = definition.retaliates;
  document["attackDamage"] = definition.attackDamage;
  document["attackInterval"] = definition.attackInterval;
  document["combatStrategy"] = definition.combatStrategy;
  if (!definition.slots.empty()) {
    nlohmann::json slots = nlohmann::json::array();
    for (const SlotSpec& slot : definition.slots) {
      if (slot.id == slot.accepts) {
        slots.push_back(slot.id);
      } else {
        slots.push_back({{"id", slot.id}, {"accepts", slot.accepts}});
      }
    }
    document["slots"] = std::move(slots);
  }
  if (!definition.inventory.empty()) {
    nlohmann::json inventory = nlohmann::json::array();
    for (const ItemStack& stack : definition.inventory) {
      inventory.push_back({{"item", stack.item}, {"count", stack.count}});
    }
    document["inventory"] = std::move(inventory);
  }
  if (!definition.startingSkills.empty()) {
    nlohmann::json skills = nlohmann::json::object();
    for (const StartingSkill& skill : definition.startingSkills) {
      nlohmann::json entry = nlohmann::json::object();
      if (!skill.stage.empty()) entry["stage"] = skill.stage;
      entry["level"] = skill.levelInStage;
      skills[skill.id] = std::move(entry);
    }
    document["skills"] = std::move(skills);
  }
  if (!definition.startingStats.empty()) {
    nlohmann::json stats = nlohmann::json::object();
    for (const StatValue& stat : definition.startingStats) {
      stats[stat.id] = stat.value;
    }
    document["stats"] = std::move(stats);
  }
  if (!definition.equipped.empty()) document["equipped"] = definition.equipped;
  return document;
}
