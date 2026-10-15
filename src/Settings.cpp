#include "Settings.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace {

// Returns the named object member, or a shared empty object when the key is
// missing or holds something else. Every section read below starts here, so a
// settings file that omits whole sections behaves as if each key in them were
// absent.
const nlohmann::json &Section(const nlohmann::json &root, const char *key) {
  static const nlohmann::json empty = nlohmann::json::object();
  const auto it = root.find(key);
  if (it == root.end() || !it->is_object()) {
    return empty;
  }
  return *it;
}

// Reads a whole text file into `out`. Returns false and leaves `out` untouched
// when the path is empty or the file cannot be opened.
bool ReadTextFile(const std::string &path, std::string &out) {
  if (path.empty()) {
    return false;
  }
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) {
    return false;
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();
  out = buffer.str();
  return true;
}

// The last-resort source of the API key, kept only so a checkout that already
// has model.json keeps working after settings.json takes over the rest of the
// model block. Anything it supplies is overridden by settings.json or
// settings.local.json.
std::string ReadLegacyApiKey() {
  std::ifstream file("model.json");
  if (!file.is_open()) {
    return "";
  }
  try {
    nlohmann::json legacy;
    file >> legacy;
    return legacy.value("apiKey", "");
  } catch (const std::exception &) {
    return "";
  }
}

std::string ToLower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return static_cast<char>(::tolower(c)); });
  return text;
}

} // namespace

GameSettings &MutableSettings() {
  static GameSettings instance;
  return instance;
}

const GameSettings &Settings() { return MutableSettings(); }

bool LoadGameSettings(const std::string &path) {
  GameSettings &settings = MutableSettings();
  // Reset first, so calling this twice (a future "reload settings" window) does
  // not leave values from the previous read behind.
  settings = GameSettings{};

  bool ok = true;

  nlohmann::json root = nlohmann::json::object();
  std::ifstream file(path);
  if (!file.is_open()) {
    std::cerr << "Settings: " << path
              << " not found; starting from built-in defaults." << std::endl;
    ok = false;
  } else {
    try {
      file >> root;
      if (!root.is_object()) {
        std::cerr << "Settings: " << path
                  << " does not contain a JSON object; starting from built-in "
                     "defaults."
                  << std::endl;
        root = nlohmann::json::object();
        ok = false;
      }
    } catch (const std::exception &e) {
      std::cerr << "Settings: could not parse " << path << ": " << e.what()
                << "; starting from built-in defaults." << std::endl;
      root = nlohmann::json::object();
      ok = false;
    }
  }

  // The untracked override layer. Merged last so it wins over both the main file
  // and the built-in defaults, and so a secret never has to live in a tracked
  // file.
  const std::filesystem::path localPath =
      std::filesystem::path(path).parent_path() / "settings.local.json";
  if (std::filesystem::exists(localPath)) {
    try {
      std::ifstream localFile(localPath);
      nlohmann::json local;
      localFile >> local;
      // RFC 7396 merge patch: nested objects merge key by key, so the override
      // file only needs the keys it actually changes.
      root.merge_patch(local);
      std::cout << "Settings: merged overrides from " << localPath.string()
                << std::endl;
    } catch (const std::exception &e) {
      std::cerr << "Settings: could not read " << localPath.string() << ": "
                << e.what() << std::endl;
    }
  }

  // Every field read lives in this block: a wrong JSON type (a string where a
  // number belongs) throws inside nlohmann, and catching here turns that into a
  // report plus whatever was already read rather than a failed start.
  try {
    const nlohmann::json &map = Section(root, "map");
    settings.map.file = map.value("file", settings.map.file);
    settings.map.spawnNPCs = map.value("spawnNPCs", settings.map.spawnNPCs);
    settings.map.mapsDirectory =
        map.value("mapsDirectory", settings.map.mapsDirectory);

    const nlohmann::json &model = Section(root, "model");
    settings.model.type = ToLower(model.value("type", settings.model.type));
    settings.model.model = model.value("model", settings.model.model);
    settings.model.apiKey = model.value("apiKey", settings.model.apiKey);
    settings.model.endpoint = model.value("endpoint", settings.model.endpoint);
    if (model.contains("temperature")) {
      settings.model.hasTemperature = true;
      settings.model.temperature = model["temperature"].get<double>();
    }
    // Both spellings are accepted: settings.json is camelCase, but model.json
    // used max_tokens and a hand-written file should not have to know that.
    if (model.contains("maxTokens")) {
      settings.model.hasMaxTokens = true;
      settings.model.maxTokens = model["maxTokens"].get<int>();
    } else if (model.contains("max_tokens")) {
      settings.model.hasMaxTokens = true;
      settings.model.maxTokens = model["max_tokens"].get<int>();
    }
    if (model.contains("provider")) {
      settings.model.hasProvider = true;
      settings.model.provider = model["provider"];
    }
    settings.model.keepAlive = model.value("keepAlive", settings.model.keepAlive);
    if (settings.model.apiKey.empty()) {
      settings.model.apiKey = ReadLegacyApiKey();
    }

    // The vision backend is a separate, independent Ollama model behind the
    // Vision Chat window. An absent section keeps the built-in defaults, which
    // already point at moondream on the default Ollama port.
    const nlohmann::json &vision = Section(root, "vision");
    settings.vision.enabled = vision.value("enabled", settings.vision.enabled);
    settings.vision.model = vision.value("model", settings.vision.model);
    settings.vision.endpoint = vision.value("endpoint", settings.vision.endpoint);
    settings.vision.keepAlive = vision.value("keepAlive", settings.vision.keepAlive);
    if (vision.contains("temperature")) {
      settings.vision.hasTemperature = true;
      settings.vision.temperature = vision["temperature"].get<double>();
    }

    // The designer backend. Absent keeps useMainModel on, so an unconfigured run
    // has the designer share the NPC backend rather than failing to start.
    const nlohmann::json &designer = Section(root, "designer");
    settings.designer.useMainModel =
        designer.value("useMainModel", settings.designer.useMainModel);
    settings.designer.promptFile =
        designer.value("promptFile", settings.designer.promptFile);
    settings.designer.maxDesignAttempts = designer.value(
        "maxDesignAttempts", settings.designer.maxDesignAttempts);
    if (settings.designer.maxDesignAttempts < 1) {
      std::cerr << "Settings: designer.maxDesignAttempts must be at least 1; "
                   "using 1." << std::endl;
      settings.designer.maxDesignAttempts = 1;
    }
    if (designer.contains("model")) {
      const nlohmann::json &designerModel = designer["model"];
      GameSettings::ModelSection &target = settings.designer.model;
      target.type = designerModel.value("type", target.type);
      target.model = designerModel.value("model", target.model);
      target.apiKey = designerModel.value("apiKey", target.apiKey);
      target.endpoint = designerModel.value("endpoint", target.endpoint);
      target.keepAlive = designerModel.value("keepAlive", target.keepAlive);
      if (designerModel.contains("temperature")) {
        target.hasTemperature = true;
        target.temperature = designerModel["temperature"].get<double>();
      }
      if (designerModel.contains("maxTokens")) {
        target.hasMaxTokens = true;
        target.maxTokens = designerModel["maxTokens"].get<int>();
      }
      if (designerModel.contains("provider")) {
        target.hasProvider = true;
        target.provider = designerModel["provider"];
      }
    }

    const nlohmann::json &ai = Section(root, "ai");
    settings.ai.npcPromptFile = ai.value("npcPromptFile", settings.ai.npcPromptFile);
    if (!settings.ai.npcPromptFile.empty()) {
      if (ReadTextFile(settings.ai.npcPromptFile, settings.ai.npcPromptTemplate)) {
        std::cout << "Settings: NPC prompt template loaded from "
                  << settings.ai.npcPromptFile << std::endl;
      } else {
        std::cerr << "Settings: could not read NPC prompt template "
                  << settings.ai.npcPromptFile
                  << "; the built-in prompt will be used." << std::endl;
        settings.ai.npcPromptTemplate.clear();
      }
    }

    const nlohmann::json &window = Section(root, "window");
    settings.window.title = window.value("title", settings.window.title);
    settings.window.width = window.value("width", settings.window.width);
    settings.window.height = window.value("height", settings.window.height);
    settings.window.fullscreen =
        window.value("fullscreen", settings.window.fullscreen);
    settings.window.vsync = window.value("vsync", settings.window.vsync);
    settings.window.targetFPS = window.value("targetFPS", settings.window.targetFPS);
    settings.window.monitor = window.value("monitor", settings.window.monitor);

    const nlohmann::json &render = Section(root, "render");
    settings.render.fontPath = render.value("fontPath", settings.render.fontPath);
    settings.render.fontSize = render.value("fontSize", settings.render.fontSize);
    settings.render.cameraMode =
        ToLower(render.value("cameraMode", settings.render.cameraMode));
    settings.render.cameraZoom =
        render.value("cameraZoom", settings.render.cameraZoom);

    const nlohmann::json &player = Section(root, "player");
    settings.player.name = player.value("name", settings.player.name);
    settings.player.startX = player.value("startX", settings.player.startX);
    settings.player.startY = player.value("startY", settings.player.startY);
    settings.player.race = player.value("race", settings.player.race);

    const nlohmann::json &world = Section(root, "world");
    if (world.contains("seed") && world["seed"].is_number()) {
      const int64_t seed = world["seed"].get<int64_t>();
      if (seed < 0) {
        std::cerr << "Settings: world.seed must not be negative; using the clock "
                     "seed instead."
                  << std::endl;
      } else {
        settings.world.seed = static_cast<uint64_t>(seed);
      }
    }

    const nlohmann::json &content = Section(root, "content");
    settings.content.objects = content.value("objects", settings.content.objects);
    settings.content.recipes = content.value("recipes", settings.content.recipes);
    settings.content.stats = content.value("stats", settings.content.stats);
    settings.content.skills = content.value("skills", settings.content.skills);
    settings.content.items = content.value("items", settings.content.items);
    settings.content.races = content.value("races", settings.content.races);
    settings.content.tiles = content.value("tiles", settings.content.tiles);
    settings.content.blueprints =
        content.value("blueprints", settings.content.blueprints);

    const nlohmann::json &debug = Section(root, "debug");
    settings.debug.windowStateFile =
        debug.value("windowStateFile", settings.debug.windowStateFile);
    settings.debug.restServer = debug.value("restServer", settings.debug.restServer);
    settings.debug.logMaxEntries =
        debug.value("logMaxEntries", settings.debug.logMaxEntries);
  } catch (const std::exception &e) {
    std::cerr << "Settings: a value in " << path
              << " has the wrong type: " << e.what()
              << "; every field before it was applied." << std::endl;
    ok = false;
  }

  // Range checks. These keep a plausible-looking typo from producing a window
  // that cannot be created or a registry scan of an empty path, and they report
  // rather than silently rewriting the file.
  if (settings.window.width <= 0 || settings.window.height <= 0) {
    std::cerr << "Settings: window.width/height must be positive; using "
              << GameSettings{}.window.width << "x" << GameSettings{}.window.height
              << "." << std::endl;
    settings.window.width = GameSettings{}.window.width;
    settings.window.height = GameSettings{}.window.height;
    ok = false;
  }
  if (settings.window.targetFPS <= 0) {
    std::cerr << "Settings: window.targetFPS must be positive; using "
              << GameSettings{}.window.targetFPS << "." << std::endl;
    settings.window.targetFPS = GameSettings{}.window.targetFPS;
    ok = false;
  }
  if (settings.render.fontSize <= 0) {
    std::cerr << "Settings: render.fontSize must be positive; using "
              << GameSettings{}.render.fontSize << "." << std::endl;
    settings.render.fontSize = GameSettings{}.render.fontSize;
    ok = false;
  }
  if (settings.render.cameraMode != "follow" && settings.render.cameraMode != "free") {
    std::cerr << "Settings: render.cameraMode '" << settings.render.cameraMode
              << "' is not 'follow' or 'free'; using 'follow'." << std::endl;
    settings.render.cameraMode = "follow";
    ok = false;
  }
  if (settings.debug.logMaxEntries <= 0) {
    std::cerr << "Settings: debug.logMaxEntries must be positive; using "
              << GameSettings{}.debug.logMaxEntries << "." << std::endl;
    settings.debug.logMaxEntries = GameSettings{}.debug.logMaxEntries;
    ok = false;
  }

  return ok;
}
