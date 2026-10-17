#pragma once

#include <flecs.h>

#include <string>

#include "TaskKindRegistry.h"

struct TaskList;

// The checking pass (docs/task-system-design.md sections 6, 7 and 8).
//
// Recomputes the engine-written half of one agent's list: facts for every short
// task with a known kind, the progress stamp staleness is measured against, and
// the anti-inflation report. It never writes a tier and never removes a task --
// an unknown kind degrades to "no facts", never to rejection -- because the
// governing rule is that the engine may report facts and rule violations but may
// never assign importance.
void VerifyAgentTaskList(flecs::entity agent, TaskList &tasks, int nowTick);

// Whether a fact records the engine failing to observe anything, as opposed to
// observing something. Only the latter is a "verified reason", which is what the
// anti-inflation rule asks a Critical task to have.
bool IsTaskFailureFact(const std::string &fact);

// Resolves a target name the way section 8 prescribes: the agent's own held items
// first (a seed is carried, not standing in the world), then any entity by
// display name, then -- because a location is not an entity -- an entity inside a
// location with that name that the kind says it can be about.
//
// Returns a null entity when nothing matched. Never cached: a name is re-resolved
// on every pass, so an object that was harvested, eaten or destroyed stops
// resolving instead of being acted on through a stale handle.
flecs::entity ResolveTaskTarget(flecs::entity agent, const TaskKind &kind,
                                const std::string &target);
