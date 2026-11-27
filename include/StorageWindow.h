#pragma once

#include "Window.h"
#include <flecs.h>
#include <string>

class Game;
class ResourceManager;

// A holder's items: a Storage container's contents, or a character's inventory.
//
// Opened by the player's "See Inventory" context-menu action on a container or NPC, and
// from the NPC Menu. Cells are drawn the way the player's inventory draws them: stackable
// items share a cell, every slot up to capacity is drawn even when empty, and one shared
// cell plate (the inventory_slot visual asset) is used by both windows.
//
// A container is interactive: right-clicking a cell offers "Take", and cells can be dragged
// between this window and the player's inventory in both directions. A character's
// inventory is read-only apart from its "Open in CE" rows -- looting a living NPC is a
// decision this window does not make.
//
// `game` is needed for the player entity, the item registry and the transfer rules, which
// is a change from the old read-only view that deliberately held no Game pointer.
// `resources` is the texture cache the cell sprites resolve through.
class StorageWindow : public Window {
public:
  StorageWindow(flecs::entity container, flecs::entity owner, Game *game,
                ResourceManager *resources = nullptr);
  void Draw() override;

private:
  flecs::entity container;
  flecs::entity owner;
  Game *game = nullptr;
  ResourceManager *resources = nullptr;
  // The last transfer result ("Took ...", "Not enough room ..."). Kept rather than spoken
  // because a refused drop has to say why somewhere the player is looking.
  std::string status;
};
