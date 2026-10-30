#pragma once

#include "Window.h"

#include <flecs.h>

#include <string>

// One world-space frame shared by every call's graph. Without this each graph
// autofits its own bounding box, so the same node jumps around when the selected
// call changes. The graph view itself draws the union of every call graph up to
// the selected one, so nodes the navigator has seen never disappear; this frame
// is the union's bounds.
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
  // Overview: show the single navigator's full message context (system prompt,
  // every user/assistant turn and every retry correction).
  bool showFullContext = false;
  // Overview: which failed attempt of the selected call is open. Reset when the
  // selected call changes; attempts only exist for failed single-navigator calls.
  int selectedAttempt = -1;
  int selectedAttemptThought = -1;
  // Cached graph bounds. Parsing every call's graph JSON every frame is O(calls)
  // per frame; this only recomputes when a new snapshot is published.
  GraphBounds cachedGraphBounds;
  int cachedGraphBoundsVersion = -1;
  // The union of every graph up to the selected call. Cached for the same
  // reason as the bounds: re-merging every call's JSON every frame is O(calls).
  std::string cachedMergedGraphJson;
  int cachedMergedGraphVersion = -1;
  int cachedMergedGraphLastIndex = -2;
  bool cachedMergedGraphIncludeObservation = false;
};
