#pragma once

#include "Window.h"

#include <string>

class Game;

// The player's inventory: a 30-cell page plus a race-aware paper-doll. Equipped
// items are owned through Holds but are not inventory cells; they live only on
// the body panel.
class InventoryWindow : public Window {
public:
  explicit InventoryWindow(Game *game);

  void Draw() override;

private:
  void DrawGrid();
  void DrawPaperDoll();
  void DrawPageControls(int pageCount);

  Game *game;
  int page = 0;
  std::string status;
};
