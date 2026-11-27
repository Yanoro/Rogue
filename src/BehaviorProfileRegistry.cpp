#include "BehaviorProfileRegistry.h"

#include "DebugLog.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {

std::string Slugify(const std::string &text) {
  std::string out;
  bool lastUnderscore = false;
  for (unsigned char c : text) {
    if (std::isalnum(c)) {
      out.push_back(static_cast<char>(std::tolower(c)));
      lastUnderscore = false;
    } else if (!out.empty() && !lastUnderscore) {
      out.push_back('_');
      lastUnderscore = true;
    }
  }
  while (!out.empty() && out.back() == '_') out.pop_back();
  return out;
}

void Warn(const std::string &message, std::vector<std::string> *warnings) {
  if (warnings != nullptr) warnings->push_back(message);
}

bool ParseProfile(const nlohmann::json &document, BehaviorProfileDef &out,
                  std::string *error, std::vector<std::string> *warnings) {
  if (!document.is_object()) {
    if (error) *error = "the behavior profile is not an object";
    return false;
  }
  out.document = document;
  out.name = document.value("name", std::string());
  out.id = Slugify(out.name);
  if (out.id.empty()) {
    if (error) *error = "the behavior profile has no \"name\", so it has no id";
    return false;
  }
  out.description = document.value("description", std::string());
  out.initial = document.value("initial", std::string("neutral"));
  out.transitions = document.value("transitions", nlohmann::json::array());
  out.behaviors.clear();
  if (document.contains("behaviors")) {
    if (!document["behaviors"].is_object()) {
      Warn("Behavior profile '" + out.name + "': behaviors is not an object",
           warnings);
    } else {
      for (auto it = document["behaviors"].begin();
           it != document["behaviors"].end(); ++it) {
        if (!it.value().is_string()) continue;
        out.behaviors[it.key()] = it.value().get<std::string>();
      }
    }
  }
  if (out.behaviors.empty()) {
    Warn("Behavior profile '" + out.name + "' has no behaviors", warnings);
  }
  return true;
}

}  // namespace

void BehaviorProfileRegistry::Load(const std::string &directoryPath) {
  profiles.clear();
  if (directoryPath.empty()) return;
  std::error_code ec;
  const std::filesystem::path root(directoryPath);
  if (!std::filesystem::exists(root, ec)) return;
  for (const auto &entry : std::filesystem::directory_iterator(root, ec)) {
    if (ec || !entry.is_regular_file()) continue;
    if (entry.path().extension() != ".json") continue;
    std::ifstream input(entry.path());
    if (!input) continue;
    nlohmann::json document;
    try {
      input >> document;
    } catch (const std::exception &exception) {
      if (debugLog) {
        debugLog->LogWarning("Behavior profile '" + entry.path().string() +
                             "' is not valid JSON: " + exception.what());
      }
      continue;
    }
    std::string error;
    std::vector<std::string> warnings;
    BehaviorProfileDef parsed;
    if (!ParseProfile(document, parsed, &error, &warnings)) {
      if (debugLog) {
        debugLog->LogWarning("Behavior profile '" + entry.path().string() +
                             "': " + error);
      }
      continue;
    }
    for (const std::string &warning : warnings) {
      if (debugLog) debugLog->LogWarning(warning);
    }
    profiles[parsed.id] = std::move(parsed);
  }
}

const BehaviorProfileDef *BehaviorProfileRegistry::Get(
    const std::string &id) const {
  const auto found = profiles.find(id);
  return found == profiles.end() ? nullptr : &found->second;
}

std::vector<const BehaviorProfileDef *> BehaviorProfileRegistry::GetAll() const {
  std::vector<const BehaviorProfileDef *> out;
  out.reserve(profiles.size());
  for (const auto &[id, profile] : profiles) {
    (void)id;
    out.push_back(&profile);
  }
  return out;
}

nlohmann::json BehaviorProfileRegistry::ToJson(const std::string &id) const {
  const BehaviorProfileDef *profile = Get(id);
  return profile ? profile->document : nlohmann::json::object();
}

bool BehaviorProfileRegistry::AddOrReplace(const nlohmann::json &document,
                                           const std::string &source,
                                           std::string *error,
                                           std::vector<std::string> *warnings) {
  (void)source;
  BehaviorProfileDef parsed;
  if (!ParseProfile(document, parsed, error, warnings)) return false;
  profiles[parsed.id] = std::move(parsed);
  return true;
}

std::string BehaviorProfileRegistry::IdFor(const nlohmann::json &document) {
  return Slugify(document.value("name", std::string()));
}
