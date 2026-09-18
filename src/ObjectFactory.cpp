#include "ObjectFactory.h"
#include <filesystem>
#include <fstream>
#include <iostream>

#include "Map.h"

void ObjectFactory::LoadTemplates(const std::string& directoryPath) {
  if (!std::filesystem::exists(directoryPath)) {
    std::cerr << "Warning: Object template directory not found: " << directoryPath << std::endl;
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
          std::cerr << "Failed to parse object template " << entry.path() << ": " << e.what() << std::endl;
        }
      }
    }
  }
}

flecs::entity ObjectFactory::SpawnObject(flecs::world& ecs, flecs::entity parent, Map* map, const std::string& type, int x, int y) {
  if (templates.find(type) == templates.end()) {
    std::cerr << "Warning: Attempted to spawn unknown object type: " << type << std::endl;
    return flecs::entity::null();
  }

  const auto& tmpl = templates[type];
  
  flecs::entity obj = ecs.entity()
                        .child_of(parent)
                        .set<GamePosition>({x, y});

  if (map) {
    obj.set<ScreenPosition>(map->GameCoordsToScreenCoords(x, y));
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

  // Assign components
  if (tmpl.contains("components")) {
    for (const auto& compName : tmpl["components"]) {
      if (compName == "Interactable") obj.add<Interactable>();
      else if (compName == "Obstacle") obj.add<Obstacle>();
      else if (compName == "Harvestable") {
        Harvestable h;
        h.resourceType = tmpl.value("resourceType", "unknown");
        h.amountRemaining = tmpl.value("amountRemaining", 1);
        obj.set<Harvestable>(h);
      }
      else if (compName == "Workstation") {
        Workstation w;
        if (tmpl.contains("recipes")) {
          for (const auto& r : tmpl["recipes"]) {
            w.recipes.push_back(r.get<std::string>());
          }
        }
        obj.set<Workstation>(w);
      }
    }
  }

  return obj;
}
