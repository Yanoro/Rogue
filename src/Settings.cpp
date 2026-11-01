#include "Settings.h"

#include "SettingsSchema.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace {

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

namespace {
// The settings file this run was started with. Read through SettingsFilePath().
std::string &SettingsFilePathStorage() {
  static std::string path = "settings.json";
  return path;
}
}  // namespace

const std::string &SettingsFilePath() { return SettingsFilePathStorage(); }


namespace {

// Reads one model-shaped JSON object into a ModelSection, with the same
// present/absent semantics the table uses for the inline blocks: a missing key
// leaves the member alone, a present one is an override.
void LoadModelSectionJson(const nlohmann::json &json,
                          GameSettings::ModelSection &out) {
  if (!json.is_object()) {
    throw std::runtime_error("the model block is not an object");
  }
  out.type = ToLower(json.value("type", out.type));
  out.model = json.value("model", out.model);
  out.apiKey = json.value("apiKey", out.apiKey);
  out.endpoint = json.value("endpoint", out.endpoint);
  out.keepAlive = json.value("keepAlive", out.keepAlive);
  if (json.contains("temperature")) {
    out.hasTemperature = true;
    out.temperature = json["temperature"].get<double>();
  }
  if (json.contains("maxTokens")) {
    out.hasMaxTokens = true;
    out.maxTokens = json["maxTokens"].get<int>();
  }
  if (json.contains("provider")) {
    out.hasProvider = true;
    out.provider = json["provider"];
  }
}

// One entry of `ai.profiles`. `error` collects warnings as well as failures; the
// boolean is false only when the entry cannot be used at all.
bool LoadAIProfile(const std::string &id, const nlohmann::json &entry,
                   GameSettings::AIProfileSection &out, std::string &error) {
  if (!entry.is_object()) {
    error = "Settings: ai profile '" + id + "' is not an object; skipping\n";
    return false;
  }
  out.label = entry.value("label", id);
  out.kind = ToLower(entry.value("kind", out.kind));
  if (out.kind != "s1" && out.kind != "s2" && out.kind != "vision") {
    error = "Settings: ai profile '" + id + "' has unknown kind '" + out.kind +
            "'; treating it as s2\n";
    out.kind = "s2";
  }
  out.preset = entry.value("preset", out.preset);
  if (!out.preset.empty() && out.kind != "s1") {
    error += "Settings: ai profile '" + id +
             "' sets a preset but is kind " + out.kind +
             "; presets only apply to s1 profiles\n";
  }
  // A preset is the authority for how to reach an S1 model, so an absent field
  // must not smuggle in ModelSection's chat-backend defaults -- Ollama's endpoint
  // and llama3 would silently override the preset, which is exactly the bug
  // SystemOneProfileSection documents. Same rule, same reason.
  if (!out.preset.empty() && out.kind == "s1") {
    out.model.model.clear();
    out.model.endpoint.clear();
    out.model.apiKey.clear();
  }
  if (entry.contains("model")) {
    try {
      LoadModelSectionJson(entry["model"], out.model);
    } catch (const std::exception &e) {
      error += std::string("Settings: ai profile '") + id +
               "' model block has the wrong type: " + e.what() + "\n";
      return false;
    }
  }
  return true;
}

}  // namespace

bool LoadGameSettings(const std::string &path) {
  GameSettings &settings = MutableSettings();
  // Remembered before the parse rather than after it, because a writer needs the
  // path even when this load failed: a missing or malformed file is exactly when
  // the person is most likely to fix the setting from a window.
  SettingsFilePathStorage() = path;
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

  // File defaults that differ from the member's own value. A missing key leaves
  // the member as GameSettings{} built it, which for a sub-object that inherits
  // from the main backend is the chat backend's default ("llama3") rather than
  // the empty string that means "inherit". Those fields say so in the table, and
  // this is where that spelling is applied before anything is read.
  for (const SettingsSchema::FieldSpec &field : SettingsSchema::Fields()) {
    if (field.stringDefault) {
      *static_cast<std::string *>(field.access(settings)) = *field.stringDefault;
    }
  }


  // --- the named backends, and the blocks that reference them ---------------
  //
  // Read before any scalar: a block's `profile` key seeds that block's members,
  // and the scalar pass below then reads the block's own keys over the seed.
  // This is the one place where load order is load-bearing.
  if (const nlohmann::json *profilesRoot =
          SettingsSchema::JsonAt(root, "ai.profiles");
      profilesRoot && profilesRoot->is_object()) {
    for (auto it = profilesRoot->begin(); it != profilesRoot->end(); ++it) {
      GameSettings::AIProfileSection profile;
      std::string error;
      bool usable = true;
      try {
        usable = LoadAIProfile(it.key(), it.value(), profile, error);
      } catch (const std::exception &e) {
        usable = false;
        error += std::string("Settings: ai profile '") + it.key() +
                 "' has the wrong type: " + e.what() + "\n";
      }
      if (!error.empty()) {
        std::cerr << error;
      }
      if (usable) {
        settings.ai.profiles[it.key()] = std::move(profile);
      }
    }
  }

  struct BlockSeed {
    const char *blockPath;
    const char *kind;
    GameSettings::ModelSection &(*block)(GameSettings &);
  };
  static const BlockSeed kBlockSeeds[] = {
      {"model", "s2",
       [](GameSettings &s) -> GameSettings::ModelSection & { return s.model; }},
      {"designer.model", "s2",
       [](GameSettings &s) -> GameSettings::ModelSection & {
         return s.designer.model;
       }},
      {"designer.rewriter.model", "s2",
       [](GameSettings &s) -> GameSettings::ModelSection & {
         return s.designer.rewriter.model;
       }},
      {"navigation.single.model", "s2",
       [](GameSettings &s) -> GameSettings::ModelSection & {
         return s.navigation.single.model;
       }},
      {"system1.model", "s1",
       [](GameSettings &s) -> GameSettings::ModelSection & {
         return s.system1.model;
       }},
  };
  for (const BlockSeed &seed : kBlockSeeds) {
    const nlohmann::json *name = SettingsSchema::JsonAt(
        root, std::string(seed.blockPath) + ".profile");
    if (!name || !name->is_string()) {
      continue;
    }
    const std::string id = name->get<std::string>();
    if (id.empty()) {
      continue;
    }
    const auto found = settings.ai.profiles.find(id);
    if (found == settings.ai.profiles.end()) {
      std::cerr << "Settings: " << seed.blockPath
                << ".profile names unknown ai profile '" << id
                << "'; using the inline block" << std::endl;
      continue;
    }
    if (found->second.kind != seed.kind) {
      std::cerr << "Settings: ai profile '" << id << "' is kind "
                << found->second.kind << " but " << seed.blockPath << " expects "
                << seed.kind << "; using it anyway" << std::endl;
    }
    seed.block(settings) = found->second.model;
  }

  // Vision is not a ModelSection: the profile's model shape maps onto the four
  // fields vision actually has, and the rest is reported rather than silently
  // dropped.
  if (const nlohmann::json *name =
          SettingsSchema::JsonAt(root, "vision.profile");
      name && name->is_string() && !name->get<std::string>().empty()) {
    const std::string id = name->get<std::string>();
    const auto found = settings.ai.profiles.find(id);
    if (found == settings.ai.profiles.end()) {
      std::cerr << "Settings: vision.profile names unknown ai profile '" << id
                << "'; using the inline block" << std::endl;
    } else {
      const GameSettings::AIProfileSection &profile = found->second;
      if (profile.kind != "vision") {
        std::cerr << "Settings: ai profile '" << id << "' is kind "
                  << profile.kind
                  << " but vision.profile expects vision; using it anyway"
                  << std::endl;
      }
      if (profile.model.type != "ollama") {
        std::cerr << "Settings: ai profile '" << id << "' has type "
                  << profile.model.type
                  << " but vision only supports ollama; ignoring the type"
                  << std::endl;
      }
      if (profile.model.hasMaxTokens || profile.model.hasProvider) {
        std::cerr << "Settings: ai profile '" << id
                  << "' sets maxTokens/provider, which vision does not use; "
                     "ignoring them"
                  << std::endl;
      }
      settings.vision.model = profile.model.model;
      settings.vision.endpoint = profile.model.endpoint;
      settings.vision.keepAlive = profile.model.keepAlive;
      settings.vision.hasTemperature = profile.model.hasTemperature;
      settings.vision.temperature = profile.model.temperature;
    }
  }

  bool aborted = false;
  for (const SettingsSchema::FieldSpec &field : SettingsSchema::Fields()) {
    if (!field.persisted) {
      continue;
    }
    const nlohmann::json *value = SettingsSchema::JsonAt(root, field.path);
    for (const std::string &alias : field.aliases) {
      if (value) break;
      value = SettingsSchema::JsonAt(root, alias);
    }
    if (!value) {
      continue;
    }
    // Recorded before the value is read, so a present-but-wrong-typed value still
    // counts as "the file said something" exactly as it did inline.
    if (field.presence) {
      *field.presence(settings) = true;
    }
    std::string messages;
    if (!SettingsSchema::ApplyJson(field, settings, *value, messages)) {
      std::cerr << messages
                << "Settings: every setting before " << field.path
                << " was applied; the rest keep their defaults." << std::endl;
      ok = false;
      aborted = true;
      break;
    }
    if (!messages.empty()) {
      std::cerr << messages;
    }
  }

  // The one collection. Read after the scalars because an entry is an object of
  // fields rather than a leaf, and because a bad entry must not stop the
  // scalars around it from loading.
  if (!aborted) {
    if (const nlohmann::json *profiles =
            SettingsSchema::JsonAt(root, "system1.profiles");
        profiles && profiles->is_array()) {
      settings.system1.profiles.clear();
      for (const nlohmann::json &entry : *profiles) {
        std::string error;
        bool usable = true;
        GameSettings::SystemOneProfileSection profile =
            SettingsSchema::ProfileFromJson(entry, error, usable);
        if (!error.empty()) {
          std::cerr << error;
        }
        if (usable) {
          settings.system1.profiles.push_back(std::move(profile));
        }
      }
    }
  }

  // --- rules that need more than one field --------------------------------

  // The last-resort key source, kept so a checkout that still has model.json
  // works after settings.json took over the rest of the model block.
  if (settings.model.apiKey.empty()) {
    settings.model.apiKey = ReadLegacyApiKey();
  }

  // Credential inheritance: a model block with no key of its own uses the main
  // model's key. This is the rule SETTINGS.md states for a profile's
  // `model.apiKey` ("empty falls back to the block's own key and then the main
  // key") and the one SystemOne::ResolveProfiles has always spelled out by hand.
  //
  // It was the missing half of profiles for every other block. A profile carries
  // the endpoint and the model id, while the credential belongs to the provider
  // rather than to the block that references it -- `"profile": "deepseek"` on
  // the navigator therefore seeded a block with no key while the only key in the
  // run sat in `model.apiKey`, and BuildAIBackend sent
  // "Authorization: Bearer " with nothing after it. OpenRouter answers that with
  // 401 "Missing Authentication header", which reaches the user as an empty reply
  // and a line in the debug log rather than as a configuration error.
  //
  // Runs after the scalar pass and after the legacy fallback, so every block's
  // own key and the main key are both final by the time it decides.
  for (const BlockSeed &seed : kBlockSeeds) {
    GameSettings::ModelSection &block = seed.block(settings);
    if (&block == &settings.model || !block.apiKey.empty()) {
      continue;
    }
    // Only the hosted backends take a key at all; Ollama has no use for one, and
    // copying a secret into a block that cannot use it is only another place to
    // leak it from.
    if (block.type == "openrouter" || block.type == "gemini") {
      block.apiKey = settings.model.apiKey;
    }
  }

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

  if (settings.art.generation.enabled &&
      settings.art.generation.command.empty()) {
    settings.art.generation.command = BuiltinTileArtCommand();
  }

  // Empty is the "not configured" sentinel for a navigator that owns its model,
  // so asking it to use a model block with no model id is a config mistake worth
  // reporting rather than sending "llama3" to somebody else's endpoint.
  if (!settings.navigation.single.useMainModel &&
      settings.navigation.single.model.model.empty()) {
    std::cerr << "Settings: navigation.single.useMainModel is false but "
                 "navigation.single.model.model is empty; using the main "
                 "model settings."
              << std::endl;
    settings.navigation.single.useMainModel = true;
  }

  // The far ring has to contain the mid one, or a place can be observed at a
  // distance its region never covers.
  if (settings.regions.farRadius < settings.regions.midRadius) {
    std::cerr << "Settings: regions.farRadius must be at least midRadius; "
                 "using midRadius."
              << std::endl;
    settings.regions.farRadius = settings.regions.midRadius;
  }

  // Range checks that keep a plausible-looking typo from producing a window that
  // cannot be created or a font that cannot be loaded. These report by setting
  // ok = false, which is how a caller has always been told the file was used but
  // corrected.
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
  if (settings.render.cameraMode != "follow" &&
      settings.render.cameraMode != "free") {
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

  // Last, because these are not in any file: a map argument, --art-pack and
  // --system1-profile are registered by main() and re-applied here so that a
  // reload (the window's Reload and Reset both call this function) cannot undo
  // them. With nothing registered -- the harness, the headless modes -- this is
  // a no-op.
  SettingsSchema::ApplyCommandLineOverrides();

  return ok;
}

//
// Startup-map writer, used by the Map Reload window's "Make current map default".
//
// It edits `map.file` in the settings file and nothing else. The one-value
// substitution below exists because settings.json is hand-authored: it has a
// _readme, blank lines between sections and an order chosen for reading, all of
// which a parse-and-dump would throw away. The parse path is kept only for the
// case a substitution cannot express -- adding a key that is not there yet.
//

namespace {

bool IsJsonSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// One past the closing quote of the JSON string that opens at `quote`, or npos
// when the string is unterminated.
std::size_t SkipJsonString(const std::string &text, std::size_t quote) {
  for (std::size_t i = quote + 1; i < text.size(); ++i) {
    if (text[i] == '\\') {
      ++i;  // Whatever is escaped cannot be the closing quote.
      continue;
    }
    if (text[i] == '"') {
      return i + 1;
    }
  }
  return std::string::npos;
}

// True when the string token [begin,end) -- quotes included -- spells `key`.
// Escapes are not decoded; every key this reads is plain ASCII.
bool JsonStringIs(const std::string &text, std::size_t begin, std::size_t end,
                  const std::string &key) {
  return end == begin + key.size() + 2 &&
         text.compare(begin + 1, key.size(), key) == 0;
}

// Scans the members of one object, whose `{` is just before `from`, for a string
// `file` value at that object's own level. Nested objects are skipped by depth
// so a `file` inside them cannot match.
bool FindFileKeyInObject(const std::string &text, std::size_t from,
                         std::size_t &begin, std::size_t &end) {
  int depth = 1;  // inside the object that owns the members
  for (std::size_t i = from; i < text.size();) {
    const char c = text[i];
    if (c == '"') {
      const std::size_t tokenEnd = SkipJsonString(text, i);
      if (tokenEnd == std::string::npos) return false;
      std::size_t after = tokenEnd;
      while (after < text.size() && IsJsonSpace(text[after])) ++after;
      const bool isKey = after < text.size() && text[after] == ':';
      if (isKey && depth == 1 && JsonStringIs(text, i, tokenEnd, "file")) {
        ++after;  // past ':'
        while (after < text.size() && IsJsonSpace(text[after])) ++after;
        if (after >= text.size() || text[after] != '"') return false;
        const std::size_t valueEnd = SkipJsonString(text, after);
        if (valueEnd == std::string::npos) return false;
        begin = after;
        end = valueEnd;
        return true;
      }
      i = tokenEnd;
      continue;
    }
    if (c == '{' || c == '[') {
      ++depth;
    } else if (c == '}' || c == ']') {
      --depth;
      if (depth == 0) return false;  // the object closed without a `file` key
    }
    ++i;
  }
  return false;
}

// Finds the string value of the top-level `map.file` key in raw JSON text.
// `begin`/`end` bracket the value's quotes. False when the key is absent, the
// text is not an object or the value is not a string -- all of which the caller
// handles by falling back to a real parse.
bool FindTopLevelMapFileValue(const std::string &text, std::size_t &begin,
                              std::size_t &end) {
  int depth = 0;
  for (std::size_t i = 0; i < text.size();) {
    const char c = text[i];
    if (c == '"') {
      const std::size_t tokenEnd = SkipJsonString(text, i);
      if (tokenEnd == std::string::npos) return false;
      std::size_t after = tokenEnd;
      while (after < text.size() && IsJsonSpace(text[after])) ++after;
      const bool isKey = after < text.size() && text[after] == ':';
      if (isKey && depth == 1 && JsonStringIs(text, i, tokenEnd, "map")) {
        ++after;  // past ':'
        while (after < text.size() && IsJsonSpace(text[after])) ++after;
        if (after >= text.size() || text[after] != '{') return false;
        return FindFileKeyInObject(text, after + 1, begin, end);
      }
      i = tokenEnd;
      continue;
    }
    if (c == '{' || c == '[') {
      ++depth;
    } else if (c == '}' || c == ']') {
      --depth;
    }
    ++i;
  }
  return false;
}

// Replaces `path` with `text` through a temporary file in the same directory, so
// a failure cannot leave a truncated settings file behind. `path` is replaced
// atomically by the rename.
bool WriteWholeFile(const std::string &path, const std::string &text,
                    std::string &error) {
  const std::filesystem::path target(path);
  std::filesystem::path temporary = target;
  temporary += ".tmp";

  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
      error = "could not open " + temporary.string() + " for writing";
      return false;
    }
    out << text;
    out.flush();
    if (!out) {
      out.close();
      std::error_code ignored;
      std::filesystem::remove(temporary, ignored);
      error = "could not write " + temporary.string();
      return false;
    }
  }

  std::error_code ec;
  std::filesystem::rename(temporary, target, ec);
  if (ec) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    error = "could not replace " + target.string() + ": " + ec.message();
    return false;
  }
  return true;
}

// True when the untracked override beside `path` sets `map.file` to something
// other than `mapPath`. That layer is merge-patched after the main file, so it
// would win at the next startup and make this write look like a no-op.
bool LocalOverrideChangesMapFile(const std::string &path,
                                 const std::string &mapPath) {
  const std::filesystem::path localPath =
      std::filesystem::path(path).parent_path() / "settings.local.json";
  std::ifstream file(localPath);
  if (!file.is_open()) return false;
  try {
    nlohmann::json local = nlohmann::json::parse(file);
    const nlohmann::json &root = local;
    if (!root.is_object()) return false;
    const auto map = root.find("map");
    if (map == root.end() || !map->is_object()) return false;
    const auto file = map->find("file");
    if (file == map->end() || !file->is_string()) return false;
    return file->get<std::string>() != mapPath;
  } catch (const std::exception &) {
    // A malformed override cannot be applied at startup either, so it cannot
    // contradict what was just written. LoadGameSettings reports it on its own.
    return false;
  }
}

}  // namespace

bool SaveStartupMapToSettingsFile(const std::string &path,
                                  const std::string &mapPath,
                                  std::string &message) {
  message.clear();
  if (mapPath.empty()) {
    message = "No map is loaded, so there is no path to write.";
    return false;
  }

  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) {
    message = "Could not open " + path + " to read it.";
    return false;
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();
  file.close();
  std::string text = buffer.str();

  const std::string jsonValue = nlohmann::json(mapPath).dump();

  std::size_t valueBegin = 0;
  std::size_t valueEnd = 0;
  bool reflowed = false;
  if (FindTopLevelMapFileValue(text, valueBegin, valueEnd)) {
    text.replace(valueBegin, valueEnd - valueBegin, jsonValue);
  } else {
    // No `map.file` string to substitute: the key, the section or a string value
    // is missing. Rebuilding is the only honest way to add one, and it reflows
    // the file -- the message says so, because that is a visible change to a
    // tracked file.
    nlohmann::ordered_json root;
    try {
      root = nlohmann::ordered_json::parse(text);
    } catch (const std::exception &e) {
      message = "Could not parse " + path + ": " + e.what();
      return false;
    }
    if (!root.is_object()) {
      message = path + " does not contain a JSON object.";
      return false;
    }
    if (!root.contains("map") || !root["map"].is_object()) {
      root["map"] = nlohmann::ordered_json::object();
    }
    root["map"]["file"] = mapPath;
    text = root.dump(2) + "\n";
    reflowed = true;
  }

  std::string writeError;
  if (!WriteWholeFile(path, text, writeError)) {
    message = "Could not update " + path + ": " + writeError + ".";
    return false;
  }

  message = "Default map set to " + mapPath + " in " + path + ".";
  if (reflowed) {
    message +=
        " The file had no map.file, so it was rewritten in a standard layout.";
  }
  if (LocalOverrideChangesMapFile(path, mapPath)) {
    message +=
        " settings.local.json also sets map.file, and that file wins at startup.";
  }
  return true;
}

//
// The settings window's write path.
//
// Edits go to `settings.local.json` beside the loaded settings file, not to
// settings.json. That layer is already merged on top at load (RFC 7396), so a
// value written here wins without touching the hand-grouped tracked file -- and
// it is the documented home for the things a run differs in, the API key above
// all. Writes are whole-file and atomic through the same temporary-and-rename
// as the map writer, and the file keeps its key order because it is held as
// ordered_json.
//

namespace {

std::string OverridePath(const std::string &settingsPath) {
  return (std::filesystem::path(settingsPath).parent_path() / "settings.local.json")
      .string();
}

bool ReadOverrideFile(const std::string &path, nlohmann::ordered_json &out,
                      std::string &message) {
  out = nlohmann::ordered_json::object();
  if (!std::filesystem::exists(path)) {
    return true;
  }
  std::ifstream file(path);
  if (!file.is_open()) {
    message = "Could not open " + path + " to read it.";
    return false;
  }
  try {
    file >> out;
  } catch (const std::exception &e) {
    message = "Could not parse " + path + ": " + e.what() + ".";
    return false;
  }
  if (!out.is_object()) {
    message = path + " does not contain a JSON object.";
    return false;
  }
  return true;
}

// The value settings.json alone supplies for `path`. `found` is false when the
// key is absent, which is what lets the writer stop recording an override that
// merely repeats the file or the built-in default.
bool ReadMainValue(const std::string &settingsPath, const std::string &path,
                   nlohmann::json &value, bool &found) {
  found = false;
  value = nullptr;
  std::ifstream file(settingsPath);
  if (!file.is_open()) {
    return false;
  }
  try {
    nlohmann::json root;
    file >> root;
    if (!root.is_object()) {
      return false;
    }
    const nlohmann::json *at = SettingsSchema::JsonAt(root, path);
    if (at) {
      value = *at;
      found = true;
    }
    return true;
  } catch (const std::exception &) {
    return false;
  }
}

bool WriteOverrideFile(const std::string &path, const nlohmann::ordered_json &root,
                       std::string &message) {
  // Removing the last override leaves `{}`, which is a file that says nothing.
  // Deleting it restores the state the run started in, and keeps the question
  // "is anything overridden?" answerable by looking at the directory.
  if (root.empty()) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    if (ec) {
      message = "Could not remove " + path + ": " + ec.message() + ".";
      return false;
    }
    return true;
  }
  std::string writeError;
  if (!WriteWholeFile(path, root.dump(2) + "\n", writeError)) {
    message = "Could not update " + path + ": " + writeError + ".";
    return false;
  }
  return true;
}

}  // namespace

std::string SettingsOverrideFilePath(const std::string &settingsPath) {
  return OverridePath(settingsPath);
}

bool HasSettingOverride(const std::string &settingsPath,
                        const std::string &fieldPath) {
  nlohmann::ordered_json overrides;
  std::string ignored;
  if (!ReadOverrideFile(OverridePath(settingsPath), overrides, ignored)) {
    return false;
  }
  return SettingsSchema::JsonAt(overrides, fieldPath) != nullptr;
}

bool ApplySettingValue(const std::string &settingsPath,
                       const std::string &fieldPath,
                       const nlohmann::json &value, std::string &message) {
  message.clear();
  const SettingsSchema::FieldSpec *field = SettingsSchema::FindField(fieldPath);
  if (!field) {
    message = fieldPath + " is not a known settings field.";
    return false;
  }
  if (!field->persisted) {
    message = fieldPath + " is produced at load and cannot be set here.";
    return false;
  }

  const std::string overridePath = OverridePath(settingsPath);
  nlohmann::ordered_json overrides;
  if (!ReadOverrideFile(overridePath, overrides, message)) {
    return false;
  }

  GameSettings &settings = MutableSettings();

  // A JSON null on a field with a presence flag means "unset", which is a real
  // state distinct from the member's value. The key is removed rather than
  // written as null: RFC 7396 would read a null as a deletion anyway, but an
  // explicit absence is easier to see in the file.
  if (value.is_null() && field->presence) {
    *field->presence(settings) = false;
    static GameSettings compiledDefaults;
    const nlohmann::json fallback =
        SettingsSchema::CurrentJson(*field, compiledDefaults);
    std::string ignored;
    SettingsSchema::ApplyJson(*field, settings, fallback, ignored);
    SettingsSchema::EraseJsonAt(overrides, field->path);
    if (!WriteOverrideFile(overridePath, overrides, message)) {
      return false;
    }
    message = field->path + " is unset; the backend's own default applies.";
    return true;
  }

  std::string corrections;
  if (!SettingsSchema::ApplyJson(*field, settings, value, corrections)) {
    message = corrections;
    return false;
  }
  if (field->presence) {
    *field->presence(settings) = true;
  }
  // The corrected value is what gets written, so a value the table clamps into
  // range is recorded as the value the game will actually run with.
  const nlohmann::json recorded =
      SettingsSchema::CurrentJson(*field, settings);

  bool redundant = false;
  nlohmann::json mainValue;
  bool mainFound = false;
  if (ReadMainValue(settingsPath, field->path, mainValue, mainFound)) {
    if (mainFound) {
      redundant = (mainValue == recorded);
    } else if (!field->presence) {
      redundant = (recorded == SettingsSchema::DefaultJson(*field));
    }
  }

  if (redundant) {
    SettingsSchema::EraseJsonAt(overrides, field->path);
  } else {
    SettingsSchema::SetJsonAt(overrides, field->path, recorded);
  }
  if (!WriteOverrideFile(overridePath, overrides, message)) {
    return false;
  }
  message = corrections;
  message += redundant ? field->path + " matches the settings file; the override was removed."
                       : field->path + " set to " + recorded.dump() + " in " +
                             overridePath + ".";
  return true;
}

bool ResetSettingValue(const std::string &settingsPath,
                       const std::string &fieldPath, std::string &message) {
  message.clear();
  const std::string overridePath = OverridePath(settingsPath);
  nlohmann::ordered_json overrides;
  if (!ReadOverrideFile(overridePath, overrides, message)) {
    return false;
  }
  const bool removed = SettingsSchema::EraseJsonAt(overrides, fieldPath);
  if (removed && !WriteOverrideFile(overridePath, overrides, message)) {
    return false;
  }
  // Reload rather than patching the one member: the value that should come back
  // may come from settings.json, from another key it interacts with, or from a
  // rule in the loader, and only a reload answers that the same way the next
  // launch will.
  LoadGameSettings(settingsPath);
  message = removed ? fieldPath + " no longer overridden; " + settingsPath +
                          " supplies it again."
                    : fieldPath + " was not overridden; settings reloaded.";
  return true;
}

bool ApplyProfilesValue(
    const std::string &settingsPath,
    const std::vector<GameSettings::SystemOneProfileSection> &profiles,
    std::string &message) {
  message.clear();
  nlohmann::json array = nlohmann::json::array();
  for (const GameSettings::SystemOneProfileSection &profile : profiles) {
    array.push_back(SettingsSchema::ProfileToJson(profile));
  }

  const std::string overridePath = OverridePath(settingsPath);
  nlohmann::ordered_json overrides;
  if (!ReadOverrideFile(overridePath, overrides, message)) {
    return false;
  }
  // An array replaces the whole array under RFC 7396, so the effective list is
  // written in full rather than diffed. That is also what makes removing an
  // entry work: a merge patch cannot express a deletion inside an array.
  SettingsSchema::SetJsonAt(overrides, "system1.profiles", array);
  if (!WriteOverrideFile(overridePath, overrides, message)) {
    return false;
  }
  MutableSettings().system1.profiles = profiles;
  message = "system1.profiles written to " + overridePath +
            ". The System 1 resolver is built at startup, so a restart makes it "
            "effective.";
  return true;
}

bool WriteSettingsReference(const std::string &path, std::string &message) {
  message.clear();
  std::string writeError;
  if (!WriteWholeFile(path, SettingsSchema::MarkdownReference(), writeError)) {
    message = "Could not write " + path + ": " + writeError + ".";
    return false;
  }
  message = "Wrote the settings reference to " + path + ".";
  return true;
}

nlohmann::json AIProfileToJson(const GameSettings::AIProfileSection &profile) {
  nlohmann::json entry = nlohmann::json::object();
  entry["label"] = profile.label;
  entry["kind"] = profile.kind;
  if (!profile.preset.empty()) {
    entry["preset"] = profile.preset;
  }
  nlohmann::json model = nlohmann::json::object();
  model["type"] = profile.model.type;
  model["model"] = profile.model.model;
  model["apiKey"] = profile.model.apiKey;
  model["endpoint"] = profile.model.endpoint;
  model["keepAlive"] = profile.model.keepAlive;
  if (profile.model.hasTemperature) {
    model["temperature"] = profile.model.temperature;
  }
  if (profile.model.hasMaxTokens) {
    model["maxTokens"] = profile.model.maxTokens;
  }
  if (profile.model.hasProvider) {
    model["provider"] = profile.model.provider;
  }
  entry["model"] = std::move(model);
  return entry;
}

bool ApplyAIProfileValue(const std::string &settingsPath, const std::string &id,
                         const nlohmann::json &entry, std::string &message) {
  message.clear();
  if (id.empty()) {
    message = "A profile needs an id.";
    return false;
  }
  // The id becomes a key in the override file's dotted path, so a dot in it would
  // silently nest objects instead of naming an entry.
  if (id.find('.') != std::string::npos) {
    message = "A profile id cannot contain '.'; override paths are dotted.";
    return false;
  }
  GameSettings::AIProfileSection probe;
  std::string warnings;
  bool usable = true;
  try {
    usable = LoadAIProfile(id, entry, probe, warnings);
  } catch (const std::exception &e) {
    message = std::string("Invalid profile: ") + e.what();
    return false;
  }
  if (!usable) {
    message = warnings;
    return false;
  }

  const std::string overridePath = OverridePath(settingsPath);
  nlohmann::ordered_json overrides;
  if (!ReadOverrideFile(overridePath, overrides, message)) {
    return false;
  }
  SettingsSchema::SetJsonAt(overrides, "ai.profiles." + id, entry);
  if (!WriteOverrideFile(overridePath, overrides, message)) {
    return false;
  }
  // References are resolved during a load, so writing the file is only half of
  // the edit; the reload is what makes it take effect. Every other in-memory
  // value is already persisted, so nothing is lost.
  LoadGameSettings(settingsPath);
  message = warnings + "ai profile '" + id + "' written to " + overridePath +
            "; settings reloaded so references resolve.";
  return true;
}

bool RemoveAIProfile(const std::string &settingsPath, const std::string &id,
                     std::string &message) {
  message.clear();
  if (id.empty()) {
    message = "A profile needs an id.";
    return false;
  }
  if (id.find('.') != std::string::npos) {
    message = "A profile id cannot contain '.'; override paths are dotted.";
    return false;
  }
  const std::string overridePath = OverridePath(settingsPath);
  nlohmann::ordered_json overrides;
  if (!ReadOverrideFile(overridePath, overrides, message)) {
    return false;
  }
  // RFC 7396 spells deletion as null, which is the only way an override file can
  // remove a profile that settings.json declares.
  SettingsSchema::SetJsonAt(overrides, "ai.profiles." + id,
                            nlohmann::json());
  if (!WriteOverrideFile(overridePath, overrides, message)) {
    return false;
  }
  LoadGameSettings(settingsPath);
  message = "ai profile '" + id + "' removed for this checkout; settings "
            "reloaded.";
  return true;
}

int CountExplicitBlockKeys(const std::string &settingsPath,
                           const std::string &blockPath) {
  std::ifstream file(settingsPath);
  if (!file.is_open()) {
    return 0;
  }
  nlohmann::ordered_json root;
  try {
    file >> root;
  } catch (const std::exception &) {
    return 0;
  }
  if (!root.is_object()) {
    return 0;
  }
  // Merge the override layer exactly as the loader does, so a block filled in
  // either file counts -- an override in settings.local.json is just as hidden
  // behind the picker as one in settings.json.
  const std::string overridePath = OverridePath(settingsPath);
  nlohmann::ordered_json local;
  std::string ignored;
  if (ReadOverrideFile(overridePath, local, ignored)) {
    root.merge_patch(local);
  }
  const nlohmann::ordered_json *block = SettingsSchema::JsonAt(root, blockPath);
  if (block == nullptr || !block->is_object()) {
    return 0;
  }
  int count = 0;
  for (auto it = block->begin(); it != block->end(); ++it) {
    if (it.key() != "profile") {
      ++count;
    }
  }
  return count;
}
