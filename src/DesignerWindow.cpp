#include "DesignerWindow.h"

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

#include "BlueprintPreview.hpp"
#include "BlueprintRegistry.h"
#include "Components.h"
#include "DebugLog.h"
#include "LogPaths.h"
#include "SpriteGapPanel.hpp"

#include <nlohmann/json.hpp>
#include "Game.h"
#include "ArtRequestsPanel.h"
#include "ContentKinds.h"
#include "PixelDSL.hpp"
#include "Settings.h"
#include "RewriteBriefs.hpp"
#include "TileArt.hpp"
#include "SpriteDraw.hpp"
#include "SpritePaths.hpp"
#include "RigRender.hpp"
#include "TileArtQueue.hpp"
#include "TimeUtils.hpp"
#include "RaylibException.hpp"
#include "imgui.h"
#include "raylib.h"

namespace {

// Neither pane may be dragged to nothing: a window with an ungrabbable sliver is a state
// the user cannot get out of.
constexpr float kMinPaneWidth = 180.0f;
constexpr float kMinTranscriptWidth = 220.0f;

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

DesignerWindow::DesignerWindow(Game *game, AI *designerAI,
                                       std::string modelLabel, bool selfTest,
                                       flecs::entity owner)
    : game(game), modelLabel(std::move(modelLabel)),
      selfTest(selfTest) {
  (void)owner;  // Windows are not ECS components yet; see Window.h.

  // One context per window instance, named so it cannot collide with an NPC's
  // ("npc-<id>"), the blueprint designer's or the vision window's.
  const std::string contextId =
      "tile-designer-" + std::to_string(reinterpret_cast<uintptr_t>(this));
  // Its own provider context. Not because the histories are sent explicitly -- they
  // are -- but because a provider that keys anything by context must not have a
  // document request and a definition request share one.
  const std::string artContextId =
      "tile-designer-art-" + std::to_string(reinterpret_cast<uintptr_t>(this));

  session = std::make_unique<DesignerSession>(
      game, designerAI, modelLabel, "designer", "-designer.txt", "Designer",
      // Neutral rather than per-kind: this window hides a blueprint, a tile or an object
      // JSON block depending on the request, and a marker naming one of them would be
      // wrong two times in three. It also reaches the model in the replayed history.
      "JSON hidden");

  // Two conversations, one transcript. The definition turn starts from the mode's own
  // prompt -- by the current mode rather than the tile-specific entry point, so the two
  // cannot disagree about what the model was told, and because the mode starts as Tile
  // this is the same prompt the old call produced.
  session->AddConversation(
      contextId,
      game != nullptr ? game->BuildDesignerPrompt(mode, requestSet)
                      : std::string());
  session->AddConversation(artContextId, std::string());

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
  promptBox.SetLogSink([this](const std::string &line) { session->Log(line); });
}

size_t DesignerWindow::RunSelfTestConversation(
    bool requestArt, DesignerDomains::Domain domain, bool setTest) {
  if (!selfTest) return 0;

  // The mode is set here because the canned reply below is a reply for that kind, and the
  // whole point of the object variant is to run the object half of the inspection path --
  // a tile-shaped reply inspected as an object would be refused for having no components,
  // which tests the refusal and not the accept.
  mode = domain;
  designMode = domain;
  // The shape, before the request is "sent": inspection reads it per exchange, and a set
  // reply judged by the single path is refused for having no "name" at the top level.
  requestSet = setTest;
  RefreshRewriteBrief();
  if (game != nullptr) {
    session->SetSystemPrompt(kDefinitionConversation,
                             game->BuildDesignerPrompt(mode, requestSet));
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

  // A building reply: a floor and one placed object. The `place` op is there so the
  // *preview*'s object branch is exercised -- it draws the object's sprite over the floor,
  // which is the reported bug this fixture now guards. `workbench` is a real template key
  // (`--object-check workbench` resolves it, and its generated sprite exists), so a wrong
  // key would fail the expansion and turn this into a refusal test.
  const std::string kCannedBlueprintReply = R"(<think>One floor, one region.</think>
A small stone-floored shed.

```json
{
  "id": "self_test_shed",
  "name": "Self Test Shed",
  "purpose": "house",
  "levels": 1,
  "regions": {
    "footprint": {
      "rect_fill": {
        "at": [0, 0],
        "to": [4, 3]
      }
    }
  },
  "parts": [
    {
      "id": "floor",
      "region": "footprint",
      "op": {
        "paint": "stone floor"
      }
    },
    {
      "id": "bench",
      "shape": {
        "point": {
          "at": [1, 1]
        }
      },
      "op": {
        "place": "workbench"
      }
    }
  ],
  "designerNotes": "Written by the self test to exercise the building half of the inspection path end to end: one region, one paint op, no placed objects."
}
```
)";

  const std::string kCannedReply =
      setTest ? kCannedSetReply
              : (domain == DesignerDomains::Domain::Blueprint
                     ? kCannedBlueprintReply
                     : (domain == DesignerDomains::Domain::Object
                            ? kCannedObjectReply
                            : kCannedTileReply));

  LogSessionHeader();

  DesignerSession::Exchange ex;
  ex.conversation = kDefinitionConversation;
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
  const size_t index = session->Append(std::move(ex));

  session->LogEvent("USER REQUEST (attempt 1 of this request)");
  session->Log("self test: log one exchange\n");

  // One synthetic request so the "what went on the wire" line is written too, with
  // a real elapsed time measured across the inspection below.
  {
    std::vector<ChatMessage> request;
    if (!session->SystemPrompt(kDefinitionConversation).empty()) {
      request.push_back({"system", session->SystemPrompt(kDefinitionConversation)});
    }
    request.push_back({"user", "self test: log one exchange"});
    session->NoteSend(request);
  }
  InspectFinishedExchange(index);

  // Re-select the entry the inspection just captured. Nothing should change on screen, but
  // it runs the one path the arrows take -- `ShowViewed`, which reloads a blueprint slot, an
  // object copy or a tile editor from a stored result -- and that path is otherwise
  // reachable only by a click. A test hook that only inspected would leave the part of the
  // feature that was reported broken untested.
  if (!viewed.empty()) {
    ShowViewed(viewed.size() - 1);
  }

  size_t inspected = 0;
  for (const DesignerSession::Exchange &exchange : session->Snapshot()) {
    if (exchange.inspected) ++inspected;
  }
  return inspected;
}

void DesignerWindow::LogSessionHeader() {
  if (headerLogged) return;
  headerLogged = true;

  // The domain this session designs, named in the header because one log file now covers
  // buildings, tiles and objects -- and they are told apart by their prompt file and their
  // save location, both of which follow from this line.
  const DesignerDomains::Descriptor &domain = ModeDescriptor();
  // The prompt as it is *now*, not as it was when the window was built: the vocabulary
  // is generated from the live catalog, so a session that created a tile earlier in the
  // run has a different prompt from the one it started with.
  const std::string &systemPrompt = session->SystemPrompt(kDefinitionConversation);
  session->Log("\n================ DESIGNER SESSION (" + domain.label +
      ") ================\n");
  session->Log("started:   " + TimeUtils::GetCurrentTimeString() + "\n");
  session->Log("domain:    " + domain.id + "\n");
  session->Log("backend:   " + (modelLabel.empty() ? std::string("(none)") : modelLabel) +
      "\n");
  session->Log("context:   " + session->ContextId(kDefinitionConversation) + "\n");

  const GameSettings &settings = Settings();
  const std::string overlay = ModeOverlayDirectory();
  session->Log("overlay:   " + (overlay.empty() ? std::string("(none)") : overlay) + "\n");
  session->Log("art:       pack=" +
      (settings.art.pack.empty() ? std::string("(none)") : settings.art.pack) +
      "  generation=" + (settings.art.generation.enabled ? "on" : "off") + "\n");
  session->Log("attempts:  " + std::to_string(game != nullptr ? game->maxDesignAttempts() : 0) +
      " per request\n");

  session->LogPromptFile(game != nullptr ? game->DesignerPromptFile(mode)
                                         : domain.promptFile);

  // The system prompt as it will be sent: the file above with %CONTRACT%, %GLYPHS% and
  // %VOCAB% substituted from the live world at the moment the window was constructed --
  // the tile catalog for a tile, the factory's components for an object. This is the
  // single most valuable line in the log, because it is the only record of what the model
  // was actually told.
  session->Log("--- system prompt as sent (" + std::to_string(systemPrompt.size()) +
      " bytes) ---\n");
  if (systemPrompt.empty()) {
    session->Log("(EMPTY -- the model is running with no instructions at all)\n");
  } else {
    session->Log(systemPrompt);
    if (systemPrompt.back() != '\n') session->Log("\n");
  }
  session->Log("--- end of system prompt ---\n");
  session->Log("=======================================================\n");
}

std::string DesignerWindow::OverlayDirectory() const {
  return Settings().content.generatedTiles;
}

void DesignerWindow::LoadEditorFrom(const TileDef &definition) {
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

std::string DesignerWindow::SaveEditedTile() {
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

void DesignerWindow::SendUserMessage(const std::string &prompt) {
  if (!session->HasBackend() || prompt.empty() || session->IsBusy()) {
    return;
  }

  // Actor sprites are the one mode whose request is not a definition: the text is a
  // description of a being to draw, and the reply is a sprite document. Routed before the
  // attempt budget and the latched kind below, because none of that applies to the art
  // conversation.
  if (mode == DesignerDomains::Domain::ActorSprite) {
    SendActorSpriteRequest(prompt);
    return;
  }

  // A new request restarts the attempt budget: the cap is per request, not per
  // session, or the third tile of a session could never be retried.
  session->ResetAttempts(kDefinitionConversation);
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
  session->LogEvent("USER REQUEST (attempt " +
           std::to_string(session->Attempts(kDefinitionConversation) + 1) +
           " of this request)");
  session->Log(prompt);
  session->Log("\n");

  // The exchange, the streaming handoff and the empty-token sentinel are
  // DesignerSession's; the kind and shape latched above travel with it so inspection
  // judges the reply by the grammar it was asked for.
  session->Send(prompt, kDefinitionConversation, /*fromUser=*/true, designSet);
}

// ---------------------------------------------------------------------------
// The pixel document turn
// ---------------------------------------------------------------------------

bool DesignerWindow::DslArtSourceActive() const {
  return Settings().art.generation.source == "dsl";
}

std::string DesignerWindow::ArtDocumentPath(const std::string &name,
                                                SpriteKinds::Kind kind) const {
  return GeneratedSpriteDocumentPath(SpriteKinds::ConventionIdFor(kind, name));
}

void DesignerWindow::AskForSpriteArt(const std::string &name,
                                     SpriteKinds::Kind kind) {
  // `true` is the point of the button. With `false` an existing document is merely
  // *compiled*, which is instant, free and produces exactly the image that is already on
  // screen -- so "regenerate" looked like a request that completed without doing anything.
  // Only a new document can produce new art; the pack's copy is never overwritten, and the
  // compile lands in the generated layer until the Content Editor's marker chooses it.
  RequestSpriteDocument(name, kind, /*replaceGenerated=*/true);
}

void DesignerWindow::RequestSpriteDocument(const std::string &name,
                                               SpriteKinds::Kind kind,
                                               bool replaceGenerated,
                                               const std::string &surroundings,
                                               const std::string &requirement) {
  if (name.empty()) return;
  if (!session->HasBackend()) {
    statusIsError = true;
    statusLine = "No designer backend is configured, so a document cannot be asked for.";
    return;
  }
  if (session->IsBusy()) {
    statusIsError = false;
    statusLine = "A request is already in flight; the document follows it.";
    return;
  }
  if (game == nullptr) return;

  // The dropdown follows the request: a gap offered from the blueprint pane is a tile or
  // an object, and leaving the header saying the other one would make the transcript read
  // as if the wrong prompt had been used.
  // An actor is not a definition domain: its definition lives in data/actors, so a
  // sprite request for one leaves the header where it is rather than claiming the next
  // request would produce an object or a tile.
  if (kind != SpriteKinds::Kind::Actor) {
    mode = kind == SpriteKinds::Kind::Object ? DesignerDomains::Domain::Object
                                             : DesignerDomains::Domain::Tile;
  }

  const std::string conventionId = SpriteKinds::ConventionIdFor(kind, name);

  // An existing document normally wins and is not overwritten: the pack's copy is
  // hand-curated, and a generated copy is what the previous turn left, which a person
  // may have edited since. `replaceGenerated` is the explicit "Ask again" action --
  // it may replace the generated copy but still never the pack's, because that one is
  // not ours to discard.
  const SpriteDSLDocument existing = FindSpriteDSLDocument(conventionId);
  if (existing.found && !replaceGenerated) {
    // An existing document is compiled as-is. This is the free path, and the pack's copy
    // is still never overwritten.
    std::string reason;
    const bool queued = game->RecompileSpriteArt(kind, name, &reason);
    statusIsError = !queued;
    statusLine = queued ? "Compiling the existing document " + existing.path
                        : reason;
    session->LogEvent("DSL DOCUMENT EXISTS (not overwriting): " + existing.path);
    return;
  }

  // `replaceGenerated` asks for a *new* document, and it is now allowed even when the
  // pack has one: what gets replaced is the generated copy at `ArtDocumentPath` below,
  // never the pack's. Before this, asking again for an asset with hand-authored art was
  // refused outright, so regeneration was impossible for exactly the assets a person can
  // see. The generated document only wins once the Content Editor's marker says so.
  if (existing.found && existing.inArtPack) {
    session->LogEvent("PACK DOCUMENT KEPT: " + existing.path +
                      " (a generated document will be written instead)");
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
  } else if (kind == SpriteKinds::Kind::Actor) {
    // Without this an actor sprite was requested with no description at all: the model
    // saw a bare name, which is why the result looked like a generic prop.
    if (const ActorSpriteDef *actor = game->actorSpriteRegistry.Get(name)) {
      artBrief.description = actor->description;
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
  // is the normal case for a document asked for by name -- the prompt builder omits the
  // paragraph entirely rather than saying "the surroundings are:" and stopping.
  artBrief.surroundings = surroundings;

  // A new document turn starts a new art conversation: the previous document's
  // instructions are about a different sprite, and its retry reasons are about a
  // different compile. Its own budget too, because `dslAttempts` is not
  // `designer.maxDesignAttempts` and one counter would let one budget silence the other.
  session->ResetAttempts(kArtConversation);
  session->SetMaxAttempts(kArtConversation, Settings().art.generation.dslAttempts);
  artVerdict.clear();
  session->ClearHistory(kArtConversation);
  session->SetSystemPrompt(
      kArtConversation,
      TileDesigner::BuildSpriteDocumentSystemPrompt(
          Settings().content.spriteDSLPromptFile,
          PixelDSL::Config{}.rampsPath));

  statusIsError = false;
  const std::string kindWord = kind == SpriteKinds::Kind::Tile
                                   ? "tile"
                                   : kind == SpriteKinds::Kind::Actor ? "actor"
                                                                      : "object";
  statusLine = "Asking for a sprite document for the " + kindWord + " '" + name +
               "'. It will be compiled before anything is written.";
  session->LogEvent("SPRITE DOCUMENT REQUESTED: " + name + " (" + kindWord + ", " +
                    conventionId + ")");
  SendArtTurn(TileDesigner::BuildSpriteDocumentRequest(artBrief, ""));
}

void DesignerWindow::SendArtTurn(const std::string &prompt) {
  if (!session->HasBackend() || prompt.empty() || session->IsBusy()) return;

  // Logged in the same designer log as the definition turn: a document and the
  // definition it belongs to are one story, and a second log file would split it.
  session->LogEvent("SPRITE DOCUMENT REQUEST (attempt " +
           std::to_string(session->Attempts(kArtConversation) + 1) + " of " +
           std::to_string(session->MaxAttempts(kArtConversation)) + ")");
  session->Log(prompt);
  session->Log("\n");

  session->Send(prompt, kArtConversation, /*fromUser=*/true);
}

void DesignerWindow::InspectFinishedArtDocument(
    size_t index, const DesignerSession::Exchange &finished) {
  // Published once, at the end of each path, through the session's lock: the Draw
  // thread reads these fields.
  auto publish = [this, index](bool ok, const std::string &failure,
                               const std::string &savedPath,
                               const std::string &note) {
    session->Update(index, [&](DesignerSession::Exchange &exchange) {
      exchange.inspected = true;
      exchange.ok = ok;
      exchange.failureReason = failure;
      exchange.savedPath = savedPath;
      exchange.artNote = note;
    });
  };

  if (finished.failed) {
    session->PushTurn(kArtConversation, finished.prompt, "");
    publish(false, finished.error, "", "no document: " + finished.error);
    session->LogEvent("SPRITE DOCUMENT TRANSPORT FAILURE: " + finished.error);
    return;
  }

  // The raw reply, not HistoryAnswer: the document turn needs the model's previous
  // document verbatim to revise it, and the pixel DSL is what it is revising.
  session->PushTurn(kArtConversation, finished.prompt, finished.answer);

  const std::string document = TileDesigner::ExtractSpriteDocument(finished.answer);
  PixelDSL::Config config;
  config.palettePath.clear();  // informational only; not worth a file read here
  const PixelDSL::Result result = PixelDSL::Compile(document, config);

  if (!result.ok) {
    artVerdict = PixelDSL::Describe(result);
    const int budget = std::max(1, session->MaxAttempts(kArtConversation));
    session->LogEvent("SPRITE DOCUMENT REFUSED (attempt " +
             std::to_string(session->Attempts(kArtConversation) + 1) + " of " +
             std::to_string(budget) + ")");

    if (session->Attempts(kArtConversation) + 1 >= budget) {
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

    session->CountAttempt(kArtConversation);
    // The failed attempt is published first, then the retry is sent -- so the
    // transcript reads as the conversation that happened rather than as one slow
    // request. The verdict goes back verbatim: it already names the line and says
    // what to do.
    publish(false, "refused; retrying", "", artVerdict);
    statusIsError = false;
    statusLine = "Document refused; asking again (attempt " +
                 std::to_string(session->Attempts(kArtConversation) + 1) + " of " +
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
  session->LogEvent("SPRITE DOCUMENT ACCEPTED: " + destination);

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
    session->LogEvent("SPRITE COMPILE QUEUED for " + artBrief.name);
  } else {
    statusIsError = true;
    statusLine = "Document accepted and saved, but the compile was refused: " + reason;
    session->LogEvent("SPRITE COMPILE REFUSED: " + reason);
  }
}

// What the model must send back on a retry: the artifact, not the domain. "Send the
// corrected building" would be a sentence about a person; the blueprint is the thing being
// produced. One rule for all three domains, so a retry sentence cannot name the wrong one.
static const char *ArtifactNoun(DesignerDomains::Domain domain) {
  switch (domain) {
    case DesignerDomains::Domain::Blueprint:
      return "blueprint";
    case DesignerDomains::Domain::Object:
      return "object";
    case DesignerDomains::Domain::Tile:
      return "tile";
    case DesignerDomains::Domain::Map:
      return "map";
    case DesignerDomains::Domain::Rig:
      return "rig";
  }
  return "definition";
}

void DesignerWindow::InspectBlueprintReply(
    size_t index, const DesignerSession::Exchange &finished) {
  std::string reason;
  bool ok = false;
  std::string designId;
  std::string notes;
  // The assets this design asked for, in the designer's own words. Kept beside the
  // design so the preview can offer each one, and dropped on save with the notes.
  std::vector<BlueprintNeed> needs;
  std::string savedPath;
  bool overwrote = false;
  // Filled when the design got far enough to be parsed and expanded, whether or
  // not it then passed. Held out here so the pane can show a refused design too.
  bool haveDesign = false;
  Blueprint parsedDesign;
  BlueprintExpansion::Expansion parsedExpansion;

  const std::string block = DesignerReply::ExtractJsonBlock(finished.ParsedAnswer());
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
      // Declared before the chain so a malformed `needs` can be refused in the same
      // channel as a malformed design -- the model gets the reason and resends, which is
      // the only way it learns.
      BlueprintNeeds::ParseResult wanted;
      if (!parsed.is_object()) {
        reason = "the json block must be a JSON object describing one blueprint.";
      } else if (!(wanted = BlueprintNeeds::Parse(parsed)).ok) {
        reason = wanted.error;
      } else if (!parsed.contains("designerNotes") ||
                 !parsed["designerNotes"].is_string() ||
                 parsed["designerNotes"].get<std::string>().empty()) {
        // Required by the contract, and the reason it is required is the whole point of
        // this window: the model's own judgement of its design.
        reason =
            "the blueprint has no non-empty \"designerNotes\" string. Every design "
            "must carry your own thoughts on it.";
      } else {
        // The assets the design asked for. `needs` is not schema: stripped here so it
        // never reaches the saved blueprint, exactly like the notes.
        parsed.erase("needs");
        needs = std::move(wanted.needs);
        notes = parsed["designerNotes"].get<std::string>();
        // Removed before saving: it is not part of the schema, so writing it back would
        // put a designer-only field into content the rest of the engine parses.
        parsed.erase("designerNotes");

        const std::string id = parsed.value("id", std::string());
        if (id.empty()) {
          reason = "the blueprint has no \"id\".";
        } else {
          BlueprintRegistry *registry = nullptr;
          if (auto *res = game != nullptr
                              ? game->ecs.get<BlueprintRegistryResource>()
                              : nullptr) {
            registry = res->registry;
          }

          std::string parseError;
          std::vector<std::string> warnings;
          BlueprintRegistry::ParseBlueprint(parsed, id, "(designer)", parsedDesign,
                                            parseError, warnings);

          // Expanded even when parsing failed only enough to set an error: a partial
          // design is still worth drawing. The pane is a debugging aid, so it prefers
          // showing something over showing nothing.
          parsedExpansion = BlueprintExpansion::Expand(parsedDesign);
          haveDesign = !parsedDesign.parts.empty();

          if (!parseError.empty()) {
            reason = parseError;
          } else if (!parsedExpansion.errors.empty()) {
            // Expansion is checked before saving, not after: a design that parses but
            // produces no cells would otherwise be written to disk and appear in the
            // list as an empty building.
            reason = parsedExpansion.errors.front();
            if (parsedExpansion.errors.size() > 1) {
              reason += " (and " +
                        std::to_string(parsedExpansion.errors.size() - 1) +
                        " more problem(s))";
            }
          } else if (parsedExpansion.cells.empty()) {
            reason = "the design expands to no cells at all.";
          } else if (const BlueprintExpansion::VoidReport voids =
                         BlueprintExpansion::FindVoids(parsedExpansion);
                     !voids.ok()) {
            // Same sentence the loader logs, because it is the same rule: the model is
            // told which cell it left bare and how to fix it, which a bare "invalid" never
            // does.
            reason = voids.Summary();
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

  // The pane shows the latest *attempt*, so this is updated even on refusal -- and
  // deliberately not cleared when a reply carries no design at all, so a failed attempt
  // does not blank the last thing you were looking at.
  if (haveDesign) {
    PreviewedDesign preview;
    preview.blueprint = std::move(parsedDesign);
    preview.expansion = std::move(parsedExpansion);
    preview.notes = notes;
    preview.needs = std::move(needs);
    preview.accepted = ok;
    preview.refusal = ok ? std::string() : reason;
    lastDesign = std::move(preview);

    // A refused attempt is shown only while it is the newest event: the pane has always
    // drawn refused designs on purpose, but walking back through failures is not what the
    // history is for. The accepted branch below captures into the history itself.
    if (!ok) {
      refusedAttempt = ViewedResult{};
      refusedAttempt->domain = DesignerDomains::Domain::Blueprint;
      refusedAttempt->label = lastDesign->blueprint.name.empty()
                                  ? lastDesign->blueprint.id
                                  : lastDesign->blueprint.name;
      refusedAttempt->accepted = false;
      refusedAttempt->design = lastDesign;
      paneDomain = DesignerDomains::Domain::Blueprint;
    }
  }

  // The automatic run needs a saved design, the "Art automatically" switch on, and
  // something to build. Started from here rather than from the Send button so that
  // nothing is needed beyond the original prompt -- which is the whole point.
  //
  // One switch, not two: the checkbox that used to guard this sat in the preview pane and
  // asked the same question as the header's "Art automatically", which meant a user had to
  // find both. Generation being enabled is checked too, because the switch is disabled in
  // that state and a stale "on" from the state file must not turn into model calls.
  if (ok && autoRequestArt && game != nullptr &&
      game->TileArtQueueState().Enabled() && lastDesign.has_value() &&
      !lastDesign->needs.empty()) {
    StartAutoArt(lastDesign->blueprint, lastDesign->needs);
  }

  session->Update(index, [&](DesignerSession::Exchange &exchange) {
    exchange.inspected = true;
    exchange.ok = ok;
    exchange.name = designId;
    exchange.savedPath = savedPath;
    exchange.overwrote = overwrote;
    exchange.notes = notes;
    exchange.failureReason = reason;
  });

  // The conversation both sides saw, so a follow-up can refer to this design. The
  // assistant turn is the raw reply, and the capped replay is what keeps a large
  // blueprint from accumulating.
  session->PushTurn(kDefinitionConversation, finished.prompt,
                    finished.HistoryAnswer(session->HiddenMarker()));

  if (ok) {
    session->LogEvent("SAVED: " + designId + " -> " + savedPath +
                      (overwrote ? " (replaced an existing design)" : " (new design)"));
    if (!notes.empty()) {
      session->Log("designerNotes: " + notes + "\n");
    }
    if (game != nullptr && game->debugLog) {
      game->debugLog->LogInfo("Designer: saved blueprint '" + designId + "' to " +
                              savedPath + (overwrote ? " (replaced)" : ""));
    }

    // Read aloud only on the attempt that landed. Speaking every stream would mean
    // reading three blueprints and two refusals, interleaved, whenever the model needed a
    // retry.
    //
    // The blueprint JSON is never spoken under any setting except "everything", which
    // exists to debug this pipeline: it is the majority of the reply's bytes and it is not
    // language, so it reads as "id two bedroom house, name two bedroom house, rect_fill".
    const std::string &content = Settings().designer.tts.content;
    std::string spoken = "Saved as " + designId + ".";
    if (content == "everything") {
      spoken += " " + finished.ParsedAnswer();
    } else {
      if (content == "prose_and_notes") {
        const std::string prose =
            DesignerReply::ProseBeforeJson(finished.ParsedAnswer());
        if (!prose.empty()) {
          spoken += " " + prose;
        }
      }
      // The notes are the reason the window exists, so they are the default payload: the
      // model's own critique of what it just made.
      if (!notes.empty()) {
        spoken += " " + notes;
      }
    }
    session->SpeakFor(index, spoken);

    if (lastDesign.has_value()) {
      ViewedResult result;
      result.domain = DesignerDomains::Domain::Blueprint;
      result.label = lastDesign->blueprint.name.empty() ? lastDesign->blueprint.id
                                                        : lastDesign->blueprint.name;
      result.design = lastDesign;
      CaptureViewed(std::move(result));
    }

    session->ResetAttempts(kDefinitionConversation);
    return;
  }

  // Refused: say why, in the same conversation, and try again. Bounded so a model that
  // cannot satisfy the contract cannot spend the whole budget.
  //
  // The one refusal that is spoken is the last one, because it is the last thing that will
  // be said about this request. Intermediate refusals stay silent -- they are superseded
  // seconds later -- but this sentence is short, actionable, and lands while you are still
  // looking at the design that failed.
  if (!session->RetryRefused(kDefinitionConversation, reason, ArtifactNoun(designMode))) {
    session->SpeakFor(index,
                      "Giving up after " +
                          std::to_string(session->Attempts(kDefinitionConversation)) +
                          " attempts. " + reason);
  }
}

// How many results the pane remembers. Bounded because each blueprint entry holds its
// parsed design *and* its expansion: a long session would otherwise keep every design it
// ever produced, which is exactly the kind of unbounded growth a chat window should not
// have.
static constexpr size_t kMaxViewedResults = 50;

void DesignerWindow::CaptureViewed(ViewedResult result) {
  if (viewed.size() >= kMaxViewedResults) {
    viewed.erase(viewed.begin());
  }
  viewed.push_back(std::move(result));
  viewedIndex = viewed.size() - 1;
  paneDomain = viewed.back().domain;
  // A refusal is the newest event until the next result replaces it; once there is a newer
  // accepted result the refusal is no longer what the pane is about.
  refusedAttempt.reset();
}

void DesignerWindow::ShowViewed(size_t index) {
  if (index >= viewed.size()) return;
  viewedIndex = index;
  refusedAttempt.reset();
  const ViewedResult &entry = viewed[index];
  paneDomain = entry.domain;

  switch (entry.domain) {
    case DesignerDomains::Domain::Blueprint:
      lastDesign = entry.design;
      break;
    case DesignerDomains::Domain::Object:
      editedObject = entry.object;
      editedObjectKey = entry.objectKey;
      editedObjectOverlayPath = entry.objectOverlayPath;
      break;
    case DesignerDomains::Domain::Rig:
      rigDraft = entry.rig;
      rigError.clear();
      rigArtQueue.clear();
      rigArtStamps.clear();
      rigPreviewWorn.clear();
      if (rigDraft.has_value()) {
        std::snprintf(rigNameBuffer, sizeof(rigNameBuffer), "%s",
                      rigDraft->name.c_str());
        for (const ItemDef *item : game->itemRegistry.GetAll()) {
          if (!item->wornSprite.empty()) rigPreviewWorn.insert(item->id);
        }
      }
      break;
    case DesignerDomains::Domain::Map:
      mapDraft = entry.map;
      mapPreviewValid = false;
      mapPreviewError.clear();
      mapPreviewZoom = 0.0f;
      mapSaveMessage.clear();
      break;
    case DesignerDomains::Domain::Tile: {
      // By name, not by pointer: the editor writes through a pointer into the registry, so
      // showing a tile means pointing the editor at the registry's current definition of
      // that name. `LoadEditorFrom` also clears the dirty marker, which belongs to the
      // definition being edited rather than to the window -- the edit itself is already
      // live in the world (`TouchRuntime`), only the overlay file lags.
      const TileDef *definition =
          game != nullptr ? game->tileRegistry.Get(entry.tileName) : nullptr;
      if (definition != nullptr) {
        editedIsRuntime = entry.tileIsRuntime;
        LoadEditorFrom(*definition);
      }
      break;
    }
  }
}

void DesignerWindow::DrawViewedToolbar() {
  if (viewed.empty()) return;

  const ViewedResult &current = viewed[viewedIndex];
  const char *domainLabel = DesignerDomains::For(current.domain).label.c_str();

  ImGui::BeginDisabled(viewedIndex == 0);
  if (ImGui::ArrowButton("##viewed_prev", ImGuiDir_Left)) {
    ShowViewed(viewedIndex - 1);
  }
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip("Back to the previous result");
  }

  ImGui::SameLine();
  ImGui::BeginDisabled(viewedIndex + 1 >= viewed.size());
  if (ImGui::ArrowButton("##viewed_next", ImGuiDir_Right)) {
    ShowViewed(viewedIndex + 1);
  }
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip("Forward to the next result");
  }

  ImGui::SameLine();
  // The list is the same history the arrows walk, newest first, so the two cannot disagree
  // about what is in it or which entry is selected. `(3/11)` is there because "which one am
  // I looking at" is otherwise unanswerable in a combo that shows only its own selection.
  char preview[256];
  std::snprintf(preview, sizeof(preview), "%s '%s'  (%zu/%zu)", domainLabel,
                current.label.empty() ? "(unnamed)" : current.label.c_str(),
                viewedIndex + 1, viewed.size());
  ImGui::SetNextItemWidth(-1.0f);
  if (ImGui::BeginCombo("##viewed_list", preview)) {
    for (size_t i = viewed.size(); i-- > 0;) {
      const ViewedResult &entry = viewed[i];
      char item[256];
      std::snprintf(item, sizeof(item), "%s '%s'##viewed%zu",
                    DesignerDomains::For(entry.domain).label.c_str(),
                    entry.label.empty() ? "(unnamed)" : entry.label.c_str(), i);
      if (ImGui::Selectable(item, i == viewedIndex)) {
        ShowViewed(i);
      }
    }
    ImGui::EndCombo();
  }
  ImGui::Separator();
}

void DesignerWindow::DrawBlueprintPreview() {
  if (!lastDesign.has_value()) {
    ImGui::TextWrapped(
        "The design from the latest reply appears here. A reply is drawn whether or not "
        "it passed validation -- a refused design is the one most worth looking at.");
    return;
  }

  const PreviewedDesign &design = *lastDesign;
  const Blueprint &blueprint = design.blueprint;

  // The verdict banner is the one part not shared with the Blueprints window: it is about
  // this conversation's reply, not about the design as content.
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

  BlueprintPreview::DrawZoomControl("##designer_zoom", blueprintZoom);

  const float canvasHeight =
      std::max(120.0f, ImGui::GetContentRegionAvail().y * 0.55f);
  if (game != nullptr) {
    BlueprintPreview::DrawCanvas("designer_preview_canvas", blueprint,
                                 design.expansion, blueprintZoom, game->tileRegistry,
                                 &game->objectFactory, game->resourceManager,
                                 canvasHeight);
    BlueprintPreview::DrawLegend(design.expansion, game->tileRegistry);
  }

  // No second switch here any more. The run is driven by the header's "Art
  // automatically", and this line says so where the assets it will build are listed --
  // otherwise the same question is answered by a control the user has to find twice.
  if (autoRequestArt && !design.needs.empty()) {
    ImGui::TextDisabled(
        "Art automatically is on: these will be created when the design is saved.");
  }

  // What this design cannot be drawn with, and a button per item. Shared with the
  // Blueprints window so the two previews cannot disagree about what is missing, and
  // shown for "no art yet" as well as for genuinely absent materials.
  if (game != nullptr) {
    SpriteGapPanel::Draw(game, game->tileRegistry, game->objectFactory, blueprint,
                         design.needs);
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

void DesignerWindow::InspectFinishedExchange(size_t index) {
  // The reply verbatim, logged before anything is extracted from it, with the request
  // number and how long the backend took. Both are the session's, so every domain's
  // transcript reports them the same way.
  if (index >= session->Size()) return;
  const DesignerSession::Exchange finished = session->BeginInspection(index);

  // A pixel-document reply is inspected by the compiler, not by the definition
  // validators: it has no `name`, no components and no glyph, and handing it to
  // ApplyReply would refuse it for the wrong reason.
  if (finished.conversation == kArtConversation) {
    InspectFinishedArtDocument(index, finished);
    return;
  }

  if (finished.failed) {
    session->LogEvent("TRANSPORT FAILURE: " + finished.error);
    session->Update(index, [](DesignerSession::Exchange &exchange) {
      exchange.inspected = true;
    });
    session->PushTurn(kDefinitionConversation, finished.prompt, "");
    return;
  }

  // A building reply is a blueprint, not a definition: it has no glyph, no components and
  // no art of its own, and handing it to either sprite validator would refuse it for
  // lacking fields the building grammar never had.
  if (designMode == DesignerDomains::Domain::Blueprint) {
    InspectBlueprintReply(index, finished);
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
  if (designMode == DesignerDomains::Domain::Object && finished.wasSet) {
    ObjectFactory *objects = Objects();
    if (objects == nullptr) {
      reason = "there is no object factory to add to.";
    } else {
      const ObjectDesigner::SetApplyResult set = ObjectDesigner::ApplySetReply(
          finished.ParsedAnswer(), *objects,
          ModeOverlayDirectoryFor(designMode),
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
  } else if (designMode == DesignerDomains::Domain::Tile && finished.wasSet) {
    TileRegistry *registry = game != nullptr ? &game->tileRegistry : nullptr;
    if (registry == nullptr) {
      reason = "there is no tile catalog to add to.";
    } else {
      const TileDesigner::SetApplyResult set = TileDesigner::ApplySetReply(
          finished.ParsedAnswer(), *registry,
          ModeOverlayDirectoryFor(designMode));
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
  } else if (designMode == DesignerDomains::Domain::Rig) {
    if (game == nullptr) {
      reason = "there is no world to validate a rig against.";
    } else {
      const RigDesigner::ApplyResult result =
          RigDesigner::ApplyReply(finished.ParsedAnswer());
      applied.ok = result.ok;
      applied.error = result.error;
      applied.name = result.draft.name;
      applied.notes = result.notes;
      applied.warnings = result.draft.warnings;
      if (result.ok) {
        rigDraft = result.draft;
        rigError.clear();
        rigArtQueue.clear();
        rigArtStamps.clear();
        rigPreviewWorn.clear();
        std::snprintf(rigNameBuffer, sizeof(rigNameBuffer), "%s",
                      result.draft.name.c_str());
        for (const ItemDef *item : game->itemRegistry.GetAll()) {
          if (!item->wornSprite.empty()) rigPreviewWorn.insert(item->id);
        }
        rigArtMessage =
            "Rig drafted. Draw its layers, look at it, then save it.";
        // "Art automatically" means exactly that for a rig: its layers *are* the art, and
        // a drafted body with none draws nothing. Missing layers are queued; layers that
        // already have art are left alone, so redrafting a body does not silently
        // overwrite art someone may have kept.
        if (autoRequestArt && game->TileArtQueueState().Enabled()) {
          int queued = 0;
          for (const RigDesigner::Part &part : rigDraft->parts) {
            std::string resolved;
            if (SpriteResolves(std::string(), part.sprite, &resolved)) continue;
            QueueRigPartArt(part);
            ++queued;
          }
          rigArtMessage =
              queued > 0
                  ? "Rig drafted. Art automatically is on: " +
                        std::to_string(queued) + " layer(s) queued."
                  : "Rig drafted. Every layer already has art.";
        }
      } else {
        rigError = result.error;
      }
    }
  } else if (designMode == DesignerDomains::Domain::Map) {
    if (game == nullptr) {
      reason = "there is no world to validate a map against.";
    } else {
      const MapDesigner::ApplyResult result = MapDesigner::ApplyReply(
          finished.ParsedAnswer(), game->tileRegistry, game->objectFactory);
      applied.ok = result.ok;
      applied.error = result.error;
      applied.name = result.draft.name;
      applied.notes = result.notes;
      applied.warnings = result.draft.warnings;
      if (result.ok) {
        mapDraft = result.draft;
        mapPreviewValid = false;
        mapPreviewError.clear();
        mapPreviewZoom = 0.0f;
        mapSaveMessage.clear();
        mapSaveIsError = false;
      } else {
        mapPreviewError = result.error;
      }
    }
  } else if (designMode == DesignerDomains::Domain::Object) {
    ObjectFactory *objects = Objects();
    if (objects == nullptr) {
      reason = "there is no object factory to add to.";
    } else {
      const ObjectDesigner::ApplyResult result = ObjectDesigner::ApplyReply(
          finished.ParsedAnswer(), *objects,
          ModeOverlayDirectoryFor(DesignerDomains::Domain::Object),
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
          ModeOverlayDirectoryFor(DesignerDomains::Domain::Tile));
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

  session->Update(index, [&](DesignerSession::Exchange &ex) {
    ex.inspected = true;
    ex.ok = applied.ok;
    ex.failureReason =
        applied.ok ? std::string() : reason.empty() ? applied.error : reason;
    ex.name = applied.name;
    ex.savedPath = applied.savedPath;
    ex.notes = applied.notes;
    ex.warnings = applied.warnings;
    ex.saveFailed = applied.saveFailed;
    ex.definition = applied.tileDefinition;
    ex.wasSet = applied.wasSet;
    ex.setName = applied.setName;
    ex.acceptedNames = applied.acceptedNames;
    ex.entryVerdicts = applied.entryVerdicts;
  });

  // The preview, on the accepted branch only. For a tile this is the pointer the editor
  // writes through; for an object it is a copy, because ObjectFactory owns its templates
  // by value and a pointer into that map is invalidated by the next runtime add.
  if (applied.ok) {
    if (designMode == DesignerDomains::Domain::Map) {
      ViewedResult result;
      result.domain = designMode;
      result.label = applied.name;
      result.map = mapDraft;
      CaptureViewed(std::move(result));
    } else if (designMode == DesignerDomains::Domain::Rig) {
      ViewedResult result;
      result.domain = designMode;
      result.label = applied.name;
      result.rig = rigDraft;
      CaptureViewed(std::move(result));
    } else {
      if (designMode == DesignerDomains::Domain::Object) {
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

      // One entry per accepted reply, not per entry of a set: the panes show one thing -- the
      // last definition the reply produced -- and an entry per sprite would make the arrows
      // walk a set while the pane showed the same object six times.
      ViewedResult result;
      result.domain = designMode;
      result.label = applied.wasSet && !applied.setName.empty() ? applied.setName
                                                               : applied.name;
      result.tileIsRuntime = true;
      if (designMode == DesignerDomains::Domain::Object) {
        result.object = editedObject;
        result.objectKey = editedObjectKey;
        result.objectOverlayPath = editedObjectOverlayPath;
      } else {
        result.tileName = applied.name;
      }
      CaptureViewed(std::move(result));
    }
  }

  // The conversation both sides saw, so a follow-up can refer to this tile. The
  // assistant turn is the raw reply minus its JSON block; the capped replay is what keeps
  // a large definition from accumulating.
  session->PushTurn(kDefinitionConversation, finished.prompt,
                    finished.HistoryAnswer(session->HiddenMarker()));

  if (applied.ok) {
    if (applied.wasSet) {
      // Names every accepted entry: the point of a set is *which* sprites it produced, and a
      // single "ADDED: <last one>" line would hide the rest.
      session->LogEvent("ADDED SET" +
               (applied.setName.empty() ? std::string()
                                        : " '" + applied.setName + "'") +
               ": " + std::to_string(applied.acceptedNames.size()) + " of " +
               std::to_string(applied.entryVerdicts.size()) + " definition(s)");
      for (size_t entry = 0; entry < applied.entryVerdicts.size(); ++entry) {
        const bool accepted = applied.entryVerdicts[entry].first;
        session->Log("  [" + std::to_string(entry) + "] " +
            (accepted ? "ACCEPTED " : "REFUSED  ") + applied.entryVerdicts[entry].second +
            "\n");
      }
    } else {
      session->LogEvent("ADDED: " + applied.name +
               (applied.savedPath.empty() ? " (not saved)"
                                          : " -> " + applied.savedPath));
    }
    if (!applied.notes.empty()) {
      session->Log("designerNotes: " + applied.notes + "\n");
    }
    for (const std::string &warning : applied.warnings) {
      session->Log("warning: " + warning + "\n");
    }
    if (game != nullptr && game->debugLog) {
      game->debugLog->LogInfo(std::string("Sprite designer: added ") +
                              ModeName() + " '" + applied.name +
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
    // A map is drawn from its tiles, so there is no sprite to ask the art queue for.
    std::vector<std::string> artNames;
    if (DesignerDomains::IsSprite(designMode)) {
      artNames = applied.acceptedNames;
      if (artNames.empty() && !applied.name.empty()) {
        artNames.push_back(applied.name);
      }
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
        RequestSpriteDocument(subject, SpriteKindOf(designMode));
        artNote = "asked for a sprite document for '" + subject +
                  "' -- it is compiled before anything is written";
        if (artNames.size() > 1) {
          artNote += "; the other " + std::to_string(artNames.size() - 1) +
                     " of the set need their own request";
        }
        session->LogEvent("SPRITE DOCUMENT REQUESTED (automatic): " + subject);
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
        session->LogEvent("ART NOT REQUESTED: " + unavailable);
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
          session->LogEvent(std::string("ART REQUESTED (automatic): ") + estimate);
          for (const std::string &name : accepted) {
            session->Log("  queued: " + name + "\n");
          }
        }
        if (!refused.empty()) {
          if (artNote.empty()) artNote = "no art was requested -- ";
          else artNote += " ";
          artNote += std::to_string(refused.size()) + " refused";
          session->LogEvent("ART NOT REQUESTED for " + std::to_string(refused.size()) +
                   " sprite(s):");
          for (const std::string &line : refused) {
            session->Log("  " + line + "\n");
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
    session->Update(index, [&](DesignerSession::Exchange &exchange) {
      exchange.artNote = artNote;
    });
    session->ResetAttempts(kDefinitionConversation);
    return;
  }

  // Refused: say why, in the same conversation, and try again. Bounded, so a model that
  // cannot satisfy the contract cannot spend the whole budget.
  //
  // `finished.wasSet` carries the shape into the retry: without it a set is retried as a
  // single definition, refused again for having no top-level "name", and burns the whole
  // budget on a question the model already answered -- which is what the first run of this
  // did.
  session->RetryRefused(kDefinitionConversation, applied.error,
                        ArtifactNoun(designMode), finished.wasSet);
}

const DesignerDomains::Descriptor &DesignerWindow::ModeDescriptor() const {
  // Address of the table row. The table is a function-local static, so the reference is
  // stable for the run and a caller may hold it.
  return DesignerDomains::For(mode);
}

const char *DesignerWindow::ModeName() const {
  // The descriptor's id, which is a stable string literal in the table rather than a
  // temporary: this is used in log lines and prose that outlive a mode change. Returning
  // the pointer into the table is what makes it safe -- a std::string here would dangle
  // the moment the expression ended.
  return DesignerDomains::For(mode).id.c_str();
}

SpriteKinds::Kind DesignerWindow::SpriteKindOf(
    DesignerDomains::Domain domain) {
  return DesignerDomains::SpriteOf(domain);
}

std::string DesignerWindow::ModeOverlayDirectoryFor(
    DesignerDomains::Domain domain) const {
  // A building has no overlay: a blueprint is saved by the blueprint registry into
  // content.blueprints, and an overlay directory it never uses would be a path a caller
  // could write a blueprint into and watch vanish.
  if (!DesignerDomains::IsSprite(domain)) {
    return {};
  }
  // Read from the settings rather than from the descriptor's default: the descriptor
  // carries the default so a caller with no settings can still name a path, and this is
  // the caller that has settings.
  const GameSettings &settings = Settings();
  return SpriteKindOf(domain) == SpriteKinds::Kind::Object
             ? settings.content.generatedObjects
             : settings.content.generatedTiles;
}

std::string DesignerWindow::ModeOverlayDirectory() const {
  return ModeOverlayDirectoryFor(mode);
}

ObjectFactory *DesignerWindow::Objects() const {
  return game != nullptr ? &game->objectFactory : nullptr;
}

void DesignerWindow::RefreshRewriteBrief() {
  // The Improve button has to speak the vocabulary of the kind being designed, or an
  // "improved" object request comes back enriched with ground-surface words and the
  // designer has no way to honour it. See RewriteBriefs.hpp.
  if (mode == DesignerDomains::Domain::Blueprint) {
    rewriter.SetBrief(RewriteBriefs::Blueprint());
  } else if (mode == DesignerDomains::Domain::Object) {
    rewriter.SetBrief(RewriteBriefs::Object());
  } else if (mode == DesignerDomains::Domain::Map) {
    rewriter.SetBrief(RewriteBriefs::Map());
  } else {
    rewriter.SetBrief(RewriteBriefs::Tile());
  }
}

void DesignerWindow::SendActorSpriteRequest(const std::string &description) {
  if (game == nullptr) return;

  // The name in the box is the filename, and the same slug the actor registry uses, so an
  // actor authored later with this name finds the sprite that was drawn for it.
  nlohmann::json named = nlohmann::json::object();
  named["name"] = std::string(actorSpriteNameBuffer);
  const std::string name = ActorSpriteRegistry::IdFor(named);
  if (name.empty()) {
    statusIsError = true;
    statusLine = "Give the sprite a name first: the box beside the mode is the filename "
                 "an actor will look for.";
    return;
  }

  // A sprite with no entry is not browsable: the Content Editor lists the catalogue, so
  // the catalogue gains an entry for what was drawn. It records the name and the
  // description only -- appearance, never a creature's stats, race or behaviour.
  if (!Settings().content.actorSprites.empty()) {
    nlohmann::json entry = nlohmann::json::object();
    entry["name"] = std::string(actorSpriteNameBuffer);
    if (!description.empty()) entry["description"] = description;
    const std::filesystem::path directory = Settings().content.actorSprites;
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    const std::filesystem::path path = directory / (name + ".json");
    std::ofstream out(path);
    std::string error;
    std::vector<std::string> warnings;
    if (!out.is_open()) {
      statusIsError = true;
      statusLine = "Could not write the sprite catalogue entry to " + path.string();
      return;
    }
    out << entry.dump(2) << "\n";
    out.close();
    // Live too, so it appears without a restart.
    game->actorSpriteRegistry.AddOrReplace(entry, path.string(), &error, &warnings);
    session->LogEvent("ACTOR SPRITE ENTRY WRITTEN: " + path.string());
  }

  // `requirement` overrides the catalog description, which is what makes this a prompt for
  // a *new* being rather than a redraw of one that exists: the text typed is the brief.
  RequestSpriteDocument(name, SpriteKinds::Kind::Actor, /*replaceGenerated=*/true,
                        std::string(), description);
}

void DesignerWindow::RequestRoute(const std::string &text, bool automatic) {
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
  routeNote = "asking which domain this is...";
  session->LogEvent(std::string("ROUTE REQUESTED") +
                    (automatic ? " (automatic)" : "") + ": " + text);

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

bool DesignerWindow::BeginAutoRoute(const std::string &prompt) {
  // No classifier: send with the domain that is already selected rather than dropping the
  // request. The setting cannot be honoured, so the request still goes out and the note
  // says why -- the alternative is a Send button that silently does nothing.
  if (game == nullptr || game->systemOneClassifier == nullptr) {
    routeNote = "automatic routing is on, but no System 1 classifier is configured; "
                "this request was built for the current domain";
    SendUserMessage(prompt);
    return true;
  }
  // The Send button is disabled while a route is pending, so this is a belt-and-braces
  // guard rather than the normal path. Declining rather than queueing is deliberate: the
  // caller keeps the text, so nothing is lost and the user can press Send again.
  if (routeInFlight || !autoSendPrompt.empty()) return false;

  autoSendPrompt = prompt;
  RequestRoute(prompt, /*automatic=*/true);
  return true;
}

void DesignerWindow::ApplyRouteVerdict(const SpriteRouter::Verdict &verdict) {
  if (verdict.unsure) {
    // The answer arrived but is below the active profile's threshold. The mode stays
    // where it is -- switching on a coin flip is worse than doing nothing -- and the
    // window says what it read so the threshold itself can be judged.
    routeNote = "the classifier was not confident enough to act: " + verdict.why;
    session->LogEvent("ROUTE: unsure -- " + routeNote);
    return;
  }

  if (verdict.domain == nullptr) {
    // Unanswered is not a verdict. The mode stays where it is, and the window says why
    // rather than picking the more likely kind.
    routeNote = verdict.error.empty()
                    ? "the classifier did not say what this is, so the mode is "
                      "unchanged"
                    : "the classifier could not be used: " + verdict.error;
    session->LogEvent("ROUTE: no domain -- " + routeNote);
    return;
  }

  mode = verdict.domain->domain;
  RefreshRewriteBrief();
  if (game != nullptr) {
    session->SetSystemPrompt(kDefinitionConversation,
                             game->BuildDesignerPrompt(mode, requestSet));
  }

  // The shape too, when the classifier answered it -- that is what the question is for. An
  // unanswered shape leaves the checkbox alone rather than being guessed, for the same reason
  // an unanswered domain leaves the dropdown alone. A building never answers it: one reply
  // is one blueprint, and `SpriteRouter::Parse` clears the shape for a building so a caller
  // cannot apply a sprite's idea of "single" to it.
  if (verdict.shapeAnswered) {
    requestSet = verdict.sheet;
    requestSetSize = verdict.requested;
    if (game != nullptr) {
      session->SetSystemPrompt(kDefinitionConversation,
                               game->BuildDesignerPrompt(mode, requestSet));
    }
  } else if (!DesignerDomains::IsSprite(mode)) {
    // Leaving a stale "Set" checkbox ticked under a building would build the next
    // building's prompt as if a set had been asked for.
    requestSet = false;
    requestSetSize = 0;
  }

  routeNote = "the classifier read this as " + verdict.domain->label;
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
  session->LogEvent("ROUTE: " + verdict.domain->id + " -- " + routeNote);
}

void DesignerWindow::DrawObjectPreview() {
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

void DesignerWindow::QueueRigPartArt(const RigDesigner::Part &part) {
  RigArtRequest request;
  request.label = part.id;
  request.sprite = part.sprite;
  request.requirement = RigDesigner::FrameContract() + "\n\nThis layer: " +
                        part.id + " -- " + part.brief +
                        "\nDraw only this layer; the rest of the 32x32 frame stays "
                        "transparent.";
  rigArtQueue.push_back(std::move(request));
  rigArtMessage = "Queued the " + part.id + " layer (" +
                  std::to_string(rigArtQueue.size()) + " waiting).";
}

void DesignerWindow::QueueWornArt(const std::string &itemId) {
  if (game == nullptr) return;
  const ItemDef *item = game->itemRegistry.Get(itemId);
  if (item == nullptr || item->wornSprite.empty()) return;
  std::string hides;
  for (const std::string &part : item->wornHides) {
    if (!hides.empty()) hides += ", ";
    hides += part;
  }
  RigArtRequest request;
  request.label = itemId;
  request.sprite = item->wornSprite;
  request.requirement =
      RigDesigner::FrameContract() + "\n\nThis layer is worn equipment: " + itemId +
      ". Draw it over that frame, aligned with the body it is worn on. " +
      (hides.empty()
           ? "It hides no rig part; it is drawn in front of the body."
           : "It hides the rig part(s): " + hides + ".") +
      " Draw only the worn item, not the body underneath.";
  rigArtQueue.push_back(std::move(request));
  rigArtMessage = "Queued worn art for " + itemId + " (" +
                  std::to_string(rigArtQueue.size()) + " waiting).";
}

void DesignerWindow::PumpRigArtQueue() {
  if (!rigDraft.has_value() || rigArtQueue.empty() || game == nullptr) return;
  if (session == nullptr || session->IsBusy()) return;
  const RigArtRequest request = rigArtQueue.front();
  rigArtQueue.pop_front();
  // Kind Actor: a rig layer is an actor sprite with a fixed frame, and the sprite-document
  // prompt already knows how to talk about an actor's look.
  RequestSpriteDocument(request.sprite, SpriteKinds::Kind::Actor,
                        /*replaceGenerated=*/true, "", request.requirement);
  rigArtMessage = "Asked the art pipeline for " + request.label + ".";
}

void DesignerWindow::RefreshRigArtMemo() {
  if (!rigDraft.has_value() || game == nullptr) return;
  // A layer's compiled PNG lands asynchronously. The texture cache remembers "missing",
  // so the first sight of a file (and every redraw, which changes its write time) has to
  // forget that path or the preview stays empty after the art arrives.
  const auto refresh = [this](const std::string &sprite) {
    std::string path;
    if (!SpriteResolves(std::string(), sprite, &path) || path.empty()) return;
    std::error_code ec;
    const auto stamp = std::filesystem::last_write_time(path, ec);
    if (ec) return;
    const auto found = rigArtStamps.find(sprite);
    if (found != rigArtStamps.end() && found->second == stamp) return;
    rigArtStamps[sprite] = stamp;
    game->ForgetSpriteMemo(std::string(), sprite);
  };
  for (const RigDesigner::Part &part : rigDraft->parts) refresh(part.sprite);
  for (const ItemDef *item : game->itemRegistry.GetAll()) {
    if (!item->wornSprite.empty()) refresh(item->wornSprite);
  }
}

void DesignerWindow::SaveRig() {
  if (!rigDraft.has_value() || game == nullptr) return;
  RigDesigner::Draft &draft = *rigDraft;
  draft.name = std::string(rigNameBuffer);
  draft.slug = RigDesigner::SlugFor(draft.name);
  if (draft.slug.empty()) {
    rigArtMessage = "Give the rig a name before saving.";
    return;
  }
  const bool existed = game->rigRegistry.Get(draft.slug) != nullptr;
  std::string savedPath;
  std::string error;
  if (!RigDesigner::Save(draft, Settings().content.generatedRigs, &savedPath, &error)) {
    rigArtMessage = error;
    return;
  }
  game->rigRegistry.Load(Settings().content.generatedRigs);
  rigArtMessage = "Saved " + savedPath +
                  (existed ? " (the existing rig with that id is replaced)" : "") +
                  ". It is now in the Content Editor under Actor rigs.";
  if (game->debugLog) {
    game->debugLog->LogInfo("Rig designer: saved " + savedPath);
  }
}

void DesignerWindow::DrawRigPreview() {
  if (!rigDraft.has_value()) {
    ImGui::TextWrapped(
        "Ask for a character body and its layer briefs appear here. Draw a layer, look "
        "at the composed body, then save the rig.");
    ImGui::TextDisabled(
        "For example: \"a broad, soot-marked village blacksmith\".");
    if (!rigError.empty()) {
      ImGui::Separator();
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.62f, 0.5f, 1.0f));
      ImGui::TextWrapped("%s", rigError.c_str());
      ImGui::PopStyleColor();
    }
    return;
  }
  RigDesigner::Draft &draft = *rigDraft;

  ImGui::TextDisabled("Rig");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(-1.0f);
  if (ImGui::InputText("##rig_name", rigNameBuffer, sizeof(rigNameBuffer))) {
    draft.name = std::string(rigNameBuffer);
    draft.slug = RigDesigner::SlugFor(draft.name);
  }
  if (!draft.description.empty()) {
    ImGui::TextWrapped("%s", draft.description.c_str());
  }
  if (!draft.designerNotes.empty()) {
    ImGui::TextDisabled("%s", draft.designerNotes.c_str());
  }

  // The composed body first: the look is what this slice is for.
  ActorRig renderRig;
  renderRig.id = draft.slug;
  renderRig.name = draft.name;
  renderRig.width = draft.width;
  renderRig.height = draft.height;
  for (const RigDesigner::Part &part : draft.parts) {
    renderRig.parts.push_back({part.id, part.sprite, part.z});
  }
  std::vector<RigRender::WornLayer> worn;
  for (const ItemDef *item : game->itemRegistry.GetAll()) {
    if (item->wornSprite.empty() || rigPreviewWorn.count(item->id) == 0) continue;
    RigRender::WornLayer layer;
    layer.label = item->id;
    layer.texture =
        RigRender::ResolveRigSprite(item->wornSprite, game->resourceManager);
    layer.z = item->wornZ;
    layer.hides = item->wornHides;
    worn.push_back(std::move(layer));
  }
  ImGui::Separator();
  RigRender::Draw(renderRig, game->resourceManager, worn, 200.0f);
  ImGui::Separator();

  // The layers, each with its own draw button: a layer the model got wrong can be redrawn
  // without touching the five that are right.
  ImGui::TextDisabled("Layers");
  for (const RigDesigner::Part &part : draft.parts) {
    ImGui::PushID(part.id.c_str());
    std::string resolved;
    const bool drawn = SpriteResolves(std::string(), part.sprite, &resolved);
    ImGui::Text("%s%s", part.id.c_str(), drawn ? "" : "   (no art yet)");
    ImGui::SameLine();
    if (ImGui::SmallButton(drawn ? "Redraw" : "Draw")) {
      QueueRigPartArt(part);
    }
    ImGui::TextDisabled("%s", part.brief.c_str());
    ImGui::PopID();
  }

  ImGui::Separator();
  ImGui::TextDisabled("Worn equipment (preview)");
  for (const ItemDef *item : game->itemRegistry.GetAll()) {
    if (item->wornSprite.empty()) continue;
    ImGui::PushID(item->id.c_str());
    bool on = rigPreviewWorn.count(item->id) > 0;
    if (ImGui::Checkbox(item->id.c_str(), &on)) {
      if (on) {
        rigPreviewWorn.insert(item->id);
      } else {
        rigPreviewWorn.erase(item->id);
      }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Redraw art")) {
      QueueWornArt(item->id);
    }
    std::string hides;
    for (const std::string &part : item->wornHides) {
      if (!hides.empty()) hides += ", ";
      hides += part;
    }
    if (!hides.empty()) {
      ImGui::TextDisabled("hides: %s", hides.c_str());
    }
    ImGui::PopID();
  }

  ImGui::Separator();
  const bool exists = game->rigRegistry.Get(draft.slug) != nullptr;
  if (exists) {
    ImGui::TextDisabled(
        "A rig with id '%s' already exists; saving replaces it in the world.",
        draft.slug.c_str());
  }
  if (ImGui::Button("Save rig")) {
    SaveRig();
  }
  if (!rigArtQueue.empty()) {
    ImGui::SameLine();
    ImGui::TextDisabled("%zu art request(s) queued", rigArtQueue.size());
  }
  if (!rigArtMessage.empty()) {
    ImGui::TextWrapped("%s", rigArtMessage.c_str());
  }
}

void DesignerWindow::DrawMapPreview() {
  if (!mapDraft.has_value()) {
    ImGui::TextWrapped(
        "Ask for a map in the prompt and it appears here, drawn with the world's "
        "own tiles and objects.");
    ImGui::TextDisabled(
        "For example: \"a small walled courtyard with a well and two chests\".");
    if (!mapPreviewError.empty()) {
      ImGui::Separator();
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.62f, 0.5f, 1.0f));
      ImGui::TextWrapped("%s", mapPreviewError.c_str());
      ImGui::PopStyleColor();
    }
    return;
  }

  const MapDesigner::Draft &draft = *mapDraft;
  ImGui::TextUnformatted(draft.name.c_str());
  if (!draft.description.empty()) {
    ImGui::TextWrapped("%s", draft.description.c_str());
  }
  ImGui::TextDisabled("%d x %d tiles (%d x %d px each) | %d object(s)", draft.width,
                      draft.height, draft.tileWidth, draft.tileHeight,
                      draft.objectCount);
  ImGui::Separator();

  if (!mapPreviewValid) {
    mapPreviewValid = MapPreview::Parse(draft.document, game->tileRegistry,
                                        mapPreviewInfo, &mapPreviewError);
  }
  if (mapPreviewValid) {
    if (ImGui::SmallButton("Fit")) {
      mapPreviewZoom = 0.0f;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("scroll to zoom, drag to pan, double-click to fit");
    const ImVec2 available = ImGui::GetContentRegionAvail();
    const float previewHeight = std::max(60.0f, available.y - 90.0f);
    MapPreview::Draw(mapPreviewInfo, game->resourceManager, &game->objectFactory,
                     &mapPreviewZoom, available.x, previewHeight);
  } else {
    ImGui::TextWrapped("The draft could not be previewed: %s",
                       mapPreviewError.c_str());
  }

  ImGui::Separator();
  // Saving is a separate step on purpose: a reply is a design, and a design that
  // writes a file on arrival leaves one file per attempt.
  ImGui::BeginDisabled(draft.name.empty() || game->mapReloader == nullptr);
  if (ImGui::Button("Save to maps/")) {
    std::string savedPath;
    std::string error;
    const std::string directory =
        game->mapReloader != nullptr ? game->mapReloader->GetDirectory() : "";
    if (MapDesigner::Save(draft, directory, &savedPath, &error)) {
      mapSaveMessage = "Saved " + savedPath;
      mapSaveIsError = false;
      game->mapReloader->RefreshMapList();
      if (game->debugLog) {
        game->debugLog->LogInfo("Map designer: saved " + savedPath);
      }
    } else {
      mapSaveMessage = error;
      mapSaveIsError = true;
    }
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::TextDisabled("refuses to overwrite an existing map");
  if (!mapSaveMessage.empty()) {
    if (mapSaveIsError) {
      ImGui::TextColored(ImVec4(1.0f, 0.62f, 0.5f, 1.0f), "%s",
                         mapSaveMessage.c_str());
    } else {
      ImGui::TextDisabled("%s", mapSaveMessage.c_str());
    }
  }
  for (const std::string &warning : draft.warnings) {
    ImGui::TextDisabled("! %s", warning.c_str());
  }
}

void DesignerWindow::DrawPreviewPane() {
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

std::string DesignerWindow::DescribeArtState() const {
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

void DesignerWindow::DrawDslPanels(const std::string &name,
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

void DesignerWindow::DrawArtSection() {
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

std::optional<TileArtQueue::Job> DesignerWindow::JobForEditedTile() const {
  if (game == nullptr || edited == nullptr) return std::nullopt;
  for (const TileArtQueue::Job &candidate : game->TileArtQueueState().Snapshot()) {
    if (candidate.tileName == edited->name) {
      return candidate;
    }
  }
  return std::nullopt;
}

int DesignerWindow::CellIndexInWorklist(const TileArtQueue::Job &job) const {
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

std::vector<DesignerWindow::Artifact> DesignerWindow::ArtifactsFor(
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

void DesignerWindow::DrawArtifactsPanel(const TileArtQueue::Job &job) {
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

void DesignerWindow::EnsurePaletteLoaded() {
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

int DesignerWindow::NearestPaletteIndex(unsigned char r, unsigned char g,
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

void DesignerWindow::EnsurePixelView(const std::string &spritePath) {
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

void DesignerWindow::DrawPixelViewPanel(const TileArtQueue::Job &job) {
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

raylib::Texture *DesignerWindow::LoadThumbnail(const std::string &path,
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

void DesignerWindow::DrawFilmstripPanel(const TileArtQueue::Job &job) {
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

DesignerWindow::ThumbnailLookup DesignerWindow::InspectThumbnailForTest(
    const std::string &path) {
  ThumbnailLookup lookup;
  raylib::Texture *texture = LoadThumbnail(path, 320);
  lookup.textureId = texture != nullptr ? texture->id : 0u;
  std::error_code ec;
  lookup.stamp = std::filesystem::last_write_time(path, ec);
  lookup.hasStamp = !ec;
  return lookup;
}

DesignerWindow::PixelViewLookup DesignerWindow::InspectPixelViewForTest(
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

std::string DesignerWindow::DefinitionPromptFor(const std::string &name,
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

void DesignerWindow::StartAutoArt(const Blueprint &design,
                                      const std::vector<BlueprintNeed> &needs) {
  // The design itself is not used yet: the automatic path carries the designer's own
  // description of each asset, which is the brief that matters. The crop and style read
  // stay on the manual path until this one has been seen working.
  (void)design;
  if (game == nullptr || !session->HasBackend() || needs.empty()) {
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
  game->designerWindowEntity.set<ActiveWindow>({game->designerWindow});
  statusIsError = false;
  statusLine = "Creating " + std::to_string(autoNeeds.size()) +
               " requested asset(s), one at a time.";
  session->LogEvent("AUTO ART STARTED: " + std::to_string(autoNeeds.size()) + " asset(s)");
}

void DesignerWindow::PumpAutoArt() {
  if (!autoRunning) return;

  if (autoIndex >= autoNeeds.size()) {
    autoRunning = false;
    if (autoFailures.empty()) {
      statusIsError = false;
      statusLine =
          "Created all " + std::to_string(autoNeeds.size()) + " requested asset(s).";
      session->LogEvent("AUTO ART DONE: all assets created");
    } else {
      std::string names;
      for (const std::string &failure : autoFailures) {
        names += (names.empty() ? "" : ", ") + failure;
      }
      statusIsError = true;
      statusLine = "Auto art finished; could not create: " + names;
      session->LogEvent("AUTO ART DONE WITH FAILURES: " + names);
    }
    return;
  }

  const BlueprintNeed &need = autoNeeds[autoIndex];

  // A turn in flight is this item's turn: the run only starts from an idle window.
  if (session->IsBusy()) {
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
    mode = need.kind == "tile" ? DesignerDomains::Domain::Tile
                               : DesignerDomains::Domain::Object;
    requestSet = false;
    session->SetSystemPrompt(kDefinitionConversation,
                             game->BuildDesignerPrompt(mode, false));
    session->LogEvent("AUTO ART ITEM: " + need.name);
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
    session->LogEvent("AUTO ART ITEM FAILED: " + need.name);
    ++autoIndex;
    autoStarted = false;
    autoIdleFrames = 0;
  }
}

void DesignerWindow::DrawGapOffer() {
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
      if (kind != SpriteKinds::Kind::Actor) {
        mode = kind == SpriteKinds::Kind::Object ? DesignerDomains::Domain::Object
                                                 : DesignerDomains::Domain::Tile;
      }
      requestSet = false;
      if (game != nullptr) {
        session->SetSystemPrompt(kDefinitionConversation,
                                 game->BuildDesignerPrompt(mode, false));
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

void DesignerWindow::DrawDomainPicker(
    const DesignerDomains::Descriptor &current) {
  ImGui::SetNextItemWidth(120.0f);
  if (ImGui::BeginCombo("##designer_domain", current.label.c_str())) {
    for (const DesignerDomains::Descriptor &candidate : DesignerDomains::All()) {
      const bool selected = candidate.domain == mode;
      if (ImGui::Selectable(candidate.label.c_str(), selected)) {
        if (candidate.domain != mode) {
          mode = candidate.domain;
          // With nothing in the history there is nothing pinned, so the pane follows the
          // domain about to be designed for. Once something is being viewed, changing
          // the design domain must not yank the pane away from it.
          if (viewed.empty()) paneDomain = mode;
          // A building has no shape: leaving "Set" ticked from a sprite request would
          // rebuild the next building's prompt as if a set had been asked for.
          if (!DesignerDomains::IsSprite(mode)) {
            requestSet = false;
            requestSetSize = 0;
          }
          RefreshRewriteBrief();
          if (game != nullptr) {
            session->SetSystemPrompt(kDefinitionConversation,
                                     game->BuildDesignerPrompt(mode, requestSet));
          }
          // A domain change invalidates the prompt, not the transcript: the conversation
          // so far stays readable, and the next request is built from the new domain's
          // document.
          routeNote.clear();
          session->LogEvent(std::string("DOMAIN: switched to ") + candidate.id +
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
        "What the next request is built for: a building, a tile or an object.\n"
        "The classifier guesses this from your text when you press Route;\n"
        "this dropdown overrides it, and the choice is what the prompt is built "
        "from.");
  }
}

void DesignerWindow::Draw() {
  bool isOpen = true;
  ImGui::SetNextWindowSize(ImVec2(780, 560), ImGuiCond_FirstUseEver);
  // The window designs buildings, tiles and objects, so the title is the domain-neutral
  // one. The class, the files and the settings keys keep their names (see
  // docs/sprite-designer-design.md, decision 6), so nothing that referenced "Tile
  // Designer" breaks -- only the label a person reads changes.
  ImGui::Begin("Designer", &isOpen, ImGuiWindowFlags_None);
  if (!isOpen) {
    game->designerWindowEntity.remove<ActiveWindow>();
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
    const DesignerDomains::Descriptor &current = ModeDescriptor();
    ImGui::TextDisabled("Designing:");
    ImGui::SameLine();
    if (Settings().designer.autoRoute) {
      // Not a combo: with automatic routing the classifier sets the domain per request,
      // so a dropdown here would be a control the next Send overwrites. It is still shown
      // as text, because "what is this about to build" has to stay visible.
      ImGui::TextUnformatted(current.label.c_str());
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Automatic routing is on (designer.autoRoute): the classifier reads your "
            "request, picks the domain, and the request is sent once it answers.\n"
            "Turn the setting off to choose the domain by hand.");
      }
    } else {
      DrawDomainPicker(current);
    }

    // The sprite needs a filename before the request can be sent, and the mode is the one
    // place it can be asked for. Only drawn in this mode: a name box beside "Building"
    // would be a control with nothing to name.
    if (mode == DesignerDomains::Domain::ActorSprite) {
      ImGui::SameLine();
      ImGui::TextDisabled("as");
      ImGui::SameLine();
      ImGui::SetNextItemWidth(140.0f);
      ImGui::InputTextWithHint("##actorSpriteName", "name (baker)", actorSpriteNameBuffer,
                               sizeof(actorSpriteNameBuffer));
    }

    // Single or set. The router proposes it, but a person can ask for one without a
    // classifier, and can correct the router -- so it is a control, not a hidden decision.
    //
    // Disabled for a building rather than hidden: a blueprint reply is one design, and a
    // checkbox that vanished would read as a bug in the dropdown.
    ImGui::SameLine();
    ImGui::BeginDisabled(!DesignerDomains::IsSprite(mode));
    if (ImGui::Checkbox("Set", &requestSet)) {
      // The prompt is per shape, so a shape change rebuilds it, exactly as a domain change
      // does. The transcript is left alone: what was already designed is still true.
      if (game != nullptr) {
        session->SetSystemPrompt(
            kDefinitionConversation,
            game->BuildDesignerPrompt(mode, requestSet));
      }
      session->LogEvent(std::string("SHAPE: ") + (requestSet ? "set" : "single") +
               " by hand; the next request uses the " +
               (requestSet ? "set" : "single") + " prompt");
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      if (!DesignerDomains::IsSprite(mode)) {
        ImGui::SetTooltip("A building is always one blueprint, so there is no set to ask "
                          "for.");
      } else {
        ImGui::SetTooltip(
            "Ask for a set of sprites that belong together, instead of one.\n"
            "The designer answers with a \"sprites\" array; every entry is validated on "
            "its own, so one bad entry does not discard the rest.\nArt for the set is "
            "requested one batch at a time, at most art.generation.maxCellsPerRequest "
            "sprites per image request.");
      }
    }
    if (requestSet && requestSetSize > 0) {
      ImGui::SameLine();
      ImGui::TextDisabled("(~%d)", requestSetSize);
    }

    if (Settings().designer.autoRoute) {
      ImGui::SameLine();
      ImGui::TextDisabled("| routed automatically on send");
    } else {
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
                ? "Ask a decision model which domain, shape and size your request is, "
                  "then switch the dropdown to match.\nIt reads the text currently in "
                  "the box. Costs a fraction of a cent -- no image is generated.\n"
                  "Set designer.autoRoute to do this on every send."
                : "No System 1 classifier is configured (system1.model.apiKey or "
                  "model.apiKey), so the request cannot be read.\nPick the domain with "
                  "the dropdown instead.");
      }
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
  ImGui::TextDisabled("| %d of %d attempts used",
                      session->Attempts(kDefinitionConversation),
                      session->MaxAttempts(kDefinitionConversation));
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
            ? "One switch for making art automatically.\n"
              "For a tile or object: ask for its sprite as soon as it is accepted, so "
              "naming it is the whole instruction.\n"
              "For a building: after a design is saved, create every asset it asked "
              "for -- definition first, then its sprite -- one at a time, with no "
              "further input.\n"
              "For a rig: draw every body layer that has no art yet, one at a time, as "
              "soon as the body is drafted.\n"
              "Requests are batched where they can be, and the run names what it could "
              "not make.\nPersisted in debug_windows_state."
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

  if (!session->HasBackend()) {
    ImGui::TextWrapped(
        "No designer backend is configured, so nothing can be sent. The tile "
        "designer shares the Designer Chat backend; set \"designer\" in "
        "settings.json (see SETTINGS.md).");
    ImGui::End();
    return;
  }

  if (!layoutLoaded && game != nullptr && game->debugWindowState) {
    showJson = game->debugWindowState->GetDesignerShowJson();
    autoRequestArt = game->debugWindowState->GetDesignerAutoArt();
    paneWidth = game->debugWindowState->GetDesignerPaneWidth();
    blueprintZoom = game->debugWindowState->GetDesignerZoom();
    // A first launch has no stored preference, so it follows the setting that
    // already exists: turning art generation on is one deliberate act, and asking
    // for it to then need a second click per tile is friction with no safety value
    // -- `art.generation.enabled` is itself off by default, which is where the
    // money decision belongs. `maxRequestsPerSession` caps the consequence, and
    // the checkbox below turns it back off.
    if (autoRequestArt == false && !game->debugWindowState->HasDesignerAutoArt()) {
      autoRequestArt = game->TileArtQueueState().Enabled();
    }
    layoutLoaded = true;
  }

  // Left: the design, editable where it is a tile. Right: the conversation, with a
  // draggable splitter between them.
  //
  // Clamped every frame rather than only when dragged, so a pane width read from a
  // hand-edited state file, or left over from a larger window, cannot produce a layout
  // with an ungrabbable pane. The splitter is the one piece of the old Designer window's
  // layout worth keeping now that it is one window: a fixed fraction was always too wide
  // for a sprite and too narrow for a building.
  // What the footer under the transcript needs: the Send row, up to two wrapped
  // status lines, and the input. Reserved from the layout rather than left to the
  // window's height, because the input used to hang below the visible area.
  constexpr float kInputHeight = 64.0f;
  const float footerHeight = ImGui::GetFrameHeightWithSpacing() * 2.0f +
                             kInputHeight +
                             ImGui::GetTextLineHeightWithSpacing() * 2.0f;

  const float available = ImGui::GetContentRegionAvail().x;
  const float maxPaneWidth =
      std::max(kMinPaneWidth, available - kMinTranscriptWidth - 8.0f);
  paneWidth = std::clamp(paneWidth, kMinPaneWidth, maxPaneWidth);
  blueprintZoom =
      std::clamp(blueprintZoom, BlueprintPreview::kMinZoom, BlueprintPreview::kMaxZoom);

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
      // The verdict has just rebuilt the prompt for the domain it read, so this is the
      // first moment the parked request can be sent as the right kind of design.
      if (!autoSendPrompt.empty()) {
        const std::string prompt = std::move(autoSendPrompt);
        autoSendPrompt.clear();
        SendUserMessage(prompt);
      }
    }
  }

  // Every request of this session, not only the edited tile's. The per-tile art section
  // lower down filters on the tile it is showing, so a job started anywhere else -- the
  // Content Editor's Regenerate button, for instance -- used to be invisible in this window
  // even though it was queued and running.
  // Opens itself as soon as there is anything in the queue: a header that starts closed
  // would hide exactly the requests this section exists to show. ImGui remembers a state
  // the user changes afterwards.
  {
    const TileArtQueue &queue = game->TileArtQueueState();
    const bool anyRequests = !queue.Snapshot().empty();
    if (ImGui::CollapsingHeader(
            "Art requests",
            anyRequests ? ImGuiTreeNodeFlags_DefaultOpen : ImGuiTreeNodeFlags_None)) {
      ArtRequestsPanel::Draw(queue,
                             edited != nullptr ? edited->name : std::string());
    }
  }

  // The rig's art requests are one conversation at a time, so they queue; the memo refresh
  // is what makes a layer appear the moment its compiled PNG lands.
  PumpRigArtQueue();
  RefreshRigArtMemo();

  ImGui::BeginChild("designer_preview", ImVec2(paneWidth, -footerHeight), true);
  DrawViewedToolbar();
  // What is *shown* and what is *being designed for* are deliberately two values now.
  // Before the history existed they were one, which is why looking at a building meant
  // the next request would be built as a building.
  const DesignerDomains::Domain shown =
      viewed.empty() && !refusedAttempt.has_value() ? mode : paneDomain;
  if (shown == DesignerDomains::Domain::Blueprint) {
    DrawBlueprintPreview();
  } else if (shown == DesignerDomains::Domain::Object) {
    DrawObjectPreview();
  } else if (shown == DesignerDomains::Domain::Map) {
    DrawMapPreview();
  } else if (shown == DesignerDomains::Domain::Rig) {
    DrawRigPreview();
  } else {
    DrawPreviewPane();
  }
  ImGui::EndChild();

  // The splitter. Built from an invisible button rather than a widget with its own look,
  // so the only thing drawn is the line below -- ImGui has no splitter control, and this
  // is the idiomatic way to make one.
  ImGui::SameLine(0.0f, 0.0f);
  const float paneLeft = ImGui::GetCursorScreenPos().x;
  ImGui::InvisibleButton("designer_splitter", ImVec2(8.0f, -footerHeight));
  const bool splitterHovered = ImGui::IsItemHovered();
  const bool splitterActive = ImGui::IsItemActive();
  if (splitterHovered || splitterActive) {
    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
  }
  if (splitterActive) {
    // Measured from the mouse's absolute position rather than from accumulated drag
    // deltas, so the splitter stays under the cursor and cannot drift when the clamp
    // stops it or the window is resized mid-drag.
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

  ImGui::BeginChild("designer_transcript", ImVec2(0, -footerHeight), true);
  // The transcript itself -- prompts, replies, the thinking tint, the streaming ellipsis,
  // the error line, the speech bar and the separator -- is DesignerSession's. What stays
  // here is the verdict: "Added", a per-entry set list and the art note are sprite facts,
  // and a blueprint reply has none of them.
  // The hint's last sentence depends on how routing is configured: telling someone to
  // press a button that is not drawn is worse than saying nothing at all.
  const std::string routeHint =
      Settings().designer.autoRoute
          ? " Requests are routed automatically: a classifier reads what you type and "
            "picks the domain before the designer is asked."
          : " Press \"Route from text\" to have a classifier read your request before "
            "the designer does.";
  const std::string emptyHint =
      std::string(mode == DesignerDomains::Domain::Blueprint
                      ? "Ask for a building -- \"a two-bedroom house\", \"a blacksmith's "
                        "workshop\", \"a prison cell\". The designer replies with one "
                        "blueprint and its own notes; a valid design is saved into "
                        "data/blueprints/ and appears in the Blueprints window straight "
                        "away."
                      : mode == DesignerDomains::Domain::Object
                            ? "Ask for an object -- \"a wooden crate\", \"a brass "
                              "candelabra\", \"a young apple tree\". The designer replies "
                              "with one object definition and its own notes; an accepted "
                              "object is written to the generated overlay and is available "
                              "to blueprints and spawns on the next load."
                      : mode == DesignerDomains::Domain::Rig
                            ? "Ask for a body -- \"a soot-stained blacksmith\", \"a "
                              "soldier in a patched coat\". The designer replies with a "
                              "brief per body layer; press Draw on the ones you want, and "
                              "the rig composes on the left as the art arrives."
                      : mode == DesignerDomains::Domain::Map
                            ? "Ask for a map -- \"a walled orchard with a well and a "
                              "road to a mill\". The designer replies with a legend and "
                              "rows of terrain plus the objects on it; the draft appears "
                              "on the left, and \"Save to maps/\" writes it."
                            : "Ask for a tile -- \"a stone hearth\", \"mossy "
                              "flagstones\", \"a puddle\". The designer replies with one "
                              "tile definition and its own notes; an accepted tile is "
                              "written to the generated overlay and is immediately "
                              "paintable from a blueprint.") +
      routeHint;
  session->DrawTranscript(
      showThoughts, showJson, emptyHint,
      [](DesignerSession::Exchange &ex) {
        if (!ex.inspected) {
          return;
        }
        // A set is shown entry by entry: the interesting fact about a six-entry reply where
        // one failed is *which* one, and a single "Added" line would hide it.
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
          ImGui::TextWrapped("Added '%s'%s", ex.name.c_str(),
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
      });
  ImGui::EndChild();

  // Inspection runs on the main thread, after the transcript is drawn. Doing it
  // on the worker would mean validating and writing content from a background
  // thread while the ECS is mid-frame.
  session->Pump([this](size_t i) { InspectFinishedExchange(i); });

  if (!statusLine.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text,
                          statusIsError ? ImVec4(1.0f, 0.5f, 0.4f, 1.0f)
                                        : ImVec4(0.5f, 1.0f, 0.6f, 1.0f));
    ImGui::TextWrapped("%s", statusLine.c_str());
    ImGui::PopStyleColor();
  }

  // A parked request waiting for its route counts as busy: the prompt it will be sent with
  // does not exist yet, so a second Send would either race it or be built for the wrong
  // domain.
  const bool busy = session->IsBusy() || !autoSendPrompt.empty();

  // Collect a rewrite that landed since the last frame. Before the buttons, so the
  // result is in the box on the same frame the user sees the state change.
  promptBox.Drain();

  ImGui::BeginDisabled(busy);
  const bool send = ImGui::Button("Send");
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (!autoSendPrompt.empty()) {
    ImGui::TextDisabled("routing this request before sending...");
  } else {
    ImGui::TextDisabled("Enter to send, Shift+Enter for a newline");
  }
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
      ImVec2(-1.0f, kInputHeight),
      ImGuiInputTextFlags_EnterReturnsTrue |
          ImGuiInputTextFlags_CtrlEnterForNewLine |
          // Wrapped rather than scrolled sideways: a long request is normal, and a
          // field that hides its own beginning is the reason the window used to be
          // widened to read it.
          ImGuiInputTextFlags_WordWrap);

  if ((send || submitted) && !busy) {
    std::string prompt(inputBuffer);
    while (!prompt.empty() && (prompt.back() == '\n' || prompt.back() == ' ')) {
      prompt.pop_back();
    }
    if (!prompt.empty()) {
      if (Settings().designer.autoRoute) {
        // Only clear the box once the prompt has been taken: a route already in flight
        // makes BeginAutoRoute decline, and clearing first would eat what was typed.
        if (BeginAutoRoute(prompt)) {
          inputBuffer[0] = '\0';
          promptBox.Reset();
        }
      } else {
        inputBuffer[0] = '\0';
        // Forget the pre-rewrite text: the next message must not offer to undo back
        // into the one just sent.
        promptBox.Reset();
        SendUserMessage(prompt);
      }
    }
  }

  if (layoutLoaded && game != nullptr && game->debugWindowState) {
    game->debugWindowState->SetDesignerShowJson(showJson);
    game->debugWindowState->SetDesignerAutoArt(autoRequestArt);
    game->debugWindowState->SetDesignerPaneWidth(paneWidth);
    game->debugWindowState->SetDesignerZoom(blueprintZoom);
  }

  ImGui::End();
}
