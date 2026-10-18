#include "ObjectFactory.h"

#include "PlaceCategoriesJson.hpp"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>

#include "AsciiArt.hpp"
#include "Map.h"
#include "DebugLog.h"
#include "Defaults.h"
#include "ItemRegistry.h"
#include "RequiresParsing.h"
#include "ResourceManager.h"
#include "SpritePaths.hpp"
#include "TrainingParsing.h"

ObjectFactory::ObjectFactory() {
  RegisterDefaultComponents();
}

void ObjectFactory::RecordComponentInfo(const std::string& name,
                                        std::vector<std::string> parameters,
                                        std::string summary) {
  std::sort(parameters.begin(), parameters.end());
  parameters.erase(std::unique(parameters.begin(), parameters.end()),
                   parameters.end());
  if (componentParameters.find(name) == componentParameters.end()) {
    componentOrder.push_back(name);
  }
  componentParameters[name] = std::move(parameters);
  componentSummaries[name] = std::move(summary);
}

namespace {

// When a template entered the factory. A loaded file's last-write time is what
// the Map Editor's date sort means by "creation date"; a source that is not a
// readable file -- a designer's in-memory add, whose file ObjectDesigner writes
// separately -- falls back to now, which is when it was created.
std::filesystem::file_time_type SourceTimeOrNow(const std::string& sourcePath) {
  std::error_code ec;
  const auto time = std::filesystem::last_write_time(sourcePath, ec);
  return ec ? std::filesystem::file_time_type::clock::now() : time;
}

// What kind of JSON value a parameter must hold.
//
// This table exists because the setters are *lenient*: `capacity` read with
// `tmpl.value("capacity", 10)` returns the default for a string, so a designer writing
// `"capacity": "lots"` would get a silent 10. Worse, the places that do not use
// `value()` -- Placeable indexes the placement object directly -- throw a nlohmann
// type_error, which reaches a user as "[json.exception.type_error.302] type must be
// number, but is string".
//
// So this is duplicated work *on purpose*, and the duplication is bounded: it asserts
// only the JSON type, never the semantics. What a value *means* stays the setter's
// business, which is what keeps the two from being able to disagree about behaviour.
enum class ParamType { Number, String, Bool, Array, Object };

struct ParamSpec {
  const char* field;  // dotted for a nested key
  ParamType type;
  bool required;
};

const std::vector<ParamSpec>& ParameterSpecs() {
  static const std::vector<ParamSpec> specs = {
      {"capacity", ParamType::Number, false},
      {"amountRemaining", ParamType::Number, false},
      {"timer", ParamType::Number, false},
      {"durabilityCost", ParamType::Number, false},
      {"drops", ParamType::Array, false},
      {"recipes", ParamType::Array, true},
      {"evolveTime", ParamType::Number, false},
      {"evolveTarget", ParamType::String, false},
      {"isActive", ParamType::Bool, false},
      {"placement", ParamType::Object, true},
      {"placement.becomes", ParamType::String, true},
      {"placement.surface", ParamType::Array, false},
      {"placement.maxDistance", ParamType::Number, false},
      {"requires", ParamType::Array, true},
      {"trains", ParamType::Array, true},
  };
  return specs;
}

const char* TypeName(ParamType type) {
  switch (type) {
    case ParamType::Number: return "a number";
    case ParamType::String: return "a string";
    case ParamType::Bool: return "true or false";
    case ParamType::Array: return "an array";
    case ParamType::Object: return "an object";
  }
  return "a value";
}

bool HasType(const nlohmann::json& value, ParamType type) {
  switch (type) {
    case ParamType::Number: return value.is_number();
    case ParamType::String: return value.is_string();
    case ParamType::Bool: return value.is_boolean();
    case ParamType::Array: return value.is_array();
    case ParamType::Object: return value.is_object();
  }
  return false;
}

// The value at a dotted path, or null when a step is missing or is not an object.
const nlohmann::json* AtPath(const nlohmann::json& root, const std::string& path) {
  const nlohmann::json* current = &root;
  size_t start = 0;
  while (current != nullptr && current->is_object()) {
    const size_t dot = path.find('.', start);
    const std::string step =
        path.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
    const auto found = current->find(step);
    if (found == current->end()) return nullptr;
    current = &(*found);
    if (dot == std::string::npos) break;
    start = dot + 1;
  }
  return current;
}

}  // namespace

void ObjectFactory::RegisterDefaultComponents() {
  // The parameter lists below are the template keys each setter actually reads, and the
  // summaries are what a designer is shown. Two rules keep them honest: list a key only
  // when its setter reads it, and update the table in the same edit that adds a key. A
  // parameter missing here is invisible to the designer, and a required one missing from
  // ParameterSpecs is not type-checked -- both are quiet failures this metadata exists to
  // prevent.
  RecordComponentInfo("Interactable", {}, "it can be used");
  RecordComponentInfo("Obstacle", {}, "it blocks movement");
  // Independent of Obstacle on purpose: a glass wall is solid and transparent, and
  // tall grass is the other way round. docs/spatial-memory-design.md section 4.3.
  RecordComponentInfo("BlocksVision", {}, "it blocks sight");
  RecordComponentInfo("Structural", {}, "it is part of a structure, not a prop");
  RecordComponentInfo("ShowNameTag", {}, "it draws a floating name tag");
  RecordComponentInfo("Portable", {}, "it can be picked up and carried");
  RecordComponentInfo("Storage", {"capacity"}, "it holds items");
  RecordComponentInfo("Harvestable",
                      {"amountRemaining", "durabilityCost", "drops", "timer"},
                      "it can be gathered from");
  RecordComponentInfo("Workstation", {"recipes"}, "it is used to craft");
  RecordComponentInfo("Evolvable", {"evolveTarget", "evolveTime", "isActive"},
                      "it changes into another template over time");
  // Nested keys are dotted, because that is how a definition has to spell them.
  RecordComponentInfo("Placeable",
                      {"placement.becomes", "placement.maxDistance",
                       "placement.surface"},
                      "it can be put down in the world");
  RecordComponentInfo("Requires", {"requires"},
                      "using it needs certain equipment");
  RecordComponentInfo("Trains", {"trains"}, "using it teaches a skill");
  RecordComponentInfo("PlaceCategories", {"placeCategories"},
                      "it is part of a kind of place");

  RegisterComponent<Interactable>("Interactable", [](flecs::entity obj, const nlohmann::json&) { obj.add<Interactable>(); });
  RegisterComponent<Obstacle>("Obstacle", [](flecs::entity obj, const nlohmann::json&) { obj.add<Obstacle>(); });
  RegisterComponent<BlocksVision>("BlocksVision", [](flecs::entity obj, const nlohmann::json&) { obj.add<BlocksVision>(); });
  RegisterComponent<Structural>("Structural", [](flecs::entity obj, const nlohmann::json&) { obj.add<Structural>(); });
  RegisterComponent<ShowNameTag>("ShowNameTag", [](flecs::entity obj, const nlohmann::json&) { obj.add<ShowNameTag>(); });
  RegisterComponent<Portable>("Portable", [](flecs::entity obj, const nlohmann::json&) { obj.add<Portable>(); });
  RegisterComponent<Storage>("Storage", [](flecs::entity obj, const nlohmann::json& tmpl) {
    Storage s;
    s.capacity = tmpl.value("capacity", 10);
    obj.set<Storage>(s);
  });
  
  RegisterComponent<Harvestable>("Harvestable", [](flecs::entity obj, const nlohmann::json& tmpl) {
    Harvestable h;
    h.amountRemaining = tmpl.value("amountRemaining", 1);
    h.timer = tmpl.value("timer", 0.0f);
    // Every harvestable has a cost; only an object that wants a different one
    // from the default has to say so.
    h.durabilityCost =
        tmpl.value("durabilityCost", DEFAULT_HARVEST_DURABILITY_COST);
    if (tmpl.contains("drops")) {
      for (const auto& drop : tmpl["drops"]) {
        h.lootTable.drops.push_back({
          drop.value("item", ""),
          drop.value("chance", 1.0f)
        });
      }
    }
    obj.set<Harvestable>(h);
  });
  
  RegisterComponent<Workstation>("Workstation", [](flecs::entity obj, const nlohmann::json& tmpl) {
    Workstation w;
    if (tmpl.contains("recipes")) {
      for (const auto& r : tmpl["recipes"]) {
        w.recipes.push_back(r.get<std::string>());
      }
    }
    obj.set<Workstation>(w);
  });
  
  RegisterComponent<Evolvable>("Evolvable", [](flecs::entity obj, const nlohmann::json& tmpl) {
    Evolvable e;
    e.timeRemaining = tmpl.value("evolveTime", 60.0f);
    e.nextStageTemplate = tmpl.value("evolveTarget", "");
    e.isActive = tmpl.value("isActive", true);
    obj.set<Evolvable>(e);
  });
  
  // Placement rules live in the template's "placement" block so the [PLACE]
  // verb can stay generic: this is what makes an item settable in the world, and
  // where it is allowed to go. A malformed block leaves the item unplaceable
  // rather than guessing, and says so at load time.
  RegisterComponent<Placeable>("Placeable", [this](flecs::entity obj, const nlohmann::json& tmpl) {
    const std::string owner =
        obj.has<ItemType>() ? obj.get<ItemType>()->id : std::string("object");
    auto warn = [this](const std::string& message) {
      if (debugLog) debugLog->LogWarning(message);
      else std::cerr << message << std::endl;
    };

    if (!tmpl.contains("placement")) {
      warn("Warning: object template '" + owner +
           "' lists the Placeable component but has no placement block; "
           "it cannot be placed.\n");
      return;
    }

    const auto& placement = tmpl["placement"];
    Placeable placeable;
    placeable.becomes = placement.value("becomes", "");
    if (placeable.becomes.empty()) {
      warn("Warning: object template '" + owner +
           "' has a placement block without 'becomes'; it cannot be placed.\n");
      return;
    }
    if (placement.contains("surface")) {
      for (const auto& surface : placement["surface"]) {
        placeable.surface.push_back(surface.get<std::string>());
      }
    }
    placeable.maxDistance =
        placement.value("maxDistance", DEFAULT_PLACEMENT_MAX_DISTANCE);
    obj.set<Placeable>(placeable);
  });

  // A capability gate: what an actor must satisfy before a verb on this object
  // will run. The equipment clause is expressed in item tags (ItemDef::tags), so
  // the requirement names a capability ("scythe") rather than a specific item id
  // and any item carrying that tag satisfies it.
  RegisterComponent<Requires>("Requires", [this](flecs::entity obj, const nlohmann::json& tmpl) {
    const std::string owner =
        obj.has<ItemType>() ? obj.get<ItemType>()->id : std::string("object");
    auto warn = [this](const std::string& message) {
      if (debugLog) debugLog->LogWarning(message);
      else std::cerr << message << std::endl;
    };

    // Shared with the guide, which reports the same gate to an agent: if the two
    // parsed it differently, [INFO] could promise a harvest the engine refuses.
    Requires gate;
    gate.equippedTags = ParseRequiredTags(tmpl, owner, warn);

    if (!gate.equippedTags.empty()) {
      obj.set<Requires>(gate);
    }
  });

  // What harvesting this object teaches. The subject-side half of skill
  // progression, so an object that teaches nothing simply omits the component --
  // harvesting an iron vein trains no skill today.
  //
  // ItemType is readable here because ApplyTemplate stamps it before running the
  // component setters, which is what lets the warning name the template.
  // {"orchard": 1.0, "poor": 0.4} -- category ids mapped to an intensity in 0..1.
  // An unknown id is not refused here: the taxonomy is a separate file, so the seed
  // carries an id the registry may not resolve, and derivation ignores it with a
  // warning. Whether a category forms a place is the taxonomy's decision, not this
  // setter's.
  RegisterComponent<PlaceCategories>(
      "PlaceCategories", [](flecs::entity obj, const nlohmann::json& tmpl) {
        if (!tmpl.contains("placeCategories")) {
          return;
        }
        // The same parser the tile definitions use, so the two file kinds cannot
        // disagree about what the key means.
        std::vector<SeedCategory> categories =
            ParsePlaceCategories(tmpl["placeCategories"]);
        if (!categories.empty()) {
          obj.set<PlaceCategories>({std::move(categories)});
        }
      });

  RegisterComponent<Trains>("Trains", [this](flecs::entity obj, const nlohmann::json& tmpl) {
    if (!tmpl.contains("trains")) {
      return;
    }
    const std::string owner =
        obj.has<ItemType>() ? obj.get<ItemType>()->id : std::string("object");

    Trains trains;
    auto warn = [this](const std::string& message) {
      if (debugLog) debugLog->LogWarning(message);
      else std::cerr << message << std::endl;
    };
    ParseTrainingGrants(tmpl["trains"], "Object template '" + owner + "'",
                        trains.grants, warn);

    if (!trains.grants.empty()) {
      obj.set<Trains>(trains);
    }
  });
}

void ObjectFactory::LoadTemplates(const std::string& directoryPath) {
  // Recorded before the exists() check: a caller that deletes a template needs to
  // name its file even when the directory has since gone missing, and "the
  // directory we were told to read" is the only honest answer to that.
  directory = directoryPath;

  if (!std::filesystem::exists(directoryPath)) {
    std::string errStr = "Warning: Object template directory not found: " + directoryPath + "\n";
    if (debugLog) debugLog->LogError(errStr);
    else std::cerr << errStr;
    return;
  }

  for (const auto& entry : std::filesystem::directory_iterator(directoryPath)) {
    if (entry.path().extension() == ".json") {
      std::ifstream file(entry.path());
      if (file.is_open()) {
        try {
          nlohmann::json j;
          file >> j;
          
          // Use filename without extension as the type key
          std::string type = entry.path().stem().string();
          templates[type] = j;
          templateTimes[type] = SourceTimeOrNow(entry.path().string());
          
        } catch (const std::exception& e) {
          std::string errStr = "Failed to parse object template " + entry.path().string() + ": " + std::string(e.what()) + "\n";
          if (debugLog) debugLog->LogError(errStr);
          else std::cerr << errStr;
        }
      }
    }
  }
}

bool ObjectFactory::RemoveTemplate(const std::string& key) {
  // erase's return is the honest answer: a key that was never loaded reports false
  // rather than pretending the removal happened.
  //
  // The runtime record goes with it: a key that is no longer in `templates` is not
  // runtime-added either, whatever it used to be.
  runtimeKeys.erase(key);
  templateTimes.erase(key);
  return templates.erase(key) > 0;
}

std::filesystem::file_time_type ObjectFactory::GetTemplateTime(
    const std::string& key) const {
  const auto it = templateTimes.find(key);
  return it == templateTimes.end() ? std::filesystem::file_time_type{}
                                   : it->second;
}

namespace {

// The template key for a definition: lowercase, spaces and punctuation to underscores.
//
// The key is not only a filename: it doubles as the sprite convention id, so the key
// decides which sprite the object will look for (`wooden_crate` -> wooden_crate.png).
// That is why it is derived here rather than left to the model, and why a definition
// that names its own `key` is honoured -- an object that wants an explicitly chosen
// sprite convention has to be able to say so.
std::string SlugifyKey(const std::string& name) {
  std::string out;
  out.reserve(name.size());
  bool lastUnderscore = false;
  for (const char c : name) {
    const unsigned char uc = static_cast<unsigned char>(c);
    if (std::isalnum(uc)) {
      out += static_cast<char>(std::tolower(uc));
      lastUnderscore = false;
    } else if (!lastUnderscore && !out.empty()) {
      out += '_';
      lastUnderscore = true;
    }
  }
  while (!out.empty() && out.back() == '_') out.pop_back();
  return out;
}

}  // namespace

std::string ObjectFactory::KeyFor(const nlohmann::json& definition) {
  if (!definition.is_object()) return {};
  const std::string explicitKey = definition.value("key", std::string());
  if (!explicitKey.empty()) return explicitKey;
  return SlugifyKey(definition.value("name", std::string()));
}

ObjectFactory::RuntimeAddResult ObjectFactory::AddRuntime(
    const nlohmann::json& definition, const std::string& source,
    bool replaceExisting, bool requireAtLeastOneComponent) {
  RuntimeAddResult result;

  if (!definition.is_object()) {
    result.error = "the definition is not a JSON object";
    return result;
  }

  result.key = KeyFor(definition);
  if (result.key.empty()) {
    result.error =
        "the definition has no \"name\", so it has no key and no sprite name either";
    return result;
  }

  const bool isLoaded = templates.find(result.key) != templates.end() &&
                        runtimeKeys.find(result.key) == runtimeKeys.end();
  if (isLoaded) {
    // The hand-authored catalog is read-only to a designer. The refusal names the key
    // because that is what the caller has to change.
    result.error = "the key '" + result.key +
                   "' is a hand-authored template, and a designer may not replace "
                   "hand-authored content";
    return result;
  }
  if (!replaceExisting && templates.find(result.key) != templates.end()) {
    result.error = "the key '" + result.key + "' already exists";
    return result;
  }

  // At least one capability. An object with none is a tile -- it has a glyph and a
  // colour and does nothing -- and the object designer's job is the difference. The
  // check is here rather than in the designer so the overlay loader can turn it off.
  if (requireAtLeastOneComponent) {
    const auto components = definition.find("components");
    if (components == definition.end() || !components->is_array() ||
        components->empty()) {
      result.error =
          "the definition lists no \"components\", so the object would do nothing. "
          "An object is at least one capability from the list; if the request is "
          "really for ground, it is a tile, not an object";
      return result;
    }
  }

  // ---- parameter types ---------------------------------------------------------
  //
  // Before the setters run, because their leniency and their exceptions are both wrong
  // messages for a designer: `capacity: "lots"` would either be silently defaulted to 10
  // or reach the user as a raw nlohmann type_error. Checked only for the parameters of
  // the components this definition actually lists, so a stray key belonging to a
  // component it did not choose is the validator's business (it is reported as an
  // unread key) rather than this table's.
  if (definition.contains("components") && definition["components"].is_array()) {
    for (const auto& componentName : definition["components"]) {
      if (!componentName.is_string()) continue;
      const std::string name = componentName.get<std::string>();
      const auto listed = componentParameters.find(name);
      if (listed == componentParameters.end()) continue;  // reported below, by the setter

      for (const ParamSpec& spec : ParameterSpecs()) {
        // Only the parameters this component declares. A shared field name read by
        // several components is checked for each of them, which is correct.
        const std::string field = spec.field;
        const std::string top = field.substr(0, field.find('.'));
        const bool owned =
            std::find(listed->second.begin(), listed->second.end(), field) !=
                listed->second.end() ||
            std::find(listed->second.begin(), listed->second.end(), top) !=
                listed->second.end();
        if (!owned) continue;

        const nlohmann::json* value = AtPath(definition, field);
        if (value == nullptr) {
          if (spec.required) {
            result.error = "\"" + name + "\" needs \"" + field + "\" (" +
                           TypeName(spec.type) + "), and the definition does not have "
                           "it";
            return result;
          }
          continue;
        }
        if (!HasType(*value, spec.type)) {
          result.error = "\"" + field + "\" must be " + TypeName(spec.type) +
                         " for the \"" + name + "\" component, and this definition "
                         "has " +
                         std::string(value->type_name()) + " instead";
          return result;
        }
      }
    }
  }

  // ---- the definition's shape, checked without a world --------------------------
  //
  // The engine has no way to try a template without a world, and the obvious workaround
  // -- build a throwaway entity in a scratch `flecs::world` and run the real setters on
  // it -- was tried and is *wrong in this codebase*. flecs resolves a C++ component type
  // globally by name and compares the size and alignment it was first registered with, so
  // a second world that registers `Sprite` or `Trains` on different terms corrupts the
  // first: the symptom was a heap-buffer-overflow in `AttachSprite` and an abort in the
  // game's own `RegisterComponents`, from an overlay entry that had validated perfectly
  // in the designer.
  //
  // So validation here is a pure function of the JSON, which is also what TileRegistry
  // does with `ParseTileDef`. What this checks is the shape: component names, parameter
  // types, required parameters, and -- in the designer, which is the only caller that
  // lacks human review -- the art block. What it deliberately does not check is
  // semantics: whether a recipe id exists, whether an evolve target is a real template.
  // Those are settled at spawn time by the setters that already report them.
  //
  // `KnownFieldNames` and the parameter lists come from the same registration call as the
  // setters, so a component can be added without describing it, but not without the
  // description being visibly absent from the table above it.

  // Every listed component must exist. This is the refusal that used to be the setter's
  // own warning ("Unknown component type: X"); it is produced here now because there is
  // no world to run the setter in, and it is worded the same way so `ObjectDesigner` can
  // still append "did you mean ..." by finding the same marker.
  if (requireAtLeastOneComponent && definition.contains("components") &&
      definition["components"].is_array()) {
    std::vector<std::string> unknown;
    for (const auto& componentName : definition["components"]) {
      if (!componentName.is_string()) {
        result.error = "\"components\" must contain only strings, and this definition "
                       "has a " +
                       std::string(componentName.type_name()) + " in it";
        return result;
      }
      const std::string name = componentName.get<std::string>();
      if (componentParameters.find(name) == componentParameters.end()) {
        unknown.push_back(name);
      }
    }
    if (!unknown.empty()) {
      std::string message = "Unknown component type: " + unknown.front();
      for (size_t i = 1; i < unknown.size(); ++i) {
        message += ", " + unknown[i];
      }
      result.error = message;
      return result;
    }
  }

  templates[result.key] = definition;
  runtimeKeys[result.key] = source;
  templateTimes[result.key] = SourceTimeOrNow(source);

  result.ok = true;
  return result;
}

bool ObjectFactory::IsRuntimeAdded(const std::string& key) const {
  return runtimeKeys.find(key) != runtimeKeys.end();
}

std::vector<std::string> ObjectFactory::RuntimeNames() const {
  std::vector<std::string> names;
  names.reserve(runtimeKeys.size());
  for (const auto& pair : runtimeKeys) {
    names.push_back(pair.first);
  }
  std::sort(names.begin(), names.end());
  return names;
}

size_t ObjectFactory::RemoveRuntimeTemplates() {
  size_t removed = 0;
  for (const auto& pair : runtimeKeys) {
    removed += templates.erase(pair.first) > 0 ? 1 : 0;
    templateTimes.erase(pair.first);
  }
  runtimeKeys.clear();
  return removed;
}

std::vector<ObjectFactory::ComponentInfo> ObjectFactory::ComponentVocabulary()
    const {
  std::vector<ComponentInfo> out;
  out.reserve(componentParameters.size());
  for (const auto& pair : componentParameters) {
    ComponentInfo info;
    info.name = pair.first;
    info.parameters = pair.second;
    const auto summary = componentSummaries.find(pair.first);
    if (summary != componentSummaries.end()) {
      info.summary = summary->second;
    }
    out.push_back(std::move(info));
  }
  std::sort(out.begin(), out.end(),
            [](const ComponentInfo& a, const ComponentInfo& b) {
              return a.name < b.name;
            });
  return out;
}

std::vector<std::string> ObjectFactory::ComponentNames() const {
  std::vector<std::string> names;
  names.reserve(componentParameters.size());
  for (const auto& pair : componentParameters) {
    names.push_back(pair.first);
  }
  std::sort(names.begin(), names.end());
  return names;
}

bool ObjectFactory::HasComponent(const std::string& name) const {
  return componentParameters.find(name) != componentParameters.end();
}

std::vector<std::string> ObjectFactory::KnownFieldNames() const {
  // The structural and display keys ApplyTemplate reads, then every component
  // parameter. Kept here rather than in the validator so it cannot drift from the
  // code that does the reading -- this function and ApplyTemplate are in the same
  // file, and a key added to one is a visible omission in the other.
  std::set<std::string> fields = {
      "name",           "key",           "description",    "components",
      "sprite",         "art",           "palette",        "character",
      "characterColor", "backgroundColor", "nameTagColor", "hint",
      "fontSize",
  };

  for (const auto& pair : componentParameters) {
    for (const std::string& parameter : pair.second) {
      fields.insert(parameter);
      // A dotted parameter ("placement.becomes") also names the object that has to
      // exist at the top level, and the object itself is a valid key.
      const size_t dot = parameter.find('.');
      if (dot != std::string::npos) {
        fields.insert(parameter.substr(0, dot));
      }
    }
  }

  return std::vector<std::string>(fields.begin(), fields.end());
}

bool ObjectFactory::ApplyTemplate(flecs::entity obj, const std::string& type, Map* map) {
  if (templates.find(type) == templates.end()) {
    std::string msg = "Warning: Attempted to apply unknown object type: " + type;
    if (debugLog) debugLog->LogWarning(msg);
    else std::cerr << msg << std::endl;
    return false;
  }

  const auto& tmpl = templates[type];

  // Remove potential old components to ensure a clean state
  for (const auto& pair : componentRemovers) {
    pair.second(obj);
  }

  // Display state comes only from the template, so it is cleared with the rest.
  // Without this, re-applying a template -- Evolvable growth, or a placed item
  // becoming a world object -- keeps the previous stage's glyph or art whenever
  // the next template happens to declare none. These two are handled here rather
  // than through componentRemovers because they are driven by the display keys
  // below, not by a template's "components" array.
  obj.remove<DrawAscii>();
  obj.remove<AsciiArt>();

  if (tmpl.contains("character")) {
    DrawAscii ascii;
    std::string charStr = tmpl.value("character", "?");
    ascii.ch = charStr.empty() ? '?' : charStr[0];

    ascii.characterColor = tmpl.contains("characterColor")
                               ? ParseAsciiColor(tmpl["characterColor"],
                                                 {255, 255, 255, 255})
                               : Color{255, 255, 255, 255};
    ascii.backgroundColor = tmpl.contains("backgroundColor")
                                ? ParseAsciiColor(tmpl["backgroundColor"],
                                                  {0, 0, 0, 0})
                                : Color{0, 0, 0, 0};

    // Defaults first, because an item is spawned without a map: harvest drops,
    // crafting output and NPC starting gear all pass nullptr, and leaving these
    // unset meant every such item carried indeterminate visual dimensions.
    ascii.width = DEFAULT_ENTITY_VISUAL_WIDTH;
    ascii.height = DEFAULT_ENTITY_VISUAL_HEIGHT;
    if (map) {
      ascii.width = map->GetTileWidth();
      ascii.height = map->GetTileHeight();
    }

    obj.set<DrawAscii>(ascii);
  }

  // Optional multi-line art. Absent or unusable art leaves the single-glyph
  // DrawAscii above as the whole visual, so this key can only add to a template.
  {
    AsciiArt art;
    std::string artWarning;
    const bool hasArt = ParseAsciiArt(tmpl, art, artWarning);
    if (!artWarning.empty()) {
      const std::string msg =
          "Warning: object template '" + type + "': " + artWarning + "\n";
      if (debugLog) debugLog->LogWarning(msg);
      else std::cerr << msg;
    }
    if (hasArt) {
      obj.set<AsciiArt>(art);
    }
  }

  // A raster sprite, which replaces that art when one resolves. Cleared with the
  // rest of the display state for the same reason DrawAscii and AsciiArt are: a
  // re-applied template -- Evolvable growth, or a placed item becoming a world
  // object -- must not keep the previous stage's sprite. When nothing resolves,
  // no Sprite is attached and the art above stands, which is the fallback.
  obj.remove<Sprite>();
  if (resourceManager != nullptr) {
    std::string explicitSprite;
    if (tmpl.contains("sprite")) {
      if (tmpl["sprite"].is_string()) {
        explicitSprite = tmpl["sprite"].get<std::string>();
      } else {
        const std::string msg = "Warning: object template '" + type +
                                "': 'sprite' is not a string; ignoring it.\n";
        if (debugLog) debugLog->LogWarning(msg);
        else std::cerr << msg;
      }
    }
    // `type` doubles as the convention id, so data/sprites/<templateKey>.png is
    // picked up with no "sprite" key at all.
    AttachSprite(obj, explicitSprite, type, *resourceManager, debugLog);
  }

  // Assign Name
  std::string name = tmpl.value("name", type);
  obj.set<DisplayName>({name});

  // Stable content id: the template key, not the display name. Recipes and any
  // other logic that must identify *what* this is match on this, so renaming a
  // display name in JSON cannot silently break them.
  obj.set<ItemType>({type});

  // Durability is per-instance state seeded from the item definition: an item
  // whose definition declares none never wears out and simply carries no
  // component. Removed first so re-applying a template -- a placed item becoming
  // a world object, say -- cannot leave a stale wear state behind.
  obj.remove<Durability>();
  if (itemRegistry) {
    if (const ItemDef* def = itemRegistry->Get(type)) {
      if (def->durability > 0) {
        obj.set<Durability>({def->durability, def->durability});
      }
    }
  }

  // Short blurb surfaced by the [EXAMINE] interaction.
  obj.set<ObjectDescription>(
      {tmpl.value("description", std::string("It looks unremarkable."))});

  // Authored guide colour for [INFO]. Removed first for the same reason
  // DrawAscii, AsciiArt and Sprite are: a re-applied template -- Evolvable
  // growth, or a placed item becoming a world object -- must not keep the
  // previous stage's hint. Set only when the template declares one, so an object
  // with no hint carries no Hint component at all.
  obj.remove<Hint>();
  if (tmpl.contains("hint")) {
    obj.set<Hint>({tmpl.value("hint", std::string(""))});
  }

  if (tmpl.contains("nameTagColor")) {
    Color tagColor = {
      tmpl["nameTagColor"][0],
      tmpl["nameTagColor"][1],
      tmpl["nameTagColor"][2],
      tmpl["nameTagColor"][3]
    };
    obj.set<NameTagColor>({tagColor});
  } else {
    obj.set<NameTagColor>({DARKGRAY});
  }

  // Assign components
  if (tmpl.contains("components")) {
    for (const auto& compNameJson : tmpl["components"]) {
      std::string compName = compNameJson.get<std::string>();
      if (componentSetters.find(compName) != componentSetters.end()) {
        componentSetters[compName](obj, tmpl);
      } else {
        std::string msg = "Warning: Unknown component type: " + compName;
        if (debugLog) debugLog->LogWarning(msg);
        else std::cerr << msg << std::endl;
      }
    }
  }

  return true;
}

flecs::entity ObjectFactory::SpawnObject(flecs::world& ecs, flecs::entity parent, Map* map, const std::string& type, std::optional<GamePosition> pos, bool authored) {
  if (templates.find(type) == templates.end()) {
    std::string errStr = "Warning: Attempted to spawn unknown object type: " + type + "\n";
    if (debugLog) debugLog->LogError(errStr);
    else std::cerr << errStr;
    return flecs::entity::null();
  }

  flecs::entity obj = ecs.entity().child_of(parent);

  if (pos.has_value()) {
    obj.set<GamePosition>(pos.value());
    if (map) {
      obj.set<ScreenPosition>(map->GameCoordsToScreenCoords(pos->x, pos->y));
    }
  }

  // Marked before ApplyTemplate so the marker survives every early-out below.
  if (authored) {
    obj.set<MapAuthored>({type});
  }

  ApplyTemplate(obj, type, map);

  return obj;
}

void ObjectFactory::SpawnInventory(flecs::world& ecs, flecs::entity holder,
                                   const std::vector<ItemStack>& inventory) {
  if (inventory.empty() || !holder.is_alive()) {
    return;
  }

  for (const ItemStack& stack : inventory) {
    for (int i = 0; i < stack.count; ++i) {
      flecs::entity item = SpawnObject(ecs, holder, nullptr, stack.item);
      if (item.is_alive()) {
        holder.add<Holds>(item);
      }
    }
  }
}
