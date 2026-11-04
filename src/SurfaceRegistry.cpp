#include "SurfaceRegistry.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

#include "DebugLog.h"

namespace {
std::string Lower(std::string text) {
  for (char &c : text) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return text;
}

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

bool SurfaceDef::HasTag(const std::string &tag) const {
  return std::find(tags.begin(), tags.end(), tag) != tags.end();
}

std::string SurfaceRegistry::IdFor(const nlohmann::json &document) {
  return Slugify(document.value("name", std::string()));
}

void SurfaceRegistry::Load(const std::string &directoryPath) {
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
        debugLog->LogError("Surfaces: " + entry.path().string() +
                           " is not JSON: " + error.what());
      }
      continue;
    }
    std::string error;
    std::vector<std::string> warnings;
    if (!AddOrReplace(document, entry.path().string(), &error, &warnings)) {
      if (debugLog) {
        debugLog->LogError("Surfaces: " + entry.path().string() + " refused: " +
                           error);
      }
      continue;
    }
    if (debugLog) {
      for (const std::string &warning : warnings) {
        debugLog->LogWarning("Surfaces: " + warning);
      }
    }
  }
}

bool SurfaceRegistry::AddOrReplace(const nlohmann::json &document,
                                   const std::string &source,
                                   std::string *error,
                                   std::vector<std::string> *warnings) {
  (void)source;
  if (!document.is_object()) {
    if (error) *error = "the entry is not a JSON object";
    return false;
  }
  SurfaceDef definition;
  definition.id = IdFor(document);
  if (definition.id.empty()) {
    if (error) *error = "the entry has no \"name\", so it has no id";
    return false;
  }
  definition.name = document.value("name", definition.id);
  definition.description = document.value("description", std::string());
  definition.sprite = document.value("sprite", std::string());
  definition.permanent = document.value("permanent", false);
  definition.durationSeconds = document.value("duration", 30.0f);
  definition.layer = document.value("layer", 0);
  definition.alpha = document.value("alpha", 0.85f);
  definition.stayInterval = document.value("stayInterval", 1.0f);
  if (document.contains("tags") && document["tags"].is_array()) {
    for (const auto &tag : document["tags"]) {
      if (tag.is_string()) definition.tags.push_back(Lower(tag.get<std::string>()));
    }
  }
  const auto parseList = [&](const char *key, std::vector<std::string> &out) {
    if (!document.contains(key)) return;
    if (!document[key].is_array()) {
      if (warnings) warnings->push_back(definition.id + ": '" + key + "' is not an array");
      return;
    }
    for (const auto &entry : document[key]) {
      std::string effectId;
      if (entry.is_string()) {
        effectId = entry.get<std::string>();
      } else if (entry.is_object()) {
        effectId = entry.value("effect", entry.value("id", std::string()));
      }
      if (effectId.empty()) {
        if (warnings) warnings->push_back(definition.id + " " + key +
                                         ": effect entry has no id");
        continue;
      }
      out.push_back(std::move(effectId));
    }
  };
  parseList("enter", definition.onEnter);
  parseList("exit", definition.onExit);
  if (document.contains("stay") && document["stay"].is_object()) {
    definition.stayInterval = document["stay"].value("every", definition.stayInterval);
    if (document["stay"].contains("effects") &&
        document["stay"]["effects"].is_array()) {
      for (const auto &entry : document["stay"]["effects"]) {
        std::string effectId;
        if (entry.is_string()) effectId = entry.get<std::string>();
        else if (entry.is_object()) effectId = entry.value("effect", std::string());
        if (!effectId.empty()) definition.onStay.push_back(std::move(effectId));
      }
    }
  }
  if (document.contains("reactions") && document["reactions"].is_array()) {
    for (const auto &entry : document["reactions"]) {
      if (!entry.is_object()) continue;
      SurfaceReaction reaction;
      reaction.withTag = Lower(entry.value("withTag", std::string()));
      reaction.result = entry.value("result", std::string());
      reaction.removeSelf = entry.value("removeSelf", true);
      reaction.removeOther = entry.value("removeOther", false);
      if (!reaction.withTag.empty()) definition.reactions.push_back(std::move(reaction));
    }
  }
  if (definition.stayInterval <= 0.0f) definition.stayInterval = 1.0f;
  if (definition.durationSeconds < 0.0f) definition.durationSeconds = 0.0f;
  if (definition.alpha < 0.0f) definition.alpha = 0.0f;
  if (definition.alpha > 1.0f) definition.alpha = 1.0f;
  surfaces[definition.id] = std::move(definition);
  return true;
}

const SurfaceDef *SurfaceRegistry::Get(const std::string &id) const {
  auto it = surfaces.find(id);
  return it == surfaces.end() ? nullptr : &it->second;
}

std::vector<const SurfaceDef *> SurfaceRegistry::GetAll() const {
  std::vector<const SurfaceDef *> out;
  for (const auto &pair : surfaces) out.push_back(&pair.second);
  std::sort(out.begin(), out.end(), [](const SurfaceDef *a, const SurfaceDef *b) {
    return a->name < b->name;
  });
  return out;
}

nlohmann::json SurfaceRegistry::ToJson(const SurfaceDef &definition) const {
  nlohmann::json j = nlohmann::json::object();
  j["name"] = definition.name;
  if (!definition.description.empty()) j["description"] = definition.description;
  if (!definition.sprite.empty()) j["sprite"] = definition.sprite;
  j["permanent"] = definition.permanent;
  j["duration"] = definition.durationSeconds;
  j["layer"] = definition.layer;
  j["alpha"] = definition.alpha;
  j["tags"] = definition.tags;
  j["stayInterval"] = definition.stayInterval;
  const auto actions = [](const std::vector<std::string> &list) {
    return nlohmann::json(list);
  };
  j["enter"] = actions(definition.onEnter);
  j["exit"] = actions(definition.onExit);
  j["stay"] = nlohmann::json{{"every", definition.stayInterval},
                             {"effects", actions(definition.onStay)}};
  nlohmann::json reactions = nlohmann::json::array();
  for (const SurfaceReaction &reaction : definition.reactions) {
    reactions.push_back({{"withTag", reaction.withTag},
                         {"result", reaction.result},
                         {"removeSelf", reaction.removeSelf},
                         {"removeOther", reaction.removeOther}});
  }
  j["reactions"] = reactions;
  return j;
}
