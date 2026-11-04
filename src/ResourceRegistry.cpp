#include "ResourceRegistry.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

#include "DebugLog.h"

namespace {
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

std::string ResourceRegistry::IdFor(const nlohmann::json &document) {
  return Slugify(document.value("name", std::string()));
}

void ResourceRegistry::Load(const std::string &directoryPath) {
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
      if (debugLog) debugLog->LogError("Resources: " + entry.path().string() +
                                       " is not JSON: " + error.what());
      continue;
    }
    std::string error;
    std::vector<std::string> warnings;
    if (!AddOrReplace(document, entry.path().string(), &error, &warnings)) {
      if (debugLog) debugLog->LogError("Resources: " + entry.path().string() +
                                       " refused: " + error);
    }
  }
}

bool ResourceRegistry::AddOrReplace(const nlohmann::json &document,
                                    const std::string &source, std::string *error,
                                    std::vector<std::string> *warnings) {
  (void)source;
  (void)warnings;
  if (!document.is_object()) {
    if (error) *error = "the entry is not a JSON object";
    return false;
  }
  ResourceDef definition;
  definition.id = IdFor(document);
  if (definition.id.empty()) {
    if (error) *error = "the entry has no \"name\", so it has no id";
    return false;
  }
  definition.name = document.value("name", definition.id);
  definition.description = document.value("description", std::string());
  definition.icon = document.value("icon", std::string());
  definition.color = document.value("color", std::string("#cccccc"));
  definition.baseMax = document.value("baseMax", 100.0f);
  definition.canBeVital = document.value("canBeVital", true);
  definition.canBeSpendable = document.value("canBeSpendable", true);
  definition.shield = document.value("shield", false);
  definition.hidden = document.value("hidden", false);
  definition.displayOrder = document.value("displayOrder", 0);
  definition.regenPerSecond = document.value("regenPerSecond", 0.0f);
  if (definition.baseMax < 0.0f) definition.baseMax = 0.0f;
  resources[definition.id] = std::move(definition);
  return true;
}

const ResourceDef *ResourceRegistry::Get(const std::string &id) const {
  auto it = resources.find(id);
  return it == resources.end() ? nullptr : &it->second;
}

std::vector<const ResourceDef *> ResourceRegistry::GetAll() const {
  std::vector<const ResourceDef *> out;
  for (const auto &pair : resources) out.push_back(&pair.second);
  std::sort(out.begin(), out.end(), [](const ResourceDef *a, const ResourceDef *b) {
    if (a->displayOrder != b->displayOrder) return a->displayOrder < b->displayOrder;
    return a->name < b->name;
  });
  return out;
}

bool ParseResourceSpecs(const nlohmann::json &value,
                        std::vector<ResourceSpec> &out,
                        const std::string &owner,
                        std::vector<std::string> *warnings) {
  if (!value.is_array()) {
    if (warnings) warnings->push_back(owner + ": 'resources' is not an array");
    return false;
  }
  for (const auto &entry : value) {
    ResourceSpec spec;
    if (entry.is_string()) {
      spec.id = entry.get<std::string>();
    } else if (entry.is_object()) {
      spec.id = entry.value("id", std::string());
      spec.vital = entry.value("vital", false);
      spec.primary = entry.value("primary", false);
      spec.current = entry.value("current", -1.0f);
      spec.maxOverride = entry.value("max", -1.0f);
    }
    if (spec.id.empty()) {
      if (warnings) warnings->push_back(owner + ": resource entry has no id");
      continue;
    }
    out.push_back(std::move(spec));
  }
  return true;
}

nlohmann::json ResourceSpecsJson(const std::vector<ResourceSpec> &specs) {
  nlohmann::json out = nlohmann::json::array();
  for (const ResourceSpec &spec : specs) {
    if (!spec.vital && !spec.primary && spec.current < 0.0f &&
        spec.maxOverride < 0.0f) {
      out.push_back(spec.id);
    } else {
      nlohmann::json entry = {{"id", spec.id}};
      if (spec.vital) entry["vital"] = true;
      if (spec.primary) entry["primary"] = true;
      if (spec.current >= 0.0f) entry["current"] = spec.current;
      if (spec.maxOverride >= 0.0f) entry["max"] = spec.maxOverride;
      out.push_back(std::move(entry));
    }
  }
  return out;
}

nlohmann::json ResourceRegistry::ToJson(const ResourceDef &definition) const {
  nlohmann::json j = nlohmann::json::object();
  j["name"] = definition.name;
  if (!definition.description.empty()) j["description"] = definition.description;
  if (!definition.icon.empty()) j["icon"] = definition.icon;
  j["color"] = definition.color;
  j["baseMax"] = definition.baseMax;
  j["canBeVital"] = definition.canBeVital;
  j["canBeSpendable"] = definition.canBeSpendable;
  j["shield"] = definition.shield;
  j["hidden"] = definition.hidden;
  j["displayOrder"] = definition.displayOrder;
  if (definition.regenPerSecond > 0.0f) {
    j["regenPerSecond"] = definition.regenPerSecond;
  }
  return j;
}
