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

nlohmann::json ArtStyleRegistry::ToJson(const ArtStyleDef &definition) const {
  nlohmann::json document = nlohmann::json::object();
  document["id"] = definition.id;
  document["name"] = definition.name;
  if (!definition.description.empty()) {
    document["description"] = definition.description;
  }
  document["appliesTo"] = definition.appliesTo;
  document["positive"] = definition.positive;
  if (!definition.negative.empty()) document["negative"] = definition.negative;
  return document;
}
