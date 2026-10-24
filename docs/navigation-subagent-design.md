# Navigation by System 1

**Status:** stages 1 and 2 implemented 2026-10-18 — one decision/one leg, then the loop with
re-asking, survey-on-unsure, budgets, no-progress, interruption/resume and the
`system1.navigation` config block. A design change during stage 3 prep added the **local compass**
(stage 2.5): the place underfoot is acted on through directional runs and adjacent places instead of
an engagement mode. Stage 3's tuning surface (`[navigation]` trace, System 1 scenario filter, runbook)
landed the same day; its acceptance is the owner's real Kev-on-CUDA run. The optional stage 4 remains.

**2026-10-19:** the long-term planner is redesigned in `docs/llm-s1-navigation-design.md`
(LLM strategist + S1 pilot + monitor questions). This document remains the S1 compass/pilot reference
and the fallback; its single-model navigation is not the target architecture.

**2026-10-20:** the engine caps changed. `maxDecisions` and the same-target no-progress stop are
gone; a repeated target is a soft `REPETITION` line in the S1/strategist state, not a `Finish`.
Resource guards are now 600 s per episode, 60 s per leg and 4 surveys. The budget/no-progress
sections below are historical; the current behaviour is in `docs/llm-s1-navigation-design.md` and
`SETTINGS.md`.

**Date:** 2026-10-18

**Companions:** `docs/agent-attention-design.md` (what System 1 is, and the rules every call site
obeys), `docs/systemone-local-models.md` (backends, latency, calibration), `docs/place-derivation-design.md`
(places and aspects), `docs/spatial-memory-design.md` (cells, places, dump), `docs/agent-exploration-task.md`
(the fair-test rules this feature must not break).

---

## 1. The problem

The main model is good at deciding *what* Wren wants and bad at, and expensive for, deciding *which
remembered place to walk to next*. Every leg of a walk today is a full main-model turn: the digest,
the sight report, the command list and the growing transcript go out again so the model can say
`[MOVE_TO an unnamed road]`. A five-leg walk is five main-model calls, and the transcript grows on
each one.

The information that decision actually needs is small and already in the engine:

- what the character is trying to find (the main model's intent),
- the places it remembers, with their category scores, distance and direction,
- where it is standing, and what it has already tried.

That is a textbook System 1 `choice` question: a short state in, a calibrated choice out. System 1
has no world model and no memory of the scene (`agent-attention-design.md` §4), so everything it
needs must be handed to it — which is exactly the point: the engine owns the facts, System 1 owns the
choice, and the main model owns the goal.

## 2. Decisions

1. **The main model delegates; System 1 chooses the next place; the engine executes.** The main model
   writes one command with a goal phrase. It does not approve each leg.

2. **One main-model turn per navigation episode.** The loop runs under `AgentAction::update` and
   returns a single summary. This is the entire economic argument: N main-model calls become
   1 main call + N cheap System 1 calls.

3. **Candidates come only from `SpatialMemory.places`.** Never the map, never `Map::GetPlaceSeeds`,
   never an unobserved place, never a coordinate. A place enters the list because the character
   observed its ground and derivation made it.

4. **The engine supplies facts; System 1 supplies the choice.** The engine formats each move's
   label, distance, direction and aspect scores. It does not rank by "likely to contain a village",
   does not prefer roads, and does not interpret the goal. That inference is System 1's.

5. **The question always carries two sentinel options**, `arrived` and `none`. `arrived` is "I am
   already where the goal asks me to be"; `none` is "none of these places is likely to help".
   The protocol has no "none of these", and without both keys the model is forced to pick a place
   even when the honest answer is "stop" or "I am done". The engine does not verify `arrived` — it
   is the model's claim and the summary reports it as such.

6. **Uncertainty gets one look around, then control returns.** If the answer is `none`, or a place
   is chosen below the probability/margin floor, the action requests the same one-shot 360-degree
   survey `[LOOK around]` uses, rebuilds the actions, and asks once more. If it is still unsure,
   the episode stops and the main model gets the summary and the nearby places. The survey is
   bounded (one per decision, a per-episode budget) so "look until something appears" cannot become
   a loop.

7. **The completion message leaves the main model oriented.** Every exit — arrived, none, unsure,
   budget, stuck — carries the reason, the route actually walked, and the nearby places in the same
   agent-readable form `[PLACES]` uses. A wake with no idea where it is is a bug in the handoff.

8. **The loop is interruptible and resumable.** The attention gate may wake the agent mid-episode
   (a signpost enters the percept, the gate says it matters); the navigation action must suspend
   without losing the goal, the visited set or the budget, and resume when the brain comes back to
   it. That is the "yielding" contract from `agent-attention-design.md` §5, applied to this action.

9. **The loop has resource bounds.** A wall-clock budget, a per-leg movement timeout, a survey
   budget, and a strategist-call cost guard. There is no per-episode decision cap and no
   back-and-forth stop: a repeated target is a soft repetition signal, and a navigation that cannot
   decide hands to the strategist or gives up rather than walking forever.

10. **Movement is not reimplemented.** The loop targets a place id and reuses the existing movement
    path (`ApproachCell`, A*, arrival rules). The new prerequisite is a move target that accepts a
    **place id**, because System 1 may choose an unnamed road stretch.

11. **`[NAVIGATE]` is a visible command.** It is a normal tool the main model can choose, documented
    alongside `[MOVE_TO]`. `[MOVE_TO]` stays the verb for "I know the place".

12. **Every call is recorded, with the exact state and reply, and marked non-advisory.** The System 1
    log file is the replay harness: a bad choice is inspected from `logs/<run-start>/system1/`, not reproduced by
    a separate program.

13. **Degrade, never block.** If System 1 is unavailable, or `system1.useMainModel` is true, the
    action does not send anything. It returns the action list to the main model and lets it
    choose with `[MOVE_TO]` — the pre-feature behaviour.

## 3. What this is not

- **Not a pathfinder.** A* still moves the character; this only chooses the destination.
- **Not an omniscient router.** It cannot see the village before the character has. It chooses among
  remembered places, and it can choose badly.
- **Not a planner.** It decides one move at a time, on arrival, with no lookahead and no memory
  beyond what the action pins into the state.
- **Not a goal-setter.** It cannot add or complete a task. The main model's task list remains the
  agent's intentions.
- **Not a prompt channel for arbitrary reasoning.** The goal text is capped and sanitised; the
  engine frames the question and owns the action set.

## 4. The command

```
[NAVIGATE $GOAL]
```

Examples:

```
[NAVIGATE find a village with a market]
[NAVIGATE the village the signpost mentioned; it said north]
[NAVIGATE somewhere quiet to sleep]
```

`[NAVIGATE]` is for the high-level case: the goal is a search, not a named destination. A short trip
to a nearby place the agent can name or see is `[MOVE_TO]`.

`$GOAL` is a short phrase (cap ~200 characters, newlines collapsed, brackets stripped). It is the
agent's own words and may contain what it learned from a signpost, a person or a previous look. The
engine wraps it in a fixed question; it never appends knowledge of its own.

The command is visible in the advertised list and has long-form help. Its help makes the split
explicit: `[NAVIGATE]` is the high-level search verb for a destination the agent cannot yet locate --
finding a village, following a rumour -- and `[MOVE_TO]` is the direct verb for a place the agent can
already name or see, or any short trip to a nearby destination. The split is goal-based, not
visibility-based: `[MOVE_TO]` is one named target that *is* the destination; `[NAVIGATE]` is for a goal
that is not a place you can name or locate, even when the next road is visible. It also says what it
does, that it only knows remembered places and observed directions, and that it stops and reports.

## 5. The request

One `Choice` question. The provider asks it in one request; batching exists for later questions, but
this feature needs only the one (`SystemOneRecorder.h`).

State, as the engine assembles it:

```
AGENT: Wren
YOU ARE: in an unnamed road (road 100%), facing east.
GOAL: find a village with a market; a signpost said the village is north.
STEPS: 2. Visited: an unnamed road (east), an unnamed orchard (north).

ACTIONS, local first:
  here:north  an unnamed orchard, 1 tile north (orchard 55%, road 22%)
  here:east  follow an unnamed road east for 6 tiles (road 100%)
  here:west  follow an unnamed road west for 6 tiles (road 100%)
  p2  an unnamed road, 20 tiles east (road 100%)
  p4  an unnamed road, 20 tiles east (road 100%)
```

The actions are two layers in one list:

- **Local compass** (`here:<direction>`): each sector around the character is a move through the
  current place, into the place next to it, or across open ground. `here:east` on a road may mean the
  road continues east; `here:north` may be the orchard beside it, with its own scores; `here:south`
  may be the open field beyond the road. This is what makes the place underfoot actionable: the
  place describes the direction, but it does not suppress the other sectors.
- **Distant places** (`p<id>`): the remembered places not already on the compass, nearest first. A
  distant move walks to the place's nearest cell; the compass takes over once inside.

The compass is built from observed cells on demand, not stored. It is not a set of temporary
`KnownPlace`s: those would pollute place ids, `[PLACES]`, the overlay and the visited set, and would
need lifetimes and merge rules.

Rules for the compass:

- Look along each of the eight sectors for the first observed place within `compassRadius` (default 6).
- If that place is the current one, the move follows its run to the last observed cell in that
  sector. A run shorter than 2 tiles is a sidestep; an open-ground run may continue through the
  current place's own cells and into the open beyond it, so the direction is not lost.
- If it is another place, the move enters its nearest cell and the line carries that place's scores.
- Each sector keeps its own direction. Two sectors into the same neighbouring place remain two
  compass lines, because the direction is the action; the place is not repeated as a distant `p`.
- At most `maxDirections` local lines (default 4). One slot is reserved for the nearest adjacent
  place; the rest are filled cardinal-first (north, east, south, west, then diagonals), not by run
  length, so a short north run survives longer south runs.
- The current place is never also a distant option; the compass is how you act on it.
- A local direction already taken **from this place** is marked `already walked` in its
  line. It is a fact for the model, not a filter: the engine still offers it, because
  backtracking can be correct (a dead end, a missed turning). At a junction this is what makes
  "I already went west" visible; without it the west run and an unexplored branch can look the
  same. The marker is keyed by the direction's **target cell**: if that cell is on a path the
  character actually walked, the line says so. Place ids are re-derived as the memory grows; the
  walked cells are the stable fact.

  At a junction after walking the west road and coming back:

  ```
  here:west        follow an unnamed road west for 6 tiles (road 100%), already walked
  here:south-east  an unnamed road, 1 tile south-east (road 100%)
  p11              an unnamed road, 8 tiles south (road 100%)
  ```

  The first line is the road just covered; the south-east and the southern road are unwalked.

**Explore, deferred.** The compass already lets the character walk a place's boundary by choosing
successive directions, so a separate "explore" *behaviour* exists without a separate option. What is
not built is a dedicated **boundary sweep**: choose a start boundary cell and follow the perimeter
waypoints without re-asking System 1, reporting at the end. If the tuning runs show the model cannot
sustain a boundary walk through the compass, add that as a multi-leg local mode with a new cell-target
leg sequence — not as temporary memory places. It is deliberately out of v1.

The two sentinels are not repeated in the action list — they are entries in the question's
`options`, which is the definitive set of things the model may answer with.

Questions:

```cpp
Option next;
next.id = "next";
next.type = OptionType::Choice;
next.instructions =
    "Which of these moves is most likely to bring the character closer to the goal?";
next.options = {
    {"here:north", "an unnamed orchard, 1 tile north (orchard 55%, road 22%)"},
    {"here:east", "follow an unnamed road east for 6 tiles (road 100%)"},
    {"here:west", "follow an unnamed road west for 6 tiles (road 100%)"},
    {"p2", "an unnamed road, 20 tiles east (road 100%)"},
    {"arrived", "you are already where the goal asks you to be (in an unnamed road)"},
    {"none", "none of these moves is likely to help; stop and let me reconsider"},
};
```

The two sentinels are **always** present, after the actions. `arrived` is about the goal;
`none` is about the action list. They are separate exits because they mean different things to
the main model: success to be confirmed versus no useful information to act on.

Rules for the state:

- **Short.** One line per action, at most `maxCandidates` (default 8) in total. The provider doc warns
  that a paragraph of context produces a worse calibrated answer than a single sentence, and a 4B
  decision model has a short trained input.
- **No chat history, no map coordinates, no place seeds.** The state is facts about the character's
  own record.
- **The goal text is used verbatim** inside a fixed question; the engine adds no interpretation.
- **Where you are and what you already tried are explicit.** System 1 has no memory of the previous
  step; the action supplies it. The `arrived` option's description says which place you are in (or
  that you are in none), so "have I arrived" is decidable from the state alone.
- **Aspect scores are `mean`**, not `peak`. `peak` names the single best seed and makes one apple tree
  look like an orchard; `mean` is what the area is like (`place-derivation-design.md` §6.2). Use the
  top two aspects whose mean is above a floor, e.g. `road 100%`, `market 62%`.
- **Distance and direction come from `PlaceQuery::DistancePhrase`** for distant places, the same
  wording `[PLACES]` uses, so the two surfaces cannot disagree about where a place is. The compass
  uses `PlaceQuery::CompassWord` and a run length.
- **The verb comes from the shape.** A linear place is *followed*; an area is *walked through*. It is
  the same `IsLinearPlace` fact the direct `[MOVE_TO]` heuristic uses, not a genre rule.

Answer handling:

- `arrived` stops the episode successfully. The engine does not verify the claim; the summary says
  "you judged that you had arrived", and the main model can check its `reach` task or look around.
- `none` means unsure: §7's one-survey recovery runs before control returns.
- `p<id>` resolves with `PlaceQuery::ById`, never by matching a name.
- An unknown key is a model error, not a place: fall back to the highest-probability *valid* key if
  the distribution names one and it clears the floor, otherwise stop as unsure with the error named.
- The probability gate uses `Answer::probabilities[choice]` and the runner-up margin. Hosted Jev's
  `choice` answers carry no `confidence` field (`SystemOneRecorder.h`), so `probabilities` is the only
  honest source. Below `minProbability` the answer counts as unsure: survey, re-ask once, then stop.
  The runner-up margin adds a second test for **distant place** moves only; a local compass move is a
  short, reversible leg, so the top local direction acts on the probability floor alone. A 0.41 west
  vs 0.30 east is the best available guess and walking it is better than standing still; a distant
  0.41 vs 0.30 is a commitment and still surveys.

## 6. Action construction

A pure function over the memory and the action's walked-cell set, so it can be unit-tested without a
world:

```cpp
struct Candidate {
  int placeId = 0;
  GamePosition target{-1, -1};
  std::string key;          // "here:north" or "p12"
  std::string label;        // "an unnamed road (north)"
  std::string description;  // "follow an unnamed road east for 6 tiles (road 100%)"
  int distance = 0;
};

std::vector<Candidate> BuildCandidates(
    const SpatialMemory &memory, GamePosition here,
    int maxOptions, int maxDirections, int compassRadius,
    const std::unordered_set<std::uint64_t> &walkedCells);
```

Rules:

1. For each of the eight sectors, look from `here` for the first observed, walkable, unoccupied
   cell within `compassRadius` that belongs to a place. An unobserved cell ends the search; a
   walkable cell in no place is open ground and the look continues to the place beyond it.
2. If that place is the current one, continue along the sector to the last observed, unoccupied cell
   of the same place. A run shorter than 2 tiles is a sidestep: an open-ground run may cross the
   current place's own cells and continue into the open beyond it, so the direction is not lost.
   The candidate's target is the run's last cell; the description says `follow` for a linear place
   and `walk ... through` for an area, with the run length and the place's scores.
3. If the first place is a different place, the target is its nearest cell and the description
   carries its scores: `an unnamed orchard, 1 tile north (orchard 55%, road 22%)`.
4. If there is no place in the sector, the move is an open-ground run over observed, walkable,
   unoccupied cells, stopping before a different place, an unobserved cell or a blocked one. Its
   target is the last open cell; the label is `open ground (...)`, the description is
   `walk ... through open ground ...`, and `placeId` is 0. No provisional `KnownPlace` is created.
   A run shorter than 2 tiles is dropped.
5. One best move is kept per sector. The nearest adjacent place receives one reserved slot; the
   remaining local slots are filled cardinal-first (north, east, south, west, then the four
   diagonals), not by run length. `maxDirections` and `maxCandidates` bound the list, so a short
   north run is not dropped because the south runs are longer. A place underfoot does not suppress
   an open direction.
6. Distant places are the remembered places not current and not already local. The target is
   `NearestWalkableCell`; the list is nearest first and fills the remaining `maxCandidates`.
7. Mark `already walked` when the candidate's **target cell** is in `walkedCells`; mark `visited` when
   any member cell of the candidate's place is in `walkedCells`. These are the action's own movement
   history (the actual leg paths and endpoints), not world knowledge. They are cell-based because
   place ids are re-derived as the memory grows; the `17:32` run showed a stale id marking an
   unentered junction branch as visited.
8. Never include the map's authored seeds, the `Village` label before it is observed, or any place
   the character has not derived.

The two sentinels (`arrived`, `none`) are always appended by the engine, after the actions.

## 7. The loop

`NavigateAction : AgentAction`, with a small explicit state machine:

```
Idle -> Asking -> (Moving -> Asking) | Surveying -> Asking -> ... -> Done | Interrupted
```

Start:

- Build the action list.
- If System 1 is unavailable (or `system1.useMainModel`): `Done` with the action list and the
  reason, so the main model can choose with `[MOVE_TO]`/`[PLACES]`. No survey — there is nobody to
  ask afterwards.
- An empty action list is an **unsure** condition, not an immediate stop: the one-survey
  recovery below runs, because a character in a place it has not named may still see one by turning.
  If the survey produces nothing, `Done`.
- Otherwise ask.

Ask:

- Build the state and the `Choice` question. Capture the classifier `shared_ptr`; the recorder is
  attached. Send `Ask` with `Attribution{"navigation", "<agent>, step N toward: <goal>"}`.
- Return `Doing` while the answer is in flight.

On answer:

- `arrived`: `Done` with the success summary. The engine reports the claim; it does not verify it.
- `none`, an unknown key, or a move chosen below the probability/margin floor: **unsure**.
- A valid move above the floor: use the move's fixed target cell and start a move to it.

Unsure:

- If a survey is still available for this decision and the character has a `Vision`: request the
  one-shot full-circle sweep (`Vision::fullCircleRequested`, the same mechanism as `[LOOK around]`),
  wait for `lastFullCircleTick` to change, then rebuild the actions and ask once more. The sweep
  goes through the normal writer and deriver, so any newly seen places are in the list.
- One survey per decision, and a per-episode `maxSurveys` (default 2). If the re-ask is still
  unsure, or no survey is left, `Done` with the unsure summary — control returns to the main model.
- If the survey itself times out (no `Vision`, no sampler), skip the re-ask and stop.

Moving:

- Use a cell-target move (`MoveToCellAction`); the target was fixed when the choice was made, so a
  place that grows or a road's far end flipping cannot turn the character around.
- **Arrival is standing on the target cell, not being next to it.** The direct `[MOVE_TO]` object
  path stops when adjacent because that is enough to interact with a crate; a navigation leg that
  stopped one tile short of a road left the character in the orchard beside it, and the compass kept
  offering the same 1-tile step. The first real run hit this: the leg reported "an unnamed road" while
  the character was still in the orchard.
- On arrival: mark the move's place visited, increment steps, re-ask. A local `here:<direction>` move
  does not add a place (the current place is already known); a distant or adjacent move does.
- On failure (A* cannot reach, per-leg timeout, target vanished): mark the **target cell** unreachable
  for this episode, re-ask once without it. A second failure of the same target stops the episode. A
  different direction with the same name is a different target and is not excluded.

Stop conditions, in order:

| condition | result |
|---|---|
| `arrived` chosen | Done: "you judged that you had arrived" + nearby places |
| `none` chosen, or still unsure after the survey/re-ask | Done: "you could not tell which way looked better" + nearby places |
| choice probability/margin below the floor and no survey left | Done: unsure + nearby places |
| no actions (after any survey) | Done: "you do not know anywhere that could lead there" |
| System 1 unavailable | Done: action list returned to the main model |
| `maxSeconds` reached | Done with a timeout reason + nearby places (resource guard) |
| repeated target (see below) | Not a stop: a `REPETITION` line is added to the next S1/strategist state |
| interrupted by a wake / new action | `Interrupted`; see §9.1. The in-flight ask is cancelled and the episode stays on the stack |

Repetition signal:

- The same **target cell** chosen twice in a row while the character has not moved more than one tile
  is reported, not stopped: the next S1 state gets a `REPETITION:` line and the strategist's
  observation gets a `repetition` field. The model decides whether to change course or repeat.
- The target, not the direction name, is the unit: `here:east` from the orchard edge steps onto the
  road (1 tile), while `here:east` from the road follows it (6 tiles). Repeating a direction while
  the character actually advances is following, not a loop.
- A survey does not reset the signal: turning in place is not travelling.

The summary is described in §8.

## 8. What the main model gets back

Exactly one message, factual, capped. It must leave the agent oriented: the reason the episode
ended, the route actually walked, and the nearby places in the same form `[PLACES]` prints. A main
model waking after a five-leg walk with no idea where it is would waste the whole saving in its first
turn.

Arrived:

```
System: You stopped after 3 legs: you judged that you had arrived.
Route: an unnamed orchard -> an unnamed road (east) -> an unnamed road (east) -> Village.
You are in Village (market 100%, workshop 100%).
Nearby places you remember:
  #12 Village -- storage 100%, workshop 100% -- you are here
  #6  an unnamed road -- road 100% -- 4 tiles south-west
  #9  an unnamed road -- road 100% -- 12 tiles north-east
You can [MOVE_TO] any of these, or use [PLACES] for the full list.
```

Unsure, after the survey and the re-ask:

```
System: You stopped after 2 legs: you could not tell which way looked more likely.
You turned a full circle to look for a better lead, and still could not choose.
Route: an unnamed orchard -> an unnamed road (east).
You are at an unnamed road (road 100%).
Nearby places you remember:
  #6  an unnamed road -- road 100% -- you are here
  #8  an unnamed road -- road 100% -- 14 tiles south
  #1  an unnamed orchard -- orchard 100% -- 2 tiles north-west
You can [MOVE_TO] any of these, or use [PLACES] for the full list.
```

Other exits use the same shape with a different reason: `none chosen after N legs`, `the decision
resource guard was reached`, `System 1 is unavailable; here is
what you know`.

- The nearby list is capped (default 5) and built by the **same renderer `[PLACES]` uses**, so the
  two can never disagree about a name, an aspect score or a distance. Factor that renderer out of
  `PlacesAction` rather than writing a second one.
- The route is the ordered list of places actually reached, not the ones considered.
- No System 1 reasoning (there is none to report), no probability table, no hidden state.
- One message, not one per leg. The transcript must not grow during the loop. The inner move
  action's own success/failure text is swallowed by `NavigateAction`; only the summary reaches the
  agent.

## 9. Threading, lifetime, cancellation

System 1 is a network call and its callback may run on another thread (`SystemOneClassifier::Ask`).
The action runs on the main thread in `AgentAction::update`.

- The callback must **not** capture `this`. It captures a `std::shared_ptr<NavigationShared>`
  containing the mutex, the pending answer, the `std::stop_source` and an `alive` flag. The action
  sets `alive = false` in its destructor. This is the same pattern `AgentBrain::ProbeMemorySalience`
  uses for its classifier callback.
- `update` polls the shared slot; it never blocks and never joins a thread.
- The classifier is held as a `std::shared_ptr` **for the duration of the episode**. A runtime
  profile swap drops the game's reference; the action's copy keeps the provider alive until the
  request finishes. A raw pointer (which the salience path stores today) would dangle once the
  `shared_ptr` is swapped, so that pattern is not copied here.
- `update` receives `deltaTime` in **milliseconds** (`AgentBrain::update` multiplies by 1000; see
  known-issue #3). The wall-clock budget accumulates that value rather than reinterpreting it.
- The provider's own `Stop`/in-flight guard handles the worker thread; the action owns no thread.

The classifier is reached through a small world resource, set at startup and updated by the profile
swap, so the command factory does not capture `Game`. The recorder is attached to the provider and
records automatically.

### 9.1 Waking during a navigation

The attention gate (`docs/agent-attention-design.md`) may decide that something the character just
saw is worth waking the agent for — a signpost, a person, a place it did not expect. That wake must
be able to interrupt a navigation; otherwise a long walk is a period in which the agent cannot read
the very thing that would tell it where to go. The gate itself is not implemented yet; this section
fixes navigation's side of the contract.

**Yielding.** `agent-attention-design.md` §5 is explicit: waking without yielding freezes the agent,
so the wake path and the yield path land together. `NavigateAction` is that yield point for this
verb. It is an ordinary action on the existing interrupt stack:

1. The wake path sets `WakeReason` (the gate does not touch the action stack itself).
2. At the brain's decision point, `AgentBrain::interruptCurrentAction` calls
   `NavigateAction::handleInterruption` and pushes the episode onto `actionStack`.
3. The main model gets the wake prompt and acts — typically `[MOVE_TO Signpost]` then `[EXAMINE]`,
   which must run as the current action while navigation waits on the stack. How the wake path
   sequences that against `executeActionQueue`'s stack-first resume is the attention feature's
   problem; see §17.10.
4. When that action finishes, `executeActionQueue` resumes the stack: `NavigateAction::resume`
   continues the episode.

**What interruption must preserve.** The goal text, the visited set, the step and survey counters,
the elapsed budget, the current leg target, and whether the episode was asking, surveying or moving.
Interruption is not an exit; only the stop conditions in §7 are.

**What interruption must cancel.** The in-flight System 1 request (`stop_source`) and the inner
movement leg. The callback already in flight reports a cancellation error and is ignored. On resume:

- was asking → re-ask (the cancelled answer is not reused);
- was surveying → if `lastFullCircleTick` advanced, rebuild and re-ask; otherwise request the survey
  again;
- was moving → recreate the leg from the remembered place id (the path is recomputed; the character
  may have been moved by the wake action).

**What the wake prompt sees.** `NavigateAction::getActionName()` returns `NAVIGATE` and
`getActionContext()` returns the goal and step (`find a village with a market (leg 2)`), so the
attention state's "current activity" line is useful without navigation exposing its internals.

**Abandoning instead of resuming.** The stack auto-resumes an interrupted action before a queued
command. That is exactly right for "read the sign, then carry on", which is the case this feature
needs. It is wrong for "stop navigating and do something else": there is currently no verb that
clears the stack, so a main model that wants to abandon an episode cannot. This is a gap in the
attention/interrupt plumbing, not something navigation can solve alone; a small `[CANCEL]` (clear the
stack) or an explicit "replace the interrupted action" option belongs to that feature. Until it
exists, naming a place or finishing the detour is how the main model influences a resumed episode.

**Testing without the gate.** The unit harness drives the action through a scripted interruption:
start a navigation, call `handleInterruption` mid-ask and mid-move, then `resume`, and check the
episode continues, the cancelled answer is ignored, and the budget is not reset. The live gate test
comes with the attention feature.

**Concurrency.** Navigation calls are System 1 calls like any other and must share the attention
feature's global concurrency cap and rate limit (`agent-attention-design.md` §6). The action must
degrade if the cap refuses the call, rather than blocking the main thread.

## 10. System 1 policy

- Scenario id: `"navigation"`. The overview window groups by it; the log file is grepped by it.
- `Attribution.advisory = false`: this verdict moves the character, so the window should flag it as
  non-advisory. `Attribution` gains a `bool advisory = true`; the provider copies it into the record.
  (Today `JevClassifier` hardcodes `record.advisory = true`.)
- One request per ask, one `Choice` question. An unsure decision can produce a second ask after the
  survey, so one leg may be two recorded calls. The record's summary says which step and whether the
  call followed a survey, so the cost of uncertainty is visible.
- The `arrived` sentinel is the v1 arrival signal. A separate `Score` question ("is the character
  already where the goal asks them to be?") is not planned unless runs show the sentinel is ignored
  or misused; adding a second question that duplicates an option is a way to get two conflicting
  answers.
- The goal text is short and imperative; the question is fixed. Do not paste the main model's
  narrative into `instructions`.
- If `system1.useMainModel` is true, the "system 1" call is the main model and the economics
  collapse. The action refuses with an explicit message rather than silently spending main-model
  tokens.

## 11. Cost and latency

Measured on this machine (`docs/systemone-local-models.md` §6, §CUDA):

| backend | one decision | note |
|---|---|---|
| hosted Jev | 0.47–1.08 s | ~$0.00002 per call |
| Kev-4B, CPU | 10.5–12.8 s | unusable for a loop |
| Kev-4B, CUDA, warm | ~0.25–0.5 s | the viable local path; first call ~4 s |

A five-leg walk is therefore ~1.5–3 s of decisions on the good path, plus movement time, and one
main-model turn. The same walk without this feature is five main-model turns, each re-sending a
growing context. The saving is the point; the local CPU backend is not a fallback, it is a
demonstration.

The action building and summary cost nothing measurable: at most 24 places, one string each.

## 12. Configuration

Proposed, under `system1` (exact names settle during implementation). Stage 2 lands the block as
`GameSettings::SystemOneNavigationSection`; the field reference is in `SETTINGS.md`:

```json
"system1": {
  "navigation": {
    "enabled": true,
    "maxCandidates": 8,
    "maxDirections": 4,
    "compassRadius": 6,
    "maxSurveys": 4,
    "maxSeconds": 600,
    "legTimeoutSeconds": 60,
    "minProbability": 0.3,
    "minMargin": 0.15
  }
}
```

- `maxCandidates` is the total action lines: local compass plus distant places.
- `maxDirections` is the local compass cap. One slot is reserved for the nearest adjacent place, so
  a wide current place cannot push the way out off the list.
- `compassRadius` is how far a sector looks for the first place. A place farther than this belongs in
  the distant list, where its own distance is shown.
- `enabled` defaults true: the command only runs when the agent asks. A user who does not want
  navigation can disable it and `[NAVIGATE]` reports so.
- `maxSurveys` is a cheap recovery budget across the whole episode. One survey per unsure decision
  is already enforced; a survey that does not settle the choice now hands to the strategist rather
  than being a navigation stop.
- `minProbability`/`minMargin` are per-task calibration and should not reuse the site-router or
  salience thresholds; calibration is not portable between tasks or models (the same reason
  `system1.minConfidence` is per profile). `minMargin` applies to distant-place choices; local
  compass moves use only `minProbability`.
- The per-leg movement timeout is separate from the System 1 timeout: `MOVE_TO` has no timeout today,
  and a navigation leg must not hang the episode.

## 13. Debug surfaces

- **System 1 Overview window** already shows each call: scenario, state, reply, latency, probabilities.
  Filter the scenario to `navigation` and turn on "non-advisory" to see only these calls.
- **`logs/<run-start>/system1/`** holds the exact request and response payloads. This is the replay: to study a
  bad choice, read the record, change the wording/action list, and re-run the game. No separate
  harness is needed.
- **Pause/Resume navigator** in the per-NPC Navigation window interrupts the current action,
  cancels its in-flight S1/strategist request, parks the episode on the brain's action stack and
  resumes it on demand. It is per NPC; other agents are unaffected.
- **`logs/<run-start>/navigator/<Agent>.txt`** holds the full navigator record: strategist prompt,
  raw response (including a partial/cut-off stream), parse error, retries, parsed plan, handoffs,
  S1 state/answer, legs and the final summary. A strategist reply that is cut off is visible here
  before the retry.
- **Debug log** gets one `[navigation]` line per event: asking (with the action count and decision
  number), the chosen key/kind/probability/margin, a survey start/completion, each leg's arrival or
  failure, interruption/resume, and the final reason with the counters. `grep '\[navigation\]'` is
  the run in order; the System 1 record beside it has the full payload.
- **Agent context / transcript** gets only the summary of §8.
- Stage 4 may add a navigation section to the NPC context window (steps, visited places, survey
  count, last state). The System 1 records, the `[navigation]` trace and the summary already cover a
  normal run.

## 14. Failure modes

| symptom | likely cause | response |
|---|---|---|
| picks the cell underfoot | no approach filter | `ApproachCell`/linear rule in §6; the repetition signal |
| ping-pong between two stretches | equal probabilities, no margin | visited marking + repetition signal (the strategist sees the route) |
| picks a place whose name it read in the goal | goal text injected into options | options are engine-built keys only |
| unknown choice key | model error | fall back to top valid probability, else stop as unsure |
| never arrives | A* failure with no timeout | per-leg timeout, mark unreachable, re-ask once |
| survey/ask alternates forever | no survey ceiling | one survey per decision + `maxSurveys` per episode |
| surveys repeatedly and finds nothing new | the character is in a closed space | the re-ask still runs once; a second `none` stops |
| `arrived` is claimed at spawn | model overconfidence | the claim is reported, not verified; main model checks its `reach` task |
| wake during the ask | callback in flight when interrupted | stop token, ignore the cancellation callback, re-ask on resume |
| wake during the move | path in flight when interrupted | remove the path, recreate the leg on resume |
| wake action never returns control | a non-bounded action replaced navigation | the attention feature's bounded-action rule (`agent-attention-design.md` §5) |
| the main model wants to abandon a resumed episode | stack auto-resumes before queued commands | open gap: needs a `[CANCEL]`/stack-clear in the attention plumbing (§9.1) |
| main model wakes at each leg | loop living in the brain instead of the action | loop stays in `update`; one summary |
| transcript grows during the walk | per-leg messages appended | only the final summary is appended |
| provider swapped mid-episode | dangling raw pointer | shared_ptr copy in the action |
| callback after destruction | `this` captured | shared `alive` flag, callback ignores |
| main model fills the goal with a paragraph | no cap | 200-char cap, collapse whitespace |
| silent main-model spend | `useMainModel` true | refuse with an explicit message |
| S1 concurrency cap is full | attention gate and navigation share the backend | degrade with a clear message, never block the frame |
| S1 picks a bad-lead place | the premise, not a bug | a real result; summary explains; tune or revert |

## 15. Worked example: the quest map

At spawn, after `[LOOK around]`, Wren's memory has an orchard and a road. The main model has read the
signpost text (or not) and issues:

```
[NAVIGATE find a village with a market; the signpost said north]
```

The first state:

```
YOU ARE: in an unnamed orchard, facing south.
GOAL: find a village with a market; the signpost said north.
PLACES:
  p2  an unnamed road, 2 tiles south-west, road 100%
  p1  an unnamed orchard, 1 tile north-west, orchard 100%
  arrived  you are already where the goal asks you to be (in an unnamed orchard)
  none     none of these places is likely to help
```

System 1 returns `"choice": "p2"`. The loop walks to the road's far end (the linear-place rule in
`ApproachCell`).

**The sign.** On the way, the perception gate notices the signpost at the fork. It wakes Wren;
`NavigateAction` is pushed onto the stack, the main model reads the sign (`[MOVE_TO Signpost]`,
`[EXAMINE]`) and learns "village — north". It names the north stretch (`[NAME_PLACE #6 the north
road]`) so the label is in the next action list, then the wake action finishes and the navigation
resumes. The episode's goal text is fixed for its lifetime; naming is how the main model injects what
it just learned into the resumed loop.

**The fork.** The re-asked state lists the north and south stretches. If the model picks north, the
loop continues to the village square; if it picks south, the loop walks into the quarry and stops when
the road ends — a real, inspectable failure.

**The survey fallback.** Suppose the fork answer is `none`, or the north/south probabilities are too
close to call. The action requests the one-shot 360-degree survey. The sweep fills memory and may
derive places the cone missed; the action list is rebuilt and the question asked again. If the
second answer is still `none`, the episode stops with the unsure summary and the nearby places. The
main model can then `[LOOK around]` itself, `[PLACES]`, or pick a `[MOVE_TO]` target.

**Arrival.** At the square, the `Village` label attaches from the observed anchor, and System 1
chooses `arrived` (or `Village` and then `arrived` on the next ask). The loop stops with the route and
nearby places.

The state, both choices and the survey flag are in `logs/<run-start>/system1/`; the run's memory is dumpable
from the Vision window.

## 16. Stages and acceptance

| stage | what lands | acceptance |
|---|---|---|
| **1. One decision, one leg** ✅ (2026-10-18) | `[NAVIGATE]`, candidate builder, place-id move target, `arrived`/`none` sentinels, one S1 choice + one move, summary with nearby places | Ask on the quest map; the chosen place is a remembered one; the character moves to it; `arrived` and `none` both produce a summary; the call is in the System 1 window with the exact state; no map/seed access |
| **2. The loop** ✅ (2026-10-18) | re-ask on arrival, survey-on-unsure, budgets/no-progress/leg timeout, visited marking, interruption/resume, one summary | A multi-leg journey returns one message; `none` triggers exactly one survey and one re-ask; a second `none` stops; it cannot exceed the budgets; a scripted interruption mid-ask and mid-move resumes the episode |
| **2.5 Local compass** ✅ (2026-10-18) | directional runs of the current place, adjacent places in the compass, distant places at their nearest edge, cell-target legs, target-based no-progress | On the quest road the model sees east/west road runs and the orchard beside it; a repeated `here:east` with movement follows the road; the same with no movement stops; the adjacent road is still offered when the current place has four runs |
| **3. Tuning** 🟡 surface ready (2026-10-18) | `[navigation]` debug trace, System 1 scenario/non-advisory filter, runbook, action/floor knobs; the real Kev-on-CUDA run is the owner's | The fork choice is correct in at least one real run, or the failure is explained from the record (this is the experiment the feature exists for) |
| **4. Optional** | survey at every waypoint, `reach` task stop, named-place learning, a cancel verb for the resumed episode | each only if the runs show they are needed |

Logic acceptance is a unit harness with a **fake classifier** (no network): scripted `choice`,
`arrived`, `none`, invalid key, unavailable, no actions, survey-then-still-none, budgets,
ping-pong, and interruption (mid-ask and mid-move) followed by resume. The model's actual taste is
judged by playing the quest map, not by a harness.

**Stage 1 actuals.** `include/NavigationPlan.hpp` (candidates, state, question, choice
interpretation, summary), `MoveToPlaceAction` (replaced by `MoveToCellAction` in 2.5) and
`NavigateAction` in `include/AgentActions.h`,
`[NAVIGATE $GOAL]` in `src/GameECS.cpp`, and `SystemOneClassifierResource` in
`include/Components.h`, published by `Game::Init` and updated on a profile swap. `Attribution` now
carries `advisory`, and `JevClassifier` copies it into the record.

One bug found and fixed while building it: the leg must **fix its target cell when it starts**. The
old `MoveToPlaceAction` recomputed `ApproachCell`; for a linear place the far end flips to the other
end at the midpoint, which would turn the character around halfway. `MoveToCellAction` takes the
decision's target as fixed, so one leg always means one destination — the next decision re-aims after
arrival.

**Stage 2 actuals.** `NavigateAction` is now the loop: `Idle → Asking → (Moving | Surveying) → …`,
one summary. Survey-on-unsure requests `Vision::fullCircleRequested` (one per decision, bounded by
`maxSurveys`) and re-asks from the updated memory; resource guards are `maxSeconds` (wall clock) and
`legTimeoutSeconds`; a leg that fails is excluded for the episode and re-asked once; a repeated
target is a soft `REPETITION` signal, not a stop; interruption cancels the ask and the movement but
keeps the goal, route, visited set and counters, and resume recreates the leg or asks again. Settings:
`system1.navigation` (`SETTINGS.md`). The historical stage-2 harness covered the loop, the survey
recovery, the budgets and the no-progress stop; the 2026-10-20 change replaced that stop with the
signal and removed the per-episode ask cap.

**Stage 2.5 actuals.** `NavigationPlan::BuildCandidates` builds one best move per compass sector:
a run of the current place to its last observed cell (min 2 tiles), the nearest cell of an adjacent
place, or an open-ground run over observed walkable cells. A one-cell-thick current place is crossed
by the open run instead of losing the direction to the 2-tile minimum; adjacent sectors no longer
merge, because both directions are valid compass moves. One local slot is reserved for the nearest
adjacent place; the rest are filled cardinal-first, not by length; the current place is excluded from
the distant list. No provisional place is created. Since 2026-10-23 the S1 state/summary say "open
ground" when nothing is underfoot (Stage 1/1.5 of `docs/region-place-design.md`). Since Stage 2 the
current place may be a base region: `PlaceQuery::At` returns a smaller semantic place underfoot,
otherwise the region; `[PLACES]`/`[MOVE_TO]`/`[NAME_PLACE]` use the combined `places` + `regions`
view. Since Stage 3 the region descriptions carry its surface aspects (`sand 82%, grass 10%`) and
its display name is the dominant surface's `regionLabel`; the semantic place underfoot keeps its
forming primary. Since Stage 4 the far/near regions are 8/16/32 parent views: the local compass
still uses the finest `PlaceQuery::At`, while the model-facing selection
(`PlaceQuery::ModelPlaces`) shows the semantic places, the current region and the nearest
`regions.modelNearCount` regions; a coarser parent is hidden while a finer selected child is
contained in it. Since Stage 5 `PlaceQuery::At` prefers a semantic place underfoot even when it is
larger than the region (village/road over the 8×8 child); `[PLACES]` lists semantic places first and
adds `#id` duplicate-name hints; a named parent region passes its label to the child containing its
Examine candidates (`examine:<objectId>`) walk to an observed object's adjacent cell and
run its `Examine` interaction. The single navigator lists every qualifying object inside
`system1.navigation.examineRadius` (default 22, the perception range). The two-context S1
caps them at `maxExamine` (default 2), reserved inside `maxCandidates`, and carries the
result as `recentExamination` in its state and the strategist observation.

Any unusable reply from the single navigator is retried in its own context up to
`navigation.single.maxAttempts` consecutive replies, with a correction naming the exact
error (invalid JSON/type, invalid plan, missing move, or an unavailable move) and the
same token budget. An accepted reply resets the counter; on the cap it falls back to the
two-context navigator when `fallbackToTwo` is on, otherwise it returns to the main LLM.

`navigation.agent: "single"` is the new default single-context navigator: one main-model
context returns both the plan and the next move in one call, using the JSON grid + graph,
the ASCII map and the action/target list. `navigation.agent: "two"` keeps the
strategist + Jev pilot described here and is the rollback/control; it is also the
fallback when the single call fails and `navigation.single.fallbackToTwo` is on.

anchor; the graph labels region nodes `[R8]`/`[R16]`/`[R32]`. The S1 state can also include a
north-up ASCII map (`system1.navigation.mapMode` = `append`/`replace`, `mapRadius` default 6); the
map marks `@`, terrain, objects, semantic-place initials and candidate target digits, with
`MAP TARGETS` as the compact action list. `off` is the unchanged prose baseline. A move is a
`Candidate` with a fixed target cell and a key (`here:north`, `p12`);
`MoveToCellAction` walks it and requires standing on the target cell (adjacency is not arrival).
No-progress is by target cell plus actual movement, so following a road is not ping-pong and the same
direction in a new place is a new move. The compass harness checks the exact-cell arrival, the
road/orchard compass, the adjacent-road reservation, a followed `here:east`, the no-movement stop, a
distant `p2`, survey recovery, the budget, interruption mid-ask/mid-move, open-ground `here:` moves,
and the direction-first case (road underfoot, open north, a place south).

### Stage 3 tuning runbook (local Kev on CUDA)

The agent does not run the model. This is the protocol for the owner's run; the recording is what
makes it a result rather than an anecdote.

**Before the run.** Kev's CUDA tree is the one `scripts/run_decision_server.sh` prefers; start it,
then prove the protocol with `scripts/test_decision_server.sh`, and prove the engine's client with
`./build/AIRogue --systemone-test`. Put the `local_kev` profile in `settings.local.json` (or pick it in
the System 1 window) and make sure it is the active profile before the first call.

**During the run.** Use `maps/Map_Village_Quest.json`, open the System 1 Overview window with the
scenario filter set to `navigation` and "non-advisory" on, and open the Vision Debug window's Memory
tab on Wren. Every navigation event also writes one line to the debug log:

```
[navigation] Wren step 0: asking (6 actions, decision 1, floor 0.30/0.15) for: find a village with a market; ...
[navigation] Wren step 0: choice 'here:east' move p=0.87 margin=0.34
[navigation] Wren leg 1: arrived at an unnamed road (east)
[navigation] Wren step 1: asking (...)
[navigation] Wren step 1: unsure, full-circle survey #1
[navigation] Wren full circle complete; re-asking
[navigation] Wren step 1: choice 'arrived' arrived p=0.91 margin=0.55
[navigation] Wren finished: you judged that you had arrived (legs=1, decisions=2, surveys=1)
```

Ask Wren to `[NAVIGATE find a village with a market; the signpost said north]` (or without the sign
clause, as a second run — that difference is the experiment). Record, per run:

- the spawn action list and the first choice with its probability/margin;
- the fork choice: north stretch, south stretch, `none`, or a survey first;
- the number of decisions/legs/surveys, the latency in the System 1 record, and the final summary;
- whether `arrived` was claimed at a place that is actually the village (the record shows the state).

**What each symptom points at** — change one knob per run:

| symptom | first thing to read | knob / edit |
|---|---|---|
| never chooses the road at spawn | spawn actions: is the road in the list at all? | if missing, it is a memory/vision problem, not navigation; if present but ignored, the goal wording or the action line |
| picks the south quarry at the fork | was the signpost's "north" in the goal text? | goal content (agent knowledge); naming the north stretch with `[NAME_PLACE]`; then action wording |
| always `none` or unsure | top probability and runner-up in the record | `minProbability` / `minMargin` against Kev's actual distribution, not against the run you want to pass |
| stops too early after one leg | the `arrived` choice and the state's `YOU ARE` line | the `arrived` description; the model's claim, not an engine verdict |
| surveys at every uncertainty | survey count vs decisions | `maxSurveys`, `maxCandidates`, `maxDirections`; action wording |
| backtracks at a junction instead of trying a branch | the state's `already walked` markers; is the new branch marked `visited` instead? | the marker is already in place; if the model still ignores it, consider dropping walked local directions when an unwalked branch exists, or give it the signpost direction in the goal |
| ping-pong | the `[navigation]` sequence and visited markers | no hard stop now; the repetition line and the strategist's route show it, so the model can change course |
| slow | `latencyMs` in the System 1 record | action cap, GPU server; CPU Kev is the wrong tool |

**Calibration.** Kev's probabilities are not Jev's; record the top score and the gap for several
decisions before moving `minProbability`/`minMargin`. A threshold chosen to make one run pass is not
calibration. Repeat the same scenario two or three times (the model is sampled; the world seed is
separate).

The first real run (hosted Jev, `17:09`) is the reason the default floor is 0.3 rather than 0.5. The
model's `here:south-east` moved from 0.28 (runner-up `none` 0.19) on the first ask to 0.37
(runner-up 0.13) after the survey. The 0.5 floor rejected both and stopped with "you turned a full
circle" while the character was one tile from the road; 0.3 passes the 0.37 re-ask and the 0.15 margin
still rejects the genuinely split 0.28/0.19. The same run also exposed a separate movement bug: the
leg stopped when it was merely adjacent to the target cell, so the character never stepped onto the
road. That is fixed in `MoveToCellAction` (exact-cell arrival); the floor alone would not have helped.

The next pair (`17:20`/`17:21`) exposed the other gate. On the road the model split west 0.41 / east
0.30 (then 0.43/0.29, 0.39/0.27). The 0.15 runner-up margin rejected each local choice as unsure,
surveyed, and the re-ask flipped east 0.39 / west 0.27; the character stood still and the episode ended
"turned a full circle" again. That is what the local-compass exemption addresses: a six-tile local leg
is reversible, so above the 0.30 floor it acts on the top direction; the margin remains for distant
place choices. The logs also confirm the exact-cell fix: after the first leg the state now says
`YOU ARE: in an unnamed road`, not the orchard beside it.

The `17:32` run is the backtracking case. The route went west to the road's dead end, east along the
whole road to the junction, and then west again for four legs; the twelve-leg budget ended it. At the
junction the state offered `here:west` 0.31, `here:south-east` 0.25 and `p11` 0.18, and nothing in the
west line said the agent had already walked that road. The walked-cell `already walked` marker is the
fix; the run is the reason it exists. If a later run still takes the marked direction, the next knob is
to suppress walked local directions when an unwalked branch is on the list — but that is an engine
preference, so it stays out until the marker has been tried.

**Acceptance.** Stage 3 is done when at least one real run picks the village-bound stretch at the
fork and reaches a place it can claim as the village — or when the run's record explains the failure
in terms of the action state, the goal text, or the model's probabilities. A failed run that is
explained is the deliverable; adding a hint to make it pass is not.

## 17. Open questions

1. **Will a decision model actually follow the road, and take the north branch?** This is the premise.
   The first real run answers it; the record makes the answer inspectable. No offline replay is
   planned — the recorder is the replay.
2. **Action count and wording.** 8? 6? One line each? The 4B input budget and calibration decide;
   sweep in-game. The compass adds a second dimension (how many local directions vs distant places);
   `maxCandidates`, `maxDirections` and `compassRadius` are the knobs.
3. **Should the survey also run at every waypoint, not only on unsure?** The unsure recovery is
   specified; surveying proactively would make the action set fresh but makes navigation stronger
   than manual play. Stage 4.
4. **Should the loop stop when a `reach` task is verified?** The task system already knows how to
   check "you are in the place"; reusing it would let the main model's intention end the loop
   directly. Stage 4; the `arrived` sentinel is the v1 answer.
5. **Should the main model be able to pass a target place name** (`[NAVIGATE Village]`) so the engine
   stops on arrival without asking? That is just `[MOVE_TO]`; keep the verb about uncertainty.
6. **Hosted or local?** Hosted Jev is fast enough and costs almost nothing per call; local Kev on
   CUDA is free and private and also fast. The CPU build is not viable.
7. **What is the probability floor that separates "go" from "ask"?** Calibrate per backend, per task.
8. **Is one survey per unsure decision enough?** The design allows one, with `maxSurveys` per episode.
   If runs show the first survey usually settles it, two is a generous ceiling; if not, the second
   call is wasted and the design should stop after one.
9. **How does the main model abandon a resumed episode?** The stack auto-resumes. A `[CANCEL]` or a
   "replace the interrupted action" option belongs to the attention/interrupt plumbing; until it
   exists, naming a place or finishing the detour is the only lever.
10. **How does the wake path sequence the main model's command against the action stack?** The
    attention feature must define whether the model's response becomes the current action (pushing
    navigation further down) or is queued; the current `executeActionQueue` resumes the stack before
    a queued command, which is right for "read the sign, then carry on" and wrong for "stop".

## 18. Planned work

| file | change |
|---|---|
| `include/NavigationPlan.hpp` (new) | move struct, `BuildCandidates`, state/question builder, summary builder — pure, testable |
| `include/AgentActions.h` | `NavigateAction` (state machine, async handoff, survey recovery, interruption/resume, summary); a place-id move target / `MoveToPlaceAction` |
| `include/PlaceQuery.hpp` | a shared place-list renderer extracted from `PlacesAction`, used by `[PLACES]` and the navigation summary |
| `src/GameECS.cpp` | register `[NAVIGATE $GOAL]` with help; set the System 1 resource |
| `include/Components.h` | `SystemOneResource` (`shared_ptr<SystemOneClassifier>`), if no resource exists |
| `include/Game.h`, `src/Game.cpp` | publish the classifier to the resource at startup and on profile swap |
| `include/SystemOneRecorder.h`, `src/JevClassifier.cpp` | `Attribution::advisory`; provider copies it into the record |
| `include/Settings.h`, `src/Settings.cpp` | `system1.navigation` config block |
| `include/Defaults.h` | caps and thresholds with one home |
| `docs/agent-attention-design.md` | already linked; §5 yielding is the contract §9.1 implements |

## 19. References

- `include/SystemOneRecorder.h` — `OptionType::Choice`, `Option::options` keys, `Answer::choice`,
  `Answer::probabilities`, `Attribution`, `advisory`, the recorder.
- `include/JevClassifier.h` — protocol shape and presets.
- `docs/systemone-local-models.md` — latency numbers and the CUDA path.
- `docs/agent-attention-design.md` — System 1 has no world model; facts are the engine's job; calls
  are recorded and advisory by default; §5 (yielding) is the contract navigation implements.
- `include/SpatialMemory.hpp` — `KnownPlace`, `DerivePlaces`.
- `include/PlaceQuery.hpp` — `DisplayName`, `DistancePhrase`, `AspectsPhrase`, `ApproachCell`,
  `ById`.
- `include/AgentActions.h` — `MoveToEntityAction`, `LookAction`, `PlacesAction`.
- `include/SightReport.hpp` — the observation-side prose; the summary can reuse it.
- `include/Components.h` — `Vision::fullCircleRequested` / `lastFullCircle` (the survey reuse).
- `src/AgentBrain.cpp` — one command per response; the every-other-frame action cadence; the shared
  `alive` flag pattern in `ProbeMemorySalience`; `interruptCurrentAction`/`resumeAction`.
- `docs/agent-exploration-task.md` — the fair test this feature must not break.
