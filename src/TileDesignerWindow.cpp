#include "TileDesignerWindow.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <utility>

#include "Components.h"
#include "DebugLog.h"
#include "LogPaths.h"

#include <nlohmann/json.hpp>
#include "Game.h"
#include "PixelDSL.hpp"
#include "Settings.h"
#include "RewriteBriefs.hpp"
#include "TileArt.hpp"
#include "SpriteDraw.hpp"
#include "SpritePaths.hpp"
#include "TileArtQueue.hpp"
#include "TimeUtils.hpp"
#include "RaylibException.hpp"
#include "imgui.h"
#include "raylib.h"

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
                                       std::string modelLabel, bool selfTest,
                                       flecs::entity owner)
    : game(game), ai(designerAI), modelLabel(std::move(modelLabel)),
      selfTest(selfTest) {
  (void)owner;  // Windows are not ECS components yet; see Window.h.
  // One context per window instance, named so it cannot collide with an NPC's
  // ("npc-<id>"), the blueprint designer's or the vision window's.
  contextId = "tile-designer-" + std::to_string(reinterpret_cast<uintptr_t>(this));
  // Its own provider context. Not because the histories are sent explicitly -- they
  // are -- but because a provider that keys anything by context must not have a
  // document request and a definition request share one.
  artContextId =
      "tile-designer-art-" + std::to_string(reinterpret_cast<uintptr_t>(this));
  if (game != nullptr) {
    // By the current mode rather than the tile-specific entry point, so the two cannot
    // disagree about what the model was told. The mode starts as Tile, so this is the same
    // prompt the old call produced.
    systemPrompt = game->BuildSpriteSystemPrompt(mode, requestSet);
  }

  // The Improve button. Same backend as the conversation, its own provider context,
  // and its instructions from data/prompts/REWRITE.md.
  rewriter.Configure(game != nullptr ? game->rewriterAI.get() : nullptr,
                     Settings().content.rewritePromptFile,
                     contextId + "-rewriter",
                     game != nullptr ? game->debugLog.get() : nullptr);
  RefreshRewriteBrief();
  promptBox.Setup(game, &rewriter, inputBuffer, sizeof(inputBuffer));
  // The rewrite transcript goes into the same file as the conversation, so a rewrite
  // that produced the wrong half is diagnosable from the log rather than by replaying
  // it: the game's DebugLog is in memory only.
  promptBox.SetLogSink([this](const std::string &line) { Log(line); });
}

size_t TileDesignerWindow::RunSelfTestConversation(bool requestArt,
                                                 SpriteKinds::Kind kind,
                                                 bool setTest) {
  if (!selfTest) return 0;

  // The mode is set here because the canned reply below is a reply for that kind, and the
  // whole point of the object variant is to run the object half of the inspection path --
  // a tile-shaped reply inspected as an object would be refused for having no components,
  // which tests the refusal and not the accept.
  mode = kind;
  designMode = kind;
  // The shape, before the request is "sent": inspection reads it per exchange, and a set
  // reply judged by the single path is refused for having no "name" at the top level.
  requestSet = setTest;
  RefreshRewriteBrief();
  if (game != nullptr) {
    systemPrompt = game->BuildSpriteSystemPrompt(mode, requestSet);
  }

  // Switched on here rather than through the settings, because the point is to test
  // the window's automatic request and not the configuration that normally turns it
  // on. Whether the request is then *accepted* is up to the queue, which is exactly
  // what the caller goes on to check.
  // The set case asks for art as well, even though it is not the auto-art mode: a set is
  // where "several sprites become several requests" happens, and a test that stops before the
  // estimate and the queue has not run the part of the set flow that spends money.
  autoRequestArt = requestArt || setTest;

  // The same sequence a real request runs, with the reply supplied instead of
  // streamed: header, user request, reply, inspection. Both replies are real ones --
  // the puddle the tile designer produced, and an object of the shape the object designer
  // is meant to produce -- because a hand-written "valid definition" would not exercise
  // the notes stripping, the ASCII renderer, the component vocabulary or the catalog's
  // response to a name it has seen before.
  const std::string kCannedTileReply = R"(<think>Keep it short.</think>
A walkable puddle, non-blocking, using a free glyph.

```json
{
  "name": "self test puddle",
  "character": "=",
  "characterColor": [80, 170, 230, 255],
  "backgroundColor": [110, 90, 70, 255],
  "blocksTile": false,
  "designerNotes": "Written by the self test to exercise the logging path end to end. The glyph and colours are copied from a real generated tile."
}
```
)";

  // The object reply. Deliberately ordinary rather than exotic: two capabilities, one
  // parameter, an art block whose palette covers every character it uses. A reply with a
  // palette gap would be accepted with a warning, which is a useful case to *have* but a
  // poor default -- an unknown warning in the log of a passing test is noise.
  const std::string kCannedObjectReply = R"(<think>Two capabilities, and art the palette covers.</think>
A stout post you can see from a distance and lean on; it blocks movement.

```json
{
  "name": "self test post",
  "description": "A weathered post set into the ground.",
  "components": ["Obstacle", "Interactable"],
  "art": ["#--#", "#++#", " ## "],
  "palette": {
    "#": [94, 72, 52, 255],
    "-": [140, 108, 74, 255],
    "+": [176, 140, 96, 255]
  },
  "character": "P",
  "characterColor": [176, 140, 96, 255],
  "backgroundColor": [46, 36, 26, 255],
  "designerNotes": "Written by the self test to exercise the object half of the inspection path end to end. Two capabilities, one art block, and a palette that covers every character the art uses."
}
```
)";

  // The set reply. Three entries with the middle one deliberately broken, so the canned
  // conversation exercises the property that matters most about a set: the entries are
  // validated independently and a refused one does not discard the accepted ones.
  const std::string kCannedSetReply = R"(<think>Three fittings that share a material, one of them broken on purpose.</think>
A set of iron fittings for one room.

```json
{
  "set": "self test set",
  "sprites": [
    {
      "name": "self test grate",
      "components": ["Obstacle"],
      "character": "g",
      "designerNotes": "Written by the self test. One capability, so it is a valid entry."
    },
    {
      "name": "self test broken",
      "components": ["Obstacle"],
      "character": "x",
      "designerNotes": ""
    },
    {
      "name": "self test bucket",
      "components": ["Portable", "Storage"],
      "capacity": 3,
      "character": "u",
      "designerNotes": "Written by the self test. Carries and holds a little."
    }
  ]
}
```
)";

  const std::string kCannedReply =
      setTest ? kCannedSetReply
              : (kind == SpriteKinds::Kind::Object ? kCannedObjectReply
                                                   : kCannedTileReply);

  LogSessionHeader();

  Exchange ex;
  ex.prompt = "self test: log one exchange";
  ex.fromUser = true;
  ex.done = true;
  ex.answer = kCannedReply;
  // Latched here as `SendUserMessage` does for a real request. The self-test builds its
  // exchange directly, so anything the normal path sets has to be set here too -- and
  // forgetting this one made the set reply be judged as a single definition, refused for
  // having no top-level "name", and retried. A test that runs the wrong path convincingly is
  // worse than no test.
  ex.wasSet = setTest;
  size_t index = 0;
  {
    std::lock_guard<std::mutex> lock(transcriptMutex);
    exchanges.push_back(std::move(ex));
    index = exchanges.size() - 1;
  }

  LogEvent("USER REQUEST (attempt 1 of this request)");
  Log("self test: log one exchange\n");

  // One synthetic request so the "what went on the wire" line is written too, with
  // a real elapsed time measured across the inspection below.
  {
    std::vector<ChatMessage> request;
    if (!systemPrompt.empty()) request.push_back({"system", systemPrompt});
    request.push_back({"user", "self test: log one exchange"});
    size_t total = 0;
    for (const ChatMessage &m : request) total += m.content.size();
    LogEvent("SENT (request 1): " + std::to_string(request.size()) +
             " message(s), " + std::to_string(total) + " chars -- 1 system, 1 user, "
             "0 assistant; history trimmed to " +
             std::to_string(kMaxHistoryMessages) + " message(s) max");
  }
  requestStarted = std::chrono::steady_clock::now();
  InspectFinishedExchange(index);

  size_t inspected = 0;
  {
    std::lock_guard<std::mutex> lock(transcriptMutex);
    for (const Exchange &exchange : exchanges) {
      if (exchange.inspected) ++inspected;
    }
  }
  return inspected;
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
    // <session>/tile-designer/, beside the blueprint designer's but in its own
    // directory: the two conversations are about different vocabularies, and
    // mixing them would make the folder harder to scan.
    const std::filesystem::path directory = LogPaths::Subdir("tile-designer");
    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec)) {
      if (game != nullptr && game->debugLog) {
        game->debugLog->LogWarning("Tile designer: could not create " +
                                   directory.string() + ": " + ec.message());
      }
      logFilePath = " ";
      return;
    }
    logFilePath = (directory / (TimeUtils::GetCurrentTimeString() +
                                "-tile-designer.txt")).string();
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

void TileDesignerWindow::LogPromptFile(const std::string &path) {
  if (path.empty()) {
    Log("prompt file: (none configured -- the vocabulary below is all the model "
        "was given)\n");
    return;
  }
  std::ifstream file(path);
  if (!file.is_open()) {
    // Worth recording precisely: a silent fallback to "no instructions" is the
    // difference between a model that ignored its brief and one that never had it.
    Log("prompt file: " + path + "  -- COULD NOT BE READ\n");
    return;
  }
  std::stringstream buffer;
  buffer << file.rdbuf();
  const std::string text = buffer.str();
  int lines = 0;
  for (const char c : text) {
    if (c == '\n') ++lines;
  }
  Log("prompt file: " + path + "  (" + std::to_string(text.size()) +
      " bytes, " + std::to_string(lines) + " lines)\n");
  Log("--- file contents (the instructions; the vocabulary is appended below) "
      "---\n");
  Log(text);
  if (!text.empty() && text.back() != '\n') Log("\n");
  Log("--- end of file ---\n");
}

void TileDesignerWindow::LogSessionHeader() {
  if (headerLogged) return;
  headerLogged = true;

  // The kind this session designs, named in the header because a log with two kinds of
  // session in it is otherwise ambiguous -- and the two are told apart by their prompt
  // file and their overlay, both of which follow from this line.
  const SpriteKinds::Descriptor &kind = SpriteKinds::For(mode);
  Log("\n================ SPRITE DESIGNER SESSION (" + kind.label +
      ") ================\n");
  Log("started:   " + TimeUtils::GetCurrentTimeString() + "\n");
  Log("kind:      " + kind.id + "\n");
  Log("backend:   " + (modelLabel.empty() ? std::string("(none)") : modelLabel) +
      "\n");
  Log("context:   " + contextId + "\n");

  const GameSettings &settings = Settings();
  const std::string overlay = ModeOverlayDirectory();
  Log("overlay:   " + (overlay.empty() ? std::string("(none)") : overlay) + "\n");
  Log("art:       pack=" +
      (settings.art.pack.empty() ? std::string("(none)") : settings.art.pack) +
      "  generation=" + (settings.art.generation.enabled ? "on" : "off") + "\n");
  Log("attempts:  " + std::to_string(game != nullptr ? game->maxDesignAttempts() : 0) +
      " per request\n");

  LogPromptFile(kind.id == "object" ? settings.content.objectDesignerPromptFile
                                    : settings.content.tileDesignerPromptFile);

  // The system prompt as it will be sent: the file above with %CONTRACT%, %GLYPHS% and
  // %VOCAB% substituted from the live world at the moment the window was constructed --
  // the tile catalog for a tile, the factory's components for an object. This is the
  // single most valuable line in the log, because it is the only record of what the model
  // was actually told.
  Log("--- system prompt as sent (" + std::to_string(systemPrompt.size()) +
      " bytes) ---\n");
  if (systemPrompt.empty()) {
    Log("(EMPTY -- the model is running with no instructions at all)\n");
  } else {
    Log(systemPrompt);
    if (systemPrompt.back() != '\n') Log("\n");
  }
  Log("--- end of system prompt ---\n");
  Log("=======================================================\n");
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

  // Keep the catalog's date in step with the file that now backs it, so the Map
  // Editor's date sort reflects a saved edit in this session rather than only
  // after the next overlay reload.
  std::error_code timeEc;
  const auto written = std::filesystem::last_write_time(path, timeEc);
  if (!timeEc) {
    edited->sourceTime = written;
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

  // The kind this request is being built for, latched here rather than read at
  // inspection time. The dropdown may move while the reply is in flight, and the reply
  // must be judged by the grammar it was asked for -- validating object JSON through the
  // tile validator would refuse it for having no glyph. See InspectFinishedExchange.
  designMode = mode;
  // The shape has to be latched for the same reason: inspection happens on a later frame, and
  // a set reply judged by the single-sprite path would be refused for having no "name" at the
  // top level.
  const bool designSet = requestSet;

  Exchange ex;
  ex.prompt = prompt;
  ex.fromUser = true;
  ex.wasSet = designSet;

  size_t index = 0;
  {
    std::lock_guard<std::mutex> lock(transcriptMutex);
    exchanges.push_back(std::move(ex));
    index = exchanges.size() - 1;
  }

  // The session header goes out with the first message, before anything else: the
  // log is created here rather than in the constructor, so a window that is never
  // used leaves no file -- and the header cannot go anywhere else without either
  // creating that file unconditionally or being written after the first reply, when
  // it is no longer the state the reply was produced under.
  LogSessionHeader();

  // Logged before the request goes out, so a send that never gets a reply still
  // leaves a record of what was asked. That is not hypothetical: a request whose
  // reply never arrives leaves a file that ends here, and the absence is the
  // evidence.
  LogEvent("USER REQUEST (attempt " + std::to_string(attempts + 1) +
           " of this request)");
  Log(prompt);
  Log("\n");

  GenerateResponse(index, prompt);
}

void TileDesignerWindow::GenerateResponse(size_t index, std::string prompt,
                                          bool artConversation) {
  AI *backend = ai;
  if (backend == nullptr) {
    return;
  }

  // Which conversation this request belongs to. Two histories and two system
  // prompts, one send path: everything below -- the trim, the log, the streaming,
  // the sentinel -- is the same for both, and a second copy of it is a second place
  // for the handoff to break.
  const std::string &activeSystem =
      artConversation ? artSystemPrompt : systemPrompt;
  const std::vector<ChatMessage> &activeHistory =
      artConversation ? artHistory : history;
  const std::string activeContext =
      artConversation ? artContextId : contextId;

  // Snapshot the history on the main thread. The worker must not read `history`
  // while a later turn mutates it, and the request has to be built from the
  // conversation as it stood when the request was made.
  std::vector<ChatMessage> request;
  if (!activeSystem.empty()) {
    request.push_back({"system", activeSystem});
  }
  const size_t firstKept = activeHistory.size() > kMaxHistoryMessages
                               ? activeHistory.size() - kMaxHistoryMessages
                               : 0;
  for (size_t i = firstKept; i < activeHistory.size(); ++i) {
    request.push_back(activeHistory[i]);
  }
  request.push_back({"user", prompt});

  // What actually goes on the wire, recorded before the thread is started so the
  // log and the request cannot disagree: the exact message list, including how much
  // of the history survived the cap. The system prompt is written out once by
  // LogSessionHeader, so here it is recorded by size -- repeating a 3KB prompt on
  // every request would bury the conversation it is meant to illuminate.
  {
    size_t totalChars = 0;
    for (const ChatMessage &message : request) {
      totalChars += message.content.size();
    }
    // The count of each role, because "the model saw four turns" and "the model
    // saw one" explain very different replies.
    size_t systemCount = 0;
    size_t userCount = 0;
    size_t assistantCount = 0;
    for (const ChatMessage &message : request) {
      if (message.role == "system") ++systemCount;
      else if (message.role == "user") ++userCount;
      else if (message.role == "assistant") ++assistantCount;
    }
    LogEvent("SENT (request " + std::to_string(requestNumber + 1) + "): " +
             std::to_string(request.size()) + " message(s), " +
             std::to_string(totalChars) + " chars -- " +
             std::to_string(systemCount) + " system, " +
             std::to_string(userCount) + " user, " +
             std::to_string(assistantCount) + " assistant; history trimmed to " +
             std::to_string(kMaxHistoryMessages) + " message(s) max");
  }

  // The clock the reply is measured against starts here, where the request is
  // handed over, not where it was typed: the logged duration should mean "what the
  // backend took".
  requestStarted = std::chrono::steady_clock::now();

  // Neither a "stream finished" branch nor a synchronous completion check
  // belongs here: OllamaAI's generateStream dispatches its own thread and returns
  // as soon as the request is accepted, so completion is the provider's
  // empty-token sentinel and nothing else. This mirrors DesignerChatWindow, which
  // is the proven implementation of the same handoff.
  std::thread([this, index, request = std::move(request), backend,
               activeContext]() {
    // A fresh provider-side context per request, because the history is sent
    // explicitly: leaving the previous request's tokens in place would replay the
    // conversation twice.
    backend->resetContext(activeContext);

    std::string failure;
    const bool delivered = backend->generateStream(
        activeContext, request,
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

// ---------------------------------------------------------------------------
// The pixel document turn
// ---------------------------------------------------------------------------

bool TileDesignerWindow::DslArtSourceActive() const {
  return Settings().art.generation.source == "dsl";
}

std::string TileDesignerWindow::ArtDocumentPath(const std::string &name,
                                                SpriteKinds::Kind kind) const {
  return GeneratedSpriteDocumentPath(SpriteKinds::ConventionIdFor(kind, name));
}

void TileDesignerWindow::RequestSpriteDocument(const std::string &name,
                                               SpriteKinds::Kind kind,
                                               bool replaceGenerated,
                                               const std::string &surroundings,
                                               const std::string &requirement) {
  if (name.empty()) return;
  if (ai == nullptr) {
    statusIsError = true;
    statusLine = "No designer backend is configured, so a document cannot be asked for.";
    return;
  }
  if (IsBusy()) {
    statusIsError = false;
    statusLine = "A request is already in flight; the document follows it.";
    return;
  }
  if (game == nullptr) return;

  // The dropdown follows the request: a gap offered from the Designer window is a tile or
  // an object, and leaving the header saying the other one would make the transcript read
  // as if the wrong prompt had been used.
  mode = kind;

  const std::string conventionId = SpriteKinds::ConventionIdFor(kind, name);

  // An existing document normally wins and is not overwritten: the pack's copy is
  // hand-curated, and a generated copy is what the previous turn left, which a person
  // may have edited since. `replaceGenerated` is the explicit "Ask again" action --
  // it may replace the generated copy but still never the pack's, because that one is
  // not ours to discard.
  const SpriteDSLDocument existing = FindSpriteDSLDocument(conventionId);
  if (existing.found && (!replaceGenerated || existing.inArtPack)) {
    if (replaceGenerated && existing.inArtPack) {
      statusIsError = true;
      statusLine = "That document is hand-authored in the art pack (" +
                   existing.path +
                   "). Edit it there; a generated document would never win over it.";
      return;
    }
    std::string reason;
    const bool queued = game->RequestTileArt(name, &reason);
    statusIsError = !queued;
    statusLine = queued ? "Compiling the existing document " + existing.path
                        : reason;
    LogEvent("DSL DOCUMENT EXISTS (not overwriting): " + existing.path);
    return;
  }

  const std::string destination = ArtDocumentPath(name, kind);
  if (destination.empty()) {
    statusIsError = true;
    statusLine =
        "No generated layer is configured, so there is nowhere to write a "
        "document. Set art.pack (or art.generated) in settings.json.";
    return;
  }

  // The brief comes from the catalog, not from the window's editor buffers: the
  // definition is the authority, and an in-place colour tweak is not part of what
  // the agent is being asked to draw.
  artBrief = TileDesigner::SpriteDocumentBrief{};
  artBrief.kind = kind;
  artBrief.name = name;
  if (kind == SpriteKinds::Kind::Tile) {
    if (const TileDef *definition = game->tileRegistry.Get(name)) {
      artBrief.description = definition->description;
      artBrief.blocksTile = definition->blocksTile;
    }
  } else if (ObjectFactory *objects = Objects()) {
    const auto &templates = objects->GetTemplates();
    const auto found = templates.find(conventionId);
    if (found != templates.end()) {
      artBrief.description = found->second.value("description", std::string());
    }
  }

  // The designer's own description wins over the catalog's. A requested asset is not in
  // the catalog at all yet, so there is nothing to look up -- and when an existing asset
  // is being redrawn, the request is the newer statement of what it should be.
  if (!requirement.empty()) {
    artBrief.description = requirement;
  }

  // What the building around the gap looked like. Empty for a standalone request, which
  // is the normal case outside the Designer window -- the prompt builder omits the
  // paragraph entirely rather than saying "the surroundings are:" and stopping.
  artBrief.surroundings = surroundings;

  artAttempts = 0;
  artVerdict.clear();
  artHistory.clear();
  artSystemPrompt = TileDesigner::BuildSpriteDocumentSystemPrompt(
      Settings().content.spriteDSLPromptFile,
      PixelDSL::Config{}.rampsPath);

  statusIsError = false;
  statusLine = "Asking for a sprite document for '" + name + "'. It will be compiled "
               "before anything is written.";
  LogEvent("SPRITE DOCUMENT REQUESTED: " + name + " (" + conventionId + ")");
  SendArtTurn(TileDesigner::BuildSpriteDocumentRequest(artBrief, ""));
}

void TileDesignerWindow::SendArtTurn(const std::string &prompt) {
  if (ai == nullptr || prompt.empty() || IsBusy()) return;

  Exchange exchange;
  exchange.prompt = prompt;
  exchange.fromUser = true;
  exchange.isArtDocument = true;

  size_t index = 0;
  {
    std::lock_guard<std::mutex> lock(transcriptMutex);
    exchanges.push_back(std::move(exchange));
    index = exchanges.size() - 1;
  }

  // Logged in the same designer log as the definition turn: a document and the
  // definition it belongs to are one story, and a second log file would split it.
  LogEvent("SPRITE DOCUMENT REQUEST (attempt " +
           std::to_string(artAttempts + 1) + " of " +
           std::to_string(Settings().art.generation.dslAttempts) + ")");
  Log(prompt);
  Log("\n");

  GenerateResponse(index, prompt, /*artConversation=*/true);
}

void TileDesignerWindow::InspectFinishedArtDocument(size_t index,
                                                    const Exchange &finished) {
  // Published under the mutex once, at the end of each path, for the same reason the
  // definition path does it that way: the Draw thread reads these fields.
  auto publish = [this, index](bool ok, const std::string &failure,
                               const std::string &savedPath,
                               const std::string &note) {
    std::lock_guard<std::mutex> lock(transcriptMutex);
    if (index >= exchanges.size()) return;
    Exchange &exchange = exchanges[index];
    exchange.inspected = true;
    exchange.ok = ok;
    exchange.failureReason = failure;
    exchange.savedPath = savedPath;
    exchange.artNote = note;
  };

  if (finished.failed) {
    {
      std::lock_guard<std::mutex> lock(transcriptMutex);
      artHistory.push_back({"user", finished.prompt});
    }
    publish(false, finished.error, "", "no document: " + finished.error);
    LogEvent("SPRITE DOCUMENT TRANSPORT FAILURE: " + finished.error);
    return;
  }

  {
    std::lock_guard<std::mutex> lock(transcriptMutex);
    artHistory.push_back({"user", finished.prompt});
    artHistory.push_back({"assistant", finished.answer});
  }

  const std::string document = TileDesigner::ExtractSpriteDocument(finished.answer);
  PixelDSL::Config config;
  config.palettePath.clear();  // informational only; not worth a file read here
  const PixelDSL::Result result = PixelDSL::Compile(document, config);

  if (!result.ok) {
    artVerdict = PixelDSL::Describe(result);
    const int budget = std::max(1, Settings().art.generation.dslAttempts);
    LogEvent("SPRITE DOCUMENT REFUSED (attempt " + std::to_string(artAttempts + 1) +
             " of " + std::to_string(budget) + ")");

    if (artAttempts + 1 >= budget) {
      publish(false,
              "the document was refused on all " + std::to_string(budget) +
                  " attempt(s)",
              "", artVerdict);
      statusIsError = true;
      statusLine = "The sprite document was refused " + std::to_string(budget) +
                   " time(s); the last reason is in the transcript. Nothing was "
                   "written.";
      return;
    }

    ++artAttempts;
    // The failed attempt is published first, then the retry is sent -- so the
    // transcript reads as the conversation that happened rather than as one slow
    // request. The verdict goes back verbatim: it already names the line and says
    // what to do.
    publish(false, "refused; retrying", "", artVerdict);
    statusIsError = false;
    statusLine = "Document refused; asking again (attempt " +
                 std::to_string(artAttempts + 1) + " of " +
                 std::to_string(budget) + ").";
    SendArtTurn(TileDesigner::BuildSpriteDocumentRequest(artBrief, artVerdict));
    return;
  }

  const std::string destination = ArtDocumentPath(artBrief.name, artBrief.kind);
  if (destination.empty()) {
    artVerdict = PixelDSL::Describe(result);
    publish(false, "no generated layer to write the document to", "", artVerdict);
    statusIsError = true;
    statusLine = "The document compiled but there is nowhere to write it.";
    return;
  }

  std::error_code error;
  const std::filesystem::path path(destination);
  std::filesystem::create_directories(path.parent_path(), error);
  std::ofstream out(destination);
  if (!out.is_open()) {
    artVerdict = PixelDSL::Describe(result);
    publish(false, "could not write " + destination, "", artVerdict);
    statusIsError = true;
    statusLine = "Could not write " + destination + ".";
    return;
  }
  out << document;
  if (document.empty() || document.back() != '\n') out << "\n";
  out.close();
  if (!out) {
    publish(false, "could not write " + destination, "", artVerdict);
    statusIsError = true;
    statusLine = "Could not write " + destination + ".";
    return;
  }

  artVerdict = PixelDSL::Describe(result);
  publish(true, "", destination,
          artVerdict + "  document: " + destination);
  LogEvent("SPRITE DOCUMENT ACCEPTED: " + destination);

  // The compile goes through the queue like any other art request, so placement and
  // the hot-swap are one code path whether a person wrote the document or an agent
  // did.
  //
  // `RecompileSpriteArt`, not `RequestSpriteArt`: a document that was just accepted is by
  // definition newer than any sprite already compiled for that name, and the queue refuses
  // a name it has generated art for before. That refusal is what made pressing the
  // Recompile button necessary -- the automatic path was silently declining to compile the
  // document it had just written.
  std::string reason;
  if (game != nullptr &&
      game->RecompileSpriteArt(artBrief.kind, artBrief.name, &reason)) {
    statusIsError = false;
    statusLine = "Document accepted and saved to " + destination +
                 "; compiling it now.";
    LogEvent("SPRITE COMPILE QUEUED for " + artBrief.name);
  } else {
    statusIsError = true;
    statusLine = "Document accepted and saved, but the compile was refused: " + reason;
    LogEvent("SPRITE COMPILE REFUSED: " + reason);
  }
}

void TileDesignerWindow::InspectFinishedExchange(size_t index) {
  Exchange finished;
  {
    std::lock_guard<std::mutex> lock(transcriptMutex);
    if (index >= exchanges.size()) return;
    finished = exchanges[index];
  }

  ++requestNumber;

  // How long the reply took, from the moment the request was handed to the
  // backend. A monotonic clock because this is a duration and a clock step would
  // make it nonsense. Without it a slow model and a stalled backend read
  // identically in a transcript.
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - requestStarted);

  LogEvent("MODEL REPLY (request " + std::to_string(requestNumber) + ", " +
           std::to_string(finished.answer.size()) + " bytes, " +
           std::to_string(elapsed.count()) + " ms" +
           (finished.fromUser ? ", answers a user request" : ", engine retry") +
           ")");
  Log(finished.answer);
  Log("\n");

  // A pixel-document reply is inspected by the compiler, not by the definition
  // validators: it has no `name`, no components and no glyph, and handing it to
  // ApplyReply would refuse it for the wrong reason.
  if (finished.isArtDocument) {
    InspectFinishedArtDocument(index, finished);
    return;
  }

  if (finished.failed) {
    LogEvent("TRANSPORT FAILURE: " + finished.error);
    std::lock_guard<std::mutex> lock(transcriptMutex);
    if (index < exchanges.size()) exchanges[index].inspected = true;
    history.push_back({"user", finished.prompt});
    return;
  }

  // ---- apply the reply, by the kind it was *asked* for ---------------------------
  //
  // `designMode`, not `mode`: a reply is inspected on a later frame than it was
  // requested, so flipping the dropdown while a request is in flight must not make the
  // reply be judged against the other kind's grammar. Validating object JSON through the
  // tile validator would refuse it for having no glyph and no colour, which is a
  // confusing way to be told about a dropdown.
  //
  // One small local struct rather than the two ApplyResult types, because the rest of this
  // function -- the transcript, the retry, the art request, the log -- is identical for
  // both kinds and duplicating it would be a second place for the retry logic to drift.
  struct Applied {
    bool ok = false;
    std::string error;
    std::string name;
    std::string savedPath;
    std::string notes;
    std::vector<std::string> warnings;
    bool saveFailed = false;
    bool revised = false;
    const TileDef *tileDefinition = nullptr;
    nlohmann::json objectDefinition;
    std::string objectKey;
    std::string objectOverlayPath;
    // Set-only: the group's name, the names that were accepted (in reply order), and one
    // display line per entry so a partial set reads as "which one failed".
    bool wasSet = false;
    std::string setName;
    std::vector<std::string> acceptedNames;
    std::vector<std::pair<bool, std::string>> entryVerdicts;
  };

  std::string reason;
  // Filled in the accepted branch below and written to the exchange afterwards.
  // Declared out here because the exchange's fields are published under the mutex
  // before that branch runs -- writing `ex.artNote` in the branch would be writing
  // to a local copy, which is exactly the bug this shape avoids.
  std::string artNote;
  Applied applied;

  // Four combinations, because the two axes are genuinely independent: kind picks the
  // document and the validator, shape picks how many definitions one reply carries. Written
  // as two nested branches rather than a table so each arm reads as what it does.
  if (designMode == SpriteKinds::Kind::Object && finished.wasSet) {
    ObjectFactory *objects = Objects();
    if (objects == nullptr) {
      reason = "there is no object factory to add to.";
    } else {
      const ObjectDesigner::SetApplyResult set = ObjectDesigner::ApplySetReply(
          finished.ParsedAnswer(), *objects,
          ModeOverlayDirectoryFor(SpriteKinds::Kind::Object),
          game != nullptr ? &game->tileRegistry : nullptr);
      applied.wasSet = true;
      applied.setName = set.setName;
      applied.ok = set.ok && set.accepted > 0;
      applied.error = set.ok ? std::string() : set.error;
      for (const ObjectDesigner::ApplyResult &entry : set.sprites) {
        if (entry.ok) applied.acceptedNames.push_back(entry.name);
        applied.entryVerdicts.emplace_back(
            entry.ok, entry.ok ? ("'" + entry.name + "'") : entry.error);
        for (const std::string &warning : entry.warnings) {
          applied.warnings.push_back(entry.name + ": " + warning);
        }
      }
      if (!set.ok) {
        applied.entryVerdicts.clear();
      }
      // The last accepted definition is what the object preview shows, matching the tile
      // pane's "the most recent thing this attempt produced".
      for (const ObjectDesigner::ApplyResult &entry : set.sprites) {
        if (!entry.ok) continue;
        applied.objectDefinition = entry.definition;
        applied.objectKey = entry.key;
        applied.objectOverlayPath = entry.savedPath;
        // Both, because they mean different things here: `savedPath` is what the log and the
        // transcript report, and `objectOverlayPath` is what the object pane shows. The
        // object-only field was set and this one was not, so a set logged as "(not saved)"
        // while its files were on disk.
        applied.savedPath = entry.savedPath;
        applied.name = entry.name;
      }
    }
  } else if (designMode == SpriteKinds::Kind::Tile && finished.wasSet) {
    TileRegistry *registry = game != nullptr ? &game->tileRegistry : nullptr;
    if (registry == nullptr) {
      reason = "there is no tile catalog to add to.";
    } else {
      const TileDesigner::SetApplyResult set = TileDesigner::ApplySetReply(
          finished.ParsedAnswer(), *registry,
          ModeOverlayDirectoryFor(SpriteKinds::Kind::Tile));
      applied.wasSet = true;
      applied.setName = set.setName;
      applied.ok = set.ok && set.accepted > 0;
      applied.error = set.ok ? std::string() : set.error;
      for (const TileDesigner::ApplyResult &entry : set.sprites) {
        if (entry.ok) applied.acceptedNames.push_back(entry.name);
        applied.entryVerdicts.emplace_back(
            entry.ok, entry.ok ? ("'" + entry.name + "'") : entry.error);
        for (const std::string &warning : entry.warnings) {
          applied.warnings.push_back(entry.name + ": " + warning);
        }
      }
      if (!set.ok) {
        applied.entryVerdicts.clear();
      }
      for (const TileDesigner::ApplyResult &entry : set.sprites) {
        if (!entry.ok) continue;
        applied.tileDefinition = entry.definition;
        applied.name = entry.name;
        applied.savedPath = entry.savedPath;
      }
    }
  } else if (designMode == SpriteKinds::Kind::Object) {
    ObjectFactory *objects = Objects();
    if (objects == nullptr) {
      reason = "there is no object factory to add to.";
    } else {
      const ObjectDesigner::ApplyResult result = ObjectDesigner::ApplyReply(
          finished.ParsedAnswer(), *objects, ModeOverlayDirectoryFor(
                                              SpriteKinds::Kind::Object),
          game != nullptr ? &game->tileRegistry : nullptr);
      applied.ok = result.ok;
      applied.error = result.error;
      applied.name = result.name;
      applied.savedPath = result.savedPath;
      applied.notes = result.notes;
      applied.warnings = result.warnings;
      applied.saveFailed = result.saveFailed;
      applied.revised = result.revised;
      applied.objectDefinition = result.definition;
      applied.objectKey = result.key;
      applied.objectOverlayPath = result.savedPath;
    }
  } else {
    TileRegistry *registry = game != nullptr ? &game->tileRegistry : nullptr;
    if (registry == nullptr) {
      reason = "there is no tile catalog to add to.";
    } else {
      const TileDesigner::ApplyResult result = TileDesigner::ApplyReply(
          finished.ParsedAnswer(), *registry,
          ModeOverlayDirectoryFor(SpriteKinds::Kind::Tile));
      applied.ok = result.ok;
      applied.error = result.error;
      applied.name = result.name;
      applied.savedPath = result.savedPath;
      applied.notes = result.notes;
      applied.warnings = result.warnings;
      applied.saveFailed = result.saveFailed;
      applied.tileDefinition = result.definition;
    }
  }

  {
    std::lock_guard<std::mutex> lock(transcriptMutex);
    if (index < exchanges.size()) {
      Exchange &ex = exchanges[index];
      ex.inspected = true;
      ex.ok = applied.ok;
      ex.failureReason =
          applied.ok ? std::string() : reason.empty() ? applied.error : reason;
      ex.tileName = applied.name;
      ex.savedPath = applied.savedPath;
      ex.notes = applied.notes;
      ex.warnings = applied.warnings;
      ex.saveFailed = applied.saveFailed;
      ex.definition = applied.tileDefinition;
      ex.wasSet = applied.wasSet;
      ex.setName = applied.setName;
      ex.acceptedNames = applied.acceptedNames;
      ex.entryVerdicts = applied.entryVerdicts;
    }
  }

  // The preview, on the accepted branch only. For a tile this is the pointer the editor
  // writes through; for an object it is a copy, because ObjectFactory owns its templates
  // by value and a pointer into that map is invalidated by the next runtime add.
  if (applied.ok) {
    if (designMode == SpriteKinds::Kind::Object) {
      editedObject = applied.objectDefinition;
      editedObjectKey = applied.objectKey;
      editedObjectOverlayPath = applied.objectOverlayPath;
    } else {
      edited = const_cast<TileDef *>(applied.tileDefinition);
      editedIsRuntime = true;
      if (edited != nullptr) {
        LoadEditorFrom(*edited);
      }
    }
  }

  // The conversation both sides saw, so a follow-up can refer to this tile. The
  // assistant turn is the raw reply; the capped replay is what keeps a large
  // definition from accumulating.
  history.push_back({"user", finished.prompt});
  history.push_back({"assistant", finished.HistoryAnswer()});

  if (applied.ok) {
    if (applied.wasSet) {
      // Names every accepted entry: the point of a set is *which* sprites it produced, and a
      // single "ADDED: <last one>" line would hide the rest.
      LogEvent("ADDED SET" +
               (applied.setName.empty() ? std::string()
                                        : " '" + applied.setName + "'") +
               ": " + std::to_string(applied.acceptedNames.size()) + " of " +
               std::to_string(applied.entryVerdicts.size()) + " definition(s)");
      for (size_t entry = 0; entry < applied.entryVerdicts.size(); ++entry) {
        const bool accepted = applied.entryVerdicts[entry].first;
        Log("  [" + std::to_string(entry) + "] " +
            (accepted ? "ACCEPTED " : "REFUSED  ") + applied.entryVerdicts[entry].second +
            "\n");
      }
    } else {
      LogEvent("ADDED: " + applied.name +
               (applied.savedPath.empty() ? " (not saved)"
                                          : " -> " + applied.savedPath));
    }
    if (!applied.notes.empty()) {
      Log("designerNotes: " + applied.notes + "\n");
    }
    for (const std::string &warning : applied.warnings) {
      Log("warning: " + warning + "\n");
    }
    if (game != nullptr && game->debugLog) {
      game->debugLog->LogInfo(std::string("Sprite designer: added ") +
                              SpriteKinds::IdOf(designMode) + " '" + applied.name +
                              "'" +
                              (applied.savedPath.empty()
                                   ? ""
                                   : " to " + applied.savedPath) +
                              (applied.revised ? " (revision of an earlier design)" : ""));
    }

    // The prompt-driven half: with this on, naming a tile is the whole instruction
    // and the art follows the definition without a second click. It is asked for
    // here, on the main thread, immediately after the tile exists -- which is the
    // only moment a definition is guaranteed to be in the catalog for the art to
    // attach to.
    //
    // The queue batches it, so "design four tiles in a row" is still one request.
    //
    // Every branch below says something, including the one where nothing is asked
    // for. A disabled feature whose only symptom is the absence of a sprite is
    // indistinguishable from a broken one, and the earlier version of this block was
    // silent whenever the toggle was off -- so a user who never found the checkbox
    // saw exactly that.
    // Both kinds are requested the same way: the art queue generates a sprite for a
    // *name*, and the convention id it resolves is the kind's own (tile_<slug> for a
    // tile, <slug> for an object). So this needs no per-kind branch -- which is the point
    // of `TileArt::PlanFor` taking the convention from the job rather than deriving it.
    // The names to ask art for: one for a single reply, every accepted entry for a set. The
    // queue batches them and splits at `maxCellsPerRequest`, so a set of ten becomes several
    // requests without this code knowing or caring about the cap.
    std::vector<std::string> artNames = applied.acceptedNames;
    if (artNames.empty() && !applied.name.empty()) {
      artNames.push_back(applied.name);
    }

    if (game != nullptr && !artNames.empty() && DslArtSourceActive()) {
      // The DSL source has nothing to compile until a document exists, so the
      // automatic path asks for that document instead of queueing a compile. A set
      // asks for its first entry: one conversation is in flight at a time, and
      // saying so beats drawing one entry of a set and staying quiet.
      if (!autoRequestArt) {
        artNote = "no sprite document was requested (turn on \"Art "
                  "automatically\" to ask for it as tiles are accepted)";
      } else {
        const std::string &subject = artNames.front();
        RequestSpriteDocument(subject, designMode);
        artNote = "asked for a sprite document for '" + subject +
                  "' -- it is compiled before anything is written";
        if (artNames.size() > 1) {
          artNote += "; the other " + std::to_string(artNames.size() - 1) +
                     " of the set need their own request";
        }
        LogEvent("SPRITE DOCUMENT REQUESTED (automatic): " + subject);
      }
    } else if (game != nullptr && !artNames.empty()) {
      const TileArtQueue &queue = game->TileArtQueueState();
      const std::string unavailable = queue.UnavailableReason();

      if (!autoRequestArt) {
        artNote = unavailable.empty()
                      ? "no art was requested (turn on \"Art automatically\" to ask "
                        "for it as tiles are accepted)"
                      : "no art was requested -- " + unavailable;
      } else if (!unavailable.empty()) {
        // Refused before spending anything, and the reason is actionable.
        artNote = "no art was requested -- " + unavailable;
        LogEvent("ART NOT REQUESTED: " + unavailable);
      } else {
        // The estimate, worked out from the number of accepted sprites and the cap rather
        // than guessed: one request holds at most `maxCellsPerRequest` sprites, so the count
        // is ceil(n / cap) and the money is that times the measured per-request price.
        //
        // Shown for a single sprite too, where it is always "1 request" -- because the point
        // of the line is that the *cost of a set* is visible before it is spent, and a line
        // that only appears for sets would make a set feel like the exceptional case.
        const int cap = std::max(1, queue.GetConfig().maxCellsPerRequest);
        const int requests =
            (static_cast<int>(artNames.size()) + cap - 1) / cap;
        char estimate[160];
        std::snprintf(estimate, sizeof(estimate),
                      "%zu sprite(s) will take %d image request(s) at a cap of %d per "
                      "request (~$%.2f)",
                      artNames.size(), requests, cap, 0.039 * requests);

        std::vector<std::string> accepted;
        std::vector<std::string> refused;
        for (const std::string &name : artNames) {
          std::string requestReason;
          if (game->RequestTileArt(name, &requestReason)) {
            accepted.push_back(name);
          } else {
            refused.push_back(name + " (" + requestReason + ")");
          }
        }

        if (!accepted.empty()) {
          // The delay is named because the default batch window is two seconds:
          // "a later frame" would read as "any moment now" and look like a hang.
          char window[48];
          std::snprintf(window, sizeof(window), "%.0f",
                        static_cast<double>(queue.GetConfig().batchDelaySeconds));
          artNote = estimate;
          artNote += std::string(". Batched for up to ") + window +
                     "s, then generated on a worker thread; sprites appear as they "
                     "arrive.";
          LogEvent(std::string("ART REQUESTED (automatic): ") + estimate);
          for (const std::string &name : accepted) {
            Log("  queued: " + name + "\n");
          }
        }
        if (!refused.empty()) {
          if (artNote.empty()) artNote = "no art was requested -- ";
          else artNote += " ";
          artNote += std::to_string(refused.size()) + " refused";
          LogEvent("ART NOT REQUESTED for " + std::to_string(refused.size()) +
                   " sprite(s):");
          for (const std::string &line : refused) {
            Log("  " + line + "\n");
          }
          // The reason for the first refusal is the one worth showing in the transcript: a
          // cap or a disabled feature explains all of them.
          if (accepted.empty() && !refused.empty()) {
            const size_t open = refused.front().find('(');
            artNote = "no art was requested -- " +
                      (open == std::string::npos
                           ? std::string("see the log")
                           : refused.front().substr(open + 1,
                                                    refused.front().size() - open - 2));
          }
        }
      }
    }
    // Published on the exchange now that it has been worked out, in the same place
    // the other post-inspection fields are written.
    {
      std::lock_guard<std::mutex> lock(transcriptMutex);
      if (index < exchanges.size()) {
        exchanges[index].artNote = artNote;
      }
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
      game->debugLog->LogWarning(std::string("Sprite designer: giving up on a ") +
                                 SpriteKinds::IdOf(designMode) + " after " +
                                 std::to_string(attempts) +
                                 " attempt(s); last reason: " + applied.error);
    }
    return;
  }

  LogEvent("RETRYING (" + std::to_string(attempts) + " of " +
           std::to_string(maxAttempts) + " used)");

  Exchange retry;
  // The same shape as the attempt being retried. Without this a set is retried as a single
  // definition, refused again for having no top-level "name", and burns the whole attempt
  // budget on a question the model already answered -- which is exactly what the first run of
  // this did.
  retry.wasSet = finished.wasSet;
  retry.prompt = std::string("That ") + SpriteKinds::IdOf(designMode) +
                 " was refused: " + applied.error +
                 "\n\nSend the corrected " + SpriteKinds::IdOf(designMode) +
                 " as a complete ```json block, with designerNotes. Do not describe the "
                 "fix in prose instead of sending it.";
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

const SpriteKinds::Descriptor *TileDesignerWindow::ModeDescriptor() const {
  // Address of the table row. The table is a function-local static, so the pointer is
  // stable for the run and a caller may hold it.
  return &SpriteKinds::For(mode);
}

const char *TileDesignerWindow::ModeName() const {
  // A literal rather than the descriptor's `id`, because this is used in log lines and
  // prose that outlive a mode change and a dangling reference into the table would be a
  // worse bug than a missing abbreviation.
  return mode == SpriteKinds::Kind::Object ? "object" : "tile";
}

std::string TileDesignerWindow::ModeOverlayDirectoryFor(SpriteKinds::Kind kind) const {
  // Read from the settings rather than from the descriptor's default: the descriptor
  // carries the default so a caller with no settings can still name a path, and this is
  // the caller that has settings.
  const GameSettings &settings = Settings();
  return kind == SpriteKinds::Kind::Object ? settings.content.generatedObjects
                                           : settings.content.generatedTiles;
}

std::string TileDesignerWindow::ModeOverlayDirectory() const {
  return ModeOverlayDirectoryFor(mode);
}

ObjectFactory *TileDesignerWindow::Objects() const {
  return game != nullptr ? &game->objectFactory : nullptr;
}

void TileDesignerWindow::RefreshRewriteBrief() {
  // The Improve button has to speak the vocabulary of the kind being designed, or an
  // "improved" object request comes back enriched with ground-surface words and the
  // designer has no way to honour it. See RewriteBriefs.hpp.
  rewriter.SetBrief(mode == SpriteKinds::Kind::Object ? RewriteBriefs::Object()
                                                      : RewriteBriefs::Tile());
}

void TileDesignerWindow::RequestRoute(const std::string &text) {
  if (game == nullptr || game->systemOneClassifier == nullptr) {
    routeNote = "no System 1 classifier is configured, so the request cannot be read. "
                "Pick the kind with the dropdown instead.";
    return;
  }
  if (routeInFlight) return;
  if (text.empty()) {
    routeNote = "type a request first -- there is nothing to classify yet.";
    return;
  }

  // The object vocabulary, so the classifier can judge "a crate that holds things" the
  // same way the object prompt would. Built from the factory, which is the only source
  // that cannot drift from what the designer may write.
  SpriteRouter::Context context;
  if (const ObjectFactory *objects = Objects()) {
    std::string list;
    for (const std::string &name : objects->ComponentNames()) {
      if (!list.empty()) list += ", ";
      list += name;
    }
    context.objectVocabulary = list;
  }

  routeInFlight = true;
  routeNote = "asking which kind this is...";
  LogEvent("ROUTE REQUESTED: " + text);

  // The callback runs on the classifier's own thread. It only latches the verdict; `Draw`
  // picks it up and applies it on the main thread. Touching ImGui, the ECS or the
  // transcript from here is exactly what this shape exists to prevent.
  //
  // The provider guarantees the callback fires exactly once -- transport failure, HTTP
  // error, unparseable body and cancellation included -- so `routeInFlight` cannot stick.
  //
  // The active profile's threshold, if there is one. Read here rather than inside the
  // router because it belongs to the *model* that is serving, and the router is handed a
  // bare classifier with no idea which profile built it.
  const SystemOne::ModelProfile *profile =
      game != nullptr ? game->ActiveSystemOneProfile() : nullptr;
  const float minConfidence = profile != nullptr ? profile->minConfidence : 0.0f;

  SpriteRouter::Classify(
      *game->systemOneClassifier, text, context,
      [this](const SpriteRouter::Verdict &verdict) {
        std::lock_guard<std::mutex> lock(routeMutex);
        routeVerdict = verdict;
        routeInFlight = false;
      },
      /*stoken=*/{}, /*minConfidence=*/minConfidence);
}

void TileDesignerWindow::ApplyRouteVerdict(const SpriteRouter::Verdict &verdict) {
  if (verdict.unsure) {
    // The answer arrived but is below the active profile's threshold. The mode stays
    // where it is -- switching on a coin flip is worse than doing nothing -- and the
    // window says what it read so the threshold itself can be judged.
    routeNote = "the classifier was not confident enough to act: " + verdict.why;
    LogEvent("ROUTE: unsure -- " + routeNote);
    return;
  }

  if (verdict.kind == nullptr) {
    // Unanswered is not a verdict. The mode stays where it is, and the window says why
    // rather than picking the more likely kind.
    routeNote = verdict.error.empty()
                    ? "the classifier did not say which kind this is, so the mode is "
                      "unchanged"
                    : "the classifier could not be used: " + verdict.error;
    LogEvent("ROUTE: no kind -- " + routeNote);
    return;
  }

  mode = verdict.kind->kind;
  RefreshRewriteBrief();
  if (game != nullptr) {
    systemPrompt = game->BuildSpriteSystemPrompt(mode, requestSet);
  }

  // The shape too, when the classifier answered it -- that is what the question is for. An
  // unanswered shape leaves the checkbox alone rather than being guessed, for the same reason
  // an unanswered kind leaves the dropdown alone.
  if (verdict.shapeAnswered) {
    requestSet = verdict.sheet;
    requestSetSize = verdict.requested;
    if (game != nullptr) {
      systemPrompt = game->BuildSpriteSystemPrompt(mode, requestSet);
    }
  }

  routeNote = "the classifier read this as " + verdict.kind->label;
  if (verdict.shapeAnswered) {
    routeNote += verdict.sheet
                     ? ", a set of about " + std::to_string(verdict.requested)
                     : ", a single sprite";
  }
  // The confidence and the model's own words are shown rather than only the conclusion: a
  // classifier that is confidently wrong should be visibly confident.
  if (verdict.kindConfidence > 0.0f) {
    routeNote += " (confidence " + std::to_string(verdict.kindConfidence) + ")";
  }
  if (!verdict.why.empty()) {
    routeNote += " -- " + verdict.why;
  }
  LogEvent("ROUTE: " + verdict.kind->id + " -- " + routeNote);
}

void TileDesignerWindow::DrawObjectPreview() {
  // Read-only by design, for this phase. An object is a component set, a palette and an
  // art block, and an editor for all four is a larger surface than phase 5 needs to
  // claim; what matters here is that the accepted definition is *visible*, so a model
  // that produced something plausible but wrong is caught by eye.
  if (!editedObject.is_object()) {
    ImGui::TextWrapped(
        "The object from the latest reply appears here. An object is a set of "
        "capabilities, a glyph and an ASCII art block -- it has no art from the model "
        "in this step.");
    return;
  }

  ImGui::Text("%s", editedObject.value("name", std::string("(unnamed)")).c_str());
  ImGui::SameLine();
  ImGui::TextDisabled("(key %s)", editedObjectKey.c_str());
  if (editedObject.contains("description")) {
    ImGui::TextWrapped("%s",
                       editedObject.value("description", std::string()).c_str());
  }

  ImGui::Separator();
  ImGui::TextDisabled("Capabilities");

  // The components, with the parameter each one was given, read back out of the
  // definition. This is the object's real design content: a preview that showed only its
  // glyph would hide everything that makes it an object rather than a tile.
  const ObjectFactory *objects = Objects();
  if (editedObject.contains("components") && editedObject["components"].is_array()) {
    for (const auto &component : editedObject["components"]) {
      if (!component.is_string()) continue;
      const std::string name = component.get<std::string>();
      ImGui::BulletText("%s", name.c_str());

      // The parameter names come from the factory, so the preview lists exactly what the
      // setter will read rather than a hand-written copy that can fall behind.
      if (objects != nullptr) {
        for (const ObjectFactory::ComponentInfo &info : objects->ComponentVocabulary()) {
          if (info.name != name) continue;
          for (const std::string &parameter : info.parameters) {
            const size_t dot = parameter.find('.');
            const std::string top = dot == std::string::npos
                                        ? parameter
                                        : parameter.substr(0, dot);
            if (editedObject.contains(top)) {
              ImGui::TextDisabled("    %s: %s", top.c_str(),
                                  editedObject[top].dump().c_str());
            }
          }
          break;
        }
      }
    }
  }

  ImGui::Separator();
  ImGui::TextDisabled("Appearance");

  // The look, exactly as the world would draw it: the template's sprite, or the
  // Unknown placeholder. There is no glyph and no art block to preview any more --
  // both were removed from rendering (docs/appearance.md) -- so showing them here
  // would promise a look the world will not deliver.
  const std::string objectName =
      editedObject.value("name", editedObjectKey);
  const Texture2D *look = nullptr;
  if (game != nullptr) {
    std::string explicitSprite;
    if (editedObject.contains("sprite") && editedObject["sprite"].is_string()) {
      explicitSprite = editedObject["sprite"].get<std::string>();
    }
    look = ResolveSpriteTexture(explicitSprite, editedObjectKey,
                                game->resourceManager, game->debugLog.get());
    if (look == nullptr) {
      look = game->resourceManager.GetTexture(UnknownSpritePath());
    }
  }

  // Is there a sprite at all? The placeholder is what the world shows when there is
  // not, and saying which of the two is on screen is the whole point of the pane.
  std::string resolved;
  const bool hasSprite =
      game != nullptr && SpriteResolves("", editedObjectKey, &resolved);

  const float lookSize = 64.0f;
  ImGui::Dummy(ImVec2(lookSize, lookSize));
  if (look != nullptr) {
    DrawSpriteImGui(*look, ImGui::GetItemRectMin(),
                    ImVec2(lookSize, lookSize));
  }
  ImGui::SameLine();
  ImGui::BeginGroup();
  if (hasSprite) {
    ImGui::TextDisabled("drawn as %s", resolved.c_str());
  } else {
    ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f),
                       "no sprite yet -- drawn as the Unknown placeholder");
  }

  const bool objectDsl = DslArtSourceActive();
  if (game != nullptr) {
    const TileArtQueue &objectQueue = game->TileArtQueueState();
    const bool objectGenerating =
        objectQueue.CountInState(TileArtQueue::State::Queued) > 0 ||
        objectQueue.CountInState(TileArtQueue::State::Running) > 0;
    ImGui::BeginDisabled(objectGenerating || !objectQueue.Enabled());
    if (ImGui::Button(objectDsl ? "Describe art (DSL)" : "Generate art")) {
      if (objectDsl) {
        RequestSpriteDocument(objectName, SpriteKinds::Kind::Object);
      } else {
        std::string reason;
        // Explicit kind: this pane means the object even if a tile shares its name.
        if (game->RequestSpriteArt(SpriteKinds::Kind::Object, objectName,
                                   &reason)) {
          statusIsError = false;
          statusLine = "Queued art for '" + objectName + "'.";
        } else {
          statusIsError = true;
          statusLine = reason;
        }
      }
    }
    ImGui::EndDisabled();
  }
  ImGui::EndGroup();

  if (objectDsl) {
    if (!artVerdict.empty()) {
      ImGui::TextWrapped("last attempt: %s", artVerdict.c_str());
    }
    DrawDslPanels(objectName, SpriteKinds::Kind::Object);
  }

  if (!editedObjectOverlayPath.empty()) {
    ImGui::TextDisabled("written to %s", editedObjectOverlayPath.c_str());
  } else {
    ImGui::TextDisabled("not written to disk -- it exists for this session only");
  }
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

void TileDesignerWindow::DrawDslPanels(const std::string &name,
                                       SpriteKinds::Kind kind) {
  const std::string conventionId = SpriteKinds::ConventionIdFor(kind, name);
  const SpriteDSLDocument document = FindSpriteDSLDocument(conventionId);

  if (!document.found) {
    ImGui::Separator();
    ImGui::TextDisabled("No sprite document for this tile yet.");
    const std::string destination = GeneratedSpriteDocumentPath(conventionId);
    if (!destination.empty()) {
      ImGui::TextWrapped("One would be written to:\n%s", destination.c_str());
    }
    ImGui::TextWrapped(
        "\"Describe art (DSL)\" above asks the designer for one. The compiler "
        "decides whether it is accepted, and nothing is written until it compiles.");
    return;
  }

  // Read + compile only when the file changed. This runs every frame, and the pane's
  // whole job is to show a file that changes rarely -- a per-frame compile would be
  // affordable but pointless, and a per-frame *read* would fight the file lock.
  std::error_code stampError;
  const auto stamp = std::filesystem::last_write_time(document.path, stampError);
  const std::string key =
      document.path + ":" +
      std::to_string(stampError ? 0 : stamp.time_since_epoch().count());
  if (key != dslCacheKey) {
    dslCacheKey = key;
    std::ifstream input(document.path);
    std::stringstream buffer;
    buffer << input.rdbuf();
    dslText = buffer.str();
    PixelDSL::Config config;
    config.palettePath.clear();
    dslResult = PixelDSL::Compile(dslText, config);
    dslCompiled = dslResult.ok;
  }

  ImGui::Separator();
  ImGui::TextDisabled("%s", document.path.c_str());
  if (document.inArtPack) {
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f),
                       "(art pack: hand-authored, never overwritten)");
  }

  const TileArtQueue &queue = game->TileArtQueueState();
  const bool generating = queue.CountInState(TileArtQueue::State::Queued) > 0 ||
                          queue.CountInState(TileArtQueue::State::Running) > 0;
  ImGui::BeginDisabled(generating || !queue.Enabled());
  if (ImGui::Button("Recompile")) {
    std::string reason;
    if (game->RecompileSpriteArt(kind, name, &reason)) {
      statusIsError = false;
      statusLine = "Compiling " + document.path + ".";
    } else {
      statusIsError = true;
      statusLine = reason;
    }
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(document.inArtPack);
  if (ImGui::Button("Ask again")) {
    RequestSpriteDocument(name, kind, /*replaceGenerated=*/true);
  }
  ImGui::EndDisabled();
  ImGui::EndDisabled();
  if (document.inArtPack) {
    ImGui::SameLine();
    ImGui::TextDisabled("(Ask again is off: the pack's document is not ours)");
  }

  // Staleness: a document edited after its sprite was compiled must not be shown next
  // to a sprite that no longer matches it.
  const TileArt::Target target =
      TileArt::PlanFor(TileArt::Request{kind, name});
  if (!target.generatedPath.empty() && !stampError) {
    std::error_code pngError;
    const auto pngStamp =
        std::filesystem::last_write_time(target.generatedPath, pngError);
    if (!pngError && stamp > pngStamp) {
      ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                         "The document is newer than the sprite; press Recompile.");
    }
  }

  if (ImGui::CollapsingHeader("Document", ImGuiTreeNodeFlags_DefaultOpen)) {
    int lines = 1;
    for (const char c : dslText) {
      if (c == '\n') ++lines;
    }
    ImGui::TextDisabled("%d line(s), %d byte(s)", lines,
                        static_cast<int>(dslText.size()));
    // Read-only by construction rather than by a flag: a text box the user can type
    // into would be a second, worse editor for a format with a compiler.
    ImGui::BeginChild("##dsl_document", ImVec2(0, 150), true,
                      ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::TextUnformatted(dslText.c_str());
    ImGui::EndChild();
  }

  if (ImGui::CollapsingHeader("Verdict", ImGuiTreeNodeFlags_DefaultOpen)) {
    for (const PixelDSL::Diagnostic &diagnostic : dslResult.diagnostics) {
      const bool error = diagnostic.severity == PixelDSL::Severity::Error;
      ImGui::PushStyleColor(ImGuiCol_Text,
                            error ? ImVec4(1.0f, 0.45f, 0.4f, 1.0f)
                                  : ImVec4(1.0f, 0.85f, 0.45f, 1.0f));
      if (diagnostic.line > 0) {
        ImGui::TextWrapped("line %d: %s", diagnostic.line,
                           diagnostic.message.c_str());
      } else {
        ImGui::TextWrapped("%s", diagnostic.message.c_str());
      }
      ImGui::PopStyleColor();
    }
    if (dslResult.diagnostics.empty()) {
      ImGui::TextDisabled("no diagnostics");
    }
    ImGui::Separator();
    for (const std::string &line : dslResult.summary) {
      ImGui::TextDisabled("%s", line.c_str());
    }
  }

  if (ImGui::CollapsingHeader("Sprite", ImGuiTreeNodeFlags_DefaultOpen)) {
    if (!dslCompiled || dslResult.frames.empty()) {
      ImGui::TextDisabled("not compiled -- see the verdict above");
    } else {
      const PixelDSL::Frame &frame = dslResult.frames.front();
      const float cell = 7.0f;
      const ImVec2 origin = ImGui::GetCursorScreenPos();
      const ImVec2 size(frame.width * cell, frame.height * cell);
      ImDrawList *draw = ImGui::GetWindowDrawList();
      // A flat plate first, so transparent pixels read as "nothing here" rather than
      // as the window background showing through a hole.
      draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y),
                          IM_COL32(32, 32, 38, 255));
      for (int y = 0; y < frame.height; ++y) {
        for (int x = 0; x < frame.width; ++x) {
          const size_t index =
              (static_cast<size_t>(y) * frame.width + x) * 4;
          const unsigned char alpha = frame.rgba[index + 3];
          if (alpha == 0) continue;
          const ImVec2 topLeft(origin.x + x * cell, origin.y + y * cell);
          draw->AddRectFilled(
              topLeft, ImVec2(topLeft.x + cell, topLeft.y + cell),
              IM_COL32(frame.rgba[index], frame.rgba[index + 1],
                       frame.rgba[index + 2], alpha));
        }
      }
      ImGui::Dummy(size);
      ImGui::TextDisabled("%dx%d, one rectangle per pixel", frame.width,
                          frame.height);
    }
  }
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

  if (DslArtSourceActive() && !artVerdict.empty()) {
    // The *last attempt's* verdict, which is not the same thing as the on-disk
    // document's: a refused attempt never wrote a file, so the panes below have
    // nothing to show and this is the only trace of why.
    ImGui::Separator();
    ImGui::TextWrapped("last attempt: %s", artVerdict.c_str());
  }

  const bool generating = queue.CountInState(TileArtQueue::State::Queued) > 0 ||
                          queue.CountInState(TileArtQueue::State::Running) > 0;
  const bool dslSource = DslArtSourceActive();
  ImGui::BeginDisabled(!editedIsRuntime || generating || !queue.Enabled());
  if (ImGui::Button(dslSource ? "Describe art (DSL)" : "Generate art")) {
    if (dslSource) {
      // The tile pane, so the kind is Tile even when the dropdown is on object: the
      // definition being edited is a TileDef and there is no object editor here.
      RequestSpriteDocument(definition.name, SpriteKinds::Kind::Tile);
    } else {
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

  // The DSL source has no pipeline to show: its document *is* the pipeline's
  // input, and its compile verdict is the output that can be wrong. The three views
  // below describe sheet generation and do not apply, so they are replaced rather
  // than hidden -- a "Pipeline stages" header over an empty pane reads as a bug.
  if (DslArtSourceActive()) {
    DrawDslPanels(definition.name, SpriteKinds::Kind::Tile);
    return;
  }

  // The three views of the pipeline, in the order the pipeline runs. Each is
  // hidden when it has nothing to show rather than drawn empty, so a tile that has
  // never been generated looks the same as before this existed.
  const std::optional<TileArtQueue::Job> job = JobForEditedTile();
  if (!job.has_value()) return;

  if (ImGui::CollapsingHeader("Pipeline artifacts", ImGuiTreeNodeFlags_DefaultOpen)) {
    DrawArtifactsPanel(*job);
  }
  if (ImGui::CollapsingHeader("Pixels and palette", ImGuiTreeNodeFlags_DefaultOpen)) {
    DrawPixelViewPanel(*job);
  }
  if (ImGui::CollapsingHeader("Pipeline stages", ImGuiTreeNodeFlags_DefaultOpen)) {
    DrawFilmstripPanel(*job);
  }
}

std::optional<TileArtQueue::Job> TileDesignerWindow::JobForEditedTile() const {
  if (game == nullptr || edited == nullptr) return std::nullopt;
  for (const TileArtQueue::Job &candidate : game->TileArtQueueState().Snapshot()) {
    if (candidate.tileName == edited->name) {
      return candidate;
    }
  }
  return std::nullopt;
}

int TileDesignerWindow::CellIndexInWorklist(const TileArtQueue::Job &job) const {
  if (job.worklistPath.empty() || edited == nullptr) return -1;

  // Keyed on the file's modification time as well as its path: a second generation
  // reuses the same filename, so the path alone would keep serving the first batch's
  // answer.
  std::error_code ec;
  const auto stamp = std::filesystem::last_write_time(job.worklistPath, ec);
  const std::string key = job.worklistPath + ":" + std::to_string(ec ? 0 : stamp.time_since_epoch().count());
  if (key == worklistCellCacheKey) return worklistCellIndex;

  worklistCellCacheKey = key;
  worklistCellIndex = -1;

  std::ifstream in(job.worklistPath);
  if (!in.is_open()) return worklistCellIndex;
  try {
    nlohmann::json worklist;
    in >> worklist;
    const auto &tiles = worklist.value("tiles", nlohmann::json::array());
    for (size_t index = 0; index < tiles.size(); ++index) {
      if (tiles[index].is_string() && tiles[index].get<std::string>() == edited->name) {
        worklistCellIndex = static_cast<int>(index);
        break;
      }
    }
  } catch (const std::exception &) {
    // A malformed worklist leaves the index at -1, so the panel shows the sheet and
    // the sprite and simply omits the per-cell stages rather than guessing.
  }
  return worklistCellIndex;
}

std::vector<TileDesignerWindow::Artifact> TileDesignerWindow::ArtifactsFor(
    const TileArtQueue::Job &job) const {
  std::vector<Artifact> artifacts;
  if (job.worklistPath.empty()) return artifacts;

  const std::filesystem::path worklist(job.worklistPath);
  const std::filesystem::path workDir = worklist.parent_path();

  auto add = [&artifacts](const std::string &label, const std::filesystem::path &path,
                          bool isImage) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return;
    Artifact artifact;
    artifact.label = label;
    artifact.path = path.string();
    artifact.isImage = isImage;
    artifacts.push_back(std::move(artifact));
  };

  // The prompt and the sheet: what was asked, and what came back. Both live beside
  // the worklist by the tool's convention (tools/gen_tile_art.py), which is why the
  // job records the worklist path rather than these directly.
  add("prompt sent", workDir / "tile_art_sheet.prompt.txt", false);
  add("raw sheet", workDir / "tile_art_sheet.png", true);

  // The per-stage cell images for *this* tile's cell.
  //
  // Only its own cell, which needs the tile's position in the batch: the tool numbers
  // the cell images `cell_<index>_<stage>.png` by position within one sheet, and a
  // later batch overwrites those files. Showing every cell that happens to be in the
  // directory is how this panel came to present another tile's stages as this one's --
  // and reading them straight off disk meant a regenerated tile appeared to have
  // unchanged stages, because the panel was showing whatever was there.
  const int cell = CellIndexInWorklist(job);
  if (cell >= 0) {
    for (const char *suffix : {"crop", "trim", "locked"}) {
      add(std::string("cell ") + std::to_string(cell) + " " + suffix,
          workDir / ("cell_" + std::to_string(cell) + "_" + suffix + ".png"), true);
    }
  }

  add("placed sprite", job.path, true);
  return artifacts;
}

void TileDesignerWindow::DrawArtifactsPanel(const TileArtQueue::Job &job) {
  const std::vector<Artifact> artifacts = ArtifactsFor(job);
  if (artifacts.empty()) {
    ImGui::TextDisabled(
        "Nothing on disk yet. The sheet, the prompt actually sent and one image "
        "per pipeline stage appear here once a request has been made.");
    if (job.state == TileArtQueue::State::Done) {
      ImGui::TextDisabled(
          "(this tile was generated before, but its work files are gone -- they "
          "live in art.generation.workDirectory and can be deleted)");
    }
    return;
  }

  for (const Artifact &artifact : artifacts) {
    if (ImGui::Button(("copy##" + artifact.path).c_str())) {
      ImGui::SetClipboardText(artifact.path.c_str());
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Copy this path to the clipboard.");
    }
    ImGui::SameLine();
    ImGui::Text("%s", artifact.label.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("%s", artifact.path.c_str());
  }

  // The prompt, which is the thing a disappointing sheet has to be judged against.
  // Read-only text, not editable: editing it here would change nothing, because the
  // next request rebuilds it from the tile definition and the prompt file.
  for (const Artifact &artifact : artifacts) {
    if (artifact.label != "prompt sent") continue;
    if (!ImGui::TreeNode("What was actually asked")) continue;
    std::ifstream in(artifact.path);
    if (in.is_open()) {
      std::stringstream buffer;
      buffer << in.rdbuf();
      // Wrapped and selectable so it can be copied out into a bug report.
      ImGui::TextWrapped("%s", buffer.str().c_str());
    } else {
      ImGui::TextDisabled("could not re-read %s", artifact.path.c_str());
    }
    ImGui::TreePop();
  }
}

void TileDesignerWindow::EnsurePaletteLoaded() {
  if (paletteLoaded) return;
  paletteLoaded = true;  // once, even on failure: a missing file is not retried per frame

  std::ifstream in("data/palette.json");
  if (!in.is_open()) return;
  try {
    nlohmann::json payload;
    in >> payload;
    for (const nlohmann::json &entry : payload.value("colors", nlohmann::json::array())) {
      if (!entry.is_array() || entry.size() < 3) continue;
      paletteColours.push_back({static_cast<unsigned char>(entry[0].get<int>()),
                                static_cast<unsigned char>(entry[1].get<int>()),
                                static_cast<unsigned char>(entry[2].get<int>())});
    }
  } catch (const std::exception &) {
    paletteColours.clear();
  }
}

int TileDesignerWindow::NearestPaletteIndex(unsigned char r, unsigned char g,
                                            unsigned char b) const {
  int best = -1;
  long bestDistance = 0;
  for (size_t i = 0; i < paletteColours.size(); ++i) {
    const long dr = static_cast<long>(r) - paletteColours[i][0];
    const long dg = static_cast<long>(g) - paletteColours[i][1];
    const long db = static_cast<long>(b) - paletteColours[i][2];
    const long distance = dr * dr + dg * dg + db * db;
    if (best < 0 || distance < bestDistance) {
      best = static_cast<int>(i);
      bestDistance = distance;
      if (distance == 0) break;
    }
  }
  return best;
}

void TileDesignerWindow::EnsurePixelView(const std::string &spritePath) {
  if (spritePath.empty()) return;

  // Identity is the path *and* the file's modification time, for the same reason
  // the thumbnails use both: a regeneration overwrites the sprite at the same path,
  // so a path-only check would keep showing the previous generation's pixels and its
  // palette verdict forever.
  std::error_code stampError;
  const auto stamp = std::filesystem::last_write_time(spritePath, stampError);
  const bool hasStamp = !stampError;
  if (pixelViewPath == spritePath && pixelView.texture) {
    // A vanished file (no stamp) invalidates too: `hasStamp` false against a stored
    // stamp is a change in every case that matters.
    const bool current = hasStamp && !(pixelViewStamp < stamp) &&
                         !(stamp < pixelViewStamp);
    if (current) return;
  }

  PixelView fresh;
  std::error_code ec;
  if (!std::filesystem::exists(spritePath, ec)) {
    return;
  }

  // raylib::Image's constructor *throws* on a path it cannot load rather than
  // returning a null image, so a missing or unreadable sprite would otherwise take
  // the window down instead of being reported. Empty paths reach here while a job
  // is queued (it has no sprite yet), which is the common case, not an edge one.
  raylib::Image image;
  try {
    image = raylib::Image(spritePath);
  } catch (const raylib::RaylibException &e) {
    if (game != nullptr && game->debugLog) {
      game->debugLog->LogWarning("Tile art: could not read " + spritePath +
                                 " for the pixel view: " + e.what());
    }
    return;
  }
  if (image.data == nullptr) {
    return;
  }
  // One format for the read-back below, so the pixel loop does not need a branch
  // per source type.
  ImageFormat(&image, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);

  EnsurePaletteLoaded();
  fresh.width = image.width;
  fresh.height = image.height;
  const size_t count = static_cast<size_t>(image.width) * image.height;
  fresh.pixels.resize(count * 4);
  std::memcpy(fresh.pixels.data(), image.data, count * 4);

  // The measurement the placement check makes in Python, done here so it can be
  // *shown*: which colours this sprite is made of and whether any of them is
  // outside the palette.
  std::set<std::tuple<unsigned char, unsigned char, unsigned char>> distinct;
  for (size_t i = 0; i < count; ++i) {
    const unsigned char *pixel = &fresh.pixels[i * 4];
    if (pixel[3] < 128) continue;  // the same alpha threshold palette.py uses
    const auto colour = std::make_tuple(pixel[0], pixel[1], pixel[2]);
    if (!distinct.insert(colour).second) continue;
    if (!paletteColours.empty() &&
        NearestPaletteIndex(pixel[0], pixel[1], pixel[2]) >= 0) {
      // Exactly equal counts as inside; nearest-but-different does not. Compared
      // directly rather than via the nearest index, which would call any colour
      // "inside" whenever a palette exists.
      const auto &match = paletteColours[NearestPaletteIndex(pixel[0], pixel[1], pixel[2])];
      if (match[0] != pixel[0] || match[1] != pixel[1] || match[2] != pixel[2]) {
        ++fresh.outsidePalette;
      }
    }
  }
  fresh.distinctColours = distinct.size();

  // A fingerprint of the pixels themselves, so a cache hit and a reload can be told
  // apart by a caller that cannot read the buffer. FNV-1a: cheap, no allocation, and
  // a collision here would only cost a test one false negative.
  size_t hash = 1469598103934665603ull;
  for (const unsigned char byte : fresh.pixels) {
    hash ^= byte;
    hash *= 1099511628211ull;
  }
  fresh.imageHash = hash;

  fresh.texture = std::make_unique<raylib::Texture>(LoadTextureFromImage(image));
  if (fresh.texture->id != 0) {
    SetTextureFilter(*fresh.texture, TEXTURE_FILTER_POINT);
  }

  // Committed together, and only on a load that produced something. Replacing the
  // cache means the previous texture is unloaded here, which is safe because this
  // runs on the main thread before any of this frame's drawing.
  pixelView = std::move(fresh);
  pixelViewPath = spritePath;
  pixelViewStamp = hasStamp ? stamp : std::filesystem::file_time_type{};
}

void TileDesignerWindow::DrawPixelViewPanel(const TileArtQueue::Job &job) {
  if (job.path.empty()) {
    ImGui::TextDisabled("No sprite to inspect yet.");
    return;
  }
  EnsurePixelView(job.path);
  if (pixelView.pixels.empty()) {
    ImGui::TextDisabled("Could not read %s", job.path.c_str());
    return;
  }

  static bool showNearest = false;
  ImGui::Checkbox("Show nearest palette colour", &showNearest);
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip(
        "Redraw every pixel as the palette entry it would snap to.\n"
        "If the lock worked, this is identical to the sprite; differences are "
        "colours the palette does not contain.");
  }
  ImGui::SameLine();
  ImGui::TextDisabled("%dx%d, %zu opaque colour(s)", pixelView.width,
                      pixelView.height, pixelView.distinctColours);
  if (!paletteColours.empty()) {
    if (pixelView.outsidePalette == 0) {
      ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                         "all colours are in the palette (%zu of %zu entries used)",
                         pixelView.distinctColours, paletteColours.size());
    } else {
      ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f),
                         "%zu colour(s) are OUTSIDE the palette", pixelView.outsidePalette);
    }
  } else {
    ImGui::TextDisabled("no derived palette (data/palette.json) to compare against");
  }

  static float zoom = 8.0f;
  ImGui::SliderFloat("zoom", &zoom, 2.0f, 16.0f, "%.0fx");

  // One rectangle per pixel. 1024 of them at 32x32, which is nothing, and it means
  // the sprite is exact at any zoom -- where a font-glyph rendering of the same tile
  // would blur its 28 colours into mush.
  ImDrawList *drawList = ImGui::GetWindowDrawList();
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const float cell = zoom;
  for (int y = 0; y < pixelView.height; ++y) {
    for (int x = 0; x < pixelView.width; ++x) {
      const unsigned char *pixel =
          &pixelView.pixels[(static_cast<size_t>(y) * pixelView.width + x) * 4];
      unsigned char r = pixel[0], g = pixel[1], b = pixel[2];
      const unsigned char a = pixel[3];
      if (a == 0) {
        // Transparent: a checkerboard, so "nothing here" is visibly different from
        // a dark pixel.
        const bool dark = ((x + y) % 2) == 0;
        drawList->AddRectFilled(
            ImVec2(origin.x + x * cell, origin.y + y * cell),
            ImVec2(origin.x + (x + 1) * cell, origin.y + (y + 1) * cell),
            dark ? IM_COL32(70, 70, 70, 255) : IM_COL32(90, 90, 90, 255));
        continue;
      }
      if (showNearest && !paletteColours.empty()) {
        const int nearest = NearestPaletteIndex(r, g, b);
        if (nearest >= 0) {
          r = paletteColours[nearest][0];
          g = paletteColours[nearest][1];
          b = paletteColours[nearest][2];
        }
      }
      drawList->AddRectFilled(
          ImVec2(origin.x + x * cell, origin.y + y * cell),
          ImVec2(origin.x + (x + 1) * cell, origin.y + (y + 1) * cell),
          IM_COL32(r, g, b, 255));
    }
  }
  ImGui::Dummy(ImVec2(pixelView.width * cell, pixelView.height * cell));

  // The palette itself, with the entries this sprite uses marked. That is the whole
  // point of the panel: "28 of 128" as a number is not the same as seeing which 28.
  if (paletteColours.empty()) return;
  ImGui::Separator();
  ImGui::TextDisabled("palette (%zu entries; boxed ones are used by this sprite)",
                      paletteColours.size());

  std::vector<bool> used(paletteColours.size(), false);
  for (size_t i = 0; i < pixelView.pixels.size(); i += 4) {
    if (pixelView.pixels[i + 3] < 128) continue;
    const int nearest = NearestPaletteIndex(pixelView.pixels[i], pixelView.pixels[i + 1],
                                            pixelView.pixels[i + 2]);
    if (nearest < 0) continue;
    const auto &match = paletteColours[nearest];
    if (match[0] == pixelView.pixels[i] && match[1] == pixelView.pixels[i + 1] &&
        match[2] == pixelView.pixels[i + 2]) {
      used[nearest] = true;
    }
  }

  const float swatch = 12.0f;
  const int perRow = 32;
  ImDrawList *paletteList = ImGui::GetWindowDrawList();
  const ImVec2 paletteOrigin = ImGui::GetCursorScreenPos();
  for (size_t i = 0; i < paletteColours.size(); ++i) {
    const int column = static_cast<int>(i) % perRow;
    const int row = static_cast<int>(i) / perRow;
    const ImVec2 topLeft(paletteOrigin.x + column * (swatch + 2.0f),
                         paletteOrigin.y + row * (swatch + 2.0f));
    paletteList->AddRectFilled(
        topLeft, ImVec2(topLeft.x + swatch, topLeft.y + swatch),
        IM_COL32(paletteColours[i][0], paletteColours[i][1], paletteColours[i][2], 255));
    paletteList->AddRect(topLeft, ImVec2(topLeft.x + swatch, topLeft.y + swatch),
                         used[i] ? IM_COL32(255, 255, 255, 255)
                                 : IM_COL32(40, 40, 40, 255));
  }
  const int rows = (static_cast<int>(paletteColours.size()) + perRow - 1) / perRow;
  ImGui::Dummy(ImVec2(perRow * (swatch + 2.0f), rows * (swatch + 2.0f)));
}

raylib::Texture *TileDesignerWindow::LoadThumbnail(const std::string &path,
                                                   int maxSide) {
  // The file's identity is its path *and* its modification time. A regeneration
  // overwrites the stage files in place, so the path alone is not enough to tell a
  // new image from the one already in the cache -- and the panels would keep showing
  // the previous batch. The stamp is also what lets a replaced image be released:
  // the entry is dropped from both maps so the erase unloads the old texture.
  std::error_code stampError;
  const auto stamp = std::filesystem::last_write_time(path, stampError);
  const bool hasStamp = !stampError;

  const auto cached = thumbnails.find(path);
  if (cached != thumbnails.end()) {
    const auto known = thumbnailStamps.find(path);
    const bool current = (known == thumbnailStamps.end())
                             ? !hasStamp
                             : (hasStamp && known->second == stamp);
    if (current) {
      return cached->second.get();
    }
    // The file changed under the cache. Unload and fall through to reload it.
    thumbnails.erase(cached);
    thumbnailStamps.erase(path);
  }

  std::unique_ptr<raylib::Texture> texture;
  std::error_code ec;
  if (!path.empty() && std::filesystem::exists(path, ec)) {
    raylib::Image image;
    try {
      image = raylib::Image(path);
    } catch (const raylib::RaylibException &) {
      // A stage that will not load draws as "unreadable" in its slot. Cached as a
      // null below, so the failure is reported once rather than re-attempted every
      // frame -- a per-frame stat and a thrown exception is the worst version of
      // this.
      thumbnails[path] = nullptr;
      if (hasStamp) thumbnailStamps[path] = stamp;
      return nullptr;
    }
    if (image.data != nullptr) {
      const int longest = std::max(image.width, image.height);
      if (longest > maxSide && maxSide > 0) {
        const bool bigReduction = longest >= maxSide * 4;
        if (bigReduction) {
          ImageResizeNN(&image, image.width * maxSide / longest,
                        image.height * maxSide / longest);
        } else {
          ImageResize(&image, image.width * maxSide / longest,
                      image.height * maxSide / longest);
        }
      }
      texture = std::make_unique<raylib::Texture>(LoadTextureFromImage(image));
      if (texture->id != 0) {
        SetTextureFilter(*texture, TEXTURE_FILTER_POINT);
      }
    }
  }
  // Stored even when null: a missing stage is the normal state for a tile whose
  // files have been cleaned, and re-attempting the load every frame would stat the
  // filesystem 60 times a second to learn the same thing. The stamp goes with it so
  // a file that later appears -- or is regenerated -- is noticed.
  raylib::Texture *raw = texture.get();
  thumbnails[path] = std::move(texture);
  if (hasStamp) {
    thumbnailStamps[path] = stamp;
  } else {
    thumbnailStamps.erase(path);
  }
  return raw;
}

void TileDesignerWindow::DrawFilmstripPanel(const TileArtQueue::Job &job) {
  const std::vector<Artifact> artifacts = ArtifactsFor(job);
  std::vector<Artifact> images;
  for (const Artifact &artifact : artifacts) {
    if (artifact.isImage) images.push_back(artifact);
  }
  if (images.empty()) {
    ImGui::TextDisabled(
        "No pipeline images to show. They are written beside the worklist "
        "(art.generation.workDirectory) and this section reads them back once a "
        "request has produced them.");
    return;
  }

  // One row of stages, left to right, which is the order the pipeline runs them.
  // Each slot states the stage's source size so the shrinking is visible -- the
  // numbers are the only place the sheet's resolution appears at all.
  const float slot = 132.0f;
  const int perRow = std::max(1, static_cast<int>(
                                    ImGui::GetContentRegionAvail().x / (slot + 12.0f)));
  int drawn = 0;
  for (const Artifact &artifact : images) {
    raylib::Texture *texture = LoadThumbnail(artifact.path, 320);
    ImGui::BeginGroup();
    ImGui::TextDisabled("%s", artifact.label.c_str());

    const ImVec2 topLeft = ImGui::GetCursorScreenPos();
    if (texture != nullptr && texture->id != 0) {
      DrawSpriteImGui(*texture, topLeft, ImVec2(slot, slot));
    } else {
      ImDrawList *drawList = ImGui::GetWindowDrawList();
      drawList->AddRectFilled(topLeft, ImVec2(topLeft.x + slot, topLeft.y + slot),
                              IM_COL32(40, 40, 44, 255));
      drawList->AddText(ImVec2(topLeft.x + 6.0f, topLeft.y + slot * 0.5f),
                        IM_COL32(200, 140, 140, 255), "unreadable");
    }
    ImGui::Dummy(ImVec2(slot, slot));
    if (texture != nullptr) {
      ImGui::TextDisabled("%dx%d", texture->width, texture->height);
    }
    ImGui::EndGroup();

    ++drawn;
    if (drawn % perRow != 0) {
      ImGui::SameLine();
    }
  }
  ImGui::NewLine();

  if (!job.worklistPath.empty()) {
    ImGui::TextDisabled("all of these are also on disk; \"copy\" in Pipeline "
                        "artifacts gives the paths");
  }
}

TileDesignerWindow::ThumbnailLookup TileDesignerWindow::InspectThumbnailForTest(
    const std::string &path) {
  ThumbnailLookup lookup;
  raylib::Texture *texture = LoadThumbnail(path, 320);
  lookup.textureId = texture != nullptr ? texture->id : 0u;
  std::error_code ec;
  lookup.stamp = std::filesystem::last_write_time(path, ec);
  lookup.hasStamp = !ec;
  return lookup;
}

TileDesignerWindow::PixelViewLookup TileDesignerWindow::InspectPixelViewForTest(
    const std::string &spritePath) {
  PixelViewLookup lookup;
  EnsurePixelView(spritePath);
  // Only answered when the view actually holds the path that was asked for, so a
  // stale view is reported as stale rather than as the new file's pixels.
  if (pixelViewPath != spritePath || !pixelView.texture) return lookup;
  lookup.hash = pixelView.imageHash;
  lookup.distinctColours = pixelView.distinctColours;
  return lookup;
}

std::string TileDesignerWindow::DefinitionPromptFor(const std::string &name,
                                                    SpriteKinds::Kind kind,
                                                    const std::string &requirement) const {
  std::string prompt = std::string("Create a new ") +
                       (kind == SpriteKinds::Kind::Tile ? "ground tile" : "object") +
                       " named \"" + name + "\".";
  if (!requirement.empty()) {
    prompt += " " + requirement;
  }
  prompt += kind == SpriteKinds::Kind::Tile
                ? " It is a ground surface, so it does not block movement."
                : " It is an object that stands on the ground in a building.";
  return prompt;
}

void TileDesignerWindow::StartAutoArt(const Blueprint &design,
                                      const std::vector<BlueprintNeed> &needs) {
  // The design itself is not used yet: the automatic path carries the designer's own
  // description of each asset, which is the brief that matters. The crop and style read
  // stay on the manual path until this one has been seen working.
  (void)design;
  if (game == nullptr || ai == nullptr || needs.empty()) {
    return;
  }
  autoNeeds = needs;
  autoIndex = 0;
  autoStarted = false;
  autoIdleFrames = 0;
  autoFailures.clear();
  autoRunning = true;
  // The run advances in this window's Draw, so it has to be visible: without this the run
  // would sit idle because nothing pumps it.
  game->tileDesignerWindowEntity.set<ActiveWindow>({game->tileDesignerWindow});
  statusIsError = false;
  statusLine = "Creating " + std::to_string(autoNeeds.size()) +
               " requested asset(s), one at a time.";
  LogEvent("AUTO ART STARTED: " + std::to_string(autoNeeds.size()) + " asset(s)");
}

void TileDesignerWindow::PumpAutoArt() {
  if (!autoRunning) return;

  if (autoIndex >= autoNeeds.size()) {
    autoRunning = false;
    if (autoFailures.empty()) {
      statusIsError = false;
      statusLine =
          "Created all " + std::to_string(autoNeeds.size()) + " requested asset(s).";
      LogEvent("AUTO ART DONE: all assets created");
    } else {
      std::string names;
      for (const std::string &failure : autoFailures) {
        names += (names.empty() ? "" : ", ") + failure;
      }
      statusIsError = true;
      statusLine = "Auto art finished; could not create: " + names;
      LogEvent("AUTO ART DONE WITH FAILURES: " + names);
    }
    return;
  }

  const BlueprintNeed &need = autoNeeds[autoIndex];

  // A turn in flight is this item's turn: the run only starts from an idle window.
  if (IsBusy()) {
    autoIdleFrames = 0;
    return;
  }

  // Done when the asset exists in the world. The sprite document and its compile follow
  // the definition through the normal acceptance path, so this one check covers the whole
  // item -- and it is a fact about the catalog rather than a flag some handler has to
  // remember to set.
  const bool exists =
      need.kind == "tile"
          ? game->tileRegistry.Get(need.name) != nullptr
          : game->objectFactory.GetTemplates().count(need.name) != 0;

  if (!autoStarted) {
    // Already there: nothing to build, and asking for it again would be a wasted model
    // call and a revision of something nobody complained about.
    if (exists) {
      ++autoIndex;
      return;
    }
    autoStarted = true;
    autoIdleFrames = 0;
    const SpriteKinds::Kind kind =
        need.kind == "tile" ? SpriteKinds::Kind::Tile : SpriteKinds::Kind::Object;
    mode = kind;
    requestSet = false;
    systemPrompt = game->BuildSpriteSystemPrompt(kind, false);
    LogEvent("AUTO ART ITEM: " + need.name);
    SendUserMessage(DefinitionPromptFor(need.name, kind, need.description));
    return;
  }

  if (exists) {
    ++autoIndex;
    autoStarted = false;
    return;
  }

  // Nothing in flight and the asset still absent: a retry may be queued for the next
  // frame, so give it a moment before calling the item failed.
  if (++autoIdleFrames > 120) {
    autoFailures.push_back(need.name);
    LogEvent("AUTO ART ITEM FAILED: " + need.name);
    ++autoIndex;
    autoStarted = false;
    autoIdleFrames = 0;
  }
}

void TileDesignerWindow::DrawGapOffer() {
  if (game == nullptr) return;
  SpriteGapOffer *offer = game->spriteGapOffer.get();
  if (offer == nullptr || !offer->Active()) return;

  ImGui::PushID("sprite-gap-offer");
  ImGui::Separator();

  const bool reading = offer->GetState() == SpriteGapOffer::State::Reading;
  const bool failed = offer->GetState() == SpriteGapOffer::State::Failed;

  ImGui::Text("%s", offer->IsGap() ? "A gap the design needs drawn"
                                   : "A sprite this design uses but cannot draw");
  ImGui::SameLine();
  if (ImGui::SmallButton("Dismiss##gap")) {
    // Cancelled, not destroyed: this runs inside the offer's own frame, and destroying it
    // would unload the texture whose draw command is already queued for this frame. The
    // object stays until Game replaces it or the run ends.
    offer->Cancel();
    ImGui::PopID();
    return;
  }

  if (offer->IsGap()) {
    ImGui::TextWrapped("\"%s\" -- the %s \"%s\", %d cell(s) marked in the crop.",
                       offer->Design().name.c_str(),
                       offer->Gap().kind == "paint" ? "ground tile" : "object",
                       offer->Gap().material.c_str(), offer->GapCells());
  } else {
    ImGui::TextWrapped(
        "\"%s\" -- the %s \"%s\", which this design uses. The definition exists and the "
        "art pack has no sprite for it, so nothing is marked: the crop is the building as "
        "it stands.",
        offer->Design().name.c_str(),
        offer->Gap().kind == "paint" ? "ground tile" : "object",
        offer->Gap().material.c_str());
  }

  if (const Texture2D *crop = offer->CropTexture()) {
    // Whole-number scaling only. The pane exists so the style brief can be judged against
    // what the model saw, and a smoothed or fractional resample would be a picture of
    // something the model never saw.
    float scale = 2.0f;
    const float available = ImGui::GetContentRegionAvail().x;
    while (scale > 1.0f && crop->width > 0 && crop->width * scale > available) {
      scale -= 1.0f;
    }
    ImGui::Image(static_cast<ImTextureID>(static_cast<std::intptr_t>(crop->id)),
                 ImVec2(static_cast<float>(crop->width) * scale,
                        static_cast<float>(crop->height) * scale));
  } else {
    ImGui::TextDisabled("(the crop exists but could not be shown)");
  }
  if (!offer->CropPath().empty()) {
    ImGui::TextDisabled("%s", offer->CropPath().c_str());
  }

  ImGui::TextWrapped("%s", offer->Status().c_str());
  if (!offer->Requirement().empty()) {
    ImGui::TextDisabled("What the designer asked for:");
    ImGui::TextWrapped("%s", offer->Requirement().c_str());
  }
  if (offer->ReadStyle().ok) {
    ImGui::TextWrapped("%s", offer->ReadStyle().summary.c_str());
  }

  // Disabled while reading, and when the crop itself failed: the numeric brief alone is
  // still a usable request, but a design that could not be drawn has nothing to offer.
  ImGui::BeginDisabled(reading || failed);
  const char *label =
      offer->IsGap() ? "Create this asset" : "Send document request";
  if (ImGui::Button(label)) {
    const std::string name = offer->Gap().material;
    const SpriteKinds::Kind kind = offer->Gap().kind == "paint"
                                       ? SpriteKinds::Kind::Tile
                                       : SpriteKinds::Kind::Object;
    const std::string surroundings =
        offer->ReadStyle().ok ? offer->ReadStyle().summary : std::string();
    const std::string requirement = offer->Requirement();
    const bool needsDefinition = offer->IsGap();
    offer->Cancel();

    if (needsDefinition) {
      // A sprite for a template nobody can spawn is a picture of nothing, so the
      // definition comes first. That is the definition turn, and accepting it is what
      // requests the document (the acceptance path calls RequestSpriteDocument when the
      // DSL source is active) -- so this one prompt is the whole chain, in the order the
      // pipeline requires.
      mode = kind;
      requestSet = false;
      if (game != nullptr) {
        systemPrompt = game->BuildSpriteSystemPrompt(kind, false);
      }
      std::string prompt =
          std::string("Create a new ") +
          (kind == SpriteKinds::Kind::Tile ? "ground tile" : "object") + " named \"" +
          name + "\".";
      if (!requirement.empty()) {
        prompt += " " + requirement;
      }
      prompt += kind == SpriteKinds::Kind::Tile
                    ? " It is a ground surface, so it does not block movement."
                    : " It is an object that stands on the ground in a building.";
      SendUserMessage(prompt);
    } else {
      RequestSpriteDocument(name, kind, /*replaceGenerated=*/true, surroundings,
                            requirement);
    }
    ImGui::EndDisabled();
    ImGui::PopID();
    return;
  }
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip(
        reading   ? "Waiting for the style read to finish."
        : failed  ? "The building could not be rendered, so there is nothing to send."
                  : "Ask the document turn for this sprite. The style brief above, if "
                    "one was read, goes into the request.");
  }

  ImGui::Separator();
  ImGui::PopID();
}

void TileDesignerWindow::Draw() {
  bool isOpen = true;
  ImGui::SetNextWindowSize(ImVec2(780, 560), ImGuiCond_FirstUseEver);
  // Renamed user-facing only: the class, the files and the settings keys keep their
  // names (see docs/sprite-designer-design.md, decision 6), so nothing that referenced
  // "Tile Designer" breaks -- only the label a person reads changes, because it now
  // designs two kinds of thing.
  ImGui::Begin("Sprite Designer", &isOpen, ImGuiWindowFlags_None);
  if (!isOpen) {
    game->tileDesignerWindowEntity.remove<ActiveWindow>();
    ImGui::End();
    return;
  }

  // At the very top: when an offer is open it is the reason this window is open, and it
  // is what the next document request will be about.
  DrawGapOffer();

  // The automatic run advances here rather than on a timer: this is the only place the
  // window's own state (busy, or idle with a queue) is meaningful.
  PumpAutoArt();

  // What is being designed, and who decided. The dropdown is the correction path for the
  // router: a wrong classification costs one decision call, and switching it here before
  // sending means the prompt is built for the right kind from the start.
  {
    const SpriteKinds::Descriptor &current = SpriteKinds::For(mode);
    ImGui::TextDisabled("Designing:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::BeginCombo("##sprite_kind", current.label.c_str())) {
      for (const SpriteKinds::Descriptor &candidate : SpriteKinds::All()) {
        const bool selected = candidate.kind == mode;
        if (ImGui::Selectable(candidate.label.c_str(), selected)) {
          if (candidate.kind != mode) {
            mode = candidate.kind;
            RefreshRewriteBrief();
            if (game != nullptr) {
              systemPrompt = game->BuildSpriteSystemPrompt(mode, requestSet);
            }
            // A mode change invalidates the prompt, not the transcript: the
            // conversation so far stays readable, and the next request is built from
            // the new kind's document.
            routeNote.clear();
            LogEvent(std::string("MODE: switched to ") + candidate.id +
                     " by hand; the next request uses the " + candidate.id +
                     " prompt");
          }
        }
        if (selected) ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip(
          "Which kind of sprite the next request is built for.\n"
          "The classifier guesses this from your text when you press Route;\n"
          "this dropdown overrides it, and the choice is what the prompt is built "
          "from.");
    }

    // Single or set. The router proposes it, but a person can ask for one without a
    // classifier, and can correct the router -- so it is a control, not a hidden decision.
    ImGui::SameLine();
    if (ImGui::Checkbox("Set", &requestSet)) {
      // The prompt is per shape, so a shape change rebuilds it, exactly as a kind change
      // does. The transcript is left alone: what was already designed is still true.
      if (game != nullptr) {
        systemPrompt = game->BuildSpriteSystemPrompt(mode, requestSet);
      }
      LogEvent(std::string("SHAPE: ") + (requestSet ? "set" : "single") +
               " by hand; the next request uses the " +
               (requestSet ? "set" : "single") + " prompt");
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip(
          "Ask for a set of sprites that belong together, instead of one.\n"
          "The designer answers with a \"sprites\" array; every entry is validated on its "
          "own, so one bad entry does not discard the rest.\nArt for the set is requested "
          "one batch at a time, at most art.generation.maxCellsPerRequest sprites per "
          "image request.");
    }
    if (requestSet && requestSetSize > 0) {
      ImGui::SameLine();
      ImGui::TextDisabled("(~%d)", requestSetSize);
    }

    // The router. Disabled with a reason rather than hidden: a missing classifier is a
    // configuration state, and a control that vanishes explains nothing.
    const bool canRoute = game != nullptr && game->systemOneClassifier != nullptr &&
                          !routeInFlight;
    ImGui::SameLine();
    ImGui::BeginDisabled(!canRoute);
    if (ImGui::Button("Route from text")) {
      RequestRoute(inputBuffer);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip(
          game != nullptr && game->systemOneClassifier != nullptr
              ? "Ask a decision model which kind, shape and size your request is, then "
                "switch the dropdown to match.\nIt reads the text currently in the box. "
                "Costs a fraction of a cent -- no image is generated."
              : "No System 1 classifier is configured (system1.model.apiKey or "
                "model.apiKey), so the request cannot be read.\nPick the kind with the "
                "dropdown instead.");
    }
    if (routeInFlight) {
      ImGui::SameLine();
      ImGui::TextDisabled("(asking...)");
    }
    if (!routeNote.empty()) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.75f, 0.78f, 0.85f, 1.0f));
      ImGui::TextWrapped("%s", routeNote.c_str());
      ImGui::PopStyleColor();
    }
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
  // Disabled while generation is off, because an auto-request that can only ever be
  // refused is worse than a checkbox that says why it cannot be used.
  ImGui::BeginDisabled(game == nullptr || !game->TileArtQueueState().Enabled());
  ImGui::Checkbox("Art automatically", &autoRequestArt);
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip(
        game != nullptr && game->TileArtQueueState().Enabled()
            ? "Ask for generated art as soon as a tile is accepted, so naming the "
              "tile is the whole instruction.\nRequests are batched, so a run of "
              "creations still costs one request.\nPersisted in debug_windows_state."
            : "Art generation is off (art.generation.enabled in settings.json), so "
              "there is nothing to request automatically.\nThe setting spends "
              "money, so it is off by default.");
  }
  ImGui::SameLine();
  ImGui::TextDisabled("| overlay: %s", OverlayDirectory().empty()
                                           ? "(none: tiles are session-only)"
                                           : OverlayDirectory().c_str());
  // The art state, in the header rather than only in the art pane. A feature that is
  // off and says nothing is indistinguishable from one that is broken, and the pane
  // it was reported in is below the fold -- which is exactly how a tile comes back
  // with no sprite and no explanation.
  ImGui::SameLine();
  if (game == nullptr) {
    ImGui::TextDisabled("| art: unknown");
  } else {
    const TileArtQueue &queue = game->TileArtQueueState();
    const std::string unavailable = queue.UnavailableReason();
    if (unavailable.empty()) {
      ImGui::TextDisabled("| art: on (%d/%d requests)", queue.RequestsUsed(),
                          queue.GetConfig().maxRequestsPerSession);
    } else {
      ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.4f, 1.0f), "| art: off");
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip(
          "%s",
          unavailable.empty()
              ? "Tile art generation is available. Accepted tiles get a sprite when "
                "\"Art automatically\" is on, or press Generate art."
              : ("Not generating art: " + unavailable +
                 "\nSee SETTINGS.md, section art.generation.")
                    .c_str());
    }
  }
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
    autoRequestArt = game->debugWindowState->GetTileDesignerAutoArt();
    // A first launch has no stored preference, so it follows the setting that
    // already exists: turning art generation on is one deliberate act, and asking
    // for it to then need a second click per tile is friction with no safety value
    // -- `art.generation.enabled` is itself off by default, which is where the
    // money decision belongs. `maxRequestsPerSession` caps the consequence, and
    // the checkbox below turns it back off.
    if (autoRequestArt == false && !game->debugWindowState->HasTileDesignerAutoArt()) {
      autoRequestArt = game->TileArtQueueState().Enabled();
    }
    layoutLoaded = true;
  }

  // Left: the tile, editable. Right: the conversation.
  const float available = ImGui::GetContentRegionAvail().x;
  const float paneWidth = std::clamp(available * 0.42f, 220.0f,
                                     std::max(220.0f, available - 260.0f));

  // The router's answer, applied on the main thread on the frame it arrives. Doing it in
  // the callback would mean switching the mode and rebuilding a prompt from a worker
  // thread while the frame is mid-draw.
  {
    std::optional<SpriteRouter::Verdict> pending;
    {
      std::lock_guard<std::mutex> lock(routeMutex);
      if (routeVerdict.has_value()) {
        pending = routeVerdict;
        routeVerdict.reset();
      }
    }
    if (pending.has_value()) {
      ApplyRouteVerdict(*pending);
    }
  }

  ImGui::BeginChild("tile_designer_preview", ImVec2(paneWidth, -70), true);
  if (mode == SpriteKinds::Kind::Object) {
    DrawObjectPreview();
  } else {
    DrawPreviewPane();
  }
  ImGui::EndChild();

  ImGui::SameLine();

  ImGui::BeginChild("tile_designer_transcript", ImVec2(0, -70), true);
  std::vector<Exchange> snapshot;
  {
    std::lock_guard<std::mutex> lock(transcriptMutex);
    snapshot = exchanges;
  }

  if (snapshot.empty()) {
    if (mode == SpriteKinds::Kind::Object) {
      ImGui::TextWrapped(
          "Ask for an object -- \"a wooden crate\", \"a brass candelabra\", \"a "
          "young apple tree\". The designer replies with one object definition and its "
          "own notes; an accepted object is written to the generated overlay and is "
          "available to blueprints and spawns on the next load.\n\nPress \"Route from "
          "text\" to have a classifier read your request before the designer does.");
    } else {
      ImGui::TextWrapped(
          "Ask for a tile -- \"a stone hearth\", \"mossy flagstones\", \"a puddle\". "
          "The designer replies with one tile definition and its own notes; an "
          "accepted tile is written to the generated overlay and is immediately "
          "paintable from a blueprint.\n\nPress \"Route from text\" to have a "
          "classifier read your request before the designer does.");
    }
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
      // A set is shown entry by entry: the interesting fact about a six-entry reply where one
      // failed is *which* one, and a single "Added" line would hide it.
      if (ex.wasSet && !ex.entryVerdicts.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 1.0f, 0.4f, 1.0f));
        ImGui::TextWrapped("Set '%s' -- %zu of %zu accepted",
                           ex.setName.empty() ? "(unnamed)" : ex.setName.c_str(),
                           ex.acceptedNames.size(), ex.entryVerdicts.size());
        ImGui::PopStyleColor();
        for (size_t entry = 0; entry < ex.entryVerdicts.size(); ++entry) {
          const bool accepted = ex.entryVerdicts[entry].first;
          ImGui::PushStyleColor(ImGuiCol_Text,
                                accepted ? ImVec4(0.55f, 0.85f, 0.55f, 1.0f)
                                         : ImVec4(1.0f, 0.5f, 0.4f, 1.0f));
          ImGui::TextWrapped("  [%zu] %s %s", entry,
                             accepted ? "accepted" : "refused",
                             ex.entryVerdicts[entry].second.c_str());
          ImGui::PopStyleColor();
        }
        if (!ex.artNote.empty()) {
          ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.75f, 0.78f, 0.85f, 1.0f));
          ImGui::TextWrapped("Art: %s", ex.artNote.c_str());
          ImGui::PopStyleColor();
        }
        for (const std::string &warning : ex.warnings) {
          ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.5f, 1.0f));
          ImGui::TextWrapped("Note: %s", warning.c_str());
          ImGui::PopStyleColor();
        }
      } else if (ex.ok) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 1.0f, 0.4f, 1.0f));
        ImGui::TextWrapped("Added '%s'%s", ex.tileName.c_str(),
                           ex.savedPath.empty()
                               ? " (session only -- not written to disk)"
                               : (" to " + ex.savedPath).c_str());
        ImGui::PopStyleColor();
        // What happened about the art, immediately under the verdict -- the place
        // someone looks after accepting a tile and not seeing a sprite. Coloured
        // differently from the verdict because "no art was requested" is not a
        // failure, it is the state of a setting.
        if (!ex.artNote.empty()) {
          ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.75f, 0.78f, 0.85f, 1.0f));
          ImGui::TextWrapped("Art: %s", ex.artNote.c_str());
          ImGui::PopStyleColor();
        }
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

  // Collect a rewrite that landed since the last frame. Before the buttons, so the
  // result is in the box on the same frame the user sees the state change.
  promptBox.Drain();

  ImGui::BeginDisabled(busy);
  const bool send = ImGui::Button("Send");
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::TextDisabled("Enter to send, Shift+Enter for a newline");
  ImGui::SameLine();

  // The Improve affordance sits on the Send row, immediately beside the box it
  // affects -- which is the whole point: the description the user is about to send is
  // the thing being improved, and a control anywhere else would not read as belonging
  // to it.
  promptBox.DrawButtons();

  // What the rewriter is doing or last said, on its own line so it can wrap. Kept out
  // of the Send row because that row is already at its width limit.
  const std::string rewriterStatus = promptBox.StatusLine();
  if (!rewriterStatus.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.75f, 0.78f, 0.85f, 1.0f));
    ImGui::TextWrapped("Improve: %s", rewriterStatus.c_str());
    ImGui::PopStyleColor();
  }
  // A rewrite is machine-written text sitting in the user's box. Saying so, with the
  // way back, is the difference between a helpful tool and one that silently puts
  // words in someone's mouth.
  if (promptBox.ShowingRewrite()) {
    ImGui::SameLine();
    ImGui::TextDisabled("(rewritten -- Undo restores your wording)");
  }

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
      // Forget the pre-rewrite text: the next message must not offer to undo back
      // into the one just sent.
      promptBox.Reset();
      SendUserMessage(prompt);
    }
  }

  if (layoutLoaded && game != nullptr && game->debugWindowState) {
    game->debugWindowState->SetDesignerShowJson(showJson);
    game->debugWindowState->SetTileDesignerAutoArt(autoRequestArt);
  }

  ImGui::End();
}
