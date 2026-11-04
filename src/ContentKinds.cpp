#include "ContentKinds.h"

#include <mutex>

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "ActorSpriteRegistry.h"
#include "ItemRegistry.h"
#include "MovementRegistry.h"
#include "RigRegistry.h"
#include "BrainTemplateRegistry.h"
#include "NpcTemplateRegistry.h"
#include "Game.h"
#include "IconRegistry.h"
#include "ObjectFactory.h"
#include "SkillRegistry.h"
#include "SurfaceRegistry.h"
#include "SurfaceSpriteRegistry.h"
#include "StatusEffectRegistry.h"
#include "EffectRegistry.h"
#include "ResourceRegistry.h"
#include "SpritePaths.hpp"
#include "TileDesigner.hpp"
#include "TileRegistry.h"

namespace ContentKinds {

namespace {

// ---- tiles -----------------------------------------------------------------

std::string TileOrigin(const TileDef &definition, const TileRegistry &tiles) {
  if (tiles.IsRuntimeAdded(definition.name)) return "generated overlay";
  if (!definition.sourcePath.empty()) return definition.sourcePath;
  return "session only";
}

std::vector<Entry> ListTiles(const World &world) {
  std::vector<Entry> out;
  for (const TileDef *definition : world.tiles.GetAll()) {
    out.push_back({definition->name, definition->name,
                   TileOrigin(*definition, world.tiles)});
  }
  return out;
}

nlohmann::json ReadTile(const World &world, const std::string &id) {
  const TileDef *definition = world.tiles.Get(id);
  if (definition == nullptr) return nlohmann::json::object();
  return TileDesigner::WriteTileDef(*definition);
}

bool ApplyTile(const World &world, const nlohmann::json &document, std::string *error,
               std::vector<std::string> *warnings) {
  const TileRegistry::AddResult result =
      world.tiles.OverrideDefinition(document, "(content editor)");
  if (!result.ok) {
    if (error != nullptr) *error = result.error;
    return false;
  }
  if (warnings != nullptr) *warnings = result.warnings;
  return true;
}

std::string TileFileName(const nlohmann::json &document) {
  return TileDesigner::SlugifyFileName(document.value("name", std::string()));
}

std::string TileSprite(const std::string &id) { return ::TileSpriteId(id); }

// ---- objects ---------------------------------------------------------------

std::vector<Entry> ListObjects(const World &world) {
  std::vector<Entry> out;
  for (const auto &entry : world.objects.GetTemplates()) {
    const std::string name = entry.second.value("name", entry.first);
    out.push_back({entry.first, name,
                   world.objects.IsRuntimeAdded(entry.first) ? "generated overlay"
                                                             : "catalog"});
  }
  std::sort(out.begin(), out.end(),
            [](const Entry &a, const Entry &b) { return a.label < b.label; });
  return out;
}

nlohmann::json ReadObject(const World &world, const std::string &id) {
  const auto &templates = world.objects.GetTemplates();
  const auto found = templates.find(id);
  if (found == templates.end()) return nlohmann::json::object();
  nlohmann::json document = found->second;
  // Always offer the art-style field, even when the template predates styles.
  if (!document.contains("style")) document["style"] = "";
  return document;
}

bool ApplyObject(const World &world, const nlohmann::json &document, std::string *error,
                 std::vector<std::string> *warnings) {
  const ObjectFactory::RuntimeAddResult result = world.objects.AddRuntime(
      document, "(content editor)", /*replaceExisting=*/true,
      /*requireAtLeastOneComponent=*/false, /*allowCatalogReplace=*/true);
  if (!result.ok) {
    if (error != nullptr) *error = result.error;
    return false;
  }
  if (warnings != nullptr) *warnings = result.warnings;
  return true;
}

std::string ObjectFileName(const nlohmann::json &document) {
  return ObjectFactory::KeyFor(document);
}

std::string ObjectSprite(const std::string &id) { return id; }

// ---- surface sprites / surfaces / statuses --------------------------------

std::vector<Entry> ListSurfaceSprites(const World &world) {
  std::vector<Entry> out;
  for (const SurfaceSpriteDef *definition : world.surfaceSprites.GetAll()) {
    out.push_back({definition->id, definition->name, "surface sprite registry"});
  }
  return out;
}

nlohmann::json ReadSurfaceSprite(const World &world, const std::string &id) {
  const SurfaceSpriteDef *definition = world.surfaceSprites.Get(id);
  if (definition == nullptr) return nlohmann::json::object();
  return world.surfaceSprites.ToJson(*definition);
}

bool ApplySurfaceSprite(const World &world, const nlohmann::json &document,
                        std::string *error, std::vector<std::string> *warnings) {
  return world.surfaceSprites.AddOrReplace(document, "(content editor)", error,
                                           warnings);
}

std::string SurfaceSpriteFileName(const nlohmann::json &document) {
  return TileDesigner::SlugifyFileName(document.value("name", std::string()));
}

std::string SurfaceSpriteSprite(const std::string &id) { return id; }

std::vector<Entry> ListSurfaces(const World &world) {
  std::vector<Entry> out;
  for (const SurfaceDef *definition : world.surfaces.GetAll()) {
    out.push_back({definition->id, definition->name, "surface registry"});
  }
  return out;
}

nlohmann::json ReadSurface(const World &world, const std::string &id) {
  const SurfaceDef *definition = world.surfaces.Get(id);
  if (definition == nullptr) return nlohmann::json::object();
  return world.surfaces.ToJson(*definition);
}

bool ApplySurface(const World &world, const nlohmann::json &document,
                  std::string *error, std::vector<std::string> *warnings) {
  return world.surfaces.AddOrReplace(document, "(content editor)", error, warnings);
}

std::string SurfaceFileName(const nlohmann::json &document) {
  return TileDesigner::SlugifyFileName(document.value("name", std::string()));
}

std::vector<Entry> ListStatusEffects(const World &world) {
  std::vector<Entry> out;
  for (const StatusEffectDef *definition : world.statusEffects.GetAll()) {
    out.push_back({definition->id, definition->name, "status registry"});
  }
  return out;
}

nlohmann::json ReadStatusEffect(const World &world, const std::string &id) {
  const StatusEffectDef *definition = world.statusEffects.Get(id);
  if (definition == nullptr) return nlohmann::json::object();
  return world.statusEffects.ToJson(*definition);
}

bool ApplyStatusEffect(const World &world, const nlohmann::json &document,
                       std::string *error, std::vector<std::string> *warnings) {
  return world.statusEffects.AddOrReplace(document, "(content editor)", error,
                                          warnings);
}

std::string StatusEffectFileName(const nlohmann::json &document) {
  return TileDesigner::SlugifyFileName(document.value("name", std::string()));
}

// ---- resources -------------------------------------------------------------

std::vector<Entry> ListResources(const World &world) {
  std::vector<Entry> out;
  for (const ResourceDef *definition : world.resources.GetAll()) {
    out.push_back({definition->id, definition->name, "resource registry"});
  }
  return out;
}

nlohmann::json ReadResource(const World &world, const std::string &id) {
  const ResourceDef *definition = world.resources.Get(id);
  if (definition == nullptr) return nlohmann::json::object();
  return world.resources.ToJson(*definition);
}

bool ApplyResource(const World &world, const nlohmann::json &document,
                   std::string *error, std::vector<std::string> *warnings) {
  return world.resources.AddOrReplace(document, "(content editor)", error,
                                      warnings);
}

std::string ResourceFileName(const nlohmann::json &document) {
  return TileDesigner::SlugifyFileName(document.value("name", std::string()));
}

// ---- art styles -------------------------------------------------------------

std::vector<Entry> ListArtStyles(const World &world) {
  std::vector<Entry> out;
  for (const ArtStyleDef *definition : world.artStyles.GetAll()) {
    out.push_back({definition->id, definition->name, "art style registry"});
  }
  return out;
}

nlohmann::json ReadArtStyle(const World &world, const std::string &id) {
  const ArtStyleDef *definition = world.artStyles.Get(id);
  if (definition == nullptr) return nlohmann::json::object();
  return world.artStyles.ToJson(*definition);
}

bool ApplyArtStyle(const World &world, const nlohmann::json &document,
                   std::string *error, std::vector<std::string> *warnings) {
  return world.artStyles.AddOrReplace(document, "(content editor)", error,
                                      warnings);
}

std::string ArtStyleFileName(const nlohmann::json &document) {
  return TileDesigner::SlugifyFileName(
      document.value("id", document.value("name", std::string())));
}

// ---- body silhouettes --------------------------------------------------------

std::vector<Entry> ListBodySilhouettes(const World &world) {
  std::vector<Entry> out;
  for (const BodySilhouetteDef *definition : world.bodySilhouettes.GetAll()) {
    out.push_back({definition->id, definition->name, "body silhouette registry"});
  }
  return out;
}

nlohmann::json ReadBodySilhouette(const World &world, const std::string &id) {
  const BodySilhouetteDef *definition = world.bodySilhouettes.Get(id);
  if (definition == nullptr) return nlohmann::json::object();
  return world.bodySilhouettes.ToJson(*definition);
}

bool ApplyBodySilhouette(const World &world, const nlohmann::json &document,
                         std::string *error,
                         std::vector<std::string> *warnings) {
  return world.bodySilhouettes.AddOrReplace(document, "(content editor)", error,
                                            warnings);
}

std::string BodySilhouetteFileName(const nlohmann::json &document) {
  return TileDesigner::SlugifyFileName(
      document.value("id", document.value("name", std::string())));
}

// ---- effects ---------------------------------------------------------------

std::vector<Entry> ListEffects(const World &world) {
  std::vector<Entry> out;
  for (const EffectDef *definition : world.effects.GetAll()) {
    out.push_back({definition->id, definition->name, "effect registry"});
  }
  return out;
}

nlohmann::json ReadEffect(const World &world, const std::string &id) {
  const EffectDef *definition = world.effects.Get(id);
  if (definition == nullptr) return nlohmann::json::object();
  return world.effects.ToJson(*definition);
}

bool ApplyEffect(const World &world, const nlohmann::json &document,
                 std::string *error, std::vector<std::string> *warnings) {
  return world.effects.AddOrReplace(document, "(content editor)", error, warnings);
}

std::string EffectFileName(const nlohmann::json &document) {
  return TileDesigner::SlugifyFileName(document.value("name", std::string()));
}

// ---- icons -----------------------------------------------------------------

std::vector<Entry> ListIcons(const World &world) {
  std::vector<Entry> out;
  for (const IconDef *definition : world.icons.GetAll()) {
    out.push_back({definition->id, definition->name, "icon registry"});
  }
  return out;
}

nlohmann::json ReadIcon(const World &world, const std::string &id) {
  const IconDef *definition = world.icons.Get(id);
  if (definition == nullptr) return nlohmann::json::object();
  return world.icons.ToJson(*definition);
}

bool ApplyIcon(const World &world, const nlohmann::json &document,
               std::string *error, std::vector<std::string> *warnings) {
  return world.icons.AddOrReplace(document, "(content editor)", error, warnings);
}

std::string IconFileName(const nlohmann::json &document) {
  return IconRegistry::IdFor(document);
}

std::string IconSprite(const std::string &id) { return id; }

// ---- skills ----------------------------------------------------------------

std::vector<Entry> ListSkills(const World &world) {
  std::vector<Entry> out;
  for (const SkillDef *definition : world.skills.GetAll()) {
    out.push_back({definition->id, definition->name, "skill catalog"});
  }
  std::sort(out.begin(), out.end(),
            [](const Entry &a, const Entry &b) { return a.label < b.label; });
  return out;
}

nlohmann::json ReadSkill(const World &world, const std::string &id) {
  return world.skills.ToJson(id);
}

bool ApplySkill(const World &world, const nlohmann::json &document, std::string *error,
                std::vector<std::string> *warnings) {
  const SkillRegistry::AddResult result =
      world.skills.OverrideDefinition(document, "(content editor)");
  if (!result.ok) {
    if (error != nullptr) *error = result.error;
    return false;
  }
  if (warnings != nullptr) *warnings = result.warnings;
  if (world.onApplied) {
    world.onApplied();
  }
  return true;
}

std::string SkillFileName(const nlohmann::json &document) {
  return (document.contains("id") && document["id"].is_string())
             ? document["id"].get<std::string>()
             : std::string();
}

nlohmann::json PrepareSkillDocument(nlohmann::json document) {
  SkillType type = SkillType::Passive;
  const std::string typeName =
      (document.contains("type") && document["type"].is_string())
          ? document["type"].get<std::string>()
          : std::string("passive");
  if (!ParseSkillType(typeName, type)) {
    return document;  // let the loader report the typo
  }
  if (SkillIsActivatable(type)) {
    // The generic form cannot add an object key, so switching a passive skill to
    // active would otherwise leave it with no activation block. An empty object
    // is enough: the loader fills every field from its defaults.
    if (!document.contains("activation") || !document["activation"].is_object()) {
      document["activation"] = nlohmann::json::object();
    }
  } else {
    document.erase("activation");
  }
  return document;
}

// ---- actor sprites ---------------------------------------------------------

// The catalogue is about appearance, not about beings: `Actor` is the ECS tag, and these
// entries say only what something looks like.
std::vector<Entry> ListActorSprites(const World &world) {
  std::vector<Entry> out;
  for (const ActorSpriteDef *definition : world.actorSprites.GetAll()) {
    out.push_back({definition->id, definition->name, "actor sprite"});
  }
  return out;
}

nlohmann::json ReadActorSprite(const World &world, const std::string &id) {
  const ActorSpriteDef *definition = world.actorSprites.Get(id);
  if (definition == nullptr) return nlohmann::json::object();
  return world.actorSprites.ToJson(*definition);
}

bool ApplyActorSprite(const World &world, const nlohmann::json &document,
                      std::string *error, std::vector<std::string> *warnings) {
  return world.actorSprites.AddOrReplace(document, "(content editor)", error, warnings);
}

std::string ActorSpriteFileName(const nlohmann::json &document) {
  return ActorSpriteRegistry::IdFor(document);
}

std::string ActorSpriteSprite(const std::string &id) { return id; }

// ---- items ----------------------------------------------------------------

std::vector<Entry> ListItems(const World &world) {
  std::vector<Entry> out;
  for (const ItemDef *item : world.items.GetAll()) {
    out.push_back({item->id, item->name.empty() ? item->id : item->name,
                   "item definition"});
  }
  return out;
}

nlohmann::json ReadItem(const World &world, const std::string &id) {
  const ItemDef *item = world.items.Get(id);
  if (item == nullptr) return nlohmann::json::object();
  return world.items.ToJson(*item);
}

bool ApplyItem(const World &world, const nlohmann::json &document,
               std::string *error, std::vector<std::string> *warnings) {
  return world.items.AddOrReplace(document, "(content editor)", error, warnings);
}

std::string ItemFileName(const nlohmann::json &document) {
  return document.value("id", std::string());
}

// ---- movements -------------------------------------------------------------

std::vector<Entry> ListMovements(const World &world) {
  std::vector<Entry> out;
  for (const MovementDef *movement : world.movements.GetAll()) {
    out.push_back({movement->id,
                   movement->name.empty() ? movement->id : movement->name,
                   movement->category.empty() ? "movement" : movement->category});
  }
  return out;
}

nlohmann::json ReadMovement(const World &world, const std::string &id) {
  const MovementDef *movement = world.movements.Get(id);
  if (movement == nullptr) return nlohmann::json::object();
  return world.movements.ToJson(*movement);
}

bool ApplyMovement(const World &world, const nlohmann::json &document,
                   std::string *error, std::vector<std::string> *warnings) {
  return world.movements.AddOrReplace(document, "(content editor)", error, warnings);
}

std::string MovementFileName(const nlohmann::json &document) {
  return document.value("id", std::string());
}

// ---- actor rigs ------------------------------------------------------------

std::vector<Entry> ListRigs(const World &world) {
  std::vector<Entry> out;
  for (const ActorRig *rig : world.rigs.GetAll()) {
    out.push_back({rig->id, rig->name, "rig definition"});
  }
  return out;
}

nlohmann::json ReadRig(const World &world, const std::string &id) {
  const ActorRig *rig = world.rigs.Get(id);
  if (rig == nullptr) return nlohmann::json::object();
  return world.rigs.ToJson(*rig);
}

bool ApplyRig(const World &world, const nlohmann::json &document,
              std::string *error, std::vector<std::string> *warnings) {
  return world.rigs.AddOrReplace(document, "(content editor)", error, warnings);
}

std::string RigFileName(const nlohmann::json &document) {
  return RigRegistry::IdFor(document);
}

// ---- NPC templates ---------------------------------------------------------

// A template is the body and the starting sheet; its art comes from the actor
// sprite it names, so this kind has no sprite of its own to edit.
std::vector<Entry> ListNpcs(const World &world) {
  std::vector<Entry> out;
  for (const NpcTemplate *definition : world.npcs.GetAll()) {
    out.push_back({definition->id, definition->name, "npc template"});
  }
  return out;
}

nlohmann::json ReadNpc(const World &world, const std::string &id) {
  const NpcTemplate *definition = world.npcs.Get(id);
  if (definition == nullptr) return nlohmann::json::object();
  return world.npcs.ToJson(*definition);
}

bool ApplyNpc(const World &world, const nlohmann::json &document,
              std::string *error, std::vector<std::string> *warnings) {
  return world.npcs.AddOrReplace(document, "(content editor)", error, warnings);
}

std::string NpcFileName(const nlohmann::json &document) {
  return NpcTemplateRegistry::IdFor(document);
}

// ---- brains ---------------------------------------------------------------

std::vector<Entry> ListBrains(const World &world) {
  std::vector<Entry> out;
  for (const BrainTemplate *definition : world.brains.GetAll()) {
    out.push_back({definition->id, definition->name, "brain"});
  }
  return out;
}

nlohmann::json ReadBrain(const World &world, const std::string &id) {
  const BrainTemplate *definition = world.brains.Get(id);
  if (definition == nullptr) return nlohmann::json::object();
  return world.brains.ToJson(*definition);
}

bool ApplyBrain(const World &world, const nlohmann::json &document,
                std::string *error, std::vector<std::string> *warnings) {
  return world.brains.AddOrReplace(document, "(content editor)", error, warnings);
}

std::string BrainFileName(const nlohmann::json &document) {
  return BrainTemplateRegistry::IdFor(document);
}

}  // namespace

bool WriteOverrideFile(const Kind &kind, const std::string &root,
                       const nlohmann::json &document, std::string *path,
                       std::string *error) {
  const std::string stem = kind.fileName != nullptr ? kind.fileName(document) : "";
  if (stem.empty()) {
    if (error != nullptr) {
      *error = "the document has no name, so there is no file to save it under";
    }
    return false;
  }

  const std::filesystem::path directory = std::filesystem::path(root) / kind.id;
  std::error_code ec;
  std::filesystem::create_directories(directory, ec);
  if (ec) {
    if (error != nullptr) {
      *error = "could not create " + directory.string() + ": " + ec.message();
    }
    return false;
  }

  const std::filesystem::path file = directory / (stem + ".json");
  std::ofstream out(file);
  if (!out.is_open()) {
    if (error != nullptr) *error = "could not open " + file.string() + " for writing";
    return false;
  }
  out << document.dump(2) << "\n";
  out.close();
  if (!out) {
    if (error != nullptr) *error = "writing " + file.string() + " failed";
    return false;
  }
  if (path != nullptr) *path = file.string();
  return true;
}

namespace {
KindRegistry &Registry() {
  static KindRegistry registry;
  return registry;
}

void RegisterBuiltinKinds() {
  static std::once_flag once;
  std::call_once(once, [] {
    Registry().Register({
          "tiles",
          "Tiles",
          /*hasSprites=*/true,
          &ListTiles,
          &ReadTile,
          &ApplyTile,
          &TileFileName,
          &TileSprite,
    });
    Registry().Register({
          "objects",
          "Objects",
          /*hasSprites=*/true,
          &ListObjects,
          &ReadObject,
          &ApplyObject,
          &ObjectFileName,
          &ObjectSprite,
    });
    Registry().Register({
          "actors",
          "Actor sprites",
          /*hasSprites=*/true,
          &ListActorSprites,
          &ReadActorSprite,
          &ApplyActorSprite,
          &ActorSpriteFileName,
          &ActorSpriteSprite,
    });
    Registry().Register({
          "icons",
          "Icons",
          /*hasSprites=*/true,
          &ListIcons,
          &ReadIcon,
          &ApplyIcon,
          &IconFileName,
          &IconSprite,
    });
    Registry().Register({
          "art_styles",
          "Art styles",
          /*hasSprites=*/false,
          &ListArtStyles,
          &ReadArtStyle,
          &ApplyArtStyle,
          &ArtStyleFileName,
          nullptr,
          "id",
    });
    Registry().Register({
          "body_silhouettes",
          "Body silhouettes",
          /*hasSprites=*/false,
          &ListBodySilhouettes,
          &ReadBodySilhouette,
          &ApplyBodySilhouette,
          &BodySilhouetteFileName,
          nullptr,
          "id",
    });
    Registry().Register({
          "skills",
          "Skills",
          /*hasSprites=*/false,
          &ListSkills,
          &ReadSkill,
          &ApplySkill,
          &SkillFileName,
          nullptr,
          "id",
          &PrepareSkillDocument,
    });
    Registry().Register({
          "items",
          "Equipment",
          /*hasSprites=*/false,
          &ListItems,
          &ReadItem,
          &ApplyItem,
          &ItemFileName,
          nullptr,
          /*fixedIdKey=*/"id",
    });
    Registry().Register({
          "movements",
          "Movements",
          /*hasSprites=*/false,
          &ListMovements,
          &ReadMovement,
          &ApplyMovement,
          &MovementFileName,
          nullptr,
          /*fixedIdKey=*/"id",
    });
    Registry().Register({
          "rigs",
          "Actor rigs",
          /*hasSprites=*/false,
          &ListRigs,
          &ReadRig,
          &ApplyRig,
          &RigFileName,
          nullptr,
    });
    Registry().Register({
          "surface_sprites",
          "Surface sprites",
          /*hasSprites=*/true,
          &ListSurfaceSprites,
          &ReadSurfaceSprite,
          &ApplySurfaceSprite,
          &SurfaceSpriteFileName,
          &SurfaceSpriteSprite,
    });
    Registry().Register({
          "surfaces",
          "Surfaces",
          /*hasSprites=*/false,
          &ListSurfaces,
          &ReadSurface,
          &ApplySurface,
          &SurfaceFileName,
          nullptr,
    });
    Registry().Register({
          "resources",
          "Resources",
          /*hasSprites=*/false,
          &ListResources,
          &ReadResource,
          &ApplyResource,
          &ResourceFileName,
          nullptr,
    });
    Registry().Register({
          "effects",
          "Effects",
          /*hasSprites=*/false,
          &ListEffects,
          &ReadEffect,
          &ApplyEffect,
          &EffectFileName,
          nullptr,
    });
    Registry().Register({
          "status_effects",
          "Status effects",
          /*hasSprites=*/false,
          &ListStatusEffects,
          &ReadStatusEffect,
          &ApplyStatusEffect,
          &StatusEffectFileName,
          nullptr,
    });
    Registry().Register({
          "npcs",
          "NPC templates",
          /*hasSprites=*/false,
          &ListNpcs,
          &ReadNpc,
          &ApplyNpc,
          &NpcFileName,
          nullptr,
    });
    Registry().Register({
          "brains",
          "Brains",
          /*hasSprites=*/false,
          &ListBrains,
          &ReadBrain,
          &ApplyBrain,
          &BrainFileName,
          nullptr,
    });
  });
}

}  // namespace

bool KindRegistry::Register(Kind kind) {
  if (kind.id.empty()) return false;
  for (Kind &existing : kinds) {
    if (existing.id == kind.id) {
      existing = std::move(kind);
      return true;
    }
  }
  kinds.push_back(std::move(kind));
  return true;
}

const Kind *KindRegistry::For(const std::string &id) const {
  for (const Kind &kind : kinds) {
    if (kind.id == id) return &kind;
  }
  return nullptr;
}

KindRegistry &MutableRegistry() {
  RegisterBuiltinKinds();
  return Registry();
}

const std::vector<Kind> &All() {
  RegisterBuiltinKinds();
  return Registry().All();
}

const Kind *For(const std::string &id) {
  return MutableRegistry().For(id);
}

}  // namespace ContentKinds
