#include "TaskVerification.h"

#include <algorithm>
#include <string>
#include <vector>

#include "AgentBrain.h"
#include "Components.h"
#include "PlaceQuery.hpp"
#include "Defaults.h"
#include "EquipmentRuntime.hpp"
#include "Map.h"
#include "StringUtils.hpp"
#include "TaskList.hpp"

namespace {

// The facts that mean "nothing was observed", as opposed to an observation. They
// are what makes the difference between "this task has a verified reason" and
// "this task is only an assertion".
constexpr const char *kTargetNotFound = "target not found";
constexpr const char *kTargetGone = "target is gone";

bool IsFailureFact(const std::string &fact) {
  return fact == kTargetNotFound || fact == kTargetGone;
}

// Whether a previous pass observed anything about this task. Used to tell a
// target that never resolved ("not found") from one that used to resolve and no
// longer does ("gone"), which is a distinction only the previous facts carry:
// section 8 says a target is re-resolved rather than cached, so there is no
// remembered handle to consult and there should not be one.
bool HadObservation(const std::vector<std::string> &facts) {
  for (const std::string &fact : facts) {
    if (!IsFailureFact(fact)) return true;
  }
  return false;
}

// Whether the goal is the one the agent says it is serving and something is
// actually running. A task the agent is demonstrably working on is not starving,
// however long the world has gone without changing around it.
bool IsBeingWorkedOn(flecs::entity agent, const TaskList &tasks,
                     const AgentGoal &goal) {
  if (tasks.activeTaskId != goal.id) return false;
  const AgentBrainWrapper *wrapper = agent.get<AgentBrainWrapper>();
  return wrapper != nullptr && wrapper->agBrain != nullptr &&
         wrapper->agBrain->getCurrentAction() != nullptr;
}

// Marks the offending entries and, when the violation is a property of the whole
// list rather than of one entry, the list itself. The tier is never touched: the
// agent's own label stays exactly as it wrote it and the violation is reported
// next to it (section 6.1).
void ReportAntiInflation(TaskList &tasks) {
  const int criticalCount = CountCriticalTasks(tasks);

  if (criticalCount > DEFAULT_TASK_MAX_CRITICAL) {
    tasks.ruleWarning =
        std::to_string(criticalCount) + " tasks marked CRITICAL; at most " +
        std::to_string(DEFAULT_TASK_MAX_CRITICAL) +
        " is allowed and it needs a verified reason";
  } else {
    tasks.ruleWarning.clear();
  }

  for (AgentGoal &goal : tasks.goals) {
    if (goal.horizon != GoalHorizon::Short) continue;
    // Recomputed from scratch on every pass, so a violation the agent has since
    // fixed stops being reported.
    goal.ruleWarning.clear();

    if (goal.tier != TaskPriority::Critical) continue;
    // Only short tasks: a terminal or long-term goal carries no kind and no
    // target by construction (section 4), so demanding a fact of one would be the
    // engine asking for something it is forbidden to check.
    if (HadObservation(goal.facts)) continue;
    goal.ruleWarning =
        "marked CRITICAL without a verified reason; nothing was observed about "
        "it";
  }
}

}  // namespace

bool IsTaskFailureFact(const std::string &fact) { return IsFailureFact(fact); }

flecs::entity ResolveTaskTarget(flecs::entity agent, const TaskKind &kind,
                                const std::string &target) {
  if (target.empty()) return flecs::entity::null();

  // 1. Something the agent carries. A seed being planted is in its hand, not
  //    standing in the world, so this has to come first or [PLANT] tasks would
  //    never resolve.
  flecs::entity held = FindHeldItem(agent, target);
  if (held.is_alive()) return held;

  // 2. Any entity by display name, the same case-insensitive match the move verb
  //    makes. Filtering on GamePosition keeps this to things standing in the
  //    world rather than every named child entity.
  flecs::entity named = flecs::entity::null();
  agent.world().filter<DisplayName, GamePosition>().each(
      [&](flecs::entity other, const DisplayName &name, const GamePosition &) {
        if (named.is_alive()) return;
        if (StringUtils::EqualsIgnoreCase(name.name, target)) named = other;
      });
  if (named.is_alive()) return named;

  // 3. A place this agent remembers, by name. A place is not an entity either, so
  //    this only means something when the kind can say which entity inside it the
  //    task is about -- "reap the wheat at the mill field" names a place, and the
  //    kind says a harvestable inside it is what was meant.
  //
  //    Memory, not the map: the agent can only resolve a name it has observed or
  //    been told, which is the point of deleting the location list.
  if (!kind.subjectFilter) return flecs::entity::null();
  const SpatialMemory *memory = agent.get<SpatialMemory>();
  if (memory == nullptr) return flecs::entity::null();

  const KnownPlace *place = PlaceQuery::ByLabel(*memory, target);
  if (place == nullptr) return flecs::entity::null();

  flecs::entity subject = flecs::entity::null();
  agent.world().filter<DisplayName, GamePosition>().each(
      [&](flecs::entity other, const DisplayName &, const GamePosition &pos) {
        if (subject.is_alive()) return;
        if (!kind.subjectFilter(other)) return;
        const std::uint64_t key = VisionQuery::CellKey(pos);
        if (std::find(place->memberCells.begin(), place->memberCells.end(),
                      key) == place->memberCells.end()) {
          return;
        }
        subject = other;
      });
  return subject;
}

void VerifyAgentTaskList(flecs::entity agent, TaskList &tasks, int nowTick) {
  const TaskKindRegistryResource *registryRes =
      agent.world().get<TaskKindRegistryResource>();
  const TaskKindRegistry *kinds = registryRes ? registryRes->registry : nullptr;
  if (kinds == nullptr) return;  // no vocabulary: nothing is checkable

  for (AgentGoal &goal : tasks.goals) {
    // Only short tasks name a kind and a target, so only they are checkable
    // (section 4). Abstract goals are never auto-completed and never checked: the
    // engine has no idea whether "be an important member of the community" has
    // been achieved and must not pretend otherwise.
    if (goal.horizon != GoalHorizon::Short) continue;

    const std::vector<std::string> previousFacts = goal.facts;

    const TaskKind *kind = goal.kind.empty() ? nullptr : kinds->Get(goal.kind);
    if (kind == nullptr) {
      // Unknown kind, or none at all: "no facts", never rejection. This is the
      // escape hatch that lets an agent express an intent the engine has not
      // anticipated, and it is why the agent can author its own list at all.
      goal.facts.clear();
      goal.ruleWarning.clear();
      continue;
    }

    const flecs::entity target = ResolveTaskTarget(agent, *kind, goal.target);
    if (!target.is_alive()) {
      goal.facts = {HadObservation(previousFacts) ? kTargetGone : kTargetNotFound};
    } else if (kind->check) {
      goal.facts = kind->check(agent, target);
    } else {
      goal.facts.clear();  // known kind whose predicate is not written yet
    }

    // Progress, which is what staleness is measured against. Two things count,
    // both engine-visible and neither a judgement: the world changed in a way the
    // check noticed, or the agent is actively working the task right now. Without
    // the second, a long walk to a distant field would flag its own task starving.
    const bool factsChanged = goal.facts != previousFacts;
    const bool firstObservation = goal.lastAdvancedTick <= 0;
    if (factsChanged || firstObservation ||
        IsBeingWorkedOn(agent, tasks, goal)) {
      goal.lastAdvancedTick = nowTick;
    }
  }

  ReportAntiInflation(tasks);
}
