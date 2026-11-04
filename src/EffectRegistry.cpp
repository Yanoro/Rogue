#include "EffectRegistry.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

#include "DebugLog.h"

namespace {
std::string Lower(std::string text) {
  for (char &c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
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
bool ParseKind(const std::string &text, EffectKind &out) {
  const std::string lower = Lower(text);
  if (lower == "damage") { out = EffectKind::Damage; return true; }
  if (lower == "heal") { out = EffectKind::Heal; return true; }
  if (lower == "apply_status") { out = EffectKind::ApplyStatus; return true; }
  if (lower == "remove_status") { out = EffectKind::RemoveStatus; return true; }
  if (lower == "create_surface") { out = EffectKind::CreateSurface; return true; }
  if (lower == "remove_surface") { out = EffectKind::RemoveSurface; return true; }
  return false;
}
const char *KindName(EffectKind kind) {
  switch (kind) {
    case EffectKind::Damage: return "damage";
    case EffectKind::Heal: return "heal";
    case EffectKind::ApplyStatus: return "apply_status";
    case EffectKind::RemoveStatus: return "remove_status";
    case EffectKind::CreateSurface: return "create_surface";
    case EffectKind::RemoveSurface: return "remove_surface";
  }
  return "damage";
}
}  // namespace

bool EffectDef::HasTag(const std::string &tag) const {
  return std::find(tags.begin(), tags.end(), tag) != tags.end();
}

std::string EffectRegistry::IdFor(const nlohmann::json &document) {
  return Slugify(document.value("name", std::string()));
}

void EffectRegistry::Load(const std::string &directoryPath) {
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
      if (debugLog) debugLog->LogError("Effects: " + entry.path().string() +
                                       " is not JSON: " + error.what());
      continue;
    }
    std::string error;
    std::vector<std::string> warnings;
    if (!AddOrReplace(document, entry.path().string(), &error, &warnings)) {
      if (debugLog) debugLog->LogError("Effects: " + entry.path().string() +
                                       " refused: " + error);
      continue;
    }
    if (debugLog) {
      for (const std::string &warning : warnings) {
        debugLog->LogWarning("Effects: " + warning);
      }
    }
  }
}

bool EffectRegistry::AddOrReplace(const nlohmann::json &document,
                                  const std::string &source, std::string *error,
                                  std::vector<std::string> *warnings) {
  (void)source;
  (void)warnings;
  if (!document.is_object()) {
    if (error) *error = "the entry is not a JSON object";
    return false;
  }
  EffectDef definition;
  definition.id = IdFor(document);
  if (definition.id.empty()) {
    if (error) *error = "the entry has no \"name\", so it has no id";
    return false;
  }
  definition.name = document.value("name", definition.id);
  definition.description = document.value("description", std::string());
  definition.base = document.value("base", 0.0f);
  definition.damageType = document.value("damageType", std::string());
  definition.statusId = document.value("status", std::string());
  definition.statusDuration = document.value("duration", 0.0f);
  definition.surfaceId = document.value("surface", std::string());
  definition.surfaceDuration = document.value("surfaceDuration", 0.0f);
  definition.surfaceRadius = document.value("surfaceRadius", 0);
  if (!ParseKind(document.value("kind", std::string("damage")), definition.kind)) {
    if (error) *error = "unknown kind '" + document.value("kind", std::string()) + "'";
    return false;
  }
  if (document.contains("tags") && document["tags"].is_array()) {
    for (const auto &tag : document["tags"]) {
      if (tag.is_string()) definition.tags.push_back(Lower(tag.get<std::string>()));
    }
  }
  switch (definition.kind) {
    case EffectKind::ApplyStatus:
      if (definition.statusId.empty()) {
        if (error) *error = "apply_status needs a \"status\" id";
        return false;
      }
      break;
    case EffectKind::RemoveStatus:
      if (definition.statusId.empty()) {
        if (error) *error = "remove_status needs a \"status\" id";
        return false;
      }
      break;
    case EffectKind::CreateSurface:
      if (definition.surfaceId.empty()) {
        if (error) *error = "create_surface needs a \"surface\" id";
        return false;
      }
      break;
    case EffectKind::RemoveSurface:
      break;
    case EffectKind::Damage:
    case EffectKind::Heal:
      break;
  }
  effects[definition.id] = std::move(definition);
  return true;
}

const EffectDef *EffectRegistry::Get(const std::string &id) const {
  auto it = effects.find(id);
  return it == effects.end() ? nullptr : &it->second;
}

std::vector<const EffectDef *> EffectRegistry::GetAll() const {
  std::vector<const EffectDef *> out;
  for (const auto &pair : effects) out.push_back(&pair.second);
  std::sort(out.begin(), out.end(), [](const EffectDef *a, const EffectDef *b) {
    return a->name < b->name;
  });
  return out;
}

nlohmann::json EffectRegistry::ToJson(const EffectDef &definition) const {
  nlohmann::json j = nlohmann::json::object();
  j["name"] = definition.name;
  if (!definition.description.empty()) j["description"] = definition.description;
  j["kind"] = KindName(definition.kind);
  j["base"] = definition.base;
  if (!definition.damageType.empty()) j["damageType"] = definition.damageType;
  if (!definition.statusId.empty()) j["status"] = definition.statusId;
  if (definition.statusDuration > 0.0f) j["duration"] = definition.statusDuration;
  if (!definition.surfaceId.empty()) j["surface"] = definition.surfaceId;
  if (definition.surfaceDuration > 0.0f) {
    j["surfaceDuration"] = definition.surfaceDuration;
  }
  if (definition.surfaceRadius > 0) j["surfaceRadius"] = definition.surfaceRadius;
  if (!definition.tags.empty()) j["tags"] = definition.tags;
  return j;
}
