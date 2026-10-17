# Agent vision and spatial memory

**Status:** design only. Nothing here is built.

**Date:** 2026-10-17.

**Scope:** how a character acquires a spatial picture of the world (vision), how it records what
it learned (`SpatialMemory`), and how that record is kept small (consolidation). The first
implementation target is **the player character only**, used as a test rig: the player has the same
body, `Velocity` and path-following as an NPC, it is drivable by hand, and it has no `AgentBrain`,
so a vision or memory bug cannot be mistaken for a model bug.

Companion documents: `docs/place-derivation-design.md` (the place rule that replaces §3.3),
`docs/placement-design.md` (why ground knowledge is the weak link),
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
//
// A tile is ground only: a wall, door or chest is an object standing in the cell,
// never a Tile::name. "Blocked" therefore has to combine Tile::blocksTile with the
// blocking objects in the cell; the ground gate alone would miss every wall.
struct MemoryCell {
  GamePosition pos;
  std::string surface;   // Tile::name at observation time -- ground only; walls/doors are objects
  bool blocked = false;  // Tile::blocksTile OR a blocking object (wall, door, furniture) at that time
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

> **Superseded.** The rule in this section — authored rects claim cells, cells cluster by terrain
> surface — was implemented, tested, and produced arbitrary places on a single-surface map. It is
> replaced by `docs/place-derivation-design.md`: objects and terrain are *seeds*, clusters are
> computed globally, and authored rects no longer claim anything. This section is kept because the
> record shapes it introduced (members, extents, facts) are still the ones the new rule fills.

**Authored geometry wins where it overlaps.** When the agent observes cells inside an authored
`Location`, the place is that Location, labelled with its authored name. Inferred clusters only form
where no authored rect claims the cells. This prevents two labels for one area, and it makes the
first observable milestone simple: *walk into the mill and "The Mill" appears in your place list* —
the place table filling in as a consequence of vision, not of a spawn-time dump.

The extent is the **observed** extent, never the authored rectangle: a place claims only the cells
the agent actually saw. That is what makes "no authored place appears before its cells are seen"
true, and it is why the record stores `memberCells` and recomputes the box from them.

The authored name becoming the label **on sight** is the convenient policy — the reason `authoringId`
is a separate field is that the data-driven `OnSight` / `OnTold` split (a mill is a mill you can see;
a settlement's name has to be heard) then changes only whether `label` is copied, not the schema.

`attribute` splits contested ground. Cells sharing a surface (or crop) under one label form one
cluster; a different `attribute` on touching cells forms a second place. That is the whole mechanism
behind "a different location per field, depending on what is planted" without any new concept. The
limitation to name now: `attribute` is the **surface**, so two fields distinguished only by what is
planted *on* the same soil are one place until object-derived attributes exist.

### 3.4 The record: three layers, three lifetimes

**The governing rule: store numbers, render prose.** Nothing textual is persisted except a name
something actually gave a place. Every description is regenerated from the record at render time,
which is why "east of where I started" can become "east of the mill square" without touching a single
stored field. A frozen phrase rots the moment the world or the agent's vocabulary changes.

```cpp
// ---- Layer 1: recent detail. Bounded, evictable, engine-only. ---------------
struct MemoryCell {
  GamePosition pos;
  std::string surface;   // Tile::name at observation: "grass", "path", "tilled soil"
  bool walkable = true;  // !Tile::blocksTile at observation
  int observedTick = 0;
  int visitCount = 1;    // a road walked twice outranks a corner seen once
};

// What was standing somewhere. The item type is the strongest evidence in the
// whole format and it is already data: "mill", "anvil", "apple_tree" say more
// about a place than any surface name does.
struct SeenObject {
  std::string templateId;      // ItemType::type; empty for a character or a drop
  std::string name;            // "Mill", "Pietro"
  GamePosition pos;
  bool character = false;      // a person is knowledge of a different kind
  int firstSeenTick = 0;
  int lastSeenTick = 0;
  int vanishedTick = -1;       // a later look at that cell found nothing there
};

// ---- Layer 2: durable structure. Survives eviction, capped, queryable. ------
// Counts, never a verdict. "7 structures" is evidence; "village" is a conclusion.
// `name` is the raw vocabulary and `category` only says which kind it is: grouping
// "mill" and "smeltery" into "workshop" is the renderer's interpretation, so a
// genre pack can rename content without editing C++.
struct PlaceFact {
  std::string category;  // "surface" or "object"
  std::string name;      // tile name, or object template id
  int count = 0;
};

// Where a place came from, and how firmly it is believed.
//
//   Origin   -- the cell the character first stood on. The root of the coordinate
//               system for someone who knows nothing, and the anchor every other
//               description falls back to.
//   Inferred -- a cluster derived from observed cells (section 3.3).
//   Authored -- overlaps a map Location.
enum class PlaceOrigin { Origin, Inferred, Authored };
enum class KnowledgeSource { Observed, Told, Inferred };

struct KnownPlace {
  int id = 0;                       // stable for the session; a label attaches to it
  PlaceOrigin origin = PlaceOrigin::Inferred;
  KnowledgeSource source = KnowledgeSource::Observed;
  unsigned char confidence = 255;   // observed full, told lower, inferred lower still
  GamePosition anchor;              // top-left of the extent
  int width = 0, height = 0;
  std::vector<PlaceFact> facts;     // the evidence its description is built from
  std::string attribute;            // cluster key: surface, crop, or structure kind
  std::string label;                // EMPTY until something names it
  std::string authoringId;          // the map's name, once learned -- not on sight
  int firstSeenTick = 0, lastSeenTick = 0;
  std::vector<std::uint64_t> memberCells;  // the observed cells this place claims
};

// ---- Layer 3: the only stored prose, and only when authored -----------------
// Written by a naming verb or a told-by transfer. Never by the engine, and never
// by a consolidation pass (section 6.4's "one author per field").
struct MemoryNote {
  std::string placeLabel;
  std::string text;
  int tick = 0;
};

struct SpatialMemory {
  KnownPlace origin;                    // created on the first sample, label ""
  // Keyed by VisionQuery::CellKey, which is what makes a re-observation an
  // in-place update rather than an append.
  std::unordered_map<std::uint64_t, MemoryCell> cells;
  std::unordered_map<flecs::entity_t, SeenObject> objects;
  std::vector<KnownPlace> places;
  std::vector<MemoryNote> notes;
  int lastVisionTick = -1;              // the sample this record has consumed
};
```

**The lifetimes are the point.** Layer 1 is a bounded store — the cap is a memory budget, not a
simulation rule — and evicts by (visitCount, then age). Layer 2 is durable: a place is never evicted
while it is still visited, so detail decays into structure rather than into nothing. Layer 3 costs
nothing and is written only by consent.

**Bootstrapping a stranger.** On the first sample, with nothing else known, memory creates exactly
one place: `Origin`, anchored on the cell it is standing on, `label` empty. That is the only thing a
memory can be anchored to when it knows nothing, and every later description falls back to it. Which
anchor a description uses is a *rendering* decision, so the sentence improves as the world is
learned without the record changing.

**Evidence, not verdicts.** The engine records `surface`, `templateId` and counts. It never records
"village", "forest" or "road". Absence is representable — zero structures, zero routes, zero tilled
rows is a fact about a place, which is what makes "I am outside the settlement" a statement the
format can support rather than a missing value.

**A place has no label until something names it.** Until then the renderer prints an evidence phrase
("a cluster of stone buildings around a path junction"), derived from `attribute` and the largest
facts. Naming is an agent verb (section 9), and this is where it belongs: an agent creates a location
by *naming a cluster it has evidence for*, not by being handed one.

**`RouteFrontier` and the knowledge horizon are derived views, never state.** "A path leaves my
knowledge to the east" is recomputed from `cells` on demand. Storing it would create a second
authority for the same fact and a staleness bug of its own.

**Shapes are a derived index, and only because the decomposition is exact.** `ToShapes` merges
horizontal runs of one surface and then stacks vertically adjacent runs with an identical span. The
union of the resulting rects is *precisely* the set of known cells, so unlike a bounding box it
cannot claim ground the agent never saw — which is the only reason a shape may be shown to a model at
all. Measured on a ragged disc of explored ground (the shape a union of cone sweeps produces):
**1961 cells → 98 rects, 8.5× smaller** than the cell list. A diagonal road or scattered single cells
decompose into 1×1 rects (~1×), and no circle or curve fitting is attempted: a coastline is not a
circle, and a failed fit is a shape worse than the cells it replaced.

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

That cursor rule has a persisted debug switch (`cursorFacing`, on by default, in the Vision tab)
which pins a stationary player's facing so a cone can be held still or swept past something without
the cursor dragging it. Walking still turns the character either way — this is a debugging control,
not a gameplay mode.

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
| `DEFAULT_MEMORY_CELLS_MAX` | `2048` | a 40-tile walk observes more than a thousand cells |
| `DEFAULT_MEMORY_OBJECTS_MAX` | `256` | |
| `DEFAULT_MEMORY_PLACES_MAX` | `24` | |
| `DEFAULT_MEMORY_PLACE_CLUSTER_RADIUS` | `3` | cells this close join one cluster |

The sum is ≈390 cells, comparable to the radius-10 square the probe already scans — **the cone is
not cheaper, it is differently shaped.** That is the point: it reaches further ahead and nothing
behind.

The radar's justification is not only convenience. A cone tested against *cell centres* is lumpy at
1–3 tiles, where a 90° cone spans very few cells; below ~4 tiles the radar is the more accurate
instrument and the cone is the approximate one. The radar also covers the placement case, where
reach is one tile (`DEFAULT_PLACEMENT_MAX_DISTANCE`).

### 4.3 Line of sight, and what is opaque

There is no line-of-sight code in the tree; `AStar` treats `blocksTile` as passable-or-not and
nothing asks whether a cell can be *seen*. The algorithm is nonetheless already known here:
`InsertLine` in `src/BlueprintExpansion.cpp:21` is a Bresenham whose comment documents the
corner-touching caveat that also bites visual rays.

**Sight is blocked by objects that carry `BlocksVision`, never by terrain.** This is a deliberate
correction to the obvious design (block on `blocksTile`): a river blocks movement and is perfectly
transparent, so tying the two together would blind whoever looked across one. `BlocksVision` is a
sibling of `Obstacle`, not a field on it, because the two are independent in both directions — a
glass wall is solid and transparent, tall grass or smoke is the reverse. Tiles describe ground;
objects describe what stands on it and gets in the way.

Consequences worth stating plainly:

- A `blocksTile: true` **ground** tile does not block sight. The village's walls are currently
  misclassified as wall tiles (the debt in `tile-and-object-vocabulary.md`), so they are currently
  transparent to vision. Under the corrected vocabulary a wall is an **object**; making it opaque
  means authoring it with `BlocksVision`, not giving ground a sight field.
- An object must be *authored* with the component. Nothing blocks sight by default.

Use a **per-cell raycast** first: from the character's cell to each candidate cell, walk the
Bresenham line and stop at a cell occupied by a `BlocksVision` object. The two endpoints are excluded
on purpose — standing on a spot must not blind you, and a character can see the barrel that hides
what is behind it (which is also what makes "the wall blocks" mean "not the tile past the wall"
rather than "not the wall"). It is obviously correct, and at the sampling rate in §4.4 the cost is
irrelevant. Recursive shadowcasting is the upgrade if it ever profiles badly — not the starting
point, and not a correctness requirement.

### 4.4 When sampling happens

**On entering a new tile, on facing change beyond 22.5°, on a parameter change, and on a periodic
ceiling of 30 ticks (~0.5 s).** Never per frame. An LOS sweep of ~390 cells at 60fps is not viable;
at walking speed it is a handful of samples per second.

The periodic ceiling is what notices an object appearing or being felled without tracking either
event: there is no change notification, and a stale opaque set is exactly the quiet wrongness this
feature exists to make visible. It costs two sweeps a second for one character.

The opaque set is rebuilt *after* the staleness test, so a character standing still pays one
comparison per frame rather than a sweep.

**Writes are idempotent.** Re-observing a cell updates `observedTick` and `visitCount`; it does not
append. The memory is a set of cells, not a log of sightings.

A re-observation that *disagrees* with what was stored (surface changed, a cell became blocked) is
the one interesting write. In this phase it **overwrites** the stored surface and walkability and
bumps the tick and the visit count — never the other way round. Keeping the first sighting would
leave sand laid over an already-remembered floor recorded as floor forever, which is the bug this
paragraph exists to prevent. Marking the contradiction (`[!]` in a digest) and the repair loop proper
belong with perception (§9).

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
- **Caps.** `cells` capped at `DEFAULT_MEMORY_CELLS_MAX`, `objects` at `DEFAULT_MEMORY_OBJECTS_MAX`,
  `places` at `DEFAULT_MEMORY_PLACES_MAX`. Cell overflow drops the lowest-value cells first —
  lowest `visitCount`, then oldest `observedTick` — never a cell inside a durable place (§6.1). This
  is the first agent component whose size is unbounded *in the world* rather than in a list —
  `DEFAULT_TASK_LIST_MAX` bounds intentions, not geography — so the cap is design, not housekeeping.
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

The window is `VisionDebugWindow`, in two tabs.

**Vision** is what the sweep sees right now: the facing-marker and visible-cell toggles, the cone
parameters (edited live on the component, so tuning needs no rebuild), the visible-cell count, and a
count of characters with and without a `Facing`. That count is why the window shipped before anything
consumed a facing: "every character got a direction" otherwise turns into a cone bug two stages
later.

**Memory** is what has been written down — cell/object/place counts against their caps, the origin,
when the last sample was consumed, and **the record itself as JSON**.

The record is JSON rather than a table, and that is a deliberate change from the original "board"
sketch: the dump writes the same string, and the worked examples in §12 use the same shape. One
serializer means the window, the file and the design document cannot disagree about what the format
is, and the examples stop being illustrative and become the acceptance artifact.

- Query: the window reads `playerEntity`'s `SpatialMemory` directly today. The per-character board —
  `ecs.filter<SpatialMemory>()`, collect rows, sort, then draw, following `TaskBoardWindow` — arrives
  with the agent phase, when more than one character has a memory.
- **Place rectangles.** A toggle draws one box per place in the player's memory: the extent as an
  outline, the cells the place actually claims tinted inside it, and a floating label (`#id surface`
  for an unnamed inferred place, the label once one is given). The gap between outline and tint is
  deliberate — the outline is a bounding box, so for an L-shaped cluster it covers ground the
  character never observed, and the tint is what makes that visible instead of implied.
- **Colour swatches.** The Memory tab lists each place with the same colour its rectangle uses, its
  extent, whether it is authored or inferred, and its facts in raw vocabulary. Colours are a stable
  function of the place id, so the same place keeps its colour across runs.
- Live-editable cone parameters stay here, because this is the window a developer has open while
  tuning.
- Two agents showing different labels for the same rect will be the first visible proof that
  subjectivity works, once labels can be authored (§9).

### 7.3 The dump

One JSON file per character, under `logs/`, written on demand from the Memory tab:
`logs/<run>-<name>-memory.json`. The repo has no test suite; its verification idiom is reading
transcripts (`docs/agent-attention-design.md` §12 uses exactly this discipline), and a JSON dump is
diffable between runs in a way a window is not — which is what makes "did the walk fill the same cells
twice" answerable.

It is the same serializer the window renders (`Memory::DumpJson` in `include/SpatialMemory.hpp`), so
"the dump reproduces the window" is true by construction rather than by discipline.

**Compact by default.** Cells are one documented tuple per line rather than one indented object per
cell — `"cellFormat": "x, y, surface, walkable, observedTick, visitCount"` then `[12, 14, "path",
true, 5380, 1]` — which is about 42 bytes per cell instead of ~170, and still line-diffable because
nlohmann's pretty-printer would otherwise spend eight lines on a six-field entry. The `shapes` array
is always present; the raw `cells` array is behind the window's **Include cells (full detail)**
switch, which is a view preference and therefore not persisted.

Measured on the 1961-cell disc above: 82 KB detail, 9.7 KB compact.

### 7.4 The rendering rule: situation, never intention

**The spatial text may contain nouns, numbers, ages and directions, and no verb of intent.**

The test is mechanical: if a line contains an imperative or an evaluative clause, the engine just
authored an intention. "A path leads east" is a fact. "A path leads east **and you have not followed
it**" is a to-do item in prose, and a digest containing to-dos routes around
`docs/task-system-design.md` §3 — intentions would enter the agent from the engine, quietly, where
nobody looks for a second author.

The reason is not purity, it is coverage. An enumeration of tips is **O(contexts)** and can never be
finished: there is always a river, a fork, a burnt field, a name in a language nobody taught the
engine. A uniform description of the situation is **O(1)** and covers every context for free.

So the engine reports the **situation** and never the **inference**:

| engine supplies | model supplies | engine supplies as a *verb*, if anything |
|---|---|---|
| a path exists at the eastern edge of what I know | paths connect places; follow it | walk a route |
| a 40-minute-old name now sits beside a person seen 30 seconds ago | "abandoned" may be wrong | rename a place |

Conflating those three is how a hint gets written: the first column is a fact, the second is common
sense the model already has, and the third is a *capability* that belongs in the command list
advertised uniformly, not in a nudge that fires in one situation. `[MOVE_TO]` being advertised does
not tell the agent where to go; a route-following verb is the same kind of thing.

Three consequences, all of them bugs avoided:

- **No `EXPLORATION` section, no "worth investigating", no "you have not".** If the facts are right,
  the model notices. If it does not, the fix is the prompt or the world, not a hardcoded tip that
  only works at one fork by one river.
- **The engine reports factual disagreements between its own observations**, and only those. "This
  cell was tilled soil an hour ago and is wheat now" is engine-owned (`[!]`). "Your name for this
  house no longer matches what you saw" is not: a house can be abandoned *and* have someone at the
  door, and an ironic name an agent chose must not be silently corrected by the narrator. Print the
  label, print the observation, print the ages, stop.
- **Selection is by a uniform rule** — recency, place membership, the cap — never "this looks
  relevant to what the agent is trying to do". Capping is unavoidable and is not hinting; relevance
  filtering is hinting wearing a different hat.

The one thing the engine must not do in the name of this rule is *bury* a fact. Few, prominent, and
uniform beats many, contextual, and guided.

---

## 8. Stages, with acceptance for each

Build in `nix-shell`, then run. Runtime verification is the owner's, not the agent's, for anything on
the AI path; here the first three stages have no AI path at all, which is the point of the ordering.

| stage | what lands | acceptance |
|---|---|---|
| **0. Facing** | `Facing` component + derive system; players and NPCs; no consumer yet; marker overlay + `VisionDebugWindow` | Facing follows movement; holds when stopped; the player's follows the cursor when idle; overlay draws the marker; the toggle survives a restart; the window reports zero characters without a `Facing` |
| **1. Vision** | `Vision` component (angle, range, radar), `VisibleCells` + Bresenham LOS in `include/VisionQuery.hpp`, overlay | Cells appear/disappear as you turn; an object carrying `BlocksVision` casts a gap behind it (a tree, the mill); the radar fills the close ring; toggle off costs nothing; tiles never block |
| **2. Memory** | `SpatialMemory` (§3.4), the memory writer consuming `Vision::lastVisible`, the `Origin` place, caps, idempotent writes, memory section in the Vision Debug window + dump. Place clustering is stage 3 | Walking fills cells with no duplicates; a revisit bumps `visitCount` rather than appending; the cap holds and evicts the least-visited first; cells and objects are zero on spawn; the dump reproduces the window |
| **3. Places** (superseded — see `docs/place-derivation-design.md` for the rule that replaces it) | `Memory::DerivePlaces`: `Authored` match against `Location`, surface clustering with a radius, membership-based extent, facts in raw vocabulary | Walking into the mill adds "The Mill" with `authoringId` set; two touching surfaces yield two places; no authored place appears before its cells are seen; a place's extent never exceeds its member cells |
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

- `include/Components.h` — `Velocity:74`, `Facing:106`, `Vision:132`, `MemoryCell:161`,
  `SeenObject:172`, `PlaceFact:184`, `KnownPlace:199`, `MemoryNote:217`, `SpatialMemory:223`,
  `Tile:156`, `Location:202`, `TaskList:411`, `BlocksVision:573`, `std_vector_support:694`,
  registration order example `:765`, `AgentGoal` registration `:816`
- `include/VisionQuery.hpp` — `CellKey`, `PathIsOpaque`, `VisibleCells` (the one sweep)
- `include/SpatialMemory.hpp` — `EnsureOrigin`, `ObserveCells`, `ObserveObjects`,
  `EnforceCellCap`, `ToShapes`, `DumpJson`
- `src/GameECS.cpp` — `ECSInitMemorySystems:1213`, `DumpSpatialMemory:1238`,
  `%LOCATIONS% :302`, tile layer `:370`, steer system `:898`, player setup `:3148`,
  player `Velocity` `:3155`, player `CharacterTag` `:3171`
- `include/AgentActions.h` — `[MOVE_TO]` resolution `:159-184`, `[LOCATIONS] :385`,
  `GetSurroundingsRadar :621`, glyph legend key `:642`, `?` as off-map `:650`,
  `GetSurroundingsList :678`, "You are at" `:745`, `SurroundingsAction :778`
- `include/Defaults.h` — `DEFAULT_SURROUNDINGS_RADIUS:41`, `DEFAULT_PLACEMENT_MAX_DISTANCE:45`,
  vision block `:72`
- `include/TaskList.hpp` — `RenderTaskDigest:516`, `FormatTaskAge:711`
- `include/PromptRewriter.hpp` — out-of-band call shape (worker thread, sentinel, main-thread collect)
- `include/SystemOneRecorder.h:194` — `SystemOneClassifier`
- `src/GameDraw.cpp:149` — `run_pipeline`, and the A* overlay precedent just after it
- `src/Game.cpp:333` — `GetScreenToWorld2D` for cursor facing; `:528` player click-to-move
- `src/PathFinding.cpp:126` — `gCost` has no terrain weight (grounds §9's deferral of street bias)
- `src/Map.cpp:146` — `'?'` fallback for a tile authored without a character (the off-map collision)
- `src/BlueprintExpansion.cpp:21` — Bresenham line helper and its corner-touching caveat
- `src/AgentBrain.cpp:569-576` — one command per response, so consolidation cannot be a turn
- `src/DebugWindows.cpp:2492` — `TaskBoardWindow::Draw`, the board pattern to copy

---

## 12. Worked examples

Each example shows the record the engine holds and the text the agent receives. The same record
renders differently as the agent learns — that is the payoff of "store numbers, render prose" (§3.4).

**Status: none of the agent-facing text below exists yet.** The record in stage 2 is real and
inspectable in the Vision Debug window and the dump; there is no digest renderer and no `[RECALL]`
verb. These are the target the later stages aim at, and the acceptance reference for stage 3.

Two of these are deliberately *revised*: an earlier draft had the engine write an `EXPLORATION` tip
and a "your name disagrees" sentence. Both were engine-authored intentions and are removed under
§7.4.

### 12.1 First sample — a stranger in open country

Engine:

```json
"places": [{ "origin": "Origin", "confidence": 255,
             "anchor": {"x":41,"y":20}, "width": 0, "height": 0,
             "label": "", "attribute": "start", "facts": [] }],
"cells":  [{ "pos": {"x":41,"y":20}, "surface": "grass", "walkable": true,
             "observedTick": 300, "visitCount": 1 }],
"objects": []
```

Agent:

```
WHERE YOU ARE
  an unnamed spot, open grass on every side you have looked at.
  (engine: 1 cell seen, no structures, no paths, no water)
  You know no other place, so everything you find will be described
  from here: "east of where you started".
```

### 12.2 A path at the edge of knowledge — no tip

Engine (cells plus a derived view, stored nowhere):

```json
"cells": [ "... 90 cells, 14 of them path" ],
"derived": { "knowledgeHorizon": { "n":"grass", "e":"path", "s":"grass", "w":"grass" } }
```

Agent:

```
WHERE YOU ARE
  an unnamed spot: grass on both sides, a path underfoot.
  (engine: 90 cells seen; 14 of them path; no structures, no water)

WHERE YOUR KNOWLEDGE ENDS
  north: grass          east: path
  south: grass          west: grass
```

That is all. The model sees a path underfoot and a path at its eastern horizon and supplies the
common sense — *paths connect places, follow it*. The engine makes the situation legible and stops.

### 12.3 Arriving — evidence, and still no name

Engine:

```json
"places": [ "... Origin ...",
  { "origin": "Inferred", "confidence": 255,
    "anchor": {"x":40,"y":18}, "width": 8, "height": 6,
    "label": "", "attribute": "stone",
    "facts": [ {"category":"object","name":"mill","count":1},
               {"category":"object","name":"anvil","count":2},
               {"category":"surface","name":"path","count":30},
               {"category":"surface","name":"tilled soil","count":12} ] }]
```

Agent:

```
WHERE YOU ARE
  a cluster of stone buildings around a path junction.
  (engine: 7 buildings, 2 workshops, 30 path cells;
          12 planted rows to the west)

  Nobody has named this place. You have not heard anyone call it anything.
```

The engine never says *village*, and it never says *workshop* either: "7 buildings, 2 workshops" is
the renderer grouping `mill`/`smeltery`/`anvil` from their template components. Storing counts of the
raw vocabulary is what lets that interpretation live outside the record.

### 12.4 Named, and told — two kinds of knowledge

Engine:

```json
"places": [
  { "label": "the mill square", "authoringId": "", "confidence": 255,
    "notes": [ {"text":"named by you","tick":3900} ] },
  { "label": "the ford", "authoringId": "", "confidence": 120,
    "anchor": {"x":28,"y":44}, "facts": [], "lastSeenTick": 0 } ]
```

Agent, after `[LOCATIONS]`:

```
PLACES YOU KNOW
  the mill square            (here; named by you 2m ago)
  7 buildings, 2 workshops, 30 path cells
  the ford                   (told to you by Margarethe 4m ago; she was not sure)
  you have never been there
```

Confidence below 255 renders as "she was not sure"; `lastSeenTick == 0` renders as "you have never
been there". A told place is a name and a position with no cells behind it.

### 12.5 Something changed — the field, and the tree

Engine:

```json
"cells": [ { "pos": {"x":12,"y":14}, "surface": "tilled soil", "observedTick": 5400,
             "reobservation": { "of": "tilled soil", "now": "tilled soil + wheat",
                                "since": 900 } } ],
"objects": [ { "templateId": "apple_tree", "pos": {"x":9,"y":12},
               "lastSeenTick": 900, "vanishedTick": 5400 } ]
```

Agent:

```
WHEN YOU LAST SAW THIS PLACE
  [!] the field west of the path: you saw bare tilled soil 1h ago;
      it is now wheat.
  [!] the apple tree by the wall is gone -- nothing is standing there now.
```

`[!]` is reserved for a factual disagreement between two of the engine's own observations.

### 12.6 The wrong belief — the abandoned house

Engine:

```json
{ "place": { "label": "the abandoned house", "confidence": 255,
             "facts": [ {"category":"surface","name":"stone floor","count":1} ] },
  "objects": [ { "templateId": "", "name": "Pietro", "character": true,
                 "pos": {"x":17,"y":6}, "firstSeenTick": 7200, "lastSeenTick": 7260 } ] }
```

Agent, after `[RECALL "the abandoned house"]`:

```
the abandoned house            (named by you 40m ago)
  a single stone structure, 1 room, no path
  seen here: Pietro, 30s ago
```

No verdict, no suggestion, no `[!]`. The engine cannot know that a house can be abandoned *and*
have someone at the door, so it prints the label, the observation, and the ages. Whether the name is
now wrong is the agent's judgement, and that judgement is the point of the feature.
