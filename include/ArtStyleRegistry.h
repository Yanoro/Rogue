#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "RigRegistry.h"

class DebugLog;

// One role's overrides on top of the style's base. A role is what the asset *is*
// visually -- an icon, a world object, a body silhouette, a rig layer -- and each has
// different constraints (size, readability, outline, transparency), which is why the
// style carries role-specific instructions rather than one prompt for everything.
struct ArtStyleRole {
  std::string positive;
  std::string negative;
  std::string palette;
  std::string reference;
  std::optional<RigGeometry> geometry;
  // 0 means inherit the base/asset size.
  int width = 0;
  int height = 0;
};

// A style after a role has been applied. Empty strings mean the role had no override
// and the base value is in use.
struct ResolvedArtStyle {
  std::string positive;
  std::string negative;
  std::string palette;
  std::string reference;
  std::optional<RigGeometry> geometry;
  int width = 0;
  int height = 0;
};

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
  // Optional palette prose/hex list. The request carries it beside the style so the model
  // has specific colours rather than only adjectives.
  std::string palette;
  // Optional DSL ramp file for this style. Empty uses the global ramps.
  std::string ramps;
  // Optional sprite/document id of a style-test character. The request includes its
  // .sprite document as <style-reference> so the model has a concrete target.
  std::string reference;
  // Optional proportions. A rig that names this style and has no geometry of its own uses
  // these rectangles and anchors in its frame contract.
  std::optional<RigGeometry> geometry;
  // Per-role instructions. The asset's role selects one; missing fields fall back to the
  // base above. Known roles are in ArtStyleRegistry::KnownRoles().
  std::unordered_map<std::string, ArtStyleRole> roles;
};

class ArtStyleRegistry {
public:
  void Load(const std::string &directoryPath);

  const ArtStyleDef *Get(const std::string &id) const;
  std::vector<const ArtStyleDef *> GetAll() const;
  size_t Count() const { return styles.size(); }

  // First registered style whose appliesTo contains `family`.
  const ArtStyleDef *DefaultFor(const std::string &family) const;

  // Apply `role`'s overrides on top of the style's base. An unknown/empty role returns
  // the base unchanged.
  ResolvedArtStyle ResolveRole(const ArtStyleDef &style,
                               const std::string &role) const;

  // The roles the generator understands. Kept here so the style editor, the visual
  // catalog and the prompt builder share one vocabulary.
  static std::vector<std::string> KnownRoles();

  // The geometry a rig should be drawn against: its own, then its style's, then the
  // canonical 32x32 humanoid.
  RigGeometry GeometryFor(const ActorRig &rig) const;
  // The geometry an item's worn art is authored in: its style's geometry, then the
  // canonical humanoid.
  RigGeometry GeometryForStyle(const std::string &styleId) const;

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
