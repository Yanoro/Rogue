#include "ContentEditorTargets.hpp"

#include <filesystem>
#include <set>
#include <utility>

#include "ContentEditorWindow.h"
#include "Game.h"
#include "Settings.h"
#include "imgui.h"

namespace {

using TargetList = std::vector<ContentEditorTarget>;
using SeenSet = std::set<std::pair<std::string, std::string>>;

// One row, if the id resolves and the kind+id pair is not already there. Deduplicating on
// the pair rather than the id alone matters: two NPCs carrying the same template on one cell
// are one row, but an actor sprite and a rig can legitimately share a name.
void Add(TargetList &out, SeenSet &seen, const std::string &kind,
         const std::string &kindLabel, const std::string &id,
         const std::string &displayName) {
  if (id.empty()) return;
  if (!seen.emplace(kind, id).second) return;
  ContentEditorTarget target;
  target.kind = kind;
  target.kindLabel = kindLabel;
  target.id = id;
  target.displayName = displayName.empty() ? id : displayName;
  out.push_back(std::move(target));
}

// The catalogue id a resolved sprite file belongs to. One name is used for the file, the
// document and the entry, so the stem is the id; the caller still checks the registry,
// because an explicit path that is not actor-sprite art has no entry to open.
std::string SpriteStem(const std::string &path) {
  if (path.empty()) return {};
  return std::filesystem::path(path).stem().string();
}

}  // namespace

std::vector<ContentEditorTarget> Game::ContentTargetsAt(GamePosition position) {
  TargetList out;
  SeenSet seen;

  // The ground first: a cell always has one tile, which is what makes a bare tile reachable
  // from a menu that used to need an object or a character to open at all.
  if (map != nullptr) {
    if (Tile *tile = map->GetTile(position.x, position.y)) {
      if (const TileDef *definition = tileRegistry.Get(tile->name)) {
        Add(out, seen, "tiles", "Tile", definition->name, definition->name);
      }
    }
  }

  // Surfaces are runtime instances in a grid, not entities. A surface has both a definition
  // and a sprite, and they are different content kinds; both are offered when they resolve.
  if (const std::vector<SurfaceInstance> *instances = surfaceGrid.Find(position)) {
    for (const SurfaceInstance &instance : *instances) {
      const SurfaceDef *definition = surfaceRegistry.Get(instance.defId);
      if (definition == nullptr) continue;
      Add(out, seen, "surfaces", "Surface", definition->id, definition->name);
      const std::string spriteId =
          definition->sprite.empty() ? definition->id : definition->sprite;
      if (const SurfaceSpriteDef *sprite = surfaceSpriteRegistry.Get(spriteId)) {
        Add(out, seen, "surface_sprites", "Surface sprite", sprite->id, sprite->name);
      }
    }
  }

  // Objects and characters standing on the cell. The player is skipped for the same reason
  // the action menu skips it: this menu looks at the world, not at yourself.
  ecs.filter<const GamePosition>().each(
      [&](flecs::entity entity, const GamePosition &at) {
        if (at.x != position.x || at.y != position.y) return;
        if (entity == playerEntity) return;
        for (const ContentEditorTarget &target : ContentTargetsFor(entity)) {
          Add(out, seen, target.kind, target.kindLabel, target.id, target.displayName);
        }
      });
  return out;
}

std::vector<ContentEditorTarget> Game::ContentTargetsFor(flecs::entity entity) {
  TargetList out;
  SeenSet seen;
  if (!entity.is_alive()) return out;

  // A template and the body it stamps are separate content: the template says what the
  // character is, the rig/sprite/portrait say what it looks like, and every one of them is
  // edited on its own tab.
  const NpcTemplate *templateDef = nullptr;
  if (const NpcTemplateId *reference = entity.get<NpcTemplateId>()) {
    templateDef = npcTemplateRegistry.Get(reference->id);
    if (templateDef != nullptr) {
      Add(out, seen, "npcs", "NPC template", templateDef->id, templateDef->name);
    }
  }

  if (const RigBody *body = entity.get<RigBody>()) {
    if (const ActorRig *rig = rigRegistry.Get(body->rig)) {
      Add(out, seen, "rigs", "Actor rig", rig->id, rig->name);
    }
  }

  if (const Portrait *portrait = entity.get<Portrait>()) {
    if (portraitRegistry.Get(portrait->id) != nullptr) {
      // A portrait has no display name of its own; its id is the name people use.
      Add(out, seen, "portraits", "Portrait", portrait->id, portrait->id);
    }
  }

  // The actor sprite id. A template usually names it; a map-authored NPC has no component
  // for it, so the resolved file's stem is matched against the catalogue.
  std::string actorSpriteId;
  if (templateDef != nullptr &&
      actorSpriteRegistry.Get(templateDef->sprite) != nullptr) {
    actorSpriteId = templateDef->sprite;
  }
  if (actorSpriteId.empty()) {
    if (const Sprite *sprite = entity.get<Sprite>()) {
      const std::string stem = SpriteStem(sprite->path);
      if (actorSpriteRegistry.Get(stem) != nullptr) actorSpriteId = stem;
    }
  }
  if (const ActorSpriteDef *sprite = actorSpriteRegistry.Get(actorSpriteId)) {
    Add(out, seen, "actors", "Actor sprite", sprite->id, sprite->name);
  }

  // An object's template key. MapAuthored wins over ItemType because ItemType is restamped
  // by Evolvable: the authored entry is the one the map still points at and the one the
  // editor should open.
  std::string objectKey;
  if (const MapAuthored *authored = entity.get<MapAuthored>()) {
    objectKey = authored->templateKey;
  }
  if (objectKey.empty()) {
    if (const ItemType *type = entity.get<ItemType>()) objectKey = type->id;
  }
  if (!objectKey.empty()) {
    const auto &templates = objectFactory.GetTemplates();
    const auto found = templates.find(objectKey);
    if (found != templates.end()) {
      Add(out, seen, "objects", "Object", objectKey,
          found->second.value("name", objectKey));
    }
  }
  return out;
}

std::vector<ContentEditorTarget> Game::ContentTargetsForSkill(
    const std::string &skillId) const {
  TargetList out;
  SeenSet seen;
  if (const SkillDef *definition = skillRegistry.Get(skillId)) {
    Add(out, seen, "skills", "Skill", definition->id, definition->name);
  }
  return out;
}

std::vector<ContentEditorTarget> Game::ContentTargetsForItem(
    const std::string &itemId) const {
  TargetList out;
  SeenSet seen;
  if (const ItemDef *definition = itemRegistry.Get(itemId)) {
    Add(out, seen, "items", "Equipment", definition->id, definition->name);
  }
  return out;
}

std::vector<ContentEditorTarget> Game::ContentTargetsForStatusEffect(
    const std::string &statusId) const {
  TargetList out;
  SeenSet seen;
  if (const StatusEffectDef *definition = statusEffectRegistry.Get(statusId)) {
    Add(out, seen, "status_effects", "Status effect", definition->id,
        definition->name);
  }
  return out;
}

void Game::OpenContentEditor(const std::string &kind, const std::string &id) {
  if (!contentEditorWindow || kind.empty() || id.empty()) return;
  // Select before showing: SelectEntry reloads the working document from the live registry,
  // and showing a window that is still on the previous entry would flash it.
  contentEditorWindow->SelectEntry(kind, id);
  contentEditorWindow->RequestFocus();
  contentEditorWindowEntity.set<ActiveWindow>({contentEditorWindow});
}

namespace ContentEditorMenu {

bool Draw(Game &game, const std::vector<ContentEditorTarget> &targets,
          bool afterRows) {
  if (!Settings().ui.openInContentEditor || targets.empty()) return false;
  if (afterRows) ImGui::Separator();
  for (const ContentEditorTarget &target : targets) {
    const std::string label =
        "Open in CE: " + target.kindLabel + " '" + target.displayName + "'";
    if (ImGui::Selectable(label.c_str())) {
      game.OpenContentEditor(target.kind, target.id);
    }
  }
  return true;
}

}  // namespace ContentEditorMenu
