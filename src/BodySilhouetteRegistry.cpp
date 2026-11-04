#include "BodySilhouetteRegistry.h"

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

float Clamp01(float value) { return std::clamp(value, 0.0f, 1.0f); }
}

std::string BodySilhouetteRegistry::IdFor(const nlohmann::json &document) {
  if (document.contains("id") && document["id"].is_string()) {
    const std::string id = document["id"].get<std::string>();
    if (!id.empty()) return Slugify(id);
  }
  return Slugify(document.value("name", std::string()));
}

void BodySilhouetteRegistry::Load(const std::string &directoryPath) {
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
        debugLog->LogError("Body silhouettes: " + entry.path().string() +
                           " is not JSON: " + error.what());
      }
      continue;
    }
    std::string error;
    std::vector<std::string> warnings;
    if (!AddOrReplace(document, entry.path().string(), &error, &warnings)) {
      if (debugLog) {
        debugLog->LogError("Body silhouettes: " + entry.path().string() +
                           " refused: " + error);
      }
      continue;
    }
    if (debugLog) {
      for (const std::string &warning : warnings) {
        debugLog->LogWarning("Body silhouettes: " + warning);
      }
    }
  }
}

bool BodySilhouetteRegistry::AddOrReplace(const nlohmann::json &document,
                                          const std::string &source,
                                          std::string *error,
                                          std::vector<std::string> *warnings) {
  (void)source;
  if (!document.is_object()) {
    if (error != nullptr) *error = "the entry is not a JSON object";
    return false;
  }
  BodySilhouetteDef definition;
  definition.id = IdFor(document);
  if (definition.id.empty()) {
    if (error != nullptr) *error = "the entry has no id or name";
    return false;
  }
  definition.name = document.value("name", definition.id);
  definition.description = document.value("description", std::string());
  definition.icon = document.value(
      "icon", document.value("silhouette", definition.id));
  if (definition.icon.empty()) definition.icon = definition.id;

  if (!document.contains("anchors")) {
    if (warnings != nullptr) {
      warnings->push_back("silhouette '" + definition.id +
                          "' has no anchors; the inventory will use its fallback layout");
    }
  } else if (!document["anchors"].is_array()) {
    if (warnings != nullptr) {
      warnings->push_back("silhouette '" + definition.id +
                          "' has a non-array 'anchors'; ignoring it");
    }
  } else {
    for (const auto &anchorJson : document["anchors"]) {
      if (!anchorJson.is_object()) continue;
      BodySlotAnchor anchor;
      anchor.slot = anchorJson.value("slot", std::string());
      if (anchor.slot.empty()) continue;
      anchor.x = Clamp01(anchorJson.value("x", 0.5f));
      anchor.y = Clamp01(anchorJson.value("y", 0.5f));
      anchor.size = std::clamp(anchorJson.value("size", 0.15f), 0.03f, 0.5f);
      definition.anchors.push_back(std::move(anchor));
    }
  }

  auto existing = silhouettes.find(definition.id);
  if (existing == silhouettes.end()) order.push_back(definition.id);
  silhouettes[definition.id] = std::move(definition);
  return true;
}

const BodySilhouetteDef *BodySilhouetteRegistry::Get(
    const std::string &id) const {
  auto found = silhouettes.find(id);
  return found == silhouettes.end() ? nullptr : &found->second;
}

std::vector<const BodySilhouetteDef *> BodySilhouetteRegistry::GetAll() const {
  std::vector<const BodySilhouetteDef *> out;
  out.reserve(order.size());
  for (const std::string &id : order) {
    auto found = silhouettes.find(id);
    if (found != silhouettes.end()) out.push_back(&found->second);
  }
  return out;
}

nlohmann::json BodySilhouetteRegistry::ToJson(
    const BodySilhouetteDef &definition) const {
  nlohmann::json document = nlohmann::json::object();
  document["id"] = definition.id;
  document["name"] = definition.name;
  if (!definition.description.empty()) {
    document["description"] = definition.description;
  }
  document["icon"] = definition.icon;
  nlohmann::json anchors = nlohmann::json::array();
  for (const BodySlotAnchor &anchor : definition.anchors) {
    anchors.push_back({{"slot", anchor.slot},
                       {"x", anchor.x},
                       {"y", anchor.y},
                       {"size", anchor.size}});
  }
  document["anchors"] = std::move(anchors);
  return document;
}
