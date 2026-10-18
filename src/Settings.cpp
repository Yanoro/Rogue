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

// The repository root: the nearest ancestor of the working directory that holds a
// `tools/gen_tile_art.py`.
//
// Searched rather than hardcoded because the game is normally launched from the
// root (`./build/AIRogue`) but nothing enforces that, and an absolute path baked in
// at configure time would break the moment the tree is moved or copied. A few
// `is_directory` calls once, at settings load, is nothing.
std::string RepoRoot() {
  std::error_code ec;
  std::filesystem::path directory = std::filesystem::current_path(ec);
  if (ec) return ".";
  while (!directory.empty()) {
    if (std::filesystem::exists(directory / "tools" / "gen_tile_art.py", ec)) {
      return directory.string();
    }
    const std::filesystem::path parent = directory.parent_path();
    if (parent == directory) break;
    directory = parent;
  }
  return ".";
}

// Quotes for `sh -c`.
std::string QuoteForShell(const std::string &text) {
  std::string out = "'";
  for (const char c : text) {
    if (c == '\'') {
      out += "'\\''";
    } else {
      out += c;
    }
  }
  out += "'";
  return out;
}

}  // namespace

std::string BuiltinTileArtCommand() {
  const std::string root = RepoRoot();
  const std::string script =
      (std::filesystem::path(root) / "tools" / "gen_tile_art.py").string();
  // ImageMagick and Pillow are not on the default PATH on this machine, so the
  // default command brings its own via nix-shell -- which means generation works
  // from a plain `./build/AIRogue` launch. The worklist placeholder is quoted and
  // substituted by TileArtQueue.
  return "(cd " + QuoteForShell(root) + " && nix-shell -p python3Packages.pillow "
         "imagemagick --run " +
         QuoteForShell("python3 " + QuoteForShell(script) +
                       " --from-worklist " + QuoteForShell("{worklist}")) +
         ")";
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
    // The designer's own output budget, independent of the agents' that it would
    // otherwise inherit (see DesignerSection::maxTokens). 0 stays the "inherit"
    // sentinel, so a negative value is a mistake rather than a small budget.
    settings.designer.maxTokens =
        designer.value("maxTokens", settings.designer.maxTokens);
    if (settings.designer.maxTokens < 0) {
      std::cerr << "Settings: designer.maxTokens must not be negative; using 0 "
                   "(inherit the backend's own limit)." << std::endl;
      settings.designer.maxTokens = 0;
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

    // The System 1 backend. Absent keeps useMainModel on, so an unconfigured run
    // has System 1 share the NPC backend rather than failing to start. See
    // Settings.h: SystemOneSection for why it is a separate section.
    const nlohmann::json &system1 = Section(root, "system1");
    settings.system1.useMainModel =
        system1.value("useMainModel", settings.system1.useMainModel);

    // Empty is the "inherit the main model" sentinel, and ModelSection's default
    // is "llama3", so the field is cleared here unless the file actually names
    // one. Otherwise a system1 block that sets only a key or an endpoint would
    // inherit llama3 and be rejected by the endpoint -- which is exactly what
    // happened the first time this was wired.
    settings.system1.model.model.clear();
    settings.system1.disableReasoning =
        system1.value("disableReasoning", settings.system1.disableReasoning);
    settings.system1.maxRecords =
        system1.value("maxRecords", settings.system1.maxRecords);
    if (settings.system1.maxRecords < 1) {
      std::cerr << "Settings: system1.maxRecords must be at least 1; using 1."
                << std::endl;
      settings.system1.maxRecords = 1;
    }
    settings.system1.timeoutSeconds =
        system1.value("timeoutSeconds", settings.system1.timeoutSeconds);
    // Recorded before the clamp below, so a malformed value still counts as "the file
    // said something" and gets the warning rather than silently falling back to the
    // endpoint-derived default.
    settings.system1.hasTimeoutSeconds = system1.contains("timeoutSeconds");
    if (settings.system1.timeoutSeconds < 1) {
      std::cerr << "Settings: system1.timeoutSeconds must be at least 1; using 30."
                << std::endl;
      settings.system1.timeoutSeconds = 30;
    }
    // Selectable backends. An array rather than an object map so authoring order is
    // stable -- the picker lists them in the order the file does, which is the order
    // the person who wrote the file expects.
    settings.system1.profiles.clear();
    settings.system1.activeProfile =
        system1.value("activeProfile", settings.system1.activeProfile);
    if (system1.contains("profiles") && system1["profiles"].is_array()) {
      for (const auto &entry : system1["profiles"]) {
        if (!entry.is_object()) {
          std::cerr << "Settings: system1.profiles entry is not an object; skipping"
                    << std::endl;
          continue;
        }
        GameSettings::SystemOneProfileSection profile;
        profile.id = entry.value("id", std::string());
        if (profile.id.empty()) {
          // An unnamed profile cannot be selected or persisted, so it is reported
          // rather than silently added under an empty id.
          std::cerr << "Settings: system1.profiles entry has no 'id'; skipping"
                    << std::endl;
          continue;
        }
        profile.label = entry.value("label", std::string());
        profile.preset = entry.value("preset", std::string());
        profile.timeoutSeconds = entry.value("timeoutSeconds", profile.timeoutSeconds);
        if (profile.timeoutSeconds < 0) {
          std::cerr << "Settings: system1 profile '" << profile.id
                    << "' has a negative timeoutSeconds; using the default"
                    << std::endl;
          profile.timeoutSeconds = 0;
        }
        profile.minConfidence =
            entry.value("minConfidence", profile.minConfidence);
        // Every connection field is cleared before the file is read, because
        // ModelSection's defaults are a *chat* backend: `endpoint` is Ollama's
        // /api/generate and `model` is llama3. Left in place they would silently
        // override a preset -- a profile saying only `"preset": "local_kev"` would
        // resolve to the hosted endpoint with model llama3, which is exactly what
        // happened the first time this parsed. Absent must mean "use the preset".
        profile.model.endpoint.clear();
        profile.model.model.clear();
        profile.model.apiKey.clear();
        if (entry.contains("model") && entry["model"].is_object()) {
          const nlohmann::json &model = entry["model"];
          GameSettings::ModelSection &target = profile.model;
          target.type = model.value("type", target.type);
          target.model = model.value("model", target.model);
          target.apiKey = model.value("apiKey", target.apiKey);
          target.endpoint = model.value("endpoint", target.endpoint);
          target.keepAlive = model.value("keepAlive", target.keepAlive);
          if (model.contains("temperature")) {
            target.hasTemperature = true;
            target.temperature = model["temperature"].get<double>();
          }
          if (model.contains("maxTokens")) {
            target.hasMaxTokens = true;
            target.maxTokens = model["maxTokens"].get<int>();
          }
          if (model.contains("provider")) {
            target.hasProvider = true;
            target.provider = model["provider"];
          }
        }
        settings.system1.profiles.push_back(std::move(profile));
      }
    }

    settings.system1.routeRequests =
        system1.value("routeRequests", settings.system1.routeRequests);
    settings.system1.minConfidence =
        system1.value("minConfidence", settings.system1.minConfidence);
    if (system1.contains("model")) {
      const nlohmann::json &system1Model = system1["model"];
      GameSettings::ModelSection &target = settings.system1.model;
      target.type = system1Model.value("type", target.type);
      target.model = system1Model.value("model", target.model);
      target.apiKey = system1Model.value("apiKey", target.apiKey);
      target.endpoint = system1Model.value("endpoint", target.endpoint);
      target.keepAlive = system1Model.value("keepAlive", target.keepAlive);
      if (system1Model.contains("temperature")) {
        target.hasTemperature = true;
        target.temperature = system1Model["temperature"].get<double>();
      }
      if (system1Model.contains("maxTokens")) {
        target.hasMaxTokens = true;
        target.maxTokens = system1Model["maxTokens"].get<int>();
      }
      if (system1Model.contains("provider")) {
        target.hasProvider = true;
        target.provider = system1Model["provider"];
      }
    }

    // The Improve button's model. Separate from the designer's because rewriting is a
    // short text edit: a reasoning model makes it slow and can spend its whole
    // completion budget thinking and return nothing -- measured on this repo's default
    // backend, see Settings.h.
    {
      const nlohmann::json &rewriter = Section(designer, "rewriter");
      settings.designer.rewriter.enabled =
          rewriter.value("enabled", settings.designer.rewriter.enabled);
      settings.designer.rewriter.disableReasoning =
          rewriter.value("disableReasoning",
                         settings.designer.rewriter.disableReasoning);
      if (rewriter.contains("model")) {
        const nlohmann::json &rewriterModel = rewriter["model"];
        GameSettings::ModelSection &target = settings.designer.rewriter.model;
        target.type = rewriterModel.value("type", target.type);
        target.model = rewriterModel.value("model", target.model);
        target.apiKey = rewriterModel.value("apiKey", target.apiKey);
        target.endpoint = rewriterModel.value("endpoint", target.endpoint);
        target.keepAlive = rewriterModel.value("keepAlive", target.keepAlive);
        if (rewriterModel.contains("temperature")) {
          target.hasTemperature = true;
          target.temperature = rewriterModel["temperature"].get<double>();
        }
        if (rewriterModel.contains("maxTokens")) {
          target.hasMaxTokens = true;
          target.maxTokens = rewriterModel["maxTokens"].get<int>();
        }
        if (rewriterModel.contains("provider")) {
          target.hasProvider = true;
          target.provider = rewriterModel["provider"];
        }
      }
    }

    // Reading the reply aloud. Nested one level deeper than everything else in
    // this section, so that the whole `tts` object can be absent and the defaults
    // still describe a working run -- just a silent one.
    const nlohmann::json &tts = Section(designer, "tts");    settings.designer.tts.enabled =
        tts.value("enabled", settings.designer.tts.enabled);
    settings.designer.tts.binary = tts.value("binary", settings.designer.tts.binary);
    settings.designer.tts.voice = tts.value("voice", settings.designer.tts.voice);
    settings.designer.tts.volume =
        tts.value("volume", settings.designer.tts.volume);
    settings.designer.tts.content = tts.value("content", settings.designer.tts.content);

    // Clamped rather than rejected: the failure modes are both quiet and nasty.
    // Above 1.0 raylib amplifies into clipping, which sounds like a broken voice
    // rather than like a loud one; below 0 it inverts the signal phase.
    if (settings.designer.tts.volume < 0.0f) {
      std::cerr << "Settings: designer.tts.volume must not be negative; using 0."
                << std::endl;
      settings.designer.tts.volume = 0.0f;
    } else if (settings.designer.tts.volume > 1.0f) {
      std::cerr << "Settings: designer.tts.volume is above 1.0 and would clip; "
                   "using 1.0." << std::endl;
      settings.designer.tts.volume = 1.0f;
    }

    // Validated here, at load, so the designer window never has to guess what an
    // unrecognised value meant. Unknown content falls back to the default rather
    // than to "everything", because "everything" reads the JSON aloud.
    if (settings.designer.tts.content != "notes" &&
        settings.designer.tts.content != "prose_and_notes" &&
        settings.designer.tts.content != "everything") {
      std::cerr << "Settings: designer.tts.content is '" +
                       settings.designer.tts.content +
                       "', which is not one of notes, prose_and_notes or "
                       "everything; using notes." << std::endl;
      settings.designer.tts.content = "notes";
    }

    const nlohmann::json &ai = Section(root, "ai");
    settings.ai.npcPromptFile = ai.value("npcPromptFile", settings.ai.npcPromptFile);
    settings.ai.memoryDigest = ai.value("memoryDigest", settings.ai.memoryDigest);
    settings.ai.memoryDigestMinAgeSeconds = ai.value(
        "memoryDigestMinAgeSeconds", settings.ai.memoryDigestMinAgeSeconds);
    settings.ai.memoryDigestCandidates = ai.value(
        "memoryDigestCandidates", settings.ai.memoryDigestCandidates);
    settings.ai.memoryDigestStartupGraceSeconds = ai.value(
        "memoryDigestStartupGraceSeconds",
        settings.ai.memoryDigestStartupGraceSeconds);
    settings.ai.memoryDigestMinCells = ai.value(
        "memoryDigestMinCells", settings.ai.memoryDigestMinCells);
    settings.ai.memoryDigestMaxTokens = ai.value(
        "memoryDigestMaxTokens", settings.ai.memoryDigestMaxTokens);
    settings.ai.memorySalience =
        ai.value("memorySalience", settings.ai.memorySalience);
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
    settings.content.generatedTiles =
        content.value("generatedTiles", settings.content.generatedTiles);
    settings.content.tileDesignerPromptFile = content.value(
        "tileDesignerPromptFile", settings.content.tileDesignerPromptFile);
    settings.content.objectDesignerPromptFile = content.value(
        "objectDesignerPromptFile", settings.content.objectDesignerPromptFile);
    settings.content.generatedObjects = content.value(
        "generatedObjects", settings.content.generatedObjects);
    settings.content.spriteContractFile = content.value(
        "spriteContractFile", settings.content.spriteContractFile);
    settings.content.rewritePromptFile =
        content.value("rewritePromptFile", settings.content.rewritePromptFile);
    settings.content.blueprints =
        content.value("blueprints", settings.content.blueprints);

    const nlohmann::json &art = Section(root, "art");
    settings.art.pack = art.value("pack", settings.art.pack);
    settings.art.generated = art.value("generated", settings.art.generated);
    {
      const nlohmann::json &generation = Section(art, "generation");
      settings.art.generation.enabled =
          generation.value("enabled", settings.art.generation.enabled);
      settings.art.generation.command =
          generation.value("command", settings.art.generation.command);
      settings.art.generation.batchDelaySeconds = generation.value(
          "batchDelaySeconds", settings.art.generation.batchDelaySeconds);
      settings.art.generation.maxRequestsPerSession =
          generation.value("maxRequestsPerSession",
                           settings.art.generation.maxRequestsPerSession);
      settings.art.generation.workDirectory = generation.value(
          "workDirectory", settings.art.generation.workDirectory);
      if (settings.art.generation.enabled &&
          settings.art.generation.command.empty()) {
        settings.art.generation.command = BuiltinTileArtCommand();
      }
      settings.art.generation.maxCellsPerRequest =
          generation.value("maxCellsPerRequest",
                           settings.art.generation.maxCellsPerRequest);
      if (settings.art.generation.maxCellsPerRequest < 1) {
        std::cerr << "Settings: art.generation.maxCellsPerRequest must be at least 1; "
                     "using 1."
                  << std::endl;
        settings.art.generation.maxCellsPerRequest = 1;
      }
      if (settings.art.generation.maxRequestsPerSession < 1) {
        std::cerr << "Settings: art.generation.maxRequestsPerSession must be at "
                     "least 1; using 1." << std::endl;
        settings.art.generation.maxRequestsPerSession = 1;
      }
    }

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
