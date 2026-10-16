# Agent goals and task lists

**Status:** design note, nothing implemented here.
**Scope:** the agent's declared intentions — what it is trying to do, at three horizons, and how that
becomes readable by the engine, by a calibration model, and by a human debugging a village.

**Companion document:** `docs/agent-attention-design.md` owns perception events, the System 1 gate
that decides whether an agent should wake, and the yield mechanism. The seam between the two is one
struct plus one digest renderer: this document defines `TaskList` and the text it produces; that one
consumes it. Neither requires the other to be implemented — see §10.

---

## 1. Symptom: the agent is a goldfish

`AgentBrain::update` has no memory of intent. When an action finishes it appends the success message
and immediately asks for the next one (`src/AgentBrain.cpp:704-717`); when nothing is running it
asks:

```cpp
// src/AgentBrain.cpp:760-763
entity.set<AIRequest>({"System: What is your next command?\n", false, "", false});
```

The only persistent state across turns is `NPCContext::history` — chat. Everything the agent "wants"
lives inside prose in that history, which means:

- the agent re-derives its goal from scratch every turn, and can silently change it;
- there is no way to ask "what is this agent trying to do?" without reading a conversation;
- nothing can be measured against the goal, because the goal is not a thing, it is a sentence in a
  transcript.

The second-order symptom is the one that motivated this whole line of work. A passing NPC cannot be
judged relevant or irrelevant, because there is no declared yardstick to judge it against. A signal
without a reference is noise.

### What this document does not do

Stated up front, because it is the easiest thing to get wrong:

- **It does not schedule actions.** The LLM still chooses actions through `GlobalCommandRegistry`
  and `AgentBrain::action_queue` remains the only scheduler (§2).
- **It does not let the engine assign importance.** The engine reports facts and bounds the model's
  own output; it never decides that one task matters more than another (§6).
- **It does not replace the `AgentAction` layer.** `currentAction` is execution; `TaskList` is
  intent. Both exist, and §7 defines what keeps them honest.

---

## 2. Decision: descriptive first, additive and inert

The task list records intent. It does not steer the agent.

The agent keeps choosing `[COMMAND]`s exactly as it does today. The list is a statement the agent
makes about itself, which the engine can render, validate, and expose. This ordering is deliberate:
it makes the first milestone independently shippable and independently *useful* — you get
village-wide observability of agent intention before any calibration model exists.

Two invariants follow, and they are load-bearing:

1. **An agent with no `TaskList` behaves exactly as it does today.** Every consumer treats the
   component as optional.
2. **If task tracking misbehaves, actions are unaffected.** Removing the feature cannot strand an
   agent, because nothing in the action path consults it.

This is the safest possible landing for a feature that touches every NPC. It also means the
"descriptive" framing is not a stepping stone we are embarrassed about — if it turns out that
observability is 80% of the value, we stop there.

---

## 3. Model

One vector, spanning horizons, with parent edges rather than separate containers. Goals at different
horizons need to share the debug surface and the digest, and the engine needs to walk parents for
bookkeeping (§7).

```cpp
enum class GoalHorizon { Terminal, LongTerm, Short };
enum class TaskPriority { Critical, High, Normal, Low, Background };

struct AgentGoal {
  // Model-authored, free text. This is what a human reads in the debug window.
  std::string description;

  GoalHorizon horizon = GoalHorizon::Short;

  // Index into TaskList::goals, or -1. "This goal serves that one."
  // Proposed by the model -- the engine never derives this edge (see below).
  int parent = -1;

  // ---- Model-authored, structural only ----
  std::string kind;    // task kind, from TaskKindRegistry (§5). Empty for Terminal/LongTerm.
  std::string target;  // human-readable target, resolved by the engine (§7)
  TaskPriority modelTier = TaskPriority::Normal;

  // ---- Engine-authored. The model may not write these. ----
  std::vector<std::string> facts;  // observed world-state predicates, never priorities
  int createdTick = 0;
  int lastAdvancedTick = 0;
  int id = 0;                      // stable identity; see §8 on activeTaskId
};

struct TaskList {
  std::vector<AgentGoal> goals;  // terminal first, then long-term, then short
  int activeTaskId = -1;         // the goal the current activity is serving
};
```

### Why `parent` is model-authored

It is the one edge the engine cannot compute. "I greet people because I want standing, because I
want to matter" is reasoning about meaning. No predicate over `flecs` components produces it. The
engine's only involvement is structural: does the parent index resolve, and does the chain
terminate. See §6.

### Horizons

Terminal and long-term goals carry a description and a parent but no `kind` and no `target`, so the
engine validates almost nothing about them. That is not a gap, it is the design: they are the frame
the short tasks are judged against, and only the model can say what serves them.

Worked example, the one this whole design was aimed at:

```
Goal:      be an important member of the community          Terminal, parent -1
Task 1:    build standing through relationships             LongTerm, parent goal
Task 2:    reap the wheat at the mill field  [HIGH]         Short,    parent task 1
           kind=harvest target=mill field  facts=[mill field ripe]
Activity:  traveling north to the mill field (serving task 2)
```

Under a short-tasks-only list, a passing NPC is correctly irrelevant and the agent never socialises.
With the terminal goal in the digest, the event is obviously on-goal. This is the case that makes the
horizons necessary rather than decorative.

---

## 4. Tiers

Five named buckets, not a numeric scale. Two reasons, both practical:

- Models are poorly calibrated at producing numbers and consistently better at choosing among named
  labels. A label is also stable enough to be a classifier question later.
- A bounded enum is checkable, loggable, and diffable. `Critical` is greppable; `74` is not.

The scale is authored by the model and **never reinterpreted by the engine.** The engine writes only
to `facts`, plus the structural correction in §6.

---

## 5. Task kinds

`kind` is chosen from a registry, not invented per task. The registry is the engine's statement of
which task shapes it can check, and it is the single place a genre pack extends.

```cpp
struct TaskKind {
  std::string id;          // "harvest", "plant", "equip", "craft", ...
  std::string description; // advertised to the model, so it knows the kind exists
  // Predicate over the resolved target. Returns observed facts, never priorities.
  std::function<std::vector<std::string>(flecs::entity agent, flecs::entity target)> check;
};
```

This mirrors the registries the codebase already has — `SkillRegistry`, `BlueprintRegistry`,
`RecipeRegistry`, `TileRegistry` — as a resource component loaded once at init
(`include/Components.h:518-572`).

The four kinds the engine can genuinely check today, all backed by components that already exist:

| kind | resolves to | checkable against |
|---|---|---|
| `harvest` | an object with `Harvestable` | `Harvestable`, `Evolvable` (ripe = final stage) |
| `plant` | a `Placeable` in inventory plus eligible ground | `Placeable`, `Placeable::surface` |
| `equip` | a `Portable` the agent holds | equipment slots, `Requires` |
| `craft` | a `Workstation` and a `CraftRecipe` | `Workstation`, `RecipeRegistry` |

**The escape hatch matters more than the table.** An unknown `kind` — or a `Terminal`/`LongTerm`
goal, which has no kind at all — must degrade to *"no facts, model's tier stands,"* never to
*"task rejected."* Otherwise the model cannot express an intent the engine has not anticipated, and
the whole point of letting it author the list is lost.

---

## 6. What the engine may and may not do

The most important rule in the document:

> **The engine may report facts. The engine may bound the model's own structure. The engine may
> never assign importance.**

| operation | allowed | notes |
|---|---|---|
| Report observed facts | **yes** | `facts: ["hands empty", "mill field ripe"]`. Must be a predicate over world state. |
| Structural clamp on the model's own output | **yes** | §6.1. The replacement value must be one the model itself could have chosen. |
| Reject or delete a task | **no** | Unknown kinds degrade to "no facts," not to removal. |
| Reorder tasks | **no** | Ordering is a model decision. |
| Tune a tier by one step | **no** | See §6.2. |
| Derive a tier from scratch | **no** | This is what makes a genre pack need C++. |

**The test for any proposed validator:** can it be phrased as a predicate over world state? If it
needs to answer "is this important?", it is not a validator.

### 6.1 Structural clamps are allowed

A clamp enforces a rule on the model's own output and needs no domain knowledge. The one we want is
the anti-inflation rule: **at most one `Critical`, and `Critical` requires at least one verified
fact.**

```
model proposes:   #1 harvest mill field  CRITICAL   (facts: mill field ripe)  -> kept
                  #2 plant south field   CRITICAL   (no facts)               -> demoted
                  #3 equip scythe        CRITICAL   (no facts)               -> demoted
                  #4 rest                CRITICAL   (no facts)               -> demoted
```

Without this, a model that marks everything urgent makes any downstream threshold meaningless, and
the calibration model in the companion document has nothing to calibrate against.

The demotion must be **visible**, or the model cannot learn from it (§8), and it must be **reported,
not silent**. A model that keeps re-proposing four `Critical`s should show up as a debug warning, not
as a quiet fight the engine wins every turn.

### 6.2 Rejected: semantic tier adjustment

An earlier draft let the engine nudge a tier when it knew a mechanical fact the model could not see
— for example raising "equip my scythe" because the agent's hands were empty and harvesting is
slower without it. **This is rejected**, and the reasoning is worth keeping because it is the failure
mode to watch for:

1. **It corrupts the model's world model.** The model sees the scythe at `HIGH`, does not know why,
   and may infer a false fact about scythes. Its subsequent priorities rest on a fabricated premise.
2. **There is no enclosure.** Every task kind with a validator would need someone to decide whether
   to nudge up or down, which is domain reasoning — the model's job, reimplemented in C++, badly.
3. **The model cannot audit it.** Its own stated priority changed with no visible cause, which is
   precisely the "my priorities keep shifting and I do not know why" confusion this system is
   supposed to remove.

The mechanical fact is still surfaced — as a fact. `facts: ["hands empty"]` is visible, checkable,
and lets *the model* decide that the scythe now matters. The engine reports; the model judges.

---

## 7. Validation, staleness, and the two sources of truth

### Facts

For each short task with a known `kind`, the engine resolves `target` to an entity or tile and runs
the kind's `check`. Results go into `facts`. The engine never writes to `modelTier`.

Target resolution reuses what `MoveToEntityAction` already does: match `DisplayName` against the
`target` string, falling back to `Map::GetLocations()` (`include/AgentActions.h:159-178`). A target
that resolves to nothing sets `facts: ["target not found"]` and is otherwise left alone.

### Targets may die

`target` is a *name*, not a handle, and is re-resolved rather than cached. The existing
`MoveToEntityAction` already handles a stale referent by re-issuing `AStar` when the target moves
(`include/AgentActions.h:199-203`). A task whose object has been harvested, eaten, or destroyed
should read `facts: ["target is gone"]` and let the model decide whether to drop or replace it.

### Staleness (the failure mode that will bite)

A task list that drifts from the world is worse than no task list, because the calibration model will
confidently act on fiction. "Plant wheat in the empty field" stays on the list after the field is
planted, and the agent is interrupted forever to go plant an already-planted field.

Mitigations, all structural:

- `createdTick` and `lastAdvancedTick` are engine-written. A short task untouched for longer than
  `DEFAULT_TASK_STALE_TICKS` is reported as a debug warning.
- The engine *can* remove a short task whose `kind` reports its own completion (a harvest whose
  target is no longer ripe, a plant whose target surface is now occupied). That is a verified fact
  about the referenced object, not a judgement about importance, so it is inside §6's rule.
- Abstract goals are never auto-removed and never auto-completed. The engine has no idea whether
  "be an important member of the community" has been achieved, and must not pretend otherwise.
- Orphaned short tasks (parent `-1`) are flagged. They are legal but usually mean the model lost the
  thread.

### The two sources of truth

`TaskList.activeTaskId` says what the agent *intends* to be doing. `AgentBrain::currentAction` is
what it is *actually* doing. These can diverge, and divergence is the single most useful debug
signal in the system:

- `activeTaskId` unset while an action is running → the action serves nothing. Normal during
  `InvalidAction` and system chatter, suspicious during `MOVE_TO`.
- `activeTaskId` names an unverified task while a verified-ripe harvest sits above it → the agent is
  doing something it can see is not its stated priority. Print it.

Because actions still schedule themselves (§2), the engine cannot *prevent* divergence. It makes it
visible. If that turns out to be insufficient, the fix is the middle option from the design
discussion — assert the invariant and fail loudly — but that is a later decision, not this document's.

---

## 8. Wire format and delivery

### The `[SET_TASKS]` verb

A hidden `GlobalCommand`, registered in `Game::ECSInit` **before** the `LoadMap` call so it lands in
every NPC's prompt. The block runs from `GlobalCommandRegistry::Clear()` at `src/GameECS.cpp:2078`,
with the 19 existing registrations from `:2080`; `LoadMap` is called at `src/GameECS.cpp:2307`. That
ordering is required — an NPC's prompt is built at spawn time from the registry, so a verb registered
after the spawn is missing from that NPC forever. Hidden, so it does not clutter the advertised
command list, exactly like the trade verbs and `[GENERIC_INTERACT]`.

```
[SET_TASKS
  1 | be an important member of the community | terminal | | | -1;
  2 | build standing through relationships    | longterm | | | 1;
  3 | reap the wheat at the mill field        | short | harvest | mill field | 2 | HIGH;
  4 | equip my scythe                         | short | equip | scythe | 3 | NORMAL ]
```

Fields: `id | description | horizon | kind | target | parent | tier`.

**Whole-list replacement, not diffs.** The model is unreliable at emitting deltas against state it
has to remember, and a diff format makes partial failure ambiguous. Replacement is idempotent and
trivially recoverable, and it costs nothing in context because the list is component state (§9).

The parser must be **tolerant**, because model output is not. A malformed line is skipped with a
warning and the rest of the list still applies; a failure to parse the list at all leaves the
previous list in place. Task `id`s are model-supplied but re-issued by the engine if duplicated or
missing, so `activeTaskId` always resolves.

### How the digest reaches the model

The existing prompt machinery substitutes placeholders at **spawn time only**:
`%LOCATIONS%`, `%BACKGROUND%`, `%CHARACTERS%`, `%COMMANDS_LIST%` are all filled in `Game::createNPC`
and stored once as the `system` turn (`src/GameECS.cpp:311-330`). Task state changes every turn, so
it cannot live there.

The digest therefore goes into `AIRequest.prompt` when the agent wakes, replacing today's bare
`"System: What is your next command?"` (`src/AgentBrain.cpp:760-763`). This keeps `NPCContext` lean:
task churn never becomes permanent history, and the model cannot mistake a stale list of its own past
tasks for current state.

A per-turn `%TASKS%` placeholder in the configurable prompt template is a deliberate non-goal for the
first milestone — it would require re-rendering the `system` turn, which changes the template
contract documented at `src/GameECS.cpp:318-320`.

### Digest format

```
YOUR CURRENT GOALS
  [Terminal]  be an important member of the community
  [LongTerm]  build standing through relationships        (serves: be an important member...)

YOUR CURRENT TASKS
  3. [HIGH]   reap the wheat at the mill field            (engine: mill field ripe)
              serves: build standing through relationships
  4. [HIGH]   equip my scythe                             (engine: hands empty)
  5. [NORMAL] (demoted from CRITICAL: no verified reason)
              plant wheat seeds in the empty field        serves: build standing...
  6. [LOW]    sit by the fire and rest

Current activity: traveling north to the mill field (serving task 3)
```

Three properties this rendering must keep:

- **Both tiers visible where they differ**, with the reason. Silent engine corrections are what §6.2
  rejects; this is the other half of that rule — if the engine bounds the model's output, the model
  must see it happen and be able to correct it.
- **The `serves` edge is re-read every turn**, so the model can notice and repair its own stale links
  rather than silently losing the frame.
- **Capped.** Terminal and long-term goals render as one line each; short tasks render to a fixed
  maximum (`DEFAULT_TASK_DIGEST_MAX`, §11), highest tier first. The digest competes with the command
  list for context, and a dwarf fortress of goals would drown it.

---

## 9. Why a component, not conversation

If the task list lived in `NPCContext::history`, a model that re-lists its tasks every turn would
grow context without bound, read its own stale prior lists, and confuse itself. As a component it
costs no history, every consumer reads the same current value, and — the reason it was proposed —
**it is queryable.**

```cpp
ecs.component<AgentGoal>()
    .member<std::string>("description")
    .member<GoalHorizon>("horizon")        // registered before the vector, same reason as SkillValue
    .member<int>("parent");
ecs.component<std::vector<AgentGoal>>().opaque(std_vector_support<AgentGoal>);
ecs.component<TaskList>().member<std::vector<AgentGoal>>("goals").member<int>("activeTaskId");
```

Reflection registration goes next to the `SkillBlock` block in `Components.h` and follows the same
ordering rule already documented there: a member's type is resolved at registration time, so
`GoalHorizon` and `AgentGoal` must be registered before `std::vector<AgentGoal>`
(`include/Components.h:684-701`).

Editor/undo integration is a non-goal: the task list is regenerated per turn, not authored.

---

## 10. Debug surface

This is the half of the feature that pays for itself immediately, and it is the original motivation:
*"we can't go individually through each agent's context."*

### Transcript

Every task transition is logged with a timestamp: tasks added, removed, completed, demoted, or
flagged stale. Transitions, not states — a per-turn dump of unchanged lists is noise that buries the
signal. The transcript already has a path for entries that must not wake the model
(`AgentBrain::logNote`, `src/AgentBrain.cpp:646-651`), which is exactly the right channel.

### NPC menu

The existing NPC window (see `DebugWindows.cpp:1281`, which already resolves an NPC's current
activity via `Busy`) gains a tasks section: active goal, active task, tier, age, and any divergence
from the current action (§7).

### Village-wide board

A dedicated debug window, one row per agent, sorted by tier then age:

```
NPC          ACTIVE GOAL                 NOW SERVING                    TIER   AGE  FLAGS
Pietro       community standing          reap wheat at mill field       HIGH   12s  ripe
Margarethe   keep the forge running      craft iron ingot               HIGH   40s  hands empty
Tomas        community standing          rest by the fire               LOW    3m   --
```

This is the tool for "why is this agent doing something dumb," answerable at a glance instead of by
reading a conversation. Register it like any other debug window (persisted in
`debug_windows_state.json`).

Two derived warnings worth surfacing here because they are impossible to see in a single context:

- **Starvation** — a `Critical`/`High` task untouched past `DEFAULT_TASK_STALE_TICKS`.
- **Inflation** — an agent demoted on N consecutive turns (§6.1).

### Skill progression hook

`AgentGoal` carries `kind`, and `Trains` already exists to grant `SkillXp` on an activity
(`include/Components.h:496-498`, with the subject-side/recipe-side split documented above it). A task
whose kind matches a progression track can close the loop from intent → action → XP, which is in
scope for the village milestone. Treat this as a follow-up, not part of the first milestone: it
touches a system with its own balance implications.

---

## 11. Tuning constants

All in `include/Defaults.h`, following its existing `DEFAULT_*` convention:

| constant | initial | purpose |
|---|---|---|
| `DEFAULT_TASK_DIGEST_MAX` | `5` | short tasks rendered into the digest |
| `DEFAULT_TASK_LIST_MAX` | `12` | hard cap on stored goals; overflow drops lowest tier, oldest first |
| `DEFAULT_TASK_STALE_TICKS` | `1800` (≈30 s at 60 fps) | age before a task is flagged stale |
| `DEFAULT_TASK_MAX_CRITICAL` | `1` | the anti-inflation clamp (§6.1) |

---

## 12. Implementation stages

Each stage is useful on its own and breaks nothing (§2).

1. **State only.** `GoalHorizon`, `TaskPriority`, `AgentGoal`, `TaskList` in `Components.h` with
   reflection. No consumer, no behaviour change.
2. **Authoring.** `TaskKindRegistry` resource; the `[SET_TASKS]` hidden verb and tolerant parser;
   registration before `LoadMap`.
3. **Reading.** Digest renderer; injection into `AIRequest.prompt`; task transitions into the
   transcript.
4. **Seeing.** Debug window (NPC section + village board). **At this point the feature has already
   delivered its original goal.**
5. **Checking.** Facts for the four known kinds; target resolution and re-resolution; staleness
   flags; the anti-inflation clamp.

Stage 5 is the point at which the companion document's calibration model has anything worth reading.
It is deliberately last, and deliberately separable.

---

## 13. Open questions

Real, unresolved, and not blocking stages 1–4:

1. **Should `modelTier` be replaced by an engine-adjusted tier, or should the engine only demote the
   model's own value?** The current design stores `modelTier`, and the clamp reports rather than
   overwrites. If the model ignores repeated demotions, the fallback is to store an adjusted tier
   and face the visibility problem of §6.2 head-on.
2. **Is a per-turn `[SET_TASKS]` too expensive?** Full-list replacement every turn costs output
   tokens on every deliberation. Alternatives: only on contradiction, every N turns, or on explicit
   request. Retained for now because replacement is idempotent and needs no diff bookkeeping.
3. **Should terminal goals be model-authored or seeded from per-NPC data?** A model that re-invents
   its life goals each session is inconsistent; a map-JSON or race/background-authored pull-quote
   ("Wants: to matter in this village") may be a better source for the summary line. The model then
   *refines* the goal rather than inventing it. This leans on the existing `background` field, and it
   may be the better answer — flagged because it changes who authors the frame.
4. **Can agents read each other's task lists?** Cheap and potentially valuable (shared plans,
   gossip, a merchant knowing who is carrying wheat). It also makes `TaskList` a cross-agent
   dependency, and should be a deliberate decision rather than an accident of it being a component.
5. **Do task lists survive save/load?** Nothing in the tree currently persists agent state across
   sessions, so the default is no.
