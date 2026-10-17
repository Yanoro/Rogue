#pragma once

#include <flecs.h>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

// The engine's vocabulary of task shapes, and the single place a genre adds one
// (docs/task-system-design.md section 7).
//
// A kind is the engine's statement of what it can check, not a constraint on
// what the agent may intend: an unknown kind -- or a goal with no kind at all,
// which every terminal and long-term goal is -- degrades to "no facts", never to
// "task rejected". That escape hatch is the reason the agent can author its own
// list at all.
//
// Registered as a resource component (TaskKindRegistryResource) alongside
// SkillRegistryResource and the rest, and populated once at init.
struct TaskKind {
  std::string id;           // "harvest", "plant", "equip", "craft", ...
  std::string description;  // advertised to the agent, so it knows the kind exists
  // Predicate over the *resolved* target. Returns observed facts, never
  // priorities -- section 6's rule is that the engine may report facts and rule
  // violations, but may never assign importance. An unset check means "this kind
  // is known but nothing is inspected yet", which is indistinguishable to
  // callers from an unknown kind and is therefore always safe.
  std::function<std::vector<std::string>(flecs::entity agent, flecs::entity target)> check;

  // Optional. Whether an entity is a candidate subject for this kind.
  //
  // It exists for the one case section 8 leaves open: the agent named a PLACE
  // rather than a thing. "reap the wheat at the mill field" names a location, and
  // a location is not an entity, so the engine has to decide which entity in it
  // the task is about. This is how a kind says what it can be about. Null means
  // "resolve by name only", which is what a kind whose subject is always named
  // directly wants.
  std::function<bool(flecs::entity)> subjectFilter;
};

class TaskKindRegistry {
public:
  // The four kinds the engine can genuinely check today, all backed by
  // components that already exist (section 7): harvest, plant, equip and craft,
  // each with the predicate it is checked by. Adding a fifth is the one thing a
  // genre has to do in C++.
  void RegisterDefaults();

  void Register(TaskKind kind);

  // nullptr for an unknown id. The pointer is stable for the registry's lifetime.
  const TaskKind* Get(const std::string& id) const;

  bool IsKnown(const std::string& id) const { return Get(id) != nullptr; }

  // Sorted, so anything built from them is stable.
  std::vector<std::string> GetIds() const;

  std::vector<const TaskKind*> GetAll() const;

  size_t Count() const { return kinds.size(); }

private:
  std::unordered_map<std::string, TaskKind> kinds;
};
