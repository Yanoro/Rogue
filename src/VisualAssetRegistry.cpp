#include "VisualAssetRegistry.h"

#include <fstream>
#include <unordered_map>

#include <nlohmann/json.hpp>

namespace {

std::vector<VisualAssetDef> BuiltinTypes() {
  return {
      {"inventory_icon", "Inventory icons", "inventory_icon", "", {"items", "icons"}, 32, 32},
      {"resource_icon", "Resource icons", "resource_icon", "", {"resources"}, 32, 32},
      {"skill_icon", "Skill icons", "skill_icon", "", {"skills", "icons"}, 32, 32},
      {"status_icon", "Status icons", "status_icon", "", {"status_effects"}, 32, 32},
      {"portrait", "Character portraits", "portrait", "", {"portraits", "actors"}, 96, 96},
      {"actor", "Actor sprites", "actor", "", {"actors"}, 96, 96},
      {"rig", "Actor rigs", "rig", "", {"rigs"}, 96, 96},
      {"object", "World objects", "object", "", {"objects"}, 32, 32},
      {"surface", "Surfaces", "surface", "", {"surface_sprites"}, 32, 32},
      {"effect_icon", "Effect icons", "effect_icon", "", {"effects"}, 32, 32},
      {"effect_sprite", "Effect sprites", "effect_sprite", "", {"effects"}, 32, 32},
      {"silhouette", "Body silhouettes", "silhouette", "", {"body_silhouettes"}, 96, 96},
      {"tile", "Ground tiles", "tile", "", {"tiles"}, 32, 32},
  };
}

}  // namespace

void VisualAssetRegistry::Load(const std::string &path) {
  types = BuiltinTypes();
  std::ifstream in(path);
  if (!in.is_open()) return;
  nlohmann::json document;
  in >> document;
  if (!document.is_object() || !document.contains("types") ||
      !document["types"].is_array()) {
    return;
  }
  for (const auto &value : document["types"]) {
    if (!value.is_object()) continue;
    const std::string id = value.value("id", std::string());
    auto found = std::find_if(types.begin(), types.end(),
                              [&id](const VisualAssetDef &type) { return type.id == id; });
    if (found == types.end()) continue;
    found->label = value.value("label", found->label);
    found->section = value.value("section", found->section);
    found->theme = value.value("theme", found->theme);
    found->width = value.value("width", found->width);
    found->height = value.value("height", found->height);
    if (value.contains("consumes") && value["consumes"].is_array()) {
      found->consumes.clear();
      for (const auto &kind : value["consumes"]) {
        if (kind.is_string()) found->consumes.push_back(kind.get<std::string>());
      }
    }
  }
}

bool VisualAssetRegistry::Save(const std::string &path) const {
  nlohmann::json document;
  document["types"] = nlohmann::json::array();
  for (const VisualAssetDef &type : types) {
    nlohmann::json value;
    value["id"] = type.id;
    value["label"] = type.label;
    value["section"] = type.section;
    value["theme"] = type.theme;
    value["consumes"] = type.consumes;
    value["width"] = type.width;
    value["height"] = type.height;
    document["types"].push_back(std::move(value));
  }
  std::ofstream out(path);
  if (!out.is_open()) return false;
  out << document.dump(2) << "\n";
  return true;
}

const VisualAssetDef *VisualAssetRegistry::Find(const std::string &id) const {
  for (const VisualAssetDef &type : types) {
    if (type.id == id) return &type;
  }
  return nullptr;
}

bool VisualAssetRegistry::SetTheme(const std::string &id, const std::string &theme) {
  for (VisualAssetDef &type : types) {
    if (type.id == id) {
      type.theme = theme;
      return true;
    }
  }
  return false;
}

std::string VisualAssetRegistry::TypeForKind(const std::string &contentKind) {
  static const std::unordered_map<std::string, std::string> mapping = {
      {"tiles", "tile"},           {"objects", "object"},
      {"actors", "actor"},         {"rigs", "rig"},
      {"items", "inventory_icon"}, {"skills", "skill_icon"},
      {"resources", "resource_icon"}, {"status_effects", "status_icon"},
      {"effects", "effect"},       {"surface_sprites", "surface"},
      {"body_silhouettes", "silhouette"}, {"icons", "inventory_icon"},
      {"npcs", "actor"},
  };
  auto found = mapping.find(contentKind);
  return found == mapping.end() ? std::string() : found->second;
}

const ArtStyleDef *VisualAssetRegistry::ResolveTheme(
    const VisualAssetDef &type, const std::string &contentTheme,
    const ArtStyleRegistry &themes) const {
  if (!contentTheme.empty()) {
    if (const ArtStyleDef *theme = themes.Get(contentTheme)) return theme;
  }
  if (!type.theme.empty()) {
    if (const ArtStyleDef *theme = themes.Get(type.theme)) return theme;
  }
  return themes.DefaultTheme();
}
