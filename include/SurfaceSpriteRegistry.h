#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

class DebugLog;

// A surface sprite is reusable art: fire, water, smoke, blood. A SurfaceDef
// names one by id; several surfaces can share a sprite, and a surface can change
// art without changing its mechanics.
struct SurfaceSpriteDef {
  std::string id;
  std::string name;
  std::string description;
  // Optional ArtStyleRegistry override; empty uses the family default.
  std::string style;
};

class SurfaceSpriteRegistry {
public:
  void Load(const std::string &directoryPath);

  const SurfaceSpriteDef *Get(const std::string &id) const;
  std::vector<const SurfaceSpriteDef *> GetAll() const;
  size_t Count() const { return sprites.size(); }

  nlohmann::json ToJson(const SurfaceSpriteDef &definition) const;
  bool AddOrReplace(const nlohmann::json &document, const std::string &source,
                    std::string *error, std::vector<std::string> *warnings);
  static std::string IdFor(const nlohmann::json &document);

  void SetDebugLog(DebugLog *log) { debugLog = log; }

private:
  std::unordered_map<std::string, SurfaceSpriteDef> sprites;
  DebugLog *debugLog = nullptr;
};
