#pragma once

#include <string>
#include <vector>

class Game;

// One piece of content an "Open in CE" row can open: the Content Editor kind id, the id the
// kind's registry knows it by, and the words the row shows.
//
// The row is "Open in CE: <kindLabel> '<displayName>'". The kind label is singular ("Tile",
// "NPC template") because one row opens one entry, while the editor's own tabs are plural.
// A cell can produce several rows -- a tile under a surface under a character -- and the
// kind is what tells them apart, so it is always in the label.
struct ContentEditorTarget {
  std::string kind;
  std::string kindLabel;
  std::string id;
  std::string displayName;
};

namespace ContentEditorMenu {

// Draws one selectable row per target, and opens the Content Editor on the clicked one.
//
// Returns true when it drew anything, so a caller can decide whether its own "No actions
// available" line is still true. Returns false when the feature is switched off
// (ui.openInContentEditor) or the list is empty, and draws nothing in that case.
//
// `afterRows` adds a leading separator because the caller has already drawn its own
// options; pass false for a popup that contains nothing but these rows.
bool Draw(Game &game, const std::vector<ContentEditorTarget> &targets, bool afterRows);

}  // namespace ContentEditorMenu
