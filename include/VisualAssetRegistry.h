#pragma once

#include <string>
#include <vector>

#include "ArtStyleRegistry.h"

// One visual asset *type*, not one item/icon/actor. An inventory icon, a resource icon, a
// character portrait, an actor rig and so on are the categories whose look a theme
// describes. The individual content entries (the sword, the health potion) belong to a
// type and inherit its theme; a content entry may override the theme as an exception.
struct VisualAssetDef {
  std::string id;        // inventory_icon, resource_icon, skill_icon, status_icon,
                         // portrait, actor, rig, object, surface, effect, silhouette, tile
  std::string label;
  std::string section;   // the theme section key (normally the same id)
  std::string theme;     // type-level theme override; empty = the global default theme
  std::vector<std::string> consumes;  // content kinds / sources this type covers
  int width = 0;         // 0 means the theme section decides
  int height = 0;
};

// The visual asset type registry. It is a registry of types, not a catalog of every
// content entry; the Content Editor's content kinds remain the data editors.
class VisualAssetRegistry {
public:
  // Loads the type list (and each type's theme override) from `path`; uses the built-in
  // defaults when the file is absent.
  void Load(const std::string &path);
  bool Save(const std::string &path) const;

  const std::vector<VisualAssetDef> &GetAll() const { return types; }
  const VisualAssetDef *Find(const std::string &id) const;
  bool SetTheme(const std::string &id, const std::string &theme);

  // The visual type a content kind feeds. Empty for non-visual kinds.
  static std::string TypeForKind(const std::string &contentKind);

  // The theme an asset uses: content override, then type override, then the global
  // default. The section is the type's section key.
  const ArtStyleDef *ResolveTheme(const VisualAssetDef &type,
                                  const std::string &contentTheme,
                                  const ArtStyleRegistry &themes) const;

private:
  std::vector<VisualAssetDef> types;
};
