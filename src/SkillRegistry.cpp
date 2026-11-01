#include "SkillRegistry.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>

#include "DebugLog.h"
#include "EffectParsing.h"

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

      SkillDef def;
      def.id = entry.path().stem().string();
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

      skills[def.id] = def;

      if (j.contains("effects")) {
        ParseEffectEntries(j["effects"], def.id, declared, warn);
      }
    } catch (const std::exception& e) {
      warn("Failed to parse skill " + entry.path().string() + ": " +
           std::string(e.what()));
    }
  }
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
