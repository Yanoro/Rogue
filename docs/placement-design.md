# Placement and ground awareness

**Status: design note, nothing implemented here.** The AI action layer in `AgentActions.h` and the
command table in `GameECS.cpp` both reference `docs/placement-design.md`; this file is that
document. It analyses why an agent cannot find a tile to place on and lays out the candidate fixes
with their trade-offs. It does not pick one unilaterally — the recommendation at the end is staged,
and the open questions are real.

## Symptom

Pietro the farmer spawns in the middle of his tilled field, holds 12 Wheat Seeds, and cannot plant.
`[SURROUNDINGS]` tells him about the Chest and nothing about the ground he is standing on.
Transcript, `logs/2026-09-25_20-07-23-Pietro.txt`:

```
[SURROUNDINGS]
System: You are at Pietro's Farm (13,9).
Nearby within 10 tiles (offsets from you: +X East, +Y South):
Objects:
- Chest at (1,-5), 5 tiles away
```

The model's own reasoning two turns later:

> "We need to know where tilled soil is. We could just sow near ourselves... The system might not
> list individual soil tiles as objects."

It never resolves the ambiguity, and the session ends in `InvalidAction`. The map says the
ambiguity is trivial to resolve from ground truth: `maps/Map_Farm.json` puts Pietro at `(13,9)` and
**all eight of his neighbours are tilled soil**, with the whole western compound tilled. The agent
is not lacking options, it is lacking *information*.

## Root cause

Four independent gaps, any one of which is enough to block placement.

### 1. `[SURROUNDINGS]` cannot see tiles, by construction

`SurroundingsAction` (`include/AgentActions.h:554`) calls `GetSurroundingsList`
(`include/AgentActions.h:454`). That function's one query is:

```cpp
entity.world().filter<GamePosition, DisplayName>().each(...)   // AgentActions.h:474
```

It enumerates **entities with a `DisplayName`**, classifies them into characters / portable items /
objects, and reports each with a relative offset. Tiles are not entities. A `Tile` is a plain
struct (`include/Components.h:127`) held in `Map::tileMap`, and `Map::LoadMap` creates a separate
`DrawAscii` entity per tile with **no `DisplayName`** (`src/Map.cpp:199-203`) — so terrain is
invisible to the filter twice over. "tilled soil" is a `Tile::name` read from `tileInfo`
(`src/Map.cpp:61`), never a name the list can emit.

**That is a statement about ground, not about structure.** Under the corrected vocabulary
(`tile-and-object-vocabulary.md`), a tile is ground only; a wall or door is an object. Once a wall
or door is an entity with a `DisplayName`, this list sees it. "Terrain is invisible" must not be
read as "walls are invisible".

The function's own doc comment claims it reports what is "close enough to walk to and interact
with". That is exactly the problem: **placement targets ground, not a thing you walk to.**

### 2. The tile radar exists but is wired to an undocumented path

`GetSurroundingsRadar` (`include/AgentActions.h:397`) renders a real glyph grid with a
glyph→tile-name legend. Its only caller is the hidden `Placeable` item-interaction hint
(`src/GameECS.cpp:2140-2165`), which is reachable only like this:

1. the model must run `[EXAMINE_ITEM Wheat Seeds]` to see `- [PLACE]: ...` (the interaction name is
   uppercased by `FormatInteractionForAI`, `include/InteractionRegistry.h:25`);
2. then issue `[PLACE Wheat Seeds]` **with no coordinates**, so it falls past the `[PLACE]` regex
   and into the `[GENERIC_INTERACT]` catch-all (`src/GameECS.cpp:2077`);
3. `GenericInteractAction` matches the held item name and the `Place` handler, and only then
   returns the radar (`include/AgentActions.h:822-829`).

Nothing in the prompt teaches steps 1–3.

### 3. `[PLACE]` is hidden, so the model cannot discover it

`[PLACE $ITEM AT X,Y ...]` is registered with `hidden = true` (`src/GameECS.cpp:2028`). The
`hidden` flag is honoured in both places a model learns verbs — the spawn prompt's `%COMMANDS_LIST%`
(`src/GameECS.cpp:275`) and `InvalidAction`'s recovery list (`include/AgentActions.h:67`). The
recovery list Pietro actually received names ten verbs and not `[PLACE]`. The farmer is saved only
because his authored background spells `[PLACE Wheat Seeds AT X,Y]` out by hand
(`maps/Map_Farm.json`); a generic NPC gets no such help.

### 4. Even the radar is not addressable

`GetSurroundingsRadar` prints a bare glyph rectangle with no coordinate labels, so the model has to
count columns and rows to turn a glyph into an offset. Its legend is `std::map<char, std::string>`
keyed by glyph (`include/AgentActions.h:402`), so two tiles sharing a `character` in `tileInfo`
silently overwrite each other's name. On `Map_Farm` the five tile types happen to use five distinct
glyphs (`,`, `#`, `'`, `.`, `~`) — but `'` and `,` are a one-pixel read apart, and a single JSON
edit collapses the legend. A model that mis-counts by one column plants into the path.

## What any fix must respect

Derived from `.agents/design/two-tier-agents.md` and the existing components, not invented here:

- **One validator, two callers.** Placement legality already lives in `PlaceAtAction::Place`
  (`include/AgentActions.h:1043-1101`): duplicate, self-tile, `maxDistance`, bounds, `blocksTile`,
  `policy.surface` match, occupancy. Anything that *advertises* a coordinate must run that same code,
  or the advertisement and the execution drift. This is the design note's "the gate and the
  predicate are one component viewed from two ends" (§8).
- **Radius must be small for a tile view.** `DEFAULT_SURROUNDINGS_RADIUS = 10`
  (`include/Defaults.h:41`) is fine for an entity list and ruinous for a grid: radius 10 is 441
  cells. A tile view wants the item's `Placeable::maxDistance` (default 1, `include/Defaults.h:45`),
  which is why `GetSurroundingsRadar` takes its own radius.
- **The engine owns facts; the agent owns intentions.** Inventories, terrain and legality are engine
  state. The direction doc's constraint 1 and §6 of the two-tier note both push coordinates out of
  model output and into engine-authored options.
- **Terrain stays data.** Surfaces are named by `Tile::name` from map JSON and matched by
  `Placeable::surface` (`data/objects/wheat_seed.json` → `["tilled soil"]`). No fix should hardcode
  "the farm".

---

## Options

### S1 — Make `[SURROUNDINGS]` return the ground as well

Extend `GetSurroundingsList` (or `SurroundingsAction`) with a terrain section covering a small
radius around the actor, using `Placeable`-style radius semantics rather than the entity radius.

Two renderings, and the choice matters:

- **S1a, an addressable grid** — reuse `GetSurroundingsRadar` but label the axes with the offsets
  they represent, e.g.

  ```
  Ground (rows are dy, columns are dx; you are @):
        dx:-1  0  +1
  dy:-1  '    '   '
  dy: 0  '    @   '
  dy:+1  '    '   '
  Legend: ' = tilled soil
  ```

- **S1b, a flat surface list** — "Tilled soil at (-1,-1), (-1,0), (-1,1), (0,-1), (0,1), (1,-1),
  (1,0), (1,1)." No counting, no legend, trivially parseable.

**Pros**

- Matches the model's expectation and the literal complaint; no new vocabulary, no new registry
  entry, no prompt change.
- Gives the model offsets directly usable in `[PLACE ... AT X,Y]`, so it composes with the existing
  DSL instead of replacing it.
- S1b in particular is nearly impossible to misread, and its size is bounded by the placement radius
  (8 entries at `maxDistance = 1`), so token cost is negligible.
- Fixes the information gap for *any* ground-dependent verb, not only placement.

**Cons**

- One probe now serves two consumers with different wants: the entity list wants radius 10, the
  ground wants radius 1. Merging them into one reply needs a stated split or the reply bloats
  (441 cells if the tile view inherits the entity radius).
- S1a keeps the model in the counting business and keeps the glyph-legend collision bug; it is the
  more "picturesque" option and the less reliable one.
- Reporting *all* ground tells the model where the field is but not what is legal for the item it
  holds — it still has to intersect the list with the `surface` rule it may not have read.
- Only useful within the placement radius; "which way does the field extend" remains unknown, so
  multi-turn field expansion still relies on trial and error.

### S2 — A placement probe that answers with executable coordinates

Add a probe, e.g. `[CAN_PLACE $ITEM]` (or reuse the existing "Place" hint and make it tell the
truth), that runs the *actual* validation for the actor's `Placeable` policy and replies with the
coordinates that would succeed:

```
System: Wheat Seeds can go on tilled soil within 1 tile.
System: Legal now: (-1,-1) tilled soil, (-1,0) tilled soil, (0,-1) tilled soil, ...
System: Not legal: (0,0) you are standing there, (1,1) 2 tiles away (max 1)
```

Implementation: extract the loop at `include/AgentActions.h:1043-1101` into a free function
(`EvaluatePlacement(actor, policy, requested) -> {ready, rejected}`) and have both `PlaceAtAction`
and the probe call it.

**Pros**

- **Zero hallucination by construction**: the coordinates offered are the ones the executor will
  accept, because it is the same function. This is the strongest correctness property available
  short of S3.
- Tiny output — at `maxDistance = 1` there are at most 8 candidates.
- Generalises across placeables for free, since it reads `Placeable` rather than naming items.
- Surfaces the policy (`surface`, `maxDistance`) in the same reply, so the model learns *why*.
- Small, additive change to the live tree; no dependency on the uncompiled task layer.

**Cons**

- Adds a round trip and a turn before every first placement (latency and tokens), unless made
  idempotent/cheap enough to call speculatively.
- Answers for one item at a time; a model holding two placeables probes twice.
- Still keeps the model typing coordinates, so a probe followed by an action can go stale if another
  agent takes the tile in between (mitigated by the executor's rejection reasons, but the failure
  mode survives).
- Making the *existing* hint honest means deciding what `[PLACE $ITEM]` with no coordinates should
  do — today it only describes. Every example in the hint currently shows coordinates
  (`src/GameECS.cpp:2142`), so a model copying the example performs a blind placement instead of
  requesting the radar. That inconsistency has to be resolved either way.

### S3 — Take coordinates out of the model's hands: `[PLACE $ITEM]` picks the tile

The engine chooses the tile(s) using the same validator plus a placement policy, and the model only
names the item and optionally how many: `[PLACE Wheat Seeds]`, `[PLACE Wheat Seeds 3]`.

**Pros**

- The failure class disappears structurally: a named-but-illegal coordinate becomes unexpressible.
- No radar, no probe, no legend, no counting — the whole problem is deleted rather than answered.
- Shortest possible prompt and reply; the model's spatial reasoning stops mattering.
- Directly aligned with the two-tier direction, where the engine "supplies the task, the legality,
  the coordinates" (§7, `.agents/design/two-tier-agents.md:75-77`).
- Already prototyped: `FreeNeighbourOffset` (`src/TaskOptions.cpp:49`) picks a free in-bounds
  non-blocking orthogonal neighbour and returns it as the `"dx,dy"` string `[PLANT_AT]` wants.

**Cons**

- The engine must own a *placement policy* — nearest-first, sweep order, field-frontier growth,
  random tie-break? — and that is game design smuggled into a mechanic. "Which tilled tile" is
  genuinely meaningful to a farmer expanding a field.
- Loses expressive multi-tile placement in one command unless the engine also decides batch layout,
  which compounds the policy question.
- No feedback when it does nothing: if all candidates are illegal, the reply has to say why, which
  means the rejection machinery from S2 is still needed.
- Requires deciding what "3 Wheat Seeds" means when only 2 tiles are free (partial success? refuse?),
  and that contract has to hold for every placeable.

### S4 — Directional or landmark targeting

Accept a direction word or an area name instead of coordinates: `[PLACE Wheat Seeds EAST]`,
`[PLACE Wheat Seeds NEAR Chest]`, or resolve against a `Location` ("Pietro's Farm" is a 13×13
`Location` in `maps/Map_Farm.json`).

**Pros**

- Natural to read and cheap in tokens; no counting anywhere.
- Reuses the existing `Location` concept, so area-targeting generalises beyond the farm.
- Handles the overwhelmingly common case ("sow next to me") in one short verb.

**Cons**

- Reintroduces a convention the DSL deliberately rejected: `GetSurroundingsList` ends with
  "Directions such as 'east' and bare coordinates are not valid targets"
  (`include/AgentActions.h:550`). Supporting directions for `[PLACE]` but not `[MOVE_TO]` is an
  inconsistency the model will probe.
- One tile per command unless the verb takes a direction list, and "EAST" is ambiguous once the
  agent needs the *second* tile east.
- Landmark targeting hides a search: the engine still has to pick which tile inside the location,
  which is S3's policy question with extra syntax.
- Needs parser work and a localisation story for a genre-swappable engine.

### S5 — Author the ground into data: location descriptions and backgrounds

Tell the agent where the soil is statically — extend `locations[].description` or the NPC
`background` with the field's extent, e.g. "your tilled field covers the fenced compound from x=8
to x=15; you stand in its centre".

**Pros**

- Zero runtime cost, zero new verbs, no extra tokens over the life of the agent.
- Pure data, which is what `airogue-direction` constraint 1 asks for, and authored text is often the
  clearest description of a place.
- Immediately fixes Pietro and any other hand-authored role without touching the AI path.

**Cons**

- Static and therefore wrong the moment the world changes — which is the entire premise of the sim.
  A second farmer tilling new ground, or the map editor repainting a plot, silently invalidates it.
- Puts raw coordinates in the prompt, exactly the brittle content the direction wants removed, and
  long coordinate prose competes for the model's attention with the actual goal.
- Costs authoring effort per map and does not generalise to a procedurally laid-out plot.
- Fails entirely for the general case: an agent that wants to set an anvil down anywhere.

### S6 — Make failure self-teaching, and teach a probing protocol

Keep the current flow, but (a) append a ground report to every failed or partial `[PLACE]` reply,
reusing S1/S2's renderer, and (b) amend the authored background to instruct a single full-ring probe.

**Pros**

- Cheapest possible change; no new verb, no registry churn, and the rejection messages already name
  ground and reason (`include/AgentActions.h:1074-1097`).
- Also documents the surface rule at exactly the moment the model needs it.
- Complements every other option instead of competing with it — it is the fallback when a probe goes
  stale.

**Cons**

- Reactive: it costs a turn and a failed attempt before the agent learns anything, and a *partial*
  success consumes real items on the coordinates that happened to be legal while the rest fail.
- Probe-by-spray only works because `maxDistance = 1` (8 candidates). At radius 2 it is 24, at 3 it
  is 48 — the technique does not scale.
- Depends on prompt compliance; the evidence in `logs/` shows the model reasoning about the gap and
  still not adopting a workaround.
- Leaves the discoverability bug (root cause 3) entirely unfixed.

### S7 — Finish the option-enumerator path and make placement engine-authored

The strategic fix already sketched in-tree: compile the task layer and let
`TaskOptions::Enumerate` offer `PLANT_AT`/`PLACE` as a first-class executable option, with
`[PLANT_AT]` staying hidden and the model only ever choosing "Plant Wheat Seeds next to you".

Status check: `TaskOptions.cpp`, `TaskDriver.cpp`, `TaskExecution.cpp`, `TaskResolver.cpp`,
`DecisionClient.cpp`, `VerbRegistry.cpp` and `GoalPlanner.cpp` are **not in `SOURCES`**
(`CMakeLists.txt:119-154`), so none of this is live. `PlantAtAction`, which
`TaskOptions::BuildAction` constructs (`src/TaskOptions.cpp:440`), does not exist in
`AgentActions.h`. The prototype is real but uncompiled.

**Pros**

- The direction's stated end state, and the only option that removes the failure class rather than
  mitigating it: the model's output can no longer contain a coordinate, so it cannot contain a wrong
  one.
- Composes with gates, outcome predicates, retry bounds and decision weighting that are already
  written and reasoned about in the two-tier note.
- The option label can carry the spatial fact — "Plant Wheat Seeds on the tilled soil to your west"
  — which is S1's information delivered without a grid.
- Scales to many targets: N adjacent legal tiles become N options, ranked, with the engine changing
  nothing else.

**Cons**

- By far the largest change: the task layer is uncompiled, so this is a project, not a fix, and it
  is coupled to the phase-5/6 roadmap (hardcoded ladder → model-authored goals).
- Does not by itself answer S3's policy question (which legal tile becomes *the* option, and how
  many) — the enumerator inherits it.
- Removes the model's ability to make expressive spatial choices, unless direction or area variants
  are added back as option flavours.
- Does nothing for the informational half of the problem: an agent still cannot *ask* what the ground
  looks like, which matters for planning a field rather than executing one step.

---

## Comparison

| option | fixes information gap | eliminates wrong coordinates | change size | works for non-farm placeables | matches two-tier direction |
|---|---|---|---|---|---|
| S1 ground in `[SURROUNDINGS]` | yes | no | small | yes | partly |
| S2 placement probe | yes | **yes** (same validator) | small | yes | partly |
| S3 engine picks tile | n/a (not needed) | **yes** | medium | yes | **yes** |
| S4 direction / landmark | partly | partly | medium | yes | no |
| S5 authored ground | yes, statically | no | tiny (data) | no | no |
| S6 self-teaching failure | after a mistake | no | tiny | yes | no |
| S7 option enumerator | via option label | **yes** | large | yes | **yes** |

## Recommendation

Staged, because the cheap fixes are not wasted work under the strategic one:

1. **Now — S2, sharing the validator.** Factor `PlaceAtAction`'s validation into one function and add
   the probe. This is a small, additive change to the live tree, it is the only small option that
   makes wrong coordinates structurally impossible, and the refactor is a prerequisite for S7
   anyway (the gate and the executor must be one code path).
2. **Now, alongside — un-hide the placement path.** Either add `[PLACE]` to the visible command list
   or make the "Place" interaction self-describing enough that `[EXAMINE_ITEM]` → `[PLACE $ITEM]`
   is discoverable. Make the hint's own example match the no-coordinate form that actually returns
   the radar, because today the example performs a blind placement.
3. **Soon — S1b, a flat surface list on `[SURROUNDINGS]`.** Small radius, name-per-entry, no glyph
   grid and no counting. This answers "what is the ground" generally, which the probe answers only
   for the held item. Skip S1a; the grid is the least reliable rendering of the three.
4. **When the task layer lands — S7, with S3 as its placement primitive.** Coordinate typing by the
   model should end when the enumerator can offer a legal tile directly.

Two things *not* recommended as the primary fix: S5 alone (static knowledge in a changing world) and
S6 alone (reactive, and it does not fix discoverability). S4 is worth revisiting only if direction
targeting is also added to `[MOVE_TO]`, so the DSL has one convention.

## Open questions

- **Which tile should the engine prefer when several are legal?** Nearest to the actor, nearest to
  the existing field frontier, or a stable sweep? This is the one piece of real design in S3/S7 and
  it is not derivable from the code.
- **Should the placement hint and the surface probe be one command?** `[PLACE $ITEM]` with no
  coordinates already means "tell me how", but its example teaches the opposite. Picking one
  behaviour changes the meaning of a command already in the wild.
- **Is a tile grid wanted anywhere?** If nothing should ever render one, `GetSurroundingsRadar`
  can be replaced by S1b and the glyph-legend collision disappears with it.
- **Should `Placeable` gain a display hint** (e.g. "tilled soil" → "soil"), so the probe's reply is
  phrased for a model rather than echoing raw tile names?

## References

- `include/AgentActions.h` — `GetSurroundingsRadar:397`, `GetSurroundingsList:454`,
  `SurroundingsAction:554`, `PlaceAtAction:900`, validation `:1043-1101`, rejection text `:1074-1097`
- `src/GameECS.cpp` — command registration `:1953-2087`, `[PLACE]` hidden `:2019-2029`,
  `Placeable` hint `:2140-2165`
- `include/GlobalCommandRegistry.h:17`, `src/GameECS.cpp:275`, `include/AgentActions.h:67` —
  `hidden` semantics
- `include/Components.h:127` (`Tile`), `:363` (`Placeable`), `include/Defaults.h:41,45`
- `src/Map.cpp:56-66` (tile creation), `:199-203` (per-tile entity, no `DisplayName`)
- `maps/Map_Farm.json` (tile types, Pietro at `13,9`), `data/objects/wheat_seed.json` (placement block)
- `CMakeLists.txt:119-154` — the task layer is not in `SOURCES`
- `src/TaskOptions.cpp:49` (`FreeNeighbourOffset`), `:330` (`PLANT_AT` option), `:440` (build action)
- `.agents/design/two-tier-agents.md` §5 `:126-158`, §7 `:315-320`, §8 `:340-355`
- `logs/2026-09-25_20-07-23-Pietro.txt` — the failing session
