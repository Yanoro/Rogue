#pragma once

#include "Window.h"
#include <flecs.h>

// Everything the game holds about one character, in one read-only window: their
// name, their stats and their skills.
//
// Opened from the player's "Show Status" context-menu action on a character.
// Like StorageWindow it is carried by its own entity rather than by the
// character, because a character's own ActiveWindow slot is already spoken for
// by whichever chat or entity-info window is open on it.
//
// `character` is the entity being described. `owner` is this window's carrier,
// holding the ActiveWindow component: the X button needs it to remove itself.
class CharacterStatusWindow : public Window {
public:
  CharacterStatusWindow(flecs::entity character, flecs::entity owner);
  void Draw() override;

private:
  flecs::entity character;
  flecs::entity owner;
};
