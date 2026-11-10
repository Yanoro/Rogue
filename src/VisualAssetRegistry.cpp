#include "VisualAssetRegistry.h"

#include <unordered_map>

void VisualAssetRegistry::Refresh(const ContentKinds::World &world) {
  assets.clear();
  for (const ContentKinds::Kind &kind : ContentKinds::All()) {
    if (!kind.hasSprites || kind.list == nullptr || kind.read == nullptr) continue;
    const std::string role = RoleForKind(kind.id);
    if (role.empty()) continue;
    for (const ContentKinds::Entry &entry : kind.list(world)) {
      nlohmann::json document = kind.read(world, entry.id);
      if (!document.is_object()) continue;
      VisualAssetDef asset;
      asset.kind = kind.id;
      asset.id = entry.id;
      asset.label = entry.label;
      asset.role = role;
      asset.styleId = document.value("style", std::string());
      asset.source = kind.spriteId ? kind.spriteId(entry.id) : std::string();
      asset.width = document.value("width", 0);
      asset.height = document.value("height", 0);
      assets.push_back(std::move(asset));
    }
  }
}

const VisualAssetDef *VisualAssetRegistry::Find(const std::string &kind,
                                                const std::string &id) const {
  for (const VisualAssetDef &asset : assets) {
    if (asset.kind == kind && asset.id == id) return &asset;
  }
  return nullptr;
}

std::string VisualAssetRegistry::RoleForKind(const std::string &kind) {
  static const std::unordered_map<std::string, std::string> roles = {
      {"tiles", "tile"},          {"objects", "object"},
      {"actors", "actor"},        {"rigs", "rig"},
      {"items", "item"},          {"icons", "icon"},
      {"surfaces", "surface"},    {"effects", "effect"},
      {"status_effects", "icon"}, {"resources", "icon"},
      {"body_silhouettes", "silhouette"},
  };
  auto found = roles.find(kind);
  return found == roles.end() ? std::string() : found->second;
}

std::string VisualAssetRegistry::StyleFamilyForKind(const std::string &kind) {
  static const std::unordered_map<std::string, std::string> families = {
      {"tiles", "tile"},              {"objects", "object"},
      {"actors", "actor_sprite"},     {"rigs", "actor_sprite"},
      {"items", "item"},              {"icons", "slot_icon"},
      {"surfaces", "surface_sprite"}, {"effects", "effect"},
      {"status_effects", "slot_icon"},{"resources", "slot_icon"},
      {"body_silhouettes", "body_silhouette"},
  };
  auto found = families.find(kind);
  return found == families.end() ? std::string() : found->second;
}

const ArtStyleDef *VisualAssetRegistry::StyleFor(
    const VisualAssetDef &asset, const ArtStyleRegistry &styles) const {
  if (!asset.styleId.empty()) {
    if (const ArtStyleDef *style = styles.Get(asset.styleId)) return style;
  }
  return styles.DefaultFor(StyleFamilyForKind(asset.kind));
}
