#include "SkillRegistry.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>

#include "DebugLog.h"
#include "EffectParsing.h"

namespace {

struct ParsedSkill {
  SkillDef def;
  DeclaredEffects effects;
  std::vector<std::string> warnings;
};

std::string SkillTypeJsonName(SkillType type) {
  switch (type) {
    case SkillType::Passive: return "passive";
    case SkillType::Active: return "active";
    case SkillType::Hybrid: return "hybrid";
    case SkillType::Toggle: return "toggle";
  }
  return "passive";
}

// One document -> one definition + its declared effects. The file loader and the
// Content Editor's save both go through here, so a skill edited in the window is
// validated by exactly the rules a hand-written file is. Structural problems
// throw (the caller reports the file); authoring problems are warnings.
void ParseSkillDocument(const nlohmann::json &j, const std::string &id,
                        ParsedSkill &out) {
  auto warn = [&out](const std::string &message) {
    out.warnings.push_back(message);
  };

  SkillDef &def = out.def;
  def.id = id;
  def.name = j.value("name", def.id);
  def.description = j.value("description", "");
  def.icon = j.value("icon", "");
  def.levelsPerStage = j.value("levelsPerStage", 9);
  // Untrained is neutral for a skill, so the baseline is 0 rather than the
  // average-human 10 a stat uses (section 3.4).
  def.baseline = j.value("baseline", 0.0f);
  def.scale = j.value("scale", 1.0f);

  // The type decides whether the skill is a button. An unknown name is a
  // content error reported once here, and the skill stays Passive so a typo
  // cannot silently produce a skill that can never be used.
  if (j.contains("type")) {
    const std::string typeName = j.value("type", std::string("passive"));
    if (!ParseSkillType(typeName, def.type)) {
      warn("Warning: Skill '" + def.id + "' has unknown type '" + typeName +
           "'; using passive.");
      def.type = SkillType::Passive;
    }
  }

  // The activation block is what a button does. Parsed leniently, then
  // checked against the type below.
  bool hasActivation = false;
  if (j.contains("activation")) {
    if (!j["activation"].is_object()) {
      warn("Warning: Skill '" + def.id +
           "' has a non-object 'activation'; ignoring it.");
    } else {
      const auto& a = j["activation"];
      hasActivation = true;
      def.activation.useWeapon = a.value("useWeapon", false);
      def.activation.baseDamage = a.value("baseDamage", 0.0f);
      def.activation.cooldownSeconds = a.value("cooldown", 1.5f);
      def.activation.rangeTiles = a.value("range", 1.0f);
      def.activation.gcd = a.value("gcd", true);
      def.activation.gcdSeconds = a.value("gcdSeconds", 0.0f);
      def.activation.autoCast = a.value("autoCast", false);
      def.activation.xpPerUse = a.value("xpPerUse", 0);
      def.activation.requiresTarget = a.value("requiresTarget", true);
      def.activation.castSeconds = a.value("cast", 0.0f);
      def.activation.interruptible = a.value("interruptible", true);
      def.activation.interrupts = a.value("interrupts", false);
      def.activation.projectile = a.value("projectile", std::string());
      def.activation.skillshot = a.value("skillshot", std::string());
    }
  }

  // Type and activation are two statements about the same thing, so the
  // loader is where they are reconciled. Silent disagreement would either
  // give a passive skill a button nobody renders or an active skill no
  // effect at all.
  if (SkillIsActivatable(def.type) && !hasActivation) {
    warn("Warning: Skill '" + def.id + "' is " + SkillTypeName(def.type) +
         " but declares no 'activation' block, so it can never be used; "
         "treating it as passive.");
    def.type = SkillType::Passive;
  }
  if (!SkillIsActivatable(def.type) && hasActivation) {
    warn("Warning: Skill '" + def.id +
         "' is passive but declares an 'activation' block; ignoring the "
         "activation.");
    def.activation = SkillActivation{};
  }
  if (SkillIsActivatable(def.type) && !def.activation.requiresTarget &&
      def.activation.useWeapon) {
    warn("Warning: Skill '" + def.id +
         "' uses the weapon but requires no target; the weapon's damage "
         "has nothing to hit.");
  }
  if (def.activation.rangeTiles <= 0.0f) {
    warn("Warning: Skill '" + def.id +
         "' has activation range <= 0; using 1 tile.");
    def.activation.rangeTiles = 1.0f;
  }
  if (def.activation.gcdSeconds < 0.0f) {
    warn("Warning: Skill '" + def.id +
         "' has a negative gcdSeconds; using 0 (the default).");
    def.activation.gcdSeconds = 0.0f;
  }
  if (def.activation.castSeconds < 0.0f) {
    warn("Warning: Skill '" + def.id +
         "' has a negative cast time; using 0 (instant).");
    def.activation.castSeconds = 0.0f;
  }
  if (!def.activation.projectile.empty() &&
      !def.activation.requiresTarget) {
    warn("Warning: Skill '" + def.id +
         "' launches a projectile but is not targeted; enabling targeting.");
    def.activation.requiresTarget = true;
  }
  if (!def.activation.skillshot.empty()) {
    if (!def.activation.projectile.empty()) {
      warn("Warning: Skill '" + def.id +
           "' declares both projectile and skillshot; using the skillshot "
           "and ignoring the homing projectile.");
      def.activation.projectile.clear();
    }
    if (def.activation.requiresTarget) {
      warn("Warning: Skill '" + def.id +
           "' is a skillshot and is aimed, not targeted; clearing "
           "requiresTarget.");
      def.activation.requiresTarget = false;
    }
    if (def.activation.rangeTiles <= 0.0f) {
      warn("Warning: Skill '" + def.id +
           "' is a skillshot with no range; using one tile.");
      def.activation.rangeTiles = 1.0f;
    }
  }
  if (def.activation.interrupts && def.activation.castSeconds > 0.0f &&
      !def.activation.interruptible) {
    // Legal, but worth reporting: a slow, uninterruptible interrupt is a
    // strange thing to author and usually a copy-paste mistake.
    warn("Warning: Skill '" + def.id +
         "' interrupts other casts but is itself uninterruptible and slow.");
  }
  if (SkillIsActivatable(def.type) && !def.activation.useWeapon &&
      def.activation.baseDamage <= 0.0f) {
    warn("Warning: Skill '" + def.id +
         "' is activatable but has neither useWeapon nor a positive "
         "baseDamage, so using it does nothing.");
  }
  if (def.type == SkillType::Toggle) {
    // A toggle exists to switch a persistent effect on and off. With no
    // effects there is nothing to toggle, which is almost certainly an
    // authoring mistake.
    if (j.contains("effects") &&
        (!j["effects"].is_array() || j["effects"].empty())) {
      warn("Warning: Skill '" + def.id +
           "' is a toggle but declares no effects, so it toggles nothing.");
    }
  }

  if (j.contains("stages")) {
    for (const auto& stage : j["stages"]) {
      def.stages.push_back(stage.get<std::string>());
    }
  }
  if (j.contains("xpPerStage")) {
    for (const auto& cost : j["xpPerStage"]) {
      def.xpPerStage.push_back(cost.get<int>());
    }
  }
  if (j.contains("trainedBy")) {
    for (const auto& activity : j["trainedBy"]) {
      def.trainedBy.push_back(activity.get<std::string>());
    }
  }

  // A skill nothing can progress through is content error, not a runtime
  // state: report it here rather than letting it sit inert forever.
  if (def.stages.empty()) {
    warn("Warning: Skill '" + def.id +
         "' declares no stages, so it can never progress.");
  }
  if (def.levelsPerStage <= 0) {
    warn("Warning: Skill '" + def.id + "' has levelsPerStage <= 0; using 9.");
    def.levelsPerStage = 9;
  }
  if (def.scale <= 0.0f) {
    warn("Warning: Skill '" + def.id +
         "' has scale <= 0, which would divide by zero when resolving; "
         "using 1 instead.");
    def.scale = 1.0f;
  }
  if (def.xpPerStage.empty()) {
    warn("Warning: Skill '" + def.id +
         "' declares no xpPerStage, so every level costs the default " +
         std::to_string(SkillDef::kDefaultXpPerLevel) + " XP.");
  }
  for (size_t i = 0; i < def.xpPerStage.size(); ++i) {
    if (def.xpPerStage[i] <= 0) {
      warn("Warning: Skill '" + def.id + "' has a non-positive XP cost for " +
           def.StageName(static_cast<int>(i)) +
           "; that level will cost the default instead.");
    }
  }

  if (j.contains("effects")) {
    ParseEffectEntries(j["effects"], def.id, out.effects, warn);
  }
}

} // namespace

void SkillRegistry::LoadSkills(const std::string& directoryPath) {
  if (!std::filesystem::exists(directoryPath)) {
    const std::string errStr =
        "Warning: Skill directory not found: " + directoryPath + "\n";
    if (debugLog) debugLog->LogError(errStr);
    else std::cerr << errStr;
    return;
  }

  auto warn = [this](const std::string& message) {
    if (debugLog) debugLog->LogWarning(message);
    else std::cerr << message << std::endl;
  };

  for (const auto& entry : std::filesystem::directory_iterator(directoryPath)) {
    if (entry.path().extension() != ".json") continue;

    std::ifstream file(entry.path());
    if (!file.is_open()) continue;

    try {
      nlohmann::json j;
      file >> j;

      const std::string id = entry.path().stem().string();
      ParsedSkill parsed;
      ParseSkillDocument(j, id, parsed);

      for (const std::string &warning : parsed.warnings) {
        warn(warning);
      }

      skills[id] = parsed.def;
      sources[id] = j;
      declared.links.erase(id);
      declared.hooks.erase(id);
      const auto &links = parsed.effects.LinksFor(id);
      if (!links.empty()) declared.links[id] = links;
      const auto &hooks = parsed.effects.HooksFor(id);
      if (!hooks.empty()) declared.hooks[id] = hooks;
    } catch (const std::exception& e) {
      warn("Failed to parse skill " + entry.path().string() + ": " +
           std::string(e.what()));
    }
  }
}

SkillRegistry::AddResult SkillRegistry::OverrideDefinition(
    const nlohmann::json& document, const std::string& sourcePath) {
  AddResult result;
  if (!document.is_object()) {
    result.error = "the skill document is not a JSON object";
    return result;
  }

  std::string id;
  if (document.contains("id") && document["id"].is_string()) {
    id = document["id"].get<std::string>();
  }
  if (id.empty() && !sourcePath.empty()) {
    id = std::filesystem::path(sourcePath).stem().string();
  }
  if (id.empty()) {
    result.error = "the skill document has no 'id'";
    return result;
  }
  if (id.find('/') != std::string::npos || id.find('\\') != std::string::npos ||
      id == "." || id == "..") {
    result.error = "skill id '" + id + "' is not a valid file stem";
    return result;
  }

  try {
    ParsedSkill parsed;
    ParseSkillDocument(document, id, parsed);
    result.warnings = parsed.warnings;

    skills[id] = parsed.def;
    sources[id] = document;
    declared.links.erase(id);
    declared.hooks.erase(id);
    const auto &links = parsed.effects.LinksFor(id);
    if (!links.empty()) declared.links[id] = links;
    const auto &hooks = parsed.effects.HooksFor(id);
    if (!hooks.empty()) declared.hooks[id] = hooks;

    result.ok = true;
  } catch (const std::exception &e) {
    result.error = std::string("could not parse the skill: ") + e.what();
  }
  return result;
}

nlohmann::json SkillRegistry::ToJson(const std::string& id) const {
  const SkillDef* def = Get(id);
  if (def == nullptr) {
    return nlohmann::json::object();
  }

  // Start from the authored source so unknown fields survive a round trip, then
  // overwrite every known field with its live value. A document written from
  // the defaults alone would silently drop the parts this editor does not know.
  nlohmann::json j = nlohmann::json::object();
  auto source = sources.find(id);
  if (source != sources.end() && source->second.is_object()) {
    j = source->second;
  }
  j.erase("id");

  j["name"] = def->name;
  j["description"] = def->description;
  j["icon"] = def->icon;
  j["type"] = SkillTypeJsonName(def->type);
  j["levelsPerStage"] = def->levelsPerStage;
  j["baseline"] = def->baseline;
  j["scale"] = def->scale;
  j["stages"] = def->stages;
  j["xpPerStage"] = def->xpPerStage;
  j["trainedBy"] = def->trainedBy;

  if (SkillIsActivatable(def->type)) {
    // Start from the source's activation so a field this build does not know
    // yet survives the round trip, then overwrite every known field.
    nlohmann::json activation = nlohmann::json::object();
    if (j.contains("activation") && j["activation"].is_object()) {
      activation = j["activation"];
    }
    activation["useWeapon"] = def->activation.useWeapon;
    activation["baseDamage"] = def->activation.baseDamage;
    activation["cooldown"] = def->activation.cooldownSeconds;
    activation["range"] = def->activation.rangeTiles;
    activation["gcd"] = def->activation.gcd;
    activation["gcdSeconds"] = def->activation.gcdSeconds;
    activation["autoCast"] = def->activation.autoCast;
    activation["xpPerUse"] = def->activation.xpPerUse;
    activation["requiresTarget"] = def->activation.requiresTarget;
    activation["cast"] = def->activation.castSeconds;
    activation["interruptible"] = def->activation.interruptible;
    activation["interrupts"] = def->activation.interrupts;
    activation["projectile"] = def->activation.projectile;
    activation["skillshot"] = def->activation.skillshot;
    j["activation"] = std::move(activation);
  } else {
    // A passive skill with an activation block is ignored by the loader; the
    // editor should not offer a control that has no effect.
    j.erase("activation");
  }

  // Always present, so the generic form has an array to add to even when the
  // authored file omitted the key entirely.
  if (!j.contains("effects") || !j["effects"].is_array()) {
    j["effects"] = nlohmann::json::array();
  }
  return j;
}

const SkillDef* SkillRegistry::Get(const std::string& id) const {
  auto it = skills.find(id);
  if (it == skills.end()) return nullptr;
  return &it->second;
}

std::vector<std::string> SkillRegistry::GetIds() const {
  std::vector<std::string> ids;
  ids.reserve(skills.size());
  for (const auto& pair : skills) {
    ids.push_back(pair.first);
  }
  std::sort(ids.begin(), ids.end());
  return ids;
}

std::vector<const SkillDef*> SkillRegistry::GetAll() const {
  std::vector<const SkillDef*> defs;
  defs.reserve(skills.size());
  for (const std::string& id : GetIds()) {
    defs.push_back(Get(id));
  }
  return defs;
}
