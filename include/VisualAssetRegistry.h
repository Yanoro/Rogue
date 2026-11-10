#pragma once

#include <string>
#include <vector>

#include "ArtStyleRegistry.h"
#include "ContentKinds.h"

// One visual asset in the game: an icon, an object sprite, a rig, a body silhouette...
// The visual catalog is an index over the existing domain registries, not a second copy
// of their data. Each entry carries the role whose style instructions it follows.
struct VisualAssetDef {
  std::string kind;     // ContentKinds id: icons, objects, actors, rigs, ...
  std::string id;       // the domain registry key
  std::string label;    // human-readable name
  std::string role;     // actor, rig, icon, item, object, surface, effect, silhouette, tile
  std::string styleId;  // the asset's explicit style; empty = the kind's default
  std::string source;   // sprite convention id / icon id / rig id
  int width = 0;
  int height = 0;
};

// The visual asset registry: the one place that can answer "what visual assets exist,
// what role are they, and which art style do they use". The actual definitions stay in
// their domain registries; Refresh() reads them through ContentKinds.
class VisualAssetRegistry {
public:
  void Refresh(const ContentKinds::World &world);

  const std::vector<VisualAssetDef> &GetAll() const { return assets; }
  const VisualAssetDef *Find(const std::string &kind, const std::string &id) const;

  // The role a content kind's entries play. Empty for non-visual kinds.
  static std::string RoleForKind(const std::string &kind);
  // The art style family the kind defaults to (ArtStyleDef::appliesTo).
  static std::string StyleFamilyForKind(const std::string &kind);

  // The style an asset should use: its own styleId, then the kind's default family.
  const ArtStyleDef *StyleFor(const VisualAssetDef &asset,
                              const ArtStyleRegistry &styles) const;

private:
  std::vector<VisualAssetDef> assets;
};
