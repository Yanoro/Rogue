#pragma once

#include <string>

class TileArtQueue;

// One panel listing the art queue's requests, drawn by every window that can start one.
//
// Why it is shared rather than duplicated. A request is made in one window (the designer's
// art section, or the Content Editor's Regenerate button) and settles in the queue, which
// belongs to neither. Two copies of the list would drift -- one would learn about a new
// state and the other would not -- and the failure this panel exists to fix is exactly
// "the request is real but no window shows it".
namespace ArtRequestsPanel {

// Draws the queue's jobs, newest first, with the counts and the shutdown/unavailable
// reason. `highlight` names the asset the caller is looking at: its row is emphasised, so
// a panel shared by both windows still answers "what about *this* one".
//
// Read-only on purpose. Cancelling or retrying a job belongs to the queue's API, and a
// panel that could start work from a button next to a row would be a second entry point
// with its own cost rules.
void Draw(const TileArtQueue &queue, const std::string &highlight);

}  // namespace ArtRequestsPanel
