#pragma once

#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

class DebugLog;

// One predicate in a behavior rule or action. Conditions are deliberately a
// closed vocabulary: content says what it wants to check, but cannot execute
// arbitrary code.
struct BehaviorCondition {
  enum class Kind {
    Always,
    All,
    Any,
    Not,
    SelfHealthPctBelow,
    SelfHealthPctAbove,
    SelfResourcePctBelow,
    SelfResourcePctAbove,
    TargetDistanceAtLeast,
    TargetDistanceAtMost,
    TargetHasStatus,
    TargetLacksStatus,
    SelfHasStatus,
    AlliesWithin,
    EnemiesWithin,
  };

  Kind kind = Kind::Always;
  std::string id;       // resource or status id
  float value = 0.0f;   // threshold, distance
  int count = 0;        // nearby count
  float range = 0.0f;   // nearby radius
  std::vector<BehaviorCondition> children;
};

struct BehaviorAction {
  std::string skillId;
  int priority = 0;
  // Maximum range always comes from the skill itself. A behavior may raise the
  // minimum so pounce is not used at point-blank range.
  float minRange = 0.0f;
  BehaviorCondition when;
};

struct MovementPolicy {
  // chase, kite, flee or hold
  std::string mode = "chase";
  float desiredRange = 1.0f;
  float tolerance = 0.0f;
  bool allowActions = true;
};

struct BehaviorMovementRule {
  std::string id;
  int priority = 0;
  BehaviorCondition enterWhen;
  BehaviorCondition exitWhen;
  bool hasExit = false;
  float minDuration = 0.0f;
  // Maximum time an active rule stays active. 0 means no maximum. The exit
  // condition can still end the rule earlier once minDuration has passed.
  float duration = 0.0f;
  float cooldown = 0.0f;
  MovementPolicy policy;
};

struct BehaviorDef {
  std::string id;
  std::string name;
  std::string description;
  // aggressive or neutral. This is the role the profile selects by mode.
  std::string type = "aggressive";
  // combat, wander or patrol. The implementation used by the behavior.
  std::string kind = "combat";
  float maxTargetDistance = 12.0f;
  float repathSeconds = 0.5f;
  MovementPolicy defaultMovement;
  std::vector<BehaviorMovementRule> rules;
  std::vector<BehaviorAction> actions;
  nlohmann::json document;
};

// Loaded from data/behaviors/<id>.json and edited through the generic Content
// Editor. The original document is kept so editor round-trips preserve fields
// this build does not understand yet.
class BehaviorRegistry {
public:
  void Load(const std::string &directoryPath);

  const BehaviorDef *Get(const std::string &id) const;
  std::vector<const BehaviorDef *> GetAll() const;
  size_t Count() const { return behaviors.size(); }

  nlohmann::json ToJson(const std::string &id) const;
  bool AddOrReplace(const nlohmann::json &document, const std::string &source,
                    std::string *error, std::vector<std::string> *warnings);
  static std::string IdFor(const nlohmann::json &document);

  void SetDebugLog(DebugLog *log) { debugLog = log; }

private:
  std::map<std::string, BehaviorDef> behaviors;
  DebugLog *debugLog = nullptr;
};
