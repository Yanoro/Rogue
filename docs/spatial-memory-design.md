# Agent vision and spatial memory

**Status:** design only. Nothing here is built.

**Date:** 2026-10-17.

**Scope:** how a character acquires a spatial picture of the world (vision), how it records what
it learned (`SpatialMemory`), and how that record is kept small (consolidation). The first
implementation target is **the player character only**, used as a test rig: the player has the same
body, `Velocity` and path-following as an NPC, it is drivable by hand, and it has no `AgentBrain`,
so a vision or memory bug cannot be mistaken for a model bug.

Companion documents: `docs/placement-design.md` (why ground knowledge is the weak link),
`docs/agent-attention-design.md` (the perception event pipeline, which this deliberately does
**not** depend on), `docs/task-system-design.md` (the component-not-history argument, and the
persistence deferral this doc inherits).

---

## 1. What an agent knows about space today

Nothing that outlives a single probe.

| surface | what the agent gets | where |
|---|---|---|
| `[SURROUNDINGS]` | entities within radius 10 with offsets, plus "You are at <location> (x,y)" | `include/AgentActions.h:678-745`, action at `:778` |
| `[LOCATIONS]` | every mapped location name, in every agent's prompt from spawn | `include/AgentActions.h:385` |
| `%LOCATIONS%` | the same global list, baked into the system turn at spawn | `src/GameECS.cpp:302` |
| tile radar | a glyph grid at a small radius, legend keyed by `tile->ascii->ch` | `include/AgentActions.h:621`, `:642` |

Three defects follow, and they are what this feature exists to fix:

1. **Omniscience.** Every agent is born knowing every location name (`%LOCATIONS%`) and can resolve
   any entity name anywhere in the world (`[MOVE_TO]` scans all `DisplayName` entities,
   `include/AgentActions.h:159-166`). The village has no strangers in it, only informed ones.
2. **No compression.** Anything learned is appended to `NPCContext::history` as prose and never
   pruned. An agent that walks around accumulates transcript, not knowledge: unbounded, unordered,
   unqueryable, and competing with its goal for context.
3. **Glyph coupling.** Terrain identity reaches the agent only through an ASCII character used as a
   legend key, so two surfaces sharing a glyph become one surface. The worst case is already live: a
   tile authored with no `character` gets `'?'` (`src/Map.cpp:146`), which is the same key the radar
   uses for `outside the map` (`include/AgentActions.h:650`), so a sprite-only surface is reported to
   the agent as off-map. `Tile::name` already exists and is the real vocabulary.

## 2. The decisions this document makes

1. **Knowledge is per-agent; geometry is shared.** `Location` stays an authored, engine-owned rect.
   Each character gets its own table of places and cells it has actually observed. The agent reads
   only its own table.
2. **A place can be authored or inferred.** Authored places inherit the map's geometry. Inferred
   places are clusters over the agent's own cells — which is how a place the map never knew about
   (a fishing spot, a second field) can exist at all.
3. **Acquisition is a vision cone plus a small radar, sampled passively; the active probe is
   unchanged.** `[SURROUNDINGS]` keeps its radius and its reliability. The cone decides what enters
   memory, not what is legal or what can be drawn.
4. **Consolidation lands in two steps: an engine-only throttle first, a model-backed pass second.**
   The first version must be judgeable without knowing whether the model or the indexing was wrong.
5. **This phase changes no agent prompt and gates no player action.** The player's memory is
   observed and drawn, never consulted. See §9 for what is deliberately deferred.

### What this phase does *not* deliver

Stated up front because it is easy to overclaim: **subjectivity is not delivered here.** The table
is per-agent, so two agents *can* disagree later, but in this phase labels are engine-derived from
authored names and tile names. A model may not invent or reword a label (§6). The subjective layer
— "abandoned house" for a house someone lives in — arrives with a naming verb and is out of scope.

---

## 3. Two layers: shared geometry, private knowledge

### 3.1 Why `Location` survives

`Location` (`include/Components.h:202`) is a rect with a name, authored in map JSON, used by the
editor, by `[MOVE_TO]` (`include/AgentActions.h:184`), by save/load and by the debug windows. It is
the only shared spatial vocabulary in the tree.

The temptation is to delete it once agents have their own places. That fails on one concrete need:
**a private place must be tellable to someone else.** The only way to say "about twelve tiles east
of the mill" is for "the mill" to be shared. Remove the authored anchors and two agents have no
language for geography at all — which is a worse outcome than the omniscience being removed. The
authored rect is the coordinate frame private places are *described against*, which is also exactly
what a told-by transfer will need later.

### 3.2 The agent's table

```cpp
enum class PlaceOrigin { Authored, Inferred };

// One place this character believes in. `label` is what it would say out loud.
struct KnownPlace {
  PlaceOrigin origin = PlaceOrigin::Inferred;
  std::string label;        // engine-derived: Location name, or a surface-derived phrase
  std::string authoringId;  // Location name for Authored; empty for Inferred
  GamePosition anchor;      // top-left of the extent, in map cells
  int width = 0, height = 0;
  // Part of the cluster key, not decoration. Two touching fields with different
  // crops are two places; without this they merge into one and the farmer's
  // "my wheat field" and "my barley field" become the same entry.
  std::string attribute;    // e.g. the surface or crop that made this cluster distinct
  int firstSeenTick = 0;
  int lastSeenTick = 0;
};

// One cell this character has actually observed. The raw record; places and
// consolidated entries are indexes over it, never replacements for it.
struct MemoryCell {
  GamePosition pos;
  std::string surface;   // Tile::name at observation time -- the real name, not a glyph
  bool blocked = false;  // Tile::blocksTile at observation time
  int observedTick = 0;  // engine tick; same clock FormatTaskAge already renders
  int visitCount = 1;    // re-observed unchanged; a cheap relevance signal for §6
};

struct SpatialMemory {
  std::vector<MemoryCell> cells;
  std::vector<KnownPlace> places;
  int lastConsolidatedTick = -1;
};
```

Two properties are load-bearing and match the argument in `docs/task-system-design.md` §10:

- **A component, not history.** Same reasons: costs no transcript, every consumer reads one current
  value, and it is queryable by a debug window.
- **Cells are the truth; places and consolidated entries are indexes over them.** A bad
  consolidation must never destroy the observation that produced it, or a consolidation bug becomes
  an unrecoverable knowledge bug.

### 3.3 Which place claims a cell

**Authored geometry wins where it overlaps.** When the agent observes cells inside an authored
`Location`, the place is that Location, labelled with its authored name. Inferred clusters only form
where no authored rect claims the cells. This prevents two labels for one area, and it makes the
first observable milestone simple: *walk into the mill and "The Mill" appears in your place list* —
the place table filling in as a consequence of vision, not of a spawn-time dump.

`attribute` splits contested ground. Cells sharing a surface (or crop) under one label form one
cluster; a different `attribute` on touching cells forms a second place. That is the whole mechanism
behind "a different location per field, depending on what is planted" without any new concept.

---

## 4. Acquisition: facing, cone, radar

### 4.1 Facing is derived, not plumbed

No movement code changes. `Velocity` (`include/Components.h:74`) is already driven by the steer
system (`src/GameECS.cpp:898`), which accelerates toward the next waypoint of
`MOVE_THROUGH_PATH_ACTION` and integrates friction and wall repulsion. So facing is a read:

```cpp
struct Facing { raylib::Vector2 dir = {0.0f, 1.0f}; };
```

Derived by one system, with three writers in priority order:

1. **An action that targets something** — harvest, talk, place, interact — turns the character to
   face the target. This matters more than it looks: without it a farmer harvests a tree behind him
   and his cone points at a wall while he works. It is also a free perception win, because facing
   the target at the start of an interaction reveals what sits *beside* the target.
2. **Movement** — `normalize(velocity)` while `|velocity| > epsilon`.
3. **Hold the last facing** otherwise. Facing is not forgotten by standing still.

Hysteresis on the epsilon, or a nearly-stopped character twitches its cone and re-samples
constantly.

**Player exception:** while moving, velocity; while stationary, the mouse cursor, which
`GetScreenToWorld2D(GetMousePosition(), camera)` already provides (`src/Game.cpp:333`). A player who
can sweep the cone without walking is a player who can debug the cone.

Per-character parameters are *fields on `Vision`* with global defaults — the schema is per-character
from the start, the content is not (§9).

### 4.2 The cone and the radar are one function

A cell is visible when it passes line of sight **and** either test:

- **radar:** within `radarRadius` cells;
- **cone:** within `range` cells and inside `±angle/2` of `facing` (dot product against the
  normalized cell direction).

One function, `VisibleCells(...)`, returns the set. Two consumers and one overlay read it (§7), for
the same reason `docs/placement-design.md:89` insists the placement gate and the executor be one
code path: an advertisement and an observation that drift apart are worse than either alone.

```cpp
// Per-character vision. Having this component is the opt-in: a character without
// it samples nothing and costs nothing, which is how the player-only phase is
// gated without a name check.
struct Vision {
  float range = DEFAULT_VISION_RANGE;                 // cells
  float angleDegrees = DEFAULT_VISION_ANGLE_DEGREES;  // full cone, not half-angle
  int radarRadius = DEFAULT_VISION_RADAR_RADIUS;

  // What the last sample saw. The overlay draws this, never a fresh sweep: an
  // LOS pass over ~390 cells at 60fps is not viable, and the set only changes
  // when the sampler runs anyway.
  std::vector<GamePosition> lastVisible;
  int lastSampleTick = -1;
};
```

Starting values, all in `include/Defaults.h` (the file already owns
`DEFAULT_SURROUNDINGS_RADIUS` at `:41` and `DEFAULT_PLACEMENT_MAX_DISTANCE` at `:45`):

| constant | initial | note |
|---|---|---|
| `DEFAULT_VISION_RANGE` | `22` | ≈380 cells in a 90° cone |
| `DEFAULT_VISION_ANGLE_DEGREES` | `90` | half-angle 45° |
| `DEFAULT_VISION_RADAR_RADIUS` | `2` | 13 cells, ≈ the placement reach |
| `DEFAULT_MEMORY_CELLS_MAX` | `512` | |
| `DEFAULT_MEMORY_PLACES_MAX` | `24` | |
| `DEFAULT_MEMORY_PLACE_CLUSTER_RADIUS` | `3` | cells this close join one cluster |

The sum is ≈390 cells, comparable to the radius-10 square the probe already scans — **the cone is
not cheaper, it is differently shaped.** That is the point: it reaches further ahead and nothing
behind.

The radar's justification is not only convenience. A cone tested against *cell centres* is lumpy at
1–3 tiles, where a 90° cone spans very few cells; below ~4 tiles the radar is the more accurate
instrument and the cone is the approximate one. The radar also covers the placement case, where
reach is one tile (`DEFAULT_PLACEMENT_MAX_DISTANCE`).

### 4.3 Line of sight

There is none in the tree today; `AStar` treats `blocksTile` as passable-or-not and nothing asks
whether a cell can be seen. The algorithm is nonetheless already known here: `InsertLine` in
`src/BlueprintExpansion.cpp:21` is a Bresenham whose comment documents the corner-touching caveat
that also bites visual rays.

Use a **per-cell raycast** first: from the character's cell to each candidate cell, stop at a
`blocksTile` cell. It is obviously correct, and at the sampling rate in §4.4 the cost is
irrelevant. Recursive shadowcasting is the upgrade if it ever profiles badly — not the starting
point, and not a correctness requirement.

### 4.4 When sampling happens

**On entering a new tile, or on facing change beyond ~22.5°.** Never per frame. An LOS sweep of ~390
cells at 60fps is not viable; at walking speed it is a handful of samples per second.

**Writes are idempotent.** Re-observing a cell updates `observedTick` and `visitCount`; it does not
append. The memory is a set of cells, not a log of sightings.

A re-observation that *disagrees* with what was stored (surface changed, a cell became blocked) is
the one interesting write: it is the engine's chance to record a contradiction. In this phase it
overwrites and bumps nothing else; the repair loop proper belongs with perception (§9).

### 4.5 Passive sampling and the active probe stay separate

| | passive (`Vision` system) | active (`[SURROUNDINGS]`) |
|---|---|---|
| when | on tile/facing change | when the agent asks |
| shape | 90° cone at 22 + radar at 2 | full radius 10, entities with offsets |
| purpose | fill memory | discover what is interactable *now* |
| gated by vision | — | **no** |

The active probe is deliberately **not** narrowed. Discovery is already the weakest link in the
placement path (`docs/placement-design.md:33-80`), and shrinking the probe to a 2-tile radar would
make turn-one placement worse in exchange for realism nobody asked for at that moment. Keeping them
separate also gives "look around" a real cost (a turn) instead of making it free and mandatory.

### 4.6 What occlusion must never do

Three hard rules, each of which would otherwise create a subtle bug:

- **It never gates legality.** Placement validity stays one function shared by the `[PLACE]` path
  and any probe (`docs/placement-design.md:89`). A cell the character cannot see is not a cell it
  cannot use; it is a cell it does not know about.
- **It never gates the player's actions** in this phase. The player keeps global click-to-move and
  its existing interaction rules. Memory is recorded, not read (§2.5).
- **It never hides the world.** Vision is a debug overlay, not fog of war (§9).

The agent-phase change these rules imply is stated now so it is not a surprise later: `[MOVE_TO]`
becomes knowledge-scoped — resolving against the character's own table of known characters, objects
and places rather than a global `DisplayName` scan — and its failure becomes teachable ("you don't
know a place called that; you know: …"). That is the same mechanism as §3.2, applied to entities.

---

## 5. Constants, caps and clocks

- **Clock.** `observedTick` uses the engine tick, and ages render through `FormatTaskAge`
  (`include/TaskList.hpp:711`) as `12s / 3m / 1h`. Not a calendar date: the sim has no calendar, and
  a second time vocabulary would eventually disagree with the first.
- **Caps.** `cells` capped at `DEFAULT_MEMORY_CELLS_MAX`, `places` at `DEFAULT_MEMORY_PLACES_MAX`.
  Overflow drops the lowest-value cells first (§6.1), never a cell inside a place. This is the first
  agent component whose size is unbounded *in the world* rather than in a list — `DEFAULT_TASK_LIST_MAX`
  bounds intentions, not geography — so the cap is design, not housekeeping.
- **Place merge before create.** A newly observed run of cells joins an existing cluster with the
  same `attribute` whose extent is within `DEFAULT_MEMORY_PLACE_CLUSTER_RADIUS`. Create only when no
  cluster claims it. Without this rule one walk across a meadow becomes thirty places.

---

## 6. Consolidation

The goal is compression with provenance, in two steps that can be judged separately.

### 6.1 First pass: the engine throttle (no model)

Pure, deterministic, and the default for the whole phase. It answers one question: *which cells earn
their slot?*

Ranking inputs available without a model:

- **recency** — `observedTick` versus now;
- **revisit count** — a path walked every day is more relevant than a corner seen once;
- **place membership** — a cell inside a `KnownPlace` outranks a stray cell (this is what keeps a
  field coherent while its surroundings fade);
- **surface interest** — optional and data-driven: an NPC may declare the surfaces it cares about
  (a farmer: grass, tilled soil; a fisherman: water, shore), which is a relevance prior requiring no
  inference at all. The player declares none, so its memory is pure recency and revisits.

Output: merged runs of same-surface cells, oldest and lowest-ranked dropped at the cap. **No prose is
generated, no label is written, no fact is invented.** This pass is judgeable on its own: the
window shows cells in, entries out, and the delta.

### 6.2 Second pass: Jev salience, out-of-band LLM merge

Only after the first pass is trusted. Two different jobs, deliberately given to two different model classes
— the same division `docs/agent-attention-design.md` §4 makes for the wake gate:

| job | model | question | output |
|---|---|---|---|
| salience | System 1 (`SystemOneClassifier`, `include/SystemOneRecorder.h:194`) | "how relevant is this observation to this character's current goals?" | one score |
| wording | System 2 chat model, **out of band** | "merge these observations into fewer entries" | a compressed entry list |

Salience is **goal-conditioned**: the state sent to the classifier includes the task digest. A
character with no goals gets a generic prior, which is correct rather than a bug — an agent with no
plans has no reason to notice the neighbour's crops. This is the mechanism behind "the farmer
notices crops, the fisherman notices water", and it needs no per-role prompt.

### 6.3 Consolidation cannot be a turn

`ParseMessageCommand` rejects any response containing more than one command with a warning and an
`InvalidAction` (`src/AgentBrain.cpp:569-576`). A `[REMEMBER …]` block therefore cannot ride along
beside a normal command, and a dedicated memory turn costs the character a turn in which it stands
still. So consolidation is **out of band**, and the tree already has the exact shape to copy:
`PromptRewriter` (`include/PromptRewriter.hpp`) runs a call on a worker thread with its own provider
context, completes on the empty-token sentinel, is collected on the main thread, and blocks no
frame. A `MemoryConsolidator` built the same way is a known-good pattern rather than new threading.

Two of that file's lessons transfer verbatim: a system-only request can return no content at all on
at least one backend (put the observations in a user turn), and `generateStream` returning means
*accepted*, never *finished*.

### 6.4 The guardrails

- **One author per field.** Task lists belong to the agent because intentions do (`docs/task-system-design.md`
  §3). Memory is impressions of engine-observed facts, whose raw form the engine already authored.
  So the consolidator may **select, merge, shorten and drop**; it may **not** invent a fact or write
  a label. Labels stay engine-derived (authored name, tile name) or are written by a dedicated verb
  later. This is the same failure mode §3 rejected — an auxiliary agent deriving an agent's state —
  and this rule is what keeps it rejected.
- **Reversible.** Cells are the truth (§3.2); consolidation writes indexes over them. Every
  consolidation event is logged with counts in and out.
- **Budgeted.** This is the first *periodic, non-event* model call that changes agent state, so the
  wake-rate ceiling in `docs/agent-attention-design.md` §6 does not bound it. Batch by count (run
  only when unconsolidated cells reach a threshold), one request in flight per character, and a
  global ceiling.

---

## 7. Debug surfaces (the actual deliverable of this phase)

Three surfaces, because each answers a question the others cannot.

### 7.1 The visibility overlay

Draw tinted cells for the character's **cached** visibility set — never recomputed per frame — plus
the cone boundary and facing marker, only when enabled.

Drawn in `GameDraw` after `ecs.run_pipeline(renderPipeline)` (`src/GameDraw.cpp:149`), next to the
existing A* path rectangle loop. That loop is the precedent: a debug overlay drawn directly with
raylib needs no entity, no component and no render system, and cannot perturb the render pipeline
that `docs/flecs-ecs` describes.

Without this overlay, cone bugs (an off-by-one half-angle, rays slipping through a corner) are
invisible and will be blamed on memory. This feature is more likely to die of "we cannot tell what it
is seeing" than of cost.

### 7.2 The memory window

`SpatialMemoryWindow`, following `TaskBoardWindow` (`src/DebugWindows.cpp:2492`): **collect rows,
then sort, then draw** — an ImGui table cannot be reordered while rows are being emitted, and
querying flecs mid-draw invites mutation hazards.

- Query: `ecs.filter<SpatialMemory>()`. Whoever has the component appears, which makes the
  "player only, for now" gate self-documenting rather than a hardcoded name check.
- Per character: cell count, place count, age of the newest cell, last consolidation (counts in and
  out, tick).
- Live-editable cone parameters (angle, range, radar radius) so tuning does not require a rebuild.
- Two agents showing different labels for the same rect will be the first visible proof that
  subjectivity works, once labels can be authored (§9).

### 7.3 The text dump

One file per character, under `logs/`, written on demand: cells, places, consolidation events. The
repo has no test suite; its verification idiom is reading transcripts (`docs/agent-attention-design.md`
§12 uses exactly this discipline). A dump is diffable between runs in a way a window is not.

---

## 8. Stages, with acceptance for each

Build in `nix-shell`, then run. Runtime verification is the owner's, not the agent's, for anything on
the AI path; here the first three stages have no AI path at all, which is the point of the ordering.

| stage | what lands | acceptance |
|---|---|---|
| **0. Facing** | `Facing` component + derive system; players and NPCs; no consumer yet | Facing follows movement; holds when stopped; the player's follows the cursor when idle; overlay draws the marker |
| **1. Vision** | `Vision` component (angle, range, radar), `VisibleCells` + Bresenham LOS, overlay | Cells appear/disappear as you turn; a wall blocks; the radar fills the close ring; toggle off costs nothing |
| **2. Memory** | `SpatialMemory`, sampler, caps, idempotent writes, memory window + dump | Walking fills cells with no duplicates; a revisit bumps rather than appends; the cap holds; cells at zero on spawn |
| **3. Places** | place derivation from cells, `Authored` match against `Location`, `attribute` clustering | Walking into the mill adds "The Mill"; two touching crops yield two places; no authored place appears before its cells are seen |
| **4. Engine consolidation** | ranking, merge runs, drops at cap, event log | Counts in/out visible; a drop never removes a place cell; raw cells still inspectable |
| **5. Model consolidation** | Jev salience (goal digest in state) + out-of-band merge, behind a setting, stage 4 the default | Off by default; identical outputs when the backend is unavailable; logged verdicts before behaviour depends on them |

Stages 0–2 attach `Vision` and `SpatialMemory` to the player only. `Facing` goes on every character,
so the agent phase is a matter of attaching the components, not of changing the systems.

Component registration follows the established order rule: register `MemoryCell` and
`KnownPlace` **before** `std::vector<…>` of them, and the vector before the component that holds it —
flecs resolves a member's type at registration time (`include/Components.h:765-780`, and
`std_vector_support` at `:694`). Register before `LoadMap` as well, per known-issue #2.

---

## 9. Deliberately not built

- **Fog of war.** Rendering is untouched; the cone is an overlay.
- **Player gating.** The player's memory is never read by its own action path.
- **Told-by transfer.** Knowledge provenance from dialogue (`source: ToldBy`) and asking for
  directions. Needs the talk path and a lying story; the player-first phase cannot exercise it.
- **Prompt wiring.** No digest, no `%PLACES%`, no change to any agent's system turn. Places follow
  the rule the task digest already settled: authored seeds are static, live state goes in the wake
  prompt (`include/TaskList.hpp:495`).
- **A naming verb.** No agent-authored labels this phase, so §3.2's per-agent subjectivity is
  schema-ready but not exercised.
- **Per-character cone *values*.** Fields exist; content does not. No NPC JSON key yet.
- **Shadowcasting**, seeing through doors/windows, sound propagation, and NPC perception of the
  player. The last one is the real prerequisite for stealth: the player is a separate,
  non-perceiving code path (`docs/agent-attention-design.md` §10), so "sneak up on an agent" is
  downstream of the perception component, not of this doc.

---

## 10. Open questions

1. **Do the constants in §4.2 survive contact?** 90° at 22 tiles is a guess. The overlay and the
   live-editable window exist precisely so the number is chosen from looking at it.
2. **Does the player's memory grow without bound in a long session?** The cap answers storage, not
   usefulness. If the player can never forget, `lastSeenTick` is the only thing distinguishing a
   village from a meadow.
3. **Should an observed authored Location and an inferred cluster ever merge into one row?** This doc
   says authored wins and inferred never forms on claimed cells. The cost is that a place the agent
   perceives *as a whole* but the map authored as two rects stays two rows.
4. **What is the acceptance threshold for stage 4?** "Fewer entries, no place lost, and I can still
   explain every surviving entry from a cell" is the intent, but it is a judgement, not a number.
5. **Is `SpatialMemory` the right name?** `Components.h` has no namespaces and already carries
   generic names (`Tile`, `Location`, `Frame`, `Storage`). `SpatialMemory`, `MemoryCell`,
   `KnownPlace`, `Vision`, `Facing` are collision-safe against what is there today.

---

## 11. References

- `include/Components.h` — `Velocity:74`, `Tile:156`, `Location:202`, `TaskList:411`,
  `std_vector_support:694`, registration order example `:765`, `AgentGoal` registration `:816`
- `include/AgentActions.h` — `[MOVE_TO]` resolution `:159-184`, `[LOCATIONS] :385`,
  `GetSurroundingsRadar :621`, glyph legend key `:642`, `?` as off-map `:650`,
  `GetSurroundingsList :678`, "You are at" `:745`, `SurroundingsAction :778`
- `include/Defaults.h` — `DEFAULT_SURROUNDINGS_RADIUS:41`, `DEFAULT_PLACEMENT_MAX_DISTANCE:45`
- `include/TaskList.hpp` — `RenderTaskDigest:516`, `FormatTaskAge:711`
- `include/PromptRewriter.hpp` — out-of-band call shape (worker thread, sentinel, main-thread collect)
- `include/SystemOneRecorder.h:194` — `SystemOneClassifier`
- `src/GameECS.cpp` — `%LOCATIONS% :302`, tile layer `:370`, steer system `:898`, player setup
  `:3148`, player `Velocity` `:3155`, player `CharacterTag`:3171
- `src/GameDraw.cpp:149` — `run_pipeline`, and the A* overlay precedent just after it
- `src/Game.cpp:333` — `GetScreenToWorld2D` for cursor facing; `:528` player click-to-move
- `src/PathFinding.cpp:126` — `gCost` has no terrain weight (grounds §9's deferral of street bias)
- `src/Map.cpp:146` — `'?'` fallback for a tile authored without a character (the off-map collision)
- `src/BlueprintExpansion.cpp:21` — Bresenham line helper and its corner-touching caveat
- `src/AgentBrain.cpp:569-576` — one command per response, so consolidation cannot be a turn
- `src/DebugWindows.cpp:2492` — `TaskBoardWindow::Draw`, the board pattern to copy
