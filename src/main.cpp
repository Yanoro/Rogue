#include "AI.h"
#include "BlueprintRegistry.h"
#include "DebugLog.h"
#include "Defaults.h"
#include "Game.h"
#include "GapLog.h"
#include "JevClassifier.h"
#include "ObjectDesigner.hpp"
#include "SpriteRouter.hpp"
#include "ObjectFactory.h"
#include "RaylibException.hpp"
#include "RewriteBriefs.hpp"
#include "Settings.h"
#include "SpritePaths.hpp"
#include "SystemOneRecorder.h"
#include "TileArt.hpp"
#include "TileDesigner.hpp"
#include "TileRegistry.h"
#include "Window.hpp"
#include "imgui.h"
#include "raylib.h"
#include "rlImGui.h"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>

namespace {

// The headless half of --gaps: build the art gap report and exit, without ever
// creating a window.
//
// Worth its own entry point rather than only a button in a debug window, for two
// reasons. It makes the report *scriptable* -- the generator drains a worklist, so
// something has to be able to produce one without a display. And it makes the
// report testable, which a window-only feature is not: this repo has no test
// framework and cannot run the game headless, so anything reachable only through
// ImGui is unreachable to verification.
//
// It mirrors the registry load order in Game::ECSInit (GameECS.cpp), minus
// everything that needs a game. RegisterDefaultComponents is skipped on purpose:
// only the template *names* are needed here, not the ability to spawn one.
int RunGapReport() {
  const GameSettings &settings = Settings();
  DebugLog log(settings.debug.logMaxEntries);

  TileRegistry tiles;
  tiles.SetDebugLog(&log);
  tiles.LoadTiles(settings.content.tiles);

  ObjectFactory objects;
  objects.SetDebugLog(&log);
  objects.LoadTemplates(settings.content.objects);

  BlueprintRegistry blueprints;
  blueprints.SetDebugLog(&log);
  blueprints.SetTileRegistry(&tiles);
  blueprints.SetObjectFactory(&objects);
  blueprints.LoadBlueprints(settings.content.blueprints);

  const GapLog::Report report = GapLog::Build(&tiles, &objects, &blueprints);
  const std::string path = GapLog::DefaultPath();
  if (!GapLog::Write(report, path, &log)) {
    return 1;
  }

  for (const GapLog::CoverageGap &gap : report.coverage) {
    std::cout << "  no art       " << gap.kind << "  " << gap.name
              << "  (looked for " << gap.conventionId << ")\n";
  }
  for (const GapLog::VocabularyGap &gap : report.vocabulary) {
    std::cout << "  undefined    " << gap.kind << "  " << gap.name
              << "  (blueprint " << gap.blueprint << ")\n";
  }
  std::cout << GapLog::Summary(report) << "\n";
  std::cout << "wrote " << path << std::endl;
  return 0;
}

// Live self-test for the System 1 provider: one real request to the decisions
// endpoint, printed, with the recorder's own copy checked afterwards.
//
// Why this exists, in the same words as the other headless modes above: this repo
// has no test framework and cannot run the game headless, so anything reachable
// only through ImGui is unreachable to verification. The System 1 Overview window
// is exactly that -- and the half of the provider worth verifying is not the
// window, it is whether the request it builds is accepted and whether the reply it
// gets is parsed back onto the right questions. A window cannot show that, because
// a provider that posts the wrong schema produces no records at all, which reads
// the same as a provider nobody called.
//
// The questions are chosen so a wrong answer is distinguishable from a wrong
// parse: both are about a state that the test wrote, so the correct answer is
// known in advance.
//
// It spends money (one small request) and needs a key, so it is never automatic.
int RunSystemOneTest() {
  const GameSettings &settings = Settings();

  // The same resolution the sprite router uses, deliberately shared: the endpoint rule in
  // particular is subtle enough that two copies would mean one of them getting it wrong.
  const SystemOne::JevClassifier::Config config = SpriteRouter::ConfigFromSettings();
  (void)settings;

  SystemOne::JevClassifier classifier(config);
  if (!classifier.Available()) {
    std::cerr << "system1: no API key in system1.model.apiKey or model.apiKey, so "
                 "there is nothing to test\n";
    return 1;
  }

  SystemOne::SystemOneRecorder recorder;
  classifier.SetRecorder(&recorder);

  // The state is written so the correct answers are known: it names an apple and
  // no number, so `fruit` must be `apple` and `has_seven` must be a low
  // probability. That makes a protocol mistake visible as a wrong answer rather
  // than as a plausible-looking reply.
  const std::string state =
      "A basket on the table holds a single red apple. There are no numbers "
      "written anywhere in this scene.";

  std::vector<SystemOne::Option> questions;

  SystemOne::Option noul;
  noul.id = "has_seven";
  noul.type = SystemOne::OptionType::Score;
  noul.instructions = "The state mentions the number seven.";
  questions.push_back(noul);

  SystemOne::Option choice;
  choice.id = "fruit";
  choice.type = SystemOne::OptionType::Choice;
  choice.instructions = "Which fruit does the state describe?";
  choice.options = {{"apple", "A red apple."},
                    {"banana", "A yellow banana."},
                    {"none", "No fruit is described."}};
  questions.push_back(choice);

  SystemOne::Option score;
  score.id = "table_emptiness";
  score.type = SystemOne::OptionType::Value;
  score.instructions = "How empty is the table?";
  score.levels = {"Completely empty.", "Holds one object.",
                  "Crowded with objects."};
  questions.push_back(score);

  std::mutex mutex;
  std::condition_variable done;
  bool answered = false;
  std::vector<SystemOne::Answer> answers;
  SystemOne::CallTrace trace;

  std::cout << "endpoint: " << config.endpoint << "\n";
  std::cout << "model:    " << config.model << "\n";
  std::cout << "asking 3 question(s) (noul, choice, score)\n";

  classifier.Ask(
      state, questions,
      [&](const std::vector<SystemOne::Answer> &result,
          const SystemOne::CallTrace &resultTrace) {
        std::lock_guard<std::mutex> lock(mutex);
        answers = result;
        trace = resultTrace;
        answered = true;
        done.notify_all();
      },
      {}, {"system1_selftest", "fixed probe: apple in a basket", /*harness=*/true});

  {
    std::unique_lock<std::mutex> lock(mutex);
    // Generous: this covers a cold connection and the endpoint's own latency, and
    // the provider has its own timeout underneath.
    if (!done.wait_for(lock, std::chrono::seconds(60), [&] { return answered; })) {
      std::cerr << "system1: no answer within 60s\n";
      return 1;
    }
  }

  std::cout << "\n---------- request ----------\n" << trace.requestPayload << "\n";
  if (!trace.responseText.empty()) {
    std::cout << "---------- response ----------\n" << trace.responseText << "\n";
  }
  std::cout << "latency: " << (trace.latencyMs < 0
                                   ? std::string("(not measured)")
                                   : std::to_string(trace.latencyMs) + "ms")
            << "   tokens: prompt " << trace.promptTokens << ", output "
            << trace.outputTokens << "\n";

  if (!trace.Ok()) {
    std::cerr << "system1: FAILED -- " << trace.error << "\n";
    // Recorded anyway, and the record is what the window would show, so a failure
    // is reportable rather than silent.
    std::cout << "recorded: " << recorder.Count() << " record(s)\n";
    return 1;
  }

  std::cout << "\n---------- answers ----------\n";
  for (const SystemOne::Answer &answer : answers) {
    std::cout << "  " << answer.id << ": choice='" << answer.choice
              << "' probability=" << answer.probability
              << " score=" << answer.score
              << " confidence=" << answer.confidence;
    if (!answer.probabilities.empty()) {
      std::cout << " distribution={";
      bool first = true;
      for (const auto &entry : answer.probabilities) {
        if (!first) std::cout << ", ";
        first = false;
        std::cout << entry.first << "=" << entry.second;
      }
      std::cout << "}";
    }
    std::cout << "\n";
  }

  // The recorder is checked rather than assumed: the overview window is only as
  // good as this, and a provider that answers correctly but records nothing would
  // look perfectly healthy from here.
  const std::vector<SystemOne::SystemOneRecord> records = recorder.Snapshot();
  std::cout << "\nrecorded: " << records.size() << " record(s) for scenario '"
            << (records.empty() ? "" : records.front().scenario) << "'\n";
  // Expected to be empty, and that is the check: a harness call is kept in memory
  // for the window but must never reach the file the game's own traffic is judged
  // from, or every count in that file would include a hand-written probe.
  std::cout << "log file: "
            << (recorder.LogFilePath().empty() ? "(none -- correct for a harness call)"
                                               : recorder.LogFilePath())
            << "\n";

  bool ok = true;
  if (records.size() != 1) {
    std::cout << "FAILED: expected exactly one record\n";
    ok = false;
  }
  if (!recorder.LogFilePath().empty()) {
    std::cout << "FAILED: a harness call wrote to the log file\n";
    ok = false;
  }
  if (!records.empty() && !records.front().harness) {
    std::cout << "FAILED: the record is not marked as a harness call\n";
    ok = false;
  }
  // The two answers whose correct value is known. A parse that put the choice onto
  // the wrong question, or that read `noul` as `score`, fails here.
  for (const SystemOne::Answer &answer : answers) {
    if (answer.id == "fruit" && answer.choice != "apple") {
      std::cout << "FAILED: fruit was '" << answer.choice
                << "', expected 'apple'\n";
      ok = false;
    }
    if (answer.id == "has_seven" && answer.probability > 0.5f) {
      std::cout << "FAILED: has_seven was " << answer.probability
                << ", expected a low probability\n";
      ok = false;
    }
  }

  std::cout << (ok ? "system1: ok\n" : "system1: FAILED\n");
  return ok ? 0 : 1;
}

}  // namespace

// The headless half of the tile designer: run the creation path for one reply
// and exit. No window, no backend.
//
// Why it exists, in the same words as RunGapReport above: this repo has no test
// framework and cannot run the game headless, so anything reachable only through
// ImGui is unreachable to verification. The tile designer's window is the least
// testable part of the feature and its validator is the part that matters, so
// the validator gets an entry point.
//
// `replyPath` may hold any of the three things a person actually has on disk:
// a bare tile object, the ```json block, or a raw model reply with prose and
// thinking around it. TileDesigner::ApplyReply is the same function the window
// calls, so this cannot pass while the window fails -- and the catalog and overlay
// are loaded in the same order Game::ECSInit loads them, so a name collision
// against either one is reported here exactly as it would be in the game.
int RunTileDesign(const std::string &replyPath) {
  const GameSettings &settings = Settings();
  DebugLog log(settings.debug.logMaxEntries);

  TileRegistry tiles;
  tiles.SetDebugLog(&log);
  tiles.LoadTiles(settings.content.tiles);

  const size_t generated = TileDesigner::LoadOverlayDirectory(
      settings.content.generatedTiles, tiles, &log);

  std::ifstream file(replyPath);
  if (!file.is_open()) {
    std::cerr << "tile design: could not read " << replyPath << std::endl;
    return 1;
  }
  std::stringstream buffer;
  buffer << file.rdbuf();
  const size_t bytes = buffer.str().size();

  std::cout << "catalog: " << tiles.Count() << " tile(s) (" << generated
            << " from the generated overlay)\n";
  std::cout << "reply:   " << replyPath << " (" << bytes << " bytes)\n";

  // The writing directory is deliberately empty here. This mode is for judging a
  // reply, and a validator that silently wrote to the live overlay would make it
  // impossible to test a refusal without editing the world. The window is the
  // thing that persists what it accepts.
  const TileDesigner::ApplyResult result =
      TileDesigner::ApplyFile(replyPath, tiles, /*overlayDirectory=*/"");

  if (!result.ok) {
    std::cout << "REFUSED: " << result.error << "\n";
    return 1;
  }

  std::cout << "ACCEPTED: " << result.name << "\n";
  if (result.definition != nullptr) {
    const TileDef &def = *result.definition;
    std::cout << "  " << TileDesigner::WriteTileDef(def).dump(2) << "\n";
  }
  if (!result.notes.empty()) {
    std::cout << "  designerNotes: " << result.notes << "\n";
  }
  for (const std::string &warning : result.warnings) {
    std::cout << "  warning: " << warning << "\n";
  }
  // The promise Map::CreateTile depends on, checked here rather than assumed: a
  // definition that was accepted but does not resolve would leave a blueprint
  // painting an invisible wall.
  if (tiles.Get(result.name) == nullptr) {
    std::cout << "  INTERNAL: the tile was accepted but does not resolve from "
                 "the catalog\n";
    return 2;
  }
  return 0;
}

// The kind-dispatched half of the above. A tile goes through the tile validator; an
// object's validator is phase 2 of docs/sprite-designer-design.md, so this says so
// plainly rather than pretending an object was checked. The prompt is built for every
// kind regardless, which is what makes the *document* verifiable before the validator
// exists.
int RunSpriteDesign(const SpriteKinds::Descriptor &kind,
                    const std::string &replyPath,
                    const std::string &overlayDirectory) {
  if (kind.kind == SpriteKinds::Kind::Tile) {
    return RunTileDesign(replyPath);
  }

  const GameSettings &settings = Settings();
  DebugLog log(settings.debug.logMaxEntries);

  // The tile catalog is loaded only so a cross-kind name collision can be reported.
  // It is not searched for object names: the two namespaces are separate by design.
  TileRegistry tiles;
  tiles.SetDebugLog(&log);
  tiles.LoadTiles(settings.content.tiles);

  // The factory the validator uses. It owns the component setters, so this is the same
  // code that will load an object at startup -- not a second parser. The catalog is
  // loaded before the overlay, in the same order Game::ECSInit loads them, so a key
  // collision against hand-authored content is refused here exactly as it would be in
  // the game.
  ObjectFactory objects;
  objects.SetDebugLog(&log);
  objects.LoadTemplates(settings.content.objects);
  const size_t generated = ObjectDesigner::LoadOverlayDirectory(
      settings.content.generatedObjects, objects, &log);

  std::ifstream file(replyPath);
  if (!file.is_open()) {
    std::cerr << "sprite design: could not read " << replyPath << "\n";
    return 1;
  }
  std::stringstream buffer;
  buffer << file.rdbuf();

  std::cout << "kind:       " << kind.id << " (" << kind.label << ")\n";
  std::cout << "catalog:    " << objects.GetTemplates().size() << " object(s) ("
            << generated << " from the generated overlay)\n";
  std::cout << "reply:      " << replyPath << " (" << buffer.str().size()
            << " bytes)\n";

  // The writing directory is deliberately the empty one by default, for the same reason
  // the tile mode's is: a validator that silently wrote to the live overlay would make it
  // impossible to test a refusal without editing the world. `--sprite-design-overlay
  // <dir>` points the write somewhere disposable instead, so the *round trip* -- write,
  // reload, resolve -- is testable without touching content a run depends on.
  // Read once, because a set has to be applied entry by entry and the file is needed for
  // that. `--sprite-design` therefore validates a set envelope when the file holds one,
  // without a separate mode: the file *is* the reply, single or set.
  std::ifstream replyFile(replyPath);
  if (!replyFile.is_open()) {
    std::cerr << "sprite design: could not read " << replyPath << "\n";
    return 1;
  }
  std::stringstream replyBuffer;
  replyBuffer << replyFile.rdbuf();
  const std::string replyText = replyBuffer.str();

  const ObjectDesigner::SetApplyResult set =
      ObjectDesigner::ApplySetReply(replyText, objects, overlayDirectory, &tiles);
  if (!set.ok) {
    std::cout << "REFUSED: " << set.error << "\n";
    return 1;
  }
  if (set.sprites.size() > 1) {
    std::cout << "set:        " << (set.setName.empty() ? "(unnamed)" : set.setName)
              << " -- " << set.sprites.size() << " definition(s), " << set.accepted
              << " accepted, " << set.refused << " refused\n";
    for (size_t index = 0; index < set.sprites.size(); ++index) {
      const ObjectDesigner::ApplyResult &entry = set.sprites[index];
      if (entry.ok) {
        std::cout << "  [" << index << "] ACCEPTED " << entry.name << "  (key "
                  << entry.key << ")\n";
      } else {
        std::cout << "  [" << index << "] REFUSED  " << entry.error << "\n";
      }
    }
    for (size_t index = 0; index < set.sprites.size(); ++index) {
      const ObjectDesigner::ApplyResult &entry = set.sprites[index];
      if (!entry.ok) continue;
      for (const std::string &warning : entry.warnings) {
        std::cout << "  [" << index << "] warning: " << warning << "\n";
      }
    }
    // A partial set is the designed outcome, not a failure: the accepted entries are in the
    // catalog and the refused one is named for the retry. Exit non-zero only when nothing
    // was accepted at all, so a caller can tell "some of it worked" from "none of it did".
    return set.accepted > 0 ? 0 : 1;
  }

  const ObjectDesigner::ApplyResult result =
      set.sprites.empty() ? ObjectDesigner::ApplyResult{} : set.sprites.front();

  if (!result.ok) {
    std::cout << "REFUSED: " << result.error << "\n";
    return 1;
  }

  std::cout << (result.revised ? "REVISED:  " : "ACCEPTED: ") << result.name
            << "  (key " << result.key << ")\n";
  std::cout << "  " << ObjectDesigner::WriteObjectDef(result.definition).dump(2)
            << "\n";
  if (!result.notes.empty()) {
    std::cout << "  designerNotes: " << result.notes << "\n";
  }
  for (const std::string &warning : result.warnings) {
    std::cout << "  warning: " << warning << "\n";
  }

  // The promise SpawnObject depends on, checked here rather than assumed: a definition
  // that was accepted but does not resolve would leave a blueprint placing nothing.
  if (objects.GetTemplates().find(result.key) == objects.GetTemplates().end()) {
    std::cout << "  INTERNAL: the object was accepted but does not resolve from the "
                 "factory\n";
    return 2;
  }
  // The sprite convention the accepted key implies. Asserted because the key doubles as
  // the sprite id, so a key the caller did not expect means art that never resolves.
  std::cout << "  sprite:   " << result.key << ".png (convention id)\n";
  return 0;
}

// Prints the system prompt for a kind and exits. The counterpart of RunTileDesign for
// the half that exists before any validator: the prompt assembly, the contract
// substitution and the vocabulary list are all inspectable with no world and no
// window, which is the same reason --gaps exists.
int RunSpritePrompt(const SpriteKinds::Descriptor &kind,
                    const std::string &shapeName) {
  const GameSettings &settings = Settings();
  DebugLog log(settings.debug.logMaxEntries);

  TileDesigner::PromptVocabulary vocabulary;
  std::string promptFile;

  if (kind.kind == SpriteKinds::Kind::Tile) {
    // The tile vocabulary needs the catalog, loaded exactly as Game::ECSInit loads it
    // so the list the model is shown is the list the world has.
    TileRegistry tiles;
    tiles.SetDebugLog(&log);
    tiles.LoadTiles(settings.content.tiles);
    const size_t generated = TileDesigner::LoadOverlayDirectory(
        settings.content.generatedTiles, tiles, &log);

    vocabulary.body = TileDesigner::TileVocabulary(tiles);
    const std::vector<std::string> legend = TileDesigner::GlyphLegend(tiles);
    for (const std::string &line : legend) {
      vocabulary.glyphs += line + "\n";
    }
    promptFile = settings.content.tileDesignerPromptFile;

    std::cout << "catalog:    " << tiles.Count() << " tile(s) (" << generated
              << " from the generated overlay)\n";
  } else {
    // The component vocabulary, generated from the factory's registered setters -- the
    // same source the window uses. Reading it here rather than letting the document
    // carry a hand-written list is what keeps the prompt, the validator and the engine
    // agreeing about what a designer may write.
    ObjectFactory objects;
    objects.SetDebugLog(&log);
    vocabulary.body = ObjectDesigner::ComponentVocabulary(objects);
    promptFile = settings.content.objectDesignerPromptFile;

    std::cout << "components: " << objects.ComponentNames().size() << " registered\n";
  }

  // Refused by name rather than defaulted, so a typo does not silently dump the wrong
  // prompt for the shape being investigated.
  if (!shapeName.empty() && shapeName != "one" && shapeName != "set") {
    std::cerr << "sprite prompt: --sprite-prompt-shape must be 'one' or 'set', not '"
              << shapeName << "'\n";
    return 1;
  }
  const TileDesigner::PromptShape shape =
      shapeName == "set" ? TileDesigner::PromptShape::Set
                         : TileDesigner::PromptShape::One;
  const std::string prompt = TileDesigner::BuildPromptForKind(
      kind, promptFile, settings.content.spriteContractFile, vocabulary, shape);

  std::cout << "kind:       " << kind.id << " (" << kind.label << ")\n";
  std::cout << "shape:      " << (shape == TileDesigner::PromptShape::Set ? "set"
                                                                        : "one")
            << "\n";
  std::cout << "prompt:     " << promptFile << "\n";
  std::cout << "contract:   " << settings.content.spriteContractFile << "\n";
  std::cout << "vocabulary: " << vocabulary.body.size() << " byte(s), "
            << vocabulary.glyphs.size() << " byte(s) of glyph legend\n";
  std::cout << "result:     " << prompt.size() << " byte(s)\n";

  // The substitutions that must not silently fail. A prompt still containing a
  // placeholder has shipped a literal token to the model, which is the failure mode of
  // every template system and is invisible from "it built".
  //
  // The scan strips backticked spans first, because a document is allowed to *mention*
  // a token in prose (`... where it says %CONTRACT% ...`) and a mention is not a
  // substitution failure. Without that, a document that explains its own assembly
  // would report itself as broken. Matched by position so a stray backtick in the
  // document's own text cannot shift the scan out of alignment.
  std::string scan;
  scan.reserve(prompt.size());
  bool inSpan = false;
  for (const char c : prompt) {
    if (c == '`') {
      inSpan = !inSpan;
      continue;
    }
    if (!inSpan) scan += c;
  }

  int unsubstituted = 0;
  for (const char *placeholder : {"%CONTRACT%", "%GLYPHS%", "%VOCAB%", "%TILES%"}) {
    if (scan.find(placeholder) == std::string::npos) continue;
    // %GLYPHS% legitimately survives for a kind with no glyphs, because nothing is
    // appended for it; the others must never.
    if (std::string(placeholder) == "%GLYPHS%" && vocabulary.glyphs.empty()) continue;
    std::cout << "  UNSUBSTITUTED: " << placeholder << "\n";
    ++unsubstituted;
  }
  std::cout << "----\n" << prompt;
  if (prompt.empty() || prompt.back() != '\n') std::cout << "\n";
  return unsubstituted == 0 ? 0 : 1;
}

// The headless half of the art wiring: report and check the path a tile's
// generated sprite takes, without a window.
//
// It runs the tail of the chain in docs/tile-art-wiring-design.md that does not
// need a graphics context -- naming, where the file goes, whether the art pack
// already wins, and the palette contract -- and asserts the two things §7 says to
// assert. What it cannot do is the truly visual half (invalidate the cache, attach
// `Sprite`, draw it), because that needs a loaded world and a GPU; the game does
// that through Game::RefreshTileVisuals, which is one call.
//
// With `sourcePng` empty it only reports: "where would this go, and is the art pack
// already covering it". With a path it places the file and checks the result --
// the mode for proving a generator's output is pipeline-correct.
int RunTileArt(const std::string &tileName, const std::string &sourcePng,
               const std::string &palettePath) {
  const GameSettings &settings = Settings();
  DebugLog log(settings.debug.logMaxEntries);

  TileRegistry tiles;
  tiles.SetDebugLog(&log);
  tiles.LoadTiles(settings.content.tiles);
  const size_t generated = TileDesigner::LoadOverlayDirectory(
      settings.content.generatedTiles, tiles, &log);

  const TileArt::Target target = TileArt::PlanForTile(tileName);

  std::cout << "tile:       " << tileName << "\n";
  std::cout << "catalog:    " << tiles.Count() << " tile(s) (" << generated
            << " from the generated overlay)\n";
  std::cout << "convention: " << target.conventionId << "  (tile_<slug>.png)\n";

  // The second contract of §7, reported rather than trusted: which layer the
  // sprite lands in, and which layer actually wins.
  const SpriteRootsInfo roots = CurrentSpriteRoots();
  std::cout << "art pack:   "
            << (roots.artPackSprites.empty() ? "(none configured)"
                                             : roots.artPackSprites)
            << "\n";
  std::cout << "generated:  "
            << (roots.generatedSprites.empty() ? "(none configured)"
                                               : roots.generatedSprites)
            << "\n";
  std::cout << "writes to:  "
            << (target.generatedPath.empty() ? "(nowhere -- no generated layer)"
                                             : target.generatedPath)
            << "\n";
  std::cout << "resolves:   "
            << (target.resolvedPath.empty() ? "(nothing -- the glyph is the look)"
                                            : target.resolvedPath)
            << "\n";
  if (target.packWins) {
    std::cout << "NOTE:       the art pack already ships " << target.conventionId
              << ".png, so generated art for this tile would be written and then "
                 "IGNORED -- the pack always wins (design §1.2).\n";
  }
  if (tiles.Get(tileName) == nullptr) {
    std::cout << "NOTE:       '" << tileName
              << "' is not in the catalog, so no cell can draw it yet.\n";
  }

  if (sourcePng.empty()) {
    std::cout << "\n(report only: pass a source PNG to place and verify it)\n";
    return target.generatedPath.empty() ? 1 : 0;
  }

  // No world is loaded here, so there is no swap callback: this proves the file
  // half of the chain, which is the half a generator can get wrong. The Sprite half
  // is Game::RefreshTileVisuals, exercised in the game.
  const TileArt::SwapResult result = TileArt::SwapIn(
      tileName, sourcePng, palettePath,
      /*swap=*/[](const std::string &) { return size_t{0}; });

  if (!result.copy.ok) {
    std::cout << "FAILED:     " << result.copy.error << "\n";
    return 1;
  }
  std::cout << "\nplaced:     " << result.copy.path
            << (result.copy.replaced
                    ? "  (replaced an existing generated sprite)"
                    : result.copy.alreadyInPlace ? "  (already in place)"
                                                 : "  (new)")
            << "\n";

  // The first contract of §7. Not claimed as passing when it could not run.
  if (!result.palette.ran) {
    std::cout << "palette:    NOT CHECKED -- " << result.palette.error << "\n";
  } else {
    std::cout << "palette:    " << result.palette.output;
    if (!result.palette.ok) {
      std::cout << "FAILED:     " << result.palette.error << "\n";
      return 1;
    }
  }

  // Contract two, enforced rather than reported: PlaceSprite refuses to write into
  // the art pack, so reaching here means the file is in the generated layer. This
  // compares the path actually written against the pack root, so a future change to
  // the roots cannot quietly make the write land in the pack.
  std::error_code ec;
  const std::filesystem::path placed =
      std::filesystem::weakly_canonical(result.copy.path, ec);
  if (!roots.artPackSprites.empty()) {
    const std::filesystem::path pack =
        std::filesystem::weakly_canonical(roots.artPackSprites, ec);
    if (!placed.empty() && !pack.empty() &&
        placed.string().rfind(pack.string(), 0) == 0) {
      std::cout << "FAILED:     the sprite landed inside the art pack ("
                << pack.string()
                << "); generated art goes to the generated layer\n";
      return 1;
    }
  }
  std::cout << "layers:     ok (generated layer, not the art pack)\n";
  return 0;
}

// Proves the hot-swap itself, which no other headless mode can: it needs a loaded
// world, and a world needs a window.
//
// The sequence is the one that matters and cannot be checked statically:
//
//   1. settle the world (so every tile's sprite has been resolved and any miss is
//      now memoised as "this path does not exist");
//   2. place the sprite for one tile -- the file appearing *after* the world was
//      built, which is the live-generation case exactly;
//   3. call Game::RefreshTileVisuals and report how many cells changed.
//
// Without this, step 1 of §8 would be three claims and a promise. With it, check 1
// (the memo) and check 2 (the attach) are observed rather than asserted, and the
// count is the evidence: a non-zero count means entities that had no `Sprite` grew
// one after the file appeared, which is the whole hot-swap.
//
// Exits after reporting, so it is usable in a script. The window is created but
// never drawn into meaningfully -- it exists because the ECS is built on Init.
int RunTileArtSelfTest(Game &game, const std::string &tileName,
                       const std::string &sourcePng, const std::string &palettePath,
                       int settleFrames) {
  const TileArt::Target target = TileArt::PlanForTile(tileName);
  std::cout << "self-test:  hot-swap for '" << tileName << "' ("
            << target.conventionId << ")\n";

  if (target.generatedPath.empty()) {
    std::cerr << "self-test: no generated layer configured, so there is nowhere "
                 "to hot-swap from\n";
    return 1;
  }

  // Step 1: let the world resolve everything once, so the absence of this sprite
  // is in the cache. This is what makes the test about the *swap* rather than about
  // a first-time load.
  for (int i = 0; i < settleFrames; ++i) {
    game.Update();
  }

  std::string before;
  const bool resolvedBefore = SpriteResolves("", target.conventionId, &before);
  const size_t cellsBefore = game.RefreshTileVisuals(tileName);
  std::cout << "before:     resolves=" << (resolvedBefore ? before : "(nothing)")
            << "  cells-with-a-sprite=" << cellsBefore << "\n";

  // Step 2: the file appears, while the world is running.
  const TileArt::SwapResult result = TileArt::SwapIn(
      tileName, sourcePng, palettePath,
      [&game, &tileName](const std::string &) {
        // Step 3: the swap runs on the main thread, here, between frames.
        return game.RefreshTileVisuals(tileName);
      });
  if (!result.copy.ok) {
    std::cerr << "self-test: placement failed: " << result.copy.error << "\n";
    return 1;
  }

  std::string after;
  const bool resolvedAfter = SpriteResolves("", target.conventionId, &after);
  std::cout << "placed:     " << result.copy.path << "\n";
  std::cout << "after:      resolves=" << (resolvedAfter ? after : "(nothing)")
            << "  cells-refreshed=" << result.refreshedCells << "\n";
  if (!result.palette.ran) {
    std::cout << "palette:    NOT CHECKED -- " << result.palette.error << "\n";
  } else {
    std::cout << "palette:    " << result.palette.output;
  }

  // The contract this mode exists to check: the file was absent when the world was
  // built, and cells drawing that tile now carry a sprite. A zero here means check 2
  // is not wired.
  if (result.refreshedCells == 0) {
    std::cout << "FAILED:     no cell changed, so the world did not take the new "
                 "sprite (is '" << tileName << "' on the loaded map?)\n";
    return 1;
  }
  std::cout << "hot-swap:   ok (" << result.refreshedCells
            << " cell(s) gained a sprite after the file appeared)\n";
  return result.palette.ok ? 0 : 1;
}

// Draws `frames` real frames, pumping the art queue exactly where the ECS system
// does. Shared by the two headless tests that need the world running rather than
// just initialized.
//
// It exists because a test that exits after Init never pumps the queue at all: the
// worker is started by the pump, so an automatic request would sit `Queued` forever
// and a test that only checked "the request was accepted" would report success
// having generated nothing. That is exactly what happened the first time this was
// wired, which is why the loop is a shared function rather than duplicated.
void DrawTileArtFrames(Game &game, int frames) {
  raylib::Window *window = game.getWindow();
  for (int frame = 0; frame < frames; ++frame) {
    game.Update();
    game.PumpTileArtQueue(1.0f / 60.0f);
    window->BeginDrawing();
    window->ClearBackground(BLACK);
    game.Draw();
    rlImGuiBegin();
    game.DrawGameWindows();
    rlImGuiEnd();
    window->EndDrawing();
  }
}

// Shows what the Improve button would ask, for one brief and one piece of text.
//
// Free by construction: it assembles the request and prints it without a backend, so
// the part that decides whether a rewrite stays inside the engine's vocabulary -- the
// brief -- can be read and diffed. That is the half of this feature worth verifying,
// and the half an ImGui-only implementation would have left unverifiable.
int RunRewritePreview(const std::string &briefName, const std::string &text) {
  const GameSettings &settings = Settings();

  // Refused by name rather than defaulted, so a typo does not silently produce a
  // different domain's brief. The three names are the three vocabularies a rewrite can
  // be held inside: see RewriteBriefs.hpp.
  if (briefName != "tile" && briefName != "object" && briefName != "building") {
    std::cerr << "rewrite: --rewrite-brief must be 'tile', 'object' or 'building', not '"
              << briefName << "'\n";
    return 1;
  }

  PromptRewriter rewriter;
  // No backend: a null AI is a supported state for Configure, and all this mode uses
  // is the assembled prompt.
  rewriter.Configure(nullptr, settings.content.rewritePromptFile, "rewrite-preview");
  // Three briefs now, and the default stays Blueprint so an unknown name does not
  // silently become a tile brief. Each is a different vocabulary the rewrite must stay
  // inside -- see RewriteBriefs.hpp.
  if (briefName == "tile") {
    rewriter.SetBrief(RewriteBriefs::Tile());
  } else if (briefName == "object") {
    rewriter.SetBrief(RewriteBriefs::Object());
  } else {
    rewriter.SetBrief(RewriteBriefs::Blueprint());
  }

  const std::string request = rewriter.DescribeRequest(text);
  if (request.size() < 200) {
    // Instructions failed to load, so there is nothing real to show.
    std::cerr << "rewrite: could not read the instructions ("
              << settings.content.rewritePromptFile << ")\n";
    return 1;
  }

  std::cout << "brief:        " << briefName << "\n";
  std::cout << "instructions: " << settings.content.rewritePromptFile << "\n";
  if (briefName == "tile" || briefName == "object") {
    // Named explicitly, because both sprite briefs change with the art pack and that is
    // the one input to this feature that lives on disk.
    const TileArt::PackStyle style = TileArt::CurrentPackStyle();
    std::cout << "art pack:     "
              << (settings.art.pack.empty() ? "(none)" : settings.art.pack) << "\n";
    std::cout << "style:        " << style.Summary() << "\n";
  }
  std::cout << "text:         " << text << "\n";
  std::cout << "\n---------- what would be sent ----------\n";
  std::cout << request << "\n";
  return 0;
}

// Proves the per-request cell cap: a set larger than the cap becomes several requests
// rather than one huge sheet.
//
// The cap trades money for legibility -- one request costs the same whether it fills one cell
// or sixteen -- so "ten sprites cost three requests instead of one" is a *feature* only if the
// requests actually happen, one after another, with every sprite still generated. What this
// checks is that the split neither drops anything nor collapses into one batch: the tiles
// requested all end up with a sprite, and the request count is ceil(n / cap).
int RunTileArtSplitTest(Game &game, int count) {
  const TileArtQueue &queue = game.TileArtQueueState();
  if (!queue.Enabled()) {
    std::cerr << "split test: art generation is off (art.generation.enabled)\n";
    return 1;
  }
  const int cap = std::max(1, queue.GetConfig().maxCellsPerRequest);
  const int expectedRequests = (count + cap - 1) / cap;
  if (expectedRequests > queue.GetConfig().maxRequestsPerSession) {
    std::cerr << "split test: " << count << " tiles at a cap of " << cap
              << " needs " << expectedRequests
              << " requests, which is over maxRequestsPerSession ("
              << queue.GetConfig().maxRequestsPerSession << ")\n";
    return 1;
  }

  std::cout << "split test: " << count << " tile(s), cap " << cap << " per request -> "
            << expectedRequests << " request(s) expected\n";

  std::vector<std::string> names;
  for (int index = 0; index < count; ++index) {
    names.push_back("split test " + std::to_string(index));
    std::string reason;
    if (!game.RequestTileArt(names.back(), &reason)) {
      std::cerr << "split test: request refused: " << reason << "\n";
      return 1;
    }
  }

  // The frame loop, pumping exactly where the ECS system does. Bounded by wall clock for the
  // same reason the queue test is: a frame budget is a time budget in disguise.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
  int frames = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    game.Update();
    game.PumpTileArtQueue(1.0f / 60.0f);
    ++frames;
    size_t done = 0;
    for (const TileArtQueue::Job &job : queue.Snapshot()) {
      if (job.state == TileArtQueue::State::Done) ++done;
      if (job.state == TileArtQueue::State::Failed) {
        std::cerr << "split test: '" << job.tileName << "' failed: " << job.error
                  << "\n";
        return 1;
      }
    }
    if (done == static_cast<size_t>(count)) break;
  }

  size_t done = 0;
  size_t withPath = 0;
  for (const TileArtQueue::Job &job : queue.Snapshot()) {
    if (job.state == TileArtQueue::State::Done) {
      ++done;
      if (!job.path.empty()) ++withPath;
    }
  }
  std::cout << "  settled:  " << done << "/" << count << " after " << frames
            << " frame(s)\n";
  std::cout << "  sprites:  " << withPath << " named a placed file\n";
  std::cout << "  requests: " << queue.RequestsUsed() << " used of "
            << queue.GetConfig().maxRequestsPerSession << " (expected "
            << expectedRequests << ")\n";

  if (done != static_cast<size_t>(count)) {
    std::cout << "FAILED:     not every sprite was generated\n";
    return 1;
  }
  if (queue.RequestsUsed() != static_cast<size_t>(expectedRequests)) {
    std::cout << "FAILED:     the split produced " << queue.RequestsUsed()
              << " request(s), not " << expectedRequests
              << " -- the cap is not being applied\n";
    return 1;
  }
  if (withPath != static_cast<size_t>(count)) {
    std::cout << "FAILED:     a settled job has no sprite path\n";
    return 1;
  }
  std::cout << "split:      ok (a set larger than the cap is split across requests, and "
               "every sprite is still generated)\n";
  return 0;
}

// Drives the live-generation queue for one tile and exits: the frame-safety half of
// §8 step 3, which the file-based self-test above cannot reach.
//
// It runs the *real* queue -- the same object the ECS system pumps -- with a
// generation command from settings, so the whole live path is exercised (request,
// batch window, worker thread, main-thread drain, swap) without an API call when the
// command is a stub. See tools/gen_tile_art.py for the real one.
//
// It asserts, in order: the request is accepted and reports as queued rather than
// doing work inline; the frame loop keeps running while the batch is in flight; and
// the job settles and the swap attaches a sprite to the map's cells.
int RunTileArtQueueTest(Game &game, const std::string &tileName, int holdFrames) {
  // The hold phase exists so the *window* can be exercised with a finished job and
  // real artifacts on disk. A test that exits before the draw loop never renders the
  // pipeline panels, which is exactly the code the panels are for -- so it would be
  // the one path nothing had run.
  std::ostringstream sink;
  std::cout << "queue test: tile '" << tileName << "'\n";

  const TileArtQueue &queue = game.TileArtQueueState();
  if (!queue.Enabled()) {
    std::cerr << "queue test: art.generation.enabled is false, so nothing can be "
                 "requested\n";
    return 1;
  }
  if (queue.GetConfig().command.empty()) {
    std::cerr << "queue test: art.generation.command is empty\n";
    return 1;
  }

  // Before: no sprite, so the request is genuinely about a tile with no art.
  const size_t cellsBefore = game.RefreshTileVisuals(tileName);
  std::cout << "before:     cells-with-a-sprite=" << cellsBefore << "\n";

  std::string reason;
  if (!game.RequestTileArt(tileName, &reason)) {
    std::cerr << "queue test: request refused: " << reason << "\n";
    return 1;
  }
  std::cout << "requested:  accepted (queued, not generated inline)\n";

  // The frame loop, with the pump exactly where the ECS system calls it, so "the
  // game keeps running" is observed rather than asserted.
  //
  // Bounded by wall-clock time, not by a frame count. A frame budget is really a
  // time budget whose length depends on the machine and on what else is running --
  // measured on this box, the same 600 frames took 10s idle and more than 30s while
  // another build shared the CPU, which turned a pass into a spurious "nothing
  // settled". Thirty seconds is generous for a local command and still bounded.
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(90);
  std::string state;
  int frames = 0;
  for (; std::chrono::steady_clock::now() < deadline; ++frames) {
    game.Update();
    game.PumpTileArtQueue(1.0f / 60.0f);

    for (const TileArtQueue::Job &job : queue.Snapshot()) {
      if (job.tileName != tileName) continue;
      switch (job.state) {
        case TileArtQueue::State::Done:
          state = "Done";
          break;
        case TileArtQueue::State::Failed:
          state = "Failed";
          break;
        case TileArtQueue::State::Skipped:
          state = "Skipped";
          break;
        default:
          break;
      }
      if (!state.empty()) {
        std::cout << "settled:    " << state << " after " << frames
                  << " frame(s)";
        if (!job.error.empty()) std::cout << " -- " << job.error;
        std::cout << "\n";
      }
      break;
    }
    if (!state.empty()) break;
  }

  if (state.empty()) {
    std::cerr << "queue test: nothing settled within the time limit (" << frames
              << " frame(s) drawn)\n";
    return 1;
  }

  // What the *swap* achieved, taken from the job rather than re-measured here: a
  // re-measurement would also count a later manual call, hiding a swap that never
  // ran. This is the number the pipeline sections and the log report.
  size_t swapped = 0;
  std::string jobPath;
  bool jobIsTile = true;
  for (const TileArtQueue::Job &job : queue.Snapshot()) {
    if (job.tileName != tileName) continue;
    swapped = job.swappedCells;
    jobPath = job.path;
    jobIsTile = job.kind == SpriteKinds::Kind::Tile;
    break;
  }

  const size_t cellsAfter = game.RefreshTileVisuals(tileName);
  std::cout << "swapped:    " << swapped << " cell(s) by the completion swap\n";
  std::cout << "after:      cells-with-a-sprite=" << cellsAfter << "\n";
  std::cout << "job path:   " << (jobPath.empty() ? "(EMPTY - bug)" : jobPath) << "\n";
  std::cout << "requests:   " << queue.RequestsUsed() << " used of "
            << queue.GetConfig().maxRequestsPerSession << "\n";

  // What the *cells* carry, read off the render entities themselves rather than
  // inferred from the call above. Asking RefreshTileVisuals again and reporting only
  // its return would be circular -- it is the thing that attaches sprites, so it
  // would relink the cells and report success even if the completion swap had never
  // run. The claim that matters is "the tile draws art instead of its ascii", and
  // that is exactly "does the cell's render entity carry a Sprite".
  int cellsWithSprite = 0;
  int cellsWithGlyph = 0;
  game.TileCellSpriteCount(tileName, &cellsWithSprite, &cellsWithGlyph);
  std::cout << "cells:      '" << tileName << "': " << cellsWithSprite
            << " drawing art, " << cellsWithGlyph << " drawing a glyph\n";
  if (jobPath.empty()) {
    std::cout << "FAILED:     the job carries no sprite path, so the pixel panel "
                 "cannot draw and the log names no file\n";
    return 1;
  }
  // Zero swapped cells is a failure only for a *tile*. An object is spawned as an entity
  // rather than painted onto a cell, so a map holds no cells drawing it and the swap has
  // nothing to attach to -- that is the normal outcome for an object, not a broken swap.
  // Reported and asserted separately so the two cannot be confused.
  if (swapped == 0) {
    if (jobIsTile) {
      std::cout << "FAILED:     the completion swap updated no cells\n";
      return 1;
    }
    std::cout << "note:       an object is not painted on the map, so there are no cells "
                 "to swap -- the sprite is in the generated layer for its next spawn\n";
  }
  if (cellsWithGlyph > 0) {
    std::cout << "FAILED:     " << cellsWithGlyph << " cell(s) drawing '" << tileName
              << "' still have no Sprite after the swap\n";
    return 1;
  }

  if (holdFrames > 0) {
    // Draw the window for real, so the pipeline panels run: artifact discovery, the
    // sprite read-back, the palette comparison and the stage thumbnails. The window's
    // own drawing writes nothing (it is a GUI on a hidden display), so the *result*
    // of that work is reported here instead: what the panels resolved from the
    // worklist. Without it "the panels drew" and "the panels were empty" look
    // identical from outside, which is the whole failure mode being tested for.
    // The panels read the worklist's directory by convention (that is the whole
    // point of Job::worklistPath), so this reads the same directory and reports what
    // is there. "The panels drew" and "the panels showed the empty state" look
    // identical from outside a GUI on a hidden display, and that distinction is the
    // failure mode being tested for.
    std::string worklistPath;
    for (const TileArtQueue::Job &job : queue.Snapshot()) {
      if (job.tileName == tileName) {
        worklistPath = job.worklistPath;
        break;
      }
    }
    // Where the sprite should be, asked the same way placement asks it rather than
    // from the job -- the job's `path` field is reserved for what the window shows
    // and is not what the swap used.
    const std::string spritePath = TileArt::PlanForTile(tileName).generatedPath;
    int stageImages = 0;
    bool hasPrompt = false;
    bool spriteReadable = false;
    int spriteWidth = 0;
    int spriteHeight = 0;
    if (!worklistPath.empty()) {
      const std::filesystem::path workDir =
          std::filesystem::path(worklistPath).parent_path();
      std::error_code ec;
      hasPrompt = std::filesystem::exists(workDir / "tile_art_sheet.prompt.txt", ec);
      if (std::filesystem::exists(workDir / "tile_art_sheet.png", ec)) ++stageImages;
      for (int index = 0; index < 64; ++index) {
        for (const char *suffix : {"crop", "trim", "locked"}) {
          if (std::filesystem::exists(
                  workDir / ("cell_" + std::to_string(index) + "_" + suffix + ".png"),
                  ec)) {
            ++stageImages;
          }
        }
      }
      // The sprite the panels read back for the pixel view. Same file the swap
      // attached, so if the window can read this the panels have something to draw.
      // Guarded: raylib::Image's constructor throws on a path it cannot load, and
      // `spritePath` is empty for a job that has not placed anything yet.
      if (!spritePath.empty() && std::filesystem::exists(spritePath, ec)) {
        raylib::Image probe;
        try {
          probe = raylib::Image(spritePath);
        } catch (const raylib::RaylibException &) {
          probe.data = nullptr;
        }
        if (probe.data != nullptr) {
          spriteReadable = true;
          spriteWidth = probe.width;
          spriteHeight = probe.height;
        }
      }
    }
    std::cout << "artifacts:  " << stageImages << " stage image(s), prompt "
              << (hasPrompt ? "present" : "MISSING") << "\n";
    std::cout << "  worklist: " << (worklistPath.empty() ? "(none)" : worklistPath)
              << "\n";
    std::cout << "  sprite:   "
              << (spriteReadable ? std::to_string(spriteWidth) + "x" +
                                       std::to_string(spriteHeight) + " readable"
                                 : std::string("NOT readable"))
              << "\n";
    if (!worklistPath.empty() && stageImages == 0) {
      std::cout << "FAILED:     the worklist directory holds no stage images\n";
      return 1;
    }
    if (!spriteReadable) {
      std::cout << "FAILED:     the placed sprite could not be read back\n";
      return 1;
    }
    DrawTileArtFrames(game, holdFrames);
  }

  if (state != "Done") {
    std::cout << "FAILED:     the job did not complete\n";
    return 1;
  }
  // Same distinction as the swap count above: a tile must be on the map for a swap to be
  // observable, and an object must not be, so "zero cells" means opposite things for the
  // two kinds.
  if (cellsAfter == 0 && jobIsTile) {
    std::cout << "FAILED:     nothing drew the tile, so no swap happened (is '"
              << tileName << "' on the loaded map?)\n";
    return 1;
  }
  if (!jobIsTile) {
    // The object's own path, taken from the job rather than re-derived: the point of the
    // assertion is that the file the queue *placed* is named by the object's key, and
    // re-asking would be checking the derivation against itself.
    std::cout << "queue:      ok (request -> worker -> drain -> sprite placed as "
              << jobPath << " for an object, which has no map cells to swap)\n";
    return 0;
  }
  std::cout << "queue:      ok (request -> worker -> drain -> swap, frames kept "
               "running throughout)\n";
  return 0;
}

// The most recently written tile designer log, or empty when there is none.
//
// Used only by --tile-designer-logtest, to print the path it produced. The window
// names its log after the time the conversation started, which is deliberately not
// knowable before the run -- so the test asks the filesystem afterwards rather than
// predicting the name.
std::string NewestTileDesignerLog() {
  const std::filesystem::path directory("logs/tile-designer");
  std::error_code ec;
  if (!std::filesystem::is_directory(directory, ec)) return {};

  std::string newest;
  std::filesystem::file_time_type newestTime{};
  for (const auto &entry : std::filesystem::directory_iterator(directory, ec)) {
    if (entry.path().extension() != ".txt") continue;
    const auto time = entry.last_write_time(ec);
    if (newest.empty() || time > newestTime) {
      newest = entry.path().string();
      newestTime = time;
    }
  }
  return newest;
}

int main(int argc, char *argv[]) {
  // Usage:
  //   AIRogue                        -> settings.json, and the map it names
  //   AIRogue <map.json>             -> settings.json, map overridden
  //   AIRogue --settings <file.json> -> a different settings file
  //   AIRogue --art-pack <dir>       -> settings.json, art pack overridden
  //   AIRogue --gaps                 -> write the art gap report and exit
  //   AIRogue --system1-profile <id> -> use that System 1 profile for this run
  //                                     (outranks the remembered selection)
  //   AIRogue --systemone-test       -> one live System 1 request, printed, and exit
  //   AIRogue --rewrite "<text>" [--rewrite-brief tile|building]
  //                                  -> print the improved-prompt request and exit
  //   AIRogue --tile-design <reply>  -> validate one tile reply and exit
  //   AIRogue --sprite-design <kind> <reply>
  //                                  -> the same, dispatched by kind. An object's
  //                                     validator is phase 2, so it reports that.
  //   AIRogue --sprite-route "<text>"     -> one live classification: kind, shape and
  //                                          count, printed with the raw answers
  //   AIRogue --sprite-design <kind> <reply> [--sprite-design-overlay <dir>]
  //                                  -> write an accepted definition to <dir> so the
  //                                     round trip can be reloaded and checked
  //   AIRogue --sprite-prompt <kind> [--sprite-prompt-shape one|set]
  //                                  -> print the system prompt for a kind and exit
  //                                     (no world, no window, no cost)
  //   AIRogue --tile-art <name>      -> report where a tile's art would go
  //   AIRogue --tile-art <name> --tile-art-sprite <png>  -> place and verify it
  //   AIRogue --tile-art <name> --tile-art-selftest      -> prove the in-world swap
  //   AIRogue --tile-art <name> --tile-art-queue         -> prove the live queue
  //   AIRogue --tile-art <name> --tile-art-queue --tile-art-hold <n>
  //                                       -> then draw the window for n frames
  //   AIRogue --tile-designer-logtest                    -> write one designer log
  //                                                         and exit (no backend)
  //   AIRogue --sprite-designer-objecttest               -> the same conversation
  //                                                         designing an object
  //   AIRogue --sprite-designer-settest                  -> the same conversation
  //                                                         asking for a set
  //   AIRogue --tile-art-split <n>                       -> n sprites become
  //                                                         ceil(n/cap) requests
  //   AIRogue --object-art-swap <key>                    -> a spawned object picks up a
  //                                                         newly generated sprite
  //   AIRogue --object-check <key>                       -> is an object in the running
  //                                                         game's factory, and from
  //                                                         the overlay or the catalog
  //   AIRogue --draw-probe <key>                         -> place that object on a
  //                                                         mapped tile, read the frame
  //                                                         back, and assert the tile
  //                                                         shows through its
  //                                                         transparent pixels
  //   AIRogue --art-cache-test                           -> prove a rewritten stage
  //                                                         reloads, not the cache
  //   AIRogue --paint-memo-test <tile>                   -> prove art arriving after
  //                                                         a paint is noticed
  // The map argument keeps its old meaning so existing run scripts still work;
  // the -- flags are the only new spellings.
  std::string settingsPath = "settings.json";
  std::string mapOverride;
  std::string artPackOverride;
  std::string tileDesignPath;
  std::string spriteDesignKind;
  std::string spriteDesignPath;
  std::string spritePromptKind;
  std::string spritePromptShape;
  std::string spriteRouteText;
  std::string spriteDesignOverlay;
  std::string tileArtName;
  std::string tileArtSource;
  std::string tileArtPalette;
  std::string paintMemoTile;
  bool tileArtSelfTest = false;
  bool tileArtQueueTest = false;
  bool tileDesignerLogTest = false;
  bool tileDesignerArtTest = false;
  bool spriteDesignerObjectTest = false;
  bool spriteDesignerSetTest = false;
  std::string objectArtSwapKey;
  bool artCacheTest = false;
  std::string objectCheckKey;
  std::string drawProbeKey;
  int tileArtSplitCount = 0;
  bool paintMemoTest = false;
  std::string rewriteText;
  std::string rewriteBrief = "tile";
  int tileArtSettleFrames = 2;
  int tileArtHoldFrames = 0;
  bool gapReportOnly = false;
  bool systemOneTest = false;
  std::string system1Profile;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--settings" && i + 1 < argc) {
      settingsPath = argv[++i];
    } else if (arg == "--art-pack" && i + 1 < argc) {
      artPackOverride = argv[++i];
    } else if (arg == "--tile-design" && i + 1 < argc) {
      // Takes a value, so it is checked before the bare-flag spellings below and
      // consumes its own argument -- otherwise the path would be read as the map.
      tileDesignPath = argv[++i];
    } else if (arg == "--sprite-design" && i + 2 < argc) {
      // The kind-dispatched spelling: <kind> <reply>. `--tile-design` stays as the
      // tile-only shorthand it has always been, so nothing that used it changes.
      spriteDesignKind = argv[++i];
      spriteDesignPath = argv[++i];
    } else if (arg == "--sprite-design-overlay" && i + 1 < argc) {
      // Where an accepted object is written, for testing the round trip. Defaults to
      // nothing, which means "accept in memory only".
      spriteDesignOverlay = argv[++i];
    } else if (arg == "--sprite-route" && i + 1 < argc) {
      // One live classification, printed, and exit. Spends a fraction of a cent, so it is
      // never automatic.
      spriteRouteText = argv[++i];
    } else if (arg == "--sprite-prompt-shape" && i + 1 < argc) {
      // Which shape the dumped prompt is built for: "one" (the default) or "set". A flag
      // rather than a second mode, because the prompt dump exists to show what the model
      // would be told and the shape is part of that.
      spritePromptShape = argv[++i];
    } else if (arg == "--sprite-prompt" && i + 1 < argc) {
      // Prints the built system prompt for a kind and exits. No world, no window, no
      // cost -- the verification for the half of the feature that exists before any
      // validator does.
      spritePromptKind = argv[++i];
    } else if (arg == "--tile-art" && i + 1 < argc) {
      tileArtName = argv[++i];
    } else if (arg == "--tile-art-sprite" && i + 1 < argc) {
      // Kept separate from --tile-art so the name stays the first value: a run
      // that only wants to know where the sprite would go passes just the name.
      tileArtSource = argv[++i];
    } else if (arg == "--tile-art-palette" && i + 1 < argc) {
      tileArtPalette = argv[++i];
    } else if (arg == "--tile-art-selftest") {
      // Not a bare flag in spirit: it needs a world, so it runs the game rather
      // than exiting before the window like the other modes.
      tileArtSelfTest = true;
    } else if (arg == "--tile-art-queue") {
      // Exercises the live queue with the configured generation command. Needs a
      // world, so like --tile-art-selftest it runs the game rather than exiting
      // before the window.
      tileArtQueueTest = true;
    } else if (arg == "--rewrite" && i + 1 < argc) {
      // Prints what the Improve button would send. Free: no backend, no request.
      rewriteText = argv[++i];
    } else if (arg == "--rewrite-brief" && i + 1 < argc) {
      rewriteBrief = argv[++i];
    } else if (arg == "--tile-designer-logtest") {
      // Needs a Game (the window reports through it), so it runs Init and exits
      // rather than being a pre-window mode like --tile-design.
      tileDesignerLogTest = true;
    } else if (arg == "--sprite-designer-settest") {
      // The canned conversation asking for a *set* of objects, with one entry deliberately
      // broken, so the per-entry verdicts are exercised end to end through the window.
      //
      // The kind is object rather than tile because the fixture is object-shaped, and a test
      // whose input does not match its kind tests the refusal path instead of the accept
      // path -- which is exactly what the first version of this did: every entry was refused
      // for having a "components" field the *tile* validator does not know.
      tileDesignerLogTest = true;
      spriteDesignerSetTest = true;
      spriteDesignerObjectTest = true;
    } else if (arg == "--sprite-designer-objecttest") {
      // The same canned conversation as --tile-designer-logtest, but designing an
      // *object*: it exercises the object half of the window's inspection path, which is
      // otherwise reachable only by a live model and a person at a keyboard. Needs a Game
      // and a window, and exits.
      tileDesignerLogTest = true;
      spriteDesignerObjectTest = true;
    } else if (arg == "--tile-designer-arttest") {
      // The same canned conversation with the automatic art request on. Also needs
      // a Game, and also exits.
      tileDesignerArtTest = true;
    } else if (arg == "--tile-art-split" && i + 1 < argc) {
      // Queue N sprites and check the per-request cap splits them. Needs a world and a
      // generation command (a stub is fine and free), and exits.
      tileArtSplitCount = std::atoi(argv[++i]);
    } else if (arg == "--object-art-swap" && i + 1 < argc) {
      // Proves a spawned object picks up a newly generated sprite without a reload. Needs a
      // world (it spawns an entity), and exits.
      objectArtSwapKey = argv[++i];
    } else if (arg == "--object-check" && i + 1 < argc) {
      // Asks the *running game's* factory whether a key resolved. Needs a world, because
      // the point is to check the load path Game::ECSInit actually runs rather than a
      // loader the test drives itself.
      objectCheckKey = argv[++i];
    } else if (arg == "--draw-probe" && i + 1 < argc) {
      // Draws one frame with the named object placed on a known tile and reads the
      // pixels back. Needs a window and a world: the whole point is the framebuffer.
      drawProbeKey = argv[++i];
    } else if (arg == "--art-cache-test") {
      // Proves the pipeline panels notice a stage file rewritten in place. Needs a
      // Game and a window (it loads a real texture), and exits.
      artCacheTest = true;
    } else if (arg == "--paint-memo-test" && i + 1 < argc) {
      // Proves art that arrives after the cell was painted is noticed. Takes the tile
      // to drive, which must be on the loaded map and have a generated sprite. Needs a
      // Game and a window, and exits.
      paintMemoTest = true;
      paintMemoTile = argv[++i];
    } else if (arg == "--tile-art-hold" && i + 1 < argc) {
      // Keep drawing for N frames after the queue test settles, so the window's
      // pipeline panels are exercised against a real job. 0 exits immediately.
      tileArtHoldFrames = std::atoi(argv[++i]);
    } else if (arg == "--tile-art-settle" && i + 1 < argc) {
      tileArtSettleFrames = std::atoi(argv[++i]);
    } else if (arg == "--gaps") {
      gapReportOnly = true;
    } else if (arg == "--system1-profile" && i + 1 < argc) {
      // Takes a value, so it is checked before the bare flags below and consumes its
      // own argument -- otherwise the id would be read as the map path.
      system1Profile = argv[++i];
    } else if (arg == "--systemone-test") {
      // Spends one real API request, so it is never automatic. Headless: it needs
      // no world and no display.
      systemOneTest = true;
    } else if (mapOverride.empty()) {
      mapOverride = arg;
    }
  }

  // Read once, before anything else, so Game::Init and the ECS see the same
  // values. A missing or malformed file is reported and leaves the built-in
  // defaults in place.
  LoadGameSettings(settingsPath);

  // Applied after the load, because LoadGameSettings resets to defaults first.
  if (!mapOverride.empty()) {
    MutableSettings().map.file = mapOverride;
  }
  // Must be set before the first sprite resolves: SpritePaths reads the art roots
  // once and caches them.
  if (!artPackOverride.empty()) {
    MutableSettings().art.pack = artPackOverride;
  }
  // Kept out of the settings file on purpose: this is a request about *this* run, so it
  // goes into cliProfile, which outranks both the remembered selection and
  // system1.activeProfile.
  if (!system1Profile.empty()) {
    MutableSettings().system1.cliProfile = system1Profile;
  }

  // Before the window: it asks a decision model one question and prints the answer, so it
  // needs no display and no world.
  if (!spriteRouteText.empty()) {
    return SpriteRouter::RunRouteTest(spriteRouteText);
  }

  // Before the window: it assembles a prompt and prints it, so it needs no display
  // and no backend.
  if (!rewriteText.empty()) {
    return RunRewritePreview(rewriteBrief, rewriteText);
  }

  // Before the window: it needs no display and no world, and it is the only way to
  // verify the System 1 wire format that the overview window depends on.
  if (systemOneTest) {
    return RunSystemOneTest();
  }

  // Before the window, so the report can run on a machine with no display.
  if (gapReportOnly) {
    return RunGapReport();
  }
  // Same reasoning, and before the window for the same reason.
  if (!tileDesignPath.empty()) {
    return RunTileDesign(tileDesignPath);
  }
  // The kind-dispatched spelling. An unknown kind is refused by name rather than
  // silently treated as a tile, because the classifier in a later phase can produce a
  // key this table does not have and that must be a loud outcome, not a tile.
  if (!spriteDesignKind.empty()) {
    const SpriteKinds::Descriptor *kind = SpriteKinds::Parse(spriteDesignKind);
    if (kind == nullptr) {
      std::cerr << "sprite design: unknown kind '" << spriteDesignKind << "'. Known "
                << "kinds:";
      for (const SpriteKinds::Descriptor &candidate : SpriteKinds::All()) {
        std::cerr << " " << candidate.id;
      }
      std::cerr << "\n";
      return 1;
    }
    return RunSpriteDesign(*kind, spriteDesignPath, spriteDesignOverlay);
  }
  if (!spritePromptKind.empty()) {
    const SpriteKinds::Descriptor *kind = SpriteKinds::Parse(spritePromptKind);
    if (kind == nullptr) {
      std::cerr << "sprite prompt: unknown kind '" << spritePromptKind << "'. Known "
                << "kinds:";
      for (const SpriteKinds::Descriptor &candidate : SpriteKinds::All()) {
        std::cerr << " " << candidate.id;
      }
      std::cerr << "\n";
      return 1;
    }
    return RunSpritePrompt(*kind, spritePromptShape);
  }
  // Likewise: the art wiring's file half is provable with no display.
  if (!tileArtName.empty() && !tileArtSelfTest && !tileArtQueueTest) {
    return RunTileArt(tileArtName, tileArtSource, tileArtPalette);
  }

  Game game;
  game.tileDesignerLogSelfTest = tileDesignerLogTest || tileDesignerArtTest;
  game.tileDesignerArtSelfTest = tileDesignerArtTest;
  // Which kind the canned conversation designs. Set before Init, because the window (and
  // its first system prompt) is constructed inside ECSInit.
  game.tileDesignerSelfTestKind = spriteDesignerObjectTest ? SpriteKinds::Kind::Object
                                                          : SpriteKinds::Kind::Tile;
  game.tileDesignerSelfTestSet = spriteDesignerSetTest;
  game.Init();

  // Writes one complete designer log (header, system prompt, request, reply,
  // verdict) with no backend, then exits. Exists because the log is the only record
  // of a conversation and a log you cannot verify by making it is not verified.
  //
  // `--tile-designer-arttest` runs the same conversation with the automatic art
  // request switched on, so the accept-then-request path is covered too: with
  // generation off it must report a refusal, and with a stub command configured it
  // must land in the queue.
  if (tileDesignerLogTest || tileDesignerArtTest) {
    const size_t inspected = game.RunTileDesignerLogSelfTest();
    const std::string newest = NewestTileDesignerLog();
    std::cout << (tileDesignerArtTest   ? "art test: "
                  : spriteDesignerObjectTest ? "object test: "
                  : spriteDesignerSetTest    ? "set test: "
                                             : "log test: ")
              << "inspected " << inspected << " exchange(s)\n";
    if (newest.empty()) {
      std::cout << "log test: no log file was written\n";
    } else {
      std::cout << "log test: wrote " << newest << " ("
                << std::filesystem::file_size(newest) << " bytes)\n";
    }

    // `--tile-art-hold` applies here too when the request is automatic: the queue is
    // only pumped by a frame, so without this the job would sit queued and the test
    // would exit having proved only that the request was accepted. It is what makes
    // the designer test able to cover "accept a tile and get its sprite" end to end.
    if (tileArtHoldFrames > 0) {
      DrawTileArtFrames(game, tileArtHoldFrames);
    }

    game.Shutdown();
    return inspected > 0 ? 0 : 1;
  }

  // The panel caches are the one part of the pipeline view that only misbehaves on
  // the *second* generation, so it gets its own switch rather than being folded into
  // the designer test.
  if (tileArtSplitCount > 0) {
    const int code = RunTileArtSplitTest(game, tileArtSplitCount);
    game.Shutdown();
    return code;
  }

  if (!objectArtSwapKey.empty()) {
    const int code = game.RunObjectArtSelfTest(objectArtSwapKey);
    game.Shutdown();
    return code;
  }

  if (!objectCheckKey.empty()) {
    const Game::ObjectLookup lookup = game.LookupObject(objectCheckKey);
    std::cout << "object:     " << objectCheckKey << "\n";
    if (!lookup.found) {
      std::cout << "  NOT LOADED in the running game\n";
      const std::vector<std::string> keys = game.ObjectKeys();
      std::cout << "  the factory holds " << keys.size() << " template(s); a few: ";
      for (size_t i = 0; i < keys.size() && i < 8; ++i) {
        std::cout << (i ? ", " : "") << keys[i];
      }
      std::cout << "\n";
      game.Shutdown();
      return 1;
    }
    std::cout << "  loaded as \"" << lookup.name << "\" from "
              << (lookup.fromGeneratedOverlay ? "the generated overlay"
                                              : "the hand-authored catalog")
              << "\n";
    game.Shutdown();
    return 0;
  }

  if (artCacheTest) {
    const int code = game.RunArtCacheSelfTest();
    game.Shutdown();
    return code;
  }

  // The drawn proof that an object's transparency reaches the world. Before the paint
  // test below because it needs no extra setup: it spawns its own object and paints its
  // own glyph tile.
  if (!drawProbeKey.empty()) {
    const int code = game.RunDrawProbe(drawProbeKey);
    game.Shutdown();
    return code;
  }

  // The paint path's own regression: art that arrives after a cell was painted has to
  // be noticed, and the memoised absence that used to swallow it lives in this path
  // rather than in the shared refresh helper.
  if (paintMemoTest) {
    const int code = game.RunPaintMemoSelfTest(paintMemoTile);
    game.Shutdown();
    return code;
  }

  // The queue test drives the live path instead of drawing: it needs the world and
  // the frame pump, not pixels on screen, and it must run before the draw loop so
  // its output is not mixed with a running game.
  if (tileArtQueueTest) {
    if (tileArtName.empty()) {
      std::cerr << "queue test: --tile-art <name> is required\n";
      game.Shutdown();
      return 1;
    }
    const int code = RunTileArtQueueTest(game, tileArtName, tileArtHoldFrames);
    game.Shutdown();
    return code;
  }

  // The hot-swap self-test runs the world it just built rather than the draw loop:
  // it needs the ECS settled, not pixels on screen.
  if (tileArtSelfTest) {
    if (tileArtName.empty() || tileArtSource.empty()) {
      std::cerr << "self-test: --tile-art <name> --tile-art-sprite <png> is "
                   "required\n";
      game.Shutdown();
      return 1;
    }
    const int code = RunTileArtSelfTest(game, tileArtName, tileArtSource,
                                        tileArtPalette, tileArtSettleFrames);
    game.Shutdown();
    return code;
  }

  raylib::Window *window = game.getWindow();

  // Detect window close button or ESC key
  while (!game.shouldClose()) {
    game.Update();
    game.handleInput();

    window->BeginDrawing();
    window->ClearBackground(BLACK);

    // Draw world
    game.Draw();

    // Draw UI
    rlImGuiBegin();
    game.DrawGameWindows();
    rlImGuiEnd();

    window->EndDrawing();
  }

  game.Shutdown();
  return 0;
}
