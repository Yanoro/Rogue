#pragma once

#include "Window.h"

class Game;

// Throwaway evaluation window: an articulated arm and a worn sword.
//
// It answers one question -- does cutout animation of this pixel art read acceptably, and
// would the production version (segmented parts, pivots, keyframes, worn gear attached to a
// pivot) be worth building? The segment rectangles, pivots, keyframes and the rig/item ids
// are hardcoded for orc_brute's right arm and the iron sword. Nothing here is a data model;
// delete this window once the decision is made.
class CutoutSpikeWindow : public Window {
public:
  explicit CutoutSpikeWindow(Game *game);

  void Draw() override;

private:
  Game *game = nullptr;
  float time = 0.0f;
  bool playing = true;
  float speed = 1.0f;
};
