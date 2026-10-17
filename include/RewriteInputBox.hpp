#pragma once

#include <functional>
#include <string>

#include "PromptRewriter.hpp"

class Game;
class AI;
class DebugLog;

// A host window's text box, with the Improve affordance attached.
//
// This exists so the feature is not reimplemented per window. A window owns one of
// these, calls DrawButtons() to lay out the controls, and reads Current() to send.
// The rules that make an in-place rewrite trustworthy live here in one place:
//
//   * **The result replaces the text, and the original is kept.** The box remembers
//     what the user typed so Undo restores exactly that, including when they had
//     already edited a previous rewrite.
//   * **A result is never applied to text that changed while it was in flight.** The
//     rewriter returns the original it was given; if the box no longer holds that
//     text, the result is shown as available rather than forced in. Typing during a
//     rewrite is therefore safe.
//   * **The rewrite is visible.** The new text sits in the box where it can be read
//     and edited before sending, and the model's one-line reason is shown beside the
//     buttons. Nothing is sent that the user has not seen.
//
// It is deliberately not tile-specific: `RewriteInputBox` knows about a text buffer
// and a PromptRewriter, and a window supplies the brief and the backend.
class RewriteInputBox {
public:
  // `buffer` is the host's text buffer and must outlive this object -- the same
  // pointer the window passes to ImGui::InputTextMultiline, so there is exactly one
  // copy of the text and no synchronisation to get wrong.
  void Setup(Game *game, PromptRewriter *rewriter, char *buffer, size_t capacity);

  // Points the rewriter at this window's domain. Call whenever the brief changes --
  // which for these windows means once, because the brief is a property of the
  // window rather than of the text.
  void SetBrief(PromptRewriter::Brief brief);

  // Collects a finished rewrite, if one arrived. Call once per frame before
  // DrawButtons.
  void Drain();

  // The Improve / Undo buttons and the reason line, on the current row.
  //
  // Returns true when the text changed this frame (a rewrite landed or Undo
  // restored), so a host that mirrors the buffer elsewhere can react.
  bool DrawButtons();

  // The text to send: the buffer as it stands.
  std::string Current() const;

  // Clears the buffer and forgets the original -- for after a message is sent, so
  // the next request does not offer to undo back into the previous one.
  void Reset();

  // A one-line state for a status area: what the rewriter is doing, or what it last
  // said. Empty when the feature is unavailable and the host has better things to
  // say.
  std::string StatusLine() const;

  // A sink for the rewrite transcript: what was asked, what came back, and the two
  // halves it was split into.
  //
  // Supplied by the host because only the host knows where its own log is -- the
  // window writes to logs/tile-designer/, the game's DebugLog is in memory only, and
  // a rewrite that produced the wrong half is undiagnosable without a record of what
  // the model actually said.
  using LogFn = std::function<void(const std::string &)>;
  void SetLogSink(LogFn sink) { logSink = std::move(sink); }

  // True when the last rewrite landed and has not been undone, so the host can mark
  // the text as machine-written rather than the user's own.
  bool ShowingRewrite() const { return hasOriginal; }

  // Stops the rewriter's worker. Call from the host window's destructor.
  void Shutdown();

private:
  void ApplyRewrite(const PromptRewriter::Result &result);
  void Undo();

  Game *game = nullptr;
  PromptRewriter *rewriter = nullptr;
  char *buffer = nullptr;
  size_t capacity = 0;

  // What the buffer held before the current rewrite. Empty `hasOriginal` means the
  // text is the user's own.
  std::string original;
  bool hasOriginal = false;

  // Set when a rewrite finished but its original no longer matches what the user has
  // since typed. Offered rather than applied.
  std::string offeredRewrite;
  std::string offeredReason;
  bool hasOffer = false;
  // Where the host wants the rewrite transcript written, when it wants one.
  LogFn logSink;
  // Why a rewrite could not be offered, for the status line.
  std::string lastError;
  // The model's note on the last applied rewrite.
  std::string lastReason;
};
