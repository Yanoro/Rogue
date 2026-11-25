#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "RigRegistry.h"

class DebugLog;

// One section's overrides on top of the style's base. A section is what the asset *is*
// visually -- an icon, a world object, a body silhouette, a rig layer -- and each has
// different constraints (size, readability, outline, transparency), which is why the
// style carries section-specific instructions rather than one prompt for everything.
struct ArtStyleSection {
  std::string positive;
  std::string negative;
  std::string palette;
  std::string reference;
  std::optional<RigGeometry> geometry;
  // 0 means inherit the base/asset size.
  int width = 0;
  int height = 0;
  // How the asset is sourced: "auto" (use a provided sprite, else generate),
  // "sprite" (use the provided sprite) or "generated" (ask the model).
  std::string source;
};

// A style after a section has been applied. Empty strings mean the section had no override
// and the base value is in use.
struct ResolvedArtStyle {
  std::string positive;
  std::string negative;
  std::string palette;
  std::string reference;
  std::optional<RigGeometry> geometry;
  int width = 0;
  int height = 0;
  std::string source;
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
  // Per-section instructions, keyed by visual asset type. The asset's type selects one;
  // missing fields fall back to the base above.
  std::unordered_map<std::string, ArtStyleSection> sections;
  // The global default theme. Exactly one theme should carry it; the registry falls back
  // to the first registered theme when none does.
  bool isDefault = false;
};

class ArtStyleRegistry {
public:
  void Load(const std::string &directoryPath);

  const ArtStyleDef *Get(const std::string &id) const;
  std::vector<const ArtStyleDef *> GetAll() const;
  size_t Count() const { return styles.size(); }

  // First registered style whose appliesTo contains `family`.
  const ArtStyleDef *DefaultFor(const std::string &family) const;

  // Apply `section`'s overrides on top of the style's base. An unknown/empty section returns
  // the base unchanged.
  ResolvedArtStyle ResolveSection(const ArtStyleDef &style,
                               const std::string &section) const;

  // The theme new assets use unless a visual asset type or content entry overrides it.
  const ArtStyleDef *DefaultTheme() const;
  std::string DefaultThemeId() const;
  bool SetDefaultTheme(const std::string &id);

  // The visual asset types the generator understands. Kept here so the theme editor,
  // the visual asset type registry and the prompt builder share one vocabulary.
  static std::vector<std::string> KnownSections();

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
