#pragma once

#include <string>

#include "SystemOneRecorder.h"
#include "Window.h"

class Game;

// The System 1 overview: every call made to the System 1 backend, with its reply.
//
// Why it exists: to answer "is this model worth keeping?" with observations
// rather than impressions. The System 1 layer is a different model class from the
// System 2 chat model, it is expected to be swapped or dropped, and the evidence
// for that decision has to be the actual requests and replies in the situations
// the game produces -- not a benchmark and not a demo prompt.
//
// It is therefore a *passive viewer*. It makes no calls of its own: no test
// button, no manual question box. A window that can generate its own traffic
// would let its contents be mistaken for production behaviour, which is the one
// thing it must not do. Everything it shows was recorded by a real call site via
// `SystemOneRecorder::Record`.
//
// The consequence to expect: with no call site wired, this window is empty. That
// is correct rather than broken, and the empty state says so.
//
// Same lifetime rules as the other debug windows: constructed when toggled on
// (see DebugWindows.cpp), reading a recorder owned by Game for the whole run.
// Nothing here touches flecs, the AI request pipeline, or any component state, so
// it cannot disturb an agent.
class SystemOneWindow : public Window {
public:
  SystemOneWindow(Game *game, SystemOne::SystemOneRecorder *recorder);

  void Draw() override;

private:
  Game *game = nullptr;

  // Borrowed. Game owns it for the whole run, so it outlives every window
  // instance; a null pointer is supported and reported rather than crashing,
  // matching the DesignerChatWindow convention for a missing backend.
  SystemOne::SystemOneRecorder *recorder = nullptr;

  // View state, deliberately not persisted: this is a debugging view, and a
  // filter silently left on from a previous run is a good way to conclude that a
  // call site is dead when it is not.
  //
  // Empty means "all scenarios".
  std::string scenarioFilter;
  // When true, only records that could have changed behaviour. The interesting
  // view while the layer is advisory is the opposite (`OnlyAdvisory`), so both
  // are offered rather than folding them into one tri-state.
  bool onlyNonAdvisory = false;
  // Selected row, as an index into the snapshot drawn this frame. Reset to the
  // newest when it falls outside the filtered list, so the detail pane cannot
  // show a record the list no longer lists.
  int selected = 0;
  bool followNewest = true;
};
