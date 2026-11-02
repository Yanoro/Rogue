#pragma once

// One description per value settings.json can carry.
//
// Before this existed there were four descriptions of the same field: the
// member in Settings.h, the `value(...)` read in LoadGameSettings, the table in
// SETTINGS.md, and — for whoever wanted to change it — nothing at all. They
// could disagree, and did: `content.places` was read by the place registry but
// never loaded from the file, so the key silently did nothing.
//
// This table is now the single description. It drives:
//
//   * loading (LoadGameSettings reads through Fields()/Profiles()),
//   * the settings window (every row is one FieldSpec),
//   * the override file writer (dotted path + type + default),
//   * the generated SETTINGS.md reference (doc text + type + default).
//
// Adding a setting is one line here plus the member in Settings.h. A field that
// is declared with the wrong ValueType for its member does not compile.
//
// Terminology:
//
//   * A *field* is one scalar leaf, addressed by its dotted JSON path
//     ("model.maxTokens"). Its `section` names the table it belongs to in the
//     reference and the tree node it appears under in the window; it is not
//     necessarily a prefix of `path` (the model blocks are documented under the
//     section that owns them).
//
//   * A *presence flag* is the `hasTemperature` / `hasMaxTokens` /
//     `hasProvider` / `hasTimeoutSeconds` boolean that keeps "absent" distinct
//     from "set to the default". A field with one is written back as a pair.
//
//   * A *collection* is the one non-scalar setting, `system1.profiles`, whose
//     elements are described by CollectionFieldSpec.
//
//   * `persisted == false` marks a member that is derived at load (the NPC
//     prompt template text) or supplied on the command line (the System 1
//     profile override). The window shows it; the loader and writer skip it.

#include "Settings.h"

#include <cstdint>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace SettingsSchema {

enum class ValueType {
  Bool,
  Int,
  Int64,   // long
  UInt64,  // uint64_t
  Float,
  Double,
  String,
  EnumString,
  Json,
};

// When a changed value can take effect. Advisory metadata for the window: the
// value is written into the running settings object either way, but `Restart`
// means the code that consumed it at startup will not read it again (window
// creation, the AI backends, the content registries, the map), so the badge
// says a restart is what makes it real.
enum class Apply { Live, Restart };

// A bound on a value, and what to do with a value outside it.
//
// `clamp` false (the default) reproduces what the loader has always done: report
// the value, replace it with `resetTo` (or the member's default when that is
// unset) and carry on. `clamp` true silently moves the value to the bound, which
// is what the 0..1 probability and confidence knobs do.
struct RangeRule {
  std::optional<double> min;
  std::optional<double> max;
  bool minInclusive = true;
  bool maxInclusive = true;
  bool clamp = false;
  // Replacement for a rejected value. Absent means "the member's default".
  std::optional<double> resetTo;
  // Empty means the loader composes "Settings: <path> must be <bounds>; using
  // <replacement>." Explicit text exists where the composed sentence would lose
  // the reason (a clipped voice, a clock seed).
  std::string lowMessage;
  std::string highMessage;
};

struct FieldSpec {
  std::string section;
  std::string path;
  ValueType type = ValueType::String;
  // Returns a pointer to the live member. Type-erased so one table can describe
  // members nested at any depth; `type` says what to cast it to.
  std::function<void *(GameSettings &)> access;
  // Null unless this field is meaningful only when present.
  std::function<bool *(GameSettings &)> presence;
  std::string doc;
  // EnumString only; the first entry is the default and the fallback.
  std::vector<std::string> enumValues;
  // False keeps an unrecognised value instead of correcting it (the backend
  // `type` fields pass unknown values through to the backend unchanged).
  bool strictEnum = true;
  // A dynamic list the window builds at draw time instead of a fixed
  // `enumValues`. "profile:s2" means "every ai.profiles entry of kind s2", which
  // is how a model block's `profile` key becomes a picker. Empty means static.
  std::string enumSource;
  // Overrides the member default for reading, display and reset. Needed where
  // the member's default is a *chat* backend default that must not leak into a
  // sub-object ("system1.model.model": llama3 -> empty means "inherit").
  std::optional<std::string> stringDefault;
  // Extra accepted spellings. The canonical one wins when both are present.
  std::vector<std::string> aliases;
  bool lowercase = false;
  bool secret = false;
  bool persisted = true;
  Apply apply = Apply::Live;
  std::optional<RangeRule> range;

  // --- chainable modifiers, for the table below ---------------------------
  FieldSpec &Enum(std::initializer_list<const char *> values, bool strict = true);
  FieldSpec &ProfileSource(const char *kind);
  // Marks a reference into ActorSpriteRegistry, so the window draws the sprite
  // picker instead of a text field.
  FieldSpec &ActorSpriteSource();
  FieldSpec &Default(std::string value);
  FieldSpec &Alias(const char *spelling);
  FieldSpec &Lower();
  FieldSpec &Secret();
  FieldSpec &Restart();
  FieldSpec &Derived();
  FieldSpec &Between(double low, double high,
                     std::optional<double> resetTo = std::nullopt);
  FieldSpec &Min(double low, std::optional<double> resetTo = std::nullopt);
  FieldSpec &MinExclusive(double low, std::optional<double> resetTo = std::nullopt);
  FieldSpec &Max(double high, std::optional<double> resetTo = std::nullopt);
  FieldSpec &ClampBetween(double low, double high);
  FieldSpec &LowMessage(const char *text);
  FieldSpec &HighMessage(const char *text);

  // The final path segment, used as the window's row label.
  std::string Label() const;
};

struct SectionSpec {
  std::string key;     // "designer.tts" -- also the tree path
  std::string title;   // "designer.tts"
  std::string doc;     // one paragraph, rendered under the heading
  std::string prose;   // optional long-form notes, rendered after the table
};

// One member of a system1.profiles entry.
struct CollectionFieldSpec {
  std::string path;  // relative to the entry: "model.model"
  ValueType type = ValueType::String;
  std::function<void *(GameSettings::SystemOneProfileSection &)> access;
  std::function<bool *(GameSettings::SystemOneProfileSection &)> presence;
  std::string label;
  std::string doc;
  std::vector<std::string> enumValues;
  bool strictEnum = false;
  // Read as empty when the key is absent, instead of keeping the member
  // default. This is what stops a profile that names only a preset from
  // inheriting the chat backend's endpoint and model id.
  bool emptyDefault = false;
  bool lowercase = false;
  bool secret = false;
  std::optional<RangeRule> range;
};

// One field of an `ai.profiles` entry, relative to the entry. Kept separate from
// CollectionFieldSpec because the registry is an object keyed by id and its
// elements are a different type; the shape is the same, and the window and the
// reference both describe it from here.
struct AIProfileFieldSpec {
  std::string path;  // "label", "kind", "model.type", ...
  ValueType type = ValueType::String;
  std::string label;
  std::string doc;
  std::vector<std::string> enumValues;
  bool presence = false;
  std::optional<RangeRule> range;
};

struct CollectionSpec {
  std::string section;
  std::string path;
  std::string title;
  std::string doc;
  Apply apply = Apply::Restart;
  std::vector<CollectionFieldSpec> fields;
};

// The table. Stable for the life of the process; built on first use.
const std::vector<SectionSpec> &Sections();
const std::vector<FieldSpec> &Fields();
const CollectionSpec &Profiles();

// The named AI backends (`ai.profiles`): the same shape as a model block plus
// label/kind/preset, selected by name from any model block's `profile` key.
const std::vector<AIProfileFieldSpec> &AIProfileFields();
const SectionSpec *FindAISection();

// The member one profile field names, and the presence flag guarding it. A path
// switch rather than a member pointer: an ai.profiles entry is not a GameSettings
// member, and keeping the switch beside the field list is what keeps the editor
// and the reference data-driven.
void *AIProfileLeaf(GameSettings::AIProfileSection &profile,
                    const std::string &path);
bool *AIProfilePresence(GameSettings::AIProfileSection &profile,
                        const std::string &path);
// The value a fresh profile field has, or null for an absent presence field.
nlohmann::json AIProfileDefaultJson(const AIProfileFieldSpec &field);

const FieldSpec *FindField(const std::string &path);
const SectionSpec *FindSection(const std::string &key);
const CollectionFieldSpec *FindProfileField(const std::string &path);

// --- JSON paths -----------------------------------------------------------
//
// One spelling of "walk to / create / remove a dotted path", shared by the
// loader, the profiles reader and the override writer. A non-object on the way
// down reads as absent rather than throwing, which is how the loader's
// Section() helper has always treated a misplaced section.

std::vector<std::string> SplitPath(const std::string &path);

// Templated on the JSON type because the override writer keeps the file's key
// order (nlohmann::ordered_json) while the loader reads a plain nlohmann::json.
template <typename Json>
const Json *JsonAt(const Json &root, const std::string &path) {
  const std::vector<std::string> parts = SplitPath(path);
  if (parts.empty()) return nullptr;
  const Json *node = &root;
  for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
    if (!node->is_object()) return nullptr;
    const auto it = node->find(parts[i]);
    if (it == node->end() || !it->is_object()) return nullptr;
    node = &*it;
  }
  if (!node->is_object()) return nullptr;
  const auto it = node->find(parts.back());
  return it == node->end() ? nullptr : &*it;
}

template <typename Json, typename Value>
void SetJsonAt(Json &root, const std::string &path, const Value &value) {
  const std::vector<std::string> parts = SplitPath(path);
  if (parts.empty()) return;
  Json *node = &root;
  for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
    if (!node->is_object()) {
      *node = Json::object();
    }
    Json &next = (*node)[parts[i]];
    if (!next.is_object()) {
      next = Json::object();
    }
    node = &next;
  }
  if (!node->is_object()) {
    *node = Json::object();
  }
  (*node)[parts.back()] = value;
}

// Removes the leaf and then any parent object the removal emptied, deepest
// first, so a reset does not leave `"model": {}` shells behind.
template <typename Json>
bool EraseJsonAt(Json &root, const std::string &path) {
  const std::vector<std::string> parts = SplitPath(path);
  if (parts.empty()) return false;
  std::vector<Json *> chain;
  Json *node = &root;
  for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
    if (!node->is_object()) return false;
    const auto it = node->find(parts[i]);
    if (it == node->end() || !it->is_object()) return false;
    chain.push_back(node);
    node = &*it;
  }
  if (!node->is_object()) return false;
  const std::size_t erased = node->erase(parts.back());
  if (erased == 0) return false;
  for (std::size_t i = chain.size(); i-- > 0;) {
    Json &parent = *chain[i];
    const auto it = parent.find(parts[i]);
    if (it != parent.end() && it->is_object() && it->empty()) {
      parent.erase(parts[i]);
    }
  }
  return true;
}

// --- command-line overrides -----------------------------------------------
//
// main() takes a few settings from its arguments: a map path, --art-pack and
// --system1-profile. Those values are not in any file, so the window could not
// tell them apart from a file value -- and re-reading the file (Reload, Reset)
// would silently undo them, because the loader starts from GameSettings{} and
// only the file is consulted.
//
// main() registers each one here instead of writing the member directly, and the
// loader re-applies the registry at the end of every load. A row backed by one
// shows `cli` in the window, so it is clear why editing settings.json's matching
// key changes nothing for this run.

void SetCommandLineOverride(const std::string &path,
                            const nlohmann::json &value);
bool IsCommandLineOverride(const std::string &path);
// Writes every registered override into the running settings. Returns how many
// were applied; called by LoadGameSettings and once by main() after registering.
std::size_t ApplyCommandLineOverrides();

// --- value access ---------------------------------------------------------

// The value a field has when nothing supplies it. For a field with a presence
// flag this is the flag's default (false = absent), which is why the reference
// and the window show `*absent*` rather than the member's value.
nlohmann::json DefaultJson(const FieldSpec &field);
nlohmann::json CurrentJson(const FieldSpec &field, const GameSettings &settings);

// Writes `value` into the member, reporting the same coercion and range/enum
// correction the loader applies. Returns false only when the JSON cannot be
// converted to the field's type; a value outside a range is corrected and
// reported through `message` with a non-false return.
bool ApplyJson(const FieldSpec &field, GameSettings &settings,
               const nlohmann::json &value, std::string &message);

// One-line description used by the window ("int 3..12", "string", ...).
std::string TypeDescription(const FieldSpec &field);

// --- profiles -------------------------------------------------------------

nlohmann::json ProfileToJson(const GameSettings::SystemOneProfileSection &profile);

// The value a profile field has when a new entry is created. `emptyDefault`
// fields report the empty string, which is the state the loader starts them in.
nlohmann::json ProfileDefaultJson(const CollectionFieldSpec &field);
GameSettings::SystemOneProfileSection ProfileFromJson(const nlohmann::json &entry,
                                                      std::string &error,
                                                      bool &usable);

// --- documentation --------------------------------------------------------

// The whole SETTINGS.md reference, generated from this table.
std::string MarkdownReference();

}  // namespace SettingsSchema

// --- declaration helpers (template, used only by SettingsSchema.cpp) -------
namespace SettingsSchema {
namespace detail {

template <typename T>
struct TypeTag;

template <>
struct TypeTag<bool> {
  static constexpr ValueType value = ValueType::Bool;
};
template <>
struct TypeTag<int> {
  static constexpr ValueType value = ValueType::Int;
};
template <>
struct TypeTag<long> {
  static constexpr ValueType value = ValueType::Int64;
};
template <>
struct TypeTag<unsigned long> {
  static constexpr ValueType value = ValueType::UInt64;
};
template <>
struct TypeTag<float> {
  static constexpr ValueType value = ValueType::Float;
};
template <>
struct TypeTag<double> {
  static constexpr ValueType value = ValueType::Double;
};
template <>
struct TypeTag<std::string> {
  static constexpr ValueType value = ValueType::String;
};
template <>
struct TypeTag<nlohmann::json> {
  static constexpr ValueType value = ValueType::Json;
};

// A string member may be declared as a plain string or as an enum; nothing else
// has two spellings.
constexpr bool TypeMatches(ValueType member, ValueType declared) {
  if (member == ValueType::String) {
    return declared == ValueType::String || declared == ValueType::EnumString;
  }
  return member == declared;
}

// Walks a member-pointer chain of any depth. The recursion is what lets the
// table name a leaf four levels down without an overload per depth.
template <typename Obj, typename T>
T *Walk(Obj &object, T Obj::*leaf) {
  return &(object.*leaf);
}

template <typename Obj, typename Mid, typename... Rest>
auto *Walk(Obj &object, Mid Obj::*step, Rest... rest) {
  return Walk(object.*step, rest...);
}

// The object that owns the last member of the chain, which is where a presence
// flag lives.
template <typename Obj, typename T>
Obj *OwnerOf(Obj &object, T Obj::* /*leaf*/) {
  return &object;
}

template <typename Obj, typename Mid, typename... Rest>
auto *OwnerOf(Obj &object, Mid Obj::*step, Rest... rest) {
  return OwnerOf(object.*step, rest...);
}

}  // namespace detail

// A scalar field. The ValueType is a template parameter so the check that it
// matches the member is a compile-time one.
template <ValueType Type, typename... Path>
FieldSpec Field(const char *section, const char *path, const char *doc,
                Path... members) {
  FieldSpec field;
  field.section = section;
  field.path = path;
  field.type = Type;
  field.doc = doc;
  field.access = [members...](GameSettings &settings) -> void * {
    auto *leaf = detail::Walk(settings, members...);
    static_assert(
        detail::TypeMatches(detail::TypeTag<std::remove_pointer_t<
                                decltype(leaf)>>::value,
                            Type),
        "SettingsSchema: declared ValueType does not match the member type");
    return leaf;
  };
  return field;
}

// A scalar field whose meaning depends on a sibling boolean ("hasTemperature").
// The flag comes first so it cannot be mistaken for the leaf of the chain; the
// owner class is deduced from it and checked against the chain's parent.
template <ValueType Type, typename Owner, typename... Path>
FieldSpec PresenceField(const char *section, const char *path, const char *doc,
                        bool Owner::*presenceFlag, Path... members) {
  FieldSpec field = Field<Type>(section, path, doc, members...);
  field.presence = [presenceFlag, members...](GameSettings &settings) -> bool * {
    auto *owner = detail::OwnerOf(settings, members...);
    static_assert(std::is_same_v<std::remove_pointer_t<decltype(owner)>, Owner>,
                  "SettingsSchema: presence flag must belong to the parent of "
                  "the field it guards");
    return &(owner->*presenceFlag);
  };
  return field;
}

template <ValueType Type, typename... Path>
CollectionFieldSpec ElementField(const char *path, const char *label,
                                 const char *doc, Path... members) {
  CollectionFieldSpec field;
  field.path = path;
  field.type = Type;
  field.label = label;
  field.doc = doc;
  field.access = [members...](GameSettings::SystemOneProfileSection &profile)
      -> void * {
    auto *leaf = detail::Walk(profile, members...);
    static_assert(
        detail::TypeMatches(detail::TypeTag<std::remove_pointer_t<
                                decltype(leaf)>>::value,
                            Type),
        "SettingsSchema: declared ValueType does not match the profile member");
    return leaf;
  };
  return field;
}

// The profile counterpart of PresenceField.
template <ValueType Type, typename Owner, typename... Path>
CollectionFieldSpec PresenceElementField(
    const char *path, const char *label, const char *doc,
    bool Owner::*presenceFlag, Path... members) {
  CollectionFieldSpec field = ElementField<Type>(path, label, doc, members...);
  field.presence =
      [presenceFlag, members...](
          GameSettings::SystemOneProfileSection &profile) -> bool * {
    auto *owner = detail::OwnerOf(profile, members...);
    static_assert(std::is_same_v<std::remove_pointer_t<decltype(owner)>, Owner>,
                  "SettingsSchema: presence flag must belong to the parent of "
                  "the profile field it guards");
    return &(owner->*presenceFlag);
  };
  return field;
}

}  // namespace SettingsSchema
