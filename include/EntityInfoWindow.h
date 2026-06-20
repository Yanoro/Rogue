#pragma once

#include "Window.h"
#include <flecs.h>

class EntityInfoWindow : public Window {
public:
  EntityInfoWindow(flecs::entity entity);
  void Draw() override;

private:
  flecs::entity entity;
};