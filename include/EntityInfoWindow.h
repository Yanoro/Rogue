#pragma once

#include "Window.h"
#include <flecs.h>
#include "Map.h"

class EntityInfoWindow : public Window {
public:
  EntityInfoWindow(flecs::entity entity, Map* map);
  void Draw() override;

private:
  flecs::entity entity;
  Map* map;
};