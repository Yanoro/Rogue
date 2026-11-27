#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

class DebugLog;

// A reaction evaluated when a surface is added. If another surface on the tile
// carries `withTag`, this surface may remove itself/other or replace both with
// `result`.
struct SurfaceReaction {
  std::string withTag;
  std::string result;
  bool removeSelf = true;
  bool removeOther = false;
};

struct SurfaceDef {
  std::string id;
  std::string name;
  std::string description;
  // SurfaceSpriteRegistry id. Empty means the surface id is the sprite convention.
  std::string sprite;
  bool permanent = false;
  float durationSeconds = 30.0f;
  int layer = 0;
  float alpha = 0.85f;
  std::vector<std::string> tags;
  float stayInterval = 1.0f;
  // Atomic EffectRegistry ids. The surface owns triggers and lifetime; the
  // effect definition owns intrinsic values and the shared fold owns scaling.
  std::vector<std::string> onEnter;
  std::vector<std::string> onStay;
  std::vector<std::string> onExit;
  std::vector<SurfaceReaction> reactions;

  bool HasTag(const std::string &tag) const;
};

class SurfaceRegistry {
public:
  void Load(const std::string &directoryPath);

  const SurfaceDef *Get(const std::string &id) const;
  std::vector<const SurfaceDef *> GetAll() const;
  size_t Count() const { return surfaces.size(); }

  nlohmann::json ToJson(const SurfaceDef &definition) const;
  bool AddOrReplace(const nlohmann::json &document, const std::string &source,
                    std::string *error, std::vector<std::string> *warnings);
  static std::string IdFor(const nlohmann::json &document);

  // Removes one definition from the live registry. Returns false when the id is
  // unknown. The Content Editor calls this after its confirmation; deleting the
  // definition file is the caller's job.
  bool Remove(const std::string &id) { return surfaces.erase(id) > 0; }

  void SetDebugLog(DebugLog *log) { debugLog = log; }

private:
  std::unordered_map<std::string, SurfaceDef> surfaces;
  DebugLog *debugLog = nullptr;
};
