# Grouped object summaries and generic navigation interactions

**Status:** **Stage 1 and Stage 2 implemented 2026-10-30.** Stage 1 added the
interaction-key snapshot, stable `InteractionHandler` keys/`requiresArgs`, readable aliases and
`BuildObjectGroups`. Stage 2 switched the single navigator to grouped objects and the
`objects:all` / `inspect` / `approach` / `<verb>:<alias>` grammar, including the
`inspect:<group>:all` detail form and timed-action handoff. `objectSummary=false` keeps the old
per-instance examine path. Stage 3 is the structured-action/`WaitingInteraction` work. This document replaces the per-instance `examine:<entityId>` list
with grouped object summaries and a single interaction grammar. The first functional version
covers movement, inspection and no-argument verbs (`examine`, `harvest`, ...).
Argument-bearing verbs (`craft`, `store`, ...) and timed-interaction waiting are deliberately
deferred.

**Date:** 2026-10-30

**Related:** `docs/llm-s1-navigation-design.md` (the strategist + single navigator prompts and
action vocabulary), `docs/navigation-subagent-design.md` (S1 candidates, `NavigateAction`,
`InteractionTarget`), `docs/spatial-memory-design.md` (`SeenObject` and the observation path),
`include/InteractionRegistry.h` (the object verb registry), `SETTINGS.md`.

---

## 1. The problem

The single navigator currently passes `maxExamine = -1`:

```cpp
// include/AgentActions.h, single-context candidates
candidates = NavigationPlan::BuildCandidates(
    *memory, *here, Settings().navigation.single.maxCandidates,
    Settings().navigation.single.maxDirections, nav.compassRadius,
    walkedCells, Settings().regions.modelNearCount, -1,
    Settings().system1.navigation.examineRadius);
```

`BuildCandidates` reads that as "every observed, non-portable object inside the examine radius":

```cpp
// include/NavigationPlan.hpp
// Examine affordances: every observed, examinable object in the stated radius.
// The single navigator passes maxExamine < 0 and gets all of them...
```

In a recent village run that produced **65 examine candidates**, most of them Apple Trees. Each
one became an action key in STATE, a target digit in the MAP, and an object node in the graph. The
model had to choose among dozens of near-identical keys before it could produce valid JSON.

The two-context S1 already caps this. The single-context navigator does not. The path is also
hardcoded around one verb: the code walks to an object and calls `ExamineNavigationObject`, which
looks up an interaction literally named `Examine`:

```cpp
// include/AgentActions.h
inline std::string ExamineNavigationObject(flecs::entity actor,
                                           flecs::entity object) {
  for (const InteractionHandler &interaction :
       InteractionRegistry::GetAvailableInteractions(object)) {
    if (StringUtils::EqualsIgnoreCase(interaction.name, "Examine")) {
      return interaction.execute(actor, object, "");
    }
  }
  return std::string();
}
```

If an Apple Tree also supports `Harvest`, the obvious next change would add a second candidate
list, then a third for another verb. That is the duplication this design removes.

## 2. Goals

- Show **one grouped line per object profile**, with all of that profile's navigator-visible no-argument verbs, instead of one
  line per object and one list per verb.
- Use one action grammar for movement and object interaction:
  `approach:<objectAlias>`, `<verb>:<objectAlias>`, `inspect:<group>` and `objects:all`.
- Generate verbs from `InteractionRegistry`; never hardcode `Examine`, `Harvest`, `Craft`, etc.
- Get concrete object aliases from `inspect:<group>`, not from the default group summary.
- Support no-argument verbs first: `examine:<objectAlias>`, `harvest:<objectAlias>`, and any future no-argument
  verb.
- When a verb starts a timed action, start it and **return control to the main agent**; do not add
  a waiting phase in the first implementation.
- Keep a rollback/A-B switch: the old per-instance examine list remains available.

## 3. Non-goals

- Supporting `craft`, `store`, `take` and other argument-bearing verbs from the navigator. They
  are deferred; the long-term plan is the structured action object in §11.
- Waiting inside the navigator for a timed action to finish. That is a later phase; v1 hands
  control back.
- Replacing `InteractionRegistry`, `ItemInteractionRegistry`, or the normal command vocabulary.
- Adding salience scoring/ranking. Distance and capability presence are the only ordering rules.
- Changing `Memory::DerivePlaces` or `DeriveRegions`.

## 4. Decisions at a glance

| decision | choice |
|---|---|
| Interaction source | `InteractionRegistry::GetAvailableInteractions(entity)` at observation time. |
| Memory representation | `SeenObject::interactions` (sorted stable verb keys, refreshed on every sighting) and a stable readable `alias`. |
| Grouping | By `templateId` plus the sorted verb-key set. Different capability profiles of the same type are separate groups. |
| Display | `Apple Tree x12 — examine, harvest — nearest south 5`. |
| Default cap | `maxObjectGroups` groups (6 initially), then `objects:all` for the rest. |
| Expansion | `objects:all` expands groups only; `inspect:<group>` lists instances and aliases. |
| Object target grammar | `approach:<objectAlias>` walks; `<verb>:<objectAlias>` walks and executes the verb. |
| Verb source | `InteractionHandler::key`; no hardcoded verb names in navigation. |
| Argument verbs | Not offered in v1. `craft`/`store` are deferred; the long-term form is a structured action object. |
| Timed verbs | Start the action, then `Finish` navigation and hand control to the main agent. No waiting in v1. |
| Immediate verbs | Execute, append the result to `LAST RESULT`, return to `Deciding`. |
| Map targets | Movement/place targets only. Object groups are STATE text, not digits. |
| Detail movement | `objects:all` and `inspect` do not move and do not increment `legs`. |
| Salience | None in the first version. Actionable groups first, then nearest distance; structural groups after actionable ones. |

## 5. Data model

### 5.1 Interaction snapshot on the object

`SeenObject` already carries template-derived data that is refreshed whenever the object is seen:
`templateId`, `name`, `pos`, `portable`, `structural`, `categories`. Add the verb set:

```cpp
// include/Components.h
struct SeenObject {
  ...
  // Stable interaction keys from InteractionRegistry, refreshed on every
  // sighting. Keys are lower-case slugs; display names/descriptions come from
  // the registry when the prompt is rendered.
  std::vector<std::string> interactions;
};
```

`Memory::ObserveObjects` already has the live `flecs::entity` for each observed object, so it can
fill this in the same place that refreshes `PlaceCategories`:

```cpp
// include/SpatialMemory.hpp, ObserveObjects
std::vector<std::string> interactions =
    InteractionRegistry::GetAvailableInteractionKeys(other);
std::sort(interactions.begin(), interactions.end());
interactions.erase(std::unique(interactions.begin(), interactions.end()),
                   interactions.end());
if (record.interactions != interactions) {
  record.interactions = std::move(interactions);
}
```

The handler revalidates state when the action is attempted. The snapshot is evidence, not a
guarantee: an object can become vanished, depleted or unreachable between the decision and the
move.

### 5.2 Stable interaction keys and argument flags

`InteractionHandler` currently has `name`, `canInteract`, `execute` and `description`. Add a stable
key and one deferred-support flag:

```cpp
struct InteractionHandler {
  std::string name;              // AI-facing/display name: "Examine"
  std::string key;               // stable lower-case id: "examine"
  // True when execute() needs arguments that this grammar cannot carry yet.
  // Argument-bearing verbs are not offered by the v1 navigator.
  bool requiresArgs = false;
  ...
};
```

`RegisterComponentInteraction` can derive `key` from the display name when a caller does not set
it explicitly, so existing registrations keep working. The navigation layer uses `key`; the prompt
shows `name`. A registry helper is useful:

```cpp
const InteractionHandler *FindInteraction(const std::string &key);
std::string InteractionDisplayName(const std::string &key);
```

`requiresArgs` is the only policy needed in v1. Timed behavior is detected at execution time from
`Busy` (see §8.3), not from a second metadata flag; a future version may add explicit timing
metadata if runtime detection proves unreliable.

If a verb is not in the current registry (content hot-reload/removal), the renderer falls back to
the key. The action path fails gracefully rather than inventing a handler.

### 5.3 Object groups

A group is one object profile, not one object:

```cpp
struct ObjectGroup {
  std::string key;          // stable group key; see below
  std::string templateId;   // "apple_tree"
  std::string label;        // "Apple Tree"
  int count = 0;            // instances seen in the examine radius
  bool structural = false;
  int nearestDistance = 0;
  std::string nearestDirection;
  GamePosition nearestPos;
  GamePosition nearestApproach; // walkable adjacent cell used by `approach`
  std::uint64_t nearestId = 0;
  std::vector<std::string> interactions; // sorted keys
};
```

Group key construction:

- `templateId` when it is non-empty; otherwise a slug of `name`; otherwise `object`.
- If exactly one capability profile exists for that type, use the type key directly
  (`apple_tree`).
- If the same type has multiple profiles, append a short deterministic suffix derived from the
  capability set (`apple_tree.harvest`, `apple_tree.empty`). The suffix keeps the action keys
  unique without turning the key into a long verb list.
- The group label is the display name (`Apple Tree`); the count is `xN`.

Grouping by `(templateId, sorted verbs)` matters. If only two of twelve apple trees are
harvestable, the prompt must not claim “apple_tree x12 — examine, harvest” for all twelve. Two
lines are honest and still compact.

### 5.4 What is counted

The summary is “what the character remembers seeing and could still approach”, not “what is in
the current field of view”:

- Include `memory.objects` entries that are not vanished, not portable, and not characters.
- Require at least one walkable, unoccupied adjacent cell; otherwise there is no `approach`
  action, and the object is excluded from the actionable groups.
- Use `Chebyshev(object.pos, here) <= examineRadius`; this is the same radius the current examine
  candidates use.
- Keep structural objects in a separate “structures/terrain” bucket (or after all actionable
  groups). They describe the world but usually have no interaction verbs.
- Do not filter by salience. An examined object remains in memory and in its group; `inspect`
  exposes instance aliases, and a later version may suppress examined instances.

### 5.5 Readable instance aliases

Raw `flecs::entity_t` values are a bad model-facing identifier: `harvest:5199` says nothing,
and a human reading the context has to keep a side table. The internal map key stays the entity
id, but every `SeenObject` also carries a stable alias:

```cpp
struct SeenObject {
  ...
  std::string alias;  // "apple_tree-3"
};
```

The alias is assigned once, when the object is first observed:

```text
base = templateId, else slug(display name), else "object"
alias = base + "-" + nextSerial[base]
```

`SpatialMemory::nextObjectAlias` is monotonic for the memory's lifetime, so a destroyed or evicted
object's alias is never reused. The alias is refreshed only when the object's content key changes
(an apple tree overwritten as a chest must not keep calling itself `apple_tree-3`). Re-sighting the
same object leaves it unchanged.

Rules:

- **Aliases are the only instance identifiers in the prompt.** `approach:apple_tree-3`,
  `examine:apple_tree-3`, `harvest:apple_tree-3` are the model-facing forms.
- **The internal entity id stays internal.** The action handler resolves an alias back to the
  remembered object and uses the entity id for the ECS call.
- **Group keys stay separate.** `inspect:apple_tree` names a group; `apple_tree-3` names one
  instance. Exact alias lookup happens before group lookup.
- **Stability beats ordering.** Aliases are assigned in first-sighting order, not nearest-first
  order; re-sorting the prompt must not rename an object.
- **Old aliases fail closed.** An alias for an evicted/vanished/type-changed object is invalid;
  the action uses the normal retry path rather than guessing.
- **Fallback names share the rule.** Content without a `templateId` uses the slugged display name;
  content with neither uses `object-<n>`.

Stage 1 already implements the alias field and monotonic assignment. Stage 2 uses the alias in the
grammar and in `inspect` output; until then the old `examine:<entityId>` path is untouched.

## 6. Prompt shape

### 6.1 Current shape

```text
ACTIONS
  here:north ...
  p5 ...
  examine:5199 walk to the Apple Tree and examine it
  examine:5198 walk to the Apple Tree and examine it
  ... 63 more ...
```

### 6.2 Proposed shape

```text
ACTIONS
  here:north        an unnamed orchard, 1 tile north
  p5                an unnamed road, 2 tiles south-west
  objects:all

OBJECT TARGETS (the key= value below is the group key)
  objects:all             list every object group; does not move
  inspect:<group>         list the nearest instance aliases and verbs
  inspect:<group>:all     list every instance alias in that group
  approach:<objectAlias>  walk to one instance; does not interact
  <verb>:<objectAlias>    walk and use a no-argument verb
                          e.g. examine:apple_tree-1, harvest:apple_tree-1

OBJECTS SEEN (6 of 14 groups; `objects:all` for the rest)
  Apple Tree x12 key=apple_tree  examine, harvest  nearest south 5
  Old Signpost x1   examine              east 15
  Rock Face x5      examine              nearest north-east 6
  Wooden Wall x18   (no interactions)    nearest north 2
```

There are no per-group `approach:apple_tree` or `inspect:apple_tree` action keys. The group line
only supplies the group key for `inspect`; concrete aliases come from the inspect result. The `OBJECT TARGETS`
lines are grammar documentation, not literal choices.

The exact ordering:

1. Actionable groups (groups with at least one verb), nearest first.
2. Non-actionable/structural groups, nearest first.
3. Cap at `maxObjectGroups`; the tail is available through `objects:all`.

No salience score is involved. The ordering is “can I do something with it?” then distance.

### 6.3 Expansion and detail

- `objects:all` — show all groups (still one line each). Does not move.
- `inspect:<group>` — list instances in that group, with aliases and available verbs. Does not move.
- `approach:<objectAlias>` — walk to one instance; sets `InteractionTarget`; does not execute a verb.
- `<verb>:<objectAlias>` — walk to one instance and execute a no-argument verb.

`objects:all` is deliberately groups-only. It answers “what kinds of things are around?” without
turning the prompt back into a 65-line instance list. `inspect` is the explicit instance-level
request.

Example `inspect` result appended to `LAST RESULT`:

```text
INSPECT Apple Tree (key=apple_tree): 12 seen within 22 tiles; showing the nearest 8.
  apple_tree-1  south 5        examine, harvest
  apple_tree-2  south 5        examine, harvest
  apple_tree-3  south-west 5   examine
  ...
Use inspect:apple_tree:all for the remaining 4.
```

The detail cap (`maxGroupDetails`, 8) keeps the on-demand view bounded. The action does not move;
the next decision is a normal model call with the detail in `LAST RESULT`. The model can then use
`examine:apple_tree-1`, `harvest:apple_tree-1`, or `approach:apple_tree-1`.

## 7. Interaction grammar

### 7.1 `inspect:<group>`

- Valid only for a current object group key.
- Returns the nearest `maxGroupDetails` instances with their aliases, distances, directions
  and available no-argument verbs.
- Does not move; does not increment `legs`.
- Sets `lastResult` and returns to `Deciding`.
- An unknown group produces the normal unusable-reply retry, with the valid group keys listed.

### 7.2 `approach:<objectAlias>`

- Valid only for an observed, non-vanished object that has a reachable adjacent cell.
- Walks to the object's approach cell.
- On arrival, sets `InteractionTarget` and returns a short “arrived next to X” result.
- Does not execute a verb; returns to `Deciding`.
- This is the generic movement form when the model wants the object but not a specific verb.

### 7.3 `<verb>:<objectAlias>`

- `<verb>` is an `InteractionHandler::key` in the registry.
- The object must be observed, non-vanished, reachable, and have that verb in its snapshot.
- If `requiresArgs` is true, the action is rejected with a message that the verb needs arguments
  and is not available to the navigator yet.
- The action walks to the object's approach cell, then invokes the handler.
- If the handler returns a non-empty result and no new timed action was started, append the
  result to `LAST RESULT` and return to `Deciding`.
- If the handler starts a timed action, **do not wait**: finish the navigation episode and hand
  control back to the main agent (§8).

`approach` and `<verb>` both consume a leg. `inspect` and `objects:all` do not.

## 8. Timed verbs and control handoff

### 8.1 Why not wait in v1

`Harvest` can create a `HarvestAction`, set `Busy`, and return an empty string while its timer
runs. Waiting correctly needs:

- a new `WaitingInteraction` phase,
- a pending action-entity handle (the target may be destroyed when the harvest completes),
- a completion-result handoff (the harvest outcome currently reports through the agent context,
  not through the navigator),
- an interaction timeout,
- explicit interruption/cancellation semantics.

That is real state and real edge cases. The first implementation should not absorb it.

### 8.2 The v1 behavior

After invoking the handler:

1. Capture whether the actor was already `Busy` and which action entity was current.
2. Execute the verb.
3. If a **new** timed action now owns `Busy`, report “interaction started; returning control” and
   `Finish` the navigation episode.
4. If the handler returned immediate text (an instant harvest, an examine, a failure), append it
   and re-decide.
5. If the handler returned empty but no new timed action appeared, append a short “interaction
   finished” result and re-decide rather than waiting.

Returning control is deliberately simple: the main agent and the existing action/busy systems own
the rest of the interaction. The navigator does not poll, does not hold a pending timer, and
cannot hang on a broken one.

### 8.3 Detecting a timed action

The primary signal is a new `Busy` action entity created by the handler. Capture the pre-existing
`Busy` entity first:

```text
Busy before execution: none / action entity A
Busy after execution:  action entity B
```

- `none -> B` or `A -> B`: a timed interaction started; finish and hand off.
- `none -> none` with a non-empty message: immediate result; re-decide.
- `A -> A` with “already busy”: immediate failure; re-decide.
- `none -> none` with an empty message: treat as finished/started; re-decide with a generic
  result.

A future explicit `startsTimedAction` handler flag can replace this inference if the runtime
signal is ambiguous. It is not part of v1.

### 8.4 What “return control” means for the caller

The navigation action ends with a summary such as:

```text
Started harvesting Apple Tree. The main agent has control while the action completes.
```

The main agent receives the normal harvest completion through the existing action/context path.
The navigator must not append a second completion or assume it knows the reward.

## 9. Interaction registry changes

The registry is the single source of truth for verb names and no-argument support:

```cpp
struct InteractionHandler {
  std::string name;              // display/AI command name
  std::string key;               // stable lower-case key
  bool requiresArgs = false;     // v1 navigator refuses these verbs
  std::function<bool(flecs::entity)> canInteract;
  std::function<std::string(flecs::entity, flecs::entity, std::string)> execute;
  std::string description;
};
```

Current registrations:

- `Examine`: `key = "examine"`, `requiresArgs = false`.
- `Harvest`: `key = "harvest"`, `requiresArgs = false`; timed behavior is runtime-only in v1.
- `Craft`, `Store`, `Take`, `Place`: `requiresArgs = true`; not offered by the navigator.
- Future verbs choose their own `requiresArgs`; no navigation-side verb list.

The navigation layer never compares `name` to `"Examine"`/`"Harvest"`; it looks up by `key`.

## 10. Code integration map

| area | change |
|---|---|
| `include/Components.h` | `SeenObject::interactions` and `SeenObject::alias`; `SpatialMemory::nextObjectAlias`; no ObjectGroup in memory. |
| `include/SpatialMemory.hpp` | `ObserveObjects` snapshots `InteractionRegistry::GetAvailableInteractionKeys`; refreshes on re-sighting; includes `InteractionRegistry.h`. **Stage 1 done.** |
| `include/InteractionRegistry.h/.cpp` | stable `key`, `requiresArgs`, lookup/display helpers; `Craft`/`Store`/`Take` marked `requiresArgs`. **Stage 1 done.** |
| `include/NavigationPlan.hpp` | **Stage 1:** `FindApproachCell`, `ObjectContentKey`, `BuildObjectGroups`. **Stage 2:** object-target grammar, group/instance text, `FindObjectByAlias`; single candidates use `maxExamine=0` so `BuildPilotMap` emits no examine digits. |
| `include/AgentActions.h` | **Stage 2 implemented:** parse `objects:all`, `inspect:<group>[:all]`, `approach:<alias>`, `<verb>:<alias>`; generic `legInteraction`/`legObjectAlias`/`legApproach`; timed verbs finish and hand control back. Empty/truncated replies retry rather than stopping immediately. |
| `include/NavigationStrategist.hpp` | **Stage 2 implemented:** `StripObjectGraph` removes the duplicate per-object graph nodes/edges when grouped objects are active; the single prompt has a concrete JSON shape, forbids `action`/`verb`/`alias`/`think` top-level fields and caps the think block. |
| `include/Settings.h`, `src/Settings.cpp` | **Stage 2 implemented:** `objectSummary` (default true), `maxObjectGroups` (6), `maxGroupDetails` (8), validation. |
| `src/GameECS.cpp` | mark `requiresArgs` at registration; no navigation-specific verb list. **Done in Stage 1.** |
| Navigation window | Stage 1 debug “Object groups” table; it reflects the active no-arg profile in Stage 2. |

`BuildCandidates` currently owns both movement and examine candidates. The group builder should be
shared by `BuildCandidates` and `BuildState` so the action list and the context list cannot
disagree about counts, reachability or nearest direction.

Parsing `inspect` and `<verb>:<objectAlias>` is not the existing "move must be one of the candidate
strings" check. `PollSingle` must resolve these forms against the current groups/observed objects
before the normal candidate lookup; unknown targets use the existing `RetrySingle` budget.

## 11. Deferred: argument-bearing verbs and structured actions

`Craft`, `Store`, `Take` and similar verbs need multiple named arguments. They are hidden from the
navigator in v1. The long-term direction is the structured action object:

```json
"action": {
  "type": "interact",
  "verb": "craft",
  "target": 5199,
  "args": {"recipe": "bread", "count": 3}
}
```

This is cleaner than adding colon-separated arguments to `move`:

- named arguments instead of positional order,
- no string escaping,
- one validation schema per verb from the registry,
- `move`, `approach`, `inspect` and `interact` can share one discriminated action form.

It is a larger schema change and is intentionally out of scope for the first
movement/examine/harvest pass. A flat optional `args` field beside the current `move` string is a
possible intermediate, but not part of this design.

## 12. Settings

Proposed additions under `navigation.single`:

```json
{
  "objectSummary": true,
  "maxObjectGroups": 6,
  "maxGroupDetails": 8
}
```

| field | default | meaning |
|---|---|---|
| `objectSummary` | `true` | Use grouped objects and the generic grammar. `false` restores the current per-instance `examine:<entityId>` list for A/B. |
| `maxObjectGroups` | `6` | Groups shown in the default STATE block. The rest are available through `objects:all`. |
| `maxGroupDetails` | `8` | Instances shown by `inspect:<group>`, nearest first; `inspect:<group>:all` ignores it. There is no separate detail-request cap. |

`maxExamine` remains meaningful for the two-context S1 and for `objectSummary=false`.

## 13. Migration and rollback

- **Stage 1 (implemented 2026-10-30):** interaction snapshot and readable alias on `SeenObject`; stable
  `InteractionHandler::key`; `requiresArgs` on Craft/Store/Take; `BuildObjectGroups`; the
  Navigation window's debug “Object groups” table. Prompt and action list unchanged, so the old
  `examine:<entityId>` path remains the only behaviour.
- **Stage 2 (implemented 2026-10-30):** `objectSummary` defaults true; single candidates use
  `objects:all`/`inspect`/`approach`/`<verb>:<objectAlias>`; per-instance examine keys are absent
  from the single prompt/map; timed verbs start and hand control back. `inspect:<group>:all`
  exposes groups larger than `maxGroupDetails`. The duplicate per-object graph nodes/edges are
  removed from the single observation when groups are active. Empty or truncated model replies
  enter the normal retry budget instead of ending the episode.
- Phase 3: add the structured action object and proper `WaitingInteraction` support for timed and
  argument-bearing verbs.
- The two-context S1 can later use the same `BuildObjectGroups` with its existing `maxExamine`
  and `maxCandidates` caps.

## 14. Testing

Static/unit-style checks:

- grouping by `(templateId, verbs)` produces the expected count and profiles;
- unreachable objects are excluded from actionable groups;
- a missing `templateId` falls back to the display name without key collisions;
- `objects:all` expands groups without expanding instances;
- `inspect` detail is capped and deterministic;
- an unknown verb is never offered;
- a `requiresArgs` verb is rejected with the correct message;
- `objectSummary=false` reproduces the old candidate set.

Prompt checks:

- a village-sized memory that currently produces 65 examine keys produces at most
  `maxObjectGroups` group lines and no per-group action keys;
- an object with two verbs appears once, not once per verb;
- the map target list contains movement/place targets only;
- the prompt contains no `[EXAMINE]`/`[HARVEST]` prose, only the standard grammar.

In-game checks:

- `inspect:apple_tree` returns aliases without moving;
- `examine:apple_tree-1` walks and returns the examination text without finishing navigation;
- `approach:apple_tree-1` walks and sets `InteractionTarget` without interacting;
- `harvest:apple_tree-1` starts the harvest, finishes navigation, and the main agent receives the
  completion;
- `craft`/`store` are not offered as navigator actions;
- A/B `objectSummary` true/false on the same route.

Metrics: prompt bytes/tokens, first-attempt valid JSON, invalid moves, decision latency, legs per
episode, and how often the model uses `inspect` before a verb.

## 15. Open questions

1. **Timed-action detection.** Is a new `Busy` action entity reliable enough, or should
   `InteractionHandler` gain an explicit `startsTimedAction` flag before implementation?
2. **Alias stability.** Aliases are prompt-facing; the internal ids are flecs entity ids, and a
   destroyed/reused entity could make an old alias point at a replacement. Resolve the alias
   against the current `SeenObject` and fail into the normal retry path if it no longer matches.
3. **`inspect` vs `examine` naming.** The names are adjacent and easy for a small model to confuse;
   `details:<group>` or `aliases:<group>` is an alternative if testing shows mistakes.
4. **Structural objects.** Keep them in the same summary after actionable groups, or in a separate
   “structures” paragraph that does not consume `maxObjectGroups`?
5. **Unreachable but seen objects.** Exclude entirely, or show them in `objects:all` with a “no
   approach” marker?
6. **Inspect budget.** Resolved for Stage 2: no separate cap; the normal decision budget applies.
   Revisit only if models loop on `inspect`.
7. **Interaction snapshot freshness.** Snapshot on sighting means a changed entity seen elsewhere
   can be stale until re-sighted. Execution revalidates, but the prompt may offer a verb that now
   fails. Decide whether to refresh from the live entity at decision time when it is still alive.
8. **Map policy.** Should grouped objects get map glyphs but no digits, or should groups appear only
   in STATE? The design removes object digits; the map remains a spatial overview.
9. **`objects:all` scope.** This design expands groups, not instances; `inspect:<group>` expands
   one group's instances. If “show all objects” means all instances, it needs its own cap and
   budget.

## 16. Acceptance criteria

The design is done when:

- the single navigator sees one line per object profile, with all no-argument verbs from the registry;
- a type with two verbs produces one group line and no duplicate per-verb list;
- the default prompt shows at most `maxObjectGroups` groups and `objects:all` can expand them;
- `inspect:<group>` lists aliases only on request and does not move;
- `approach:<objectAlias>` reaches an object and sets `InteractionTarget`;
- `<verb>:<objectAlias>` runs `examine`, `harvest` and future no-argument verbs;
- a timed verb starts, returns control to the main agent, and leaves no navigator wait state;
- argument-bearing verbs are rejected as not yet available, not silently misparsed;
- the Stage 2 object path contains no `Examine`/`Harvest`/`Craft` string checks; the legacy
  `examine:<entityId>` rollback still uses `ExamineNavigationObject`;
- `objectSummary=false` restores the old path for A/B;
- the 65-candidate village case becomes a handful of group lines without losing the ability to
  inspect a signpost or harvest an apple tree.
