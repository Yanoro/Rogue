#include "StatusEffectRegistry.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

#include "DebugLog.h"

namespace {
std::string Lower(std::string text) {
  for (char &c : text) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return text;
}
std::string Slugify(const std::string &text) {
  std::string out;
  bool separator = false;
  for (const char c : text) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (std::isalnum(u)) {
      out.push_back(static_cast<char>(std::tolower(u)));
      separator = false;
    } else if (!out.empty() && !separator) {
      out.push_back('_');
      separator = true;
    }
  }
  while (!out.empty() && out.back() == '_') out.pop_back();
  return out;
}
}  // namespace

bool StatusEffectDef::HasTag(const std::string &tag) const {
  return std::find(tags.begin(), tags.end(), tag) != tags.end();
}

std::string StatusEffectRegistry::IdFor(const nlohmann::json &document) {
  return Slugify(document.value("name", std::string()));
}

void StatusEffectRegistry::Load(const std::string &directoryPath) {
  if (directoryPath.empty()) return;
  std::error_code ec;
  if (!std::filesystem::is_directory(directoryPath, ec)) return;
  for (const auto &entry : std::filesystem::directory_iterator(directoryPath, ec)) {
    if (entry.path().extension() != ".json") continue;
    std::ifstream in(entry.path());
    if (!in.is_open()) continue;
    nlohmann::json document;
    try {
      in >> document;
    } catch (const std::exception &error) {
      if (debugLog) {
        debugLog->LogError("Status effects: " + entry.path().string() +
                           " is not JSON: " + error.what());
      }
      continue;
    }
    std::string error;
    std::vector<std::string> warnings;
    if (!AddOrReplace(document, entry.path().string(), &error, &warnings)) {
      if (debugLog) {
        debugLog->LogError("Status effects: " + entry.path().string() +
                           " refused: " + error);
      }
    }
  }
}

bool StatusEffectRegistry::AddOrReplace(const nlohmann::json &document,
                                        const std::string &source,
                                        std::string *error,
                                        std::vector<std::string> *warnings) {
  (void)source;
  (void)warnings;
  if (!document.is_object()) {
    if (error) *error = "the entry is not a JSON object";
    return false;
  }
  StatusEffectDef definition;
  definition.id = IdFor(document);
  if (definition.id.empty()) {
    if (error) *error = "the entry has no \"name\", so it has no id";
    return false;
  }
  definition.name = document.value("name", definition.id);
  definition.description = document.value("description", std::string());
  definition.icon = document.value("icon", std::string());
  definition.durationSeconds = document.value("duration", 5.0f);
  definition.tickSeconds = document.value("tickSeconds", 1.0f);
  definition.stacking = document.value("stacking", "refresh");
  definition.maxStacks = std::max(1, document.value("maxStacks", 1));
  if (definition.tickSeconds <= 0.0f) definition.tickSeconds = 1.0f;
  const auto readEffects = [&](const char *key, std::vector<std::string> &out) {
    if (!document.contains(key)) return;
    const auto &value = document[key];
    if (value.is_array()) {
      for (const auto &entry : value) {
        if (entry.is_string()) out.push_back(entry.get<std::string>());
        else if (entry.is_object()) {
          const std::string id = entry.value("effect", std::string());
          if (!id.empty()) out.push_back(id);
        }
      }
    } else if (value.is_object() && value.contains("effects") &&
               value["effects"].is_array()) {
      for (const auto &entry : value["effects"]) {
        if (entry.is_string()) out.push_back(entry.get<std::string>());
        else if (entry.is_object()) {
          const std::string id = entry.value("effect", std::string());
          if (!id.empty()) out.push_back(id);
        }
      }
    }
  };
  readEffects("tick", definition.tickEffects);
  readEffects("onApply", definition.onApplyEffects);
  readEffects("onExpire", definition.onExpireEffects);
  if (document.contains("tags") && document["tags"].is_array()) {
    for (const auto &tag : document["tags"]) {
      if (tag.is_string()) definition.tags.push_back(Lower(tag.get<std::string>()));
    }
  }
  effects[definition.id] = std::move(definition);
  return true;
}

const StatusEffectDef *StatusEffectRegistry::Get(const std::string &id) const {
  auto it = effects.find(id);
  return it == effects.end() ? nullptr : &it->second;
}

std::vector<const StatusEffectDef *> StatusEffectRegistry::GetAll() const {
  std::vector<const StatusEffectDef *> out;
  for (const auto &pair : effects) out.push_back(&pair.second);
  std::sort(out.begin(), out.end(), [](const StatusEffectDef *a,
                                       const StatusEffectDef *b) {
    return a->name < b->name;
  });
  return out;
}

nlohmann::json StatusEffectRegistry::ToJson(
    const StatusEffectDef &definition) const {
  nlohmann::json j = nlohmann::json::object();
  j["name"] = definition.name;
  if (!definition.description.empty()) j["description"] = definition.description;
  j["tags"] = definition.tags;
  if (!definition.icon.empty()) j["icon"] = definition.icon;
  j["duration"] = definition.durationSeconds;
  j["tickSeconds"] = definition.tickSeconds;
  j["stacking"] = definition.stacking;
  j["maxStacks"] = definition.maxStacks;
  j["onApply"] = definition.onApplyEffects;
  j["onExpire"] = definition.onExpireEffects;
  j["tick"] = definition.tickEffects;
  return j;
}
