#include "PortraitRegistry.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>

#include <nlohmann/json.hpp>

#include "DebugLog.h"

void PortraitRegistry::LoadPortraits(const std::string &directoryPath) {
  if (!std::filesystem::exists(directoryPath)) {
    const std::string errStr =
        "Warning: Portrait directory not found: " + directoryPath + "\n";
    if (debugLog) debugLog->LogError(errStr);
    else std::cerr << errStr;
    return;
  }

  auto warn = [this](const std::string &message) {
    if (debugLog) debugLog->LogWarning(message);
    else std::cerr << message << std::endl;
  };

  for (const auto &entry : std::filesystem::directory_iterator(directoryPath)) {
    if (entry.path().extension() != ".json") continue;

    std::ifstream file(entry.path());
    if (!file.is_open()) continue;

    try {
      nlohmann::json j;
      file >> j;

      PortraitDef def;
      def.id = entry.path().stem().string();
      def.sprite = j.value("sprite", "");
      def.source = j.value("source", "auto");

      if (def.sprite.empty() && def.source != "generated") {
        // A portrait with no image is the fallback's job, not this file's: an
        // entry that resolves to nothing would shadow the unknown sprite and
        // look like a broken portrait rather than a missing one.
        warn("Warning: Portrait '" + def.id +
             "' declares no 'sprite', so it is ignored.");
        continue;
      }

      portraits[def.id] = std::move(def);
    } catch (const std::exception &e) {
      warn("Failed to parse portrait " + entry.path().string() + ": " +
           std::string(e.what()));
    }
  }
}

nlohmann::json PortraitRegistry::ToJson(const PortraitDef &definition) const {
  nlohmann::json document = nlohmann::json::object();
  document["id"] = definition.id;
  if (!definition.sprite.empty()) document["sprite"] = definition.sprite;
  document["source"] = definition.source;
  return document;
}

bool PortraitRegistry::AddOrReplace(const nlohmann::json &document,
                                    const std::string &source, std::string *error,
                                    std::vector<std::string> *warnings) {
  (void)source;
  (void)warnings;
  if (!document.is_object()) {
    if (error) *error = "the entry is not a JSON object";
    return false;
  }
  PortraitDef definition;
  definition.id = IdFor(document);
  if (definition.id.empty()) {
    if (error) *error = "the entry has no id";
    return false;
  }
  definition.sprite = document.value("sprite", std::string());
  definition.source = document.value("source", std::string("auto"));
  if (definition.sprite.empty() && definition.source != "generated") {
    if (error) *error = "a portrait with no sprite must use source 'generated'";
    return false;
  }
  portraits[definition.id] = std::move(definition);
  return true;
}

std::string PortraitRegistry::IdFor(const nlohmann::json &document) {
  if (document.contains("id") && document["id"].is_string()) {
    return document["id"].get<std::string>();
  }
  return std::string();
}

const PortraitDef *PortraitRegistry::Get(const std::string &id) const {
  auto it = portraits.find(id);
  return it == portraits.end() ? nullptr : &it->second;
}

std::vector<std::string> PortraitRegistry::GetIds() const {
  std::vector<std::string> ids;
  ids.reserve(portraits.size());
  for (const auto &pair : portraits) {
    ids.push_back(pair.first);
  }
  std::sort(ids.begin(), ids.end());
  return ids;
}

std::vector<const PortraitDef *> PortraitRegistry::GetAll() const {
  std::vector<const PortraitDef *> defs;
  for (const std::string &id : GetIds()) {
    defs.push_back(Get(id));
  }
  return defs;
}
