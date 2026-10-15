# Blueprints: hand-authored and agent-authored buildings

**Status: design note. Nothing implemented here, and parts of it are deliberately unfinished.**
Proposal written 2026-10-14. It answers one question: *can a small grammar express a hut, a house
extension and a church, without letting a model freehand geometry into the world?*

Read `placement-design.md` first for the placement validator this builds on, and
`guide-command-design.md` for the derivation/hint split this borrows its shape from.

---

## 1. The idea

A **blueprint** is a structure written down as data:

- **Design** — produce a blueprint. Expensive, happens once, output is data, validated before
  anything is built, reusable forever after.
- **Construct** — stamp a blueprint into the world at a site, over turns, consuming materials.

Design's output is **data with a name, not a diff to the world.** Structure placement is the same
problem for every blueprint — pick an anchor and a rotation, check it fits, check the door reaches
a road — so a 4x4 field and a 12x20 church run through one code path with different footprints.

The property worth optimizing for: **a hand-authored blueprint and an agent-authored blueprint are
the same artifact.** The validator, the site finder and the construction process never need to know
who designed one. This is what makes `["all agents own every blueprint"]` a header change later
rather than a rewrite.

---

## 2. Two cell kinds, and why the schema must respect both

The world already has two parallel occupancy systems, and a blueprint needs both:

| | who owns it today | examples | what it means |
|---|---|---|---|
| **`Tile`** | `Map::tileMap`, a flat `vector<Tile*>`; mutated via `Map::addTileToMap` (`src/Map.cpp:469`) | `grass`, `tilled soil`, `wooden floor`, `path`, `water`, `wooden wall` | **the ground.** `blocksTile` decides walkability. Cheap, and the same tile is shared by many cells. |
| **object entity** | `ObjectFactory::SpawnObject` from `data/objects/*.json` | `chest`, `anvil`, `apple_tree`, `wheat_mature`, placed seeds | **something standing on the ground.** Gets a `DisplayName`, so `[SURROUNDINGS]` and `[MOVE_TO]` can see it. |

`Map`'s own `wooden wall` tile is the interesting case: a wall can be *terrain* (`blocksTile =
true`) or an *object* (`Obstacle`). A blueprint must be able to say which, because they differ in
cost, visibility and destruction:

- **terrain** — free to paint, invisible to `[SURROUNDINGS]`, no name, no loot. Right for mass
  that is never interacted with.
- **object** — costs an item, visible, nameable, harvestable, interactable. Right for doors,
  chests, beds, altars, workstations.

So the grammar's occupancy operation is **`paint`** (a tile) or **`place`** (a template). Nothing
in the schema conflates them.

---

## 3. The grammar

Shape and material are **separate** throughout. A part is *a shape filled with a thing*; the shape
does not know what it is made of, and the material does not know its layout. This is the whole
reason a small grammar can cover a village: a church is a solid rectangle, two outlines, a carve, a
fill and four placed objects — not two thousand decisions.

### 3.1 Skeleton

```json
{
  "id": "stone_church",
  "name": "Stone Church",
  "owner": "agent:Milo",
  "purpose": "worship",
  "regions": { "have": { "rect_outline": { "at": [0,0], "to": [11,19] } } },
  "levels": 2,
  "parts": [
    { "id": "floor", "region": "nave", "op": { "paint": "stone floor" } }
  ]
}
```

- `id` is a content id, the same id space as `ItemType::id` and recipe ids.
- `owner` is recorded but **not enforced** (see §7).
- `regions` is a named-shape dictionary so a shape computed once can be reused by several parts.
  This is what makes a wall's `fill` interior addressable elsewhere.
- `levels` and per-part `level` give multi-storey designs (`level: 1` or `[1,3]` for a tower).
  Each level is a separate flat grid whose coordinates are relative to the footprint; the engine
  reconciles vertical adjacency. **Note:** the current `Map` is single-layer, so levels are a
  later step — the schema carries them so designs do not have to be re-authored.

### 3.2 Shape primitives

Each yields a set of cells on one level. Absolute world coordinates do not appear anywhere.

**Filled or outline is always in the name.** Every area primitive comes in exactly two forms, and
there is no `filled` boolean anywhere. This is the one rule the vocabulary is built on: a shape
name never leaves you guessing which one it produces, and there is only one way to say it.

| primitive | fields | notes |
|---|---|---|
| `point` | `at` | one cell. Neither filled nor outline — the distinction does not apply. |
| `line` | `at`, `to` | inclusive, axis-aligned or stepped. Always a path; there is no filled form. |
| `rect_fill` | `at`, `to` *or* `at`, `width`, `height` | solid rectangle |
| `rect_outline` | `at`, `to` *or* `at`, `width`, `height` | rectangle border, one cell thick — **the wall primitive** |
| `disk_fill` | `at`, `radius` | solid circle |
| `disk_outline` | `at`, `radius` | circle border, one cell thick |
| `polygon_fill` | `points` | **any** shape, including concave |
| `polygon_outline` | `points` | border, one cell thick |
| `region` | `ref` | a named shape, which may itself be a `_fill` or an `_outline` |
| `union` / `intersect` / `difference` | `of: [...]` | set algebra, so shapes compose without re-listing cells |

Why `rect_*` and not `box`/`frame`/`ring`: `ring` reads as circular, `frame` reads as an object you
place rather than a cell set, and `box` is ambiguous in the other direction (it suggests a solid).
`_fill` / `_outline` is borrowed from `fill`/`outline` in the world-editing tools a model is most
likely to have seen, so it costs no learning.

**No `thickness` parameter exists.** Wall thickness is shape algebra: a two-cell wall around
`[0,0]..[11,19]` is `union(rect_outline[0,0,11,19], rect_outline[1,1,10,18])`, and the same
difference-of-two-fills written either way. Each spelling of a shape yields exactly one cell set
and there is exactly one way to say each, which is the property that makes the validator's
self-overlap and sealing checks unambiguous.

**Wall sealing rule.** A wall written as four `line` segments is connected only *diagonally* at its
corners, so a naive 4-connected flood fill leaks out through the corner. `rect_outline` guarantees a
properly sealed border (it is 4-connected by construction), and the validator enforces 4-connectivity
on any material the design marks `structural` (see §5.3). Note that `line` with a stepped
(Bresenham) run is *not* guaranteed 4-connected, so a diagonal wall cannot be sealed — that case is
caught by the validator rather than silently producing a leaky building. This is a real trap: an
unsealed "house" is the most likely output of a first-generation designer, and it should be caught,
not discovered later.

### 3.3 Occupancy operations

Each cell's final content comes from the last operation that writes it.

| op | argument | effect |
|---|---|---|
| `paint` | tile name | write a `Tile` |
| `place` | template id | spawn an object entity (`Door`, `Chest`, `Anvil`, ...) |
| `carve` | — | clear the cell to `void`/outside — door and window openings |
| `fill` | `{ paint }` or `{ place }` | fill the **enclosed interior** of the part's region / its current footprint |

`carve` is what turns a wall into a doorway without arithmetic: carve the one cell, then `place` a
door there. Keeping these as separate operations rather than a `door: true` flag is what lets a
window, a gate, and a hole in a roof all be expressed without adding vocabulary.

### 3.4 Composite sugar

These expand into the primitives above at load time, so they cost the validator nothing.

| composite | expands to |
|---|---|
| `enclosed` | `rect_outline`/`polygon_outline` (as walls) + `fill` of the interior |
| `room` | `enclosed`, plus the interior registered as a named region for later `ref` |
| `connect` | a `line` corridor from entry to exit |
| `call` | an embedded blueprint's parts at an offset — **composition** (a village wall design containing a tower design) |
| `repeat` | `count` copies at a `step`, for colonnades, fence posts, pews |

`call` is the one that makes the grammar genuinely capable rather than merely flexible: complex
structures become assemblies of designed parts, and a village can share a wall design between
every building. It needs the same cycle guard as the guide's chain walk (`kDepthCap`).

### 3.5 Deliberately excluded

| excluded | why |
|---|---|
| absolute world coordinates | site selection is a separate concern; the design is anchor-relative |
| new materials not in the world | a designer may **compose** the material vocabulary, never invent it |
| per-tile scripting, conditionals, loops at tile level | the grammar composes, it does not compute |
| references to locations or other maps | a blueprint is valid anywhere it fits |

---

## 4. Worked examples

### 4.1 A hut — the minimum that must work

```json
{
  "id": "small_hut",
  "name": "Small Hut",
  "levels": 1,
  "regions": {
    "hut":   { "rect_fill": { "at": [0,0], "to": [5,5] } },
    "floor": { "rect_fill": { "at": [0,0], "to": [5,5] } }
  },
  "parts": [
    { "id": "floor", "region": "floor", "op": { "paint": "wooden floor" } },
    { "id": "walls", "shape": { "rect_outline": { "at": [0,0], "to": [5,5] } },
      "op": { "paint": "wooden wall" }, "role": "structural" },
    { "id": "doorway", "shape": { "point": { "at": [2,5] } }, "op": { "carve": true } },
    { "id": "door", "shape": { "point": { "at": [2,5] } },
      "op": { "place": "wooden_door" }, "role": "entrance" }
  ]
}
```

Five parts, no coordinates in the model's world, and the door is an object that a later
`[SURROUNDINGS]` can name — which matters, because the agent has to find its way in.

### 4.2 A house extension — the growing-village case

The interesting one is not a bigger building, it is **editing an existing design**. A dwelling that
grew a second room is three more parts on the same blueprint:

```json
[
  { "id": "bedroom_wall", "shape": { "rect_outline": { "at": [7,0], "to": [11,5] } },
    "op": { "paint": "wooden wall" }, "role": "structural" },
  { "id": "bedroom_floor", "shape": { "rect_fill": { "at": [7,0], "to": [11,5] } },
    "op": { "paint": "wooden floor" } },
  { "id": "hall_to_bedroom", "shape": { "point": { "at": [6,2] } },
    "op": { "place": "wooden_door" }, "role": "interior_door" }
]
```

The engine can then *show* the agent its own previous blueprint and let it add parts, which is how
a village stops looking procedurally generated: buildings that were extended over time look like
buildings that were extended over time. This is also why blueprints are stored (§7) rather than
being one-shot outputs.

### 4.3 A church — the expressiveness test

This is the case that decides whether the grammar is capable enough. It uses levels, a reused
region, an interior door, a carrier region, and an entrance facing the street.

```json
{
  "id": "stone_church",
  "name": "Stone Church",
  "purpose": "worship",
  "levels": 2,
  "regions": {
    "nave": { "rect_fill": { "at": [0,0], "to": [11,19] } },
    "have": { "rect_outline": { "at": [0,0], "to": [11,19] } },
    "apse": { "disk_fill": { "at": [5,4], "radius": 3 } },
    "tower": { "rect_fill": { "at": [0,20], "to": [3,23] } }
  },
  "parts": [
    { "id": "nave_floor", "region": "nave", "op": { "paint": "stone floor" } },
    { "id": "nave_walls", "region": "have",
      "op": { "paint": "stone wall" }, "role": "structural" },
    { "id": "apse_floor", "region": "apse", "op": { "paint": "stone floor" } },
    { "id": "apse_wall", "shape": { "polygon_outline": { "points": [[2,2],[8,2],[8,8],[2,8]] } },
      "op": { "paint": "stone wall" }, "role": "structural" },
    { "id": "apse_opening", "shape": { "line": { "at": [5,8], "to": [6,8] } },
      "op": { "carve": true } },
    { "id": "tower_floor_l1", "region": "tower", "level": 1,
      "op": { "paint": "stone floor" } },
    { "id": "tower_walls", "shape": { "rect_outline": { "at": [0,20], "to": [3,23] } },
      "op": { "paint": "stone wall" }, "role": "structural" },
    { "id": "tower_door", "shape": { "point": { "at": [3,21] } },
      "op": { "place": "wooden_door" }, "role": "interior_door" },
    { "id": "altar", "shape": { "point": { "at": [5,18] } },
      "op": { "place": "stone_altar" }, "role": "fixture" },
    { "id": "pews_l", "shape": { "line": { "at": [3,10], "to": [3,16] } },
      "op": { "place": "wooden_bench" }, "role": "fixture" },
    { "id": "pews_r", "shape": { "line": { "at": [8,10], "to": [8,16] } },
      "op": { "place": "wooden_bench" }, "role": "fixture" },
    { "id": "main_door", "shape": { "point": { "at": [5,0] } },
      "op": { "carve": true } },
    { "id": "main_door_leaf", "shape": { "point": { "at": [5,0] } },
      "op": { "place": "wooden_door" }, "role": "entrance" }
  ]
}
```

A gothic arch, a buttress line, a spire in plan — all reachable with `polygon` and set algebra.
What this grammar cannot do is **freehand sculptural terrain**: it is a composition language, not a
painting one. For a village sim that is the right trade, because composition is what makes
validation possible.

---

## 5. Validation

The point of the grammar is that everything below is decidable. There is exactly one validator,
and **design-time** and **site-time** both call it — the same rule as `PlaceAtAction` owning the
placement predicate.

### 5.1 Design-time, structural

| check | reject when |
|---|---|
| structure | duplicate part `id`; a `region` ref that does not resolve; `call` cycles; depth over cap |
| material vocabulary | a `paint`/`place` naming something not in the world's available set (§5.5) |
| level range | `level` outside `levels` |
| cell count | over the cap (§8) — the guard against an agent emitting a 5000-cell blueprint |

### 5.2 Design-time, geometric

| check | reject when |
|---|---|
| self-overlap | two parts write the same cell unless one is an explicit later overwrite; a `place` and a `paint` on one cell is a conflict |
| in-memory fit | the *shape itself* is degenerate (zero area, `at` beyond `to` in a way that yields nothing) |

### 5.3 Design-time, topological — the checks that catch generated nonsense

| check | how |
|---|---|
| **sealed** | materials with `role: structural` form a 4-connected closed loop around a non-empty interior. Catches the diagonal-corner leak. |
| **enclosed** | flood fill from outside the design bounding box; unreached free cells are interior; report them per level |
| **has a room** | interior is non-empty; report the interior cell count and dimensions |
| **has an entrance** | at least one `role: entrance` door, 4-adjacent to both an interior cell and an exterior cell |
| **reachable interior** | every interior cell is reachable from an entrance when only walkable cells (interior + doors) are traversable — kills a sealed-off wing |
| **fixtures are inside** | a `place` with `role: fixture` lands on an interior walkable cell, not inside a wall |
| **level connectivity** | if `levels > 1`, some part must connect level *n* to *n+1* |

Rejections are **reported with reasons**, in the shape the placement work already uses
(`docs/placement-design.md` §S2):

```
stone_church rejected: nave_walls leaked at (5,0)-(6,0) — the wall is not sealed
stone_church rejected: tower_door (3,21) does not touch the interior
stone_church accepted: 1 room, 1 entrance, 212 cells, 2 levels
```

### 5.4 Site-time

Once a design is valid, finding a site is the same query as a field, with an irregular footprint:

```
Could not build Stone Church at anchor (4,12): overlaps Carl's House at (6,10)
Could not build Stone Church at anchor (30,4): needs 12x24 clear, only 9x24 is free
Can build Stone Church at anchor (40,8), rotation 0: entrance faces the east track
```

Site-time checks: fits in bounds at every rotation; overlaps no object, no structural terrain, and
no location owned by someone else; every cell of the footprint is the same *kind* of ground
(clearing a tree is fine, filling water is not, unless the design says so); the entrance remains
4-connected to the existing path network after construction.

### 5.5 The material palette

The designer must not invent materials, so the engine hands it the available set — this is the same
**engine owns facts** split as `[GUIDE]`. Derived from world data, not authored twice:

```
Available materials, from what exists in this world:
  tiles:   grass, path, tilled soil, wooden floor, stone floor, water
  walls:   wooden wall (structural, blocks movement)
  objects: wooden_door, wooden_bench, chest, anvil, stone_altar
```

---

## 6. Construction

A blueprint materializes as a **sequence of steps over turns**, not an instant stamp. That is where
it joins the existing Craft/Harvest machinery: a claim holds the materials while work proceeds, and
interruption is already handled.

1. **Clear** — remove conflicting objects (trees, loose items) within the footprint.
2. **Prepare** — paint any substrate/bedding the design expects under walls and floors.
3. **Build** — per part: paint tiles (cheap, instant) and place objects (costs materials, may take
   time).
4. **Verify** — the same validator runs against the *real* world, not the design. A building is
   finished when the target state is reached, which is what makes partial construction resumable.

Materials come from real inventory. Painting a wall tile is cheap; spawning a door consumes an
actual door. Exactly what a part costs, and whether the design declares a bill of materials or the
engine derives one from the op counts, is **not decided here** (§9).

---

## 7. Ownership, storage and token cost

- **Storage:** `data/blueprints/*.json`, persisted the way maps are (`Map::WriteMap`), so a design
  outlives the agent that made it. Runtime-designed blueprints need a separate store from authored
  content — the same content-root question the genre-pack work already has open.
- **Ownership (decided for now):** every blueprint records an `owner`, and **all agents can see and
  use all blueprints.** This is deliberate while the game is being built: a per-owner visibility
  rule costs tokens on every request for no gameplay benefit yet.
- **The debug window (decided for later):** a blueprint explorer showing the part list of a
  selected design with its validation result, a settable owner, and visibility toggles
  (`shared` / `private` / `hidden`). It is a debug and curation tool first — the fastest way to
  find out whether an agent's design is bad or the grammar is.
- **Token discipline.** The full part list must **never** sit in a spawn prompt. What the prompt
  may carry is a one-line index — `id — display name (cells, levels, owner)` — and the body is
  loaded on demand. A design in progress is a couple of hundred tokens; fourty of them in every
  request is a permanent tax, and `NPCContext::history` is never trimmed
  (`guide-command-design.md` §9). The verb split follows `[GUIDE]`/`[INFO]`: an index verb and a
  detail verb.

---

## 8. Limits

| limit | value | why |
|---|---|---|
| `call` depth | 3 | same guard as `kDepthCap` in the guide; composition must not recurse forever |
| cells per design | a content constant, tentatively 1024 per level | a 5000-cell blueprint is almost always a mistake, and construction cost should be predictable |
| parts per design | 256 | keeps the validator and the report readable |
| levels | 8 | sanity bound, not a design statement |

---

## 9. Open questions

- **Nothing has been built, and the build order matters.** The rectangle/zone case (fields) is the
  only one the farming test needs; blueprints are a separate milestone. The grammar above is
  designed to *not* need revisiting when that milestone arrives.
- **Does the design carry a bill of materials, or does the engine derive one from the ops?**
  Deriving cannot drift; declaring allows substitutions ("any wood"). Undecided.
- **How are levels represented?** `Map` is single-layer today. Per-level grids with an explicit
  vertical-connection part is the sketch, but this is the least certain part of the schema.
- **May a design reference another by id, or must it inline it?** Referencing keeps updates
  propagating; inlining keeps designs self-contained. `call` currently does the latter.
- **What happens when materials run out mid-construction?** A half-built church is a legitimate
  world state, but someone has to own the decision to continue, abandon, or salvage it.
- **Who can design?** Any agent, or an architect role? This is the class-system question arriving
  early, and the direction doc's "progression is in scope now" suggests a design skill.
- **Rebuilding after destruction.** Is a damaged building repaired against its blueprint, or does
  the owner redesign? Repair is cheaper and makes blueprints useful long after construction.

---

## 10. References

- `docs/placement-design.md` — the placement validator, S2's shared-predicate pattern, and the
  rejection-report style reused in §5
- `docs/guide-command-design.md` — derived skeleton + authored hints, the stateless-query decision,
  and §9 on untrimmed history
- `include/GuideBuilder.h`, `src/GuideBuilder.cpp` — `kDepthCap`, the depth guard `call` copies
- `include/Components.h` — `Tile` (`:156`), `Location`, `Placeable`, `Storage`, `Workstation`
- `src/Map.cpp` — tile creation (`:56`), `addTileToMap` (`:469`), `WriteMap` persistence
- `src/ObjectFactory.cpp` — `LoadTemplates`, `ApplyTemplate`, `RegisterDefaultComponents`
- `data/objects/*.json`, `data/tilesets/` — the object/tile vocabulary a palette is derived from
- `maps/Map_Farm.json`, `maps/Map_Village.json` — the `tileInfo` and `locations` shapes
