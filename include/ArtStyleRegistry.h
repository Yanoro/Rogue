#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

class DebugLog;

// A prose style profile for generated art. It is deliberately prompt data, not a
// renderer: the sprite DSL still owns the pixels. Styles are layered by the prompt
// builder (global direction is authored in the sprite prompt; this is the family
// voice) and assets may name an override.
struct ArtStyleDef {
  std::string id;
  std::string name;
  std::string description;
  // Families this style is the default for: slot_icon, skill_icon, item,
  // surface_sprite, actor_sprite, tile, portrait, body_silhouette.
  std::vector<std::string> appliesTo;
  // Positive prose copied into the sprite-document request.
  std::string positive;
  // Negative prose. Kept separate so the request can say "Do not" once.
  std::string negative;
};

class ArtStyleRegistry {
public:
  void Load(const std::string &directoryPath);

  const ArtStyleDef *Get(const std::string &id) const;
  std::vector<const ArtStyleDef *> GetAll() const;
  size_t Count() const { return styles.size(); }

  // First registered style whose appliesTo contains `family`.
  const ArtStyleDef *DefaultFor(const std::string &family) const;

  nlohmann::json ToJson(const ArtStyleDef &definition) const;
  bool AddOrReplace(const nlohmann::json &document, const std::string &source,
                    std::string *error, std::vector<std::string> *warnings);
  static std::string IdFor(const nlohmann::json &document);

  void SetDebugLog(DebugLog *log) { debugLog = log; }

private:
  std::unordered_map<std::string, ArtStyleDef> styles;
  std::vector<std::string> order;
  DebugLog *debugLog = nullptr;
};
