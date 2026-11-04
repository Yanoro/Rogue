#include "SurfaceSpriteRegistry.h"

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

std::string SurfaceSpriteRegistry::IdFor(const nlohmann::json &document) {
  return Slugify(document.value("name", std::string()));
}

void SurfaceSpriteRegistry::Load(const std::string &directoryPath) {
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
      if (debugLog) {
        debugLog->LogError("Surface sprites: " + entry.path().string() +
                           " is not JSON: " + error.what());
      }
      continue;
    }
    std::string error;
    std::vector<std::string> warnings;
    if (!AddOrReplace(document, entry.path().string(), &error, &warnings)) {
      if (debugLog) {
        debugLog->LogError("Surface sprites: " + entry.path().string() +
                           " refused: " + error);
      }
    }
  }
}

bool SurfaceSpriteRegistry::AddOrReplace(const nlohmann::json &document,
                                         const std::string &source,
                                         std::string *error,
                                         std::vector<std::string> *warnings) {
  (void)source;
  (void)warnings;
  if (!document.is_object()) {
    if (error) *error = "the entry is not a JSON object";
    return false;
  }
  SurfaceSpriteDef definition;
  definition.id = IdFor(document);
  if (definition.id.empty()) {
    if (error) *error = "the entry has no \"name\", so it has no id";
    return false;
  }
  definition.name = document.value("name", definition.id);
  definition.description = document.value("description", std::string());
  definition.style = document.value("style", std::string());
  sprites[definition.id] = std::move(definition);
  return true;
}

const SurfaceSpriteDef *SurfaceSpriteRegistry::Get(const std::string &id) const {
  auto it = sprites.find(id);
  return it == sprites.end() ? nullptr : &it->second;
}

std::vector<const SurfaceSpriteDef *> SurfaceSpriteRegistry::GetAll() const {
  std::vector<const SurfaceSpriteDef *> out;
  for (const auto &pair : sprites) out.push_back(&pair.second);
  std::sort(out.begin(), out.end(), [](const SurfaceSpriteDef *a,
                                       const SurfaceSpriteDef *b) {
    return a->name < b->name;
  });
  return out;
}

nlohmann::json SurfaceSpriteRegistry::ToJson(
    const SurfaceSpriteDef &definition) const {
  nlohmann::json document = nlohmann::json::object();
  document["name"] = definition.name;
  if (!definition.description.empty()) document["description"] = definition.description;
  document["style"] = definition.style;
  return document;
}
