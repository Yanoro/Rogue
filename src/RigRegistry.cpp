#include "RigRegistry.h"

#include "DebugLog.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>

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

bool ParseRig(const nlohmann::json& document, ActorRig& out, std::string* error,
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
  out.width = document.value("width", 32);
  out.height = document.value("height", 32);
  if (out.width <= 0 || out.height <= 0) {
    if (error != nullptr) *error = "width and height must be positive";
    return false;
  }

  if (!document.contains("parts") || !document["parts"].is_array() ||
      document["parts"].empty()) {
    if (error != nullptr) {
      *error = "a rig needs a non-empty \"parts\" array: the layers it is made of";
    }
    return false;
  }

  std::set<std::string> seen;
  for (const auto& entry : document["parts"]) {
    if (!entry.is_object()) {
      if (warnings != nullptr) warnings->push_back("a part is not an object; skipped");
      continue;
    }
    RigPart part;
    part.id = entry.value("id", std::string());
    part.sprite = entry.value("sprite", std::string());
    part.z = entry.value("z", 0);
    if (part.id.empty()) {
      if (warnings != nullptr) {
        warnings->push_back("a part has no \"id\"; skipped");
      }
      continue;
    }
    if (part.sprite.empty()) {
      if (warnings != nullptr) {
        warnings->push_back("part '" + part.id +
                            "' has no \"sprite\"; skipped");
      }
      continue;
    }
    if (!seen.insert(part.id).second) {
      if (warnings != nullptr) {
        warnings->push_back("part '" + part.id +
                            "' appears more than once; the later one wins");
        out.parts.erase(std::remove_if(out.parts.begin(), out.parts.end(),
                                       [&](const RigPart& existing) {
                                         return existing.id == part.id;
                                       }),
                        out.parts.end());
      }
    }
    out.parts.push_back(std::move(part));
  }
  if (out.parts.empty()) {
    if (error != nullptr) *error = "no part could be read";
    return false;
  }
  return true;
}

}  // namespace

std::string RigRegistry::IdFor(const nlohmann::json& document) {
  return Slugify(document.value("name", std::string()));
}

void RigRegistry::Load(const std::string& directoryPath) {
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
        debugLog->LogError("Actor rigs: " + path.string() + " is not JSON: " +
                           parseError.what());
      }
      continue;
    }
    std::string error;
    std::vector<std::string> warnings;
    if (!AddOrReplace(document, path.string(), &error, &warnings)) {
      if (debugLog) {
        debugLog->LogError("Actor rigs: " + path.string() + " refused: " + error);
      }
      continue;
    }
    if (debugLog) {
      for (const std::string& warning : warnings) {
        debugLog->LogWarning("Actor rigs: " + path.string() + ": " + warning);
      }
    }
  }
  if (debugLog) {
    debugLog->LogInfo("Actor rigs: " + std::to_string(rigs.size()) + " rig(s) from " +
                      directoryPath);
  }
}

bool RigRegistry::AddOrReplace(const nlohmann::json& document,
                               const std::string& source, std::string* error,
                               std::vector<std::string>* warnings) {
  (void)source;
  ActorRig parsed;
  if (!ParseRig(document, parsed, error, warnings)) {
    return false;
  }
  rigs[parsed.id] = std::move(parsed);
  return true;
}

const ActorRig* RigRegistry::Get(const std::string& id) const {
  const auto found = rigs.find(id);
  return found == rigs.end() ? nullptr : &found->second;
}

std::vector<const ActorRig*> RigRegistry::GetAll() const {
  std::vector<const ActorRig*> out;
  out.reserve(rigs.size());
  for (const auto& [id, rig] : rigs) {
    (void)id;
    out.push_back(&rig);
  }
  return out;
}

nlohmann::json RigRegistry::ToJson(const ActorRig& definition) const {
  nlohmann::json document = nlohmann::json::object();
  document["name"] = definition.name;
  if (!definition.description.empty()) {
    document["description"] = definition.description;
  }
  document["width"] = definition.width;
  document["height"] = definition.height;
  nlohmann::json parts = nlohmann::json::array();
  for (const RigPart& part : definition.parts) {
    parts.push_back({{"id", part.id}, {"sprite", part.sprite}, {"z", part.z}});
  }
  document["parts"] = std::move(parts);
  return document;
}
