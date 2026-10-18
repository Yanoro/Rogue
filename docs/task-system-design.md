# Agent goals and task lists

**Status:** design note, nothing implemented here.
**Scope:** the agent's declared intentions — what it is trying to do, at three horizons, and how that
becomes readable by the engine, by a validation model, and by a human debugging a village.

**Companion document:** `docs/agent-attention-design.md` owns perception events, the System 1 gate
that decides whether an agent should wake, and the yield mechanism. The seam between the two is one
struct plus one digest renderer: this document defines `TaskList` and the text it produces; that one
consumes it. Neither requires the other to be implemented — see §14.

**Toggleable:** this feature is switched on and off by `docs/agent-feature-toggles-design.md`, which
also defines the `LogOnly` state used to evaluate it without changing behaviour.

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
- **It does not let the engine assign importance.** The engine reports facts and reports rule
  violations; it never decides that one task matters more than another (§6).
- **It does not replace the `AgentAction` layer.** `currentAction` is execution; `TaskList` is
  intent. Both exist, and §8 defines what keeps them honest.

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

## 3. Authority: one author, and it is the agent

**The agent owns its task list.** There is exactly one writer, and it is the agent's own model.

This is a deliberate rejection of an alternative that was considered and discarded: an auxiliary
"task agent" reading the agent's chain of thought and deriving the list for it. It was rejected for
concrete reasons, recorded here because the idea is attractive and will be proposed again:

- **Chain of thought is not a reliable input channel.** Reasoning may arrive in a provider-side field
  the engine never sees — the stream callback is a single `(const std::string&)` token
  (`src/AgentBrain.cpp:667-669`), so a reasoning-mode response can leave nothing to read. And the
  codebase already deliberately distrusts CoT as text: `ParseMessageCommand` strips `<think>…</think>`
  before matching, precisely because reasoning polluted command parsing
  (`src/AgentBrain.cpp:565-567`). A consumer built on that channel breaks when a setting changes.
- **Two authors for the most load-bearing state in the system.** When the list disagrees with
  reality, there is no longer one place to look. Debugging — a primary motivation for this feature —
  gets harder, not easier.
- **It does not obviously save tokens.** The filtering benefit applies to *call count*; the cost of
  transmitting a reasoning trace happens before any filtering. Whether the two-model version wins
  overall depends on unmeasured input/output price ratios.

The saving actually available here is not delegation, it is **frequency** (§8).

### A validator is welcome; a second author is not

What *is* in scope is a cheap model that **validates** the agent's proposed list — a consistency
check, not a generator. That lives in the companion document
(`docs/agent-attention-design.md` §6), because it shares the System 1 provider interface.

The division is: the agent authors, System 1 validates, the engine reports facts about the world.

---

## 4. Model

One vector, spanning horizons, with parent edges rather than separate containers. Goals at different
horizons need to share the debug surface and the digest, and the engine needs to walk parents for
bookkeeping (§8).

```cpp
enum class GoalHorizon { Terminal, LongTerm, Short };
enum class TaskPriority { Critical, High, Normal, Low, Background };

struct AgentGoal {
  // Agent-authored, free text. This is what a human reads in the debug window.
  std::string description;

  GoalHorizon horizon = GoalHorizon::Short;

  // Index into TaskList::goals, or -1. "This goal serves that one."
  // Proposed by the agent -- the engine never derives this edge (see below).
  int parent = -1;

  // ---- Authored by the agent ----
  // Empty for Terminal and LongTerm goals: only Short tasks name a kind and a target.
  std::string kind;    // task kind, from TaskKindRegistry (§7)
  std::string target;  // human-readable target, resolved by the engine (§8)
  TaskPriority tier = TaskPriority::Normal;

  // ---- Written by the engine. The agent may not set these. ----
  std::vector<std::string> facts;  // observed world-state predicates, never priorities
  std::string ruleWarning;         // rule violation report, never a corrected value (§6.1)
  int createdTick = 0;
  int lastAdvancedTick = 0;
  int id = 0;                      // stable identity; see §8 on activeTaskId
};

struct TaskList {
  std::vector<AgentGoal> goals;  // terminal first, then long-term, then short
  int activeTaskId = -1;         // the goal the current activity is serving

  // List-level rule violation, written by the engine, never a corrected value (§6.1).
  // Entry-level violations use AgentGoal::ruleWarning instead; the two are separate
  // fields precisely because coherence is a property of the list, not of one entry.
  std::string ruleWarning;
};
```

There is deliberately **one `tier` field and no "engine-adjusted" counterpart.** An earlier draft
stored a corrected value alongside the agent's original; §6 explains why that is rejected.

### Why `parent` is agent-authored

It is the one edge the engine cannot compute. "I greet people because I want standing, because I
want to matter" is reasoning about meaning. No predicate over `flecs` components produces it. The
engine's only involvement is structural: does the parent index resolve, and does the chain terminate.
See §6.

### Horizons

Terminal and long-term goals carry a description and a parent but no `kind` and no `target`, so the
engine validates almost nothing about them. That is not a gap, it is the design: they are the frame
the short tasks are judged against, and only the agent can say what serves them.

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

## 5. Where terminal goals come from: the NPC JSON

Terminal goals are **authored data**, not model output. They are declared per NPC in the map JSON,
exactly like `background`, `inventory`, `skills` and `equipped`.

Rationale:

- A terminal goal is a statement about *who this character is*. A model inventing its life goals each
  session makes a character inconsistent across runs and across saves — and inconsistency is the
  specific thing this whole feature exists to remove.
- It is a debugging win of the same kind as authored `background`: reading the map tells you what an
  NPC wants, without reading a transcript.
- Terminal goals are the frame the agent reasons *inside*, not a conclusion it should reach. Framing
  inputs are authored.

### JSON shape

```json
{
  "name": "Pietro",
  "position": [13, 9],
  "background": "A farmer who has worked this land his whole life.",
  "goals": {
    "terminal": [
      "be an important member of the community",
      "keep the family farm in good standing"
    ]
  }
}
```

An array, because a character can have more than one pull. Keep it short — §9's digest renders each
one as a line, and a character with six life goals has no frame at all.

### Implementation path

This rides the existing authored-field pattern, so nothing new is invented:

- `NPCData` (`include/Map.h:15-52`) gains `std::vector<std::string> terminalGoals`, alongside
  `inventory`, `startingSkills`, `equipped` and `race`, which are all authored-in-JSON fields already
  stamped at spawn.
- `Map::LoadMap` reads `npcJson.value("background", "")` at `src/Map.cpp:197`; the goals array reads
  the same way, a few lines below.
- `Game::createNPC` (`src/GameECS.cpp:203`) seeds `TaskList` with one `Terminal` entry per authored
  goal, each with `parent = -1` and no `kind`/`target`.
- `Map::SaveMap` writes the `npcs[]` array back at `src/Map.cpp:553-561`. Terminal goals are authored
  data and must round-trip; the comment there already establishes that things `NPCData` does not
  model get dropped, so the field must be modelled to survive a save.

### The agent owns everything below the terminal line

Once spawned, the agent revises its own list:

- **It may add, complete, re-tier and re-parent LongTerm and Short goals freely.** These track what it
  is actually doing and must stay accurate, which means the agent must be the one editing them.
- **It may not author Terminal goals.** They are read-only after spawn, and the digest presents them
  that way ("YOUR LIFE GOALS", not "your goals, which you may edit"). Re-deriving a life goal from one
  turn's reasoning is the inconsistency §5 exists to prevent.
- **Whether it may ever refine a terminal goal's wording is open** (§14.3). Default is strictly
  read-only.

---

## 6. Tiers, and what the engine may do

Five named buckets, not a numeric scale. Two reasons, both practical:

- Models are poorly calibrated at producing numbers and consistently better at choosing among named
  labels. A label is also stable enough to be a classifier question later.
- A bounded enum is checkable, loggable, and diffable. `Critical` is greppable; `74` is not.

### One tier, authored by the agent, never written by the engine

`AgentGoal::tier` is set by the agent and **never modified by the engine**. There is deliberately no
second "adjusted" or "engine-approved" tier field.

An earlier draft had the engine *store* a corrected tier alongside the agent's original, so the digest
could show `demoted from CRITICAL`. That is rejected for the same reason §6.2 rejects semantic tier
adjustment: an invisible third value between what the agent decided and what the system acts on is
exactly what makes priorities feel arbitrary and unauditable. One field, one author, visible.

### What the engine may and may not do

The governing rule:

> **The engine may report facts. The engine may report rule violations. The engine may never assign
> importance.**

| operation | allowed | notes |
|---|---|---|
| Report observed facts | **yes** | `facts: ["hands empty", "mill field ripe"]`. Must be a predicate over world state. |
| Report a rule violation | **yes** | §6.1. Reported and attributed, never silently applied. |
| Reject or delete a task | **no** | Unknown kinds degrade to "no facts," not to removal. |
| Rewrite a tier | **no** | Even structurally. The violation is reported instead. |
| Reorder tasks | **no** | Ordering is an agent decision. |
| Derive a tier from scratch | **no** | This is what makes a genre pack need C++. |

**The test for any proposed validator:** can it be phrased as a predicate over world state? If it
needs to answer "is this important?", it is not a validator.

### 6.1 The anti-inflation rule is reported, not enforced

The desired rule: **at most one `Critical`, and `Critical` requires at least one verified fact.** A
list where everything is urgent has no priorities at all, and inflation makes the digest unreadable
and the debug board useless.

How it is handled, and this is the important part:

- The engine **detects** the violation and **records it** in `ruleWarning` on the offending entries,
  or in `TaskList::ruleWarning` when the violation is a property of the whole list.
- The engine **does not rewrite** the tier. `tier` keeps whatever the agent said.
- The digest shows the violation so the agent can correct it on its next revision:

```
  5. [CRITICAL]  plant wheat seeds in the empty field
                 serves: build standing through relationships
                 ⚠ 2 tasks marked CRITICAL; at most 1 is allowed and it needs a verified reason
```

- Repeated violations across revisions are surfaced on the village debug board (§11) — an agent that
  cries wolf every turn is visible, not silently overruled.

Note the scope honestly: because the list is descriptive (§2), an inflated tier currently has **no
control-flow consequence** — it is a readability, ranking, and future-calibration problem. That is
precisely why reporting is sufficient and enforcement would be overreach. If tiers later drive
behaviour, this decision must be revisited, because reporting alone will not be enough.

### 6.2 Rejected: engine-assigned tiers

An earlier draft let the engine adjust a tier when it knew a mechanical fact the agent could not see
— for example raising "equip my scythe" because the agent's hands were empty and harvesting is slower
without it. **This is rejected**, and the reasoning is worth keeping because it is the failure mode to
watch for:

1. **It corrupts the agent's world model.** The agent sees the scythe at `HIGH`, does not know why,
   and may infer a false fact about scythes. Its subsequent priorities rest on a fabricated premise.
2. **There is no enclosure.** Every task kind with a validator would need someone to decide whether to
   nudge up or down, which is domain reasoning — the model's job, reimplemented in C++, badly.
3. **The agent cannot audit it.** Its own stated priority changed with no visible cause, which is
   precisely the "my priorities keep shifting and I do not know why" confusion this system is supposed
   to remove.

The mechanical fact is still surfaced — as a fact. `facts: ["hands empty"]` is visible, checkable, and
lets *the agent* decide that the scythe now matters. The engine reports; the agent judges.

---

## 7. Task kinds

`kind` is chosen from a registry, not invented per task. The registry is the engine's statement of
which task shapes it can check, and it is the single place a genre pack extends.

```cpp
struct TaskKind {
  std::string id;          // "harvest", "plant", "equip", "craft", ...
  std::string description; // advertised to the agent, so it knows the kind exists
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

**The escape hatch matters more than the table.** An unknown `kind` — or a `Terminal`/`LongTerm` goal,
which has no kind at all — must degrade to *"no facts, agent's tier stands,"* never to *"task
rejected."* Otherwise the agent cannot express an intent the engine has not anticipated, and the whole
point of it authoring the list is lost.

---

## 8. Validation, staleness, and update frequency

### Update frequency: event-driven, not per-turn

Task list revisions are expensive — they are agent output tokens. So the list is **not** rewritten
every turn. It is revised when something changed the plan:

- an action completed or failed,
- a `WakeReason` fired (companion document §5),
- the engine completed or invalidated a task by verification (below),
- the agent explicitly chose to re-plan.

Between these, the list stands unchanged. This is the honest fix for "the agent spends reasoning time
on its goals" — the cost was never the authorship, it was doing it every single turn.

### Facts

For each short task with a known `kind`, the engine resolves `target` to an entity or tile and runs
the kind's `check`. Results go into `facts`. The engine never writes to `tier`.

Target resolution reuses what `MoveToEntityAction` already does: match `DisplayName` against the
`target` string, falling back to `Map::GetLocations()` (`include/AgentActions.h:159-178`). A target
that resolves to nothing sets `facts: ["target not found"]` and is otherwise left alone.

### Targets may die

`target` is a *name*, not a handle, and is re-resolved rather than cached. The existing
`MoveToEntityAction` already handles a stale referent by re-issuing `AStar` when the target moves
(`include/AgentActions.h:199-203`). A task whose object has been harvested, eaten, or destroyed
should read `facts: ["target is gone"]` and let the agent decide whether to drop or replace it.

### Staleness (the failure mode that will bite)

A task list that drifts from the world is worse than no task list, because the validation model will
confidently act on fiction. "Plant wheat in the empty field" stays on the list after the field is
planted, and the agent is flagged forever to go plant an already-planted field.

Mitigations, all structural:

- `createdTick` and `lastAdvancedTick` are engine-written. A short task untouched for longer than
  `DEFAULT_TASK_STALE_TICKS` is reported as a debug warning.
- The engine *can* remove a short task whose `kind` reports its own completion (a harvest whose target
  is no longer ripe, a plant whose target surface is now occupied). That is a verified fact about the
  referenced object, not a judgement about importance, so it is inside §6's rule.
- Abstract goals are never auto-removed and never auto-completed. The engine has no idea whether "be
  an important member of the community" has been achieved, and must not pretend otherwise.
- Orphaned short tasks (parent `-1`) are flagged. They are legal but usually mean the agent lost the
  thread.

### The two sources of truth

`TaskList.activeTaskId` says what the agent *intends* to be doing. `AgentBrain::currentAction` is what
it is *actually* doing. These can diverge, and divergence is the single most useful debug signal in
the system:

- `activeTaskId` unset while an action is running → the action serves nothing. Normal during
  `InvalidAction` and system chatter, suspicious during `MOVE_TO`.
- `activeTaskId` names an unverified task while a verified-ripe harvest sits above it → the agent is
  doing something it can see is not its stated priority. Print it.

Because actions still schedule themselves (§2), the engine cannot *prevent* divergence. It makes it
visible.

---

## 9. Wire format and delivery

### The `[SET_TASKS]` verb

A hidden `GlobalCommand`, registered in `Game::ECSInit` **before** the `LoadMap` call so it lands in
every NPC's prompt. The block runs from `GlobalCommandRegistry::Clear()` at `src/GameECS.cpp:2078`,
with the 19 existing registrations from `:2080`; `LoadMap` is called at `src/GameECS.cpp:2307`. That
ordering is required — an NPC's prompt is built at spawn time from the registry, so a verb registered
after the spawn is missing from that NPC forever. Hidden, so it does not clutter the advertised
command list, exactly like the trade verbs and `[GENERIC_INTERACT]`.

```
[SET_TASKS
  2 | build standing through relationships | longterm | | | 1 | ;
  3 | reap the wheat at the mill field     | short | harvest | mill field | 2 | HIGH;
  4 | equip my scythe                      | short | equip | scythe | 3 | NORMAL ]
```

Fields: `id | description | horizon | kind | target | parent | tier`.

**Terminal goals are not settable by this verb.** They are authored data seeded at spawn (§5), and the
digest presents them as read-only. A `terminal` row in `[SET_TASKS]` is ignored with a warning — the
agent cannot re-derive who it is.

**Whole-list replacement, not diffs.** The model is unreliable at emitting deltas against state it has
to remember, and a diff format makes partial failure ambiguous. Replacement is idempotent and
trivially recoverable, and it costs nothing in context because the list is component state (§10).
Terminal goals survive replacement because they are not part of what the verb replaces.

The parser must be **tolerant**, because model output is not. A malformed line is skipped with a
warning and the rest of the list still applies; a failure to parse the list at all leaves the previous
list in place. Task `id`s are model-supplied but re-issued by the engine if duplicated or missing, so
`activeTaskId` always resolves.

### How the digest reaches the agent

The existing prompt machinery substitutes placeholders at **spawn time only**:
`%BACKGROUND%`, `%CHARACTERS%`, `%COMMANDS_LIST%` are all filled in `Game::createNPC` and stored once
as the `system` turn (`src/GameECS.cpp:311-330`). Task state changes, so it cannot live there.

The digest therefore goes into `AIRequest.prompt` when the agent wakes, replacing today's bare
`"System: What is your next command?"` (`src/AgentBrain.cpp:760-763`). This keeps `NPCContext` lean:
task churn never becomes permanent history, and the agent cannot mistake a stale list of its own past
tasks for current state.

A per-turn `%TASKS%` placeholder in the configurable prompt template is a deliberate non-goal — it
would require re-rendering the `system` turn, which changes the template contract documented at
`src/GameECS.cpp:318-320`.

### Digest format

```
YOUR LIFE GOALS  (fixed -- these are who you are, not something to rewrite)
  [Terminal]  be an important member of the community
  [Terminal]  keep the family farm in good standing

YOUR CURRENT GOALS
  [LongTerm]  build standing through relationships        (serves: be an important member...)

YOUR CURRENT TASKS
  3. [HIGH]   reap the wheat at the mill field            (engine: mill field ripe)
              serves: build standing through relationships
  4. [HIGH]   equip my scythe                             (engine: hands empty)
  5. [CRITICAL] plant wheat seeds in the empty field
              serves: build standing through relationships
              ⚠ 2 tasks marked CRITICAL; at most 1 is allowed and it needs a verified reason
  6. [LOW]    sit by the fire and rest

Current activity: traveling north to the mill field (serving task 3)
```

Four properties this rendering must keep:

- **Life goals are separated and labelled read-only**, so the agent is never invited to rewrite them
  (§5).
- **The agent's tier is shown exactly as the agent set it**, with violations annotated rather than
  corrected (§6.1).
- **The `serves` edge is re-read every turn**, so the agent can notice and repair its own stale links
  rather than silently losing the frame.
- **Capped.** Terminal and long-term goals render as one line each; short tasks render to a fixed
  maximum (`DEFAULT_TASK_DIGEST_MAX`, §12), highest tier first. The digest competes with the command
  list for context, and a fortress of goals would drown it.

---

## 10. Why a component, and why it is private

### Component, not history

If the task list lived in `NPCContext::history`, a model that re-lists its tasks every turn would grow
context without bound, read its own stale prior lists, and confuse itself. As a component it costs no
history, every consumer reads the same current value, and **it is queryable.**

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

### Private to its owner: no agent may read another's list

Task lists are **not** readable by other agents. This is a decision, not an oversight:

- **It breaks agent privacy and exposes deception.** An agent whose goal is to trick another would be
  transparent to anyone who could read its list. That does not merely spoil the trick — it *changes
  behaviour*, because an agent that knows it can be read will stop putting real intentions in the list,
  or will actively lie in it. The task list would then be worse than useless: a debug surface that
  misleads in exactly the cases you most want to inspect.
- **It keeps the list honest for its owner.** The list is only useful as a debugging and calibration
  input if it is the agent's real intent. Anything that gives an agent a reason to curate it for an
  audience destroys that.

Consequences, which must hold in code:

- The debug board (§11) is a **developer tool**, not a diegetic capability. It renders NPC lists
  because a human with a debug build should see everything.
- `TaskList` must not be exposed through any interaction, query, `AgentAction`, or command available
  to another agent. A future social feature that wants "what are you working on?" must get the answer
  through *dialogue* — where the agent can lie — not by reading the component.
- The validation model in the companion document reads a single agent's own list for consistency. It
  is engine-side infrastructure, not an in-world observer, and must not aggregate across agents for
  any purpose that leaks back into the world.

Editor/undo integration is a non-goal: the task list is regenerated per revision, not authored.

---

## 11. Debug surface

This is the half of the feature that pays for itself immediately, and it is the original motivation:
*"we can't go individually through each agent's context."*

### Transcript

Every task transition is logged with a timestamp: tasks added, removed, completed, or flagged stale or
in violation. Transitions, not states — a per-turn dump of unchanged lists is noise that buries the
signal. The transcript already has a path for entries that must not wake the model
(`AgentBrain::logNote`, `src/AgentBrain.cpp:646-651`), which is exactly the right channel.

### NPC menu

The existing NPC window (see `DebugWindows.cpp:1281`, which already resolves an NPC's current activity
via `Busy`) gains a tasks section: life goals, active goal, active task, tier, age, and any divergence
from the current action (§8).

### Village-wide board

A dedicated debug window, one row per agent, sorted by tier then age:

```
NPC          LIFE GOAL                    NOW SERVING                 TIER      AGE  FLAGS
Pietro       community standing           reap wheat at mill field    HIGH      12s  ripe
Margarethe   keep the forge respected     craft iron ingot            HIGH      40s  hands empty
Tomas        community standing           rest by the fire            LOW       3m   --
```

This is the tool for "why is this agent doing something dumb," answerable at a glance instead of by
reading a conversation. Register it like any other debug window (persisted in
`debug_windows_state.json`).

Two derived warnings worth surfacing here because they are impossible to see in a single context:

- **Starvation** — a `Critical`/`High` task untouched past `DEFAULT_TASK_STALE_TICKS`.
- **Inflation** — an agent with more than one `Critical` (§6.1), repeated across revisions.

### Skill progression hook

`AgentGoal` carries `kind`, and `Trains` already exists to grant `SkillXp` on an activity
(`include/Components.h:496-498`, with the subject-side/recipe-side split documented above it). A task
whose kind matches a progression track can close the loop from intent → action → XP, which is in scope
for the village milestone. Treat this as a follow-up, not part of the first milestone: it touches a
system with its own balance implications.

---

## 12. Tuning constants

All in `include/Defaults.h`, following its existing `DEFAULT_*` convention:

| constant | initial | purpose |
|---|---|---|
| `DEFAULT_TASK_DIGEST_MAX` | `5` | short tasks rendered into the digest |
| `DEFAULT_TASK_LIST_MAX` | `12` | hard cap on stored goals; overflow drops lowest tier, oldest first |
| `DEFAULT_TASK_STALE_TICKS` | `1800` (≈30 s at 60 fps) | age before a task is flagged stale |
| `DEFAULT_TASK_MAX_CRITICAL` | `1` | the anti-inflation threshold, used for the violation report |

---

## 13. Implementation stages

Each stage is useful on its own and breaks nothing (§2).

1. **State only.** `GoalHorizon`, `TaskPriority`, `AgentGoal`, `TaskList` in `Components.h` with
   reflection. No consumer, no behaviour change.
2. **Authoring.** `NPCData::terminalGoals` and the map-JSON read/write path; `TaskKindRegistry`
   resource; the `[SET_TASKS]` hidden verb and tolerant parser; registration before `LoadMap`.
3. **Reading.** Digest renderer; injection into `AIRequest.prompt`; task transitions into the
   transcript; revision restricted to the events in §8.
4. **Seeing.** Debug window (NPC section + village board). **At this point the feature has already
   delivered its original goal.**
5. **Checking.** Facts for the four known kinds; target resolution and re-resolution; staleness flags;
   the anti-inflation violation report.

Stage 5 is the point at which the companion document's validation model has anything worth reading. It
is deliberately last, and deliberately separable.

---

## 14. Open questions

1. **~~Should the engine overwrite `tier`, or only demote it?~~ Resolved.** One `tier`, authored by
   the agent, never written by the engine; violations are reported, not applied (§6, §6.1). Revisit
   only if tiers ever drive control flow, at which point reporting alone will not be sufficient.
2. **~~Is a per-turn `[SET_TASKS]` too expensive?~~ Resolved.** Revisions are event-driven, not
   per-turn (§8); the cost was the frequency, not the authorship.
3. **~~Model-authored or seeded terminal goals?~~ Resolved.** Authored in the NPC JSON, seeded at
   spawn (§5), with the agent owning everything below the terminal line. **Still open:** may an agent
   ever *refine a terminal goal's wording* through a deliberate, logged revision, or are terminal
   goals strictly read-only for life? Default is strictly read-only. The argument for allowing a
   logged revision is character development; the argument against is exactly the drift §5 exists to
   prevent.
4. **~~Can agents read each other's task lists?~~ Resolved: no.** Privacy, and behaviour distortion —
   see §10. This is a constraint on future features, not just a current choice: any social feature
   that wants another agent's intentions must ask through dialogue, where lying is possible.
5. **Persistence across save/load: not now, planned later.** Nothing in the tree currently persists
   agent state across sessions. Terminal goals round-trip with the map because they are authored; live
   `TaskList` state does not survive, and that is acceptable for now. Recorded so the later decision is
   deliberate rather than accidental.
