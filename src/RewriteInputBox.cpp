#include "RewriteInputBox.hpp"

#include <algorithm>
#include <cstring>

#include "imgui.h"

namespace {

void SetBuffer(char *buffer, size_t capacity, const std::string &text) {
  if (buffer == nullptr || capacity == 0) return;
  // Truncated rather than overflowing, and the newline is stripped: ImGui's multiline
  // box would otherwise append one on the next edit and the request would carry a
  // trailing blank line.
  const size_t count = std::min(text.size(), capacity - 1);
  std::memcpy(buffer, text.data(), count);
  buffer[count] = '\0';
}

}  // namespace

void RewriteInputBox::Setup(Game *hostGame, PromptRewriter *hostRewriter,
                            char *textBuffer, size_t bufferCapacity) {
  game = hostGame;
  rewriter = hostRewriter;
  buffer = textBuffer;
  capacity = bufferCapacity;
}

void RewriteInputBox::SetBrief(PromptRewriter::Brief brief) {
  if (rewriter != nullptr) {
    rewriter->SetBrief(std::move(brief));
  }
}

void RewriteInputBox::Drain() {
  if (rewriter == nullptr) return;
  std::optional<PromptRewriter::Result> result = rewriter->TakeResult();
  if (!result.has_value()) return;

  // Logged before anything is applied, including on failure: the reply is the only
  // record of what the model said, and a wrong half in the box is a parsing question
  // that cannot be answered without it.
  if (logSink) {
    logSink("IMPROVE asked: " + result->original + "\n");
    if (result->rawReply.empty()) {
      logSink("IMPROVE failed: " + result->error + "\n");
    } else {
      logSink("IMPROVE raw reply:\n" + result->rawReply + "\n");
    }
    if (result->ok) {
      logSink("IMPROVE split -> text (" + std::to_string(result->text.size()) +
              " chars): " + result->text + "\n");
      logSink("IMPROVE split -> why  (" + std::to_string(result->why.size()) +
              " chars): " + result->why + "\n");
    }
  }

  if (!result->ok) {
    lastError = result->error;
    return;
  }
  lastError.clear();
  ApplyRewrite(*result);
}

void RewriteInputBox::ApplyRewrite(const PromptRewriter::Result &result) {
  const std::string current = Current();
  // The safety rule: only replace text the user has not touched since the request
  // went out. Anything else would silently discard typing, which is the one thing an
  // in-place rewrite must never do.
  if (!result.original.empty() && current != result.original) {
    offeredRewrite = result.text;
    offeredReason = result.why;
    hasOffer = true;
    return;
  }

  // The pre-rewrite text is remembered only if this is the user's own text. A second
  // rewrite of an already-rewritten box must still undo back to what the user typed,
  // not to the previous machine version.
  if (!hasOriginal) {
    original = current;
    hasOriginal = true;
  }
  SetBuffer(buffer, capacity, result.text);
  lastReason = result.why;
}

void RewriteInputBox::Undo() {
  if (!hasOriginal) return;
  SetBuffer(buffer, capacity, original);
  hasOriginal = false;
  lastReason.clear();
}

std::string RewriteInputBox::Current() const {
  return buffer == nullptr ? std::string() : std::string(buffer);
}

void RewriteInputBox::Reset() {
  SetBuffer(buffer, capacity, std::string());
  original.clear();
  hasOriginal = false;
  offeredRewrite.clear();
  offeredReason.clear();
  hasOffer = false;
  lastError.clear();
  lastReason.clear();
  if (rewriter != nullptr) {
    rewriter->Clear();
  }
}

std::string RewriteInputBox::StatusLine() const {
  if (rewriter == nullptr) return {};
  if (!lastError.empty()) return lastError;
  if (rewriter->Busy()) return "improving the description...";
  if (hasOffer) return "a rewrite is ready (the text changed while it ran)";
  if (!lastReason.empty()) return lastReason;
  return {};
}

bool RewriteInputBox::DrawButtons() {
  if (rewriter == nullptr || buffer == nullptr) return false;
  bool changed = false;

  const std::string unavailable = rewriter->UnavailableReason();
  const bool busy = rewriter->Busy();
  const bool empty = Current().empty();

  ImGui::BeginDisabled(!unavailable.empty() || busy || empty);
  if (ImGui::Button("Improve")) {
    std::string reason;
    // The brief was installed by SetBrief before this frame's DrawButtons; Request
    // copies it into the prompt, so nothing needs re-setting here.
    if (!rewriter->Request(Current(), &reason)) {
      lastError = reason;
    } else {
      lastError.clear();
      // A pending offer is stale the moment a new request goes out: it describes text
      // this request no longer starts from.
      hasOffer = false;
      offeredRewrite.clear();
      offeredReason.clear();
    }
  }
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip(
        "%s",
        !unavailable.empty()
            ? unavailable.c_str()
            : empty ? "Type a description first; this improves what you wrote."
                    : busy ? "Already improving..."
                           : "Rewrite your description with more detail, keeping "
                             "what you asked for. The text stays editable, and Undo "
                             "restores your own wording.");
  }

  // Undo is only offered when there is something to go back to. A disabled Undo
  // sitting next to every rewrite is noise.
  if (hasOriginal) {
    ImGui::SameLine();
    if (ImGui::Button("Undo")) {
      Undo();
      changed = true;
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Restore your own wording.");
    }
  }

  // A rewrite that finished while the text was being edited. Offered, never forced:
  // the user's newer text is the one they meant.
  if (hasOffer) {
    ImGui::SameLine();
    if (ImGui::Button("Use rewrite")) {
      original = Current();
      hasOriginal = true;
      SetBuffer(buffer, capacity, offeredRewrite);
      lastReason = offeredReason;
      hasOffer = false;
      offeredRewrite.clear();
      offeredReason.clear();
      changed = true;
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip(
          "Apply the rewrite. You edited the text while it was being written, so it "
          "was not applied automatically.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Dismiss")) {
      hasOffer = false;
      offeredRewrite.clear();
      offeredReason.clear();
    }
  }

  return changed;
}

void RewriteInputBox::Shutdown() {
  if (rewriter != nullptr) {
    rewriter->Stop();
  }
}
