# Task: an agent that finds its way to a village

**Status:** brief for a new session. First cut implemented 2026-10-18 — see "Implementation status"
at the end. The AI run itself is still the owner's to verify.

**Goal.** One NPC starts in a forest with **zero spatial memory** and no hints, and must reach a
village using what it can see — a road, signposts, the shape of the land. It should be a fair test of
whether the memory and place features are worth keeping, not a scripted walk.

**Acceptance.**

- The agent spawns in the forest with an empty `SpatialMemory` (the Origin anchor is allowed — it is
  the character's own starting point, not a hint).
- Nothing in its prompt names the village or gives a direction.
- It wakes, explores, and arrives at a place labelled `Village` on its own.
- The run is inspectable afterwards: the agent's memory (cells, runs, places, digest) can be read from
  the Vision window and dumped to JSON.
- A run that fails is a real result. Do not add hints to make it pass.

**Non-goals.** No A* over the whole map, no omniscient sense of direction, no path the engine draws
for it. The agent's information is what it observed.

---

## What already exists

Everything below works and is in use; the new session should read the two design docs before writing
code.

| feature | where | notes |
|---|---|---|
| `Facing` on every character | `include/Components.h` | follows movement; holds when stopped |
| `Vision` cone + radar, LOS | `include/VisionQuery.hpp` | `BlocksVision` objects block; tiles never do |
| per-character memory | `include/SpatialMemory.hpp` | cells, objects, name seeds, runs, places, digest |
| place derivation | `include/SpatialMemory.hpp` (`DerivePlaces`) | seeds → regions → aspect fields |
| consolidation | `Memory::Consolidate` | runs shadow cells; place ground never dropped |
| place queries | `include/PlaceQuery.hpp` | `At`, `ByLabel`, `DisplayName`, `ApproachCell` |
| `[PLACES]`, `[NAME_PLACE]` | `include/AgentActions.h` | places come from memory, never from the map |
| `[MOVE_TO]` | `include/AgentActions.h` | resolves characters/objects, then remembered place labels |
| authored names as seeds | `maps/*.json` → `Map::GetPlaceSeeds` | inert until their ground is observed |
| task kinds | `src/TaskKindRegistry.cpp` | `check(agent, target)`, `subjectFilter` |
| task target resolution | `src/TaskVerification.cpp` | display name, then place label via memory |

Design docs: `docs/place-derivation-design.md` (places and aspects), `docs/spatial-memory-design.md`
(cells, caps, consolidation, digest), `docs/task-system-design.md` (goals and verification).

**Crucial limitation:** `Vision` and `SpatialMemory` are attached to the **player only**
(`src/GameECS.cpp`, the player setup near the end of the world init). The systems themselves filter on
the components, so an NPC with them should be written and derived per-entity — that is an assumption to
verify, not a fact to rely on.

---

## The gaps, in the order they break the test

### 1. Agents do not perceive (blocking)

Attach `Vision`, `SpatialMemory` and the map's place seeds to the NPC at spawn, exactly as the player
gets them. Then verify, with the window and the dump, that:

- the writer's `lastVisionTick` guard works per entity (no shared state);
- `DerivePlaces` and `Consolidate` run for that entity;
- the cell/object/place caps are per entity (they are fields of `SpatialMemory`, so this should hold);
- nothing in the sampler assumes one character.

Where: `src/GameECS.cpp` (spawn), possibly `ECSInitVisionSystems` / `ECSInitMemorySystems`.

### 2. No goal can be completed by *finding* something (blocking)

`TaskVerification` resolves a target name to an entity: first by `DisplayName`, then by a place label in
the agent's memory with the kind's `subjectFilter`. For "find the village" the target only resolves
after the place is known, which is correct — but no kind's `check` expresses "I have arrived".

Add one kind, e.g. `reach` ("go to a place or thing you have heard of"). Its `check(agent, target)`
should report whether the agent is close enough to `target` (or inside the place containing it). The
agent can then hold a short task with kind `reach`, target `Village`.

Terminal goals are seeded from the map's `npcs` entries (`Map::terminalGoals`, `SeedTerminalGoals`), so
the "find the village" intention arrives as a description, not as a task. The agent must be able to turn
it into a short task on its own — check that the task digest tells it the `reach` kind exists.

### 3. We cannot watch an NPC's memory (blocking for judging)

`VisionDebugWindow` reads `game->playerEntity`, and `DumpSpatialMemory(entity, includeCells)` takes an
entity but the window only passes the player. Add an entity selector to the Memory tab (the
`TaskBoardWindow` pattern: `ecs.filter<SpatialMemory>()`, collect, sort, select) and dump the selected
entity. Without this, a failed run cannot be diagnosed.

### 4. Roads are tiles, and "follow the road" is not a thing

- `[PLACES]` shows a road place once one is derived: `road 100% -- 30 tiles north-east`. That is enough
  to head toward it.
- `[MOVE_TO] <place>` targets the **nearest walkable member cell** (`PlaceQuery::ApproachCell`). On a
  long thin road that is one step. Following a road is therefore a loop of `[PLACES]` → pick a farther
  stretch → `[MOVE_TO]`.
- Test that loop before building anything. If the model cannot do it, the cheapest fix is in
  `ApproachCell`: when a place is linear (large extent, small area), target the **far end** from the
  character instead of the nearest cell. Do not add a travel verb until the cheap fix is shown to fail.

### 5. An NPC cannot look around

`Facing` follows movement; the player has "turn to the mouse when idle", NPCs have nothing. An NPC's
cone is locked to its direction of travel. At a junction or a clearing it cannot look east without
stepping east. A `[LOOK north]` verb that sets `Facing` is small and probably necessary for a fair test.

### 6. Signs do not exist

There is `ObjectDescription` and an `[EXAMINE]` interaction that surfaces it; there is no sign template
and no sign content. A `signpost` object whose description reads like a real sign is world content, not
a hint — a human player could read it too. Keep the distinction sharp:

- **fine:** an authored place *name* (a seed that only attaches once observed), a signpost in the world;
- **not fine:** the engine telling the agent where the village is, a prompt that names it, a direction
  in the task text, or an omniscient `[MOVE_TO]`.

### 7. The map

New map, `maps/Map_Village_Quest.json` (do not reuse `Place_Test.json`; that one tests derivation
features). It needs:

- a forest: dense `apple_tree` / `rock_face` with `BlocksVision`, so the cone matters;
- a road (`path` tiles) entering the forest, branching once;
- two signposts at the branch (the clues);
- a village: buildings, a market, a workshop, a well or trough — content that derives as places;
- **one** authored seed, `"label": "Village"`, at the village centre, and no other named seed;
- the NPC's `npcs` entry with a background that mentions a village **without saying where it is**, and
  a terminal goal like "Find the village".

Map format facts that cost time last session:

- `positions` is written **column-major**: `x` outer, `y` inner (`Map::LoadMap`). Writing it row-major
  transposes the tile layer against the objects, which looks like a derivation bug.
- `tilePalette` entries may be bare strings (catalog names); `placeSeeds` is `{label, position, radius}`.
- Objects are placed by explicit `x`, `y`; tiles come from the palette.

---

## Build and test rules

- Build inside `nix-shell`: `nix-shell --run 'cmake --build build -j4'`. Never `-j` above 4.
- If the assembler reports `.loa`/`leb128` on a source file, the ccache entry is corrupt: rebuild with
  `CCACHE_DISABLE=1`. It is not a C++ error.
- ASan is always on. A crash in a run is a real bug.
- Pure logic is tested by compiling a throwaway harness in `/tmp` against the real headers
  (`SpatialMemory.hpp`, `PlaceQuery.hpp`, `MemoryDigest.hpp`) plus `src/PlaceCategoryRegistry.cpp` and
  `src/DebugLog.cpp`, then running it. See the pattern used for `DerivePlaces` / `Consolidate`.
- Runtime verification on the AI path is the owner's, not the agent's. Do not claim a model run
  succeeded.

---

## Suggested first cut

1. Attach perception to the NPC; confirm with the existing `Place_Test` map before building the quest
   map (the NPC should derive the same places the player does).
2. Entity selector in the Vision window so the NPC's memory can be read.
3. `reach` task kind + terminal goal seeding; confirm the agent can create a short task with it.
4. Build `Map_Village_Quest.json` + the `signpost` template and its description.
5. Run it. Read the agent's memory: did it see the road? Did it make a road place? Did it walk *along*
   it or bounce at the first cell? Did it name anything?
6. Only then decide whether `[LOOK]` or a far-end `ApproachCell` is needed.

## Open questions

- Should the agent be able to *name* the village if the authored seed has not attached? (`[NAME_PLACE
  here Village]` would work; it is the agent's own choice, not a hint.)
- Is the vision cone (90° at 22 tiles) enough to spot a road in a forest, or does the forest need
  deliberate clearings at the road's edge?
- The digest is off by default and requires 60 s + 400 newly discovered cells. For a test run of a few
  minutes, is the engine's place list (`[PLACES]`) enough without it? Probably yes; do not turn it on
  just to make the test work.
- How long is a run allowed to be? Each wake is a model call; an agent that wanders for ten minutes is
  ten minutes of calls and a large transcript.

## Reading order for the next session

1. `docs/place-derivation-design.md` — how a place comes to exist, and why nothing is authored geometry.
2. `docs/spatial-memory-design.md` — cells, caps, consolidation, digest, and the debug surfaces.
3. `include/SpatialMemory.hpp` — `ObserveCells`, `ObserveObjects`, `DerivePlaces`, `Consolidate`.
4. `include/PlaceQuery.hpp`, `include/AgentActions.h` (`PlacesAction`, `MoveToEntityAction`).
5. `src/GameECS.cpp` — vision/memory systems and the player-only setup near the end.
6. `src/TaskVerification.cpp` + `src/TaskKindRegistry.cpp` — how a goal is checked.

---

## Appendix: starter prompt for the new session

Paste this into a fresh session in this repository:

> Read `docs/agent-exploration-task.md` in full before doing anything, then the two design docs it
> points at (`docs/place-derivation-design.md`, `docs/spatial-memory-design.md`).
>
> The task: an NPC starts in a forest with an empty `SpatialMemory` and no hints, and must reach a
> village using only what it observes — a road, signposts, the land. Today `Vision` and
> `SpatialMemory` are attached to the player only, there is no task kind that means "I arrived", and
> the Vision window cannot show an NPC's memory. The brief lists those gaps in dependency order with
> file references.
>
> Work in the order of "Suggested first cut". Before writing code, confirm by reading the systems
> that an NPC with the components would be written and derived per-entity, and say what you found.
> Build with `nix-shell --run 'cmake --build build -j4'` (add `CCACHE_DISABLE=1` if the assembler
> complains about `.loa`). Test pure logic with a throwaway harness in `/tmp` against the real
> headers. Do not add hints to make the agent succeed, and do not claim a model run passed — runtime
> verification is mine.
>
> Start by reporting: what already works for an NPC today, what specifically blocks it, and the
> smallest set of changes for a first honest attempt.

---

## Implementation status (2026-10-18)

Landed, in the order of "Suggested first cut":

1. **Perception for NPCs.** `Game::AttachPerception` (`src/GameECS.cpp:1133`) gives any character
   `Vision`, an empty `SpatialMemory` and the map's place seeds; `createNPC` calls it
   (`src/GameECS.cpp:252`) and the player's bespoke setup was replaced by the same call
   (`src/GameECS.cpp:3537`). The systems were already written per entity; verified by driving the
   real `VisionQuery::VisibleCells` → `Memory::Observe*` → `Memory::DerivePlaces`/`Consolidate` path
   for two characters over `Map_Village_Quest` — the second record grew, the first was untouched.
2. **`reach` task kind.** `CheckReach` + `CheckReachPlace` (`src/TaskKindRegistry.cpp:257`), with a
   new `TaskKind::placeCheck` for targets that are places rather than entities
   (`src/TaskVerification.cpp:166`). The digest now prints the engine's kind vocabulary alongside the
   `[SET_TASKS]` reminder (`src/AgentBrain.cpp:773`), so "find the village" can become a task the
   engine reports on.
3. **Vision window selector.** The Memory tab lists every entity carrying `SpatialMemory` (player
   first), selects one, and draws and dumps *that* record (`src/DebugWindows.cpp:3063`).
4. **Road following.** `PlaceQuery::ApproachCell` targets the far end of a linear place instead of
   the nearest cell (`include/PlaceQuery.hpp:121`); `[MOVE_TO] <label>` among places sharing a name
   picks the one whose approach is nearest (`include/AgentActions.h:191`). Without the first, the
   nearest cell of a road is the one underfoot and the command is a no-op.
5. **`[LOOK $DIRECTION]`.** `LookAction` (`include/AgentActions.h:314`), registered as a visible
   command (`src/GameECS.cpp:2510`). It sets `Facing`, waits one frame for the sampler, then returns
   a prose report of the sweep: what ground, things and places are in sight, in eight compass
   sectors (`include/SightReport.hpp`). `[LOOK around]` is the same action with a one-shot
   360-degree sample (`Vision::fullCircleRequested`, consumed by the sampler and cleared after one
   sweep), so the character gets its bearings without permanently widening its cone; the cells,
   objects and places it sees are written to memory by the normal writer, so `[PLACES]` afterwards
   reflects the survey. The sampler keeps the survey's cell set in `Vision::lastFullCircle` because
   a running action is stepped every other frame (the brain's 10 ms sleep), by which time the cone
   has been restored.
6. **Signposts and the map.** `data/objects/signpost.json`, `data/objects/signpost_old.json`, and
   `maps/Map_Village_Quest.json` (64×64): forest in 0..38 × 40..63, a road from the starting
   clearing (x 3..29 at y=50) to a T at (30,50), a quarry branch south from it, the village square
   around (50,20), exactly one seed `Village`, and one NPC `Wren` at (14,48) whose background
   mentions a village and whose terminal goal is "Find the village".

Harness result on the real map: at spawn the NPC sees 77 cells and derives 5 places including the
road (5 cells). A full-circle survey from the same spot sees 285 cells, 56 objects and 7 places, and
the text it produces names the road and the trees in every direction. After a walk along the road to
the square it has 1110 cells, 8 places, the `Village` label attached (19×18, 246 cells), and it
stands on ground the place claims.

Still open: whether the model names and follows the road stretches on its own (it must —
`[MOVE_TO]` only matches names it has), how long a run may take, and whether `[LOOK]` plus the
far-end rule are enough in practice. No hints were added and no model run was claimed.
