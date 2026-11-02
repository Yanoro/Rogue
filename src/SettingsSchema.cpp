#include "SettingsSchema.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <sstream>
#include <type_traits>

namespace SettingsSchema {
namespace {

// The nested section types, unqualified. The table names members of the same
// types at several depths, and `&ModelSection::temperature` reads better than
// the fully qualified pointer inside a five-argument call.
using ModelSection = GameSettings::ModelSection;
using VisionSection = GameSettings::VisionSection;
using DesignerSection = GameSettings::DesignerSection;
using RewriterSection = GameSettings::DesignerSection::RewriterSection;
using TtsSection = GameSettings::DesignerSection::TtsSection;
using SystemOneSection = GameSettings::SystemOneSection;
using SystemOneProfileSection = GameSettings::SystemOneProfileSection;
using SystemOneNavigationSection = GameSettings::SystemOneNavigationSection;
using NavigationSection = GameSettings::NavigationSection;
using NavigationSingleSection = GameSettings::NavigationSingleSection;
using NavigationStrategistSection = GameSettings::NavigationStrategistSection;
using NavigationObservationSection = GameSettings::NavigationObservationSection;
using PerceptionSection = GameSettings::PerceptionSection;
using RegionsSection = GameSettings::RegionsSection;
using ChunksSection = GameSettings::ChunksSection;
using WindowSection = GameSettings::WindowSection;
using RenderSection = GameSettings::RenderSection;
using PlayerSection = GameSettings::PlayerSection;
using CombatSection = GameSettings::CombatSection;
using WorldSection = GameSettings::WorldSection;
using ContentSection = GameSettings::ContentSection;
using ArtSection = GameSettings::ArtSection;
using GenerationSection = GameSettings::ArtSection::GenerationSection;
using DebugSection = GameSettings::DebugSection;
using AISection = GameSettings::AISection;
using MapSection = GameSettings::MapSection;

std::string ToLowerCopy(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return static_cast<char>(::tolower(c)); });
  return text;
}

// Compact number for messages and the reference: 6 rather than 6.000000.
std::string NumberText(double value) {
  std::ostringstream out;
  if (std::floor(value) == value && std::abs(value) < 1e15) {
    out << static_cast<long long>(value);
  } else {
    out << value;
  }
  return out.str();
}

double Numeric(const void *leaf, ValueType type) {
  switch (type) {
  case ValueType::Int:
    return *static_cast<const int *>(leaf);
  case ValueType::Int64:
    return static_cast<double>(*static_cast<const long *>(leaf));
  case ValueType::UInt64:
    return static_cast<double>(*static_cast<const unsigned long *>(leaf));
  case ValueType::Float:
    return *static_cast<const float *>(leaf);
  case ValueType::Double:
    return *static_cast<const double *>(leaf);
  default:
    return 0.0;
  }
}

void StoreNumeric(void *leaf, ValueType type, double value) {
  switch (type) {
  case ValueType::Int:
    *static_cast<int *>(leaf) = static_cast<int>(value);
    break;
  case ValueType::Int64:
    *static_cast<long *>(leaf) = static_cast<long>(value);
    break;
  case ValueType::UInt64:
    *static_cast<unsigned long *>(leaf) = static_cast<unsigned long>(value);
    break;
  case ValueType::Float:
    *static_cast<float *>(leaf) = static_cast<float>(value);
    break;
  case ValueType::Double:
    *static_cast<double *>(leaf) = value;
    break;
  default:
    break;
  }
}

nlohmann::json ReadLeaf(const void *leaf, ValueType type) {
  switch (type) {
  case ValueType::Bool:
    return *static_cast<const bool *>(leaf);
  case ValueType::Int:
    return *static_cast<const int *>(leaf);
  case ValueType::Int64:
    return static_cast<long long>(*static_cast<const long *>(leaf));
  case ValueType::UInt64:
    return static_cast<unsigned long long>(
        *static_cast<const unsigned long *>(leaf));
  case ValueType::Float:
    return *static_cast<const float *>(leaf);
  case ValueType::Double:
    return *static_cast<const double *>(leaf);
  case ValueType::String:
  case ValueType::EnumString:
    return *static_cast<const std::string *>(leaf);
  case ValueType::Json:
    return *static_cast<const nlohmann::json *>(leaf);
  }
  return nullptr;
}

// Defined below the table; declared here because the correction pass needs it.
double DefaultNumeric(const FieldSpec &field);
double DefaultNumeric(const CollectionFieldSpec &field);

// The prefix every loader complaint has always carried.
void AppendLine(std::string &message, const std::string &text) {
  message += "Settings: " + text + "\n";
}

std::string BoundsText(const RangeRule &rule) {
  if (rule.min && rule.max) {
    return NumberText(*rule.min) + ".." + NumberText(*rule.max);
  }
  if (rule.min) {
    return rule.minInclusive ? "at least " + NumberText(*rule.min)
                             : "positive";
  }
  if (rule.max) {
    return rule.maxInclusive ? "at most " + NumberText(*rule.max)
                             : "less than " + NumberText(*rule.max);
  }
  return "in range";
}

// Applies the enum and range rules to whatever the member now holds. Shared by
// the loader, the window and the override writer so all three correct a value
// the same way and say the same thing about it.
template <typename Spec>
void CorrectField(const Spec &field, void *leaf, std::string &message,
                  const std::string &pathPrefix = "") {
  const std::string namedPath = pathPrefix + field.path;
  if (field.type == ValueType::EnumString && field.strictEnum &&
      !field.enumValues.empty()) {
    std::string &value = *static_cast<std::string *>(leaf);
    const bool known =
        std::find(field.enumValues.begin(), field.enumValues.end(), value) !=
        field.enumValues.end();
    if (!known) {
      std::string allowed;
      for (std::size_t i = 0; i < field.enumValues.size(); ++i) {
        if (i > 0) {
          allowed += (i + 1 == field.enumValues.size()) ? " or " : ", ";
        }
        allowed += field.enumValues[i];
      }
      AppendLine(message, namedPath + " must be one of " + allowed +
                              "; using " + field.enumValues.front() + ".");
      value = field.enumValues.front();
    }
  }

  if (!field.range) {
    return;
  }
  const RangeRule &rule = *field.range;
  const double value = Numeric(leaf, field.type);
  const bool below =
      rule.min && (rule.minInclusive ? value < *rule.min : value <= *rule.min);
  const bool above =
      rule.max && (rule.maxInclusive ? value > *rule.max : value >= *rule.max);
  if (!below && !above) {
    return;
  }

  if (rule.clamp) {
    StoreNumeric(leaf, field.type, below ? *rule.min : *rule.max);
    const std::string &custom = below ? rule.lowMessage : rule.highMessage;
    if (!custom.empty()) {
      AppendLine(message, custom);
    }
    return;
  }

  // Replace rather than clamp, which is what every range check in the loader
  // used to do: a rejected value becomes a known-good one, and the report says
  // what was used.
  const double replacement =
      rule.resetTo ? *rule.resetTo : DefaultNumeric(field);
  StoreNumeric(leaf, field.type, replacement);
  const std::string &custom = below ? rule.lowMessage : rule.highMessage;
  if (!custom.empty()) {
    AppendLine(message, custom);
    return;
  }
  AppendLine(message, namedPath + " must be " + BoundsText(rule) +
                          "; using " + NumberText(replacement) + ".");
}

}  // namespace

// ---------------------------------------------------------------------------
// FieldSpec modifiers
// ---------------------------------------------------------------------------

FieldSpec &FieldSpec::Enum(std::initializer_list<const char *> values,
                           bool strict) {
  enumValues.clear();
  for (const char *value : values) {
    enumValues.emplace_back(value);
  }
  strictEnum = strict;
  return *this;
}

FieldSpec &FieldSpec::ProfileSource(const char *kind) {
  enumSource = std::string("profile:") + kind;
  return *this;
}

FieldSpec &FieldSpec::ActorSpriteSource() {
  enumSource = "actorSprites";
  return *this;
}

FieldSpec &FieldSpec::Default(std::string value) {
  stringDefault = std::move(value);
  return *this;
}

FieldSpec &FieldSpec::Alias(const char *spelling) {
  aliases.emplace_back(spelling);
  return *this;
}

FieldSpec &FieldSpec::Lower() {
  lowercase = true;
  return *this;
}

FieldSpec &FieldSpec::Secret() {
  secret = true;
  return *this;
}

FieldSpec &FieldSpec::Restart() {
  apply = Apply::Restart;
  return *this;
}

FieldSpec &FieldSpec::Derived() {
  persisted = false;
  return *this;
}

FieldSpec &FieldSpec::Between(double low, double high,
                              std::optional<double> resetTo) {
  RangeRule rule;
  rule.min = low;
  rule.max = high;
  rule.resetTo = resetTo;
  range = std::move(rule);
  return *this;
}

FieldSpec &FieldSpec::Min(double low, std::optional<double> resetTo) {
  RangeRule rule;
  rule.min = low;
  rule.resetTo = resetTo;
  range = std::move(rule);
  return *this;
}

FieldSpec &FieldSpec::MinExclusive(double low, std::optional<double> resetTo) {
  RangeRule rule;
  rule.min = low;
  rule.minInclusive = false;
  rule.resetTo = resetTo;
  range = std::move(rule);
  return *this;
}

FieldSpec &FieldSpec::Max(double high, std::optional<double> resetTo) {
  RangeRule rule;
  rule.max = high;
  rule.resetTo = resetTo;
  range = std::move(rule);
  return *this;
}

FieldSpec &FieldSpec::ClampBetween(double low, double high) {
  RangeRule rule;
  rule.min = low;
  rule.max = high;
  rule.clamp = true;
  range = std::move(rule);
  return *this;
}

FieldSpec &FieldSpec::LowMessage(const char *text) {
  if (!range) {
    range = RangeRule{};
  }
  range->lowMessage = text;
  return *this;
}

FieldSpec &FieldSpec::HighMessage(const char *text) {
  if (!range) {
    range = RangeRule{};
  }
  range->highMessage = text;
  return *this;
}

std::string FieldSpec::Label() const {
  const std::size_t dot = path.rfind('.');
  return dot == std::string::npos ? path : path.substr(dot + 1);
}

// ---------------------------------------------------------------------------
// Dotted-path helpers
// ---------------------------------------------------------------------------

std::vector<std::string> SplitPath(const std::string &path) {
  std::vector<std::string> parts;
  std::string current;
  for (const char c : path) {
    if (c == '.') {
      if (!current.empty()) parts.push_back(current);
      current.clear();
    } else {
      current += c;
    }
  }
  if (!current.empty()) parts.push_back(current);
  return parts;
}

// ---------------------------------------------------------------------------
// The table
// ---------------------------------------------------------------------------

namespace {

GameSettings &Defaults() {
  static GameSettings defaults;
  return defaults;
}

double DefaultNumeric(const FieldSpec &field) {
  const nlohmann::json value = DefaultJson(field);
  return value.is_number() ? value.get<double>() : 0.0;
}

double DefaultNumeric(const CollectionFieldSpec &field) {
  static GameSettings::SystemOneProfileSection defaults;
  return Numeric(field.access(defaults), field.type);
}

}  // namespace

nlohmann::json DefaultJson(const FieldSpec &field) {
  if (field.presence && !*field.presence(Defaults())) {
    return nullptr;
  }
  if (field.stringDefault) {
    return *field.stringDefault;
  }
  return ReadLeaf(field.access(Defaults()), field.type);
}

nlohmann::json CurrentJson(const FieldSpec &field, const GameSettings &settings) {
  GameSettings &mutableSettings = const_cast<GameSettings &>(settings);
  return ReadLeaf(field.access(mutableSettings), field.type);
}

bool ApplyJson(const FieldSpec &field, GameSettings &settings,
               const nlohmann::json &value, std::string &message) {
  message.clear();
  void *leaf = field.access(settings);
  try {
    switch (field.type) {
    case ValueType::Bool:
      *static_cast<bool *>(leaf) = value.get<bool>();
      break;
    case ValueType::Int:
      *static_cast<int *>(leaf) = value.get<int>();
      break;
    case ValueType::Int64:
      *static_cast<long *>(leaf) = static_cast<long>(value.get<long long>());
      break;
    case ValueType::UInt64: {
      const long long raw = value.get<long long>();
      if (raw < 0) {
        *static_cast<unsigned long *>(leaf) = 0;
        AppendLine(message,
                   field.path + " must not be negative; using 0.");
      } else {
        *static_cast<unsigned long *>(leaf) =
            static_cast<unsigned long>(raw);
      }
      break;
    }
    case ValueType::Float:
      *static_cast<float *>(leaf) = value.get<float>();
      break;
    case ValueType::Double:
      *static_cast<double *>(leaf) = value.get<double>();
      break;
    case ValueType::String:
    case ValueType::EnumString: {
      std::string text = value.get<std::string>();
      if (field.lowercase) text = ToLowerCopy(text);
      *static_cast<std::string *>(leaf) = std::move(text);
      break;
    }
    case ValueType::Json:
      *static_cast<nlohmann::json *>(leaf) = value;
      break;
    }
  } catch (const std::exception &error) {
    message = std::string("Settings: ") + field.path +
              " has the wrong type: " + error.what() + ".\n";
    return false;
  }

  CorrectField(field, leaf, message);
  return true;
}

std::string TypeDescription(const FieldSpec &field) {
  std::string text;
  switch (field.type) {
  case ValueType::Bool:
    text = "bool";
    break;
  case ValueType::Int:
  case ValueType::Int64:
  case ValueType::UInt64:
    text = "int";
    break;
  case ValueType::Float:
  case ValueType::Double:
    text = "number";
    break;
  case ValueType::String:
    text = "string";
    break;
  case ValueType::EnumString:
    if (!field.enumSource.empty()) {
      // Values come from a registry at draw time, not from the table.
      text = field.enumSource == "actorSprites" ? "actor sprite" : "profile";
      break;
    }
    text = "string";
    for (std::size_t i = 0; i < field.enumValues.size(); ++i) {
      text += (i == 0 ? ": " : " | ");
      text += field.enumValues[i];
    }
    break;
  case ValueType::Json:
    text = "object";
    break;
  }
  if (field.range) {
    text += " (" + BoundsText(*field.range) + ")";
  }
  return text;
}

// ---------------------------------------------------------------------------
// Profiles
// ---------------------------------------------------------------------------

nlohmann::json ProfileDefaultJson(const CollectionFieldSpec &field) {
  static GameSettings::SystemOneProfileSection defaults;
  if (field.emptyDefault) {
    return std::string();
  }
  return ReadLeaf(field.access(defaults), field.type);
}

nlohmann::json ProfileToJson(
    const GameSettings::SystemOneProfileSection &profile) {
  nlohmann::json entry = nlohmann::json::object();
  GameSettings::SystemOneProfileSection &mutableProfile =
      const_cast<GameSettings::SystemOneProfileSection &>(profile);
  for (const CollectionFieldSpec &field : Profiles().fields) {
    if (field.presence && !*field.presence(mutableProfile)) {
      continue;
    }
    void *leaf = field.access(mutableProfile);
    nlohmann::json value = ReadLeaf(leaf, field.type);
    // A presence flag that is on but whose member is a null JSON is written as
    // an empty object: `"provider": null` reads as "present but nothing", which
    // is a state the OpenRouter backend cannot use.
    if (field.presence && value.is_null() &&
        field.type == ValueType::Json) {
      value = nlohmann::json::object();
    }
    // Absent means "inherit the preset", so an empty string in a field that
    // reads as empty when absent is left out rather than written as "".
    if (field.emptyDefault && value.is_string() &&
        value.get<std::string>().empty()) {
      continue;
    }
    SetJsonAt(entry, field.path, value);
  }
  return entry;
}

GameSettings::SystemOneProfileSection ProfileFromJson(
    const nlohmann::json &entry, std::string &error, bool &usable) {
  GameSettings::SystemOneProfileSection profile;
  error.clear();
  usable = true;
  if (!entry.is_object()) {
    error = "Settings: system1.profiles entry is not an object; skipping\n";
    usable = false;
    return profile;
  }
  for (const CollectionFieldSpec &field : Profiles().fields) {
    void *leaf = field.access(profile);
    if (field.emptyDefault) {
      static_cast<std::string *>(leaf)->clear();
    }
    const nlohmann::json *value = JsonAt(entry, field.path);
    if (!value) continue;
    if (field.presence) {
      *field.presence(profile) = true;
    }
    try {
      switch (field.type) {
      case ValueType::Bool:
        *static_cast<bool *>(leaf) = value->get<bool>();
        break;
      case ValueType::Int:
        *static_cast<int *>(leaf) = value->get<int>();
        break;
      case ValueType::Int64:
        *static_cast<long *>(leaf) = static_cast<long>(value->get<long long>());
        break;
      case ValueType::UInt64:
        *static_cast<unsigned long *>(leaf) =
            static_cast<unsigned long>(value->get<long long>());
        break;
      case ValueType::Float:
        *static_cast<float *>(leaf) = value->get<float>();
        break;
      case ValueType::Double:
        *static_cast<double *>(leaf) = value->get<double>();
        break;
      case ValueType::String:
      case ValueType::EnumString: {
        std::string text = value->get<std::string>();
        if (field.lowercase) text = ToLowerCopy(text);
        *static_cast<std::string *>(leaf) = std::move(text);
        break;
      }
      case ValueType::Json:
        *static_cast<nlohmann::json *>(leaf) = *value;
        break;
      }
    } catch (const std::exception &e) {
      error = std::string("Settings: system1.profiles.") + field.path +
              " has the wrong type: " + e.what() + ".\n";
      usable = false;
      return profile;
    }
    // Profile fields are corrected the same way a top-level one is.
    std::string corrections;
    CorrectField(field, leaf, corrections, "system1.profiles.");
    if (!corrections.empty()) {
      error += corrections;
    }
  }
  if (profile.id.empty()) {
    error += "Settings: system1.profiles entry has no 'id'; skipping\n";
    usable = false;
  }
  return profile;
}

// ---------------------------------------------------------------------------
// Sections
// ---------------------------------------------------------------------------

const std::vector<SectionSpec> &Sections() {
  static const std::vector<SectionSpec> sections = {
      {"map", "map", "Which map the run starts on, and how its authored NPCs are spawned.", ""},
      {"model", "model", "The backend the agents talk to. Read when the ECS world builds its AI instance.", ""},
      {"vision", "vision", "A second, independent Ollama backend behind the Vision Chat window. Separate from `model` because one Ollama instance keys conversation context by id alone, so it cannot serve two models.", ""},
      {"ai", "ai", "The NPC prompt source and the spatial-memory digest.", ""},
      {"ai.profiles", "ai.profiles", "Named AI backends: the endpoint, model, key and options for one model, declared once and referenced by id from any model block. A block's inline fields override the profile; a profile of kind s1, s2 or vision is offered only in the pickers it belongs to.", ""},
      {"designer", "designer", "The blueprint designer's backend, prompt and output budget.", ""},
      {"designer.tts", "designer.tts", "Reading the designer's reply aloud.", ""},
      {"designer.rewriter", "designer.rewriter", "The model behind the Improve button. A short text edit wants a fast, cheap, non-reasoning model, which is usually not the designer's.", ""},
      {"system1", "system1", "The typed-classifier layer and its selectable backends.", ""},
      {"system1.profiles", "system1.profiles", "The selectable System 1 backends. Each entry is how to reach a model plus the two knobs that are meaningful per model.", ""},
      {"system1.navigation", "system1.navigation", "How the System 1 navigator spends calls and time. Properties of the game's use of the model, not of the endpoint, so swapping models does not silently change them.", ""},
      {"navigation", "navigation", "Which navigator runs.", ""},
      {"navigation.single", "navigation.single", "One main-model context that returns both the plan and the next move each decision.", ""},
      {"navigation.strategist", "navigation.strategist", "The standard-LLM strategist above the System 1 pilot.", ""},
      {"perception", "perception", "The acquisition field for every character's vision. Named `perception` because `vision` is already the Vision Chat backend.", ""},
      {"regions", "regions", "Observed base regions: the coarse spatial memory places are derived from.", ""},
      {"chunks", "chunks", "Fixed world chunk grid and the agent-facing area card/route strip.", ""},
      {"window", "window", "Window creation.", ""},
      {"render", "render", "Font and starting camera.", ""},
      {"player", "player", "The player entity and its starting tile.", ""},
      {"combat", "combat", "Aimed skills: indicator-first or instant at the cursor.", ""},
      {"world", "world", "World generation.", ""},
      {"content", "content", "Where each content registry loads its definitions from.", ""},
      {"art", "art", "The art pack and its generated overlay. Convention sprites resolve from `<pack>/sprites/` first, then the generated layer.", ""},
      {"art.generation", "art.generation", "Live generation of art for content an agent invents while the game runs. This is the one part of the art feature that spends money unattended.", ""},
      {"debug", "debug", "Debug-window state, the ECS REST server and log retention.", ""},
  };
  return sections;
}

// ---------------------------------------------------------------------------
// Fields
// ---------------------------------------------------------------------------

const std::vector<FieldSpec> &Fields() {
  static const std::vector<FieldSpec> fields = [] {
    std::vector<FieldSpec> out;
    auto add = [&out](FieldSpec field) -> FieldSpec & {
      out.push_back(std::move(field));
      return out.back();
    };

    // --- map --------------------------------------------------------------
    add(Field<ValueType::String>("map", "map.file",
            "Map loaded at startup. A map path on the command line overrides it.", &GameSettings::map, &MapSection::file))
        .Restart();
    add(Field<ValueType::Bool>("map", "map.spawnNPCs",
            "Whether the map's authored NPCs are spawned. Off gives an empty world for map editing.", &GameSettings::map, &MapSection::spawnNPCs))
        .Restart();
    add(Field<ValueType::String>("map", "map.mapsDirectory",
            "Directory the Map Reload window lists reloadable maps from.", &GameSettings::map, &MapSection::mapsDirectory))
        .Restart();

    // --- model ------------------------------------------------------------
        add(Field<ValueType::EnumString>("model", "model.profile",
            "Name of an ai.profiles entry to take this block's settings from; any inline field overrides it. Empty uses the inline fields alone.", &GameSettings::model, &ModelSection::profile))
        .ProfileSource("s2")
        .Restart();
add(Field<ValueType::EnumString>("model", "model.type",
            "Backend to use. Anything other than the three known names is treated as ollama.", &GameSettings::model, &ModelSection::type))
        .Enum({"ollama", "gemini", "openrouter"}, false)
        .Lower()
        .Restart();
    add(Field<ValueType::String>("model", "model.model",
            "Model id passed to the backend.", &GameSettings::model, &ModelSection::model))
        .Restart();
    add(Field<ValueType::String>("model", "model.apiKey",
            "Key for the gemini and openrouter backends. Leave empty here; put it in settings.local.json.", &GameSettings::model, &ModelSection::apiKey))
        .Secret()
        .Restart();
    add(Field<ValueType::String>("model", "model.endpoint",
            "Ollama endpoint. Only read by the ollama backend.", &GameSettings::model, &ModelSection::endpoint))
        .Restart();
    add(PresenceField<ValueType::Double>("model", "model.temperature",
        "Sampling temperature. Absent leaves the provider's own default in charge.",
        &ModelSection::hasTemperature, &GameSettings::model, &ModelSection::temperature))
        .Restart();
    add(PresenceField<ValueType::Int>("model", "model.maxTokens",
        "Completion budget. Absent leaves the provider's own default in charge.",
        &ModelSection::hasMaxTokens, &GameSettings::model, &ModelSection::maxTokens))
        .Alias("model.max_tokens")
        .Restart();
    add(PresenceField<ValueType::Json>("model", "model.provider",
        "OpenRouter provider routing object, passed through verbatim.",
        &ModelSection::hasProvider, &GameSettings::model, &ModelSection::provider))
        .Restart();
    add(Field<ValueType::String>("model", "model.keepAlive",
            "How long Ollama keeps the model resident, e.g. 30m. Empty sends nothing and leaves Ollama's default in charge.", &GameSettings::model, &ModelSection::keepAlive))
        .Restart();

    // --- vision -----------------------------------------------------------
        add(Field<ValueType::EnumString>("vision", "vision.profile",
            "Name of an ai.profiles entry of kind vision. model, endpoint, keepAlive and temperature come from it; maxTokens and provider have no meaning for vision and are reported.", &GameSettings::vision, &VisionSection::profile))
        .ProfileSource("vision")
        .Restart();
add(Field<ValueType::Bool>("vision", "vision.enabled",
            "Off leaves the Vision Chat window usable but reporting that no backend is configured.", &GameSettings::vision, &VisionSection::enabled))
        .Restart();
    add(Field<ValueType::String>("vision", "vision.model",
            "Ollama model tag for vision.", &GameSettings::vision, &VisionSection::model))
        .Restart();
    add(Field<ValueType::String>("vision", "vision.endpoint",
            "Ollama endpoint. The payload is the /api/generate shape.", &GameSettings::vision, &VisionSection::endpoint))
        .Restart();
    add(PresenceField<ValueType::Double>("vision", "vision.temperature",
        "Sampling temperature. Absent leaves the provider's own default in charge.",
        &VisionSection::hasTemperature, &GameSettings::vision, &VisionSection::temperature))
        .Restart();
    add(Field<ValueType::String>("vision", "vision.keepAlive",
            "How long Ollama keeps the vision model resident. A vision model is several GB, so the default is warm.", &GameSettings::vision, &VisionSection::keepAlive))
        .Restart();

    // --- ai ---------------------------------------------------------------
    add(Field<ValueType::String>("ai", "ai.npcPromptFile",
            "Text file holding the NPC system prompt template. Empty uses the compiled-in prompt.", &GameSettings::ai, &AISection::npcPromptFile))
        .Restart();
    add(Field<ValueType::String>("ai", "ai.npcPromptTemplate",
            "The template text LoadGameSettings read from npcPromptFile. Not a settings key.", &GameSettings::ai, &AISection::npcPromptTemplate))
        .Derived();
    add(Field<ValueType::Bool>("ai", "ai.memoryDigest",
            "Whether a wake asks the model to summarise what the character remembers.", &GameSettings::ai, &AISection::memoryDigest));
    add(Field<ValueType::Int>("ai", "ai.memoryDigestMinAgeSeconds",
            "How stale a digest may be before a wake asks for a new one.", &GameSettings::ai, &AISection::memoryDigestMinAgeSeconds));
    add(Field<ValueType::Int>("ai", "ai.memoryDigestCandidates",
            "How many candidate items the model is shown.", &GameSettings::ai, &AISection::memoryDigestCandidates));
    add(Field<ValueType::Int>("ai", "ai.memoryDigestStartupGraceSeconds",
            "Wall-clock seconds after a world loads before the first digest may be asked for.", &GameSettings::ai, &AISection::memoryDigestStartupGraceSeconds));
    add(Field<ValueType::Int>("ai", "ai.memoryDigestMinCells",
            "How much ground must be remembered before a digest is worth asking for.", &GameSettings::ai, &AISection::memoryDigestMinCells));
    add(Field<ValueType::Int>("ai", "ai.memoryDigestMaxTokens",
            "Output budget for the digest's own request.", &GameSettings::ai, &AISection::memoryDigestMaxTokens));
    add(Field<ValueType::Bool>("ai", "ai.memorySalience",
            "The System 1 salience probe at wake. Logged only; nothing acts on it yet.", &GameSettings::ai, &AISection::memorySalience));

    // --- designer ---------------------------------------------------------
    add(Field<ValueType::Bool>("designer", "designer.useMainModel",
            "Share the NPC backend instead of using the model block below.", &GameSettings::designer, &DesignerSection::useMainModel))
        .Restart();
    add(Field<ValueType::String>("designer", "designer.promptFile",
            "The designer's instructions: the grammar, the reply contract and the worked examples. Empty uses the compiled-in default.", &GameSettings::designer, &DesignerSection::promptFile))
        .Restart();
    add(Field<ValueType::Int>("designer", "designer.maxDesignAttempts",
            "How many times the engine re-prompts after an invalid design before giving up.", &GameSettings::designer, &DesignerSection::maxDesignAttempts))
        .Min(1, 1)
        .Restart();
    add(Field<ValueType::Bool>("designer", "designer.autoRoute",
            "Ask the classifier which domain a request is for, and send once it answers, instead of waiting for the Route button. Hides the domain dropdown. Costs one decision call per request.", &GameSettings::designer, &DesignerSection::autoRoute));
    add(Field<ValueType::Int>("designer", "designer.maxTokens",
            "The designer's own completion budget, overriding the backend's. 0 inherits it.", &GameSettings::designer, &DesignerSection::maxTokens))
        .Min(0, 0)
        .Restart();
        add(Field<ValueType::EnumString>("designer", "designer.model.profile",
            "Name of an ai.profiles entry for the designer's own model block; inline fields override it.", &GameSettings::designer, &DesignerSection::model, &ModelSection::profile))
        .ProfileSource("s2")
        .Restart();
add(Field<ValueType::EnumString>("designer", "designer.model.type",
            "Backend for the designer's own model block.", &GameSettings::designer, &DesignerSection::model, &ModelSection::type))
        .Enum({"ollama", "gemini", "openrouter"}, false)
        .Restart();
    add(Field<ValueType::String>("designer", "designer.model.model",
            "Model id for the designer's own model block.", &GameSettings::designer, &DesignerSection::model, &ModelSection::model))
        .Restart();
    add(Field<ValueType::String>("designer", "designer.model.apiKey",
            "Key for the designer's own model block.", &GameSettings::designer, &DesignerSection::model, &ModelSection::apiKey))
        .Secret()
        .Restart();
    add(Field<ValueType::String>("designer", "designer.model.endpoint",
            "Ollama endpoint for the designer's own model block.", &GameSettings::designer, &DesignerSection::model, &ModelSection::endpoint))
        .Restart();
    add(PresenceField<ValueType::Double>("designer", "designer.model.temperature",
        "Sampling temperature for the designer's own model block.",
        &ModelSection::hasTemperature, &GameSettings::designer, &DesignerSection::model, &ModelSection::temperature))
        .Restart();
    add(PresenceField<ValueType::Int>("designer", "designer.model.maxTokens",
        "Completion budget for the designer's own model block.",
        &ModelSection::hasMaxTokens, &GameSettings::designer, &DesignerSection::model, &ModelSection::maxTokens))
        .Restart();
    add(PresenceField<ValueType::Json>("designer", "designer.model.provider",
        "OpenRouter provider routing for the designer's own model block.",
        &ModelSection::hasProvider, &GameSettings::designer, &DesignerSection::model, &ModelSection::provider))
        .Restart();
    add(Field<ValueType::String>("designer", "designer.model.keepAlive",
            "Ollama keep-alive for the designer's own model block.", &GameSettings::designer, &DesignerSection::model, &ModelSection::keepAlive))
        .Restart();

    // --- designer.rewriter ------------------------------------------------
    add(Field<ValueType::Bool>("designer.rewriter", "designer.rewriter.enabled",
            "False shares the designer's backend with reasoning turned off. True reads the model block below.", &GameSettings::designer, &DesignerSection::rewriter, &RewriterSection::enabled))
        .Restart();
    add(Field<ValueType::Bool>("designer.rewriter", "designer.rewriter.disableReasoning",
            "Turn reasoning off on the rewriter's backend when it shares the designer's.", &GameSettings::designer, &DesignerSection::rewriter, &RewriterSection::disableReasoning))
        .Restart();
        add(Field<ValueType::EnumString>("designer.rewriter", "designer.rewriter.model.profile",
            "Name of an ai.profiles entry for the rewriter's own model block; inline fields override it.", &GameSettings::designer, &DesignerSection::rewriter, &DesignerSection::RewriterSection::model, &ModelSection::profile))
        .ProfileSource("s2")
        .Restart();
add(Field<ValueType::EnumString>("designer.rewriter", "designer.rewriter.model.type",
            "Backend for the rewriter's own model block.", &GameSettings::designer, &DesignerSection::rewriter, &RewriterSection::model, &ModelSection::type))
        .Enum({"ollama", "gemini", "openrouter"}, false)
        .Restart();
    add(Field<ValueType::String>("designer.rewriter", "designer.rewriter.model.model",
            "Model id for the rewriter's own model block.", &GameSettings::designer, &DesignerSection::rewriter, &RewriterSection::model, &ModelSection::model))
        .Restart();
    add(Field<ValueType::String>("designer.rewriter", "designer.rewriter.model.apiKey",
            "Key for the rewriter's own model block.", &GameSettings::designer, &DesignerSection::rewriter, &RewriterSection::model, &ModelSection::apiKey))
        .Secret()
        .Restart();
    add(Field<ValueType::String>("designer.rewriter", "designer.rewriter.model.endpoint",
            "Ollama endpoint for the rewriter's own model block.", &GameSettings::designer, &DesignerSection::rewriter, &RewriterSection::model, &ModelSection::endpoint))
        .Restart();
    add(PresenceField<ValueType::Double>("designer.rewriter", "designer.rewriter.model.temperature",
        "Sampling temperature for the rewriter's own model block.",
        &ModelSection::hasTemperature, &GameSettings::designer, &DesignerSection::rewriter, &DesignerSection::RewriterSection::model, &ModelSection::temperature))
        .Restart();
    add(PresenceField<ValueType::Int>("designer.rewriter", "designer.rewriter.model.maxTokens",
        "Completion budget for the rewriter's own model block.",
        &ModelSection::hasMaxTokens, &GameSettings::designer, &DesignerSection::rewriter, &DesignerSection::RewriterSection::model, &ModelSection::maxTokens))
        .Restart();
    add(PresenceField<ValueType::Json>("designer.rewriter", "designer.rewriter.model.provider",
        "OpenRouter provider routing for the rewriter's own model block.",
        &ModelSection::hasProvider, &GameSettings::designer, &DesignerSection::rewriter, &DesignerSection::RewriterSection::model, &ModelSection::provider))
        .Restart();
    add(Field<ValueType::String>("designer.rewriter", "designer.rewriter.model.keepAlive",
            "Ollama keep-alive for the rewriter's own model block.", &GameSettings::designer, &DesignerSection::rewriter, &RewriterSection::model, &ModelSection::keepAlive))
        .Restart();

    // --- designer.tts -----------------------------------------------------
    add(Field<ValueType::Bool>("designer.tts", "designer.tts.enabled",
            "Read the designer's reply aloud.", &GameSettings::designer, &DesignerSection::tts, &TtsSection::enabled));
    add(Field<ValueType::String>("designer.tts", "designer.tts.binary",
            "The piper CLI. A bare name is resolved on PATH.", &GameSettings::designer, &DesignerSection::tts, &TtsSection::binary));
    add(Field<ValueType::String>("designer.tts", "designer.tts.voice",
            "The voice model. Piper resolves <voice>.onnx.json beside it for the sample rate.", &GameSettings::designer, &DesignerSection::tts, &TtsSection::voice));
    add(Field<ValueType::Float>("designer.tts", "designer.tts.volume",
            "Playback gain, 0..1. Above 1.0 raylib amplifies into clipping; below 0 the phase inverts.", &GameSettings::designer, &DesignerSection::tts, &TtsSection::volume))
        .ClampBetween(0.0, 1.0)
        .LowMessage("designer.tts.volume must not be negative; using 0.")
        .HighMessage("designer.tts.volume is above 1.0 and would clip; using 1.0.");
    add(Field<ValueType::EnumString>("designer.tts", "designer.tts.content",
            "What gets read out. Most of a reply's bytes are the blueprint JSON, so the default speaks only the verdict and critique.", &GameSettings::designer, &DesignerSection::tts, &TtsSection::content))
        .Enum({"notes", "prose_and_notes", "everything"});

    // --- system1 ----------------------------------------------------------
    add(Field<ValueType::Bool>("system1", "system1.useMainModel",
            "Share the NPC backend instead of using the model block below.", &GameSettings::system1, &SystemOneSection::useMainModel))
        .Restart();
        add(Field<ValueType::EnumString>("system1", "system1.model.profile",
            "Name of an ai.profiles entry of kind s1 for the System 1 block; inline fields override it.", &GameSettings::system1, &SystemOneSection::model, &ModelSection::profile))
        .ProfileSource("s1")
        .Restart();
add(Field<ValueType::String>("system1", "system1.model.model",
            "Model id. Empty means inherit the main model's id rather than ModelSection's llama3 default.", &GameSettings::system1, &SystemOneSection::model, &ModelSection::model))
        .Default("")
        .Restart();
    add(Field<ValueType::EnumString>("system1", "system1.model.type",
            "Backend for the System 1 model block.", &GameSettings::system1, &SystemOneSection::model, &ModelSection::type))
        .Enum({"ollama", "gemini", "openrouter"}, false)
        .Restart();
    add(Field<ValueType::String>("system1", "system1.model.apiKey",
            "Key for the System 1 model block.", &GameSettings::system1, &SystemOneSection::model, &ModelSection::apiKey))
        .Secret()
        .Restart();
    add(Field<ValueType::String>("system1", "system1.model.endpoint",
            "Decisions endpoint for the System 1 model block.", &GameSettings::system1, &SystemOneSection::model, &ModelSection::endpoint))
        .Restart();
    add(PresenceField<ValueType::Double>("system1", "system1.model.temperature",
        "Sampling temperature for the System 1 model block.",
        &ModelSection::hasTemperature, &GameSettings::system1, &SystemOneSection::model, &ModelSection::temperature))
        .Restart();
    add(PresenceField<ValueType::Int>("system1", "system1.model.maxTokens",
        "Completion budget for the System 1 model block.",
        &ModelSection::hasMaxTokens, &GameSettings::system1, &SystemOneSection::model, &ModelSection::maxTokens))
        .Restart();
    add(PresenceField<ValueType::Json>("system1", "system1.model.provider",
        "OpenRouter provider routing for the System 1 model block.",
        &ModelSection::hasProvider, &GameSettings::system1, &SystemOneSection::model, &ModelSection::provider))
        .Restart();
    add(Field<ValueType::String>("system1", "system1.model.keepAlive",
            "Ollama keep-alive for the System 1 model block.", &GameSettings::system1, &SystemOneSection::model, &ModelSection::keepAlive))
        .Restart();
    add(Field<ValueType::Bool>("system1", "system1.disableReasoning",
            "Ask the backend for a single non-reasoning answer. System 1's premise is a cheap, fast, one-shot classification.", &GameSettings::system1, &SystemOneSection::disableReasoning))
        .Restart();
    add(Field<ValueType::Int>("system1", "system1.maxRecords",
            "Bounds the in-memory view the overview window keeps. The log file keeps everything.", &GameSettings::system1, &SystemOneSection::maxRecords))
        .Min(1, 1)
        .Restart();
    add(PresenceField<ValueType::Int64>("system1", "system1.timeoutSeconds",
        "Whole-request budget for one decision. Absent follows the endpoint: 120s local, 30s hosted.",
        &SystemOneSection::hasTimeoutSeconds, &GameSettings::system1, &SystemOneSection::timeoutSeconds))
        .Min(1, 30)
        .Restart();
    add(Field<ValueType::String>("system1", "system1.activeProfile",
            "Which profile starts active, unless the persisted choice or --system1-profile says otherwise. Empty means 'default'.", &GameSettings::system1, &SystemOneSection::activeProfile));
    add(Field<ValueType::String>("system1", "system1.cliProfile",
            "Set from --system1-profile by main() after the file is loaded. Never read from the file.", &GameSettings::system1, &SystemOneSection::cliProfile))
        .Derived();
    add(Field<ValueType::Bool>("system1", "system1.routeRequests",
            "Whether the sprite designer asks a classifier what kind of sprite a request is for.", &GameSettings::system1, &SystemOneSection::routeRequests));
    add(Field<ValueType::Float>("system1", "system1.minConfidence",
            "Below this, the route verdict is reported as unsure rather than acted on. 0 disables the check.", &GameSettings::system1, &SystemOneSection::minConfidence));

    // --- system1.navigation -----------------------------------------------
    add(Field<ValueType::Bool>("system1.navigation", "system1.navigation.enabled",
            "The only switch. [NAVIGATE] reports that it is off rather than silently falling back.", &GameSettings::system1, &SystemOneSection::navigation, &SystemOneNavigationSection::enabled));
    add(Field<ValueType::Int>("system1.navigation", "system1.navigation.maxCandidates",
            "One line per action: local compass move or distant place.", &GameSettings::system1, &SystemOneSection::navigation, &SystemOneNavigationSection::maxCandidates))
        .Min(1, 1);
    add(Field<ValueType::Int>("system1.navigation", "system1.navigation.maxDirections",
            "Local compass moves offered before the distant place list.", &GameSettings::system1, &SystemOneSection::navigation, &SystemOneNavigationSection::maxDirections))
        .Min(0, 0);
    add(Field<ValueType::Int>("system1.navigation", "system1.navigation.compassRadius",
            "How far a sector looks for the first place.", &GameSettings::system1, &SystemOneSection::navigation, &SystemOneNavigationSection::compassRadius))
        .Min(1, 1);
    add(Field<ValueType::Int>("system1.navigation", "system1.navigation.maxSurveys",
            "Surveys allowed for one navigation before it gives up.", &GameSettings::system1, &SystemOneSection::navigation, &SystemOneNavigationSection::maxSurveys))
        .Min(0, 0);
    add(Field<ValueType::Float>("system1.navigation", "system1.navigation.maxSeconds",
            "Wall-clock budget for one navigation.", &GameSettings::system1, &SystemOneSection::navigation, &SystemOneNavigationSection::maxSeconds))
        .MinExclusive(0.0);
    add(Field<ValueType::Float>("system1.navigation", "system1.navigation.legTimeoutSeconds",
            "Budget for one leg of a navigation.", &GameSettings::system1, &SystemOneSection::navigation, &SystemOneNavigationSection::legTimeoutSeconds))
        .MinExclusive(0.0);
    add(Field<ValueType::Float>("system1.navigation", "system1.navigation.minProbability",
            "Below this probability a candidate is not acted on.", &GameSettings::system1, &SystemOneSection::navigation, &SystemOneNavigationSection::minProbability))
        .ClampBetween(0.0, 1.0);
    add(Field<ValueType::Float>("system1.navigation", "system1.navigation.minMargin",
            "Required difference between the best and the runner-up candidate.", &GameSettings::system1, &SystemOneSection::navigation, &SystemOneNavigationSection::minMargin))
        .ClampBetween(0.0, 1.0);
    add(Field<ValueType::EnumString>("system1.navigation", "system1.navigation.mapMode",
            "What spatial input the sector prompt gets: none, an appended ASCII map, or a map that replaces the prose.", &GameSettings::system1, &SystemOneSection::navigation, &SystemOneNavigationSection::mapMode))
        .Enum({"off", "append", "replace"})
        .Lower();
    add(Field<ValueType::Int>("system1.navigation", "system1.navigation.mapRadius",
            "Radius of the ASCII map around the character.", &GameSettings::system1, &SystemOneSection::navigation, &SystemOneNavigationSection::mapRadius))
        .Between(3, 12, 6);
    add(Field<ValueType::Int>("system1.navigation", "system1.navigation.examineRadius",
            "Radius searched for the examine affordance list.", &GameSettings::system1, &SystemOneSection::navigation, &SystemOneNavigationSection::examineRadius))
        .Between(1, 64);
    add(Field<ValueType::Int>("system1.navigation", "system1.navigation.maxExamine",
            "Objects from that list offered to the two-context System 1, reserved inside maxCandidates.", &GameSettings::system1, &SystemOneSection::navigation, &SystemOneNavigationSection::maxExamine))
        .Between(0, 12, 2);

    // --- navigation -------------------------------------------------------
    add(Field<ValueType::EnumString>("navigation", "navigation.agent",
            "Which navigator runs: the single-context plan-and-move navigator, or the strategist plus System 1 pilot.", &GameSettings::navigation, &NavigationSection::agent))
        .Enum({"single", "two"})
        .Lower();
    add(Field<ValueType::Int>("navigation.single", "navigation.single.maxTokens",
            "Completion budget for one decision.", &GameSettings::navigation, &NavigationSection::single, &NavigationSingleSection::maxTokens))
        .Between(64, 8192, 500)
        .Restart();
    add(Field<ValueType::Bool>("navigation.single", "navigation.single.dedicatedBackend",
            "Build a dedicated AI instance for the navigator instead of sharing the NPC backend.", &GameSettings::navigation, &NavigationSection::single, &NavigationSingleSection::dedicatedBackend))
        .Restart();
    add(Field<ValueType::Bool>("navigation.single", "navigation.single.disableReasoning",
            "Turn the dedicated backend's chain-of-thought off. Reasoning can spend the whole budget before any JSON is emitted.", &GameSettings::navigation, &NavigationSection::single, &NavigationSingleSection::disableReasoning))
        .Restart();
    add(Field<ValueType::Bool>("navigation.single", "navigation.single.compactJson",
            "Send compact JSON in the single prompt. Every space and newline is a prompt token.", &GameSettings::navigation, &NavigationSection::single, &NavigationSingleSection::compactJson));
    add(Field<ValueType::Int>("navigation.single", "navigation.single.historyTurns",
            "Previous decision turns kept in the navigator's context. 0 keeps the whole episode.", &GameSettings::navigation, &NavigationSection::single, &NavigationSingleSection::historyTurns))
        .Between(0, 64, 3);
    add(Field<ValueType::Bool>("navigation.single", "navigation.single.concise",
            "Ask for a one-sentence strategy and a short think block.", &GameSettings::navigation, &NavigationSection::single, &NavigationSingleSection::concise));
    add(Field<ValueType::Bool>("navigation.single", "navigation.single.objectSummary",
            "Group objects by type/profile in the STATE block and use the target grammar (objects:all, inspect:<group>, approach:<alias>) instead of listing every examine candidate.", &GameSettings::navigation, &NavigationSection::single, &NavigationSingleSection::objectSummary));
    add(Field<ValueType::Int>("navigation.single", "navigation.single.maxObjectGroups",
            "Object groups shown in the default STATE block. The rest stay available through objects:all and inspect.", &GameSettings::navigation, &NavigationSection::single, &NavigationSingleSection::maxObjectGroups))
        .Between(1, 64, 6);
    add(Field<ValueType::Int>("navigation.single", "navigation.single.maxGroupDetails",
            "Instances shown by inspect:<group>. inspect:<group>:all ignores this.", &GameSettings::navigation, &NavigationSection::single, &NavigationSingleSection::maxGroupDetails))
        .Between(1, 128, 8);
    add(Field<ValueType::Bool>("navigation.single", "navigation.single.includeGraph",
            "Include the observation graph in the single prompt. Off for a small local model: the place graph duplicates what STATE/ACTIONS already say.", &GameSettings::navigation, &NavigationSection::single, &NavigationSingleSection::includeGraph));
    add(Field<ValueType::Bool>("navigation.single", "navigation.single.includeMap",
            "Include the ASCII map and its target list in the single prompt. Off for a small local model; the candidate actions remain in ACTIONS.", &GameSettings::navigation, &NavigationSection::single, &NavigationSingleSection::includeMap));
    add(Field<ValueType::Int>("navigation.single", "navigation.single.contextTokens",
            "Context window requested from Ollama (options.num_ctx). Too small and the model loses the start of the prompt.", &GameSettings::navigation, &NavigationSection::single, &NavigationSingleSection::contextTokens))
        .Between(2048, 32768, 8192)
        .Restart();
    add(Field<ValueType::Float>("navigation.single", "navigation.single.timeoutSeconds",
            "Whole-request budget for one decision.", &GameSettings::navigation, &NavigationSection::single, &NavigationSingleSection::timeoutSeconds))
        .Min(1.0, 60.0);
    add(Field<ValueType::Bool>("navigation.single", "navigation.single.fallbackToTwo",
            "Fall back to the strategist plus pilot when the single navigator's replies are unusable.", &GameSettings::navigation, &NavigationSection::single, &NavigationSingleSection::fallbackToTwo));
    add(Field<ValueType::Int>("navigation.single", "navigation.single.mapRadius",
            "Radius of the ASCII map offered with each decision.", &GameSettings::navigation, &NavigationSection::single, &NavigationSingleSection::mapRadius))
        .Between(3, 12, 6);
    add(Field<ValueType::Bool>("navigation.single", "navigation.single.useMainModel",
            "Keep the NPC model settings. False reads the model block below.", &GameSettings::navigation, &NavigationSection::single, &NavigationSingleSection::useMainModel))
        .Restart();
        add(Field<ValueType::EnumString>("navigation.single", "navigation.single.model.profile",
            "Name of an ai.profiles entry for the navigator's own model block; inline fields override it.", &GameSettings::navigation, &NavigationSection::single, &NavigationSingleSection::model, &ModelSection::profile))
        .ProfileSource("s2")
        .Restart();
add(Field<ValueType::String>("navigation.single", "navigation.single.model.model",
            "Model id for the navigator's own model block. Empty is not configured.", &GameSettings::navigation, &NavigationSection::single, &NavigationSingleSection::model, &ModelSection::model))
        .Default("")
        .Restart();
    add(Field<ValueType::EnumString>("navigation.single", "navigation.single.model.type",
            "Backend for the navigator's own model block.", &GameSettings::navigation, &NavigationSection::single, &NavigationSingleSection::model, &ModelSection::type))
        .Enum({"ollama", "gemini", "openrouter"}, false)
        .Restart();
    add(Field<ValueType::String>("navigation.single", "navigation.single.model.apiKey",
            "Key for the navigator's own model block.", &GameSettings::navigation, &NavigationSection::single, &NavigationSingleSection::model, &ModelSection::apiKey))
        .Secret()
        .Restart();
    add(Field<ValueType::String>("navigation.single", "navigation.single.model.endpoint",
            "Ollama endpoint for the navigator's own model block.", &GameSettings::navigation, &NavigationSection::single, &NavigationSingleSection::model, &ModelSection::endpoint))
        .Restart();
    add(PresenceField<ValueType::Double>("navigation.single", "navigation.single.model.temperature",
        "Sampling temperature for the navigator's own model block.",
        &ModelSection::hasTemperature, &GameSettings::navigation, &NavigationSection::single, &NavigationSingleSection::model, &ModelSection::temperature))
        .Restart();
    add(PresenceField<ValueType::Int>("navigation.single", "navigation.single.model.maxTokens",
        "Completion budget for the navigator's own model block.",
        &ModelSection::hasMaxTokens, &GameSettings::navigation, &NavigationSection::single, &NavigationSingleSection::model, &ModelSection::maxTokens))
        .Restart();
    add(PresenceField<ValueType::Json>("navigation.single", "navigation.single.model.provider",
        "OpenRouter provider routing for the navigator's own model block.",
        &ModelSection::hasProvider, &GameSettings::navigation, &NavigationSection::single, &NavigationSingleSection::model, &ModelSection::provider))
        .Restart();
    add(Field<ValueType::String>("navigation.single", "navigation.single.model.keepAlive",
            "Ollama keep-alive for the navigator's own model block.", &GameSettings::navigation, &NavigationSection::single, &NavigationSingleSection::model, &ModelSection::keepAlive))
        .Restart();
    add(Field<ValueType::Int>("navigation.single", "navigation.single.maxAttempts",
            "Consecutive unusable replies allowed for one decision before the fallback.", &GameSettings::navigation, &NavigationSection::single, &NavigationSingleSection::maxAttempts))
        .Between(1, 8, 3);
    add(Field<ValueType::Int>("navigation.single", "navigation.single.maxDirections",
            "Compass moves offered to the main model.", &GameSettings::navigation, &NavigationSection::single, &NavigationSingleSection::maxDirections))
        .Between(1, 8, 8);
    add(Field<ValueType::Int>("navigation.single", "navigation.single.maxCandidates",
            "Total candidates offered: compass moves plus distant places.", &GameSettings::navigation, &NavigationSection::single, &NavigationSingleSection::maxCandidates))
        .Between(1, 32, 12);

    // --- navigation.strategist --------------------------------------------
    add(Field<ValueType::Bool>("navigation.strategist", "navigation.strategist.enabled",
            "Whether the strategist runs above the System 1 pilot.", &GameSettings::navigation, &NavigationSection::strategist, &NavigationStrategistSection::enabled));
    add(Field<ValueType::Bool>("navigation.strategist", "navigation.strategist.fallbackToS1",
            "Fall back to the System 1 pilot alone when the strategist gives nothing usable.", &GameSettings::navigation, &NavigationSection::strategist, &NavigationStrategistSection::fallbackToS1));
    add(Field<ValueType::Int>("navigation.strategist", "navigation.strategist.maxStrategies",
            "Strategies kept per navigation.", &GameSettings::navigation, &NavigationSection::strategist, &NavigationStrategistSection::maxStrategies))
        .Min(1, 1);
    add(Field<ValueType::Int>("navigation.strategist", "navigation.strategist.maxAttempts",
            "Attempts per strategist request: the first call plus retries.", &GameSettings::navigation, &NavigationSection::strategist, &NavigationStrategistSection::maxAttempts))
        .Min(1, 1);
    add(Field<ValueType::Int>("navigation.strategist", "navigation.strategist.maxMonitors",
            "Monitors the strategist may keep running.", &GameSettings::navigation, &NavigationSection::strategist, &NavigationStrategistSection::maxMonitors))
        .Min(0, 0);
    add(Field<ValueType::Float>("navigation.strategist", "navigation.strategist.monitorMinConfidence",
            "Below this confidence a monitor is not acted on.", &GameSettings::navigation, &NavigationSection::strategist, &NavigationStrategistSection::monitorMinConfidence))
        .ClampBetween(0.0, 1.0);
    add(Field<ValueType::Float>("navigation.strategist", "navigation.strategist.timeoutSeconds",
            "Whole-request budget for one strategist call.", &GameSettings::navigation, &NavigationSection::strategist, &NavigationStrategistSection::timeoutSeconds))
        .MinExclusive(0.0, 30.0);
    add(Field<ValueType::Int>("navigation.strategist", "navigation.strategist.observation.localGridRadius",
            "Radius of the local grid in the strategist's observation.", &GameSettings::navigation, &NavigationSection::strategist, &NavigationStrategistSection::observation, &NavigationObservationSection::localGridRadius))
        .Min(1, 1);
    add(Field<ValueType::Int>("navigation.strategist", "navigation.strategist.observation.maxGraphNodes",
            "Nodes kept in the minimal observed graph.", &GameSettings::navigation, &NavigationSection::strategist, &NavigationStrategistSection::observation, &NavigationObservationSection::maxGraphNodes))
        .Min(0, 0);
    add(Field<ValueType::Int>("navigation.strategist", "navigation.strategist.observation.maxGraphEdges",
            "Edges kept in the minimal observed graph.", &GameSettings::navigation, &NavigationSection::strategist, &NavigationStrategistSection::observation, &NavigationObservationSection::maxGraphEdges))
        .Min(0, 0);
    add(Field<ValueType::Int>("navigation.strategist", "navigation.strategist.observation.maxTokens",
            "Completion budget for the observation text.", &GameSettings::navigation, &NavigationSection::strategist, &NavigationStrategistSection::observation, &NavigationObservationSection::maxTokens))
        .Min(0, 0);

    // --- perception -------------------------------------------------------
    add(Field<ValueType::Float>("perception", "perception.range",
            "How far a character can see.", &GameSettings::perception, &PerceptionSection::range))
        .Between(1, 40);
    add(Field<ValueType::Float>("perception", "perception.angleDegrees",
            "Field of view. 360 is all-round; 90 is the forward cone.", &GameSettings::perception, &PerceptionSection::angleDegrees))
        .Between(10, 360);
    add(Field<ValueType::Int>("perception", "perception.radarRadius",
            "The all-round near ring in either mode.", &GameSettings::perception, &PerceptionSection::radarRadius))
        .Between(0, 10);

    // --- regions ----------------------------------------------------------
    add(Field<ValueType::Bool>("regions", "regions.enabled",
            "Whether observed base regions are formed at all.", &GameSettings::regions, &RegionsSection::enabled));
    add(Field<ValueType::Int>("regions", "regions.nearSize",
            "Cell size of the near region grid.", &GameSettings::regions, &RegionsSection::nearSize))
        .Between(1, 64);
    add(Field<ValueType::Int>("regions", "regions.nearRadius",
            "How far the near grid extends.", &GameSettings::regions, &RegionsSection::nearRadius))
        .Between(0, 40);
    add(Field<ValueType::Int>("regions", "regions.midSize",
            "Cell size of the mid region grid.", &GameSettings::regions, &RegionsSection::midSize))
        .Between(1, 128);
    add(Field<ValueType::Int>("regions", "regions.midRadius",
            "How far the mid grid extends.", &GameSettings::regions, &RegionsSection::midRadius))
        .Between(0, 80);
    add(Field<ValueType::Int>("regions", "regions.farSize",
            "Cell size of the far region grid.", &GameSettings::regions, &RegionsSection::farSize))
        .Between(1, 256);
    add(Field<ValueType::Int>("regions", "regions.farRadius",
            "How far the far grid extends. Must be at least midRadius.", &GameSettings::regions, &RegionsSection::farRadius))
        .Between(1, 512);
    add(Field<ValueType::Int>("regions", "regions.maxRegions",
            "Regions remembered per character.", &GameSettings::regions, &RegionsSection::maxRegions))
        .Between(0, 512);
    add(Field<ValueType::Int>("regions", "regions.modelNearCount",
            "Near regions offered to the model in one prompt.", &GameSettings::regions, &RegionsSection::modelNearCount))
        .Between(0, 32);

    // --- chunks -----------------------------------------------------------
    add(Field<ValueType::Bool>("chunks", "chunks.enabled",
            "Whether the agent-facing chunk area card and route strip are built.", &GameSettings::chunks, &ChunksSection::enabled));
    add(Field<ValueType::Int>("chunks", "chunks.size",
            "Cells per chunk side. Read at map load; a change remaps handles.", &GameSettings::chunks, &ChunksSection::size))
        .Between(4, 64, DEFAULT_CHUNK_SIZE);
    add(Field<ValueType::Int>("chunks", "chunks.maxTraversedAreas",
            "Areas listed in one last-leg strip.", &GameSettings::chunks, &ChunksSection::maxTraversedAreas))
        .Between(1, 32, DEFAULT_CHUNK_MAX_TRAVERSED_AREAS);
    add(Field<ValueType::Int>("chunks", "chunks.maxJourneySegments",
            "Journey stretches kept in the prompt.", &GameSettings::chunks, &ChunksSection::maxJourneySegments))
        .Between(1, 64, DEFAULT_CHUNK_MAX_JOURNEY_SEGMENTS);
    add(Field<ValueType::Int>("chunks", "chunks.maxNewAreas",
            "Off-path newly seen areas listed after a leg.", &GameSettings::chunks, &ChunksSection::maxNewAreas))
        .Between(1, 32, DEFAULT_CHUNK_MAX_NEW_AREAS);
    add(Field<ValueType::Int>("chunks", "chunks.maxFeaturesPerArea",
            "Features listed per area in the last-leg strip.", &GameSettings::chunks, &ChunksSection::maxFeaturesPerArea))
        .Between(0, 16, DEFAULT_CHUNK_MAX_FEATURES_PER_AREA);
    add(Field<ValueType::Int>("chunks", "chunks.cardAspects",
            "Surfaces listed in the area card.", &GameSettings::chunks, &ChunksSection::cardAspects))
        .Between(0, 16, DEFAULT_CHUNK_CARD_ASPECTS);
    add(Field<ValueType::Int>("chunks", "chunks.cardObjectGroups",
            "Object groups listed in the area card.", &GameSettings::chunks, &ChunksSection::cardObjectGroups))
        .Between(0, 32, DEFAULT_CHUNK_CARD_OBJECT_GROUPS);
    add(Field<ValueType::Int>("chunks", "chunks.cardPlaces",
            "Places listed in the area card.", &GameSettings::chunks, &ChunksSection::cardPlaces))
        .Between(0, 16, DEFAULT_CHUNK_CARD_PLACES);
    add(Field<ValueType::Bool>("chunks", "chunks.includeCharacters",
            "Whether the area card lists observed characters.", &GameSettings::chunks, &ChunksSection::includeCharacters));

    // --- window -----------------------------------------------------------
    add(Field<ValueType::String>("window", "window.title",
            "Window title.", &GameSettings::window, &WindowSection::title))
        .Restart();
    add(Field<ValueType::Int>("window", "window.width",
            "Initial window width. Also drives the camera's fit-to-map zoom reference.", &GameSettings::window, &WindowSection::width))
        .Restart();
    add(Field<ValueType::Int>("window", "window.height",
            "Initial window height. Also drives the camera's fit-to-map zoom reference.", &GameSettings::window, &WindowSection::height))
        .Restart();
    add(Field<ValueType::Bool>("window", "window.fullscreen",
            "False keeps the windowed size above.", &GameSettings::window, &WindowSection::fullscreen))
        .Restart();
    add(Field<ValueType::Bool>("window", "window.vsync",
            "Request vertical sync.", &GameSettings::window, &WindowSection::vsync))
        .Restart();
    add(Field<ValueType::Int>("window", "window.targetFPS",
            "Frame rate cap.", &GameSettings::window, &WindowSection::targetFPS))
        .Restart();
    add(Field<ValueType::Int>("window", "window.monitor",
            "-1 picks a monitor the way the game always has. 0 or more forces that index.", &GameSettings::window, &WindowSection::monitor))
        .Restart();

    // --- render -----------------------------------------------------------
    add(Field<ValueType::String>("render", "render.fontPath",
            "The bitmap font file.", &GameSettings::render, &RenderSection::fontPath))
        .Restart();
    add(Field<ValueType::Int>("render", "render.fontSize",
            "Font size in pixels.", &GameSettings::render, &RenderSection::fontSize))
        .Restart();
    add(Field<ValueType::EnumString>("render", "render.cameraMode",
            "follow keeps the camera on the player; free leaves it to the arrow keys.", &GameSettings::render, &RenderSection::cameraMode))
        .Enum({"follow", "free"}, false)
        .Lower()
        .Restart();
    add(Field<ValueType::Float>("render", "render.cameraZoom",
            "0 fits the map to the window. Any other value is the starting zoom.", &GameSettings::render, &RenderSection::cameraZoom))
        .Restart();

    // --- player -----------------------------------------------------------
    add(Field<ValueType::String>("player", "player.name",
            "The player entity's name.", &GameSettings::player, &PlayerSection::name))
        .Restart();
    add(Field<ValueType::Int>("player", "player.startX",
            "Starting tile. Out-of-bounds or wall positions are relocated to the nearest walkable tile.", &GameSettings::player, &PlayerSection::startX))
        .Restart();
    add(Field<ValueType::Int>("player", "player.startY",
            "Starting tile.", &GameSettings::player, &PlayerSection::startY))
        .Restart();
    add(Field<ValueType::String>("player", "player.race",
            "Race the player's body slots come from. Empty means the human default.", &GameSettings::player, &PlayerSection::race))
        .Restart();
    add(Field<ValueType::EnumString>("player", "player.actorSprite",
            "Actor sprite the player is drawn with. Empty uses the race's own look, then the ASCII art.", &GameSettings::player, &PlayerSection::actorSprite))
        .ActorSpriteSource();
    add(Field<ValueType::Float>("player", "player.maxSpeed",
            "Top movement speed in pixels per second. Holding a direction adds velocity and the movement system clamps it to this.", &GameSettings::player, &PlayerSection::maxSpeed))
        .Min(0.0, DEFAULT_MAXSPEED);
    add(Field<ValueType::String>("player", "player.portrait",
            "PortraitRegistry id for the player's HUD portrait. Empty means the race's default, then the unknown sprite.", &GameSettings::player, &PlayerSection::portrait))
        .Restart();

    // --- combat -----------------------------------------------------------
    add(Field<ValueType::Bool>("combat", "combat.instantSkillshots",
            "Fire aimed skills at the cursor immediately instead of showing their aim indicator first.", &GameSettings::combat, &CombatSection::instantSkillshots));

    // --- world ------------------------------------------------------------
    add(Field<ValueType::UInt64>("world", "world.seed",
            "0 seeds from the clock, so every run differs. Any other value makes the run reproducible.", &GameSettings::world, &WorldSection::seed))
        .Min(0, 0)
        .Restart();

    // --- content ----------------------------------------------------------
    add(Field<ValueType::String>("content", "content.objects",
            "Object template directory.", &GameSettings::content, &ContentSection::objects))
        .Restart();
    add(Field<ValueType::String>("content", "content.recipes",
            "Recipe registry directory.", &GameSettings::content, &ContentSection::recipes))
        .Restart();
    add(Field<ValueType::String>("content", "content.stats",
            "Stat registry directory.", &GameSettings::content, &ContentSection::stats))
        .Restart();
    add(Field<ValueType::String>("content", "content.skills",
            "Skill registry directory.", &GameSettings::content, &ContentSection::skills))
        .Restart();
    add(Field<ValueType::String>("content", "content.items",
            "Item registry directory.", &GameSettings::content, &ContentSection::items))
        .Restart();
    add(Field<ValueType::String>("content", "content.actorSprites",
            "Actor-sprite catalogue: art for beings, keyed by sprite id. Not a definition of a creature.", &GameSettings::content, &ContentSection::actorSprites))
        .Restart();
    add(Field<ValueType::String>("content", "content.rigs",
            "Actor-rig catalogue: layered humanoid rigs (parts plus worn equipment). The rigged counterpart of an actor sprite.", &GameSettings::content, &ContentSection::rigs))
        .Restart();
    add(Field<ValueType::String>("content", "content.generatedRigs",
            "Generated overlay for Designer-authored actor rigs. Empty means a generated rig lives for this run only.", &GameSettings::content, &ContentSection::generatedRigs))
        .Restart();
    add(Field<ValueType::String>("content", "content.generatedItems",
            "Generated overlay for Designer-authored equipment. Loaded after data/items; empty means a generated item lives for this run only.", &GameSettings::content, &ContentSection::generatedItems))
        .Restart();
    add(Field<ValueType::String>("content", "content.equipmentPromptFile",
            "The equipment designer's instructions: the worn-item reply format and which slots a rig can show.", &GameSettings::content, &ContentSection::equipmentPromptFile))
        .Restart();
    add(Field<ValueType::String>("content", "content.rigPromptFile",
            "The rig designer's instructions: the humanoid frame and the per-part reply format.", &GameSettings::content, &ContentSection::rigPromptFile))
        .Restart();
    add(Field<ValueType::String>("content", "content.races",
            "Race registry directory.", &GameSettings::content, &ContentSection::races))
        .Restart();
    add(Field<ValueType::String>("content", "content.npcs",
            "NPC template directory: named starting configurations a map spawns by name and overrides.", &GameSettings::content, &ContentSection::npcs))
        .Restart();
    add(Field<ValueType::String>("content", "content.brains",
            "Brain template directory: background, goals and model profile for agents. A template with no brain is a body with no agent.", &GameSettings::content, &ContentSection::brains))
        .Restart();
    add(Field<ValueType::String>("content", "content.portraits",
            "Portrait registry directory: one JSON per portrait, mapping an id to a sprite path or convention id.", &GameSettings::content, &ContentSection::portraits))
        .Restart();
    add(Field<ValueType::String>("content", "content.projectiles",
            "Projectile registry directory: one JSON per flying object, with its sprite, speed and size.", &GameSettings::content, &ContentSection::projectiles))
        .Restart();
    add(Field<ValueType::String>("content", "content.skillshots",
            "Skillshot registry directory: one JSON per aimed delivery, with its sprite, path, speed, width and length.", &GameSettings::content, &ContentSection::skillshots))
        .Restart();
    add(Field<ValueType::String>("content", "content.places",
            "The place taxonomy file: which categories form places, their seed thresholds, link radii and split rule.", &GameSettings::content, &ContentSection::places))
        .Restart();
    add(Field<ValueType::String>("content", "content.tiles",
            "The hand-authored tile catalog directory. Agent-created tiles go to generatedTiles instead.", &GameSettings::content, &ContentSection::tiles))
        .Restart();
    add(Field<ValueType::String>("content", "content.generatedTiles",
            "Generated overlay for agent-created tiles, loaded additively on top of the catalog.", &GameSettings::content, &ContentSection::generatedTiles))
        .Restart();
    add(Field<ValueType::String>("content", "content.editor",
            "Where the Content Editor saves its overrides (tiles/ and objects/ beneath it). Loaded last, so an edit wins over the catalog and the generated overlay; the hand-authored files are never rewritten.", &GameSettings::content, &ContentSection::editor))
        .Restart();
    add(Field<ValueType::String>("content", "content.tilePromptFile",
            "The tile designer's instructions. Empty falls back to the generated vocabulary alone.", &GameSettings::content, &ContentSection::tilePromptFile))
        // The old spelling. It named the *window* that read the file, which now designs
        // buildings too; the key names the domain instead. Read, never written, so an
        // existing settings.json keeps working without the new name being advertised.
        .Alias("content.tileDesignerPromptFile")
        .Restart();
    add(Field<ValueType::String>("content", "content.objectPromptFile",
            "The object designer's instructions.", &GameSettings::content, &ContentSection::objectPromptFile))
        .Alias("content.objectDesignerPromptFile")
        .Restart();
    add(Field<ValueType::String>("content", "content.mapPromptFile",
            "The map designer's instructions: the compact legend-and-rows reply format and the rules a reply is validated by.", &GameSettings::content, &ContentSection::mapPromptFile))
        .Restart();
    add(Field<ValueType::String>("content", "content.generatedObjects",
            "Generated overlay for agent-created objects. Empty means an accepted object lives for this run only.", &GameSettings::content, &ContentSection::generatedObjects))
        .Restart();
    add(Field<ValueType::String>("content", "content.spriteContractFile",
            "The reply contract every kind's prompt is built from: format, refusal protocol and prohibitions.", &GameSettings::content, &ContentSection::spriteContractFile))
        .Restart();
    add(Field<ValueType::String>("content", "content.spriteDSLPromptFile",
            "The system prompt for the turn that writes a pixel document for an accepted definition.", &GameSettings::content, &ContentSection::spriteDSLPromptFile))
        .Restart();
    add(Field<ValueType::String>("content", "content.rewritePromptFile",
            "The prompt-improver's instructions, shared by every window that offers the Improve button.", &GameSettings::content, &ContentSection::rewritePromptFile))
        .Restart();
    add(Field<ValueType::String>("content", "content.blueprints",
            "Building designs. Optional: with none, the placement window lists nothing.", &GameSettings::content, &ContentSection::blueprints))
        .Restart();

    // --- art --------------------------------------------------------------
    add(Field<ValueType::String>("art", "art.pack",
            "Art pack directory. Empty means no art pack, and the generated layer is not consulted.", &GameSettings::art, &ArtSection::pack))
        .Restart();
    add(Field<ValueType::String>("art", "art.generated",
            "Where this pack's generated sprites live directly. Empty derives it from the pack's location.", &GameSettings::art, &ArtSection::generated))
        .Restart();
    add(Field<ValueType::Bool>("art.generation", "art.generation.enabled",
            "Turn live art generation on. Off by default because it spends money unattended.", &GameSettings::art, &ArtSection::generation, &GenerationSection::enabled));
    add(Field<ValueType::String>("art.generation", "art.generation.command",
            "The command that turns a worklist JSON into sprites. Empty uses the built-in command.", &GameSettings::art, &ArtSection::generation, &GenerationSection::command))
        .Restart();
    add(Field<ValueType::Float>("art.generation", "art.generation.batchDelaySeconds",
            "How long to collect requests before spending one request on them.", &GameSettings::art, &ArtSection::generation, &GenerationSection::batchDelaySeconds));
    add(Field<ValueType::Int>("art.generation", "art.generation.maxRequestsPerSession",
            "Hard ceiling on generation requests for the session.", &GameSettings::art, &ArtSection::generation, &GenerationSection::maxRequestsPerSession))
        .Min(1, 1)
        .Restart();
    add(Field<ValueType::Int>("art.generation", "art.generation.maxCellsPerRequest",
            "How many sprites may share one image request.", &GameSettings::art, &ArtSection::generation, &GenerationSection::maxCellsPerRequest))
        .Min(1, 1);
    add(Field<ValueType::String>("art.generation", "art.generation.workDirectory",
            "Where the worklist and raw sheets go. Empty uses data/generated/work.", &GameSettings::art, &ArtSection::generation, &GenerationSection::workDirectory))
        .Restart();
    add(Field<ValueType::EnumString>("art.generation", "art.generation.source",
            "Which producer makes a sprite: the sheet API behind the command, or the pixel DSL compiler.", &GameSettings::art, &ArtSection::generation, &GenerationSection::source))
        .Enum({"image", "dsl"});
    add(Field<ValueType::Int>("art.generation", "art.generation.dslAttempts",
            "Compile/describe attempts a designer agent gets when the DSL is the source.", &GameSettings::art, &ArtSection::generation, &GenerationSection::dslAttempts))
        .Min(1, 1);

    // --- debug ------------------------------------------------------------
    add(Field<ValueType::String>("debug", "debug.windowStateFile",
            "Where debug-window visibility is written on shutdown and read on start.", &GameSettings::debug, &DebugSection::windowStateFile))
        .Restart();
    add(Field<ValueType::Bool>("debug", "debug.restServer",
            "Starts flecs' unauthenticated ECS REST server. Turn off on a shared machine.", &GameSettings::debug, &DebugSection::restServer))
        .Restart();
    add(Field<ValueType::Int>("debug", "debug.logMaxEntries",
            "Entries the Debug Log keeps before dropping the oldest.", &GameSettings::debug, &DebugSection::logMaxEntries))
        .Restart();

    return out;
  }();
  return fields;
}

// ---------------------------------------------------------------------------
// Profiles
// ---------------------------------------------------------------------------

const CollectionSpec &Profiles() {
  static const CollectionSpec profiles = [] {
    CollectionSpec spec;
    spec.section = "system1.profiles";
    spec.path = "system1.profiles";
    spec.title = "system1.profiles";
    const SectionSpec *section = FindSection("system1.profiles");
    spec.doc = section ? section->doc : "";
    auto add = [&spec](CollectionFieldSpec field) -> CollectionFieldSpec & {
      spec.fields.push_back(std::move(field));
      return spec.fields.back();
    };
    add(ElementField<ValueType::String>("id", "id",
        "Identifier used to select this profile: system1.activeProfile, the --system1-profile flag and every status line. Required.",
        &GameSettings::SystemOneProfileSection::id));
    add(ElementField<ValueType::String>("profile", "profile",
        "Name of an ai.profiles entry to take the model settings from. Its preset and fields are applied before this entry's own model block, so calibration and timeout can sit on top of a shared backend.",
        &GameSettings::SystemOneProfileSection::profile));
    add(ElementField<ValueType::String>("label", "label",
        "Display name. Falls back to the id.",
        &GameSettings::SystemOneProfileSection::label));
    add(ElementField<ValueType::String>("preset", "preset",
        "hosted_jev or local_kev: seeds endpoint, model, key and timeout before the explicit fields below are applied. Empty means no seed.",
        &GameSettings::SystemOneProfileSection::preset));
    add(ElementField<ValueType::EnumString>("model.type", "type",
        "Backend for this profile's model block.",
        &GameSettings::SystemOneProfileSection::model, &ModelSection::type))
        .enumValues = {"ollama", "gemini", "openrouter"};
    add(ElementField<ValueType::String>("model.model", "model",
        "Model id. Empty means use the preset's.",
        &GameSettings::SystemOneProfileSection::model, &ModelSection::model))
        .emptyDefault = true;
    add(ElementField<ValueType::String>("model.apiKey", "apiKey",
        "Key for this profile's model block. Empty means use the preset's or the main key.",
        &GameSettings::SystemOneProfileSection::model, &ModelSection::apiKey))
        .emptyDefault = true;
    spec.fields.back().secret = true;
    add(ElementField<ValueType::String>("model.endpoint", "endpoint",
        "Endpoint. Empty means use the preset's.",
        &GameSettings::SystemOneProfileSection::model, &ModelSection::endpoint))
        .emptyDefault = true;
    add(ElementField<ValueType::String>("model.keepAlive", "keepAlive",
        "Ollama keep-alive for this profile.",
        &GameSettings::SystemOneProfileSection::model, &ModelSection::keepAlive));
    add(PresenceElementField<ValueType::Double>("model.temperature", "temperature",
        "Sampling temperature. Absent leaves the preset's in charge.",
        &ModelSection::hasTemperature, &GameSettings::SystemOneProfileSection::model, &ModelSection::temperature));
    add(PresenceElementField<ValueType::Int>("model.maxTokens", "maxTokens",
        "Completion budget. Absent follows the preset.",
        &ModelSection::hasMaxTokens, &GameSettings::SystemOneProfileSection::model, &ModelSection::maxTokens));
    add(PresenceElementField<ValueType::Json>("model.provider", "provider",
        "OpenRouter provider routing.",
        &ModelSection::hasProvider, &GameSettings::SystemOneProfileSection::model, &ModelSection::provider));
    add(ElementField<ValueType::Int64>("timeoutSeconds", "timeoutSeconds",
        "Whole-request budget. 0 follows the preset, then the endpoint rule (120s local, 30s hosted).",
        &GameSettings::SystemOneProfileSection::timeoutSeconds))
        .range = [] {
      RangeRule rule;
      rule.min = 0;
      rule.resetTo = 0;
      return rule;
    }();
    add(ElementField<ValueType::Float>("minConfidence", "minConfidence",
        "Below this, a verdict is reported as unsure rather than acted on. 0 disables the check. Calibration is per model, which is why it lives here.",
        &GameSettings::SystemOneProfileSection::minConfidence));
    return spec;
  }();
  return profiles;
}

// ---------------------------------------------------------------------------
// Command-line overrides
// ---------------------------------------------------------------------------

namespace {

struct CommandLineOverride {
  std::string path;
  nlohmann::json value;
};

std::vector<CommandLineOverride> &CommandLineOverrideList() {
  static std::vector<CommandLineOverride> overrides;
  return overrides;
}

}  // namespace

void SetCommandLineOverride(const std::string &path,
                            const nlohmann::json &value) {
  std::vector<CommandLineOverride> &overrides = CommandLineOverrideList();
  for (CommandLineOverride &entry : overrides) {
    if (entry.path == path) {
      entry.value = value;
      return;
    }
  }
  overrides.push_back({path, value});
}

bool IsCommandLineOverride(const std::string &path) {
  for (const CommandLineOverride &entry : CommandLineOverrideList()) {
    if (entry.path == path) return true;
  }
  return false;
}

std::size_t ApplyCommandLineOverrides() {
  std::size_t applied = 0;
  for (const CommandLineOverride &entry : CommandLineOverrideList()) {
    const FieldSpec *field = FindField(entry.path);
    if (field == nullptr) {
      continue;
    }
    std::string message;
    // A wrong-typed command-line value is a programming error in main(), not a
    // user's; it is reported and skipped rather than corrected silently.
    if (ApplyJson(*field, MutableSettings(), entry.value, message)) {
      ++applied;
    } else {
      std::cerr << message;
    }
  }
  return applied;
}

// ---------------------------------------------------------------------------
// Lookup
// ---------------------------------------------------------------------------

const FieldSpec *FindField(const std::string &path) {
  for (const FieldSpec &field : Fields()) {
    if (field.path == path) return &field;
  }
  return nullptr;
}

const SectionSpec *FindSection(const std::string &key) {
  for (const SectionSpec &section : Sections()) {
    if (section.key == key) return &section;
  }
  return nullptr;
}

const std::vector<AIProfileFieldSpec> &AIProfileFields() {
  static const std::vector<AIProfileFieldSpec> fields = {
      {"label", ValueType::String, "label", "Display name in the pickers.", {}, false, std::nullopt},
      {"kind", ValueType::EnumString, "kind", "What the model is for. The picker for a block only offers matching kinds; a hand-written mismatch is reported and still used.", {"s1", "s2", "vision"}, false, std::nullopt},
      {"preset", ValueType::String, "preset", "S1 only: hosted_jev or local_kev seeds endpoint, model and key before the fields below.", {}, false, std::nullopt},
      {"model.type", ValueType::EnumString, "type", "Backend: ollama, gemini or openrouter. Anything else is treated as ollama.", {"ollama", "gemini", "openrouter"}, false, std::nullopt},
      {"model.model", ValueType::String, "model", "Model id passed to the backend.", {}, false, std::nullopt},
      {"model.apiKey", ValueType::String, "apiKey", "Key for gemini and openrouter. Empty falls back to the block's own key and then the main key.", {}, false, std::nullopt},
      {"model.endpoint", ValueType::String, "endpoint", "Ollama endpoint. Ignored by the other backends.", {}, false, std::nullopt},
      {"model.keepAlive", ValueType::String, "keepAlive", "How long Ollama keeps the model resident, e.g. 30m.", {}, false, std::nullopt},
      {"model.temperature", ValueType::Double, "temperature", "Sampling temperature. Absent leaves the provider's own default in charge.", {}, true, std::nullopt},
      {"model.maxTokens", ValueType::Int, "maxTokens", "Completion budget. Absent leaves the provider's own default in charge.", {}, true, std::nullopt},
      {"model.provider", ValueType::Json, "provider", "OpenRouter provider routing object, passed through verbatim.", {}, true, std::nullopt},
  };
  return fields;
}

const SectionSpec *FindAISection() { return FindSection("ai.profiles"); }

void *AIProfileLeaf(GameSettings::AIProfileSection &profile,
                    const std::string &path) {
  if (path == "label") return &profile.label;
  if (path == "kind") return &profile.kind;
  if (path == "preset") return &profile.preset;
  if (path == "model.type") return &profile.model.type;
  if (path == "model.model") return &profile.model.model;
  if (path == "model.apiKey") return &profile.model.apiKey;
  if (path == "model.endpoint") return &profile.model.endpoint;
  if (path == "model.keepAlive") return &profile.model.keepAlive;
  if (path == "model.temperature") return &profile.model.temperature;
  if (path == "model.maxTokens") return &profile.model.maxTokens;
  if (path == "model.provider") return &profile.model.provider;
  return nullptr;
}

bool *AIProfilePresence(GameSettings::AIProfileSection &profile,
                        const std::string &path) {
  if (path == "model.temperature") return &profile.model.hasTemperature;
  if (path == "model.maxTokens") return &profile.model.hasMaxTokens;
  if (path == "model.provider") return &profile.model.hasProvider;
  return nullptr;
}

nlohmann::json AIProfileDefaultJson(const AIProfileFieldSpec &field) {
  static GameSettings::AIProfileSection defaults;
  if (field.presence) {
    const bool *flag = AIProfilePresence(defaults, field.path);
    if (flag != nullptr && !*flag) {
      return nullptr;
    }
  }
  void *leaf = AIProfileLeaf(defaults, field.path);
  return leaf == nullptr ? nullptr : ReadLeaf(leaf, field.type);
}

const CollectionFieldSpec *FindProfileField(const std::string &path) {
  for (const CollectionFieldSpec &field : Profiles().fields) {
    if (field.path == path) return &field;
  }
  return nullptr;
}

}  // namespace SettingsSchema
