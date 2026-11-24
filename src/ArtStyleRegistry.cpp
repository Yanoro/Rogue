#include "ArtStyleRegistry.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

#include "DebugLog.h"

namespace {
std::string Slugify(const std::string &text) {
  std::string out;
  bool lastWasSeparator = false;
  for (const char c : text) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (std::isalnum(u)) {
      out.push_back(static_cast<char>(std::tolower(u)));
      lastWasSeparator = false;
    } else if (!out.empty() && !lastWasSeparator) {
      out.push_back('_');
      lastWasSeparator = true;
    }
  }
  while (!out.empty() && out.back() == '_') out.pop_back();
  return out;
}
}

std::string ArtStyleRegistry::IdFor(const nlohmann::json &document) {
  if (document.contains("id") && document["id"].is_string()) {
    const std::string id = document["id"].get<std::string>();
    if (!id.empty()) return Slugify(id);
  }
  return Slugify(document.value("name", std::string()));
}

void ArtStyleRegistry::Load(const std::string &directoryPath) {
  if (directoryPath.empty()) return;
  std::error_code ec;
  if (!std::filesystem::is_directory(directoryPath, ec)) return;

  for (const auto &entry : std::filesystem::directory_iterator(directoryPath, ec)) {
    if (entry.path().extension() != ".json") continue;
    std::ifstream file(entry.path());
    if (!file.is_open()) continue;
    nlohmann::json document;
    try {
      file >> document;
    } catch (const std::exception &error) {
      if (debugLog) {
        debugLog->LogError("Art styles: " + entry.path().string() +
                           " is not JSON: " + error.what());
      }
      continue;
    }
    std::string error;
    std::vector<std::string> warnings;
    if (!AddOrReplace(document, entry.path().string(), &error, &warnings)) {
      if (debugLog) {
        debugLog->LogError("Art styles: " + entry.path().string() +
                           " refused: " + error);
      }
      continue;
    }
    if (debugLog) {
      for (const std::string &warning : warnings) {
        debugLog->LogWarning("Art styles: " + warning);
      }
    }
  }
}

bool ArtStyleRegistry::AddOrReplace(const nlohmann::json &document,
                                    const std::string &source,
                                    std::string *error,
                                    std::vector<std::string> *warnings) {
  (void)source;
  if (!document.is_object()) {
    if (error != nullptr) *error = "the entry is not a JSON object";
    return false;
  }
  ArtStyleDef definition;
  definition.id = IdFor(document);
  if (definition.id.empty()) {
    if (error != nullptr) *error = "the entry has no id or name";
    return false;
  }
  definition.name = document.value("name", definition.id);
  definition.description = document.value("description", std::string());
  definition.positive = document.value("positive", std::string());
  definition.negative = document.value("negative", std::string());
  definition.palette = document.value("palette", std::string());
  definition.ramps = document.value("ramps", std::string());
  definition.reference = document.value("reference", std::string());
  definition.isDefault = document.value("default", false);
  if (document.contains("geometry")) {
    definition.geometry = RigGeometryFromJson(document["geometry"]);
  }
  if (document.contains("sections")) {
    if (!document["sections"].is_object()) {
      if (warnings != nullptr) {
        warnings->push_back("style '" + definition.id +
                            "' has a non-object 'sections'; ignoring it");
      }
    } else {
      for (const auto &[section, value] : document["sections"].items()) {
        if (!value.is_object()) {
          if (warnings != nullptr) {
            warnings->push_back("style '" + definition.id + "' section '" + section +
                                "' is not an object; ignoring it");
          }
          continue;
        }
        ArtStyleSection roleDefinition;
        roleDefinition.positive = value.value("positive", std::string());
        roleDefinition.negative = value.value("negative", std::string());
        roleDefinition.palette = value.value("palette", std::string());
        roleDefinition.reference = value.value("reference", std::string());
        roleDefinition.width = value.value("width", 0);
        roleDefinition.height = value.value("height", 0);
        if (value.contains("geometry")) {
          roleDefinition.geometry = RigGeometryFromJson(value["geometry"]);
        }
        definition.sections[section] = std::move(roleDefinition);
      }
    }
  }
  if (definition.positive.empty()) {
    if (warnings != nullptr) {
      warnings->push_back("style '" + definition.id +
                          "' has no 'positive' prose; it will not affect prompts");
    }
  }
  if (document.contains("appliesTo")) {
    if (!document["appliesTo"].is_array()) {
      if (warnings != nullptr) {
        warnings->push_back("style '" + definition.id +
                            "' has a non-array 'appliesTo'; ignoring it");
      }
    } else {
      for (const auto &family : document["appliesTo"]) {
        if (family.is_string() && !family.get<std::string>().empty()) {
          definition.appliesTo.push_back(family.get<std::string>());
        }
      }
    }
  }
  auto existing = styles.find(definition.id);
  if (existing == styles.end()) order.push_back(definition.id);
  styles[definition.id] = std::move(definition);
  return true;
}

const ArtStyleDef *ArtStyleRegistry::Get(const std::string &id) const {
  auto found = styles.find(id);
  return found == styles.end() ? nullptr : &found->second;
}

std::vector<const ArtStyleDef *> ArtStyleRegistry::GetAll() const {
  std::vector<const ArtStyleDef *> out;
  out.reserve(order.size());
  for (const std::string &id : order) {
    auto found = styles.find(id);
    if (found != styles.end()) out.push_back(&found->second);
  }
  return out;
}

const ArtStyleDef *ArtStyleRegistry::DefaultFor(const std::string &family) const {
  for (const std::string &id : order) {
    auto found = styles.find(id);
    if (found == styles.end()) continue;
    if (std::find(found->second.appliesTo.begin(),
                  found->second.appliesTo.end(),
                  family) != found->second.appliesTo.end()) {
      return &found->second;
    }
  }
  return nullptr;
}

ResolvedArtStyle ArtStyleRegistry::ResolveSection(const ArtStyleDef &style,
                                              const std::string &section) const {
  ResolvedArtStyle resolved;
  resolved.positive = style.positive;
  resolved.negative = style.negative;
  resolved.palette = style.palette;
  resolved.reference = style.reference;
  resolved.geometry = style.geometry;
  auto found = style.sections.find(section);
  if (found != style.sections.end()) {
    const ArtStyleSection &value = found->second;
    if (!value.positive.empty()) resolved.positive = value.positive;
    if (!value.negative.empty()) resolved.negative = value.negative;
    if (!value.palette.empty()) resolved.palette = value.palette;
    if (!value.reference.empty()) resolved.reference = value.reference;
    if (value.geometry) resolved.geometry = value.geometry;
    if (value.width > 0) resolved.width = value.width;
    if (value.height > 0) resolved.height = value.height;
  }
  return resolved;
}

std::vector<std::string> ArtStyleRegistry::KnownSections() {
  return {"inventory_icon", "resource_icon", "skill_icon", "status_icon",
          "portrait",       "actor",         "rig",        "object",
          "surface",        "effect",        "silhouette", "tile"};
}

const ArtStyleDef *ArtStyleRegistry::DefaultTheme() const {
  for (const std::string &id : order) {
    auto found = styles.find(id);
    if (found != styles.end() && found->second.isDefault) return &found->second;
  }
  if (order.empty()) return nullptr;
  auto found = styles.find(order.front());
  return found == styles.end() ? nullptr : &found->second;
}

std::string ArtStyleRegistry::DefaultThemeId() const {
  const ArtStyleDef *theme = DefaultTheme();
  return theme == nullptr ? std::string() : theme->id;
}

bool ArtStyleRegistry::SetDefaultTheme(const std::string &id) {
  auto selected = styles.find(id);
  if (selected == styles.end()) return false;
  for (auto &[themeId, theme] : styles) {
    theme.isDefault = themeId == id;
  }
  return true;
}

RigGeometry ArtStyleRegistry::GeometryForStyle(const std::string &styleId) const {
  const ArtStyleDef *style = Get(styleId);
  if (style != nullptr && style->geometry) return *style->geometry;
  return RigGeometry{};
}

RigGeometry ArtStyleRegistry::GeometryFor(const ActorRig &rig) const {
  if (rig.geometry) return *rig.geometry;
  const ArtStyleDef *style = Get(rig.style);
  if (style != nullptr && style->geometry) return *style->geometry;
  return RigGeometry{};
}

nlohmann::json ArtStyleRegistry::ToJson(const ArtStyleDef &definition) const {
  nlohmann::json document = nlohmann::json::object();
  document["id"] = definition.id;
  document["name"] = definition.name;
  if (definition.geometry) document["geometry"] = RigGeometryToJson(*definition.geometry);
  if (!definition.description.empty()) {
    document["description"] = definition.description;
  }
  document["appliesTo"] = definition.appliesTo;
  document["positive"] = definition.positive;
  if (!definition.negative.empty()) document["negative"] = definition.negative;
  if (!definition.palette.empty()) document["palette"] = definition.palette;
  if (!definition.ramps.empty()) document["ramps"] = definition.ramps;
  if (!definition.reference.empty()) document["reference"] = definition.reference;
  if (definition.isDefault) document["default"] = true;
  if (!definition.sections.empty()) {
    nlohmann::json sections = nlohmann::json::object();
    for (const auto &[section, value] : definition.sections) {
      nlohmann::json entry = nlohmann::json::object();
      if (!value.positive.empty()) entry["positive"] = value.positive;
      if (!value.negative.empty()) entry["negative"] = value.negative;
      if (!value.palette.empty()) entry["palette"] = value.palette;
      if (!value.reference.empty()) entry["reference"] = value.reference;
      if (value.geometry) entry["geometry"] = RigGeometryToJson(*value.geometry);
      if (value.width > 0) entry["width"] = value.width;
      if (value.height > 0) entry["height"] = value.height;
      sections[section] = std::move(entry);
    }
    document["sections"] = std::move(sections);
  }
  return document;
}
