#pragma once

#include <flecs.h>
#include <string>
#include <vector>

#include "Window.h"

class Game;

// The player-facing crafting station window.
//
// It lists the recipes this station offers and this PC has learned, dims the ones the
// player cannot currently afford (they stay listed and selectable, so the requirements
// remain readable), and shows the selected recipe's materials, outputs, time and training
// with a Craft button. The button runs the same registered "Craft" interaction the [CRAFT]
// command does, so the timed action, material spending and busy rule cannot drift between
// the UI and the AI path.
//
// One window is shared: opening a second station retargets it. Knowledge is the PC's
// KnownRecipes; Debug -> craft -> show all recipes bypasses the knowledge filter only.
class CraftWindow : public Window {
public:
  CraftWindow(Game *game, flecs::entity station);
  void Draw() override;

  void SetStation(flecs::entity station);
  flecs::entity Station() const { return station; }

private:
  // Appends one system message to this session's log, trimming trailing newlines.
  void AppendMessage(std::string text);

  Game *game = nullptr;
  flecs::entity station;
  std::string selectedRecipe;
  // Every start/completion message since the window opened, in order. Never cleared by
  // selecting another recipe or starting another craft; only by closing the window.
  std::vector<std::string> messages;
  bool scrollMessages = false;
  // Serial of the last craft-completion message the window has shown, so a message from a
  // craft that finished while the window was closed is not replayed on the next open.
  unsigned long long seenCraftSerial = 0;
};
