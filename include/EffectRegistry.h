#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

class DebugLog;

// What an atomic effect does. Effects are deliberately small: one effect is one
// operation, never a formula and never a list of caster modifiers.
enum class EffectKind {
  Damage,
  Heal,
  ApplyStatus,
  RemoveStatus,
  CreateSurface,
  RemoveSurface,
};

struct EffectDef {
  std::string id;
  std::string name;
  std::string description;
  std::vector<std::string> tags;
  EffectKind kind = EffectKind::Damage;

  // Intrinsic defaults only. Caster equipment/class/stat scaling is resolved by
  // the shared effect fold, never stored here.
  float base = 0.0f;
  std::string damageType;

  std::string statusId;
  float statusDuration = 0.0f;

  std::string surfaceId;
  float surfaceDuration = 0.0f;
  int surfaceRadius = 0;

  bool HasTag(const std::string &tag) const;
};

class EffectRegistry {
public:
  void Load(const std::string &directoryPath);

  const EffectDef *Get(const std::string &id) const;
  std::vector<const EffectDef *> GetAll() const;
  size_t Count() const { return effects.size(); }

  nlohmann::json ToJson(const EffectDef &definition) const;
  bool AddOrReplace(const nlohmann::json &document, const std::string &source,
                    std::string *error, std::vector<std::string> *warnings);
  static std::string IdFor(const nlohmann::json &document);

  void SetDebugLog(DebugLog *log) { debugLog = log; }

private:
  std::unordered_map<std::string, EffectDef> effects;
  DebugLog *debugLog = nullptr;
};
