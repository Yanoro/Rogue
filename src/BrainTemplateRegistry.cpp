#include "BrainTemplateRegistry.h"

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

bool ParseBrain(const nlohmann::json& document, BrainTemplate& out,
                std::string* error, std::vector<std::string>* warnings) {
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
  out.modelProfile = document.value("modelProfile", std::string());
  out.background = document.value("background", std::string());
  if (document.contains("goals")) {
    const nlohmann::json& goals = document["goals"];
    if (!goals.is_object()) {
      if (warnings != nullptr) {
        warnings->push_back("Brain '" + out.name +
                            "' has a non-object \"goals\"; ignoring it");
      }
    } else if (goals.contains("terminal")) {
      if (!goals["terminal"].is_array()) {
        if (warnings != nullptr) {
          warnings->push_back("Brain '" + out.name +
                              "' has a non-array \"goals.terminal\"; ignoring it");
        }
      } else {
        for (const auto& goal : goals["terminal"]) {
          if (goal.is_string()) out.terminalGoals.push_back(goal.get<std::string>());
        }
      }
    }
  }
  return true;
}

}  // namespace

std::string BrainTemplateRegistry::IdFor(const nlohmann::json& document) {
  return Slugify(document.value("name", std::string()));
}

void BrainTemplateRegistry::Load(const std::string& directoryPath) {
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
        debugLog->LogError("Brains: " + path.string() + " is not JSON: " +
                           parseError.what());
      }
      continue;
    }
    std::string error;
    std::vector<std::string> warnings;
    if (!AddOrReplace(document, path.string(), &error, &warnings) && debugLog) {
      debugLog->LogError("Brains: " + path.string() + " refused: " + error);
    }
  }
  if (debugLog) {
    debugLog->LogInfo("Brains: " + std::to_string(brains.size()) +
                      " brain(s) from " + directoryPath);
  }
}

bool BrainTemplateRegistry::AddOrReplace(const nlohmann::json& document,
                                         const std::string& source,
                                         std::string* error,
                                         std::vector<std::string>* warnings) {
  (void)source;
  BrainTemplate parsed;
  if (!ParseBrain(document, parsed, error, warnings)) {
    return false;
  }
  brains[parsed.id] = std::move(parsed);
  return true;
}

const BrainTemplate* BrainTemplateRegistry::Get(const std::string& id) const {
  const auto found = brains.find(id);
  return found == brains.end() ? nullptr : &found->second;
}

std::vector<const BrainTemplate*> BrainTemplateRegistry::GetAll() const {
  std::vector<const BrainTemplate*> out;
  out.reserve(brains.size());
  for (const auto& [id, brain] : brains) {
    (void)id;
    out.push_back(&brain);
  }
  return out;
}

nlohmann::json BrainTemplateRegistry::ToJson(const BrainTemplate& definition) const {
  nlohmann::json document = nlohmann::json::object();
  document["name"] = definition.name;
  if (!definition.description.empty()) document["description"] = definition.description;
  if (!definition.modelProfile.empty()) document["modelProfile"] = definition.modelProfile;
  if (!definition.background.empty()) document["background"] = definition.background;
  if (!definition.terminalGoals.empty()) {
    document["goals"] = {{"terminal", definition.terminalGoals}};
  }
  return document;
}
