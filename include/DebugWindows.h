#pragma once

#include "Window.h"
#include "BlueprintExpansion.hpp"
#include "ContentFiles.h"
#include <flecs.h>
#include <optional>
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

// Lists every blueprint in the registry and previews the one you are looking at.
// The Select button on a design arms a mouse-following ghost in the world; a left
// click on a legal site builds it. See docs/blueprint-design.md.
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

  // Which design the preview pane shows, and its expansion.
  //
  // Deliberately separate from Game::selectedBlueprintId: browsing a design and
  // arming one for placement are different acts now, so clicking a row previews it
  // while the Select button is what arms the ghost. The expansion is cached because
  // the preview redraws every frame and rasterising a shape set per frame would be
  // wasted work.
  std::string previewedBlueprintId;
  std::optional<BlueprintExpansion::Expansion> previewedExpansion;

  // A design awaiting deletion, applied at the top of the next Draw.
  //
  // Deferred because deleting reloads the registry, and this window is mid-frame
  // holding a `registry` pointer and a `selected` pointer into the map it frees.
  // Applying it later means the frame that requests the delete never reads either
  // one again.
  std::string pendingDeleteId;

  // Layout, persisted. paneWidth is the preview pane; zoom is the cell size in it.
  float paneWidth = 420.0f;
  float zoom = 64.0f;
  // Read on the first frame rather than in the constructor, because the state file
  // is loaded during Init and this window is built as part of it.
  bool layoutLoaded = false;
};

class MapEditorWindow : public Window {
public:
  explicit MapEditorWindow(Game* game);
  void Draw() override;
private:
  Game* game;

  // Applies a confirmed deletion: removes the files, then makes the two content
  // registries agree with the disk. Called first thing in Draw, so the frame that
  // confirmed never goes on to read the definitions this frees.
  void ApplyConfirmedDelete();

  // The confirmation dialog: names every file that would go and every map that
  // references the content, and is the only place a deletion can be committed.
  void DrawDeleteConfirmation();

  // --- Permanent content deletion (the palette rows' Delete buttons) ---------
  //
  // Separate from Game::removeObjectMode in every way: that one edits the map,
  // this one erases files from disk. Kept here rather than on Game because it is
  // entirely the window's business -- the grid has no part in it and nothing
  // outside this class needs to know a deletion is pending.
  //
  // The plan is built once, when a row's Delete button is pressed, and cached for
  // the life of the confirmation popup: building it scans every map file on disk
  // for references, which is not something to redo on every frame the dialog is
  // open. It is also what the dialog reads, so what is shown is exactly what will
  // be deleted.
  std::optional<ContentPlan> pendingDeletePlan;

  // A plan the user confirmed, applied at the top of the next Draw.
  //
  // Deferred for the same reason BlueprintWindow defers: the palette loop above is
  // mid-iteration over TileDef pointers this deletion frees, so the frame that
  // confirms must not go on to read them. Next frame, nothing holds them.
  std::optional<ContentPlan> confirmedDeletePlan;

  // Result of the last deletion, shown under the palette. A member rather than a
  // local because the popup closes before the outcome is known.
  std::string lastContentDeleteMessage;
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

// The village-wide task board (docs/task-system-design.md section 11): one row
// per agent, sorted by tier then age, so "why is this agent doing something dumb"
// is answerable at a glance instead of by reading a conversation.
//
// A developer tool, not a diegetic capability: it renders NPCs' task lists
// because a human with a debug build should see everything, and no agent can read
// another's list through any in-world path (section 10).
class TaskBoardWindow : public Window {
public:
  explicit TaskBoardWindow(Game* game);
  void Draw() override;
private:
  Game* game;
};

// The vision debug window (docs/spatial-memory-design.md section 7).
//
// Stage 0 holds one control -- whether the map draws a facing marker per
// character -- plus the counts that say whether every character actually has a
// `Facing`. It exists this early on purpose: the marker has to be trustworthy
// before anything is sampled from it, and "I turned it on and some characters
// have no direction" is exactly the bug that would otherwise be blamed on the
// cone in stage 1.
//
// The window owns no state. The checkbox value lives in DebugWindowState so it
// survives the window being closed and the process being restarted.
class VisionDebugWindow : public Window {
public:
  explicit VisionDebugWindow(Game* game);
  void Draw() override;
private:
  Game* game;
};
