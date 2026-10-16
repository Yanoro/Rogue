#include "DesignerChatWindow.h"

#include <algorithm>
#include <string_view>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <thread>
#include <utility>

#include "BlueprintExpansion.hpp"
#include "BlueprintPreview.hpp"
#include "BlueprintRegistry.h"
#include "Components.h"
#include "DebugLog.h"
#include "Game.h"
#include "Settings.h"
#include "SpriteDraw.hpp"
#include "SpritePaths.hpp"
#include "TimeUtils.hpp"
#include "imgui.h"

namespace {

// The language tag that opens the fenced block the designer must produce.
constexpr const char *kJsonFence = "```json";

// How many of the most recent messages are replayed to the model. Unbounded
// history is the one cost that grows without limit in a chat like this, and a
// design is a large object: after a handful of them every request would carry
// every blueprint ever made. The cap is generous enough that "change the door
// position" still has the previous design in view.
constexpr size_t kMaxHistoryMessages = 8;

// Strips reasoning blocks ("<think>...</think>") from a complete reply.
//
// A pure function over the whole text rather than a stateful filter over the token
// stream, and that is the point: the raw reply is what gets stored, so turning the
// thoughts back on is possible. A streaming filter is lossy -- once the reasoning
// has been dropped there is nothing left to show -- which is fine when hiding is
// mandatory and wrong when it is a checkbox.
//
// The scanner still has to be careful in the same way a stream filter is, because
// a '<' can be literal text ("a < b") and a tag can be adjacent to one ("a
// <think>x</think>b"). A '<' is only treated as a tag start when the folloing
// characters actually spell one.
inline std::string StripReasoning(const std::string &text) {
  static constexpr const char *kOpen = "<think>";
  static constexpr const char *kClose = "</think>";
  static constexpr size_t kOpenLength = 7;
  static constexpr size_t kCloseLength = 8;

  std::string out;
  out.reserve(text.size());

  size_t i = 0;
  while (i < text.size()) {
    if (text.compare(i, kOpenLength, kOpen) == 0) {
      const size_t close = text.find(kClose, i + kOpenLength);
      if (close == std::string::npos) {
        break;  // an unterminated block swallows the rest
      }
      i = close + kCloseLength;
      continue;
    }
    out += text[i++];
  }
  return out;
}

// Neither pane may be dragged to nothing: a window with an ungrabbable sliver is
// a state the user cannot get out of.
constexpr float kMinPaneWidth = 180.0f;
constexpr float kMinTranscriptWidth = 220.0f;

// The reply with its fenced json block replaced by a marker, for when the user has
// turned JSON display off.
//
// Only the *display* is filtered. The engine still parses and saves the block
// regardless, so hiding it cannot change what the designer produced.
inline std::string HideJsonBlock(const std::string &text) {
  const size_t fence = text.find(kJsonFence);
  if (fence == std::string::npos) {
    return text;
  }
  const size_t bodyStart = text.find('\n', fence);
  if (bodyStart == std::string::npos) {
    return text;
  }

  std::string out = text.substr(0, fence);
  const size_t close = text.find("```", bodyStart);
  out += close == std::string::npos ? "(blueprint hidden)"
                                    : "(blueprint hidden)" + text.substr(close + 3);

  // Collapse the blank line the removed block leaves behind, and trim the end, so
  // the transcript does not grow a gap between every reply.
  while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) {
    out.pop_back();
  }
  std::string collapsed;
  for (size_t i = 0; i < out.size(); ++i) {
    collapsed += out[i];
    if (out[i] == '\n') {
      while (i + 1 < out.size() && (out[i + 1] == '\n' || out[i + 1] == ' ')) {
        ++i;
      }
    }
  }
  return collapsed;
}

}  // namespace

DesignerChatWindow::DesignerChatWindow(Game *game, AI *designerAI,
                                       std::string modelLabel,
                                       flecs::entity owner)
    : game(game), ai(designerAI), modelLabel(std::move(modelLabel)) {
  // One context per window instance, named so it cannot collide with an NPC's
  // ("npc-<id>") or the vision window's.
  contextId = "designer-chat-" + std::to_string(reinterpret_cast<uintptr_t>(this));
  if (game != nullptr) {
    systemPrompt = game->BuildDesignerSystemPrompt();
  }
}

void DesignerChatWindow::Log(const std::string &text) {
  if (text.empty()) return;

  if (logFilePath.empty()) {
    // logs/designer/, beside the NPC transcripts but in its own directory: the
    // designer is not an NPC, and mixing its conversations in with the villagers'
    // would make the folder harder to scan.
    std::error_code ec;
    std::filesystem::create_directories("logs/designer", ec);
    if (ec) {
      // Not fatal: a session with nowhere to write still works, it is just not
      // recorded. Reported through the game's log so the absence is explicable.
      if (game != nullptr && game->debugLog) {
        game->debugLog->LogWarning(
            "Designer: could not create logs/designer: " + ec.message());
      }
      // Marks the attempt as failed so every later call re-tries rather than
      // spinning on an empty path forever.
      logFilePath = " ";
      return;
    }
    logFilePath = "logs/designer/" + TimeUtils::GetCurrentTimeString() +
                  "-designer.txt";
  }
  if (logFilePath == " ") return;

  std::ofstream out(logFilePath, std::ios::app);
  if (out.is_open()) {
    out << text;
  }
}

void DesignerChatWindow::LogEvent(const std::string &text) {
  Log("\n=== [" + TimeUtils::GetCurrentTimeString() + "] " + text + "\n");
}

std::string DesignerChatWindow::ExtractJsonBlock(const std::string &text) {
  // The first fenced json block. Deliberately tolerant about what surrounds it,
  // because models like to add a closing remark, and strict about the fence,
  // because that is the one thing the prompt is unambiguous about.
  const size_t fence = text.find(kJsonFence);
  if (fence == std::string::npos) {
    return {};
  }

  const size_t bodyStart = text.find('\n', fence);
  if (bodyStart == std::string::npos) {
    return {};
  }

  const size_t close = text.find("```", bodyStart);
  if (close == std::string::npos) {
    // An unterminated fence: take the rest. A model that forgot to close it has
    // usually still produced the object, and reporting "no JSON block" would be
    // a less useful error than whatever the parser says about the text.
    return text.substr(bodyStart + 1);
  }
  return text.substr(bodyStart + 1, close - bodyStart - 1);
}

std::string DesignerChatWindow::ProseBeforeJson(const std::string &text) {
  const size_t fence = text.find(kJsonFence);
  const std::string prose = fence == std::string::npos ? text : text.substr(0, fence);

  // Trimmed because the fence sits on its own line, so the prose otherwise ends
  // with the blank line that separated them -- which the engine would read as a
  // pause at the end of every utterance.
  const size_t first = prose.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return {};
  }
  const size_t last = prose.find_last_not_of(" \t\r\n");
  return prose.substr(first, last - first + 1);
}

SpeechQueue *DesignerChatWindow::Speech() const {
  if (game == nullptr || !game->speechQueue) {
    return nullptr;
  }
  return game->speechQueue.get();
}

void DesignerChatWindow::StopSpeech() {
  if (SpeechQueue *speech = Speech()) {
    speech->Stop();
  }
}

void DesignerChatWindow::SpeakFor(size_t index, const std::string &text) {
  SpeechQueue *speech = Speech();
  const SpeechClipId id =
      speech != nullptr ? speech->Enqueue(text) : kNoSpeechClip;

  {
    std::lock_guard<std::mutex> lock(transcriptMutex);
    if (index < exchanges.size()) {
      exchanges[index].speechId = id;
      // Kept whether or not a clip was created: it is what the bar's "Speak again"
      // needs, and the only record of what was actually read aloud.
      exchanges[index].spokenText = text;
    }
  }

  // Logged even when nothing was queued. "The engine was asked and produced no
  // clip" is exactly the fact worth having, and it is invisible otherwise.
  LogEvent(id == kNoSpeechClip ? "SPOKEN (no audio): " + text : "SPOKEN: " + text);
}

void DesignerChatWindow::DrawSpeechBar(size_t index, Exchange &ex) {
  SpeechQueue *speech = Speech();
  const bool speechAvailable = speech != nullptr && speech->HasBackend();
  const SpeechQueue::ClipState state =
      speech != nullptr ? speech->StateOf(ex.speechId)
                        : SpeechQueue::ClipState::Unavailable;

  const bool preparing = state == SpeechQueue::ClipState::Preparing;
  const bool playing = state == SpeechQueue::ClipState::Playing;
  const bool paused = state == SpeechQueue::ClipState::Paused;
  const bool hasAudio = playing || paused || state == SpeechQueue::ClipState::Ready;

  ImGui::PushID(static_cast<int>(index));

  if (!hasAudio && !preparing) {
    // No audio for this reply: speech was off or muted when it arrived, the engine
    // produced nothing, or the cap dropped it. Three different causes, one useful
    // action, so the button is the same and the tooltip explains which it was.
    const bool canSpeak =
        speechAvailable && !speech->IsMuted() && !ex.spokenText.empty();

    std::string why;
    if (state == SpeechQueue::ClipState::Failed) {
      why = "The speech engine produced no audio for this reply.";
    } else if (state == SpeechQueue::ClipState::Dropped) {
      why = "The audio for this reply was dropped to stay under the retention cap "
            "(the last " + std::to_string(SpeechQueue::kMaxRetainedClips) +
            " replies are kept).";
    } else if (!speechAvailable) {
      why = game != nullptr && !game->speechUnavailableReason.empty()
                ? game->speechUnavailableReason
                : std::string("No speech engine is available.");
    } else if (speech->IsMuted()) {
      why = "Speech is muted. Unmute to have this read aloud.";
    } else {
      why = "This reply was not spoken.";
    }

    ImGui::BeginDisabled(!canSpeak);
    if (ImGui::Button("Speak") && canSpeak) {
      ex.speechId = speech->Enqueue(ex.spokenText);
      std::lock_guard<std::mutex> lock(transcriptMutex);
      if (index < exchanges.size()) {
        exchanges[index].speechId = ex.speechId;
      }
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("%s", why.c_str());
    }

    ImGui::PopID();
    return;
  }

  // Widths are measured from the font rather than guessed, because this bar has to
  // fit the transcript pane at any splitter position. The slider takes whatever is
  // left, with a floor so it never becomes unusable at a narrow width.
  const ImGuiStyle &style = ImGui::GetStyle();
  const float spacing = style.ItemSpacing.x;
  const float buttonWidth =
      std::max(ImGui::CalcTextSize("Pause").x, ImGui::CalcTextSize("Stop").x) +
      style.FramePadding.x * 2.0f;
  const float timeWidth = ImGui::CalcTextSize("00.0 / 00.0s").x;
  const float sliderWidth =
      std::max(60.0f, ImGui::GetContentRegionAvail().x - buttonWidth * 2.0f -
                          timeWidth - spacing * 3.0f);

  const float duration = speech->Duration(ex.speechId);
  float position = speech->Position(ex.speechId);

  ImGui::BeginDisabled(preparing);

  if (ImGui::Button(playing ? "Pause" : "Play", ImVec2(buttonWidth, 0.0f))) {
    speech->Toggle(ex.speechId);
  }
  if (ImGui::IsItemHovered() && paused) {
    ImGui::SetTooltip("Resume from %.1fs.", static_cast<double>(position));
  }

  ImGui::SameLine();
  ImGui::SetNextItemWidth(sliderWidth);
  // Seeking as it is dragged. ImGui rewrites `position` from the mouse for the
  // frames the handle is held, and the queue is told each time, so the readout and
  // the audio cannot disagree about where playback is.
  if (ImGui::SliderFloat("##seek", &position, 0.0f, std::max(duration, 0.001f),
                         "")) {
    speech->Seek(ex.speechId, position);
  }
  if (ImGui::IsItemHovered() && duration > 0.0f) {
    ImGui::SetTooltip("Drag to move through the reading.");
  }

  ImGui::SameLine();
  if (preparing) {
    ImGui::TextDisabled("preparing...");
  } else {
    ImGui::Text("%.1f / %.1fs", static_cast<double>(position),
                static_cast<double>(duration));
  }

  ImGui::SameLine();
  // Stopping is only meaningful once something has been loaded, and it rewinds
  // rather than discards: the audio stays for another play.
  ImGui::BeginDisabled(!playing && !paused);
  if (ImGui::Button("Stop", ImVec2(buttonWidth, 0.0f))) {
    speech->StopClip(ex.speechId);
  }
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Stop and rewind. The reading stays available.");
  }

  ImGui::EndDisabled();
  ImGui::PopID();
}

// The reply as the engine reads it when validating: reasoning removed, so the
// model cannot talk itself past the contract, and the JSON left in place because
// that is what is being extracted.
std::string DesignerChatWindow::Exchange::ParsedAnswer() const {
  return StripReasoning(answer);
}

// The reply with only the JSON block dropped.
//
// JSON removal is deliberate rather than cosmetic: leaving it in would replay a
// whole blueprint, and its `designerNotes` string, as if the model had just
// produced it, which both wastes the largest part of the context budget and invites
// the model to echo its previous JSON instead of writing a new one.
//
// Reasoning is deliberately NOT removed here. The user's requirement is that the
// model keeps access to its own earlier thinking; hiding thoughts from the
// transcript is a display choice and has no business changing what the model sees.
std::string DesignerChatWindow::Exchange::HistoryAnswer() const {
  return HideJsonBlock(answer);
}

// What the transcript shows. Both hidden forms are still fully present in
// `answer`, which is why toggling either checkbox reveals them retroactively.
std::string DesignerChatWindow::Exchange::DisplayAnswer(bool showThoughts,
                                                        bool showJson) const {
  const std::string text = showThoughts ? answer : StripReasoning(answer);
  return showJson ? text : HideJsonBlock(text);
}

void DesignerChatWindow::SendUserMessage(const std::string &prompt) {
  if (ai == nullptr || prompt.empty() || IsBusy()) {
    return;
  }

  // Whatever was being read out is now answering a request nobody is waiting for
  // any more. Stopping here rather than letting it run is what keeps the spoken
  // reply attributable to the reply on screen.
  StopSpeech();

  // A new request restarts the attempt budget: the cap is per request, not per
  // session, or the third design of a session could never be retried.
  attempts = 0;

  Exchange ex;
  ex.prompt = prompt;
  ex.fromUser = true;

  size_t index = 0;
  {
    std::lock_guard<std::mutex> lock(transcriptMutex);
    exchanges.push_back(std::move(ex));
    index = exchanges.size() - 1;
  }

  // Logged before the request goes out, so a send that never gets a reply still
  // leaves a record of what was asked.
  LogEvent("USER REQUEST (attempt " + std::to_string(attempts + 1) +
           " of this request)");
  Log(prompt);
  Log("\n");

  GenerateResponse(index, prompt);
}

void DesignerChatWindow::GenerateResponse(size_t index, std::string prompt) {
  AI *backend = ai;
  if (backend == nullptr) {
    return;
  }

  // Snapshot the history on the main thread. The worker must not read `history`
  // while a later turn mutates it, and the request it is answering has to be
  // built from the conversation as it stood when the request was made.
  std::vector<ChatMessage> request;
  if (!systemPrompt.empty()) {
    request.push_back({"system", systemPrompt});
  }
  const size_t firstKept =
      history.size() > kMaxHistoryMessages ? history.size() - kMaxHistoryMessages : 0;
  for (size_t i = firstKept; i < history.size(); ++i) {
    request.push_back(history[i]);
  }
  request.push_back({"user", prompt});

  // The worker keeps the game loop responsive for providers whose generateStream
  // blocks. OllamaAI's does not -- it returns immediately and streams from its own
  // thread -- which is why completion is signalled by the empty-token sentinel and
  // never by this function returning.
  //
  // Nothing is written to the transcript from here: the stream callback runs on a
  // provider thread and fires once per token, so logging there would mean a file
  // open per token. The whole reply is written once, by InspectFinishedExchange on
  // the main thread.
  std::thread([this, index, request = std::move(request), backend]() {
    // A fresh provider-side context per request, because the history is sent
    // explicitly: leaving the previous request's tokens in place would replay the
    // conversation twice.
    backend->resetContext(contextId);

    std::string failure;
    const bool delivered = backend->generateStream(
        contextId, request,
        [this, index](const std::string &token) {
          std::lock_guard<std::mutex> lock(transcriptMutex);
          if (index >= exchanges.size()) {
            return;
          }
          Exchange &ex = exchanges[index];
          if (!token.empty()) {
            // Stored verbatim. Reasoning is stripped when the exchange is
            // inspected, and again at display time, so the checkbox can put it back
            // -- which a filter applied here could not.
            ex.answer += token;
            return;
          }
          // Empty token: every provider uses it as the end-of-stream sentinel.
          ex.done = true;
          if (ex.answer.empty()) {
            ex.failed = true;
            ex.error =
                "No answer came back. Check that the backend is running and "
                "that the model is available.";
          }
        });
    // generateStream's remaining parameters (a stop token and an image list) have
    // defaults, and the designer is text-only, so neither is passed.

    if (!delivered) {
      failure = "The designer backend could not accept the request.";
    }

    // NOTE the absence of a "stream finished" branch here. OllamaAI's
    // generateStream dispatches its own thread and returns immediately
    // (OllamaAI.cpp: `std::thread([...]{ curl_multi_perform ... }).detach()`), so
    // returning means only "the request was accepted". Treating the return as
    // completion marks the exchange done while the answer is still empty, which
    // raises the bogus "stream ended without a reply" error, inspects a
    // half-received reply, and -- worst -- starts a retry that calls
    // resetContext on a context whose first stream is still in flight.
    //
    // Completion is the provider's end-of-stream sentinel and nothing else, which
    // is what the callback above sets. This mirrors VisionChatWindow, which is
    // the proven implementation of the same handoff.
    if (!failure.empty()) {
      std::lock_guard<std::mutex> lock(transcriptMutex);
      if (index < exchanges.size()) {
        Exchange &ex = exchanges[index];
        ex.done = true;
        ex.failed = true;
        ex.error = failure;
      }
    }
  }).detach();
}

void DesignerChatWindow::DrawPreviewPane() {
  if (!lastDesign.has_value()) {
    ImGui::TextWrapped(
        "The design from the latest reply appears here. A reply is drawn whether "
        "or not it passed validation -- a refused design is the one most worth "
        "looking at.");
    return;
  }

  const PreviewedDesign &design = *lastDesign;
  const Blueprint &blueprint = design.blueprint;

  // The verdict banner is the one part not shared with the Blueprints window: it
  // is about this conversation's reply, not about the design as content.
  if (!design.accepted) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.4f, 1.0f));
    ImGui::TextWrapped("REFUSED");
    ImGui::PopStyleColor();
    if (!design.refusal.empty()) {
      ImGui::TextWrapped("%s", design.refusal.c_str());
    }
  } else {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 1.0f, 0.4f, 1.0f));
    ImGui::TextWrapped("SAVED");
    ImGui::PopStyleColor();
  }
  ImGui::Text("%s", blueprint.name.empty() ? blueprint.id.c_str()
                                           : blueprint.name.c_str());
  ImGui::TextDisabled("%s | %d x %d | %zu cells", blueprint.id.c_str(),
                      blueprint.Width(), blueprint.Height(),
                      design.expansion.cells.size());
  ImGui::Separator();

  BlueprintPreview::DrawZoomControl("##designer_zoom", zoom);

  const float canvasHeight =
      std::max(120.0f, ImGui::GetContentRegionAvail().y * 0.55f);
  if (game != nullptr) {
    BlueprintPreview::DrawCanvas("designer_preview_canvas", blueprint,
                                 design.expansion, zoom, game->tileRegistry,
                                 game->resourceManager, canvasHeight);
    BlueprintPreview::DrawLegend(design.expansion, game->tileRegistry);
  }

  // The designer's own critique, which is the reason this pane sits next to the
  // transcript rather than behind a button.
  if (!design.notes.empty()) {
    if (ImGui::TreeNodeEx("Designer's notes", ImGuiTreeNodeFlags_DefaultOpen)) {
      ImGui::TextWrapped("%s", design.notes.c_str());
      ImGui::TreePop();
    }
  }
}

bool DesignerChatWindow::IsBusy() {
  std::lock_guard<std::mutex> lock(transcriptMutex);
  for (const Exchange &ex : exchanges) {
    if (!ex.done) {
      return true;
    }
  }
  return false;
}

void DesignerChatWindow::InspectFinishedExchange(size_t index) {
  Exchange finished;
  {
    std::lock_guard<std::mutex> lock(transcriptMutex);
    if (index >= exchanges.size()) return;
    finished = exchanges[index];
  }

  ++requestNumber;

  // The model's reply verbatim, including whatever prose surrounds the fence. This
  // is the thing a bad parse has to be diagnosed against, so it is recorded
  // before anything is extracted from it.
  LogEvent("MODEL REPLY (request " + std::to_string(requestNumber) + ", " +
           std::to_string(finished.answer.size()) + " bytes" +
           (finished.fromUser ? ", answers a user request" : ", engine retry") + ")");
  Log(finished.answer);
  Log("\n");

  if (finished.failed) {
    LogEvent("TRANSPORT FAILURE: " + finished.error);
    // Spoken as well as logged: a backend that died is exactly the thing worth
    // hearing while looking somewhere else, and it is one short sentence.
    SpeakFor(index, "The designer backend failed. " + finished.error);
    std::lock_guard<std::mutex> lock(transcriptMutex);
    if (index < exchanges.size()) exchanges[index].inspected = true;
    history.push_back({"user", finished.prompt});
    return;
  }

  std::string reason;
  bool ok = false;
  std::string designId;
  std::string notes;
  std::string savedPath;
  bool overwrote = false;
  // Filled when the design got far enough to be parsed and expanded, whether or
  // not it then passed. Held out here so the pane can show a refused design too.
  bool haveDesign = false;
  Blueprint parsedDesign;
  BlueprintExpansion::Expansion parsedExpansion;

  const std::string block = ExtractJsonBlock(finished.ParsedAnswer());
  if (block.empty()) {
    reason =
        "the reply had no ```json block. The blueprint must be inside one fenced "
        "block tagged json.";
  } else {
    nlohmann::json parsed;
    try {
      parsed = nlohmann::json::parse(block);
    } catch (const std::exception &e) {
      reason = std::string("the json block is not valid JSON: ") + e.what();
    }

    if (reason.empty()) {
      if (!parsed.is_object()) {
        reason = "the json block must be a JSON object describing one blueprint.";
      } else if (!parsed.contains("designerNotes") ||
                 !parsed["designerNotes"].is_string() ||
                 parsed["designerNotes"].get<std::string>().empty()) {
        // Required by the contract, and the reason it is required is the whole
        // point of this window: the model's own judgement of its design.
        reason =
            "the blueprint has no non-empty \"designerNotes\" string. Every design "
            "must carry your own thoughts on it.";
      } else {
        notes = parsed["designerNotes"].get<std::string>();
        // Removed before saving: it is not part of the schema, so writing it back
        // would put a designer-only field into content the rest of the engine
        // parses.
        parsed.erase("designerNotes");

        const std::string id = parsed.value("id", std::string());
        if (id.empty()) {
          reason = "the blueprint has no \"id\".";
        } else {
          BlueprintRegistry *registry = nullptr;
          if (auto *res = game != nullptr ? game->ecs.get<BlueprintRegistryResource>()
                                          : nullptr) {
            registry = res->registry;
          }

          std::string parseError;
          std::vector<std::string> warnings;
          BlueprintRegistry::ParseBlueprint(parsed, id, "(designer)", parsedDesign,
                                            parseError, warnings);

          // Expanded even when parsing failed only enough to set an error: a
          // partial design is still worth drawing. The pane is a debugging aid,
          // so it prefers showing something over showing nothing.
          parsedExpansion = BlueprintExpansion::Expand(parsedDesign);
          haveDesign = !parsedDesign.parts.empty();

          if (!parseError.empty()) {
            reason = parseError;
          } else {
            // Expansion is checked before saving, not after: a design that parses
            // but produces no cells would otherwise be written to disk and appear
            // in the list as an empty building.
            if (!parsedExpansion.errors.empty()) {
              reason = parsedExpansion.errors.front();
              if (parsedExpansion.errors.size() > 1) {
                reason += " (and " + std::to_string(parsedExpansion.errors.size() - 1) +
                          " more problem(s))";
              }
            } else if (parsedExpansion.cells.empty()) {
              reason = "the design expands to no cells at all.";
            } else if (registry == nullptr) {
              reason = "there is no blueprint registry to save to.";
            } else {
              const BlueprintRegistry::SaveResult saved =
                  registry->SaveBlueprint(parsedDesign);
              if (!saved.ok) {
                reason = saved.error;
              } else {
                ok = true;
                designId = parsedDesign.id;
                savedPath = saved.path;
                overwrote = saved.overwrote;
              }
            }
          }
        }
      }
    }
  }

  // The pane shows the latest *attempt*, so this is updated even on refusal -- and
  // deliberately not cleared when a reply carries no design at all, so a failed
  // attempt does not blank the last thing you were looking at.
  if (haveDesign) {
    PreviewedDesign preview;
    preview.blueprint = std::move(parsedDesign);
    preview.expansion = std::move(parsedExpansion);
    preview.notes = notes;
    preview.accepted = ok;
    preview.refusal = ok ? std::string() : reason;
    lastDesign = std::move(preview);
  }

  {
    std::lock_guard<std::mutex> lock(transcriptMutex);
    if (index < exchanges.size()) {
      Exchange &ex = exchanges[index];
      ex.inspected = true;
      ex.designOk = ok;
      ex.designId = designId;
      ex.savedPath = savedPath;
      ex.overwrote = overwrote;
      ex.designerNotes = notes;
      ex.failureReason = reason;
    }
  }

  // The conversation both sides saw, so a follow-up can refer to this design. The
  // assistant turn is stored as the raw reply, and the capped replay is what keeps
  // a large blueprint from accumulating.
  history.push_back({"user", finished.prompt});
  history.push_back({"assistant", finished.HistoryAnswer()});

  if (ok) {
    LogEvent("SAVED: " + designId + " -> " + savedPath +
             (overwrote ? " (replaced an existing design)" : " (new design)"));
    if (!notes.empty()) {
      Log("designerNotes: " + notes + "\n");
    }
    if (game != nullptr && game->debugLog) {
      game->debugLog->LogInfo("Designer: saved blueprint '" + designId + "' to " +
                              savedPath + (overwrote ? " (replaced)" : ""));
    }

    // Read aloud only on the attempt that landed. Speaking every stream would mean
    // reading three blueprints and two refusals, interleaved, whenever the model
    // needed a retry.
    //
    // The verdict leads because it is the part that is missing from the ear: the
    // preview pane already shows *that* the design was accepted, but the name it
    // was saved under only exists in the transcript.
    //
    // The blueprint JSON is never spoken under any setting except "everything",
    // which exists to debug this pipeline: it is the majority of the reply's bytes
    // and it is not language, so it reads as "id two bedroom house, name two
    // bedroom house, rect_fill, rect_outline".
    const std::string &content = Settings().designer.tts.content;
    std::string spoken = "Saved as " + designId + ".";
    if (content == "everything") {
      spoken += " " + finished.ParsedAnswer();
    } else {
      if (content == "prose_and_notes") {
        const std::string prose = ProseBeforeJson(finished.ParsedAnswer());
        if (!prose.empty()) {
          spoken += " " + prose;
        }
      }
      // The notes are the reason the window exists, so they are the default
      // payload: the model's own critique of what it just made.
      if (!notes.empty()) {
        spoken += " " + notes;
      }
    }
    SpeakFor(index, spoken);

    attempts = 0;
    return;
  }

  // Refused: say why, in the same conversation, and try again. Bounded so a model
  // that cannot satisfy the contract cannot spend the whole budget.
  LogEvent("REFUSED: " + reason);
  ++attempts;
  const int maxAttempts = game != nullptr ? game->maxDesignAttempts() : 3;
  if (attempts >= maxAttempts) {
    LogEvent("GIVING UP after " + std::to_string(attempts) + " attempt(s)");
    if (game != nullptr && game->debugLog) {
      game->debugLog->LogWarning("Designer: giving up after " +
                                 std::to_string(attempts) +
                                 " attempt(s); last reason: " + reason);
    }
    // The one refusal that is spoken, because it is the last thing that will be
    // said about this request. Intermediate refusals stay silent -- they are
    // superseded seconds later -- but this sentence is short, actionable, and
    // lands while you are still looking at the design that failed.
    SpeakFor(index, "Giving up after " + std::to_string(attempts) + " attempts. " + reason);
    return;
  }

  LogEvent("RETRYING (" + std::to_string(attempts) + " of " +
           std::to_string(maxAttempts) + " used)");

  Exchange retry;
  retry.prompt =
      "That design was refused: " + reason +
      "\n\nSend the corrected blueprint as a complete ```json block, with "
      "designerNotes. Do not describe the fix in prose instead of sending it.";
  retry.fromUser = false;

  size_t retryIndex = 0;
  {
    std::lock_guard<std::mutex> lock(transcriptMutex);
    exchanges.push_back(std::move(retry));
    retryIndex = exchanges.size() - 1;
  }

  // The engine's own prompt is logged too, not just the user's: diagnosing a retry
  // loop means seeing the exact reason the model was handed.
  Log(retry.prompt);
  Log("\n");

  GenerateResponse(retryIndex, exchanges[retryIndex].prompt);
}

void DesignerChatWindow::Draw() {
  bool isOpen = true;
  ImGui::SetNextWindowSize(ImVec2(760, 560), ImGuiCond_FirstUseEver);
  ImGui::Begin("Designer", &isOpen, ImGuiWindowFlags_None);
  if (!isOpen) {
    game->designerWindowEntity.remove<ActiveWindow>();
    ImGui::End();
    return;
  }

  // Resolved once: three separate places in this window need to know whether a
  // speech engine exists at all, and each of them has to behave differently when
  // it does not.
  SpeechQueue *speech = Speech();
  const bool speechAvailable = speech != nullptr && speech->HasBackend();

  ImGui::TextDisabled("Backend: %s", modelLabel.empty() ? "(none)" : modelLabel.c_str());
  ImGui::SameLine();
  ImGui::TextDisabled("| %d of %d attempts used", attempts,
                      game != nullptr ? game->maxDesignAttempts() : 0);
  ImGui::SameLine();
  // Which voice is speaking, and whether it is muted. Shown rather than hidden
  // behind a tooltip because "why is it reading in that accent" and "why is it
  // silent" are both answered here.
  if (speechAvailable) {
    ImGui::TextDisabled("| speech: %s%s", speech->StatusLine().c_str(),
                        speech->IsMuted() ? " (muted)" : "");
  } else {
    ImGui::TextDisabled("| speech: off");
  }
  // A speech engine that exists but cannot be heard is the failure mode worth
  // spelling out: raylib reports a healthy device even when it has fallen back to
  // a null one, so "speech: on" would be a lie.
  if (speechAvailable && !speech->AudioWarning().empty()) {
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "SILENT");
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("%s", speech->AudioWarning().c_str());
    }
  }
  if (!speechAvailable && game != nullptr &&
      !game->speechUnavailableReason.empty()) {
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "(?)");
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("%s", game->speechUnavailableReason.c_str());
    }
  }
  ImGui::SameLine();
  // Checkmarks rather than buttons: both are standing preferences about how the
  // transcript reads, not actions.
  //
  // Neither affects what the engine does. The reply is stored raw, so turning
  // reasoning on reveals it in replies already received; the JSON is parsed and
  // saved whichever way these are set.
  ImGui::Checkbox("Show thinking", &showThoughts);
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip(
        "Show the model's <think> reasoning in the transcript.\n"
        "It is always received and always written to the log -- this only decides "
        "whether you read it.");
  }
  ImGui::SameLine();
  ImGui::Checkbox("Show JSON", &showJson);
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip(
        "Show the blueprint JSON in the transcript.\n"
        "It is parsed and saved either way -- this only changes what you read.");
  }
  ImGui::SameLine();
  // A button rather than a checkbox, because muting is not only a preference: it
  // has to silence what is being said right now as well as what comes next, and a
  // checkbox does not imply that.
  //
  // Disabled with an explanation when there is no engine at all. A control that
  // does nothing and says nothing is the worst version of this.
  ImGui::BeginDisabled(!speechAvailable);
  const bool muted = speechAvailable && speech->IsMuted();
  if (ImGui::Button(muted ? "Unmute" : "Mute")) {
    speech->SetMuted(!muted);
  }
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip(
        speechAvailable
            ? "Stop reading replies aloud, and silence the one being read now.\n"
              "The launch default comes from designer.tts in settings.json."
            : "No speech engine. Set designer.tts.enabled in settings.json and "
              "download a voice model -- see SETTINGS.md.");
  }
  ImGui::Separator();

  if (ai == nullptr) {
    ImGui::TextWrapped(
        "No designer backend is configured, so nothing can be sent. Set "
        "\"designer\" in settings.json (see SETTINGS.md) or leave "
        "\"useMainModel\" on to share the NPC backend.");
    ImGui::End();
    return;
  }

  // Read the persisted layout on the first frame rather than in the constructor:
  // the state file is loaded during Init and the window is built as part of it, so
  // depending on that order here would be a silent way to lose the setting.
  if (!layoutLoaded && game != nullptr && game->debugWindowState) {
    paneWidth = game->debugWindowState->GetDesignerPaneWidth();
    zoom = game->debugWindowState->GetDesignerZoom();
    showJson = game->debugWindowState->GetDesignerShowJson();
    showThoughts = game->debugWindowState->GetDesignerShowThoughts();
    layoutLoaded = true;
  }

  if (ImGui::Button("Load existing design")) {
    // Copies the currently selected design in the Blueprints window into the
    // prompt, which is how "change this one" is asked without pasting JSON by
    // hand.
    BlueprintRegistry *registry = nullptr;
    if (auto *res = game->ecs.get<BlueprintRegistryResource>()) {
      registry = res->registry;
    }
    const Blueprint *selected =
        registry != nullptr && !game->selectedBlueprintId.empty()
            ? registry->Get(game->selectedBlueprintId)
            : nullptr;
    if (selected != nullptr) {
      const std::string json =
          BlueprintRegistry::WriteBlueprint(*selected).dump(2);
      const std::string message =
          "Here is my current " + selected->name + ":\n\n```json\n" + json +
          "\n```";
      std::snprintf(inputBuffer, sizeof(inputBuffer), "%s",
                    message.substr(0, sizeof(inputBuffer) - 1).c_str());
    }
  }
  ImGui::SameLine();
  ImGui::TextDisabled(
      "(select a design in the Blueprints window first)");

  // Left: the design from the latest attempt, drawn as the world would draw it.
  // Right: the conversation, with a draggable splitter between them.
  //
  // Clamped every frame rather than only when dragged, so a pane width read from a
  // hand-edited state file, or left over from a larger window, cannot produce a
  // layout with an ungrabbable pane.
  const float available = ImGui::GetContentRegionAvail().x;
  const float maxPaneWidth =
      std::max(kMinPaneWidth, available - kMinTranscriptWidth - 8.0f);
  paneWidth = std::clamp(paneWidth, kMinPaneWidth, maxPaneWidth);
  zoom = std::clamp(zoom, BlueprintPreview::kMinZoom, BlueprintPreview::kMaxZoom);

  ImGui::BeginChild("designer_preview_pane", ImVec2(paneWidth, -70), true);
  DrawPreviewPane();
  ImGui::EndChild();

  // The splitter. Built from an invisible button rather than a widget with its own
  // look, so the only thing drawn is the line below -- ImGui has no splitter
  // control, and this is the idiomatic way to make one.
  //
  // The pane's left edge is captured BEFORE the button, because that is the
  // reference the new width is measured from. (Captured after, it would be the
  // splitter's own position, and the width would be computed against the wrong
  // origin.)
  ImGui::SameLine(0.0f, 0.0f);
  const float paneLeft = ImGui::GetCursorScreenPos().x;
  ImGui::InvisibleButton("designer_splitter", ImVec2(8.0f, -70));
  const bool splitterHovered = ImGui::IsItemHovered();
  const bool splitterActive = ImGui::IsItemActive();
  if (splitterHovered || splitterActive) {
    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
  }
  if (splitterActive) {
    // Measured from the mouse's absolute position rather than from accumulated
    // drag deltas, so the splitter stays under the cursor and cannot drift when
    // the clamp stops it or the window is resized mid-drag.
    paneWidth = ImGui::GetIO().MousePos.x - paneLeft;
  }
  {
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    const float centerX = (min.x + max.x) * 0.5f;
    const ImU32 color = splitterActive ? IM_COL32(255, 170, 80, 255)
                        : splitterHovered ? IM_COL32(200, 200, 200, 255)
                                          : IM_COL32(90, 90, 90, 255);
    ImGui::GetWindowDrawList()->AddLine(ImVec2(centerX, min.y),
                                        ImVec2(centerX, max.y), color,
                                        splitterActive ? 3.0f : 1.0f);
  }

  ImGui::SameLine(0.0f, 0.0f);

  // The transcript. Drawn from a copy taken under the lock, so the worker
  // appending tokens cannot race the render pass.
  ImGui::BeginChild("designer_transcript", ImVec2(0, -70), true);
  std::vector<Exchange> snapshot;
  {
    std::lock_guard<std::mutex> lock(transcriptMutex);
    snapshot = exchanges;
  }

  if (snapshot.empty()) {
    ImGui::TextWrapped(
        "Ask for a building. The designer replies with a blueprint and its own "
        "notes on it; a valid design is saved into data/blueprints/ and appears "
        "in the Blueprints window straight away.");
  }

  bool needsInspection = false;
  for (size_t i = 0; i < snapshot.size(); ++i) {
    // Non-const: DrawSpeechBar writes a fresh handle back when it re-speaks a reply.
    // The snapshot is this frame's copy of the transcript, and the write-through to
    // `exchanges` happens under the lock inside the bar.
    Exchange &ex = snapshot[i];
    if (!ex.inspected && ex.done) {
      needsInspection = true;
    }

    if (ex.fromUser && !ex.prompt.empty()) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.8f, 1.0f, 1.0f));
      ImGui::TextWrapped("You: %s", ex.prompt.c_str());
      ImGui::PopStyleColor();
    } else if (!ex.prompt.empty()) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.5f, 1.0f));
      ImGui::TextWrapped("Retry (%d): %s", static_cast<int>(i), ex.prompt.c_str());
      ImGui::PopStyleColor();
    }

    if (!ex.answer.empty()) {
      // Tinted while reasoning is shown, so it is obvious at a glance that this
      // part is the model working rather than its answer.
      const std::string shown = ex.DisplayAnswer(showThoughts, showJson);
      if (showThoughts && shown.find("<think>") != std::string::npos) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.62f, 0.66f, 0.72f, 1.0f));
        ImGui::TextWrapped("%s", shown.c_str());
        ImGui::PopStyleColor();
      } else {
        ImGui::TextWrapped("%s", shown.c_str());
      }
    }
    if (!ex.done) {
      ImGui::TextDisabled("...");
    }

    if (ex.inspected) {
      if (ex.designOk) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 1.0f, 0.4f, 1.0f));
        ImGui::TextWrapped("Saved '%s' to %s%s", ex.designId.c_str(),
                           ex.savedPath.c_str(),
                           ex.overwrote ? " (replaced an existing design)" : "");
        ImGui::PopStyleColor();
        if (!ex.designerNotes.empty()) {
          ImGui::TextDisabled("Designer's notes:");
          ImGui::TextWrapped("%s", ex.designerNotes.c_str());
        }
      } else if (!ex.failureReason.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.4f, 1.0f));
        ImGui::TextWrapped("Refused: %s", ex.failureReason.c_str());
        ImGui::PopStyleColor();
      }
    }
    if (ex.failed && !ex.error.empty()) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
      ImGui::TextWrapped("Error: %s", ex.error.c_str());
      ImGui::PopStyleColor();
    }

    // The transport bar, directly under the reply it belongs to.
    //
    // Only for replies that were actually spoken: an intermediate refusal that was
    // about to be retried never reached the engine, and a bar under it would be a
    // control for something that never happened. The user's own messages get no bar
    // either. `spokenText` is set by SpeakFor, which is what marks a reply as spoken
    // -- including the case where it was handed over but no audio came back.
    if (!ex.fromUser && !ex.spokenText.empty()) {
      DrawSpeechBar(i, ex);
    }

    ImGui::Separator();
  }
  ImGui::EndChild();

  // Inspection runs on the main thread, after the transcript is drawn, and starts
  // any retry itself. Doing it on the worker would mean validating and saving
  // content from a background thread while the ECS is mid-frame.
  if (needsInspection) {
    for (size_t i = 0; i < snapshot.size(); ++i) {
      if (!snapshot[i].inspected && snapshot[i].done) {
        InspectFinishedExchange(i);
      }
    }
  }

  const bool busy = IsBusy();
  ImGui::BeginDisabled(busy);
  const bool send = ImGui::Button("Send");
  ImGui::EndDisabled();
  ImGui::SameLine();
  // No global "stop speech" button here any more: every reply carries its own bar,
  // so a reading is stopped where it is, and a second panic button for the same
  // thing would only make people wonder which one to press.
  ImGui::TextDisabled("Enter to send, Shift+Enter for a newline");

  ImGui::SetNextItemWidth(-1.0f);
  const bool submitted = ImGui::InputTextMultiline(
      "##designer_input", inputBuffer, sizeof(inputBuffer), ImVec2(-1.0f, 54.0f),
      ImGuiInputTextFlags_EnterReturnsTrue |
          ImGuiInputTextFlags_CtrlEnterForNewLine);

  if ((send || submitted) && !busy) {
    std::string prompt(inputBuffer);
    // Trim the whitespace an Enter press leaves behind.
    while (!prompt.empty() && (prompt.back() == '\n' || prompt.back() == ' ')) {
      prompt.pop_back();
    }
    if (!prompt.empty()) {
      inputBuffer[0] = '\0';
      SendUserMessage(prompt);
    }
  }

  // Written back every frame rather than on shutdown: the value is already
  // clamped here, so what is stored is a layout that is known to be usable, and a
  // crash costs at most the last drag instead of the whole session's layout.
  if (layoutLoaded && game != nullptr && game->debugWindowState) {
    game->debugWindowState->SetDesignerPaneWidth(paneWidth);
    game->debugWindowState->SetDesignerZoom(zoom);
    game->debugWindowState->SetDesignerShowJson(showJson);
    game->debugWindowState->SetDesignerShowThoughts(showThoughts);
  }

  ImGui::End();
}
