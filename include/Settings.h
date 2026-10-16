#pragma once

// One JSON file, read once at startup, holding every setting the game takes from
// outside its own code: which map to start on, which model to talk to, how the
// window and camera behave, where the player starts, and where the content
// registries live.
//
// The member defaults below are exactly the values the compiled-in constants in
// Defaults.h use, so a missing key, a missing file, or a corrupt file reproduces
// today's behaviour rather than a different one. See SETTINGS.md for the field
// reference and settings.json for a populated example.
//
// Secrets do not belong in settings.json. If a settings.local.json sits beside
// it, that file is merged on top (see LoadGameSettings), which is where an API
// key goes.

#include "Defaults.h"

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>

struct GameSettings {
  struct MapSection {
    // The map the run starts on. A path on the command line still overrides it.
    std::string file = std::string(MAPS_DIRECTORY) + "/Map_Farm.json";
    // Whether the map's authored NPCs are spawned. Off gives an empty world that
    // is useful for map editing.
    bool spawnNPCs = true;
    // Directory the Map Reload window lists its reloadable maps from.
    std::string mapsDirectory = MAPS_DIRECTORY;
  } map;

  struct ModelSection {
    // "ollama", "gemini" or "openrouter". Anything else is treated as ollama.
    std::string type = "ollama";
    std::string model = "llama3";
    // May stay empty when the key lives in model.json or settings.local.json.
    std::string apiKey;
    // Only read for the ollama backend.
    std::string endpoint = "http://localhost:11434/api/generate";
    // The has* flags exist so "absent" stays distinct from "set to the default":
    // absent means the provider's own default is used, exactly as before.
    bool hasTemperature = false;
    double temperature = 0.0;
    bool hasMaxTokens = false;
    int maxTokens = 1500;
    bool hasProvider = false;
    // Passed through to OpenRouter verbatim as its "provider" routing object.
    nlohmann::json provider;
    // How long Ollama keeps the model resident after a request. Empty sends
    // nothing, leaving Ollama's own default (5 minutes, or OLLAMA_KEEP_ALIVE) in
    // charge. Only read for the ollama backend.
    std::string keepAlive;
  } model;

  // Settings for the second, independent backend behind the Vision Chat window.
  //
  // It is deliberately separate from `model` rather than a flag on it: the
  // vision model is a local Ollama model (moondream) while the text backend is
  // whatever the NPCs talk to, and OllamaAI keys its conversation context by
  // contextId alone, so serving both from one instance would replay a vision
  // model's context into a text model's request.
  struct VisionSection {
    // Off leaves the window usable but reporting that no backend is configured,
    // which is friendlier than making the feature look broken when Ollama is not
    // running.
    bool enabled = true;
    // The Ollama model tag. "moondream" resolves to the default tag; write e.g.
    // "moondream:latest" or "moondream:v2" when the installed tag differs.
    std::string model = "moondream";
    // Only the ollama backend is supported for vision today; the field exists so a
    // different host/port works without a rebuild. Point this at /api/chat instead
    // and it will not work -- the payload built here is the /api/generate shape.
    std::string endpoint = "http://localhost:11434/api/generate";
    bool hasTemperature = false;
    double temperature = 0.0;
    // How long Ollama keeps the vision model resident, e.g. "30m". Warm by
    // default: a vision model is several GB, so at Ollama's 5-minute default the
    // first question after any pause pays the full load -- tens of seconds that
    // look like slow inference but are disk I/O. Empty sends nothing and defers to
    // Ollama. Costs VRAM/RAM while it sits there.
    std::string keepAlive = "30m";
  } vision;

  // Settings for the backend behind the Designer Chat window.
  //
  // Its own section rather than reusing `model`, for the same reason Vision has
  // one -- OllamaAI keys conversation context by contextId alone, so one instance
  // cannot serve two different models -- but the reason it is *separately
  // configurable* is different: producing a valid blueprint is a different skill
  // from roleplay, so pointing the designer at a stronger (or cheaper) model
  // without retuning the NPCs is a real thing to want.
  //
  // `useMainModel` defaults on, so an unconfigured run behaves as though the
  // designer shared the NPC backend, which is the least surprising default.
  struct DesignerSection {
    bool useMainModel = true;
    // Read only when useMainModel is false. Same shape and meaning as `model`.
    ModelSection model;
    // The designer's instructions: the grammar, the reply contract and the
    // worked examples. Empty means the compiled-in default (see
    // Game::BuildDesignerSystemPrompt), so a missing file is not a broken run.
    std::string promptFile = "data/blueprints/DESIGNER.md";
    // How many times the engine re-prompts after an invalid design before giving
    // up. Every attempt is shown, so this bounds cost rather than hiding failure.
    int maxDesignAttempts = 3;

    // Reading the designer's reply aloud. Off by default: it is a development
    // window, and a feature that starts making noise on first launch is a
    // feature people turn off before judging.
    //
    // Every field here can be absent, and the whole section can be absent, in
    // which case the game runs exactly as it did before this existed.
    struct TtsSection {
      bool enabled = false;
      // The piper CLI. A bare name is resolved on PATH, which is what the
      // nix-shell environment provides; an absolute path also works.
      std::string binary = "piper";
      // The voice model. Piper resolves "<voice>.onnx.json" beside it for the
      // sample rate, so both files must be present.
      std::string voice = "models/tts/en_GB-alba-medium.onnx";
      // Playback gain, 0..1.
      float volume = 1.0f;
      // What gets read out, because the reply is a bad thing to read literally:
      // most of its bytes are the blueprint JSON, and its prose restates the
      // notes. "notes" speaks the verdict and the model's own critique;
      // "prose_and_notes" adds the prose the notes summarise; "everything"
      // exists only for debugging the pipeline and will read the JSON aloud.
      std::string content = "notes";
    } tts;
  } designer;

  struct AISection {
    // Path to a text file holding the NPC system prompt template. Empty means the
    // compiled-in DEFAULT_NPC_PROMPT is used. The template keeps the same
    // %COMMANDS_LIST%, %LOCATIONS%, %BACKGROUND% and %CHARACTERS% placeholders.
    std::string npcPromptFile;
    // The template text itself, filled by LoadGameSettings. Empty means "use the
    // compiled-in prompt"; Game::createNPC owns that fallback so this struct
    // does not need to know about AgentBrain.
    std::string npcPromptTemplate;
  } ai;

  struct WindowSection {
    std::string title = "AIRogue";
    // Initial window size. On a fullscreen run these also drive the camera's
    // fit-to-map zoom reference, so changing them changes how much world is on
    // screen.
    int width = 1920;
    int height = 1080;
    // false keeps the windowed size above instead of going fullscreen.
    bool fullscreen = true;
    bool vsync = true;
    int targetFPS = 60;
    // -1 picks the monitor the way the game always has: monitor 1 when more than
    // one display is connected, otherwise monitor 0. 0 or more forces that index.
    int monitor = -1;
  } window;

  struct RenderSection {
    std::string fontPath = DEFAULT_FONT_PATH;
    int fontSize = static_cast<int>(DEFAULT_FONTSIZE);
    // "follow" keeps the camera on the player; "free" leaves it to the arrow
    // keys. Anything else falls back to follow.
    std::string cameraMode = "follow";
    // 0 fits the map to the window, which is the original behaviour. Any other
    // value is used as the starting zoom directly.
    float cameraZoom = 0.0f;
  } render;

  struct PlayerSection {
    std::string name = DEFAULT_PLAYER_ENTITY_NAME;
    // Starting tile. Out-of-bounds or wall positions are relocated to the nearest
    // walkable tile by the same pass LoadMap already runs for every entity.
    int startX = 48;
    int startY = 20;
    // Race the player's body slots come from. Empty means the human default.
    std::string race;
  } player;

  struct WorldSection {
    // 0 seeds from the clock, so every run differs. Any other value makes the run
    // reproducible, which is what the Debug Log's printed seed is for.
    uint64_t seed = 0;
  } world;

  struct ContentSection {
    // Where each data registry loads its definitions from.
    std::string objects = "data/objects";
    std::string recipes = "data/recipes";
    std::string stats = "data/stats";
    std::string skills = "data/skills";
    std::string items = "data/items";
    std::string races = "data/races";
    // The world's tile vocabulary. A map's own "tileInfo" is a set of overrides
    // on top of these; with no catalog, maps must declare every tile inline,
    // which is how they all worked before the catalog existed.
    //
    // This is the HAND-AUTHORED catalog and nothing else. Agent-created tiles go
    // to `generatedTiles` below -- see docs/asset-generation-design.md §4.3 --
    // because the vocabulary a designer designs against has to stay exactly as
    // it was written.
    std::string tiles = "data/tiles";
    // The generated overlay for agent-created tiles: loaded additively on top of
    // the catalog at startup, and where the tile designer writes what it makes.
    //
    // Deliberately a sibling of data/tiles rather than a subdirectory of it.
    // TileRegistry::LoadTiles reads every .json in one directory and is not
    // recursive, so a nested overlay would either be picked up as catalog (which
    // would defeat the split) or never be read at all. A separate path also makes
    // the two layers separately deletable: removing this directory reverts the
    // world to hand-authored vocabulary exactly.
    std::string generatedTiles = "data/generated/tiles";
    // The tile designer's instructions: the field grammar, the reply contract and
    // the glyph table. Empty falls back to the generated vocabulary alone, the
    // same way designer.promptFile does.
    std::string tileDesignerPromptFile = "data/tiles/TILEDESIGNER.md";
    // Building designs. Optional: with none, the placement window lists nothing.
    std::string blueprints = "data/blueprints";
  } content;

  // Where the art comes from. Two roots, tried in this order for a convention
  // sprite, so a generated image can never shadow hand-authored art:
  //
  //   1. <pack>/sprites/<id>.png   the art pack: authoritative, wins
  //   2. <generated>/<id>.png      the generated layer: additive, fills only gaps
  //
  // An empty `pack` keeps the pre-split arrangement exactly as it was: every
  // sprite resolves from data/sprites/ and the generated layer is not consulted,
  // because with no pack there is no id to key it by.
  //
  // See docs/asset-generation-design.md for what an art pack is and why the
  // generated layer lives outside it rather than inside the pack.
  struct ArtSection {
    // Art pack directory, e.g. packs/art/pixel_crawler. Empty means "no art
    // pack", which is the current arrangement and needs nothing else changed.
    std::string pack;
    // The directory this pack's generated sprites are in *directly* -- not its
    // parent. Empty derives <pack's grandparent>/generated/<pack dir name>/sprites,
    // which is what the documented layout gives for no configuration at all.
    std::string generated;

    // Live generation of tile art, for tiles an agent invents while the game runs.
    // See docs/tile-art-wiring-design.md sections 4 and 10.2 for why it is a queue
    // and not a call in the frame loop.
    //
    // **This is the one part of the art feature that spends money unattended**, so
    // it is off unless turned on deliberately, and capped.
    struct GenerationSection {
      bool enabled = false;
      // The command that turns a worklist JSON into sprites. `{worklist}` is
      // replaced with the path the game writes; the command must write
      // `<worklist>.result.json` (see tools/gen_tile_art.py --from-worklist).
      //
      // A command rather than a linked-in backend on purpose: image tooling here is
      // Python plus ImageMagick, neither of which is on the default PATH, and the
      // local-LoRA path (tools/gen_tile.py) has to stay swappable behind the same
      // seam. Empty uses `builtinDefaultCommand()` in Settings.cpp, which runs
      // tools/gen_tile_art.py under nix-shell.
      std::string command;
      // How long to collect requests before spending one request on them. One
      // request costs the same whether it carries one tile or nineteen, so this
      // window is what makes a burst of creations cost one request.
      float batchDelaySeconds = 2.0f;
      // Hard ceiling on requests for the session. A request is ~$0.04 (measured),
      // so this default caps a session at about 32 cents.
      int maxRequestsPerSession = 8;
      // Where the worklist and the raw sheets go. Empty uses the default,
      // `data/generated/work`, which is where tools/gen_tile_art.py already keeps
      // them and which .gitignore excludes -- a raw sheet is ~1-3MB and is the one
      // thing in this feature that costs money to reproduce, so it is kept on disk
      // but never committed.
      std::string workDirectory;
    } generation;
  } art;

  struct DebugSection {
    // Visibility state of the debug windows, written on shutdown and read on
    // start.
    std::string windowStateFile = "./debug_windows_state.json";
    // Starts flecs' unauthenticated REST server, which serves the live ECS world
    // over HTTP. Fine locally, best turned off on a shared machine.
    bool restServer = true;
    int logMaxEntries = 1000;
  } debug;
};

// The one settings object for the process. LoadGameSettings fills it before any
// subsystem reads it; Settings() is how the rest of the code reaches it.
GameSettings &MutableSettings();
const GameSettings &Settings();

// Reads `path` into the settings object. When a file named settings.local.json
// sits beside it, that file is merged on top with an RFC 7396 merge patch, so it
// only needs the keys it overrides -- which is where an API key belongs, since
// settings.json is tracked and settings.local.json is not.
//
// Never throws. A missing or malformed file leaves every default in place and
// returns false after reporting on stderr, so a bad settings file is a visible
// "started with defaults" rather than a crash.
bool LoadGameSettings(const std::string &path);

// The command `art.generation.command` falls back to when it is empty but
// generation is enabled: `tools/gen_tile_art.py --from-worklist {worklist}`, run
// under `nix-shell -p python3Packages.pillow imagemagick` because neither tool is
// on the default PATH here. Lives in Settings.cpp so the one place that knows how
// to spell a working command is the one place that resolves it.
std::string BuiltinTileArtCommand();
