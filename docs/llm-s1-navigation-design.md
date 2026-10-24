# LLM Strategist + System 1 Pilot Navigation

**Status:** Stage 1 implemented 2026-10-19. The strategist is a separate call/context on the main AI
backend; the monitors are System 1 `Score` yes/no questions; the observation is the local JSON grid +
the minimal observed graph; the S1 pilot, handoff and fallback are wired into `NavigateAction`.
Stage 2 tuning, the factual-monitor question and the full request/response record remain.

**2026-10-20:** removed the per-episode S1 decision cap and the strategy step limit; the same-target
back-and-forth is now a soft `REPETITION` signal instead of a stop. Resource guards were raised to
600 s episode, 60 s leg, 12 strategist calls and 4 surveys; they are not navigation decisions.
**2026-10-23:** Stage 1 of `docs/region-place-design.md` is implemented: when no place is underfoot,
the S1 candidates include open-ground `here:<direction>` runs over observed walkable cells, and the
S1 state/summary say "open ground". **Stage 1.5** makes the compass direction-first in every case:
one best move per sector — current place run, adjacent place, or open ground — with the nearest
adjacent place reserved and the rest cardinal-first. A place underfoot no longer hides an open
direction. No provisional `KnownPlace` or region node is created. **Stage 2** is also implemented: observed
8×8 base regions within 12 tiles are the current place in open ground, appear in `[PLACES]` and the
strategist graph, and can be named. **Stage 3** adds the terrain aspects: a region reads `an unnamed
sand field (sand 82%, grass 10%)`, and its objects remain aspects inside it. **Stage 4** adds the
8/16/32 parent views and `PlaceQuery::ModelPlaces`: the model sees the semantic places, the current
region and the nearest `regions.modelNearCount` regions, with a coarser parent hidden while a finer
child is selected. `regions.modelNearCount` is wired to the S1 candidates, `[PLACES]` and the
summary; the existing 5/8/24 caps remain. **Stage 5** makes `PlaceQuery::At` prefer a semantic place
underfoot even when the region is smaller, lists semantic places first in `[PLACES]` with `#id`
duplicate-name hints, and labels the graph’s region nodes `[R8]`/`[R16]`/`[R32]`. A named parent
region passes its label to the child containing the parent anchor.

### Examine affordances (2026-10-24)

`[NAVIGATE]` now offers examine candidates: `examine:<objectId>` walks to the observed
object's nearest walkable adjacent cell and executes its `Examine` interaction. The
result becomes the single navigator's next `LAST RESULT`, or `recentExamination` in the
two-context S1 state and strategist observation.

`system1.navigation.examineRadius` defaults to the perception range (22) and is the only
bound; every observed non-character, non-portable, non-vanished object inside it is listed.
The single navigator shows all of them. The two-context S1 caps the examine candidates at
`maxExamine` (default 2), reserved inside `maxCandidates`; the single ignores that cap.

## Single-context navigator (2026-10-24)

`navigation.agent` selects the navigation architecture. `single` is the new default: one
main-model context (`navsingle-<entity>`) holds the whole episode — JSON grid + graph,
ASCII map with target digits, current state, plan, route and last result — and returns
an updated plan and the next move in **one decision call**. `two` restores the previous
strategist + Jev pilot; it is also the fallback when `navigation.single.fallbackToTwo` is
on and the single call times out, is unsure, loses a leg, or repeats an unusable reply
past the `maxAttempts` budget.

Any unusable model reply is retried in the **same** context. That covers invalid JSON
or field types, an invalid plan, a missing `move`, and a move that is not one of the
offered candidates; the rejected assistant reply and a user correction naming the exact
error are appended, and the model is asked for a shorter think and one complete JSON
object. The same `maxTokens` budget is kept. Up to `navigation.single.maxAttempts`
(default 3) consecutive unusable replies are allowed; an accepted reply (a move,
`look_around`, or `arrived`) resets the counter. On the cap, `fallbackToTwo` switches to
the two-context navigator, or — when false — returns control to the main LLM with the
summary.

The main LLM receives all eight compass sectors (`navigation.single.maxDirections = 8`) and
up to 12 total movement options (`maxCandidates = 12`: eight local + four distant). The
two-context S1 keeps its smaller `system1.navigation` caps. Examine candidates are a
separate affordance list and do not consume those slots.

The `road` category is marked `diagonal` in `data/places.json`: `SplitAtJunctions` connects
road cells in all eight directions and keeps a junction cell on the diagonal stretch, so a
diagonal road that crosses a vertical road is one road place rather than a chain of 1-tile
endpoints. The navigator then follows it with the same `here:<direction>` run used for
horizontal and vertical roads.

The single response is a short `<think>` block followed by JSON:

```json
{"plan": {"strategy": "...", "subtask": "...", "monitors": []},
 "move": "here:east", "confidence": 0.7, "done": false,
 "unsure": false, "reason": "one sentence"}
```

The engine validates the move against the candidate set, resolves the target cell, walks
one leg and checks arrival; the model never invents coordinates. The full history is
retained for the quality-first experiment; context compaction/caching is deliberately
left for the optimization pass. `navigation.single.mapRadius` (default 6) controls the
ASCII map; `maxTokens` (500) and `timeoutSeconds` (60) bound one decision.

**2026-10-23 S1 ASCII-map experiment:** `system1.navigation.mapMode` selects the S1's spatial
input: `off` (the current sector prose), `append` (prose + a north-up ASCII map + target list), or
`replace` (map + compact target list instead of the `SURROUNDINGS`/`FRONTIER` prose). The map is
13×13 by default (`mapRadius: 6`), marks `@`, terrain, objects, semantic-place initials and the
candidate target digits, and is built by `BuildPilotMap`. It is context only: the target list and
`ACTIONS` remain the choice vocabulary. Default is `off` so the map can be A/B tested against the
current behavior.
**Date:** 2026-10-19
**Related:** `navigation-subagent-design.md` (the S1 compass/pilot, which this document keeps as
the local executor), `agent-attention-design.md` (waking and interruption), `spatial-memory-design.md`
(what the observation is built from), `agent-exploration-task.md` (the fair-test brief),
`place-derivation-design.md` (semantic places and their aspects), `region-place-design.md`
(perceived base regions and their terrain/aspect categorization).

---

## 1. Decisions at a glance

The six decisions that shape this document, chosen deliberately:

| decision | choice |
|---|---|
| Who is the strategist? | A **separate standard-LLM sub-agent**, called with a curated brief. The main NPC model issues `[NAVIGATE]`, hands over, and resumes with a summary. |
| First version scope | **Hybrid**: the strategist writes a strategy and monitors; the **S1 pilot** chooses local moves; the engine checks handoffs. Full LLM-issued DSL commands are a later stage. |
| Observation | A **compact JSON local grid** for immediate geometry plus a **place/relation graph** for the observed whole picture. ASCII is a debug view, not the model input. |
| Monitor uncertainty | An unknown/low answer is recorded but is **advisory**: it does not gate the leg. Only a **confident** answer matching `handoff_when` hands back. The pilot's own uncertainty or no-move still asks the strategist. |
| Handoff timing | At the **decision point, before the next leg**. The S1's local choice is discarded; the character does not start the leg. |
| Memory scope | **Per episode** first. Persistent route/dead-end/relation memory is a later stage. |

The rest of this document is the detailed design; the open questions at the end are the parts that
still need a decision.

---

## 2. The problem this redesign addresses

The current `[NAVIGATE]` is a single small decision model (System 1) choosing one local action at a
time from a snapshot: the current place, the compass, the visited list, the goal, and the distant
places. It is fast and cheap, and it is good at the local question "which of these compass moves is
the best next step". It is not a long-horizon planner:

- it sees the decision point, not the route or the whole observed space;
- its `Visited` list is place labels, not explored edges or dead ends;
- the `already walked` markers are per target cell and local;
- the action's route, visited and walked sets die with the episode, so a new `[NAVIGATE]` starts
  amnesic;
- the goal is a short string; the S1 never sees the main model's task, the signpost's rumour, or the
  social context;
- at a junction it can choose the road just covered again (the 17:32 run), and a long route can
  exhaust the decision budget before the goal (the 17:59 run).

The failure is not only model size. It is the **state, the representation, and the division of
labour**. A standard LLM with the same per-decision state would still be short-sighted.

The redesign splits the job:

- **Strategist (standard LLM):** takes the goal, the curated context and the observed space; writes
  a long-term strategy and a small set of monitor questions; re-plans when the S1 hands control back.
- **S1 pilot (System 1):** executes the local moves using the existing compass and actions; answers
  the strategist's monitors from the observation; hands back on a trigger or its own uncertainty.
- **Engine:** builds the observed space, moves the character, checks the strategist's handoff rules,
  records everything, and owns the budgets.

The S1 is no longer the navigator. It is the **pilot and the perception/monitor service**.

---

## 3. Goals and non-goals

### Goals

1. Give the navigation episode a **long-term strategy** that survives more than one local decision
   and can name a route or a subgoal ("follow the road east until a signpost; if the road ends, go to
   the market").
2. Keep the **fast, cheap local execution** of the S1 and the compass.
3. Let the strategist define **arbitrary monitor questions** without the engine growing a C++
   predicate for every possible condition.
4. Give both models only **observed information**: no map, no seeds, no undiscovered content.
5. Make the handoff **auditable**: what was asked, how the S1 answered, what triggered the strategist,
   and what the strategist changed.
6. Be implementable in stages, starting from the current `NavigateAction` and S1 loop.

### Non-goals (v1)

- No full LLM-issued DSL command stream; the strategist writes a strategy and monitors, the S1
  executes.
- No persistent relation graph across episodes; the observation graph is per episode.
- No engine-coded strategy conditions. The engine has a **generic monitor/trigger mechanism** and a
  **general observation**, not a library of predicates.
- No replacing `[MOVE_TO]`. The direct verb remains for a single known destination.
- No map access, no authored place seeds, no undiscovered places in the observation.

---

## 4. The cast

| role | model / component | responsibility |
|---|---|---|
| Main model | the NPC's standard LLM | Issues `[NAVIGATE <goal>]`, provides the goal/task/told facts in the brief, resumes with the summary. Does not micromanage the route. |
| Strategist | a separate standard-LLM sub-agent | Reads the curated brief + observation; writes the strategy and monitors; re-plans on handoff. |
| S1 pilot | the System 1 decision model (Jev/Kev) | Answers the local `next` choice and the monitor questions in one batched call; executes the local compass actions; hands back on a trigger or uncertainty. |
| Engine | C++ navigation action + `SpatialMemory`/`PlaceQuery`/A* | Builds the observation, moves the character, checks the strategist's generic handoff rules, records, enforces budgets. |
| Memory | `SpatialMemory` (observed cells, places, objects) | The only source of the observation; no authored map. |

The strategist is **not** the main model. The main model's full conversation is not given to the
strategist; the curated brief carries only what the navigation needs. This keeps the sub-agent's
context small and its behaviour predictable.

---

## 5. Control flow

```
main model: [NAVIGATE <goal>]
        |
        v
NavigationEpisode
  Strategize: strategist(brief + observation) -> strategy + monitors
        |
        v
  Pilot loop:
      S1: local next + monitors (one Ask)
        |
        +-- confident monitor trigger
        |        -> Strategize (re-plan), before the next leg starts
        |
        +-- S1 local unsure / none
        |        -> Strategize (re-plan or abort)
        |
        +-- S1 local arrived
        |        -> Done: summary to the main model
        |
        +-- budget / timeout
        |        -> Done: summary to the main model
        |
        +-- otherwise
                 -> execute one leg, observe, next decision
```

The strategist is called **at the start and on handoff**, not every decision. The S1 is called once
per decision with the local `next` question and the strategist's monitors in the same request.

### 5.1 Episode states

```
Strategizing --strategist answer--> Piloting
Piloting --confident monitor trigger--> Strategizing
Piloting --S1 unsure/none--> Strategizing
Piloting --S1 arrived--> Done
Piloting --budget/timeout/interrupted--> Done or Interrupted
Strategizing --error/unavailable--> S1-only fallback or Done
```

The S1 pilot keeps its existing internal states (`Idle`, `Asking`, `Moving`, `Surveying`) from
`navigation-subagent-design.md`; the episode states are the outer layer.

### 5.2 What a "decision point" is here

The S1 is asked when it reaches a state where a local choice matters: after a leg, after a survey, or
at episode start. In this design the strategist **does not** get control mid-leg; the handoff happens
at the decision point, before the next leg starts. This keeps the state machine simple and makes the
handoff auditable.

---

## 6. The strategist

### 6.1 Identity and context

The strategist is a separate standard-LLM call on the **main AI backend** (`AIBackend`) with its own
navigation `contextId`. It is the same provider as the NPC's model but a different call and context;
it is not the System 1 model. A dedicated navigation profile is a later option, not Stage 1.

Its **curated brief** contains only:

- the goal text from `[NAVIGATE <goal>]` (the main model may carry the signpost/rumour knowledge into
  the goal; that is the intended channel),
- the current task/life goal and any constraints the main model wants the navigator to respect,
- the observed space (Section 8): the local JSON grid + the **minimal** observed graph (current
  place, local compass places/objects, distant places, traversed route, frontier, dead ends),
- the S1's local capabilities and the monitor schema,
- the fair-test rule (observed only, no map/seed access),
- on re-plan: the previous strategy, the handoff reason (which monitor and answer/probability), the
  observed route in the graph, the current place, and the strategies used so far (the engine
  enforces the time budget).

It does **not** contain the main model's full conversation, the authored map, place seeds, or any
unobserved place. The main model can put what it learned from a sign or an NPC into the `[NAVIGATE]`
goal; that is the agent's own knowledge, not a map leak.

### 6.2 Output

The strategist first writes a short `<think>...</think>` block (2–5 sentences) explaining what it
observed and why this strategy; the engine ignores it for planning and the Navigation window shows it.
Then it returns a strict JSON object:

```json
{
  "strategy": "Follow the road east until a signpost. If the road ends, go to the market and ask.",
  "subtask": "follow the road east",
  "monitors": [
    {
      "id": "signpost",
      "question": "Is a signpost or sign visible ahead or nearby?",
      "handoff_when": "yes",
      "min_confidence": 0.6
    },
    {
      "id": "road_end",
      "question": "Does the road ahead end with no walkable continuation?",
      "handoff_when": "yes",
      "min_confidence": 0.6
    }
  ]
}
```

Rules:

- The `<think>` block is the navigator's reasoning, not machine output. It is stripped before parsing
  and kept per strategist call for the debug window; the JSON plan is never shown in the thoughts
  section.
- `strategy` is natural language for the S1's state and for the main-model summary. It is not a DSL
  program in v1.
- `subtask` is the short instruction the S1 sees at every decision.
- `monitors` is 1–3 yes/no questions. The S1 answers each with a System 1 `Score` (the yes/no
  likelihood primitive) from the observation, not from the strategist's prose.
- `handoff_when` is `yes` or `no`: the answer that triggers a handoff.
- `min_confidence` is the minimum **confidence** for an actionable answer, where confidence is
  `2*|p-0.5|`. A confident answer matching `handoff_when` hands back; below it the monitor is
  **unknown**, which is recorded in the debug view but does **not** gate the leg. The pilot's own
  uncertainty/no-move asks the strategist. The monitor's own value overrides the engine's
  `monitorMinConfidence`.
- No direct `move_to`/`explore` commands in v1. Those belong to the full-LLM extension (Section 18).

The engine validates the JSON before use: non-empty strategy, unique monitor ids, non-empty
questions, a yes/no `handoff_when`, a `min_confidence` in 0..1, the strategy/subtask length, and the
absence of a map/coordinate payload that would bypass the observation. Extra monitors beyond
`maxMonitors` are truncated; an invalid plan gets up to `maxAttempts` attempts (default 3: the
first call plus two retries) and then the S1 fallback.

### 6.3 Call cadence and cost

- At episode start.
- On a confident monitor trigger.
- On the S1's `unsure`/`none` after its survey, or when the compass has no local candidates left.
- Not on the S1's `arrived`: in v1 the S1's arrival ends the episode. A strategist-side arrival
  confirmation is an open question (Section 18).

The strategist is a standard LLM call: seconds and tokens, not the S1's 0.25–1 s. It is called a
handful of times per episode, not per leg. The engine's `maxStrategies` and time budget bound it.

### 6.4 Re-planning

On re-plan, the strategist receives the previous strategy, the handoff reason, the strategies used so
far, the observed route (in the graph) and the current observation. The engine enforces the remaining
time and strategy budget. It returns a new strategy/monitors or an explicit `give_up`:

```json
{"give_up": true, "reason": "the road ends at a lake and no route is known"}
```

A give-up ends the episode with the summary rather than spending the whole budget.

### 6.5 Failure

- Invalid JSON or invalid monitors: retry up to `maxAttempts` (default 3). If all attempts fail, keep the **last accepted strategy**
  (its monitors are dropped, since they caused the failing handoff) and let the S1 continue with that
  strategy. The strategist is asked again on the S1's next uncertainty or no-move, not abandoned.
- Strategist timeout/transport failure: retry up to `maxAttempts`; then the same fallback, with the failure reason recorded in the summary
  and the Navigation debug window.
- The fallback is a setting (`fallbackToS1`). A run with no strategist must still navigate with the
  S1.
- A strategist `give_up` is the intended non-arrival stop. The S1's "could not choose" only ends the
  episode when no strategist is configured; otherwise it calls the strategist. When the strategist
  call budget is exhausted, the episode ends with a context summary that says so, keeping the last
  strategy and route.

---

## 7. Monitors

A monitor is the strategist's **question**, evaluated by the S1 at every decision point. It is not an
engine predicate; the engine only implements the generic dispatch.

### 7.1 The answer

The S1 is asked the local `next` and every monitor in one `Ask` call. Each monitor is a System 1
`OptionType::Score` question: the answer is a probability `p` that the condition holds. The engine
derives the answer's confidence as `2*|p-0.5|` and reads:

- the probability `p`,
- the derived confidence,
- the monitor's `handoff_when` (yes/no).

If the confidence is below `min_confidence`, the monitor is **unknown**: the answer is recorded in
the debug view but does not itself hand back and does not stop the leg. A probability of 0.5 is the
canonical unknown; 0.0 and 1.0 are certain. Stage 1 has no explicit `unknown` option. This is
deliberate: a small model answers vague yes/no monitors near 0.5 often, and gating the leg on that
stopped navigation while the pilot's local choice was confident. The pilot's own uncertainty/no-move
is the handoff path (Section 10).

### 7.2 The generic trigger

The engine does not know what a signpost is. It compares the monitor's answer with the strategist's
rule:

```
confidence = 2 * |p - 0.5|
trigger =
    confidence >= monitor.min_confidence
    && ((p >= 0.5) == monitor.handoff_when)   // handoff_when is yes or no
```

An unknown/low answer is recorded (`unsure`, `triggered = false`) but does not hand back. This is
the whole engine-side condition mechanism: the strategist owns the semantics; the engine owns the
dispatch.

### 7.3 Limits

- 1–3 monitors per strategy. A "multitude" of questions at every decision makes the S1's prompt
  longer, degrades its local choice, and costs tokens even though it is the same network call.
- The strategist must choose the monitors for the current subtask; unused monitors are removed on
  re-plan.
- The strategist is told the observation schema so its questions are answerable from the state. A
  question that cannot be grounded in the observation will produce an unknown, which is recorded but
  is advisory; the pilot's own uncertainty/no-move is the handoff path.

### 7.4 Factual vs qualitative questions

The observation (Section 8) carries the facts: places, objects, terrain, exits, frontier, dead ends.
Some monitor questions can be answered from those facts directly (a junction, a dead end, a water
place, a building object). Others are semantic ("is this the village?", "does this look like a
market?", "is that a signpost?").

In v1, the S1 answers all monitors from the observation. The engine's observation is the evidence;
the S1 is the classifier. The strategist's question wording and the observation's completeness are
the tuning knobs. The engine may later answer the purely factual monitors deterministically as a
cross-check (an open question), but the v1 contract is S1-answered monitors and a generic handoff.

---

## 8. The observation

### 8.1 The rule

The observation is built **only** from `SpatialMemory` (observed cells, observed objects, derived
places) and the episode's route. No authored map, no place seeds, no undiscovered content, no
coordinates the agent has not seen. The engine's observation builder is the fair-test boundary.

The observation is per episode in v1. The next `[NAVIGATE]` rebuilds it from the memory; persistent
relations and persistent dead ends are later stages.

### 8.2 The local JSON grid

A compact JSON grid around the agent, with a legend and a cell scale:

```json
{
  "legend": {
    "?": "unknown",
    ".": "open",
    "#": "blocked",
    "~": "water",
    "T": "tree",
    "B": "building",
    "S": "sign",
    "@": "you"
  },
  "origin": [12, 40],
  "scale": 1,
  "radius": 8,
  "facing": "east",
  "rows": [
    "??????????",
    "??..T.....",
    "?..T..S...",
    "?.....@...",
    "?..#......",
    "??????????"
  ]
}
```

The rows above are abbreviated for the document; the real grid is `(2 * radius + 1)` square.

Rules:

- `radius` bounds the view (default 8 tiles). Large open areas are downsampled by `scale` if needed;
  the default is 1 tile per cell.
- Unknown cells are explicitly `?`; the model must not infer through them.
- The grid contains observed terrain/objects, not place seeds or authored map data.
- The grid is for the **local geometry**: obstacles, open space, a lake edge, a building, a sign, the
  agent's position.
- ASCII is a debug rendering of the same data, not the model input.

### 8.3 The place/relation graph

The graph is the observed **whole picture**:

```json
{
  "current": "place:junction8",
  "nodes": [
    {"key": "place:road3", "kind": "place", "category": "road", "aspects": {"road": 1.0},
     "anchor": [10, 50], "visited": true},
    {"key": "place:junction8", "kind": "place", "category": "road", "aspects": {"road": 1.0},
     "anchor": [30, 48], "visited": true},
    {"key": "place:orchard7", "kind": "place", "category": "orchard", "aspects": {"orchard": 0.8},
     "anchor": [12, 44], "visited": false},
    {"key": "obj:signpost", "kind": "object", "category": "signpost", "cell": [14, 47]}
  ],
  "edges": [
    {"from": "place:road3", "to": "place:junction8", "rel": "traversed", "direction": "east",
     "distance": 18, "source": "walked"},
    {"from": "place:road3", "to": "place:orchard7", "rel": "adjacent", "direction": "north",
     "distance": 1, "source": "observed"},
    {"from": "place:junction8", "to": "place:orchard7", "rel": "adjacent", "direction": "north-west",
     "distance": 3, "source": "observed"}
  ],
  "frontier": [
    {"direction": "north", "kind": "place", "key": "place:orchard7"},
    {"direction": "east", "kind": "open", "cells": 6}
  ],
  "route": ["place:road3 (east)", "place:junction8"],
  "dead_ends": []
}
```

Stage 1 builds the **minimal** version of this graph: the current place, the local compass
places/objects, the distant places, the traversed route, the frontier and the dead ends. The relations
are `adjacent`, `traversed` and `visible-from`; the broader compass/containment/egocentric vocabulary
and the persistent relation memory are Stage 3.

Rules:

- **Nodes** are observed places, objects/landmarks, and (later) regions. Keys must be stable across
  the episode: a place anchor + category + label, or an entity id. `KnownPlace.id` is re-derived as
  memory grows and is not a stable key; the graph must not depend on it.
- **Edges** are observed adjacency, traversal, compass relations, containment, and visibility. The
  `rel` vocabulary is intentionally small: `adjacent`, `traversed`, `north-of`/`south-of`/..., `inside`,
  `visible-from`.
- **Egocentric relations** (`left-of`, `right-of`) are only stored when a reference frame is known
  (the observer's facing, the character's facing); the graph otherwise keeps the compass relation. A
  told "left of the miller" belongs to the strategist's brief, not the observed graph, in v1.
- **Frontier** is the observed boundary: unvisited adjacent places, open directions with observed
  walkable cells, visible landmarks. It is what the strategist's strategy can aim at.
- **Dead ends** are places/edges whose observed exits are exhausted or blocked. They are the reason
  the next episode should not re-tread.
- **Route** is the current episode's actual sequence, with the direction and place label.
- The graph is per episode in v1. Persistent relations, told facts and provenance across episodes are
  Stage 3 (Section 18).

### 8.4 The two views

The **strategist** receives the full JSON (local grid + graph + frontier/route) in its brief.

The **S1 pilot** receives a compact natural-language observation, not the full JSON, because it is a
4B decision model with a short input budget:

```
AROUND YOU:
  current: an unnamed road (road 100%), facing east; the road runs east and west.
  north:  orchard, 1 tile; a signpost 2 tiles north-east.
  east:   the road continues 6 tiles; no junction in sight.
  south:  a lake 3 tiles south.
  south-east: a stone building 4 tiles.
ROUTE: came from the west; the west road is already walked; the east road is new.
```

The S1's state also includes the current strategy/subtask ("follow the road east until a signpost"),
the local `ACTIONS` (the existing compass/distant actions), the step count, the budget, and the
visited/walked markers. The monitor questions are in the `Ask`; their evidence is the observation.

### 8.5 Token budgets

The observation is the largest new prompt cost:

- local grid: `radius` 8 → ≤ ~300 characters with a compact legend; downsampled for open terrain;
- graph: cap nodes/edges (e.g. 24 nodes, 48 edges), nearest/salient first;
- route/frontier: compact labels, not the full path;
- the S1's text state: a compact subset; the strategist's JSON: the full observation.

The exact budget is an open question; the design target is "the strategist's brief stays under a few
thousand tokens, the S1's state under a few hundred words".

### 8.6 ASCII

An ASCII rendering of the local grid is useful in the debug window, the transcript and the logs. It is
not the model input; the JSON is.

---

## 9. The S1 pilot

The S1 pilot is the existing compass/local navigation (`navigation-subagent-design.md`) with two
additions: the strategist's strategy/subtask in its state and the monitors in its `Ask`.

### 9.1 The request

At each decision point:

- `state`: the compact observation (Section 8.4), the goal, the strategy/subtask, the step count, the
  local actions, the route/frontier, the budget.
- `questions`:
  - `next`: the local choice over the compass/distant actions and the sentinels, exactly as today;
  - one `Score` (yes/no) question per monitor.

This is **one** S1 call; the monitors do not add a network round trip. They add prompt/output tokens
and the S1's attention load, which is why the limit is small.

### 9.2 The answers

The S1 returns the local `next` and the monitor answers. The engine:

1. resolves the local choice;
2. evaluates the monitor rules (Section 7.2);
3. if a confident monitor trigger fires → handoff **before** the local leg starts;
   an unknown/low monitor answer is advisory and does not block the leg;
4. otherwise executes the leg and loops.

The S1's local choice is not executed when a handoff fires; the character stays at the decision point
and the strategist re-plans.

### 9.3 The S1's uncertainty

- The local `next` below the probability/margin floor is **unsure** (the existing rule). In the
  hybrid, unsure after the one-survey recovery hands control to the strategist instead of ending the
  episode. The strategist can narrow the subtask, change the monitors, or give up.
- `none` similarly hands to the strategist.
- The local `arrived` ends the episode (Section 9.6).

### 9.4 The existing local mechanics

Unchanged from `navigation-subagent-design.md`:

- the compass/distant candidate builder,
- the local move (`MoveToCellAction`),
- the soft repetition signal (a repeated target is reported, not stopped),
- the one-survey recovery,
- the interruption/resume state,
- the per-leg timeout and the resource guards.

The hybrid adds the strategist/monitors around them; it does not rewrite the pilot.

### 9.5 Confidence and calibration

The monitor's `min_confidence` is separate from the local `minProbability`/`minMargin`. Both are
per-backend and need calibration; a Jev confidence is not a Kev confidence. The monitor answer and
the local answer are recorded separately so the calibration is inspectable.

The S1's confidence is a **soft signal**. The strategist should not assume a high-confidence monitor
answer is true; the observation and the recorder are the check. An unknown/low monitor answer is
advisory, not a gate; the pilot's own uncertainty or no-move is the handoff path.

### 9.6 Arrival

The S1's `arrived` is the local judgment that the character is where the goal asks it to be. In v1 it
ends the episode and produces the summary; the strategist is not asked to confirm. If the strategist's
strategy needs confirmation ("is this the village with a market?"), add a monitor for it; the monitor
is answered before the local `arrived`/leg. The main model can still verify the claim with its task
system or a look.

---

## 10. The handoff

### 10.1 When

A handoff fires when, at a decision point:

- a monitor's answer is confident and meets the strategist's `handoff_when` with confidence ≥
  `min_confidence`;
- the S1's local `next` is unsure/none after the survey (the strategist is asked whenever one is
  configured and within its call budget; there is no `strategyReady` gate);
- an unknown/low monitor answer is **not** a handoff; it is recorded for the debug view and the
  pilot keeps moving.
- a repeated target is reported (a soft signal, not by itself a handoff).

It does **not** fire mid-leg. The current leg is not started; the character remains at the decision
point. The handoff is triggered by the same S1 answer that produced the local choice.

### 10.2 What the strategist receives

- the previous strategy and subtask,
- the handoff reason (which monitor, the answer and confidence, or S1 unsure),
- the current observation (the full local grid + graph),
- the observed route in the graph and the current place,
- the strategy count and the resource guards left.

### 10.3 What happens to the S1's local choice

Discarded. The character does not move. The strategist may produce the same subtask; the S1 is asked
again on the next decision. The S1 call that produced the handoff was spent; the leg was not.

### 10.4 Re-plan limits

- `maxStrategies` (default 12) is a cost guard on the strategist calls per episode. When it is
  exhausted the episode ends with a context summary ("strategist call budget was exhausted") that
  keeps the last strategy and route; it does not fall back to the S1's "could not choose".
- The engine does not yet compare consecutive plans for an identical handoff loop; `maxStrategies`
  is the hard bound in Stage 1. An identical-handoff guard is a Stage 2 tuning item.
- The strategist can give up explicitly; that ends the episode.
- The main model can interrupt the whole episode (the attention gate); the S1/strategist state is
  preserved for resume.

### 10.5 The summary

The episode summary to the main model is a **recap**, not a dump of the internal state:

- the outcome: "You arrived at <place> after N legs" on success, otherwise "You stopped ...: <reason>";
- a compact `Journey:` line: the starting point and the route's runs, e.g. "left open ground; took an
  unnamed road; west x2, east x5, north-east x1, north x2 ...; Village". This exposes a backtracking
  route at a glance. The full leg-by-leg route stays in `NavigationDebugState` for the debug window;
- the current place and its aspects, then the nearby remembered places;
- when a nearby place shares the current place's name (the village edge and the market core both
  "Village"), a line naming it and the working command (`[NAME_PLACE #<id> <name>]`) because the
  list's `#id` is not a `[MOVE_TO]` target;
- the strategist side: the number of plans and handoffs, the last handoff reason, and the last
  strategy/subtask. The monitor list is not repeated; the full plan/monitors are in the debug window.

The main model uses this to refine the next `[NAVIGATE]` goal or to switch to `[MOVE_TO]` for a
now-known destination.

---

## 11. Main-model integration

### 11.1 The command

`[NAVIGATE <goal>]` remains the entry point. The command creates a **NavigationEpisode** instead of
the current S1-only loop. The main model's turn is suspended for the episode; the strategist and S1
run; the summary returns to the main model.

The main model can put what it knows into the goal: "find the village with a market; the signpost said
north". That text is the strategist's semantic seed. The strategist does not read the main model's
full conversation.

### 11.2 The curated brief vs the main model's context

The strategist's brief is built by the engine from:

- the `[NAVIGATE]` goal,
- the current task/life goal (a compact task digest),
- the main model's explicit constraints if the goal carries them,
- the observation.

It is deliberately **not** the NPC's `NPCContext`: no conversation, no full task history, no social
transcript. The main model's job is to state the goal and the known facts; the strategist's job is to
plan; the S1's job is to execute locally.

### 11.3 Resume and interruption

When the episode ends, the main model receives the summary as the action's result, as today. The
attention gate can interrupt the episode (a sign, a character, a sudden event); the navigation
episode is pushed/stacked and resumes. This document assumes the existing interruption contract from
`navigation-subagent-design.md` §9.1 and `agent-attention-design.md`; the gate itself is out of scope.

### 11.4 `[MOVE_TO]`

Unchanged. The direct verb remains "one known destination". The verb-selection help (goal-based, not
visibility-based) stays; the strategist is the search verb's engine.

---

## 12. Fair test

The observation must be only what the agent has observed:

- cells, objects and places from `SpatialMemory`, built by the normal vision writer and place
  deriver;
- the episode's route;
- the main model's goal/task/told facts from the brief;
- no authored map, no seeds, no undiscovered place, no coordinates the agent has not seen.

The strategist's world knowledge ("a village usually has a market") is allowed; the map is not. The
strategist's brief says this explicitly.

Told relations ("the blacksmith is north of the market") are the main model's knowledge in v1; they
belong in the `[NAVIGATE]` goal or the brief, with their source. The observed graph does not invent
them. Persistent told/observed relations with provenance are Stage 3.

The recorder keeps the observation, the strategy and the monitor answers, so a run can be replayed and
the fair test audited.

---

## 13. Cost, latency, budgets

- **Strategist**: a standard LLM call; seconds and tokens; at start and per handoff; `maxStrategies`
  bounds it. The main model waits for the episode; this is the accepted trade-off.
- **S1**: one call per decision, as today, plus the monitors' tokens; no extra network; 0.25–1 s
  typical; the 4B's attention is the limit on the monitor count.
- **Observation**: the local grid + graph; the largest new prompt cost. Keep it compact; the strategist
  can tolerate more than the S1.
- **Resource guards**: the S1's `maxSeconds`, `legTimeoutSeconds`, `maxSurveys`; the strategist's
  `maxStrategies`, call timeout, `maxMonitors`, observation caps. There is no per-episode decision
  cap and no back-and-forth stop; the strategist/pilot stop normally. The UI shows the guard
  counters.
- **Local/hosted**: the strategist needs a standard model; a local 4B S1 is fine for the pilot, but
  the strategist is a larger model (hosted or a bigger local). The cost is per episode, not per leg.

---

## 14. Configuration

The implemented keys. The strategist uses the main AI backend with its own navigation context, so
there is no provider/model block in Stage 1. The existing S1 section remains:

```json
"navigation": {
  "strategist": {
    "enabled": true,
    "maxStrategies": 12,
    "maxAttempts": 3,
    "maxMonitors": 3,
    "monitorMinConfidence": 0.6,
    "timeoutSeconds": 30,
    "observation": {
      "localGridRadius": 8,
      "maxGraphNodes": 24,
      "maxGraphEdges": 48,
      "maxTokens": 2000
    },
    "fallbackToS1": true
  }
},
"system1": {
  "navigation": {
    "enabled": true,
    "maxSeconds": 600,
    "legTimeoutSeconds": 60,
    "maxSurveys": 4,
    "minProbability": 0.3,
    "minMargin": 0.15,
    "maxCandidates": 8,
    "maxDirections": 4,
    "compassRadius": 6
  }
}
```

These are the current defaults. The strategist's think time is inside the S1's `maxSeconds`, so a
long strategist call can consume part of the episode's wall clock. These are resource guards, not
navigation decisions; the strategist/pilot stop on arrival, `none`, uncertainty or give-up.

Notes:

- Stage 1 uses the **main AI backend** with a separate `contextId`; a dedicated model/profile is a
  later option, not a key here.
- `fallbackToS1` keeps a run working if the strategist is unavailable.
- The observation caps are per episode; the graph caps keep the brief small. `maxTokens` is parsed
  but not enforced in Stage 1; the grid/graph caps are the actual bound.
- The monitor confidence is per backend; tune it with the S1 records.
- The S1 guards are the pilot's; the strategist's `maxStrategies`/timeout are the outer layer.

---

## 15. Records and debug

- **Stage 1 strategist records**: the `[navigation]` debug-log trace records the strategist start
  (reason, strategy number, model), the accepted plan (subtask + monitors), each handoff, and the
  fallback. The full request/response and the full JSON observation are now persisted to
  `logs/<run-start>/navigator/<Agent>.txt`, including a partial/cut-off stream and the parse error.
- **S1 records**: the existing `System 1` record now holds the local `next` and the monitor
  questions/answers, their probabilities, and the scenario. Its state includes the strategy/subtask
  and the compact observation, so the exact S1 input is inspectable. The same state/answer is also
  in the per-agent navigator log.
- **Handoff record**: the `[navigation]` trace line for the monitor/unsure handoff with the reason;
  the S1 record has the question and answer; the summary has the new plan and the counts.
- **Observation**: the S1's compact text is in the System 1 record and the navigator log. The
  strategist's JSON grid/graph is in the navigator request; the same memory produces it, and the plan summary in the trace
  is the audit trail.
- **Per-NPC navigation window (Stage 1 debug UI)**: the NPC Menu has a `Navigation` column; its
  button opens a per-character window (`NavigationDebugWindow`) reading the last
  `NavigationDebugState` snapshot. Tabs show the strategist plan/monitors, the observation JSON and
  brief, the exact S1 pilot state, the current candidates, and a compact table of the last eight S1
  answers (choice, probability, margin, monitor p/confidence, handoff, latency) plus the latest raw
  S1 reply. The snapshot is kept after the episode ends, so a completed or failed run can be
  inspected. It is read-only.
- **Agent location graph**: the Overview tab draws the observed place/object graph the strategist
  received at the selected call, above the thoughts. The graph header has two toggles: **Hide
  objects** removes every `object` node (and its edges); **Hide names** removes every node label,
  including “you are here”, while the green current ring remains. With names hidden, hovering a node
  shows its full name and region level. The toggles are per NPC Navigation window session. Region
  nodes from `docs/region-place-design.md` appear in this same graph. Nodes are at their observed
  world positions, and all calls share one world-to-screen frame computed from the union of the
  stored call graphs plus the latest observation, so a node keeps the same screen position when the
  selected call changes. The frame expands if a new call adds a node beyond it. Node positions are
  stable by construction: a region uses its grid-cell centre (`regionCell` + `regionSize`), a
  semantic place uses its extent centre rather than the growing top-left `anchor`, and an object
  uses its cell. The current region node is included even when a semantic place is the current key.
  edges are observed or walked; the current place is green, the frontier yellow, visited places
  blue, objects orange, dead ends red. The graph is captured per strategist call, so selecting an
  older call shows the world picture at that point. With no call history the latest observation's
  graph is used.
- **Navigator thoughts**: the Overview tab has a live section for the strategist's `<think>` text.
  The latest call is followed while it streams; buttons select any earlier strategist call (initial
  plan, re-plan, retry), and `Latest` returns to the live one. A call with no `<think>` says so; a
  failed call shows its error. The JSON is not shown here; the raw plan and observation stay in the
  Strategist tab.
- **S1 responses**: the Overview tab shows the S1 answers that followed the selected strategist
  call, between Navigator thoughts and Monitors. Every answer for that call is retained, not only
  the newest eight. With no strategist call (strategist disabled or no call history) it falls back
  to the compact newest-eight S1 list. The old **S1 Responses** tab is gone; the latest raw S1
  response and last S1 error now live in the **Pilot** tab beside the S1 input.
- **Pause/Resume navigator**: the window has a per-NPC Pause/Resume button beside the status.
  Pausing interrupts the current `NavigateAction` and cancels its in-flight S1 ask and strategist
  call; the episode, strategy, route, counters, memory, position and selected snapshots stay
  intact. The brain is paused for that NPC only, so no new step or S1/strategist call starts.
  Resume continues the same episode where it stopped. Other NPCs and the rest of the simulation
  continue. The button is disabled when the NPC has no navigator action.
- Selecting a call also switches the Overview's **Strategy/Subtask**, **Monitors** and **Pilot
  candidates** to that call's snapshot. The candidates are the list the strategist built for that
  call's observation, not the later S1 decision's list. A failed/retry call shows "no plan for this
  call" rather than borrowing an earlier plan. The Strategist tab stays on the latest
  observation/brief/raw reply.
- **Metrics**: arrivals, S1 decisions per episode, strategist calls, handoffs, monitor false
  positives/negatives, route length, revisits, dead ends, latency, tokens/cost.
- **UI**: a navigation/strategist panel (the strategist's plan, the monitors, the answers, the
  handoff history) beside the System 1 window; the transcript gets the summary.

---

## 16. Failure modes

| failure | behaviour |
|---|---|
| Strategist unavailable/timeout/invalid | Retry up to `maxAttempts` (default 3); then `fallbackToS1` (the existing S1-only loop) or end with the summary. |
| Strategist invalid JSON/monitors | Retry once; then fallback/end. |
| S1 unavailable | The strategist cannot execute; fall back to the main model/`[MOVE_TO]`; end with the summary. |
| Monitor unanswerable/unknown/low | Recorded as advisory; it does not block the leg. A confident `handoff_when` answer hands to the strategist; the pilot's own uncertainty/no-move also hands back. |
| Repeated identical handoff | Stop after the second identical trigger to avoid a strategist/S1 loop. |
| S1 unsure/none after survey | Handoff to the strategist; it can narrow the subtask, change monitors, or give up. |
| Observation missing/truncated | The strategist is told the budget; the engine logs the truncation; the S1's state is compact; a truncated graph is not a map leak but it limits the plan. |
| Budget exhausted | End with the summary; the main model gets the route and the current place. |
| Main model interrupts | The episode is pushed; the S1/strategist state is kept; the summary on resume. |
| Fair-test leak | The observation builder and the strategist prompt are audited; no map/seeds in the brief; the recorder shows the exact input. |
| S1 monitor confidence miscalibrated | Per-backend thresholds; the monitor answers and handoffs are recorded; tune with runs. |

---

## 17. Stages and acceptance

### Stage 1 — v1: strategy + monitors + S1 pilot — implemented 2026-10-19

What landed:

- `include/NavigationStrategist.hpp`: the plan parser/validator, the `Score` monitor questions and
  evaluation, the local JSON grid, the minimal observed graph, the compact S1 observation, and the
  strategist prompt.
- `NavigateAction` in `include/AgentActions.h`: the async strategist call on the main `AIBackend`
  with a navigation `contextId`, the `Strategizing` phase, the S1 `Ask` with the local `next` plus
  monitors, the monitor/unsure handoff before the leg, the strategy-preserving fallback, the
  per-episode route and the strategy summary.
- `navigation.strategist` in `Settings.h`/`Settings.cpp` and `SETTINGS.md`.
- the debug-log `[navigation]` trace and the System 1 record's strategy/observation context.

Acceptance:

- **Wiring acceptance (fake strategist AI + fake S1, real quest map): PASS A–H** — initial plan +
  monitor handoff + re-plan + summary; a confident monitor trigger; an unknown monitor does not block a leg; strategist disabled; invalid reply retry (up to `maxAttempts`, default 3)
  + S1 fallback; plan/leg/arrival; survey/unsure handoff; direct parse/evaluate; failed re-plan keeps
  the last strategy, the pilot's unsure calls the strategist again, and the episode ends with the
  strategist's give-up and the last strategy in the summary (not the old "full circle" message).
- The real hosted-model run on the quest map is the next acceptance; the fake harness proves the
  wiring, not the model's plan quality.
- The observation contains no map/seed/unobserved place: the builder only reads `SpatialMemory`.
- The S1's local moves still use the compass, the survey and the soft repetition signal.
- A strategist failure keeps the last accepted strategy (monitors dropped) and the S1 continues;
  the strategist is asked again on the next pilot uncertainty/no-move. A give-up, or the strategist
  call budget being exhausted, ends with a summary that keeps the last strategy, the reason and the
  route.

Still open for Stage 2: the real-model calibration (monitor thresholds, plan quality), the
forest/market/village scenarios, the full strategist request/response record, the identical-handoff
guard, and the factual-vs-qualitative monitor split.

### Stage 2 — tuning and factual monitors

- Tune the observation compression, the strategist prompt, the monitor wording, the confidence
  thresholds, the handoff frequency, the strategist model/profile.
- Evaluate the S1's monitor accuracy from the records; add the engine's factual cross-check where the
  observation already has the fact (an open question, not the v1 contract).
- Compare against the S1-only design on the same scenarios; the metrics.

### Stage 3 — persistent relations and memory

- A persistent spatial-relations memory across episodes: places/landmarks/regions, compass and
  egocentric relations with frames, provenance (observed/told/inferred), confidence, stable node keys.
- Persistent dead ends, traversed edges, and the route; the strategist's brief can include them.
- The relation graph is not a road graph: nodes are places/landmarks/regions; a forest, a market and a
  village are first-class.

### Stage 4 — full LLM DSL control

- The strategist (or a full navigation LLM) issues direct DSL commands (`move_to`, `explore`,
  `survey`, `wait`, conditions/loops) validated by the engine; the S1 becomes the monitor/perception
  service rather than the executor.
- The same observation/monitors/records; the S1's local role shrinks.
- Only if the hybrid's S1 execution is shown to be the remaining bottleneck.

The stages are not a commitment to build all four; each is justified by the previous stage's runs.

---

## 18. Open questions

These are the parts the implementation still leaves for tuning runs:

1. **Strategist model/provider — resolved for Stage 1.** The main AI backend with a separate
   navigation context. A dedicated model/profile and the cost/latency budget remain tuning questions.
2. **Brief contents — partially resolved.** The `[NAVIGATE]` goal plus the task digest reach the
   strategist; the main model's full conversation does not. Whether the engine should also build a
   sign/rumour digest is still open.
3. **Observation budget.** The grid radius 8 and the graph caps are the Stage 1 defaults; the
   `maxTokens` cap is not enforced yet. How much the S1's compact state can carry before the 4B
   degrades is unmeasured.
4. **Monitor count and calibration.** 1–3 and `min_confidence` 0.6 are starting points; the real
   hosted run must show how often a confident monitor trigger fires, and how often the pilot's own
   uncertainty hands back.
5. **Factual vs qualitative monitors.** Stage 1 has the S1 answer every monitor. Should the engine
   answer the factual ones (junction, dead end, water, building) deterministically, leaving the S1
   only the semantic ones? Still open; it changes the authorship and improves reliability.
6. **S1 instruction-following.** The fake harness does not prove the 4B can use the strategist's
   subtask reliably; the real run's records answer this.
7. **Arrival confirmation.** Stage 1 lets the S1's `arrived` end the episode; a strategist-side
   confirmation monitor is a Stage 2 tuning option.
8. **Persistent relations.** The node identity scheme (stable keys), the frames/provenance, the
   observed/told/inferred mix, and the memory lifecycle. Stage 3.
9. **Full LLM DSL.** The command vocabulary, the validator/executor, the plan/replan loop, and whether
   the S1 remains as a monitor. Stage 4.
10. **Attention/interruption.** The handoff mid-leg via the attention gate (the gate itself is outside
    this document), and how the strategist/S1 state is preserved.
11. **Full records.** The strategist request/response and the full JSON observation are persisted in
    `logs/<run-start>/navigator/<Agent>.txt`; the replay reads that file.

---

## 19. References

- `docs/navigation-subagent-design.md` — the S1 compass, `NavigateAction`, budgets, survey, the
  `already walked`/visited markers; the local pilot this document wraps.
- `docs/agent-attention-design.md` — waking, interruption, the main model's control.
- `docs/spatial-memory-design.md` — the observed cells, places, objects, the memory writer/deriver.
- `docs/place-derivation-design.md` — places, categories, aspects, anchors.
- `docs/agent-exploration-task.md` — the fair-test brief, `[LOOK]`, the command vocabulary.
- `SETTINGS.md` — the field reference for the existing and proposed settings.
