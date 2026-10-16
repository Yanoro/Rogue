#include "TileDesignerWindow.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <utility>

#include "Components.h"
#include "DebugLog.h"
#include "Game.h"
#include "Settings.h"
#include "SpritePaths.hpp"
#include "TileArtQueue.hpp"
#include "TimeUtils.hpp"
#include "imgui.h"

namespace {

constexpr const char *kJsonFence = "```json";

// How many of the most recent messages are replayed to the model. A tile is a
// small object, but a session is long: unbounded history is the one cost that
// grows without limit here, and the cap is generous enough that "make it
// darker" still has the previous tile in view.
constexpr size_t kMaxHistoryMessages = 8;

// The reply with its fenced json block replaced by a marker, for when the user
// has turned JSON display off. Display only: the block is still parsed and
// applied, so hiding it cannot change what the designer produced.
std::string HideJsonBlock(const std::string &text) {
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
  out += close == std::string::npos ? "(tile hidden)" + std::string()
                                    : "(tile hidden)" + text.substr(close + 3);

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

// A coloured plate showing what a tile looks like: the background as a filled
// square with the glyph centred on it in the character colour. Local because
// DebugWindows.cpp keeps its own copy in an anonymous namespace and a shared
// "draw a tile" helper is a bigger seam than two callers justify.
void DrawGlyphPlate(char glyph, Color characterColor, Color backgroundColor,
                    float size) {
  ImDrawList *drawList = ImGui::GetWindowDrawList();
  const ImVec2 topLeft = ImGui::GetCursorScreenPos();
  const ImVec2 bottomRight(topLeft.x + size, topLeft.y + size);

  drawList->AddRectFilled(topLeft, bottomRight,
                          IM_COL32(backgroundColor.r, backgroundColor.g,
                                   backgroundColor.b, backgroundColor.a));
  drawList->AddRect(topLeft, bottomRight, IM_COL32(140, 140, 140, 255));

  const char text[2] = {glyph, '\0'};
  const ImVec2 glyphSize = ImGui::CalcTextSize(text);
  drawList->AddText(ImVec2(topLeft.x + (size - glyphSize.x) * 0.5f,
                           topLeft.y + (size - glyphSize.y) * 0.5f),
                    IM_COL32(characterColor.r, characterColor.g,
                             characterColor.b, characterColor.a),
                    text);

  ImGui::Dummy(ImVec2(size, size));
}

// Three or four channel sliders, 0-255, written straight into `channels`.
// `label` heads the row. Returns true when any channel moved this frame.
bool ColorEditorRow(const char *id, const char *label, int *channels, int count) {
  bool changed = false;
  ImGui::TextUnformatted(label);
  ImGui::PushID(id);
  for (int i = 0; i < count; ++i) {
    static const char *kNames[4] = {"red", "green", "blue", "alpha"};
    ImGui::PushID(i);
    ImGui::SetNextItemWidth(64.0f);
    if (ImGui::SliderInt("##channel", &channels[i], 0, 255)) {
      changed = true;
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("%s, 0-255", kNames[i]);
    }
    ImGui::PopID();
    if (i + 1 < count) ImGui::SameLine();
  }
  ImGui::PopID();
  return changed;
}

}  // namespace

TileDesignerWindow::TileDesignerWindow(Game *game, AI *designerAI,
                                       std::string modelLabel, flecs::entity owner)
    : game(game), ai(designerAI), modelLabel(std::move(modelLabel)) {
  (void)owner;  // Windows are not ECS components yet; see Window.h.
  // One context per window instance, named so it cannot collide with an NPC's
  // ("npc-<id>"), the blueprint designer's or the vision window's.
  contextId = "tile-designer-" + std::to_string(reinterpret_cast<uintptr_t>(this));
  if (game != nullptr) {
    systemPrompt = game->BuildTileDesignerSystemPrompt();
  }
}

std::string TileDesignerWindow::Exchange::ParsedAnswer() const {
  return TileDesigner::StripReasoning(answer);
}

std::string TileDesignerWindow::Exchange::HistoryAnswer() const {
  return HideJsonBlock(answer);
}

std::string TileDesignerWindow::Exchange::DisplayAnswer(bool showThoughts,
                                                        bool showJson) const {
  const std::string text =
      showThoughts ? answer : TileDesigner::StripReasoning(answer);
  return showJson ? text : HideJsonBlock(text);
}

void TileDesignerWindow::Log(const std::string &text) {
  if (text.empty()) return;

  if (logFilePath.empty()) {
    // logs/tile-designer/, beside the blueprint designer's but in its own
    // directory: the two conversations are about different vocabularies, and
    // mixing them would make the folder harder to scan.
    std::error_code ec;
    std::filesystem::create_directories("logs/tile-designer", ec);
    if (ec) {
      if (game != nullptr && game->debugLog) {
        game->debugLog->LogWarning(
            "Tile designer: could not create logs/tile-designer: " + ec.message());
      }
      logFilePath = " ";
      return;
    }
    logFilePath = "logs/tile-designer/" + TimeUtils::GetCurrentTimeString() +
                  "-tile-designer.txt";
  }
  if (logFilePath == " ") return;

  std::ofstream out(logFilePath, std::ios::app);
  if (out.is_open()) {
    out << text;
  }
}

void TileDesignerWindow::LogEvent(const std::string &text) {
  Log("\n=== [" + TimeUtils::GetCurrentTimeString() + "] " + text + "\n");
}

std::string TileDesignerWindow::OverlayDirectory() const {
  return Settings().content.generatedTiles;
}

void TileDesignerWindow::LoadEditorFrom(const TileDef &definition) {
  // The definition is reachable as mutable (it is a runtime-added entry this
  // window owns the editing of), but it arrives here as a const TileDef* from
  // AddRuntime's result, so the constness is dropped at this one boundary rather
  // than threaded through the callers.
  edited = const_cast<TileDef *>(&definition);

  // The glyph buffer is a plain char array so ImGui can edit it in place. A
  // definition with no character gets an empty box rather than a mystery
  // placeholder, so the user can see that the field is unset.
  if (definition.hasCharacter) {
    glyphBuffer[0] = definition.character;
    glyphBuffer[1] = '\0';
  } else {
    glyphBuffer[0] = '\0';
  }

  const Color fg = definition.hasCharacterColor ? definition.characterColor
                                                : Color{255, 255, 255, 255};
  foreground[0] = fg.r;
  foreground[1] = fg.g;
  foreground[2] = fg.b;

  const Color bg = definition.hasBackgroundColor ? definition.backgroundColor
                                                 : Color{0, 0, 0, 0};
  background[0] = bg.r;
  background[1] = bg.g;
  background[2] = bg.b;
  background[3] = bg.a;

  editorBlocks = definition.blocksTile;
  editedIsDirty = false;
  editedIsRuntime = false;
  if (game != nullptr) {
    editedIsRuntime = game->tileRegistry.IsRuntimeAdded(definition.name);
  }
}

std::string TileDesignerWindow::SaveEditedTile() {
  if (edited == nullptr) {
    return "there is no tile to save.";
  }
  if (!editedIsRuntime) {
    return "this tile came from the hand-authored catalog, so the overlay may "
           "not rewrite it. Ask for a new tile instead.";
  }
  const std::string directory = OverlayDirectory();
  if (directory.empty()) {
    return "no generated overlay directory is configured "
           "(content.generatedTiles), so there is nowhere to save it.";
  }

  const nlohmann::json json = TileDesigner::WriteTileDef(*edited);
  std::error_code ec;
  std::filesystem::create_directories(directory, ec);
  if (ec) {
    return "could not create " + directory + ": " + ec.message();
  }
  const std::filesystem::path path =
      std::filesystem::path(directory) /
      (TileDesigner::SlugifyFileName(edited->name) + ".json");
  std::ofstream out(path);
  if (!out.is_open()) {
    return "could not open " + path.string() + " for writing.";
  }
  out << json.dump(2) << "\n";
  if (!out.good()) {
    return "writing " + path.string() + " failed.";
  }
  return {};
}

void TileDesignerWindow::SendUserMessage(const std::string &prompt) {
  if (ai == nullptr || prompt.empty() || IsBusy()) {
    return;
  }

  // A new request restarts the attempt budget: the cap is per request, not per
  // session, or the third tile of a session could never be retried.
  attempts = 0;
  statusLine.clear();

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

void TileDesignerWindow::GenerateResponse(size_t index, std::string prompt) {
  AI *backend = ai;
  if (backend == nullptr) {
    return;
  }

  // Snapshot the history on the main thread. The worker must not read `history`
  // while a later turn mutates it, and the request has to be built from the
  // conversation as it stood when the request was made.
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

  // Neither a "stream finished" branch nor a synchronous completion check
  // belongs here: OllamaAI's generateStream dispatches its own thread and returns
  // as soon as the request is accepted, so completion is the provider's
  // empty-token sentinel and nothing else. This mirrors DesignerChatWindow, which
  // is the proven implementation of the same handoff.
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
            ex.answer += token;
            return;
          }
          ex.done = true;
          if (ex.answer.empty()) {
            ex.failed = true;
            ex.error =
                "No answer came back. Check that the backend is running and "
                "that the model is available.";
          }
        });

    if (!delivered) {
      failure = "The tile designer backend could not accept the request.";
    }
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

bool TileDesignerWindow::IsBusy() {
  std::lock_guard<std::mutex> lock(transcriptMutex);
  for (const Exchange &ex : exchanges) {
    if (!ex.done) {
      return true;
    }
  }
  return false;
}

void TileDesignerWindow::InspectFinishedExchange(size_t index) {
  Exchange finished;
  {
    std::lock_guard<std::mutex> lock(transcriptMutex);
    if (index >= exchanges.size()) return;
    finished = exchanges[index];
  }

  ++requestNumber;

  LogEvent("MODEL REPLY (request " + std::to_string(requestNumber) + ", " +
           std::to_string(finished.answer.size()) + " bytes" +
           (finished.fromUser ? ", answers a user request" : ", engine retry") +
           ")");
  Log(finished.answer);
  Log("\n");

  if (finished.failed) {
    LogEvent("TRANSPORT FAILURE: " + finished.error);
    std::lock_guard<std::mutex> lock(transcriptMutex);
    if (index < exchanges.size()) exchanges[index].inspected = true;
    history.push_back({"user", finished.prompt});
    return;
  }

  TileRegistry *registry =
      game != nullptr ? &game->tileRegistry : nullptr;
  std::string reason;
  TileDesigner::ApplyResult applied;
  if (registry == nullptr) {
    reason = "there is no tile catalog to add to.";
  } else {
    applied = TileDesigner::ApplyReply(finished.ParsedAnswer(), *registry,
                                       OverlayDirectory());
  }

  {
    std::lock_guard<std::mutex> lock(transcriptMutex);
    if (index < exchanges.size()) {
      Exchange &ex = exchanges[index];
      ex.inspected = true;
      ex.ok = applied.ok;
      ex.failureReason = applied.ok ? std::string() : reason.empty() ? applied.error
                                                                    : reason;
      ex.tileName = applied.name;
      ex.savedPath = applied.savedPath;
      ex.notes = applied.notes;
      ex.warnings = applied.warnings;
      ex.saveFailed = applied.saveFailed;
      ex.definition = applied.definition;
    }
  }

  // The conversation both sides saw, so a follow-up can refer to this tile. The
  // assistant turn is the raw reply; the capped replay is what keeps a large
  // definition from accumulating.
  history.push_back({"user", finished.prompt});
  history.push_back({"assistant", finished.HistoryAnswer()});

  if (applied.ok) {
    LogEvent("ADDED: " + applied.name +
             (applied.savedPath.empty() ? " (not saved)" : " -> " + applied.savedPath));
    if (!applied.notes.empty()) {
      Log("designerNotes: " + applied.notes + "\n");
    }
    for (const std::string &warning : applied.warnings) {
      Log("warning: " + warning + "\n");
    }
    if (game != nullptr && game->debugLog) {
      game->debugLog->LogInfo("Tile designer: added tile '" + applied.name + "'" +
                              (applied.savedPath.empty()
                                   ? ""
                                   : " to " + applied.savedPath));
    }
    if (applied.definition != nullptr) {
      LoadEditorFrom(*applied.definition);
    }
    attempts = 0;
    return;
  }

  // Refused: say why, in the same conversation, and try again. Bounded, so a
  // model that cannot satisfy the contract cannot spend the whole budget.
  LogEvent("REFUSED: " + applied.error);
  ++attempts;
  const int maxAttempts = game != nullptr ? game->maxDesignAttempts() : 3;
  if (attempts >= maxAttempts) {
    LogEvent("GIVING UP after " + std::to_string(attempts) + " attempt(s)");
    if (game != nullptr && game->debugLog) {
      game->debugLog->LogWarning("Tile designer: giving up after " +
                                 std::to_string(attempts) +
                                 " attempt(s); last reason: " + applied.error);
    }
    return;
  }

  LogEvent("RETRYING (" + std::to_string(attempts) + " of " +
           std::to_string(maxAttempts) + " used)");

  Exchange retry;
  retry.prompt =
      "That tile was refused: " + applied.error +
      "\n\nSend the corrected tile as a complete ```json block, with "
      "designerNotes. Do not describe the fix in prose instead of sending it.";
  retry.fromUser = false;

  size_t retryIndex = 0;
  {
    std::lock_guard<std::mutex> lock(transcriptMutex);
    exchanges.push_back(std::move(retry));
    retryIndex = exchanges.size() - 1;
  }

  Log(retry.prompt);
  Log("\n");

  GenerateResponse(retryIndex, exchanges[retryIndex].prompt);
}

void TileDesignerWindow::DrawPreviewPane() {
  if (edited == nullptr) {
    ImGui::TextWrapped(
        "The tile from the latest reply appears here, live and editable. A tile "
        "is a glyph, a colour and whether it blocks movement -- there is no art "
        "in this step.");
  } else {
    // Mutable: the editor writes through this reference, then tells the registry
    // through TouchRuntime. That is what makes an edit visible everywhere at once
    // rather than to a copy that lives and dies with this pane.
    TileDef &definition = *edited;
    const Color fg = definition.hasCharacterColor ? definition.characterColor
                                                  : Color{255, 255, 255, 255};
    const Color bg = definition.hasBackgroundColor ? definition.backgroundColor
                                                   : Color{0, 0, 0, 0};

    ImGui::PushStyleColor(ImGuiCol_Text, editedIsRuntime
                                             ? ImVec4(0.4f, 1.0f, 0.4f, 1.0f)
                                             : ImVec4(1.0f, 0.8f, 0.4f, 1.0f));
    ImGui::TextWrapped("%s", editedIsRuntime
                                 ? "CREATED (in the generated overlay)"
                                 : "HAND-AUTHORED (read-only here)");
    ImGui::PopStyleColor();
    ImGui::Text("%s", definition.name.c_str());
    ImGui::TextDisabled("%s | %s",
                        definition.hasCharacter ? "glyph" : "no glyph",
                        definition.blocksTile ? "blocks movement" : "walkable");
    ImGui::Separator();

    // The plate, drawn from the definition itself so what is shown is exactly
    // what the world would draw.
    DrawGlyphPlate(definition.hasCharacter ? definition.character : '?', fg, bg,
                   64.0f);
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextDisabled("foreground  %3d,%3d,%3d", fg.r, fg.g, fg.b);
    ImGui::TextDisabled("background  %3d,%3d,%3d,%3d", bg.r, bg.g, bg.b, bg.a);
    if (definition.hasHitboxSize) {
      ImGui::TextDisabled("hitbox      %zu", definition.hitboxSize);
    }
    if (!definition.sprite.empty()) {
      ImGui::TextDisabled("sprite      %s", definition.sprite.c_str());
    }
    ImGui::EndGroup();

    ImGui::Separator();
    // Editor. Disabled for a catalog tile rather than hidden: seeing the values
    // and being told they are not yours to change is more useful than an empty
    // pane.
    //
    // Every control writes into `edited` and then calls TouchRuntime, which is
    // the only way the rest of the engine is told the definition changed -- the
    // material palette the *other* designer gets is rebuilt from this catalog, so
    // an edit that did not bump the revision would leave the vocabulary stale.
    ImGui::BeginDisabled(!editedIsRuntime);

    bool dirty = false;
    ImGui::SetNextItemWidth(70.0f);
    // The glyph is taken on a single-character change only. A paste of two
    // characters, or an empty box, is left as typed and reported, rather than
    // written through: the schema accepts exactly one glyph, and silently keeping
    // the first character (or clearing the glyph entirely) would hide a mistake
    // instead of showing it. The previous glyph is therefore restored before the
    // write, so a transient empty box cannot strip the tile of its appearance.
    if (ImGui::InputText("glyph", glyphBuffer, sizeof(glyphBuffer),
                         ImGuiInputTextFlags_CharsNoBlank)) {
      const std::string typed(glyphBuffer);
      if (typed.size() == 1) {
        definition.character = typed[0];
        definition.hasCharacter = true;
        dirty = true;
      }
    }
    if (std::string(glyphBuffer).size() != 1) {
      ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f),
                         "exactly one character is required (you have %zu)",
                         std::string(glyphBuffer).size());
    }

    ImGui::LabelText("name", "%s", definition.name.c_str());
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip(
          "The name is the tile's identity in the catalog and on disk.\n"
          "It is not editable here: ask for a different tile instead, so a "
          "reference to this one can never point somewhere new.");
    }

    dirty |= ColorEditorRow("fg", "character colour", foreground, 3);
    dirty |= ColorEditorRow("bg", "background colour", background, 4);
    dirty |= ImGui::Checkbox("blocks tile", &editorBlocks);

    if (dirty) {
      definition.characterColor = Color{
          static_cast<unsigned char>(foreground[0]),
          static_cast<unsigned char>(foreground[1]),
          static_cast<unsigned char>(foreground[2]),
          definition.hasCharacterColor ? definition.characterColor.a
                                       : static_cast<unsigned char>(255)};
      definition.hasCharacterColor = true;
      definition.backgroundColor =
          Color{static_cast<unsigned char>(background[0]),
                static_cast<unsigned char>(background[1]),
                static_cast<unsigned char>(background[2]),
                static_cast<unsigned char>(background[3])};
      definition.hasBackgroundColor = true;
      definition.blocksTile = editorBlocks;
      game->tileRegistry.TouchRuntime(&definition);
      // The edit is live for the session either way; this only tracks whether the
      // overlay file is now behind the definition.
      editedIsDirty = true;
    }

    ImGui::EndDisabled();

    ImGui::Separator();
    ImGui::BeginDisabled(!editedIsRuntime);
    if (ImGui::Button("Save to overlay")) {
      const std::string error = SaveEditedTile();
      statusIsError = !error.empty();
      statusLine = error.empty()
                       ? "Saved " + definition.name + " to " + OverlayDirectory()
                       : error;
      if (error.empty()) {
        editedIsDirty = false;
      }
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip(
          editedIsRuntime
              ? "Write this tile to the generated overlay so it survives a "
                "restart.\nEvery edit is already live for this session."
              : "Only a tile this window created can be written back.");
    }
    ImGui::SameLine();
    ImGui::TextDisabled(editedIsDirty ? "(edited -- not yet written)"
                                      : "(on disk)");
  }

  // Below the definition editor, because art is about the tile's appearance and the
  // editor is about its identity. A catalog tile gets this too -- its art cannot be
  // generated, but "what does this draw as" is still worth reading while looking at
  // a definition that came from the same catalog.
  DrawArtSection();
}

std::string TileDesignerWindow::DescribeArtState() const {
  if (game == nullptr) return "no game";

  const TileArtQueue &queue = game->TileArtQueueState();
  const int used = queue.RequestsUsed();
  const int cap = queue.GetConfig().maxRequestsPerSession;
  const size_t queued = queue.CountInState(TileArtQueue::State::Queued);
  const size_t running = queue.CountInState(TileArtQueue::State::Running);
  const size_t done = queue.CountInState(TileArtQueue::State::Done);
  const size_t failed = queue.CountInState(TileArtQueue::State::Failed);
  const size_t skipped = queue.CountInState(TileArtQueue::State::Skipped);

  std::string out;
  if (!queue.Enabled()) {
    out = "art generation off (art.generation.enabled in settings.json)";
  } else {
    out = std::to_string(used) + "/" + std::to_string(cap) + " request(s) used";
    if (queue.ReportedCost() > 0.0) {
      // The API's own number, summed. Shown because "what has this cost so far" is
      // a question the person paying should not have to go and look up.
      char buffer[64];
      std::snprintf(buffer, sizeof(buffer), ", $%.4f", queue.ReportedCost());
      out += buffer;
    }
  }
  if (queued > 0) out += " | " + std::to_string(queued) + " queued";
  if (running > 0) out += " | " + std::to_string(running) + " generating";
  if (done > 0) out += " | " + std::to_string(done) + " done";
  if (failed > 0) out += " | " + std::to_string(failed) + " failed";
  if (skipped > 0) out += " | " + std::to_string(skipped) + " skipped";
  return out;
}

void TileDesignerWindow::DrawArtSection() {
  if (game == nullptr || edited == nullptr) return;

  ImGui::Separator();
  ImGui::TextUnformatted("Art");

  const TileArtQueue &queue = game->TileArtQueueState();
  const TileDef &definition = *edited;

  // What the tile draws as right now, before anything is generated. The glyph is a
  // legitimate rendering (phase 1), so this is stated as a fact rather than as a
  // gap -- "not yet generated" is not the same as "broken", and the feature has to
  // work with art absent.
  std::string resolved;
  const bool hasArt = SpriteResolves(definition.sprite,
                                     TileSpriteId(definition.name), &resolved);
  if (hasArt) {
    ImGui::TextDisabled("drawn as: %s", resolved.c_str());
  } else {
    ImGui::TextDisabled("drawn as its glyph '%c' (no sprite yet)",
                        definition.hasCharacter ? definition.character : '?');
  }

  // The queue's state for this tile, if it has ever been asked for.
  for (const TileArtQueue::Job &job : queue.Snapshot()) {
    if (job.tileName != definition.name) continue;
    switch (job.state) {
      case TileArtQueue::State::Queued:
        ImGui::TextColored(ImVec4(0.7f, 0.8f, 1.0f, 1.0f),
                           "queued -- batched with anything else asked for in the "
                           "next %.0fs",
                           static_cast<double>(queue.GetConfig().batchDelaySeconds));
        break;
      case TileArtQueue::State::Running:
        ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.4f, 1.0f),
                           "generating... (the game stays responsive)");
        break;
      case TileArtQueue::State::Done: {
        std::string line = "generated";
        if (job.refreshedCells > 0) {
          line += " -- " + std::to_string(job.refreshedCells) + " cell(s) updated";
        }
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s", line.c_str());
        break;
      }
      case TileArtQueue::State::Failed:
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "failed: %s",
                           job.error.c_str());
        break;
      case TileArtQueue::State::Skipped:
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.4f, 1.0f), "skipped: %s",
                           job.error.c_str());
        break;
    }
    break;
  }

  ImGui::TextDisabled("%s", DescribeArtState().c_str());

  const bool generating = queue.CountInState(TileArtQueue::State::Queued) > 0 ||
                          queue.CountInState(TileArtQueue::State::Running) > 0;
  ImGui::BeginDisabled(!editedIsRuntime || generating || !queue.Enabled());
  if (ImGui::Button("Generate art")) {
    std::string reason;
    if (game->RequestTileArt(definition.name, &reason)) {
      statusIsError = false;
      statusLine = "Queued art for '" + definition.name +
                   "'. It draws as its glyph until the sprite lands.";
    } else {
      statusIsError = true;
      statusLine = reason;
    }
  }
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip(
        "%s",
        !queue.Enabled()
            ? "Art generation is off. Set art.generation.enabled in "
              "settings.json -- it spends money, so it is off by default."
            : !editedIsRuntime
                  ? "Only a tile this window created can have art generated for "
                    "it; a hand-authored catalog tile is not ours to re-draw."
                  : generating
                        ? "A request is already in flight."
                        : "Ask for one sprite for this tile.\nRequests are "
                          "batched, because one request costs the same whether it "
                          "carries one tile or several.");
  }
}

void TileDesignerWindow::Draw() {
  bool isOpen = true;
  ImGui::SetNextWindowSize(ImVec2(780, 560), ImGuiCond_FirstUseEver);
  ImGui::Begin("Tile Designer", &isOpen, ImGuiWindowFlags_None);
  if (!isOpen) {
    game->tileDesignerWindowEntity.remove<ActiveWindow>();
    ImGui::End();
    return;
  }

  ImGui::TextDisabled("Backend: %s",
                      modelLabel.empty() ? "(none)" : modelLabel.c_str());
  ImGui::SameLine();
  ImGui::TextDisabled("| %d of %d attempts used", attempts,
                      game != nullptr ? game->maxDesignAttempts() : 0);
  ImGui::SameLine();
  ImGui::Checkbox("Show thinking", &showThoughts);
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip(
        "Show the model's <think> reasoning in the transcript.\n"
        "It is always written to the log -- this only decides whether you read it.");
  }
  ImGui::SameLine();
  ImGui::Checkbox("Show JSON", &showJson);
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip(
        "Show the tile JSON in the transcript.\n"
        "It is parsed and applied either way -- this only changes what you read.");
  }
  ImGui::SameLine();
  ImGui::TextDisabled("| overlay: %s", OverlayDirectory().empty()
                                           ? "(none: tiles are session-only)"
                                           : OverlayDirectory().c_str());
  ImGui::Separator();

  if (ai == nullptr) {
    ImGui::TextWrapped(
        "No designer backend is configured, so nothing can be sent. The tile "
        "designer shares the Designer Chat backend; set \"designer\" in "
        "settings.json (see SETTINGS.md).");
    ImGui::End();
    return;
  }

  if (!layoutLoaded && game != nullptr && game->debugWindowState) {
    showJson = game->debugWindowState->GetDesignerShowJson();
    layoutLoaded = true;
  }

  // Left: the tile, editable. Right: the conversation.
  const float available = ImGui::GetContentRegionAvail().x;
  const float paneWidth = std::clamp(available * 0.42f, 220.0f,
                                     std::max(220.0f, available - 260.0f));

  ImGui::BeginChild("tile_designer_preview", ImVec2(paneWidth, -70), true);
  DrawPreviewPane();
  ImGui::EndChild();

  ImGui::SameLine();

  ImGui::BeginChild("tile_designer_transcript", ImVec2(0, -70), true);
  std::vector<Exchange> snapshot;
  {
    std::lock_guard<std::mutex> lock(transcriptMutex);
    snapshot = exchanges;
  }

  if (snapshot.empty()) {
    ImGui::TextWrapped(
        "Ask for a tile -- \"a stone hearth\", \"mossy flagstones\", \"a puddle\". "
        "The designer replies with one tile definition and its own notes; an "
        "accepted tile is written to the generated overlay and is immediately "
        "paintable from a blueprint.");
  }

  bool needsInspection = false;
  for (size_t i = 0; i < snapshot.size(); ++i) {
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
      if (ex.ok) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 1.0f, 0.4f, 1.0f));
        ImGui::TextWrapped("Added '%s'%s", ex.tileName.c_str(),
                           ex.savedPath.empty()
                               ? " (session only -- not written to disk)"
                               : (" to " + ex.savedPath).c_str());
        ImGui::PopStyleColor();
        if (!ex.notes.empty()) {
          ImGui::TextDisabled("Designer's notes:");
          ImGui::TextWrapped("%s", ex.notes.c_str());
        }
        for (const std::string &warning : ex.warnings) {
          ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.5f, 1.0f));
          ImGui::TextWrapped("Note: %s", warning.c_str());
          ImGui::PopStyleColor();
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

    ImGui::Separator();
  }
  ImGui::EndChild();

  // Inspection runs on the main thread, after the transcript is drawn. Doing it
  // on the worker would mean validating and writing content from a background
  // thread while the ECS is mid-frame.
  if (needsInspection) {
    for (size_t i = 0; i < snapshot.size(); ++i) {
      if (!snapshot[i].inspected && snapshot[i].done) {
        InspectFinishedExchange(i);
      }
    }
  }

  if (!statusLine.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text,
                          statusIsError ? ImVec4(1.0f, 0.5f, 0.4f, 1.0f)
                                        : ImVec4(0.5f, 1.0f, 0.6f, 1.0f));
    ImGui::TextWrapped("%s", statusLine.c_str());
    ImGui::PopStyleColor();
  }

  const bool busy = IsBusy();
  ImGui::BeginDisabled(busy);
  const bool send = ImGui::Button("Send");
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::TextDisabled("Enter to send, Shift+Enter for a newline");

  ImGui::SetNextItemWidth(-1.0f);
  const bool submitted = ImGui::InputTextMultiline(
      "##tile_designer_input", inputBuffer, sizeof(inputBuffer),
      ImVec2(-1.0f, 54.0f),
      ImGuiInputTextFlags_EnterReturnsTrue |
          ImGuiInputTextFlags_CtrlEnterForNewLine);

  if ((send || submitted) && !busy) {
    std::string prompt(inputBuffer);
    while (!prompt.empty() && (prompt.back() == '\n' || prompt.back() == ' ')) {
      prompt.pop_back();
    }
    if (!prompt.empty()) {
      inputBuffer[0] = '\0';
      SendUserMessage(prompt);
    }
  }

  if (layoutLoaded && game != nullptr && game->debugWindowState) {
    game->debugWindowState->SetDesignerShowJson(showJson);
  }

  ImGui::End();
}
