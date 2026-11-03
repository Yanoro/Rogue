#pragma once

#include "Window.h"

class Game;

// Throwaway evaluation window: an articulated arm and a worn sword.
//
// The first version used orc_brute's 4x12 bar arm with two straight cuts, no hand and
// extreme angles; it was a stress test, not a fair look. This version uses a small
// three-segment test arm (sleeve, forearm, hand) with overlapping joints, a hand the sword
// is attached to, eased six-pose keyframes, and an angle-step control that simulates drawing
// the sword at a few pre-rendered angles rather than rotating one image freely.
//
// Nothing here is a data model. Delete the window once the decision is made.
class CutoutSpikeWindow : public Window {
public:
  explicit CutoutSpikeWindow(Game *game);

  void Draw() override;

private:
  Game *game = nullptr;
  float time = 0.0f;
  bool playing = true;
  float speed = 1.0f;
  // Degrees per sword-angle step; 0 rotates one image freely. Snapping approximates a set
  // of pre-drawn sword angles, which is what production would need for a weapon.
  int swordAngleStep = 0;
  float gripOffset = 0.0f;
  bool jointCaps = true;
};
