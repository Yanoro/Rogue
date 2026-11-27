#pragma once

#include "EquipmentTypes.h"
#include "ItemStack.hpp"
#include "StartingSkills.hpp"
#include "ResourceRegistry.h"
#include "StatTypes.h"

#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

class DebugLog;

// One NPC definition: everything a character starts with, declared once and
// spawned by name.
//
// This is a **seed, not a class or a role**. A "blacksmith" template says where a
// character starts -- stats, skills, gear, a look -- and nothing about where he
// goes. What motivates him is his brain (see BrainTemplateRegistry), not this
// file. Nothing here is enforced at runtime: createNPC copies these values onto
// the entity, and the instance owns them from then on, which is why a template's
// skills can be raised and its gear sold without changing the template.
//
// A template is deliberately flat: there is no species/role inheritance. Races
// still supply the body (slots and art) when a template names one, and a map
// entry may override any field -- the map is the instance, the template is the
// starting point.
struct NpcTemplate {
  // The file stem and the id a map names in "template".
  std::string id;
  std::string name;
  std::string description;
  // ActorSpriteRegistry id. Empty falls through to the race's art.
  std::string sprite;
  // ActorRigRegistry id. Empty means the sprite/race look; a rig replaces the look with
  // layered parts and worn equipment, and its clips play when the NPC moves.
  std::string rig;
  // PortraitRegistry id. Empty falls through to the race's portrait.
  std::string portrait;
  // RaceRegistry id. Empty means the human default, exactly like the map key.
  std::string race;
  // The background the agent is given. A brain's background wins when both are
  // set, because the brain is where the model-facing context is authored.
  std::string background;
  // BrainTemplateRegistry id. Empty means no brain: the NPC is spawned with a
  // body and no agent, which is the non-LLM case.
  std::string brain;
  // "neutral" or "aggressive". Aggressive attacks on sight; Neutral does not
  // initiate, and only fights back when it also has `retaliates`.
  std::string disposition = "neutral";
  bool retaliates = false;
  // Solid body for actor collision. The Content Editor can turn this off for a
  // ghost, a decorative creature, or an NPC the player should be able to walk
  // through.
  bool blocksMovement = true;
  // Whether other actors can push this NPC. Defaults off; opt in per template.
  bool pushable = false;
  // IconRegistry id used for this NPC's death sprite. Empty keeps the global
  // combat.deathIcon fallback.
  std::string deathIcon;
  // Constant path speed in pixels per second. NPCs do not accelerate toward it;
  // they move at this value, with only the final-waypoint slow-down retained.
  // Matches the player's default max speed. Authored in the Content Editor.
  float velocity = 100.0f;
  // The simple melee controller's profile. These are per-instance values once
  // spawned; editing them never writes back to the template unless the user
  // saves through the Content Editor.
  float attackDamage = 8.0f;
  float attackInterval = 6.0f;
  // The simple controller's wind-up, strike range and call-out. The defaults
  // match Aggressive's old built-in profile; the sparring dummy authors them.
  float attackRange = 1.0f;
  float attackCastSeconds = 2.0f;
  float attackFirstDelay = 2.0f;
  std::string attackLabel = "Strike";
  // Training dummies are actors with a special reset/death rule. The old object
  // templates also marked them solid, so the template carries both facts.
  bool trainingDummy = false;
  bool obstacle = false;
  float armor = 0.0f;
  // CombatStrategyRegistry id used by the non-LLM chase/tactic layer. Empty
  // means hold position, which is the pre-strategy behavior.
  std::string combatStrategy;
  // BehaviorRegistry id used by the skill-driven combat controller. Empty falls
  // back to the old Aggressive/CombatStrategy layers.
  std::string behavior;
  // BehaviorRegistry id used while this NPC is neutral. Empty keeps the legacy
  // idle behavior.
  std::string neutralBehavior;
  // One profile that maps neutral/aggressive roles to atomic behaviors.
  std::string behaviorProfile;
  // DamageFeedbackRegistry id used for hit flash/shake/clip overrides.
  std::string damageFeedback;
  // Wander controls. Waits are seconds; radius is in tiles and is measured from
  // the NPC's current position each time it chooses a destination.
  float wanderMinWait = 2.0f;
  float wanderMaxWait = 6.0f;
  float wanderRadius = 6.0f;
  // NPC template id this patroller turns hostile against. "player" is accepted
  // as a special id for the player.
  std::string patrolHostileTemplate;
  std::vector<SlotSpec> slots;
  std::vector<ItemStack> inventory;
  std::vector<StartingSkill> startingSkills;
  // Starting values for stats, by stat id. Anything not named keeps the
  // registry's baseline (see GiveDefaultStats).
  std::vector<StatValue> startingStats;
  std::vector<std::string> equipped;
  // Resource pools this template grants. Empty falls through to the race's
  // resources, and then to the default Health pool.
  std::vector<ResourceSpec> resources;
};

// The NPC-template catalogue, loaded from data/npcs/<id>.json.
//
// Same contract as ActorSpriteRegistry and ObjectFactory: a directory of JSON documents,
// a typed view for the game, and AddOrReplace/ToJson for the Content Editor, so one
// generic form edits every registry. Keyed by id in a std::map so listing is stable
// (alphabetical) rather than hash order.
class NpcTemplateRegistry {
public:
  void Load(const std::string& directoryPath);

  // nullptr for an unknown id. The pointer is stable until the next add or replace.
  const NpcTemplate* Get(const std::string& id) const;
  std::vector<const NpcTemplate*> GetAll() const;
  size_t Count() const { return templates.size(); }

  // The document as the Content Editor edits it.
  nlohmann::json ToJson(const NpcTemplate& definition) const;

  // Validates and stores `document`, replacing any entry with the same id.
  bool AddOrReplace(const nlohmann::json& document, const std::string& source,
                    std::string* error, std::vector<std::string>* warnings);

  // The id a document is stored under: the slug of its name.
  static std::string IdFor(const nlohmann::json& document);

  // Where Load read from, for turning an id back into a path.
  const std::string& GetDirectory() const { return directory; }

  // Removes one definition from the live registry. Returns false when the id is
  // unknown. The Content Editor calls this after its confirmation; deleting the
  // definition file is the caller's job.
  bool Remove(const std::string &id) { return templates.erase(id) > 0; }

  void SetDebugLog(DebugLog* log) { debugLog = log; }

private:
  std::map<std::string, NpcTemplate> templates;
  std::string directory;
  DebugLog* debugLog = nullptr;
};
