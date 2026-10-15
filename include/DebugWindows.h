#pragma once

#include "Window.h"
#include <flecs.h>
#include <string>

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
  FontSelectionWindow(Game* game);
  void Draw() override;
private:
  Game* game;
};

// Lists every blueprint in the registry. Selecting one arms a mouse-following
// ghost in the world; a left click on a legal site builds it. See
// docs/blueprint-design.md.
//
// A debug and curation tool rather than a game feature: its job is to make a
// design's result visible so a bad design can be told apart from a bad grammar.
// It therefore deliberately places designs without the full structural validator
// -- only in-bounds and overlap are checked -- so a broken design can still be
// seen.
class BlueprintWindow : public Window {
public:
  explicit BlueprintWindow(Game* game);
  void Draw() override;
private:
  Game* game;
  // Result of the last delete, shown under the buttons. A member rather than a
  // local because the confirmation runs in a popup that closes before the result
  // is known.
  std::string lastDeleteMessage;
};

class MapEditorWindow : public Window {
public:
  explicit MapEditorWindow(Game* game);
  void Draw() override;
private:
  Game* game;
};

// Modal-ish prompt shown after a location rectangle is dragged out. Reads and
// clears Game::pendingLocation, and only calls Map::AddLocation on confirm.
class LocationNamingWindow : public Window {
public:
  explicit LocationNamingWindow(Game* game);
  void Draw() override;
private:
  Game* game;
};

class AIMenuWindow : public Window {
public:
  explicit AIMenuWindow(Game* game);
  void Draw() override;
private:
  Game* game;
};

class NPCMenuWindow : public Window {
public:
  explicit NPCMenuWindow(Game* game);
  void Draw() override;
private:
  Game* game;
};
