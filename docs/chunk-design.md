# Chunks: world structure and the agent's spatial view

**Status:** design, 2026-10-31. **Implemented:** the chunk geometry and per-agent
handle layer (`include/Chunk.hpp`), the area card / leg trace / journey
(`include/ChunkView.hpp`), both navigators (single prompt `JOURNEY`/`AREA`/expanded
`LAST RESULT`; two-context strategist observation and S1 state), all `chunks`
settings, the structured JSON trace, and a Chunks tab in the Navigation debug
window. Object/examine legs join the journey. Not implemented yet: agent chunk
naming and chunk targets.

**Scope:** a fixed, configurable chunk grid over the world, plus an agent-facing view of
the agent's **own knowledge** of that grid: the current/stopped area card and the route
strip. No chunk targets, no memory queries, no blueprint placement, no long-route
planning, and no engine-side chunk content store in this version.

**Related:** `docs/region-place-design.md` (the observed ground layer that stays
unchanged), `docs/llm-s1-navigation-design.md` (the single-context navigator and its
state), `docs/navigation-subagent-design.md` (legs and the compass),
`docs/spatial-memory-design.md` (cells, objects, places and consolidation),
`docs/agent-exploration-task.md` (the fair-test boundary), `SETTINGS.md`.

---

## 1. Decisions at a glance

| decision | choice |
|---|---|
| Chunk ontology | **Three layers:** objective chunk grid, objective world contents, per-agent chunk knowledge. |
| Does a chunk depend on observation? | **No.** Every chunk exists in the world grid whether or not any agent has seen it. |
| Chunk size | **8×8 cells default**, configurable globally as `chunks.size`, fixed at map load. |
| Agent-facing handle | **`area-N`**, assigned when the agent first observes the chunk. Per-agent, stored in `SpatialMemory`. |
| World identity | `(cx, cy)` stays engine-side. `C3` is an **engine/debug ID only**; it is never shown to the agent. |
| Unobserved chunks in the prompt | **Absent.** Only chunks with at least one observed cell appear. |
| v1 implementation | Chunk grid/API + the agent knowledge view (card + route strip) + the per-agent handle layer. No world-content store. |
| Route | The raw `ROUTE:` chain is replaced by a cumulative `JOURNEY:` line of handles; the last-leg strip replaces the one-line `LAST RESULT`. |
| Cards | A full card for the **current/stopped** area; traversed areas get one line each in the strip. |
| Explored amount | **Observed cell count only** in the agent card. No denominator, so the map edge is not revealed. |
| Future naming | Per-agent aliases over the handle; the agent must have observed the chunk; duplicates allowed, disambiguated by the handle. |
| Regions | **Unchanged.** Chunks are a fixed index, not a replacement for regions in v1. |
| Fair test | The agent view reads `SpatialMemory` plus the leg path and chunk geometry; never map contents, map dimensions, or the world-content layer. |

---

## 2. The problem

The 2026-10-30 maze run (`logs/2026-10-30_20-56-42/navigator/Wren.txt`) is the immediate
motivation. The agent made 23 accepted decisions in 22 legs, covered at least 79 tiles of
endpoint displacement, and never arrived. Median leg length was 3.5 tiles; the maximum was
exactly 6 (`compassRadius`); eight legs were exactly 6 tiles. After each leg the model got
one line -- "arrived at an unnamed road (leg 5)" -- and a long `ROUTE:` chain of place
labels. It did not get a usable picture of what it had crossed, what it had passed, or what
was still unknown around it.

This document does not change movement, and it does not fix the six-tile leg. That is the
separate `follow`/`straight`/`back` edge-generator proposal. The chunk system is a **stable
spatial index**; the route strip and area card are the agent-facing view of the agent's own
knowledge of that index, so a leg is not, from the agent's perspective, a jump from A to B.

### 2.1 What a chunk is not

A chunk is **not** created by an agent observing it, and it is **not** a unit of memory.
It is a fixed piece of world geometry. The agent may have no information about a chunk, a
little, or a stale memory of it; in all three cases the chunk exists.

The agent-facing handle is **also** knowledge-layer. `area-3` is assigned when this agent
first observes the chunk; another agent may call it `area-1` or have no handle for it at
all. The chunk's world identity is the key `(cx, cy)`, which never changes.

### 2.2 Why the old `C3` ID was wrong

A coordinate ID such as `A1` or `C3` encodes the world origin. An agent holding `A1`
already knows it is in the upper-left region of the map, and an agent holding `C3` can
infer the grid's orientation and bounds. That is map knowledge the agent did not observe.

The agent-facing handle therefore must not encode coordinates or the world origin. The
world key and the debug ID stay engine-side; the agent gets a per-agent discovery handle.

---

## 3. The three layers

```
WORLD (objective)                                   AGENT (subjective)
+-----------------------------+                     +------------------------------+
| 1. chunk grid               |  geometry           | 3. chunk knowledge           |
|    every chunk exists       | ------------------> |    handle area-N, observed   |
|    key (cx,cy), bounds      |                     |    cells/objects/places      |
|    size                     |                     |    -> area card, route strip |
+-----------------------------+                     +------------------------------+
| 2. chunk world contents     |  objective          |  not read by the agent view  |
|    cells, objects, places,  |  (engine-side)      |  (engine/future uses only)   |
|    characters               |                     |                              |
+-----------------------------+                     +------------------------------+
```

### 3.1 Chunk grid -- objective world geometry

The map is partitioned into a fixed grid of chunks. Every cell belongs to exactly one
chunk. Every chunk exists, including chunks no agent has ever observed. A chunk is defined
by its key `(cx, cy)`, its bounds, and the configured size. It has no contents of its own;
it is a box.

This layer is pure geometry. It needs no `SpatialMemory`, no observation, and no agent. It
is the only layer the agent's view is allowed to take for granted.

### 3.2 Chunk world contents -- objective, engine-side

For a given chunk, the world has contents: ground tiles, objects, places, characters. The
engine can compute them from the map and the live entity state. This layer is objective and
independent of any agent's knowledge.

**v1 does not persist this layer and does not expose it to agents.** It is defined here so
that future systems (placement, simulation, a known-world query) have a name and a
boundary. If it is ever built, it is an engine-side cache, not a per-agent record.

### 3.3 Chunk knowledge -- per-agent, partial

An agent's knowledge of a chunk is what its `SpatialMemory` contains for the cells in that
chunk: observed cells, surfaces, objects, characters, places, and when it last saw them. It
also holds the agent's handle for the chunk (`area-N`) and, in the future, any names the
agent has given it.

It is partial, possibly stale, and different for every agent. It can be empty. The area
card and the route strip are renderings of this layer. They answer "what does this agent
know about this area?" -- never "what is in this area?".

### 3.4 Why the separation matters

- **Fair test.** The agent view must not read the world contents, the map, map dimensions,
  place seeds, or any unobserved cell. The world contents layer is engine-side and
  off-limits to the view.
- **No observation-defined chunks.** A chunk does not appear or disappear because an agent
  looks at it. It is a stable address. The agent's knowledge and handle change; the chunk
  does not.
- **No coordinate leak.** The agent sees `area-3`, not `C3`. The number is discovery order,
  not position. The card reports an observed-cell count, not a fraction that would reveal
  the map edge.
- **Partial knowledge is honest.** The card can say "38 cells observed" and "west border
  unknown" without pretending to know the rest. A chunk with no observed cells is simply
  absent from the agent's prompt; it still exists in the grid.
- **Future systems get a clean boundary.** Placement, memory queries, and long-route
  planning can use the world contents layer later without contaminating the agent's
  knowledge view.

---

## 4. The chunk grid

### 4.1 Size and configuration

- `chunks.size` is the number of cells along each side of a chunk: `8` by default.
- Range: `4..64`. A size below 4 would approach a per-cell map; a size above 64 would make
  the card uselessly coarse.
- Global setting, read at map load. The grid is world geometry; changing the size mid-run
  would change every key, bound and handle, so a settings change requires a map reload and
  resets the episode journey.
- 8×8 is the default because it aligns with the near region level in
  `docs/region-place-design.md`, so a chunk card can often reuse the near region's aspects
  when convenient. The chunk code must not depend on that alignment; it works for any size.

Configuration lives in the `chunks` block of `settings.json` (see §7.6). Per-map overrides
are deferred.

### 4.2 Coordinates, bounds, world key

- The chunk grid is anchored to world cell `(0, 0)`, the map's north-west corner. The game
  already uses `+x` east and `+y` south.
- `cx = floor(x / size)`, `cy = floor(y / size)`.
- Bounds are half-open: `[cx*size, (cx+1)*size) × [cy*size, (cy+1)*size)`.
- The world key is `(cx, cy)`. It is engine-side only.
- The engine/debug ID is a column letter plus a row number (`A1`, `C3`, `E2`), derived from
  `(cx, cy)`. It is used in logs and the debug window, **never in the agent prompt**.
- The agent-facing handle is `area-N`, assigned per agent on first observation (§4.6).
  A prompt line is `AREA area-3 (the north fields)`, never `chunk (2,2)` or `C3`.

### 4.3 Map edges and partial chunks

A chunk at the map edge is still a chunk; its bounds may extend past the last map cell. The
engine/debug view can compute the true in-world area, but **the agent card does not use
it**. The card reports the number of observed cells, with no denominator. This keeps the
map boundary out of the agent's knowledge and avoids implying that unobserved cells outside
the world exist.

The world dimensions are engine geometry. They are not passed to the agent view.

### 4.4 Observed, known, traversed, newly observed

These words are used precisely:

| term | meaning |
|---|---|
| observed cell | an entry in `SpatialMemory.cells` |
| observed chunk (agent) | a chunk with at least one observed cell; only these appear in the prompt |
| handle | the agent's `area-N` for an observed chunk; allocated on first observation |
| traversed chunk | a chunk with at least one cell on the leg's actual walked path |
| known before the leg | the chunk had at least one observed cell before the leg started |
| newly observed chunk | zero observed cells before the leg, at least one after |
| newly seen off-path | a newly observed chunk that the path did not traverse |
| observed count | the number of observed cells in the chunk; no denominator |
| world contents | the objective map state; not the agent's observation |

`new` in the strip means **first observed during this leg**, not "every cell in it is new".
A known chunk that gains cells keeps its handle and its `Known` label; the card's observed
count shows the growth.

### 4.5 What the grid does not do

- It does not create, delete or move a chunk when an agent observes it.
- It does not store what is in a chunk. That is the world contents layer (§3.2), not built
  in v1.
- It does not decide where the agent goes, and it is not a navigation candidate in v1.
- It does not replace the observed region/place layer (§7.7).

### 4.6 Agent handles and future names

**Handle assignment.**

- The handle is per-agent and lives in `SpatialMemory`:
  `chunkHandles: unordered_map<world key, int>` and `nextChunkHandle`.
- The first chunk the agent observes gets `area-1`, the next new chunk `area-2`, and so on.
  The number records **discovery order**, not map position. It leaks neither the world
  origin nor the map bounds.
- The handle is assigned lazily: the first time the agent's chunk view needs it (normally
  when the chunk's first cell is observed). It is stable for the lifetime of the agent's
  memory, across `[NAVIGATE]` episodes.
- The world key stays `(cx, cy)`; the handle is a *name for this agent*. Two agents can
  have different handles for the same chunk.
- If the agent has no observed cell in a chunk, it has no handle, and the chunk is absent
  from the prompt.

**Future naming (not v1).**

- A name is a per-agent alias over the handle, not the chunk's identity. The chunk's world
  key never changes, and another agent may name it differently.
- The agent must have observed the chunk before naming it. Told/rumoured names (naming an
  area the agent has only heard about) are a future knowledge-source feature, not part of
  this design.
- Names are additive aliases, like `KnownPlace.labels`: an area can have several names.
  Duplicate names across areas are allowed; the prompt shows `the north fields (area-3)` to
  disambiguate.
- Proposed future storage: `chunkNames: unordered_map<int, vector<string>>` in
  `SpatialMemory`, keyed by handle.
- Proposed future command: `[NAME_AREA area-3 the north fields]`, sanitised like
  `[NAME_PLACE]` (strip brackets, cap length). A future chunk target could then be
  `[MOVE_TO the north fields]`, resolved through the agent's names to the handle and the
  world key.
- The prompt prefers a name over the handle; the handle remains in parentheses for
  disambiguation and in the structured trace.
- The world contents layer never uses agent names; it keys by `(cx, cy)`. Names do not
  leak across agents.

---

## 5. The agent-facing view (v1)

The view has three parts, rendered in the navigator's next prompt:

1. **`JOURNEY:`** -- the cumulative chunk route of the episode, in handles. Replaces the
   raw `ROUTE:` place chain.
2. **`AREA <handle>`** -- the full card for the current/stopped chunk.
3. **`LAST RESULT:`** -- the detailed strip for the last leg, ending with the stop reason.

The existing `YOU ARE:` line stays as it is. It gives the precise current place (a semantic
place or a region); the area card gives the surrounding chunk context. They can name
different things -- "on an unnamed road" and "area-3, the north fields" -- and that is
correct: the place is the fine target, the area is the coarse box.

### 5.1 The current/stopped area card

The full card is built for one chunk: the chunk containing the cell where the leg ended.
Nothing else gets a full card in the prompt.

```
AREA area-3 (the north fields) -- 38 cells observed, last seen now
  ground: grass, path, stone floor
  places: the north orchard; the east road (continues east)
  objects: 3 apple trees, a well, a signpost, stone walls
  characters: 1 (a trader)
  crossings: east -> area-4 (road); south -> area-5 (path);
             north -> an unexplored area (no crossing observed);
             west -> an unexplored area (unknown)
```

Fields:

- **Handle and description.** `area-3` plus a short observed phrase. If a semantic place or
  region intersects the chunk, its display name is a good phrase ("the north orchard").
  Otherwise use the dominant observed surfaces ("grass and road"). No authored chunk names
  in v1. If the agent has named the area (future), show the name first and the handle in
  parentheses: `the north fields (area-3)`.
- **Observed count.** The number of observed cells in the chunk, with **no denominator**.
  The engine/debug view may show the true in-world fraction; the agent card does not.
- **Ground.** The top observed surfaces (`MemoryCell.surface`), capped by `cardAspects`
  (default 3).
- **Places.** Semantic places and regions whose `memberCells` intersect the chunk. Use
  `PlaceQuery::DisplayName`/aspects. If a place has cells in a neighbouring chunk, say
  "continues east" so the agent does not think the place ends at the chunk border. Capped.
- **Objects.** Observed, non-vanished objects grouped by `ObjectContentKey` (the same key
  the object-target grammar uses), with counts. Exclude characters (they get their own
  line) and do not list every instance. Capped by `cardObjectGroups` (default 6).
- **Characters.** Count, plus the nearest observed character and its direction if the
  display name is known; otherwise "a person". `includeCharacters` defaults to true; the
  card never lists a character the agent has not seen.
- **Crossings.** The four cardinal borders. For each side:
  - `Observed` if at least one pair of adjacent observed walkable cells crosses the border;
    report the dominant surface and the neighbour's handle if the agent has observed it
    (`east -> area-4 (road)`). If the neighbour is unobserved, say `east -> an unexplored
    area (road)`; do not invent a handle.
  - `Blocked` if the whole border is observed and no walkable pair crosses it; say "no
    crossing observed" -- not "there is no crossing".
  - `Unknown` if part of the border is unobserved; say "unknown".
  The card never promotes an unknown border to blocked. Diagonal/corner crossings are a
  path fact, not a side connection; see §10.
- **Last seen.** The chunk's `lastSeenTick`; in v1 the card is always for the current or
  just-stopped chunk, so "now" is common.

The card is **neutral**. It lists facts (ground, places, objects, characters, crossings); it
does not say "this is a good place to build", "the goal is likely here", or "you should
search". The model draws the conclusion.

### 5.2 The route strip (last leg)

The strip is built from the leg's actual walked path, the memory before the leg, and the
memory after it. It replaces the one-line `LAST RESULT` with a short, topology-aware account.

```
LAST RESULT: walked 6 tiles east from the crossroads to a junction.
  area-1  road, straight; entered W, left E. Known.
  area-2  road; a side path north at +4 (unvisited); stopped here. New.
NEWLY SEEN: area-3 to the east (the road continues).
STOPPED at a T-junction: north is an unvisited path; east continues 9 tiles.
```

Structure:

- **Outcome line.** "walked N tiles <direction> from <start> to <end>" or "could not walk
  there: <reason>". N is the number of cells in the leg path. The start/end use the leg's
  start cell and the chosen target's description; the per-area entry/exit gives the route.
- **Per traversed area, in path order.** One line each:
  - handle/name and a one-line summary (dominant place/surface plus at most a few features);
  - entry and exit: the border side where the path entered and left, or "stopped here" for
    the last area;
  - `Known` / `New` (first observed during this leg);
  - features: a branch/opening, a place crossing, an object group, a character, capped at
    `maxFeaturesPerArea` (proposal: 3). Features are facts, not advice.
- **Newly seen off-path.** A compact line for chunks with newly observed cells that the path
  did not traverse. This is the perception-radius effect; list the handles/directions, cap
  at `maxNewAreas` (default 6), and say "and N more" if needed. No full cards for them.
- **Stop line.** The stop reason and the local decision point: "at a T-junction; north is an
  unvisited path; east continues 9 tiles". This is the same information the current
  `LAST RESULT` carries, enriched with the area's crossings.

The strip does **not** dump every cell or object. The structured version (§7.3) has the full
trace for the log and debug window; the prompt gets the prose above.

### 5.3 The journey line

The raw `ROUTE:` line was a cumulative chain of place labels. Replacing it with a per-leg
strip would lose the journey, so the strip is paired with a cumulative chunk line:

```
JOURNEY: area-1 -> area-2 -> area-3 (east 18); area-3 -> area-4 (south 6).
```

- It is the episode's chunk route, in order. Within the cap, it lists the handles; when the
  journey exceeds `maxJourneySegments`, the oldest segments collapse into an
  `earlier: N areas, M tiles` prefix.
- It is capped by `maxJourneySegments` (default 12). The full route stays in the episode log.
- It resets at the start of a `[NAVIGATE]` episode, like the current route.
- It uses handles (or future names), not place labels. Place names live in the strip's
  features and the area card; the raw place chain is removed from the prompt.
- The internal `route` vector remains for the summary and the navigator log. Only the prompt
  changes.

### 5.4 Rendering, caps, compaction

- Prose in the prompt, structured JSON in the navigator log/debug window. Both come from
  the same `LegTrace`/`ChunkKnowledge` structures, so they cannot disagree.
- Caps are settings, not magic numbers: `maxTraversedAreas` (strip lines per leg),
  `maxJourneySegments`, `maxNewAreas`, `maxFeaturesPerArea`, `cardAspects`,
  `cardObjectGroups`.
- Short legs compact: a 1-tile leg is one or two lines ("walked 1 tile south into area-3;
  known; no new features"). Do not print an empty per-area table.
- A leg that fails still gets a strip ("could not walk there: no way ...; no area
  traversed"), so the model learns why.
- The prompt budget: the whole view should stay around 10-12 lines (journey + card + strip).
  The caps are there to keep it there.

### 5.5 Worked examples

**The maze's long east road.** Chunk size 8. The road at `y=12` is the second row of chunks.
The road from `x=5` to `x=36` crosses five chunks. Over the six eastward legs the journey
accumulates as `area-1 -> area-2 -> area-3 -> area-4 -> area-5 (east 31)`; each leg's strip
lists the one or two areas it crossed. The handles are discovery order, not map order; the
world keys stay in the debug log. This is the spatial understanding the run lacked: the
model sees that it has walked the whole road, which areas it passed, and which side paths
it left unvisited.

**The grass room.** The 5×5 room at `x=17..21, y=17..21` sits inside one chunk. Its card
shows the room's surroundings and the observed crossings, but the room's *fine* topology --
the one exit, the corridor behind it -- still comes from the observation grid and the
decision-point state. The card is a coarse index, not a replacement for local geometry.
This is deliberate: chunks tell the agent which box it is in and what it knows about the
box; they do not replace looking at the room.

---

## 6. Fair test

The agent-facing view is built from:

- `SpatialMemory` -- observed cells, objects, places, regions, ticks, and the agent's
  chunk handles/names;
- the leg's actual walked path (the character's own movement);
- the chunk geometry (key, bounds, size).

It never reads:

- the map's tile/object/place contents;
- the map dimensions (the card uses an observed count, not a fraction);
- unobserved cells or places;
- authored place seeds that have not been observed;
- the world contents layer.

Rules:

1. A chunk with no observed cells is absent from the prompt. It still exists in the grid.
2. The card reports only observed cells/objects/places/characters. It reports an observed
   cell count, not a fraction of the world.
3. "No crossing observed" is not "there is no crossing". "Unknown" is not "empty".
4. The route strip lists only traversed/observed chunks. "Newly seen" uses a memory diff,
   not a map diff.
5. The handle is per-agent and coordinate-free. `area-3` says nothing about where the chunk
   is; `C3`/`A1` never reach the prompt.
6. The structured trace in the log may contain world keys (`cx,cy`), debug IDs, cell
   coordinates and ticks; it is built from the same memory, not from map contents. The
   debug window may show the world contents layer only if it is clearly labelled
   engine-side and is never passed to the agent prompt.

---

## 7. Integration

### 7.1 World API

A small, header-only geometry layer, in `include/Chunk.hpp`:

```cpp
struct ChunkCoord {
  int cx = 0;
  int cy = 0;
};

struct ChunkBounds {
  GamePosition min;          // inclusive
  GamePosition maxExclusive; // exclusive
};

ChunkCoord ChunkAt(GamePosition cell, int size);
ChunkBounds ChunkBoundsOf(ChunkCoord chunk, int size);
bool ChunkContains(ChunkCoord chunk, GamePosition cell, int size);
std::string ChunkDebugId(ChunkCoord chunk);            // "C3"; engine/debug only
bool ParseChunkDebugId(std::string_view id, ChunkCoord &out);
int ChunkAreaInWorld(ChunkCoord chunk, int size, int mapWidth, int mapHeight); // debug only
```

This layer is pure geometry: no map lookup, no memory, no world contents. `ChunkDebugId`
and `ChunkAreaInWorld` are never used in the agent prompt.

### 7.2 Per-agent handles

`SpatialMemory` gains:

```cpp
// Per-agent chunk handles. The world key is (cx,cy); the handle is what the
// model sees. Assigned on first observation. Future: chunkNames[handle] aliases.
std::unordered_map<std::uint64_t, int> chunkHandles;
int nextChunkHandle = 1;
```

Helpers, in `include/Chunk.hpp`:

```cpp
std::uint64_t ChunkKey(ChunkCoord chunk);                 // (cx,cy) -> 64-bit key
int EnsureChunkHandle(SpatialMemory &memory, ChunkCoord chunk);
std::string ChunkHandleLabel(int handle);                 // "area-3"
const std::string *ChunkFirstName(const SpatialMemory &memory, int handle); // future
std::string ChunkDisplayLabel(const SpatialMemory &memory, int handle);     // name or area-N
```

`EnsureChunkHandle` is the only mutation: it assigns the next handle when the key is not in
the map. It is called when the chunk view first needs the chunk (normally on its first
observed cell). The handle is stable for the memory's lifetime.

Future naming adds `chunkNames` keyed by handle; `ChunkDisplayLabel` prefers the first
name, then `area-N`. The naming command and `[MOVE_TO <name>]` are future features.

### 7.3 Chunk knowledge

A read-only view over `SpatialMemory`, built on demand:

```cpp
enum class ChunkCrossingState { Observed, Blocked, Unknown };

struct ChunkCrossing {
  int dx = 0, dy = 0;              // cardinal neighbour
  ChunkCrossingState state = ChunkCrossingState::Unknown;
  std::string surface;             // dominant observed surface at the crossing
  int crossingPairs = 0;           // adjacent observed walkable pairs
  int neighbourHandle = 0;         // 0 when the neighbour is unobserved
};

struct ChunkKnowledge {
  ChunkCoord coord;                // world key, engine-side
  int handle = 0;                  // per-agent area-N
  std::vector<std::string> names;  // future aliases
  int observedCells = 0;           // no denominator in the agent view
  int firstSeenTick = 0;
  int lastSeenTick = 0;
  std::string description;         // observed phrase, no authored name
  std::vector<std::pair<std::string, int>> surfaces;   // top N
  std::vector<const KnownPlace *> places;              // intersecting
  std::vector<ObjectGroup> objectGroups;               // grouped, counted
  std::vector<CharacterObservation> characters;        // nearest observed only
  int characterCount = 0;                              // observed in the chunk
  std::array<ChunkCrossing, 4> crossings;              // N, E, S, W
};

ChunkKnowledge BuildChunkKnowledge(SpatialMemory &memory,
                                   ChunkCoord chunk,
                                   const ChunkViewSettings &settings,
                                   GamePosition here);
```

`BuildChunkKnowledge` uses only `memory` plus the geometry. It calls `EnsureChunkHandle`
so the handle exists. It may reuse a near region's aspects when the chunk aligns with it;
that is an optimization, not a dependency. The object groups use
`ObjectContentKey`/`SeenObject`; vanished objects are excluded; characters are separated.
`ObjectGroup`, `CharacterObservation` and `ChunkViewSettings` are illustrative names; the
implementation can reuse `SeenObject`/`ObjectContentKey` and the `chunks` settings struct.

### 7.4 Leg trace and journey

```cpp
struct ChunkFeature {
  std::string kind;      // "branch", "opening", "place", "object", "character"
  std::string text;      // "a side path north (unvisited)", "a well"
};

struct ChunkSegment {
  ChunkCoord chunk;      // world key, structured trace only
  int handle = 0;        // per-agent area-N
  GamePosition enter, exit;
  int tiles = 0;
  bool knownBefore = false;
  bool firstObserved = false;
  std::vector<ChunkFeature> features;
};

struct LegTrace {
  std::string target;                 // the chosen candidate description
  int tiles = 0;
  std::string outcome;                // "walked ...", "could not walk there ..."
  std::vector<ChunkSegment> traversed;
  std::vector<int> newlyObserved;         // off-path handles
  std::string stopReason;                 // the decision point at the end
  std::string ToProse(const SpatialMemory &memory,
                      const ChunkViewSettings &settings) const;
};

struct JourneySegment {
  ChunkCoord from, to;
  int fromHandle = 0, toHandle = 0;
  std::string direction;
  int tiles = 0;
};

struct Journey {
  std::vector<JourneySegment> segments;
};

void AppendLeg(Journey &journey, const LegTrace &trace);
std::string RenderJourney(const Journey &journey, int maxSegments);
```

`BuildLegTrace` inputs: the leg path (`MoveToCellAction::getPath()`), the start cell, the
memory before/after, the set of chunks observed before the leg, the leg start tick, the
chosen target, and the stop reason. It:

1. Walks the path in order and groups cells by chunk; records entry/exit cells and tiles
   per segment; ensures the handle for each traversed chunk.
2. Marks `knownBefore` from the before-set and `firstObserved` when the chunk was not in it.
3. Collects features per segment:
   - **places** crossed, via `PlaceQuery::At` on the path cells, grouped into consecutive
     runs; display name and aspects;
   - **branches/openings** at path cells: observed walkable neighbours that are not the
     previous or next path cell; grouped by direction; marked visited/unvisited from the
     walked set;
   - **object groups** within one tile of the path with `lastSeenTick >= legStartTick`,
     grouped by `ObjectContentKey`, counted; characters separated;
   - capped by `maxFeaturesPerArea`.
4. Collects off-path newly observed chunks from the before-set diff.
5. Fills the stop reason/decision point from the arrival outcome and the current/stopped
   area's crossings.

The path is the character's actual movement, so the strip is not a map-derived route. A
failed leg has an empty path and an outcome line; the model still learns why.

### 7.5 Navigation state and prompts

`SingleNavigateAction` already holds the episode state (`route`, `lastResult`, `legs`,
`walkedCells`, `memory`). Add:

- `Journey journey;`
- `LegTrace lastTrace;`
- `std::string journeyText;`
- `std::string areaCard;` (the current/stopped card, rendered)

After each leg:

1. Build `lastTrace` from the path and the pre/post memory.
2. `AppendLeg(journey, lastTrace)`.
3. Set `lastResult = lastTrace.ToProse(*memory, settings)` (the strip includes
   the stop line).
4. Build and render the current area card for the chunk of `here`.

Object/examine legs use the same `RecordChunkLeg` path: the interaction text is
the strip's outcome line, so the journey still includes the movement they made.
When chunks are disabled, the old `arrived at ...`/interaction result is kept and
the raw `ROUTE:` line is used.

`BuildSingleUserPrompt` gains the journey string and the area card, and no longer prints
the raw `ROUTE:` line:

```
JOURNEY: area-1 -> area-2 -> area-3 (east 18); area-3 -> area-4 (south 6).
AREA area-4 (the north fields) -- 38 cells observed, last seen now
  ...
LAST RESULT: walked 6 tiles south ...
  ...
```

`BuildState` keeps its current `YOU ARE`/`ACTIONS` content. The chunk view is prompt
context, not a candidate and not a change to `BuildCandidates`.

Both navigators get the view. The single prompt carries `JOURNEY`/`AREA`/the
expanded `LAST RESULT`; the two-context strategist observation gets `journey`,
`area_card` and `last_result` fields, and the S1 state appends the same blocks.
With chunks disabled, both keep their previous route/result text.

### 7.6 Logging and debug

- The full prompt/state already goes to `logs/<session>/navigator/<Agent>.txt`. The strip and
  card are inspectable there.
- A structured `-- chunk trace (json) --` line is written after each leg through
  `NavigatorLog::ChunkTrace`/`LegTrace::ToJson`. It can contain world keys, debug
  IDs and cell coordinates; it is engine/log only.
- The Navigation window's **Chunks** tab shows the journey, the current area
  card, the last result strip and the leg-trace JSON. The JSON may contain world
  keys/debug IDs; the agent prompt never does.
- Do not write the world contents layer into the agent prompt. If a debug tab shows it, label
  it "world (engine)" and keep it out of the navigation state.

### 7.7 Settings

The `chunks` block in `settings.json`, with defaults in `include/Defaults.h`,
schema entries in `src/SettingsSchema.cpp`, and rows in `SETTINGS.md`:

```json
"chunks": {
  "enabled": true,
  "size": 8,
  "maxTraversedAreas": 8,
  "maxJourneySegments": 12,
  "maxNewAreas": 6,
  "maxFeaturesPerArea": 3,
  "cardAspects": 3,
  "cardObjectGroups": 6,
  "cardPlaces": 4,
  "includeCharacters": true
}
```

- `size`: 4..64; read at map load; a change requires a map reload and resets the journey.
- `enabled`: false restores the raw `ROUTE:` line and removes the card/strip, for A/B.
- All caps are wired through `GameSettings::ChunksSection` into `ChunkViewSettings`.
- `includeCharacters`: see §10.
- No handle setting: handles are per-agent and assigned automatically.

### 7.8 No changes to movement, candidates, or regions

- No changes to `BuildCandidates`, `PlaceQuery::At`, `PlaceQuery::ModelPlaces`, the region
  derivation, or the semantic place derivation.
- No chunk navigation targets in v1. The card and strip are presentation only.
- One small `SpatialMemory` addition: the per-agent handle map (§7.2). No changes to the
  cell/object/place/region storage.
- No map changes. The chunk grid is derived from `chunks.size`.
- The existing region layer stays. A fixed chunk and an observed region can cover the same
  8×8 cell; that is expected. The card may reuse the near region's aspects when it helps;
  the region is not replaced or renamed.

Why keep regions for now: they are load-bearing in `At`, the compass candidates, the
`[PLACES]` rows, naming/`[MOVE_TO]` and the strategist graph. Replacing them with chunks is
a migration of those consumers, not a deletion; it should be decided after the chunk view is
measured. The chunk is an index; the region is observed, place-like ground.

---

## 8. Acceptance

There is no unit-test framework in the repository; these are the checks to run, in order.

### 8.1 Geometry

- `ChunkAt` for `(0,0)`, `(7,7)`, `(8,8)`, `(39,39)` with size 8 -> the same four chunks;
  `ChunkDebugId` gives `A1`, `A1`, `B2`, `E5`.
- Bounds are half-open and contain exactly the expected cells.
- `ChunkDebugId`/`ParseChunkDebugId` round-trip `A1`, `Z1`, `AA1`, `AB1`, `C3`; rows start
  at 1.
- Size 4, 8, 16 all produce a consistent partition (every cell exactly one chunk).

### 8.2 Handles

- A fresh memory assigns `area-1` to the first observed chunk, `area-2` to the next new
  one; re-observing a known chunk does not consume a handle.
- The handle is stable across `[NAVIGATE]` episodes.
- Two memories can assign different handles to the same world key.
- No prompt line contains `C3`, `A1`, `(cx,cy)`, or a map dimension.
- The card's observed count has no denominator.

### 8.3 Knowledge

- A chunk with no observed cells produces no card and no prompt row.
- A card over a memory with 3 observed cells reports "3 cells observed" and only those
  cells' surfaces/objects.
- A crossing on observed walkable ground on both sides is `Observed`; a fully observed
  border with no walkable pair is `Blocked`; a partly unobserved border is `Unknown`.
- An observed neighbour shows its handle; an unobserved neighbour says "an unexplored
  area", not a handle.
- A place whose cells span the border is listed once and says "continues <direction>".
- A character is counted only if observed; a vanished object is not counted.
- No unobserved object, place, character or surface appears in the card.

### 8.4 Trace

- A path crossing three chunks produces three segments in order, with the correct
  entry/exit sides and tile counts; each has a handle.
- A path that re-enters a chunk produces two segments or one segment with a note; decide
  which in implementation (proposal: two segments, because the route is topological).
- A 1-tile leg renders compactly.
- A diagonal path that clips a chunk corner includes that chunk.
- `newlyObserved` contains only chunks with a memory diff, not map knowledge.
- `knownBefore`/`firstObserved` are correct across a multi-leg episode.
- The structured JSON contains the world keys; the prose does not.

### 8.5 Prompt and regression

- The raw `ROUTE:` line is gone; `JOURNEY:`/`AREA`/`LAST RESULT` are present.
- The strip/card stay within the caps; the whole view is around 10-12 lines.
- With `chunks.enabled` true/false, `BuildCandidates` returns the same candidates and the
  same targets. Only the prompt context changes.
- The navigation summary and the navigator log still make sense; the structured trace has
  the full path.

### 8.6 Runtime

- Run the maze map. The six eastward legs accumulate the journey `area-1 -> ... -> area-5`;
  each strip names the areas crossed and the side path it passed; the grass-room card is
  partial and does not claim the room's fine topology.
- Run a village map. The cards/strips name observed places and objects; no unobserved place
  appears.
- Fair-test map: place a hidden room/feature in an unobserved part of a chunk; the card and
  strip must not mention it until observed.
- A/B `chunks.enabled` on a fixed episode: compare prompt size and model output; movement
  candidates are identical. The knowledge benefit is judged from whether the model's plan
  references passed features/areas and whether it re-enters explored dead ends less often.

---

## 9. Non-goals and deferred

**Not in v1:**

- chunk navigation targets / candidates / chunk pathfinding;
- chunk-level route planning, waypoints, or a known-chunk map command;
- memory queries (`[FIND]`, "where was the apple tree?") and chunk query commands;
- blueprint placement, ownership, or site search;
- long-distance route planning;
- an engine-side persistent chunk content store;
- chunk LOD or per-map chunk size;
- agent chunk naming (`[NAME_AREA]`) -- designed in §4.6, not implemented;
- chunk-level triggers, chunk ownership;
- replacing or removing the region layer.

**Future, in rough order:**

1. Chunk targets (a handle/name destination) and a known-chunk map, once the agent knowledge
   view is proven.
2. Agent chunk naming (`[NAME_AREA]`) and name-based targets, over the handle layer.
3. A chunk connection graph (observed crossings) for route reasoning; still engine A* for
   the path.
4. Memory queries by object/place/area.
5. Placement scope ("legal sites in area-3") using the site-selection ranking.
6. Long-route planning (waypoints/constraints), if it earns its place.
7. Per-map chunk size, chunk LOD, and a decision on whether the region layer can be
   migrated into chunks.

---

## 10. Open questions

1. **Characters in the card.** Counts and nearest direction, or named characters? The
   proposal is counts/nearest, with `includeCharacters` default true. Names are more useful
   but age faster and add prompt noise.
2. **Journey format.** List every handle for a long run, or collapse to `area-1 -> area-5
   (east 31, 5 areas)`? The proposal lists handles up to `maxJourneySegments`, then
   compresses.
3. **Current card placement.** `JOURNEY`, `AREA`, `LAST RESULT` (proposed) or a different
   order? The model reads top-down; the current area before the last-leg detail is the
   proposed choice.
4. **Two-context navigator.** The single navigator gets the view first. Should the
   two-context fallback get the card/strip in the same version, or later?
5. **`chunks.size` hot reload.** Fixed at map load (proposed). Is a live size change worth
   supporting? It would remap every handle and reset the journey.
6. **Card crossings.** Cardinal only (proposed), or also diagonal/corner crossings? A
   diagonal route can clip a chunk corner; the route strip handles the path, but the card's
   connections are cardinal.
7. **Object groups.** Group by `ObjectContentKey` (proposed) or by display name? The former
   is stable and matches the object-target grammar.
8. **The `new` definition.** First-observed chunk (proposed) or new cells inside a known
   chunk? The former is cheap and explains the route; the latter needs a cell snapshot.
9. **Handle lifetime details.** The handle lives in `SpatialMemory` for the agent's memory
   lifetime (currently the session). Should it persist across sessions when memory does?
   What happens on memory reset/eviction? Proposal: reset with the memory; the handle is a
   knowledge-layer value.
10. **Future naming details.** Name length, duplicates display, whether names can be learned
    from other agents (told), and whether a name survives the chunk being renamed by another
    agent (it should not; names are per-agent). These are for the naming feature, not v1.

---

## Appendix A: the 2026-10-30 maze run

Chunk size 8. The road at `y=12` is the second chunk row. The world keys and engine/debug
IDs below are for the log; the agent sees handles.

| cell | world key | debug ID | agent handle (illustrative) |
|---|---|---|---|
| `(5,12)` | `(0,1)` | `A2` | `area-1` |
| `(11,12)` | `(1,1)` | `B2` | `area-2` |
| `(17,12)` | `(2,1)` | `C2` | `area-3` |
| `(28,12)` | `(3,1)` | `D2` | `area-4` |
| `(36,12)` | `(4,1)` | `E2` | `area-5` |
| `(17,19)` | `(2,2)` | `C3` | `area-6` (illustrative) |

The handles above are in discovery order only for the example; a real run assigns them in
whatever order the agent first observes the chunks. A first leg from `(5,12)` to `(11,12)`
renders roughly:

```
JOURNEY: area-1 -> area-2 (east 6).
LAST RESULT: walked 6 tiles east to a junction.
  area-1  road, straight; entered W, left E. Known.
  area-2  road; a side path north at +4 (unvisited); stopped here. New.
NEWLY SEEN: area-3 to the east (the road continues).
STOPPED at a T-junction: north is an unvisited path; east continues 9 tiles.
```

After the six eastward legs the journey is `area-1 -> ... -> area-5 (east 31)`. The
grass-room chunk is a separate card when the character is there; it does not replace the
room's local geometry.

## Appendix B: settings and field summary

See §7.7 for the settings block and §7.1-7.4 for the API sketches.

- `chunks.size` is cells per side, default 8, range 4..64, fixed at map load.
- World key `(cx,cy)`; engine/debug ID `C3`; agent handle `area-N`.
- The agent-facing term is "area"; code/docs say "chunk".
- Handles are per-agent, assigned on first observation, stored in `SpatialMemory`; names
  are future per-agent aliases over the handle.
- The card is built from `SpatialMemory` + chunk geometry; it never reads the world contents
  or the map dimensions. It reports an observed-cell count, not a fraction.
- The strip is built from the leg path + memory before/after; it never reads the map.
- The journey replaces the raw `ROUTE:` chain; the strip replaces the one-line `LAST
  RESULT`; the area card is new.
- Regions and candidates are unchanged in v1.
