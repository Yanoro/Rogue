# Traversal primitives

**Status:** proposal, 2026-10-30. Written against the run in
`logs/2026-10-30_20-56-42/navigator/Wren.txt` (`Map_Nav_Maze`, goal "find the far
chamber"). Nothing here is implemented; this is the vocabulary and the argument
for it, not the code.

**The short version.** The navigator does not need more one-tile moves. It needs
*commitments*: bounded, interruptible intentions that walk further than one leg
and stop at the next real choice. The current candidate set has exactly two
distance intentions -- a compass run capped at `compassRadius` (6 tiles) and a
remembered place -- so a long road becomes a decision every six tiles. Adding a
`follow`, a `continue`, a `to:`, an `explore` and a `back` gives the model a way
to say "stay on this road to the junction", "keep this heading until the line
changes", "go to that visible gap", "search this area", and "undo that dead end".
None of those are maze tools; the maze is just the scenario that makes the
absence obvious.

**Companions:** `docs/llm-s1-navigation-design.md` (strategist + single-context
navigator), `docs/navigation-subagent-design.md` (compass and leg mechanics),
`docs/agent-exploration-task.md` (the fair-test boundary), `docs/region-place-design.md`
(places, regions and the terrain aspects the primitives should use).

---

## 1. What the run shows

The episode is 23 accepted decisions and 22 legs, 206 seconds of wall clock, and
no arrival. The transcript ends mid-episode at decision 23, at `(21,21)`, with
the goal region at `x 29..34, y 32..36`.

| measurement | value |
|---|---|
| accepted decisions | 23 (plus 2 rejected replies, both retried in the same context) |
| legs | 22 (one accepted `look_around` has no leg) |
| leg displacement, Chebyshev | 79 tiles total; a lower bound on the walked path |
| leg length | median 3.5, mean 3.6, **max 6** |
| legs of exactly 6 tiles | **8 of 22** |
| legs of 1 tile | 5 |
| legs of 2 tiles or fewer | 10 of 22 |
| decisions per 10 tiles | 2.9 |
| wall clock | 206 s; roughly 8.5 s per decision including the leg |
| `here:` / `p<id>` / survey answers | 18 / 4 / 1 |
| invalid moves | 2 (`here:south` at decision 4, `here:north` at decision 20) |
| optimal route | 126 steps (`maps/nav_suite/Maze.test.json`) |

The map's own spec says the quiet part out loud:

> `"maxLegs": 24` ... "Each leg is at most six tiles, so the serpentine is many
> decisions. Watch legTimeoutSeconds and frame time: AStar is quadratic in visited
> nodes." -- `maps/nav_suite/Maze.test.json`

The test was written around the six-tile cap. That cap, not the maze, is what
produced most of the 23 decisions.

### Three excerpts

**The long straight road: six decisions for one road.**

At `(5,12)` the state offers `here:east -- follow an unnamed road east for 6
tiles`. The road actually runs to `x=36`. The model takes `here:east`; the next
state offers `here:east` six tiles again; it takes a `p14` place move on the same
road once, then `here:east` again. Six decisions, 35 tiles, all one road:

```
here:east  follow an unnamed road east for 6 tiles (path 100%, road 100%)
...
here:east  follow an unnamed road east for 6 tiles (path 100%, road 100%)
```

The model is not being indecisive. The vocabulary cannot say "the road, to its
next junction".

**The 1-tile place transition.**

At `(36,20)` the state offers `here:west -- an unnamed road, 1 tile west`, then
`p26 -- an unnamed path, 5 tiles west`. The model picks the 1-tile west; the next
state offers `here:west -- follow an unnamed road west for 6 tiles`, and it walks
six. Nothing changed about the road; the character crossed from the vertical road
place into the horizontal road place. The compass correctly stops at a place
boundary, but the model had already decided to go west, so the boundary cost a
whole decision (`navigator/Wren.txt:1351-1410`).

**The grass room.**

Decisions 17-23 are seven decisions and 11 tiles around a 5x5 grass room off the
long corridor: enter north, north again, survey, misread a wall, retry, east,
south, south, and back into the room. The room has one exit, the way the
character came. `back` would have undone the excursion in one decision; a state
that said "one exit, already walked" might have prevented it. That is 30% of the
episode for zero progress (`navigator/Wren.txt:1848-2536`).

### What the 23 decisions are *not*

They are not 23 real choices. The maze is a serpentine: between the spawn room
and the goal there are about ten corners, one decoy room opening and the goal
chamber. The serpentine has no branch at most corners, so a person walking it
would decide at the room opening and the final chamber, not every six tiles. The
23 decisions are the cost of asking "which way?" at every point where the
engine's leg ended, not at every point where a choice existed.

---

## 2. The diagnosis

The current navigator has a good contract: the model chooses an intention, the
engine resolves it to a target cell, A* walks it, the engine reports the result.
The weakness is the **unit of decision**, not the pathfinder.

1. **The only long intention is "a remembered place".** `p<id>` can be 32 tiles
   away, which is why it exists. But the place underfoot is never a distant
   option -- "the compass is how you act on it" -- and the compass is capped at
   `compassRadius`. So on a long road the only way to continue is repeated
   `here:<dir>` calls. In this run the longest leg is exactly 6, the cap.
2. **The compass stops at place identity, not at what the character can see.**
   A corner where two road places meet, or a place boundary in the middle of a
   continuous surface, is a 1-tile step. That is the right default for a
   *sector* move (the agent may want to turn), but it is the wrong granularity
   when the agent has already committed to a line.
3. **There is no route history primitive.** `walkedCells` marks walked cells and
   the state says "already walked"; there is no "go back to the last junction",
   so leaving a dead end is another sequence of local choices.
4. **There is no unknown-space primitive.** `explore` has no candidate. The
   frontier lives in the strategist's observation graph, not in the single
   navigator's action list (`include/NavigationStrategist.hpp:650`). When no
   remembered place is a good answer, the model still has to pick a sector.
5. **The state is flat.** At a junction the model gets up to twelve lines, one
   per sector/place, and has to infer the topology from a 17x17 JSON grid. The
   invalid `here:north` into a wall and the re-entry into the visited room are
   state-legibility failures, not vocabulary failures. Fixing the vocabulary
   without fixing the state will reduce the number of stops but not the quality
   of the choice at each stop.

The distinction matters for what to build first:

| symptom | cause | fix |
|---|---|---|
| 8 legs of exactly 6 tiles; 6 decisions for one road | no distance commitment beyond `compassRadius` | `follow` / `continue` |
| 1-tile step at a place transition | sector move stops at place identity | `follow` / `continue` across the boundary when the surface is unchanged |
| 7 decisions around a one-exit room | no route history; state does not say "one exit" | `back` + decision-point state |
| invalid `here:north` into a wall | grid legibility | decision-point state + survey diff |
| no progress when no place is plausible | no frontier candidate | `explore` |
| can see a spot but cannot name it | no observed-cell target | `to:<n>` |

---

## 3. The design principle: intention, horizon, stop event

A traversal primitive is a triple:

- **Intention** -- what the character is trying to do in words a villager could
  say: "stay on this road", "keep going this way", "go to that gap", "search this
  room", "go back to the crossroads", "follow that merchant".
- **Horizon** -- how far the engine expands that intention before asking again.
  The horizon is a *cell* computed from the character's own memory, not from the
  map.
- **Stop events** -- the closed list of things that end the expansion early. The
  engine stops at the **next decision point**, not at the next tile and not after
  a fixed number of tiles.

This is the same idea as hierarchical route planning: people decide at junctions,
doors, landmarks and crossings, not every six metres. The engine owns the
expansion; the model owns the choice at the decision point.

### The stop-event vocabulary

A macro that says "walk until something happens" is only useful if "something"
is a closed, testable list. This is the list:

| group | event | meaning |
|---|---|---|
| geometry | `Blocked` | the next step is not walkable, or is occupied |
| geometry | `Bend` | the only continuation changes heading (a straight-line move stops here; a feature move does not) |
| geometry | `Branch` | another walkable way leaves the line (a junction) |
| geometry | `Opening` | a door, gate, room mouth or side area opens off the line |
| geometry | `Surface` | the ground or place category changes (road to grass, indoor to outdoor) |
| geometry | `Lost` | the horizon would be beyond observed ground; stop at the last observed cell |
| perception | `Novelty` | a new object group, character, place or terrain enters observation or memory |
| perception | `Trigger` | a task, monitor, threat or dialogue interrupts |
| safety | `Budget` | the macro's tile/time/leg cap |
| safety | `NoRoute` | A* cannot reach the horizon |
| safety | `Invalid` | the target is no longer observed, walkable or unoccupied |
| completion | `Arrived` | the horizon cell is reached |

Every macro reports the event that ended it, the cell, and a one-line
description. That line becomes `LAST RESULT` in the next state, and a trace line
in the navigation window. This is the difference between a commitment and a
blind run: the model always learns *why* it is standing where it is.

`Novelty` is deliberately defined on the agent's memory, not the map: a new
`KnownPlace` id, a new object alias/content version, a new character, or a new
terrain category. That keeps the fair-test boundary intact.

### Commitment without abdication

Every macro obeys four rules:

1. **Bounded.** A tile cap, a time cap and (for multi-leg) a leg cap. No
   `explore` without a budget.
2. **Interruptible.** `handleInterruption` keeps the episode (goal, route,
   visited set, remaining budget); a wake, a monitor or a threat can take over.
3. **Observable.** The stop event and the stop cell are recorded and reported.
4. **Non-omniscient.** The horizon is computed from `SpatialMemory`; the model
   is never shown a cell it has not observed, and the engine never targets one.

A macro may cross a place boundary. It may not cross into unobserved ground, and
it must stop at a genuine branch/opening/change. "Go around the bend" is not a
loss of control; "go through the next three junctions without telling me" is.

---

## 4. The primitive family

Two tiers. Tier 1 is the distance/history set that the run shows is missing;
Tier 2 is context and information. The keys are the model-facing strings; the
engine can change the labels without changing the intention.

| # | key | intention | horizon | tier |
|---|---|---|---|---|
| 1 | `follow` | stay on the road/river/wall I am on | next junction, crossing, end or change | 1 |
| 2 | `continue` | keep this heading | first bend, branch, opening or change | 1 |
| 3 | `to:<n>` | go to that spot I can see | the chosen observed cell | 1 |
| 4 | `explore` | find something; I do not know where it is | nearest unexplored edge | 1 |
| 5 | `back` | undo that, or return to the last choice | last junction / N legs / start | 1 |
| 6 | `follow:<alias>` | go with that person | their position, until they stop | 2 |
| 7 | `lookout` | I need to see farther | best observed vantage | 2 |
| 8 | `around:<feature>` | get past that | the far side | 2 |

### 4.1 `follow` -- stay on the feature

**Intention.** "This road/river/wall/hall should lead somewhere; keep me on it
until it makes me choose." The feature is the place underfoot when it is linear
(a road, path, river, wall, fence, corridor, hall), or a feature in a sector.

**Horizon.** The next **junction of the feature**, not the next bend: a cell
where another walkable way leaves it, a crossing (bridge, ford, gate, door), the
feature's end, or a surface/category change. On a road split at junctions
(`SplitAtJunctions`) this is the far end of the current stretch. The engine
already has the pieces: `PlaceQuery::IsLinearPlace`, `PlaceQuery::ApproachCell`
(the far end from the character for a linear place) and the junction split.

**Stops on.** `Branch`, `Opening`, `Surface`, `Lost`, `Blocked`, `Novelty`,
`Trigger`, `Budget`, `Arrived`. It does **not** stop on `Bend`: the feature
bends, the character stays on it. It stops **at** the junction (or the last cell
before it if the junction is occupied) so the next state can describe the
branches; it never auto-takes one.

**Pick it when.** "The road/river/wall I am on is the way; I do not want to
confirm every six tiles." In the run this is the missing option on the long
east road: one `follow` from `(5,12)` reaches the junction at `(36,12)` in one
decision, and a second `follow` after the turn reaches `(36,20)`.

**General, not maze-specific.** The high street to the square; the river
downstream to the ford; the wall around to the gate; the hall to the next door;
the forest trail to the clearing; the coast to the harbour; the fence line to
the stile. Any world with a linear feature has this intention.

**Engine hook.** Add one `follow` candidate when the current place is linear:
target = the next junction cell, or the far end if the stretch has no junction;
label = "follow <place> <direction> to the next junction (N tiles)" or "to its
end (N tiles)". If the far end is beyond observed ground, target the last
observed cell and expect `Lost` ("the road continues into unknown ground").

The code already names this distinction: `include/PlaceQuery.hpp:228` calls
`IsLinearPlace` part of "navigation's mode vocabulary (follow vs cross)". The
primitive is the follow half of that vocabulary, not a new idea bolted on.

**Guards.** Max tiles (default ~32). The feature must be observed. Do not cross
into a different surface without stopping. Do not take the junction. Do not
loop on a closed feature; a closed loop (a road loop, a lake shore) needs a
`trace` variant with a "returned to start" stop, which is a later addition, not
a core primitive.

### 4.2 `continue` -- keep this heading

**Intention.** "Keep going the way I was going until the line changes." This is
the user's "walk forward until something happens", made testable.

**Horizon.** The last cell along the last travel heading that is still the same
line: observed, walkable, unoccupied, same surface, no branch/opening, no bend.
In a narrow line (a corridor, road, trail, bridge, tunnel, aisle) the stop is
the first bend, branch, opening or end. In open ground the line is the surface
and the stop is the first surface/feature change (a road, fence, tree line,
water, building edge, place boundary) or the edge of observation.

**Stops on.** `Blocked`, `Bend`, `Branch`, `Opening`, `Surface`, `Lost`,
`Novelty`, `Trigger`, `Budget`, `Arrived`.

**Pick it when.** "The next choice is at the end of this line, and the line is
obvious." It is the cheap version of `follow` when there is no linear place to
follow (a corridor that never derived as a place, a forest trail, a bridge, a
straight field boundary). It is also the option that crosses the 1-tile place
transition: the surface is unchanged, so a change of place id is not a stop.

**General, not maze-specific.** Down the hall; along the alley; over the bridge;
through the market aisle; across the field to the tree line; along the ridge;
through the tunnel. It is the "obvious line" intention, not a corridor rule.

**Engine hook.** The action must remember the **last heading** (the net direction
of the last leg; `Facing` can be stale when the character stopped). Walk while
the cell is observed/walkable/unoccupied and the surface is unchanged. In a
narrow line, stop when the next cell's observed walkable degree (excluding the
cell we came from) is not 1, or when the only continuation is a turn. Bound by
`commitMaxTiles` (default ~24).

**Guards.** Never through unobserved cells (`Lost` at the edge). Stop at the
first surface change; do not walk from day into night because the ground is
still "walkable". In open ground the line is the surface, so `continue` runs to
the first feature/surface change or the memory edge; if the model wants a
shorter, precise commitment there, `to:` is the tool.

**Why not just raise `compassRadius`?** Because a sector run is direction-only.
A 32-tile `here:north` in open ground or across a room would walk past branches,
doors and objects without stopping. `continue`/`follow` are *scoped* expansions:
they know what ends them. The cap is not the bug; the missing stop events are.

### 4.3 `to:<n>` -- go to that spot I can see

**Intention.** "I can see where I need to be, but it is not a place or an object
I can name, and the route is not a straight sector." This is the point-at-it
primitive.

**Horizon.** The chosen observed cell. Two forms:

- **Curated, model-facing (`to:1`, `to:2`, `to:3`).** The engine offers up to
  three *notable* observed cells, each with a description: the gap in the wall
  (3 tiles north-east), the doorway (6 tiles north), the far bank (9 tiles
  east), the open spot beyond the crowd (5 tiles south), the end of the pier
  (7 tiles west). The model picks a number; no coordinate arithmetic, no
  hallucinated target.
- **General, behind it (`to:<x>,<y>`).** Absolute coordinates as shown in the
  observation's `origin` and `rows`. The engine accepts the cell only if it is in
  `SpatialMemory.cells`, walkable, unoccupied and reachable. This is the same
  target with a parser; it can be exposed later for models that handle it.

**Stops on.** `Arrived`, `NoRoute`, `Invalid`, `Novelty`, `Trigger`, `Budget`.

**Pick it when.** "I can see the spot; the way there is around something." The
log's failed `here:north` into a wall is the failure mode `to:` prevents: the
model points at a cell, and the engine either finds a real target or reports
that the cell is not available. It never invents a direction.

**General, not maze-specific.** The gap in the wall; the doorway; the dry patch;
the far bank; the shade under the tree; the spot beside the speaker; the other
side of the square; the end of the queue. It is how a person says "meet me over
there".

**Engine hook.** A `CandidateKind::ObservedCell`. A "notable cell" scanner over
observed cells: narrow openings (a walkable cell with blocked neighbours on two
opposite sides), feature ends, the far side of an obstacle, isolated open spots
inside a cluttered area. Label from the observed local geometry and the objects
around it; cap at `toTargets` (default 3). Validate against memory; A*; if no
route, report `NoRoute` and drop the target for the episode (the existing
`unreachableTargets` mechanism).

**Guards.** Only observed cells; no map peek. No cells occupied by an obstacle
or a character. A distance cap (default ~48) so it cannot be used as a free
cross-map teleport. If the target becomes invalid (an NPC moves onto it, a door
closes), stop with `Invalid`, do not retry the same cell.

### 4.4 `explore` -- find something; I do not know where it is

**Intention.** "The goal is a search, not a destination." This is the unknown-
space primitive the navigator currently lacks.

**Horizon.** The nearest reachable **frontier**: an observed walkable cell
adjacent to an unobserved cell, or an observed opening that has not been
entered. Cluster adjacent frontier cells into one edge and pick the nearest by
A*. `explore:here` restricts the frontier to the connected observed walkable
component around the character -- the room, building, shop or clearing -- so it
searches the current area rather than leaving it.

**Execution.** v1: one frontier per decision, as a normal candidate. v2: a
multi-leg commitment -- walk frontier to frontier, recomputing after each leg,
until a stop event, then report once. The v1 form is cheap and safe; the v2 form
is what actually turns "search the district" into one decision.

**Stops on.** `Novelty` (a new object group, character, place or terrain), `Trigger`,
`Budget` (legs/tiles/time), `Arrived` (the goal), `NoRoute`, or no frontier left
("the area is covered").

**Pick it when.** "No remembered place is a strong candidate, and I need to find
something." In the run it is the right answer once the character was inside the
walled area and the chamber was not visible; the model instead kept picking
sectors.

**General, not maze-specific.** Find the market in a town; find herbs in a
forest; search a house; find the source of a sound; look for a path down a
cliff; search a shop for a specific item. This is the standard answer to unknown
space in every genre.

**Engine hook.** Frontier from `SpatialMemory.cells` (observed walkable with an
unobserved 8-neighbour) and `memory.places` (unentered openings). Exclude cells
already walked and cells whose only unobserved neighbours are blocked (wall
edges). Keep a per-episode explored set so the macro does not oscillate between
two frontiers. Novelty is a memory content-version / new place id / new alias
check, not a map check.

**Guards.** Bounded; interruptible; never uses the map; reports *why* it stopped
("found a new place: an unnamed orchard" / "covered the room; no new exits" /
"budget"). `explore:here` must stop at an unentered exit and report it rather
than leaving the area on its own.

### 4.5 `back` -- undo that, or return to the last choice

**Intention.** "I went into a dead end / I passed a turning / I need to leave
this room / I need to get back to the crossroads." This is the only primitive
that uses **route history** rather than geometry.

**Horizon.** The walked route: the cell path of the last leg (`back`), the last
junction on the route (`back:junction`), N legs (`back:2`), or the episode start
(`back:start`). The last junction is the first cell on the reversed route with a
walkable neighbour that is not on the route and not the cell we came from.

**Stops on.** `Arrived`, `NoRoute`, `Invalid` (the route is blocked now),
`Novelty`, `Trigger`, `Budget`.

**Pick it when.** "I have already been here; the choice I want is behind me."
In the run, `back` after the second north step would have returned to the
corridor in one decision instead of the seven-decision room excursion.

**General, not maze-specific.** Return to the crossroads; leave the shop; back
out of the alley; return to the camp; go back to the conversation; retreat from
the crowd. Every world with a route has this intention.

**Engine hook.** Record the per-leg cell path (`MoveToCellAction::getPath()`
already exposes it) and the route labels. `back` targets the previous leg's
start; `back:junction` walks the route backwards and tests the observed local
degree; `back:start` targets the episode's first cell. A* to the target; if the
route is blocked, stop and report.

**Guards.** The route may be stale or blocked; do not force it. Cap at
`backMaxLegs` (default 4). Do not make `back` an infinite ping-pong: it is a
single commitment, and the repetition line still applies.

### 4.6 `follow:<alias>` -- go with that person

**Intention.** "That person is going somewhere; go with them." A moving
character is a landmark and a route at the same time.

**Horizon.** The character's current cell, re-pathed as it moves. Stop at
interaction range (1-2 tiles), when the character stops (report where), when the
character vanishes or leaves observation (`Lost`), when the path is blocked
(`NoRoute`), or on budget.

**Pick it when.** "I do not know where the market is, but this trader does";
"The guard is going to the gate"; "Follow the guide through the forest."

**General, not maze-specific.** Follow the merchant to the market; the guard to
the gate; the steward to the hall; the guide through the forest; tail a suspect
through the streets; escort someone home. In a village sim this is a core
traversal mode, not a niche.

**Engine hook.** `MoveToEntityAction` already follows a moving entity. Expose
seen characters from `SpatialMemory.objects` (`character == true`) as candidates;
the alias grammar disambiguates `follow:<alias>` (an instance alias such as
`guard-1`) from `follow` (the current feature) and `follow:<dir>` (a feature in
a sector).

**Guards.** Do not follow into an unreachable cell; do not follow forever; stop
at interaction range; if the character stops, hand back with "they stopped at the
market stall" -- that is the payoff, not a failure.

### 4.7 `lookout` -- I need to see farther

**Intention.** "I am in a forest/valley/room and the goal is out there; I need a
wider view." A movement that is an *information* move, not a progress move. The
current `look_around` is the zero-distance version; `lookout` is the moving one.

**Horizon.** The best **observed** vantage cell: high openness (a large observed
walkable area around it), an edge (adjacent to a large unobserved area), an
elevation/aspect if the world has one, or the end of a pier/ridge/balcony. Walk
there, run the full-circle survey, then report a **diff** (new cells, objects,
places, characters) rather than "survey completed".

**Stops on.** `Arrived` + survey, `NoRoute`, `Budget`, `Novelty`/`Trigger` on the
way.

**Pick it when.** "I cannot see enough from here, and moving a little would show
me more." In the maze it is useless (walls block); that is fine -- a general
toolbox does not need every tool to help every map.

**General, not maze-specific.** Climb the hill/ridge/tower; step out of the
forest into the field; go to the end of the pier; stand in the square where the
streets radiate; look over the wall from a cart.

**Fair-test caveat.** Scoring true view gain for an unvisited cell would need the
map and would leak unobserved content. v1 scores only observed geometry
(openness, edge, known height) and reveals the actual view only after moving.
This is honest and still useful; a map-based visibility estimate is an open
question, not a default.

### 4.8 `around:<feature>` -- get past that

**Intention.** "A wall/fence/lake/building/crowd separates me from where I want
to be; get me to the other side."

**Horizon.** The nearest observed reachable cell on the far side of the feature,
chosen by the shorter way around (A*). Stop there and report which side was
taken: "went around the north side; the gate is on the far side."

**Stops on.** `Arrived`, `NoRoute` (sealed), `Budget`, `Novelty`, `Trigger`.

**Pick it when.** "I can see the goal on the other side, but not the way around."
`Lake` and `LakeSealed` are the existing tests.

**General, not maze-specific.** Around the lake; around the market hall; around
the crowd; around the wall to the gate; around the orchard hedge; around a
chasm. The intention is "get past", not "keep my left hand on the wall".

**Engine hook.** Identify the feature (an obstacle group such as `stone_wall`, a
water body, a building, or a place with a boundary shape). Find the far-side
cell by a ray through the feature, or the nearest observed walkable cell on the
opposite side. A*; if no route within the cap, report "no way around; it may be
sealed".

**Guards.** A concrete far-side target; no perimeter loop; if the far side is
not observed, do not guess -- use `explore`/`to:` first.

---

## 5. What not to add

These are the tempting additions that would overfit the maze or break the
fair-test boundary.

- **Wall-following / the left-hand rule.** It is a maze solver, not an
  intention. It ignores the goal, loops around islands, is useless in open
  ground, and silently overrides the model's choice. If the model wants to stay
  on a wall, `follow` the wall feature is the general form; it still stops at
  junctions and lets the model choose.
- **Free-form coordinates with no validation.** A hallucinated cell or a map
  coordinate outside memory is a leak and a teleport. Only observed, walkable,
  unoccupied, reachable cells; reject with `Invalid`.
- **A map-wide `explore` / "reveal the map".** Omniscient; violates the
  observation boundary. Frontiers come from memory.
- **`teleport` / `fast-travel` / `unstick`.** Debug tools, not traversal.
- **Speed modes (`run`, `sprint`).** They reduce time, not decision density; they
  belong to movement stats, not the navigator's vocabulary.
- **New movement mechanics (`climb`, `swim`, `jump`).** Only if the world has
  them; then they are interactions with a terrain/object, not navigator
  primitives. Do not invent mechanics to make a primitive look general.
- **`wander`.** A random walk is `explore` without the information logic. It
  hides the model's intent and cannot be scored.
- **`patrol`.** A scripted loop is a behavior (guard duty), not a general
  traversal intention. Build it from `follow`/`to:`/`explore` if a role needs it.
- **A bigger `compassRadius` as the fix.** It removes the cap but not the
  decision points: a direction-only run walks past doors, branches and objects.
  The missing piece is the stop events, not the number.

---

## 6. Keeping the vocabulary small

The single navigator already spends its 12-candidate budget (8 compass sectors +
4 distant places) and a 500-token reply budget on a 4B local model. The new
primitives must **replace** sector lines, not be added on top. This is a
reallocation, not a menu expansion.

### A proposed offer order

At most ~12 movement candidates:

1. **one forward commitment** -- `follow` if the place underfoot is linear,
   otherwise `continue` if the last leg had a heading, otherwise the best sector;
2. **the nearest adjacent place** -- the side the character might turn into
   (the compass already reserves a slot for this);
3. **one `back`** -- only if the route has a junction behind it;
4. **one or two `to:<n>`** -- only if a notable observed cell exists;
5. **one `explore`** -- only when no strong place/visible target is offered;
6. **one `follow:<alias>`** -- only if a seen character is moving nearby;
7. **two or three distant places** -- nearest first, plus one unvisited;
8. **two object groups** -- the nearest interactable.

The other five compass sectors are not shown. The model can still make a precise
sidestep with the two or three sector lines that survive, and it can point at a
cell with `to:`. Fewer, better options is the right direction for a small model.

### Context-sensitive offering

- `follow` only when the current place is linear (or a feature is in the sector).
- `continue` only when there is a last heading and the line is observed.
- `back` only when the route has a previous leg/junction.
- `explore` only when the frontier is non-empty; rank it after a strong visible
  target, not instead of it.
- `follow:<alias>` only for a moving character in observation.
- `lookout` only when the local observed openness is low and a better vantage is
  observed; never indoors in a dense room unless there is a balcony/stairs.
- `around` only when a feature separates the character from an observed far side.
- `to:` only for cells that are genuinely notable; do not fill the list with
  arbitrary open ground.

### One legend, not per-line prose

The system prompt gets one short block, not a paragraph per candidate:

```
COMMITMENTS (the engine walks further than one leg and stops at the next real choice):
  follow     stay on the road/river/wall you are on, to its next junction or end
  continue   keep your heading until the line bends, branches, opens or ends
  to:1..3    go to a spot you can see (a gap, doorway, far bank, open spot)
  explore    head for the nearest unexplored edge; explore:here searches this area
  back       return to the last junction you walked through
```

The per-candidate line stays short ("follow an unnamed road east to the next
junction (14 tiles)"). The legend explains the verbs; the line explains the
target. That keeps the prompt roughly the same size as today's.

---

## 7. The decision-point state (the companion change)

The primitives reduce the *number* of stops. They do not, by themselves, make
each stop worth making. The run's worst decisions (the invalid `here:north`, the
re-entry into the visited room) are state-legibility failures: the model had a
17x17 grid and a flat list of 12 options, and it misread the topology.

At the end of a commitment, render a **decision point**, not another flat list:

```
YOU ARE: at a junction on an unnamed road, facing east.
DECISION POINT: the road meets a path here.
  forward  the road continues east 18 tiles to a gate (unexplored)
  north    a path, 6 tiles, visited 4 tiles ago
  back     west, 6 tiles to the last junction
  side     a gap in the wall 2 tiles north (to:1)
LAST RESULT: followed the road 12 tiles from the crossroads; stopped at the junction.
```

The engine can compute this from observed cells:

- group the walkable neighbours by direction and place;
- label each with the distance to the next decision point if known;
- mark visited / dead end / unknown;
- for a fully observed bounded component with one exit, say "dead end: one exit
  (the way you came)"; for a partially observed one, say "no other exit
  observed" (never claim a dead end you cannot see);
- compress the route. The current `ROUTE:` line is a chain of 19 labels; it is
  long and does not carry distances or turns. "Came from the west: 6 tiles to
  the last junction; before that, the north corridor" is shorter and more useful.

Two more state fixes that the run asks for:

- **Survey diff.** `look_around` returned "full-circle survey completed; choose
  again" while the observation was already all-round at radius 8 and nothing in
  the grid changed. A survey should say what is new ("12 new cells; a gap 3
  tiles east") or "nothing new; north is blocked by a stone wall". That turns the
  survey into an information move and would have prevented the invented
  `here:north`.
- **Blocked-sector facts.** If a sector is blocked, say so in words. The grid
  shows `B`; the action list does not. "North: blocked by a stone wall" is one
  line and removes a whole class of invalid moves.

These are not new traversal primitives; they are what makes the primitives pay
off. They can land after Tier 1.

---

## 8. Generalization check

The rule for every primitive: **it must be describable in one sentence that makes
sense in a village, a forest, a building and a wilderness. If the sentence needs
"maze", reject it.** Second rule: it must be useful on at least two nav-suite
maps that are not the maze.

| primitive | maze | village streets | open field / forest | lake / river | building interior | social / escort |
|---|---|---|---|---|---|---|
| `follow` | strong (serpentine, next junction) | strong (high street to square) | ok (trail, riverbank) | strong (river/coast) | strong (hall to next door) | ok (follow a route) |
| `continue` | strong (straight runs, place crossings) | strong (alley) | ok (field to tree line) | ok (bridge, causeway) | strong (corridor) | - |
| `to:<n>` | strong (the gap, the doorway) | strong (the stall, the shade) | strong (the ford, the clearing) | strong (the far bank) | strong (the doorway, the stair) | ok (stand beside) |
| `explore` | ok (unknown side passages) | strong (find the market) | strong (find herbs, a path) | ok (find the crossing) | strong (search the house) | ok (find where they went) |
| `back` | strong (leave the dead end) | strong (return to the crossroads) | ok (back to camp) | ok (back to the ford) | strong (leave the room) | ok (back to the conversation) |
| `follow:<alias>` | - | strong (merchant to market) | strong (guide through forest) | ok (ferryman) | ok (steward to the hall) | strong (escort, tail) |
| `lookout` | - (walls) | strong (square, steeple) | strong (hill, ridge) | strong (cliff, pier) | ok (balcony, stairs) | ok (spot who is coming) |
| `around:<feature>` | strong (wall to the gap) | strong (around the hall) | ok (around the copse) | strong (around the lake) | ok (around the crowd) | ok (get past a crowd) |

This is design intent, to be validated by the suite, not measured fact.

### What the nav-suite maps should show

- **Threshold** (open ground): `continue`, `to:` and `explore` must work without
  a place; `follow` is not offered. The old "the compass only exists inside a
  place" problem must not come back.
- **Fork** (long road, T junction, quarry dead end): `follow` reaches the T and
  stops; it does not take the quarry; `back` recovers from the quarry in one
  decision.
- **Hub** (five arms, four slots): context-sensitive offering must not hide the
  goal arm. The forward commitment plus the nearest adjacent place plus one
  `explore` should still include the goal arm; A/B this specifically.
- **Streets** (two-tile stubs): `continue` must not run through a stub junction;
  `follow` must stop at the stub; the existing minimum-run rule still applies.
- **Lake / LakeSealed**: `to:` rejects an unreachable far bank; `around` reports
  "no way around; it may be sealed" instead of looping.
- **BlindStart** (keyhole view): `explore`/`continue` must not invent targets
  beyond observation; they stop with `Lost`.
- **Decoy** (two hamlets): `explore` must not silently pick the decoy; the model
  chooses, and the stop reason says what was found.
- **Maze**: `follow`/`continue` cut the decision count; `back` cuts the room
  excursion; `to:` handles the gap; `explore` is the fallback.
- **Pinch** (A* only): no traversal primitive helps; do not break it. The
  engine's route is the only route, which is a pathfinding/A*-body issue, not a
  vocabulary issue.

The point of this matrix is that the maze is one column. If a primitive wins only
there, it is a maze tool and should be dropped or re-scoped.

---

## 9. Implementation notes

The important economy: **almost every primitive is still one leg.** `follow`,
`continue`, `to:`, `back`, `follow:<alias>`, `lookout` and `around` all resolve
to a single target cell; the existing `MoveToCellAction` and arrival/interrupt
machinery do the work. Only `explore` is a true multi-leg commitment, and its v1
form is one frontier per decision. So this is mostly **target selection**, not a
new execution engine.

### The shared shape

`NavigationPlan::BuildCandidates` (`include/NavigationPlan.hpp`) produces
`Candidate{key, label, description, placeId, target, distance}`. Grow it by a
kind and a cap, not by a new hierarchy:

```cpp
enum class TraversalKind {
  Sector, AdjacentPlace, DistantPlace, Follow, Continue,
  ObservedCell, Explore, Back, Character, Lookout, Around
};

enum class StopEvent {
  Arrived, Blocked, Bend, Branch, Opening, Surface, Lost,
  Novelty, Trigger, Budget, NoRoute, Invalid
};

// Candidate gains:
//   TraversalKind kind;
//   StopEvent expectedStop;  // what the horizon was computed to hit
//   int maxTiles;            // 0 = the kind's default
//   bool multiLeg;           // explore only
```

`MoveToCellAction` gets a `StopEvent` for its own end (arrived / blocked /
no route), and the action folds it into the `LAST RESULT` line. A macro that
expects `Branch` but ends `Blocked` reports the mismatch; that is a tuning
signal.

### Per-primitive hook

- **`follow`**: `PlaceQuery::IsLinearPlace` + the current place's far end
  (`PlaceQuery::ApproachCell` already does the far end for linear places) or the
  next junction from `SplitAtJunctions`. If the far end is beyond memory, target
  the last observed cell and expect `Lost`.
- **`continue`**: store the last heading in the navigation state (net direction
  of the last leg, not `Facing`). Walk while observed/walkable/unoccupied/same
  surface; in a narrow line test the next cell's observed degree; in open ground
  stop at the first surface/feature change or the memory edge.
- **`to:`**: a notable-cell scanner over `SpatialMemory.cells`; labels from the
  observed local geometry and nearby objects; validate with `SpatialMemory` and
  the existing `unreachableTargets` set.
- **`explore`**: frontier scan; cluster; exclude walked cells and wall edges;
  per-episode explored set; v1 one frontier, v2 a bounded loop.
- **`back`**: record the per-leg cell path (`MoveToCellAction::getPath()`); the
  last junction by reverse walk and observed degree; A*.
- **`follow:<alias>`**: seen characters from `SpatialMemory.objects`
  (`character == true`); reuse `MoveToEntityAction`.
- **`lookout`**: observed openness/edge score over `SpatialMemory.cells`; survey
  + diff. Do not score visibility from the map.
- **`around`**: feature detection (obstacle group, water, building, boundary
  place); far-side cell; A*; `NoRoute` is a valid result.

### Settings

Add a `navigation.traversal` block (or fields under `navigation.single`), with
defaults in `include/Defaults.h`, parsing in `src/Settings.cpp`, and a row in
`SETTINGS.md`:

| setting | default | purpose |
|---|---|---|
| `commitMaxTiles` | 24 | cap for `continue` |
| `followMaxTiles` | 32 | cap for `follow` |
| `toTargets` | 3 | notable observed cells offered |
| `toMaxDistance` | 48 | cap for `to:` |
| `exploreMaxLegs` | 6 | v2 `explore` cap |
| `exploreMaxTiles` | 48 | v2 `explore` cap |
| `backMaxLegs` | 4 | cap for `back` |
| `lookoutTargets` | 1 | vantage candidates offered |

Per-primitive booleans for A/B (`offerFollow`, `offerContinue`, `offerTo`,
`offerExplore`, `offerBack`, `offerCharacterFollow`, `offerLookout`,
`offerAround`) let one primitive be tested at a time without a rebuild.

### Logging and the fair test

- Log every macro as `[navigation] key=follow target=(36,12) stop=Branch
  tiles=31 ms=...`; the `LAST RESULT` line is the model-facing version.
- The navigation debug window shows the stop reason and the decision point.
- Targets come from `SpatialMemory` only. A* uses the map for movement, as it
  already does; the target is never unobserved. `to:` labels use observed
  content only. `lookout` does not score unseen visibility.
- Do not add a macro that reads `Map` for target selection. That is the line.

### Build order

1. The shared **decision-point horizon** helper (next branch/bend/opening/end
   along a heading or feature) + `StopEvent` in the log.
2. **`follow`** -- biggest win, reuses `ApproachCell`/`SplitAtJunctions`.
3. **`continue`** -- last heading + topology; the place-boundary fix.
4. **`back`** -- route cell history; the dead-end fix.
5. **`to:<n>`** -- notable-cell scanner; the point-at-it fix.
6. **Decision-point state + survey diff** -- state legibility (can land with 2-5).
7. **`explore`** -- frontier, v1 one leg, v2 bounded loop.
8. Tier 2: **`follow:<alias>`** (reuses `MoveToEntityAction`), then **`lookout`**,
   then **`around`**.

---

## 10. Measuring it

The baseline from this run, and the targets:

| metric | run baseline | target |
|---|---|---|
| decisions | 23 | <= 10 on the maze, with arrival |
| decisions per 10 tiles | 2.9 | <= 1.5 overall; <= 1.0 on a long road/corridor |
| leg length | median 3.5, max 6 | median >= 6 on roads; max >= 20 on a long feature |
| model calls | 25 | <= 10 on the maze |
| wall clock | 206 s | <= 90 s on the maze |
| invalid moves | 2 | 0 |
| dead-end re-entry | 2+ (the grass room) | 0-1 |
| stop reason accuracy | n/a | >= 90% of macro stops on a real decision point |
| arrival | none | arrive; walked/optimal 0.9-1.3 when arriving |

The "stop reason accuracy" audit matters more than the decision count: a macro
that stops in the middle of a uniform corridor is a bug even if it reduces
calls. Sample ten stops per primitive and check that each stop cell has a
branch, opening, end, surface change or target.

### Experiment plan

- **Non-maze first.** Threshold, Fork, Streets, Hub. A primitive that wins only
  on the maze is a maze tool; do not ship it on that evidence.
- **One primitive at a time.** Baseline (current) then `+follow`, `+continue`,
  `+back`, `+to`, `+explore`; do not turn on all five and attribute the win.
- **Three runs per map** (the model is sampled; the world seed is separate).
- **Then the maze.** The maze is the stress test for long corridors and decoy
  rooms, not the design target.
- **Read the stops, not only the arrival.** A run that arrives with two stops in
  the middle of a corridor has the wrong horizon even though the score looks
  good.
- **A/B the offer budget.** With 12 slots, adding five primitives can crowd out
  the goal arm on Hub. Compare the current 8+4 offer against the proposed
  commitment-first offer on Hub explicitly.

### What would falsify the proposal

- `follow` reaches the junction but the model still re-asks every six tiles
  because it does not understand the line: then the legend/wording is the
  problem, not the primitive.
- `continue` crosses a real junction or a room opening without stopping: then
  the horizon is too greedy and the stop events need tightening.
- `explore` wanders to the decoy hamlet: then it is not an information move, it
  is a random walk with better branding; fix the frontier/novelty logic or drop
  it.
- The decision count falls but arrival gets worse: then the primitives are
  hiding choices the model needed, and the horizons are too long.
- Non-maze maps regress while the maze improves: the vocabulary is overfit;
  re-scope.

---

## 11. Open questions

1. **Curated cells vs coordinates.** `to:<n>` avoids coordinate arithmetic for a
   4B model, but the engine's "notable cell" heuristic is the most overfit-prone
   part. Start curated, expose `to:<x>,<y>` only after the curated version is
   measured.
2. **`follow` through bends.** Should `follow` go around a bend (stay on the
   feature) or stop at it (the bend is a decision)? I argue it should go around:
   a bend with no branch is not a choice. But a sharp switchback or a hidden side
   opening is a choice; the `Opening`/`Branch` events handle those.
3. **`continue` in open ground.** What exactly ends it? A road, fence, tree line,
   water, building edge, place boundary, or the memory edge. The list needs to be
   fixed and tested on Threshold; otherwise "continue" is "walk until the budget".
4. **`explore` stop criteria.** What counts as `Novelty`? A new object group, a
   new place, a new character, a new terrain category, or any of them? The
   broader the definition, the more often it stops; the narrower, the more it
   wanders. Start broad (any new group/place/character) and tune.
5. **`lookout` fairness.** An observed-only score is honest but weak; a
   map-based visibility estimate is stronger but leaks. Decide which the
   fair-test brief allows before building it.
6. **`around` feature detection.** What is a feature worth going around? A
   wall group, a water body, a building footprint, a crowd? The engine can
   detect the first three from memory; a crowd is dynamic. Start with the
   static features and the Lake maps.
7. **Macros and monitors.** A strategist monitor can fire mid-macro. Does the
   macro check it after every leg internally, or does it only stop at the
   `Novelty`/`Trigger` event? The existing monitor mechanism is per-decision;
   the macro needs a defined handoff point. This is the main integration
   question with `docs/llm-s1-navigation-design.md`.
8. **Is `continue` a candidate or an automatic extension?** A candidate is
   observable and the model chooses; an automatic extension is cheaper but hides
   the place-boundary choice. I argue candidate, because the log's 1-tile
   transition was a real place change; the model should decide whether to cross.
9. **Data or C++?** The project direction wants named behaviors in a table, not
   hardcoded role logic. The *keys* (`follow`, `continue`, `to`, `explore`,
   `back`) are stable names; the *stop-event set and budgets* are engine data;
   the *labels* are content. Keep the primitive's definition in the table, not
   in a maze-specific C++ branch. This is the genre-swap constraint applied to
   navigation.
10. **Does the model need the grid at all?** At a decision point, a branch list
    with distances may be better than a 17x17 grid for a 4B model. A/B the grid
    against the decision-point prose; keep the grid for the multi-turn spatial
    picture, not for the local choice.

---

## Appendix: the 23 decisions

Times are request times from the transcript; distances are endpoint displacement
(Chebyshev, the game's 8-way step count), a lower bound on the leg's path.

| # | time | reply | leg | note |
|---|---|---|---|---|
| 1 | 20:57:40 | `here:south` | 1 | start `(4,4)`; out of the spawn room |
| 2 | 20:57:50 | `p5` | 3 | |
| 3 | 20:57:59 | `here:south` | 4 | |
| 4 | 20:58:07 | `here:south` rejected, then `p12` | 2 | the state no longer offered south; retry |
| 5 | 20:58:24 | `here:east` | 6 | the long road |
| 6 | 20:58:33 | `p14` | 5 | |
| 7 | 20:58:40 | `here:east` | 6 | |
| 8 | 20:58:48 | `here:east` | 6 | |
| 9 | 20:58:55 | `here:east` | 6 | |
| 10 | 20:59:01 | `here:east` | 6 | 35 tiles east in six decisions |
| 11 | 20:59:08 | `p21` | 2 | |
| 12 | 20:59:15 | `here:south` | 6 | |
| 13 | 20:59:24 | `here:west` | 1 | vertical road to horizontal road; place boundary |
| 14 | 20:59:32 | `here:west` | 6 | |
| 15 | 20:59:40 | `here:west` | 6 | |
| 16 | 20:59:50 | `here:west` | 6 | |
| 17 | 20:59:59 | `here:north` | 1 | into the 5x5 grass room |
| 18 | 21:00:05 | `here:north` | 2 | |
| 19 | 21:00:18 | `look_around` | 0 | no new information in the state |
| 20 | 21:00:27 | `here:north` rejected, then `here:east` | 4 | the model misread the wall |
| 21 | 21:00:43 | `here:south` | 2 | |
| 22 | 21:00:50 | `here:south` | 1 | |
| 23 | 21:01:01 | `here:south` | 1 | back into the room; transcript ends |

The pattern is exact: **8 legs hit the 6-tile cap; 10 legs are 2 tiles or
shorter; no leg exceeds 6.** That is not a model that wants to stop every few
tiles. It is a vocabulary that cannot walk any further.
