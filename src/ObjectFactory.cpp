#include "ObjectFactory.h"
#include <filesystem>
#include <fstream>
#include <iostream>

#include "Map.h"
#include "DebugLog.h"
#include "Defaults.h"
#include "ItemRegistry.h"
#include "TrainingParsing.h"

ObjectFactory::ObjectFactory() {
  RegisterDefaultComponents();
}

void ObjectFactory::RegisterDefaultComponents() {
  RegisterComponent<Interactable>("Interactable", [](flecs::entity obj, const nlohmann::json&) { obj.add<Interactable>(); });
  RegisterComponent<Obstacle>("Obstacle", [](flecs::entity obj, const nlohmann::json&) { obj.add<Obstacle>(); });
  RegisterComponent<Portable>("Portable", [](flecs::entity obj, const nlohmann::json&) { obj.add<Portable>(); });
  RegisterComponent<Storage>("Storage", [](flecs::entity obj, const nlohmann::json& tmpl) {
    Storage s;
    s.capacity = tmpl.value("capacity", 10);
    obj.set<Storage>(s);
  });
  
  RegisterComponent<Harvestable>("Harvestable", [](flecs::entity obj, const nlohmann::json& tmpl) {
    Harvestable h;
    h.amountRemaining = tmpl.value("amountRemaining", 1);
    h.timer = tmpl.value("timer", 0.0f);
    // Every harvestable has a cost; only an object that wants a different one
    // from the default has to say so.
    h.durabilityCost =
        tmpl.value("durabilityCost", DEFAULT_HARVEST_DURABILITY_COST);
    if (tmpl.contains("drops")) {
      for (const auto& drop : tmpl["drops"]) {
        h.lootTable.drops.push_back({
          drop.value("item", ""),
          drop.value("chance", 1.0f)
        });
      }
    }
    obj.set<Harvestable>(h);
  });
  
  RegisterComponent<Workstation>("Workstation", [](flecs::entity obj, const nlohmann::json& tmpl) {
    Workstation w;
    if (tmpl.contains("recipes")) {
      for (const auto& r : tmpl["recipes"]) {
        w.recipes.push_back(r.get<std::string>());
      }
    }
    obj.set<Workstation>(w);
  });
  
  RegisterComponent<Evolvable>("Evolvable", [](flecs::entity obj, const nlohmann::json& tmpl) {
    Evolvable e;
    e.timeRemaining = tmpl.value("evolveTime", 60.0f);
    e.nextStageTemplate = tmpl.value("evolveTarget", "");
    e.isActive = tmpl.value("isActive", true);
    obj.set<Evolvable>(e);
  });
  
  // Placement rules live in the template's "placement" block so the [PLACE]
  // verb can stay generic: this is what makes an item settable in the world, and
  // where it is allowed to go. A malformed block leaves the item unplaceable
  // rather than guessing, and says so at load time.
  RegisterComponent<Placeable>("Placeable", [this](flecs::entity obj, const nlohmann::json& tmpl) {
    const std::string owner =
        obj.has<ItemType>() ? obj.get<ItemType>()->id : std::string("object");
    auto warn = [this](const std::string& message) {
      if (debugLog) debugLog->LogWarning(message);
      else std::cerr << message << std::endl;
    };

    if (!tmpl.contains("placement")) {
      warn("Warning: object template '" + owner +
           "' lists the Placeable component but has no placement block; "
           "it cannot be placed.\n");
      return;
    }

    const auto& placement = tmpl["placement"];
    Placeable placeable;
    placeable.becomes = placement.value("becomes", "");
    if (placeable.becomes.empty()) {
      warn("Warning: object template '" + owner +
           "' has a placement block without 'becomes'; it cannot be placed.\n");
      return;
    }
    if (placement.contains("surface")) {
      for (const auto& surface : placement["surface"]) {
        placeable.surface.push_back(surface.get<std::string>());
      }
    }
    placeable.maxDistance =
        placement.value("maxDistance", DEFAULT_PLACEMENT_MAX_DISTANCE);
    obj.set<Placeable>(placeable);
  });

  // A capability gate: what an actor must satisfy before a verb on this object
  // will run. The equipment clause is expressed in item tags (ItemDef::tags), so
  // the requirement names a capability ("scythe") rather than a specific item id
  // and any item carrying that tag satisfies it.
  RegisterComponent<Requires>("Requires", [this](flecs::entity obj, const nlohmann::json& tmpl) {
    const std::string owner =
        obj.has<ItemType>() ? obj.get<ItemType>()->id : std::string("object");
    auto warn = [this](const std::string& message) {
      if (debugLog) debugLog->LogWarning(message);
      else std::cerr << message << std::endl;
    };

    if (!tmpl.contains("requires")) {
      warn("Warning: object template '" + owner +
           "' lists the Requires component but has no 'requires' block; it is "
           "ungated.\n");
      return;
    }

    const auto& requiresJson = tmpl["requires"];
    if (!requiresJson.is_object()) {
      warn("Warning: object template '" + owner +
           "' has a non-object 'requires'; it is ungated.\n");
      return;
    }

    // Named `gate`, not `requires`: the latter is a C++20 keyword and cannot be
    // an identifier.
    Requires gate;
    if (requiresJson.contains("equipped")) {
      const auto& equippedJson = requiresJson["equipped"];
      if (equippedJson.is_string()) {
        gate.equippedTags.push_back(equippedJson.get<std::string>());
      } else if (equippedJson.is_array()) {
        for (const auto& tagJson : equippedJson) {
          if (!tagJson.is_string() || tagJson.get<std::string>().empty()) {
            warn("Warning: object template '" + owner +
                 "' has a 'requires.equipped' entry that is not a non-empty "
                 "string; skipping it.\n");
            continue;
          }
          gate.equippedTags.push_back(tagJson.get<std::string>());
        }
      } else {
        warn("Warning: object template '" + owner +
             "' has a 'requires.equipped' that is neither a string nor an "
             "array; ignoring it.\n");
      }
    }

    if (!gate.equippedTags.empty()) {
      obj.set<Requires>(gate);
    } else {
      warn("Warning: object template '" + owner +
           "' lists the Requires component but requires nothing; it is "
           "ungated.\n");
    }
  });

  // What harvesting this object teaches. The subject-side half of skill
  // progression, so an object that teaches nothing simply omits the component --
  // harvesting an iron vein trains no skill today.
  //
  // ItemType is readable here because ApplyTemplate stamps it before running the
  // component setters, which is what lets the warning name the template.
  RegisterComponent<Trains>("Trains", [this](flecs::entity obj, const nlohmann::json& tmpl) {
    if (!tmpl.contains("trains")) {
      return;
    }
    const std::string owner =
        obj.has<ItemType>() ? obj.get<ItemType>()->id : std::string("object");

    Trains trains;
    auto warn = [this](const std::string& message) {
      if (debugLog) debugLog->LogWarning(message);
      else std::cerr << message << std::endl;
    };
    ParseTrainingGrants(tmpl["trains"], "Object template '" + owner + "'",
                        trains.grants, warn);

    if (!trains.grants.empty()) {
      obj.set<Trains>(trains);
    }
  });
}

void ObjectFactory::LoadTemplates(const std::string& directoryPath) {
  if (!std::filesystem::exists(directoryPath)) {
    std::string errStr = "Warning: Object template directory not found: " + directoryPath + "\n";
    if (debugLog) debugLog->LogError(errStr);
    else std::cerr << errStr;
    return;
  }

  for (const auto& entry : std::filesystem::directory_iterator(directoryPath)) {
    if (entry.path().extension() == ".json") {
      std::ifstream file(entry.path());
      if (file.is_open()) {
        try {
          nlohmann::json j;
          file >> j;
          
          // Use filename without extension as the type key
          std::string type = entry.path().stem().string();
          templates[type] = j;
          
        } catch (const std::exception& e) {
          std::string errStr = "Failed to parse object template " + entry.path().string() + ": " + std::string(e.what()) + "\n";
          if (debugLog) debugLog->LogError(errStr);
          else std::cerr << errStr;
        }
      }
    }
  }
}

bool ObjectFactory::ApplyTemplate(flecs::entity obj, const std::string& type, Map* map) {
  if (templates.find(type) == templates.end()) {
    std::string msg = "Warning: Attempted to apply unknown object type: " + type;
    if (debugLog) debugLog->LogWarning(msg);
    else std::cerr << msg << std::endl;
    return false;
  }

  const auto& tmpl = templates[type];

  // Remove potential old components to ensure a clean state
  for (const auto& pair : componentRemovers) {
    pair.second(obj);
  }

  if (tmpl.contains("character")) {
    DrawAscii ascii;
    std::string charStr = tmpl.value("character", "?");
    ascii.ch = charStr.empty() ? '?' : charStr[0];

    auto parseColor = [](const nlohmann::json& j) -> Color {
      if (j.is_array() && j.size() >= 3) {
        return { j[0].get<unsigned char>(), j[1].get<unsigned char>(), j[2].get<unsigned char>(), j.size() > 3 ? j[3].get<unsigned char>() : (unsigned char)255 };
      }
      return {255, 255, 255, 255};
    };

    if (tmpl.contains("characterColor")) {
      ascii.characterColor = parseColor(tmpl["characterColor"]);
    } else {
      ascii.characterColor = {255, 255, 255, 255};
    }

    if (tmpl.contains("backgroundColor")) {
      ascii.backgroundColor = parseColor(tmpl["backgroundColor"]);
    } else {
      ascii.backgroundColor = {0, 0, 0, 0};
    }

    if (map) {
      ascii.width = map->GetTileWidth();
      ascii.height = map->GetTileHeight();
    }
    
    obj.set<DrawAscii>(ascii);
  }

  // Assign Name
  std::string name = tmpl.value("name", type);
  obj.set<DisplayName>({name});

  // Stable content id: the template key, not the display name. Recipes and any
  // other logic that must identify *what* this is match on this, so renaming a
  // display name in JSON cannot silently break them.
  obj.set<ItemType>({type});

  // Durability is per-instance state seeded from the item definition: an item
  // whose definition declares none never wears out and simply carries no
  // component. Removed first so re-applying a template -- a placed item becoming
  // a world object, say -- cannot leave a stale wear state behind.
  obj.remove<Durability>();
  if (itemRegistry) {
    if (const ItemDef* def = itemRegistry->Get(type)) {
      if (def->durability > 0) {
        obj.set<Durability>({def->durability, def->durability});
      }
    }
  }

  // Short blurb surfaced by the [EXAMINE] interaction.
  obj.set<ObjectDescription>(
      {tmpl.value("description", std::string("It looks unremarkable."))});

  if (tmpl.contains("nameTagColor")) {
    Color tagColor = {
      tmpl["nameTagColor"][0],
      tmpl["nameTagColor"][1],
      tmpl["nameTagColor"][2],
      tmpl["nameTagColor"][3]
    };
    obj.set<NameTagColor>({tagColor});
  } else {
    obj.set<NameTagColor>({DARKGRAY});
  }

  // Assign components
  if (tmpl.contains("components")) {
    for (const auto& compNameJson : tmpl["components"]) {
      std::string compName = compNameJson.get<std::string>();
      if (componentSetters.find(compName) != componentSetters.end()) {
        componentSetters[compName](obj, tmpl);
      } else {
        std::string msg = "Warning: Unknown component type: " + compName;
        if (debugLog) debugLog->LogWarning(msg);
        else std::cerr << msg << std::endl;
      }
    }
  }

  return true;
}

flecs::entity ObjectFactory::SpawnObject(flecs::world& ecs, flecs::entity parent, Map* map, const std::string& type, std::optional<GamePosition> pos, bool authored) {
  if (templates.find(type) == templates.end()) {
    std::string errStr = "Warning: Attempted to spawn unknown object type: " + type + "\n";
    if (debugLog) debugLog->LogError(errStr);
    else std::cerr << errStr;
    return flecs::entity::null();
  }

  flecs::entity obj = ecs.entity().child_of(parent);

  if (pos.has_value()) {
    obj.set<GamePosition>(pos.value());
    if (map) {
      obj.set<ScreenPosition>(map->GameCoordsToScreenCoords(pos->x, pos->y));
    }
  }

  // Marked before ApplyTemplate so the marker survives every early-out below.
  if (authored) {
    obj.set<MapAuthored>({type});
  }

  ApplyTemplate(obj, type, map);

  return obj;
}

void ObjectFactory::SpawnInventory(flecs::world& ecs, flecs::entity holder,
                                   const std::vector<ItemStack>& inventory) {
  if (inventory.empty() || !holder.is_alive()) {
    return;
  }

  for (const ItemStack& stack : inventory) {
    for (int i = 0; i < stack.count; ++i) {
      flecs::entity item = SpawnObject(ecs, holder, nullptr, stack.item);
      if (item.is_alive()) {
        holder.add<Holds>(item);
      }
    }
  }
}
