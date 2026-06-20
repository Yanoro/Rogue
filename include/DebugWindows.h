#pragma once

#include "Window.h"
#include <flecs.h>

class Game;

class DebugConsoleWindow : public Window {
public:
  explicit DebugConsoleWindow(Game* game);
  void Draw() override;
private:
  Game* game;
};

class TileInfoWindow : public Window {
public:
  explicit TileInfoWindow(Game* game);
  void Draw() override;
private:
  Game* game;
};

class AStarWindow : public Window {
public:
  explicit AStarWindow(Game* game);
  void Draw() override;
private:
  Game* game;
};

class EntityOverviewWindow : public Window {
public:
  explicit EntityOverviewWindow(Game* game);
  void Draw() override;
private:
  Game* game;
};

class DebugLogWindow : public Window {
public:
  explicit DebugLogWindow(Game* game);
  void Draw() override;
private:
  Game* game;
};

class MapReloadWindow : public Window {
public:
  explicit MapReloadWindow(Game* game);
  void Draw() override;
private:
  Game* game;
};

class DrawAsciiDebugWindow : public Window {
public:
  explicit DrawAsciiDebugWindow(Game* game);
  void Draw() override;
private:
  Game* game;
};

class FontSelectionWindow : public Window {
public:
  explicit FontSelectionWindow(Game* game);
  void Draw() override;
private:
  Game* game;
};
