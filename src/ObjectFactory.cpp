#include "ObjectFactory.h"
#include <filesystem>
#include <fstream>
#include <iostream>

#include "Map.h"
#include "DebugLog.h"
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
  
  RegisterComponent<Seed>("Seed", [](flecs::entity obj, const nlohmann::json& tmpl) {
    obj.add<Seed>();
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
