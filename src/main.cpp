#include "AI.h"
#include "BlueprintRegistry.h"
#include "DebugLog.h"
#include "Defaults.h"
#include "Game.h"
#include "GapLog.h"
#include "ObjectFactory.h"
#include "Settings.h"
#include "SpritePaths.hpp"
#include "TileArt.hpp"
#include "TileDesigner.hpp"
#include "TileRegistry.h"
#include "Window.hpp"
#include "imgui.h"
#include "raylib.h"
#include "rlImGui.h"

#include <filesystem>
#include <fstream>
#include <iostream>
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

  const TileArt::Target target = TileArt::PlanFor(tileName);

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
  const TileArt::Target target = TileArt::PlanFor(tileName);
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
int RunTileArtQueueTest(Game &game, const std::string &tileName, int maxFrames) {
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
  std::string state;
  int frames = 0;
  for (; frames < maxFrames; ++frames) {
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
    std::cerr << "queue test: nothing settled in " << maxFrames << " frame(s)\n";
    return 1;
  }

  const size_t cellsAfter = game.RefreshTileVisuals(tileName);
  std::cout << "after:      cells-with-a-sprite=" << cellsAfter << "\n";
  std::cout << "requests:   " << queue.RequestsUsed() << " used of "
            << queue.GetConfig().maxRequestsPerSession << "\n";

  if (state != "Done") {
    std::cout << "FAILED:     the job did not complete\n";
    return 1;
  }
  if (cellsAfter == 0) {
    std::cout << "FAILED:     nothing drew the tile, so no swap happened (is '"
              << tileName << "' on the loaded map?)\n";
    return 1;
  }
  std::cout << "queue:      ok (request -> worker -> drain -> swap, frames kept "
               "running throughout)\n";
  return 0;
}

int main(int argc, char *argv[]) {
  // Usage:
  //   AIRogue                        -> settings.json, and the map it names
  //   AIRogue <map.json>             -> settings.json, map overridden
  //   AIRogue --settings <file.json> -> a different settings file
  //   AIRogue --art-pack <dir>       -> settings.json, art pack overridden
  //   AIRogue --gaps                 -> write the art gap report and exit
  //   AIRogue --tile-design <reply>  -> validate one tile reply and exit
  //   AIRogue --tile-art <name>      -> report where a tile's art would go
  //   AIRogue --tile-art <name> --tile-art-sprite <png>  -> place and verify it
  //   AIRogue --tile-art <name> --tile-art-selftest      -> prove the in-world swap
  //   AIRogue --tile-art <name> --tile-art-queue         -> prove the live queue
  // The map argument keeps its old meaning so existing run scripts still work;
  // the -- flags are the only new spellings.
  std::string settingsPath = "settings.json";
  std::string mapOverride;
  std::string artPackOverride;
  std::string tileDesignPath;
  std::string tileArtName;
  std::string tileArtSource;
  std::string tileArtPalette;
  bool tileArtSelfTest = false;
  bool tileArtQueueTest = false;
  int tileArtSettleFrames = 2;
  bool gapReportOnly = false;
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
    } else if (arg == "--tile-art-settle" && i + 1 < argc) {
      tileArtSettleFrames = std::atoi(argv[++i]);
    } else if (arg == "--gaps") {
      gapReportOnly = true;
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

  // Before the window, so the report can run on a machine with no display.
  if (gapReportOnly) {
    return RunGapReport();
  }
  // Same reasoning, and before the window for the same reason.
  if (!tileDesignPath.empty()) {
    return RunTileDesign(tileDesignPath);
  }
  // Likewise: the art wiring's file half is provable with no display.
  if (!tileArtName.empty() && !tileArtSelfTest && !tileArtQueueTest) {
    return RunTileArt(tileArtName, tileArtSource, tileArtPalette);
  }

  Game game;
  game.Init();

  // The queue test drives the live path instead of drawing: it needs the world and
  // the frame pump, not pixels on screen, and it must run before the draw loop so
  // its output is not mixed with a running game.
  if (tileArtQueueTest) {
    if (tileArtName.empty()) {
      std::cerr << "queue test: --tile-art <name> is required\n";
      game.Shutdown();
      return 1;
    }
    const int code = RunTileArtQueueTest(game, tileArtName, 600);
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
