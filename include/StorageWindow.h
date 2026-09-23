#pragma once

#include "Window.h"
#include <flecs.h>

// Read-only view of a Storage container's contents, opened by the player's
// "Examine" context-menu action on a chest. Every held item is drawn with its
// DrawAscii glyph and colors, so the window reads the same way the map does.
//
// `container` is the chest being inspected. `owner` is the entity carrying the
// ActiveWindow component (Game's storage window entity): the X button needs it
// to remove itself, the same way the other windows do.
class StorageWindow : public Window {
public:
  StorageWindow(flecs::entity container, flecs::entity owner);
  void Draw() override;

private:
  flecs::entity container;
  flecs::entity owner;
};
