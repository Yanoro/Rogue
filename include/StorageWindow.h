#pragma once

#include "Window.h"
#include <flecs.h>

class ResourceManager;

// Read-only view of the items an entity holds: a Storage container's contents
// or a character's inventory. Opened by the player's "See Inventory"
// context-menu action on a container or NPC, and from the NPC Menu. Every held
// item is drawn with its sprite when it has one and its DrawAscii art otherwise,
// so the window reads the same way the map does.
//
// `container` is the entity being inspected. `owner` is this window's own
// carrier entity holding the ActiveWindow component (one per open inventory):
// the X button needs it to remove itself, the same way the other windows do.
// `resources` is the texture cache the sprite rows resolve through. It is handed
// in rather than reached for through Game, because this window deliberately holds
// no Game pointer. Deliberately non-const: resolving a sprite is what populates
// the cache's success and absence memos.
class StorageWindow : public Window {
public:
  StorageWindow(flecs::entity container, flecs::entity owner,
                ResourceManager *resources = nullptr);
  void Draw() override;

private:
  flecs::entity container;
  flecs::entity owner;
  ResourceManager *resources = nullptr;
};
