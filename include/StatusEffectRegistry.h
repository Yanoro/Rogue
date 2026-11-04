#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

class DebugLog;

// A reusable actor state: Wet, Burning, Slowed. Surfaces apply ids from this
// registry; the status system owns duration, tags and periodic damage.
struct StatusEffectDef {
  std::string id;
  std::string name;
  std::string description;
  std::vector<std::string> tags;
  // IconRegistry id used by the HUD and Content Editor. A status icon is UI art,
  // not a separate sprite registry.
  std::string icon;
  float durationSeconds = 5.0f;
  // Atomic EffectRegistry ids applied on each tick and at the edges of the
  // status. A tick effect can be damage, healing, another status, or a surface;
  // this definition carries no damage formula.
  std::vector<std::string> tickEffects;
  std::vector<std::string> onApplyEffects;
  std::vector<std::string> onExpireEffects;
  float tickSeconds = 1.0f;
  std::string stacking = "refresh";
  int maxStacks = 1;

  bool HasTag(const std::string &tag) const;
};

class StatusEffectRegistry {
public:
  void Load(const std::string &directoryPath);

  const StatusEffectDef *Get(const std::string &id) const;
  std::vector<const StatusEffectDef *> GetAll() const;
  size_t Count() const { return effects.size(); }

  nlohmann::json ToJson(const StatusEffectDef &definition) const;
  bool AddOrReplace(const nlohmann::json &document, const std::string &source,
                    std::string *error, std::vector<std::string> *warnings);
  static std::string IdFor(const nlohmann::json &document);

  void SetDebugLog(DebugLog *log) { debugLog = log; }

private:
  std::unordered_map<std::string, StatusEffectDef> effects;
  DebugLog *debugLog = nullptr;
};
