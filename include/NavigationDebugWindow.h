#pragma once

#include "Window.h"

#include <flecs.h>

// Per-character navigation debug view (docs/llm-s1-navigation-design.md).
//
// Opened from the NPC Menu's Navigation column. It reads the NavigationDebugState
// snapshot that NavigateAction publishes, so it keeps the last episode after the
// action ends. Read-only: the snapshot is evidence, not an edit surface.
class NavigationDebugWindow : public Window {
public:
  NavigationDebugWindow(flecs::entity npc, flecs::entity owner);
  void Draw() override;

private:
  flecs::entity npc;
  flecs::entity owner;
  // Overview thought selection. -1 / true follows the newest strategist call;
  // clicking an older call pins it until the user presses Latest.
  int selectedThought = -1;
  bool followLatestThought = true;
};
