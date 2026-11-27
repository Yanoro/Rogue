#pragma once

#include "Window.h"
#include <flecs.h>

class Game;
class StatusEffectRegistry;

// Everything the game holds about one character, in one read-only window: their
// name, their stats, their skills and their active status effects.
//
// Opened from the player's "Show Status" context-menu action on a character.
// Like StorageWindow it is carried by its own entity rather than by the
// character, because a character's own ActiveWindow slot is already spoken for
// by whichever chat or entity-info window is open on it.
//
// `character` is the entity being described. `owner` is this window's carrier,
// holding the ActiveWindow component: the X button needs it to remove itself.
// `game` and `statuses` exist for the "Open in CE" row on a status effect: the
// catalogue is owned by Game for the run, not by the ECS.
class CharacterStatusWindow : public Window {
public:
  CharacterStatusWindow(Game *game, flecs::entity character, flecs::entity owner,
                        const StatusEffectRegistry *statuses);
  void Draw() override;

private:
  Game *game = nullptr;
  const StatusEffectRegistry *statuses = nullptr;
  flecs::entity character;
  flecs::entity owner;
};
