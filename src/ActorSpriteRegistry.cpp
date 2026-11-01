#include "ActorSpriteRegistry.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

#include "DebugLog.h"

namespace {

std::string Slugify(const std::string& text) {
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

}  // namespace

std::string ActorSpriteRegistry::IdFor(const nlohmann::json& document) {
  return Slugify(document.value("name", std::string()));
}

void ActorSpriteRegistry::Load(const std::string& directoryPath) {
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
    } catch (const std::exception& error) {
      if (debugLog) {
        debugLog->LogError("Actor sprites: " + path.string() + " is not JSON: " +
                           error.what());
      }
      continue;
    }
    std::string error;
    std::vector<std::string> warnings;
    if (!AddOrReplace(document, path.string(), &error, &warnings) && debugLog) {
      debugLog->LogError("Actor sprites: " + path.string() + " refused: " + error);
    }
  }
  if (debugLog) {
    debugLog->LogInfo("Actor sprites: " + std::to_string(sprites.size()) +
                      " sprite(s) from " + directoryPath);
  }
}

bool ActorSpriteRegistry::AddOrReplace(const nlohmann::json& document,
                                       const std::string& source, std::string* error,
                                       std::vector<std::string>* warnings) {
  (void)source;
  (void)warnings;
  if (!document.is_object()) {
    if (error != nullptr) *error = "the entry is not a JSON object";
    return false;
  }
  ActorSpriteDef definition;
  definition.id = IdFor(document);
  if (definition.id.empty()) {
    if (error != nullptr) *error = "the entry has no \"name\", so it has no id";
    return false;
  }
  definition.name = document.value("name", definition.id);
  definition.description = document.value("description", std::string());
  sprites[definition.id] = std::move(definition);
  return true;
}

const ActorSpriteDef* ActorSpriteRegistry::Get(const std::string& id) const {
  const auto found = sprites.find(id);
  return found == sprites.end() ? nullptr : &found->second;
}

std::vector<const ActorSpriteDef*> ActorSpriteRegistry::GetAll() const {
  std::vector<const ActorSpriteDef*> out;
  out.reserve(sprites.size());
  for (const auto& entry : sprites) out.push_back(&entry.second);
  std::sort(out.begin(), out.end(), [](const ActorSpriteDef* a, const ActorSpriteDef* b) {
    return a->name < b->name;
  });
  return out;
}

nlohmann::json ActorSpriteRegistry::ToJson(const ActorSpriteDef& definition) const {
  nlohmann::json document = nlohmann::json::object();
  document["name"] = definition.name;
  if (!definition.description.empty()) document["description"] = definition.description;
  return document;
}
