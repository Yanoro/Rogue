#pragma once

#include "Window.h"

#include <flecs.h>

// One world-space frame shared by every call's graph. Without this each graph
// autofits its own bounding box, so the same node jumps around when the selected
// call changes. The union of all call graphs (plus the latest observation) keeps
// every node at the same screen position across calls.
struct GraphBounds {
  bool valid = false;
  float minX = 0.0f;
  float maxX = 0.0f;
  float minY = 0.0f;
  float maxY = 0.0f;
};

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
  // Graph display toggles, per NPC window session. Reset when the window is
  // recreated; the graph snapshots themselves are unchanged.
  bool hideGraphObjects = false;
  bool hideGraphNames = false;
  // Cached graph bounds. Parsing every call's graph JSON every frame is O(calls)
  // per frame; this only recomputes when a new snapshot is published.
  GraphBounds cachedGraphBounds;
  int cachedGraphBoundsVersion = -1;
};
