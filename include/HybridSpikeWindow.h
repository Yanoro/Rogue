#pragma once

#include <filesystem>
#include <map>
#include <string>

#include "Window.h"

class Game;

// Throwaway evaluation window: body frames + generated worn-item frames.
//
// The body is the existing frame-swapped rig (orc_brute); the sword is a generated frame
// set for the same clip, drawn at the item's z with no anchors and no runtime rotation. It
// exists to answer whether equipment frames are enough to make the hybrid (frame-swapped
// body, frame-swapped gear) read well, and whether the model can place the sword in the
// hand when it is given the arm's animation document. Nothing here is a data model; the
// worn frames are test outputs named <worn sprite>_<clip>_fN.
class HybridSpikeWindow : public Window {
public:
  explicit HybridSpikeWindow(Game *game);

  void Draw() override;

private:
  Game *game = nullptr;
  float time = 0.0f;
  bool playing = true;
  float speed = 1.0f;
  std::string clipId;
  std::string itemId = "iron_sword";
  // 0 = static worn sprite, 1 = generated worn frames, 2 = static sprite on a grip path
  // derived from the arm frames.
  int wornMode = 1;
  float gripOffset = 0.0f;
  int gripAngleStep = 0;
  std::string status;
  bool statusIsError = false;
  // Write times of the body/worn frames, so generated art replaces the cached texture.
  std::map<std::string, std::filesystem::file_time_type> stamps;
};
