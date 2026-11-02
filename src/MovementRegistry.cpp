#include "MovementRegistry.h"

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

void ReadStrings(const nlohmann::json& document, const char* key,
                 std::vector<std::string>& out, std::vector<std::string>* warnings) {
  if (!document.contains(key) || document[key].is_null()) return;
  if (!document[key].is_array()) {
    if (warnings != nullptr) {
      warnings->push_back(std::string("\"") + key + "\" is not an array; ignored");
    }
    return;
  }
  for (const auto& entry : document[key]) {
    if (entry.is_string()) out.push_back(entry.get<std::string>());
  }
}

}  // namespace

std::string MovementRegistry::IdFor(const nlohmann::json& document) {
  return Slugify(document.value("name", std::string()));
}

void MovementRegistry::Load(const std::string& directoryPath) {
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
        debugLog->LogError("Movements: " + path.string() + " is not JSON: " +
                           parseError.what());
      }
      continue;
    }
    std::string error;
    std::vector<std::string> warnings;
    if (!AddOrReplace(document, path.string(), &error, &warnings)) {
      if (debugLog) {
        debugLog->LogError("Movements: " + path.string() + " refused: " + error);
      }
      continue;
    }
    if (debugLog) {
      for (const std::string& warning : warnings) {
        debugLog->LogWarning("Movements: " + path.string() + ": " + warning);
      }
    }
  }
  if (debugLog) {
    debugLog->LogInfo("Movements: " + std::to_string(movements.size()) +
                      " movement(s) from " + directoryPath);
  }
}

bool MovementRegistry::AddOrReplace(const nlohmann::json& document,
                                    const std::string& source, std::string* error,
                                    std::vector<std::string>* warnings) {
  (void)source;
  if (!document.is_object()) {
    if (error != nullptr) *error = "the document is not a JSON object";
    return false;
  }

  MovementDef movement;
  movement.name = document.value("name", std::string());
  movement.id = Slugify(movement.name);
  if (movement.id.empty()) {
    if (error != nullptr) *error = "the movement has no \"name\", so it has no id";
    return false;
  }
  movement.category = document.value("category", std::string());
  movement.description = document.value("description", std::string());
  movement.frames = document.value("frames", 4);
  movement.fps = document.value("fps", 8);
  movement.loop = document.value("loop", true);

  if (movement.frames < 1 || movement.frames > 64) {
    if (warnings != nullptr) {
      warnings->push_back("frame count is out of range; clamped to 1..64");
    }
    movement.frames = std::clamp(movement.frames, 1, 64);
  }
  if (movement.fps < 1 || movement.fps > 60) {
    if (warnings != nullptr) warnings->push_back("fps is out of range; clamped to 1..60");
    movement.fps = std::clamp(movement.fps, 1, 60);
  }

  ReadStrings(document, "parts", movement.parts, warnings);
  ReadStrings(document, "requires", movement.needs, warnings);
  ReadStrings(document, "poses", movement.poses, warnings);

  if (!movement.poses.empty() &&
      static_cast<int>(movement.poses.size()) != movement.frames) {
    if (warnings != nullptr) {
      warnings->push_back("has " + std::to_string(movement.poses.size()) +
                          " pose line(s) for " + std::to_string(movement.frames) +
                          " frame(s); the extra lines are ignored, missing ones are "
                          "left to the prompt");
    }
  }
  if (movement.description.empty() && warnings != nullptr) {
    warnings->push_back("has no description; the generation prompt will be weak");
  }

  movements[movement.id] = std::move(movement);
  return true;
}

const MovementDef* MovementRegistry::Get(const std::string& id) const {
  const auto found = movements.find(id);
  return found == movements.end() ? nullptr : &found->second;
}

std::vector<const MovementDef*> MovementRegistry::GetAll() const {
  std::vector<const MovementDef*> out;
  out.reserve(movements.size());
  for (const auto& [id, movement] : movements) {
    (void)id;
    out.push_back(&movement);
  }
  std::sort(out.begin(), out.end(),
            [](const MovementDef* a, const MovementDef* b) { return a->id < b->id; });
  return out;
}

nlohmann::json MovementRegistry::ToJson(const MovementDef& definition) const {
  nlohmann::json document = nlohmann::json::object();
  document["name"] = definition.name;
  if (!definition.category.empty()) document["category"] = definition.category;
  if (!definition.description.empty()) document["description"] = definition.description;
  document["frames"] = definition.frames;
  document["fps"] = definition.fps;
  document["loop"] = definition.loop;
  if (!definition.parts.empty()) document["parts"] = definition.parts;
  if (!definition.needs.empty()) document["requires"] = definition.needs;
  if (!definition.poses.empty()) document["poses"] = definition.poses;
  return document;
}
