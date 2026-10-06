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
  } model;

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
  } content;

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
